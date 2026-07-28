#include "concurrent_scheduler/scheduler.h"

#include "concurrent_scheduler/concurrent_task_queue.h"
#include "platform/sync.h"
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
#include "internal/scheduler_profiling.h"
static _Thread_local size_t scheduler_profiling_worker_index =
    SCHEDULER_PROFILING_MAX_WORKERS;
#endif

#include <stdint.h>
#include <stdlib.h>

typedef enum {
    SCHEDULER_STATE_INITIALIZED,
    SCHEDULER_STATE_STARTING,
    SCHEDULER_STATE_RUNNING,
    SCHEDULER_STATE_SHUTTING_DOWN,
    SCHEDULER_STATE_STOPPED,
    SCHEDULER_STATE_FAILED
} SchedulerState;

typedef struct SchedulerImplementation SchedulerImplementation;

typedef struct {
    SchedulerImplementation *scheduler;
    size_t worker_index;
} WorkerContext;

struct SchedulerImplementation {
    ConcurrentTaskQueue queue;
    SchedulerTaskExecuteFunction execute;
    void *execute_context;
    size_t worker_count;
    SchedulerState state;
    SchedMutex lifecycle_mutex;
    SchedCondition lifecycle_condition;
    size_t active_submitter_count;
    SchedThread *threads;
    WorkerContext *worker_contexts;
    size_t started_worker_count;
    SchedMutex worker_mutex;
    SchedCondition worker_condition;
    size_t ready_worker_count;
    size_t successful_callback_count;
    size_t failed_callback_count;
    int worker_infrastructure_failure;
    int worker_sync_initialized;
};

enum {
    WORKER_RESULT_OK = 0,
    WORKER_RESULT_INVALID_CONTEXT = -1,
    WORKER_RESULT_SYNCHRONIZATION = -2,
    WORKER_RESULT_QUEUE = -3
};

static SchedulerResult map_queue_init_result(
    ConcurrentTaskQueueResult result
)
{
    switch (result) {
    case CONCURRENT_TASK_QUEUE_OK:
        return SCHEDULER_OK;
    case CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION:
        return SCHEDULER_ERROR_ALLOCATION;
    case CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT:
    case CONCURRENT_TASK_QUEUE_ERROR_SYSTEM:
    case CONCURRENT_TASK_QUEUE_ERROR_FULL:
    case CONCURRENT_TASK_QUEUE_ERROR_EMPTY:
    case CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN:
    default:
        return SCHEDULER_ERROR_SYSTEM;
    }
}

static SchedulerResult map_try_enqueue_result(
    ConcurrentTaskQueueResult result
)
{
    switch (result) {
    case CONCURRENT_TASK_QUEUE_OK:
        return SCHEDULER_OK;
    case CONCURRENT_TASK_QUEUE_ERROR_FULL:
        return SCHEDULER_ERROR_QUEUE_FULL;
    case CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN:
        return SCHEDULER_ERROR_SHUTDOWN;
    case CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT:
    case CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION:
    case CONCURRENT_TASK_QUEUE_ERROR_EMPTY:
    case CONCURRENT_TASK_QUEUE_ERROR_SYSTEM:
    default:
        return SCHEDULER_ERROR_SYSTEM;
    }
}

static SchedulerResult map_blocking_enqueue_result(
    ConcurrentTaskQueueResult result
)
{
    switch (result) {
    case CONCURRENT_TASK_QUEUE_OK:
        return SCHEDULER_OK;
    case CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN:
        return SCHEDULER_ERROR_SHUTDOWN;
    case CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT:
    case CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION:
    case CONCURRENT_TASK_QUEUE_ERROR_FULL:
    case CONCURRENT_TASK_QUEUE_ERROR_EMPTY:
    case CONCURRENT_TASK_QUEUE_ERROR_SYSTEM:
    default:
        return SCHEDULER_ERROR_SYSTEM;
    }
}

static SchedulerResult map_queue_shutdown_result(
    ConcurrentTaskQueueResult result
)
{
    switch (result) {
    case CONCURRENT_TASK_QUEUE_OK:
        return SCHEDULER_OK;
    case CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT:
    case CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION:
    case CONCURRENT_TASK_QUEUE_ERROR_SYSTEM:
    case CONCURRENT_TASK_QUEUE_ERROR_FULL:
    case CONCURRENT_TASK_QUEUE_ERROR_EMPTY:
    case CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN:
    default:
        return SCHEDULER_ERROR_SYSTEM;
    }
}

static SchedulerResult set_scheduler_state(
    SchedulerImplementation *implementation,
    SchedulerState state
)
{
    if (sched_mutex_lock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    implementation->state = state;
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    return SCHEDULER_OK;
}

static int scheduler_worker_entry(void *argument)
{
    WorkerContext *context = argument;
    SchedulerImplementation *implementation;
    ConcurrentTaskQueueResult queue_result;
    Task *task;
    int callback_result;

    if (context == NULL
        || context->scheduler == NULL
        || context->worker_index >= context->scheduler->worker_count) {
        return WORKER_RESULT_INVALID_CONTEXT;
    }

    implementation = context->scheduler;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    scheduler_profiling_worker_index = context->worker_index;
    concurrent_task_queue_profiling_set_worker_index(context->worker_index);
#endif
    if (sched_mutex_lock(&implementation->worker_mutex) != SCHED_SYNC_OK) {
        return WORKER_RESULT_SYNCHRONIZATION;
    }
    implementation->ready_worker_count++;
    if (sched_condition_broadcast(&implementation->worker_condition)
        != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&implementation->worker_mutex);
        return WORKER_RESULT_SYNCHRONIZATION;
    }
    if (sched_mutex_unlock(&implementation->worker_mutex)
        != SCHED_SYNC_OK) {
        return WORKER_RESULT_SYNCHRONIZATION;
    }

    for (;;) {
        task = NULL;
        queue_result = concurrent_task_queue_dequeue(
            &implementation->queue,
            &task
        );
        if (queue_result == CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN) {
            return WORKER_RESULT_OK;
        }
        if (queue_result != CONCURRENT_TASK_QUEUE_OK || task == NULL) {
            if (sched_mutex_lock(&implementation->worker_mutex)
                == SCHED_SYNC_OK) {
                implementation->worker_infrastructure_failure = 1;
                (void)sched_mutex_unlock(&implementation->worker_mutex);
            }
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            return WORKER_RESULT_QUEUE;
        }

        callback_result = implementation->execute(
            task,
            implementation->execute_context
        );
        if (sched_mutex_lock(&implementation->worker_mutex) != SCHED_SYNC_OK) {
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            return WORKER_RESULT_SYNCHRONIZATION;
        }
        if (callback_result == 0) {
            implementation->successful_callback_count++;
        } else {
            implementation->failed_callback_count++;
        }
        if (sched_mutex_unlock(&implementation->worker_mutex)
            != SCHED_SYNC_OK) {
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            return WORKER_RESULT_SYNCHRONIZATION;
        }
    }
}

static SchedulerResult release_workers(SchedulerImplementation *implementation)
{
    SchedulerResult result = SCHEDULER_OK;
    size_t index;
    int worker_result;
    int all_joined = 1;

    for (index = 0U;
         index < implementation->started_worker_count;
         index++) {
        if (implementation->threads == NULL
            || implementation->threads[index].implementation == NULL) {
            continue;
        }
        worker_result = WORKER_RESULT_SYNCHRONIZATION;
        if (sched_thread_join(
                &implementation->threads[index],
                &worker_result
            ) != SCHED_SYNC_OK) {
            result = SCHEDULER_ERROR_SYSTEM;
            all_joined = 0;
            continue;
        }
        if (worker_result != WORKER_RESULT_OK) {
            result = SCHEDULER_ERROR_SYSTEM;
        }
        sched_thread_destroy(&implementation->threads[index]);
    }

    if (!all_joined) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (implementation->worker_infrastructure_failure) {
        result = SCHEDULER_ERROR_SYSTEM;
    }

    free(implementation->worker_contexts);
    free(implementation->threads);
    implementation->worker_contexts = NULL;
    implementation->threads = NULL;
    implementation->started_worker_count = 0U;
    implementation->ready_worker_count = 0U;

    if (implementation->worker_sync_initialized) {
        sched_condition_destroy(&implementation->worker_condition);
        sched_mutex_destroy(&implementation->worker_mutex);
        implementation->worker_sync_initialized = 0;
    }
    return result;
}

static SchedulerResult register_submitter(
    Scheduler *scheduler,
    Task *task,
    SchedulerImplementation **implementation
)
{
    if (scheduler == NULL || task == NULL) {
        return SCHEDULER_ERROR_INVALID_ARGUMENT;
    }
    if (scheduler->implementation == NULL) {
        return SCHEDULER_ERROR_INVALID_STATE;
    }

    *implementation = scheduler->implementation;
    if (sched_mutex_lock(&(*implementation)->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    if ((*implementation)->state != SCHEDULER_STATE_RUNNING) {
        SchedulerResult state_result =
            ((*implementation)->state == SCHEDULER_STATE_SHUTTING_DOWN
                || (*implementation)->state == SCHEDULER_STATE_STOPPED)
            ? SCHEDULER_ERROR_SHUTDOWN
            : SCHEDULER_ERROR_INVALID_STATE;

        (void)sched_mutex_unlock(&(*implementation)->lifecycle_mutex);
        return state_result;
    }
    (*implementation)->active_submitter_count++;
    if (sched_mutex_unlock(&(*implementation)->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    return SCHEDULER_OK;
}

static SchedulerResult deregister_submitter(
    SchedulerImplementation *implementation,
    SchedulerResult operation_result
)
{
    SchedulerResult result = operation_result;

    if (sched_mutex_lock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (implementation->active_submitter_count == 0U) {
        result = SCHEDULER_ERROR_SYSTEM;
    } else {
        implementation->active_submitter_count--;
    }
    if (implementation->active_submitter_count == 0U
        && sched_condition_broadcast(&implementation->lifecycle_condition)
            != SCHED_SYNC_OK) {
        result = SCHEDULER_ERROR_SYSTEM;
    }
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        result = SCHEDULER_ERROR_SYSTEM;
    }
    return result;
}

SchedulerResult scheduler_init(
    Scheduler *scheduler,
    size_t queue_capacity,
    size_t worker_count,
    SchedulerTaskExecuteFunction execute,
    void *execute_context
)
{
    SchedulerImplementation *implementation;
    ConcurrentTaskQueueResult queue_result;
    SchedulerResult result;

    if (scheduler == NULL
        || queue_capacity == 0U
        || worker_count == 0U
        || execute == NULL) {
        return SCHEDULER_ERROR_INVALID_ARGUMENT;
    }
    if (scheduler->implementation != NULL) {
        return SCHEDULER_ERROR_INVALID_STATE;
    }

    implementation = calloc(1U, sizeof(*implementation));
    if (implementation == NULL) {
        return SCHEDULER_ERROR_ALLOCATION;
    }

    queue_result = concurrent_task_queue_init(
        &implementation->queue,
        queue_capacity
    );
    if (queue_result != CONCURRENT_TASK_QUEUE_OK) {
        result = map_queue_init_result(queue_result);
        free(implementation);
        return result;
    }
    if (sched_mutex_init(&implementation->lifecycle_mutex) != SCHED_SYNC_OK) {
        concurrent_task_queue_destroy(&implementation->queue);
        free(implementation);
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (sched_condition_init(&implementation->lifecycle_condition)
        != SCHED_SYNC_OK) {
        sched_mutex_destroy(&implementation->lifecycle_mutex);
        concurrent_task_queue_destroy(&implementation->queue);
        free(implementation);
        return SCHEDULER_ERROR_SYSTEM;
    }

    implementation->execute = execute;
    implementation->execute_context = execute_context;
    implementation->worker_count = worker_count;
    implementation->state = SCHEDULER_STATE_INITIALIZED;
    scheduler->implementation = implementation;
    return SCHEDULER_OK;
}

SchedulerResult scheduler_start(Scheduler *scheduler)
{
    SchedulerImplementation *implementation;
    size_t index;

    if (scheduler == NULL) {
        return SCHEDULER_ERROR_INVALID_ARGUMENT;
    }
    if (scheduler->implementation == NULL) {
        return SCHEDULER_ERROR_INVALID_STATE;
    }

    implementation = scheduler->implementation;
    if (sched_mutex_lock(&implementation->lifecycle_mutex) != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (implementation->state != SCHEDULER_STATE_INITIALIZED) {
        (void)sched_mutex_unlock(&implementation->lifecycle_mutex);
        return SCHEDULER_ERROR_INVALID_STATE;
    }
    implementation->state = SCHEDULER_STATE_STARTING;
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }

    if (implementation->worker_count > SIZE_MAX / sizeof(*implementation->threads)
        || implementation->worker_count
            > SIZE_MAX / sizeof(*implementation->worker_contexts)) {
        (void)set_scheduler_state(
            implementation,
            SCHEDULER_STATE_INITIALIZED
        );
        return SCHEDULER_ERROR_ALLOCATION;
    }

    implementation->threads = calloc(
        implementation->worker_count,
        sizeof(*implementation->threads)
    );
    implementation->worker_contexts = calloc(
        implementation->worker_count,
        sizeof(*implementation->worker_contexts)
    );
    if (implementation->threads == NULL
        || implementation->worker_contexts == NULL) {
        free(implementation->worker_contexts);
        free(implementation->threads);
        implementation->worker_contexts = NULL;
        implementation->threads = NULL;
        (void)set_scheduler_state(
            implementation,
            SCHEDULER_STATE_INITIALIZED
        );
        return SCHEDULER_ERROR_ALLOCATION;
    }

    if (sched_mutex_init(&implementation->worker_mutex) != SCHED_SYNC_OK) {
        free(implementation->worker_contexts);
        free(implementation->threads);
        implementation->worker_contexts = NULL;
        implementation->threads = NULL;
        (void)set_scheduler_state(
            implementation,
            SCHEDULER_STATE_INITIALIZED
        );
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (sched_condition_init(&implementation->worker_condition)
        != SCHED_SYNC_OK) {
        sched_mutex_destroy(&implementation->worker_mutex);
        free(implementation->worker_contexts);
        free(implementation->threads);
        implementation->worker_contexts = NULL;
        implementation->threads = NULL;
        (void)set_scheduler_state(
            implementation,
            SCHEDULER_STATE_INITIALIZED
        );
        return SCHEDULER_ERROR_SYSTEM;
    }
    implementation->worker_sync_initialized = 1;

    for (index = 0U; index < implementation->worker_count; index++) {
        implementation->worker_contexts[index].scheduler = implementation;
        implementation->worker_contexts[index].worker_index = index;
        if (sched_thread_create(
                &implementation->threads[index],
                scheduler_worker_entry,
                &implementation->worker_contexts[index]
            ) != SCHED_SYNC_OK) {
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            (void)release_workers(implementation);
            (void)set_scheduler_state(
                implementation,
                SCHEDULER_STATE_FAILED
            );
            return SCHEDULER_ERROR_SYSTEM;
        }
        implementation->started_worker_count++;
    }

    if (sched_mutex_lock(&implementation->worker_mutex) != SCHED_SYNC_OK) {
        (void)concurrent_task_queue_shutdown(&implementation->queue);
        (void)release_workers(implementation);
        (void)set_scheduler_state(implementation, SCHEDULER_STATE_FAILED);
        return SCHEDULER_ERROR_SYSTEM;
    }
    while (implementation->ready_worker_count < implementation->worker_count) {
        if (sched_condition_wait(
                &implementation->worker_condition,
                &implementation->worker_mutex
            ) != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&implementation->worker_mutex);
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            (void)release_workers(implementation);
            (void)set_scheduler_state(
                implementation,
                SCHEDULER_STATE_FAILED
            );
            return SCHEDULER_ERROR_SYSTEM;
        }
    }
    if (sched_mutex_unlock(&implementation->worker_mutex) != SCHED_SYNC_OK) {
        (void)concurrent_task_queue_shutdown(&implementation->queue);
        (void)release_workers(implementation);
        (void)set_scheduler_state(implementation, SCHEDULER_STATE_FAILED);
        return SCHEDULER_ERROR_SYSTEM;
    }

    if (sched_mutex_lock(&implementation->lifecycle_mutex) != SCHED_SYNC_OK) {
        (void)concurrent_task_queue_shutdown(&implementation->queue);
        (void)release_workers(implementation);
        (void)set_scheduler_state(implementation, SCHEDULER_STATE_FAILED);
        return SCHEDULER_ERROR_SYSTEM;
    }
    implementation->state = SCHEDULER_STATE_RUNNING;
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        (void)concurrent_task_queue_shutdown(&implementation->queue);
        (void)release_workers(implementation);
        (void)set_scheduler_state(implementation, SCHEDULER_STATE_FAILED);
        return SCHEDULER_ERROR_SYSTEM;
    }
    return SCHEDULER_OK;
}

SchedulerResult scheduler_submit(Scheduler *scheduler, Task *task)
{
    SchedulerImplementation *implementation;
    SchedulerResult result;

    result = register_submitter(scheduler, task, &implementation);
    if (result != SCHEDULER_OK) {
        return result;
    }
    result = map_blocking_enqueue_result(
        concurrent_task_queue_enqueue(
            &implementation->queue,
            task
        )
    );
    return deregister_submitter(implementation, result);
}

SchedulerResult scheduler_try_submit(Scheduler *scheduler, Task *task)
{
    SchedulerImplementation *implementation;
    SchedulerResult result;

    result = register_submitter(scheduler, task, &implementation);
    if (result != SCHEDULER_OK) {
        return result;
    }
    result = map_try_enqueue_result(
        concurrent_task_queue_try_enqueue(&implementation->queue, task)
    );
    return deregister_submitter(implementation, result);
}

SchedulerResult scheduler_shutdown(Scheduler *scheduler)
{
    SchedulerImplementation *implementation;
    SchedulerResult result;

    if (scheduler == NULL) {
        return SCHEDULER_ERROR_INVALID_ARGUMENT;
    }
    if (scheduler->implementation == NULL) {
        return SCHEDULER_ERROR_INVALID_STATE;
    }

    implementation = scheduler->implementation;
    if (sched_mutex_lock(&implementation->lifecycle_mutex) != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (implementation->state == SCHEDULER_STATE_STOPPED) {
        if (sched_mutex_unlock(&implementation->lifecycle_mutex)
            != SCHED_SYNC_OK) {
            return SCHEDULER_ERROR_SYSTEM;
        }
        return SCHEDULER_OK;
    }
    if (implementation->state != SCHEDULER_STATE_RUNNING
        && implementation->state != SCHEDULER_STATE_SHUTTING_DOWN) {
        (void)sched_mutex_unlock(&implementation->lifecycle_mutex);
        return SCHEDULER_ERROR_INVALID_STATE;
    }
    if (implementation->state == SCHEDULER_STATE_RUNNING) {
        implementation->state = SCHEDULER_STATE_SHUTTING_DOWN;
    }
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }

    result = map_queue_shutdown_result(
        concurrent_task_queue_shutdown(&implementation->queue)
    );
    if (sched_mutex_lock(&implementation->lifecycle_mutex) != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    while (implementation->active_submitter_count != 0U) {
        if (sched_condition_wait(
                &implementation->lifecycle_condition,
                &implementation->lifecycle_mutex
            ) != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&implementation->lifecycle_mutex);
            return SCHEDULER_ERROR_SYSTEM;
        }
    }
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    return result;
}

SchedulerResult scheduler_join(Scheduler *scheduler)
{
    SchedulerImplementation *implementation;
    SchedulerResult result;
    SchedulerState state;

    if (scheduler == NULL) {
        return SCHEDULER_ERROR_INVALID_ARGUMENT;
    }
    if (scheduler->implementation == NULL) {
        return SCHEDULER_ERROR_INVALID_STATE;
    }

    implementation = scheduler->implementation;
    if (sched_mutex_lock(&implementation->lifecycle_mutex) != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    state = implementation->state;
    if (state == SCHEDULER_STATE_STOPPED) {
        if (sched_mutex_unlock(&implementation->lifecycle_mutex)
            != SCHED_SYNC_OK) {
            return SCHEDULER_ERROR_SYSTEM;
        }
        return SCHEDULER_OK;
    }
    if (state != SCHEDULER_STATE_SHUTTING_DOWN
        && state != SCHEDULER_STATE_FAILED) {
        (void)sched_mutex_unlock(&implementation->lifecycle_mutex);
        return SCHEDULER_ERROR_INVALID_STATE;
    }
    while (implementation->active_submitter_count != 0U) {
        if (sched_condition_wait(
                &implementation->lifecycle_condition,
                &implementation->lifecycle_mutex
            ) != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&implementation->lifecycle_mutex);
            return SCHEDULER_ERROR_SYSTEM;
        }
    }
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }

    result = release_workers(implementation);
    if (implementation->threads != NULL) {
        (void)set_scheduler_state(implementation, SCHEDULER_STATE_FAILED);
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (set_scheduler_state(implementation, SCHEDULER_STATE_STOPPED)
        != SCHEDULER_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    return result;
}

SchedulerResult scheduler_destroy(Scheduler *scheduler)
{
    SchedulerImplementation *implementation;
    SchedulerState state;

    if (scheduler == NULL) {
        return SCHEDULER_ERROR_INVALID_ARGUMENT;
    }
    if (scheduler->implementation == NULL) {
        return SCHEDULER_OK;
    }

    implementation = scheduler->implementation;
    if (sched_mutex_lock(&implementation->lifecycle_mutex) != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    state = implementation->state;
    if (state != SCHEDULER_STATE_INITIALIZED
        && state != SCHEDULER_STATE_STOPPED
        && state != SCHEDULER_STATE_FAILED) {
        (void)sched_mutex_unlock(&implementation->lifecycle_mutex);
        return SCHEDULER_ERROR_INVALID_STATE;
    }
    if (implementation->active_submitter_count != 0U
        || implementation->threads != NULL
        || implementation->worker_contexts != NULL) {
        (void)sched_mutex_unlock(&implementation->lifecycle_mutex);
        return SCHEDULER_ERROR_INVALID_STATE;
    }
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (implementation->worker_sync_initialized) {
        sched_condition_destroy(&implementation->worker_condition);
        sched_mutex_destroy(&implementation->worker_mutex);
        implementation->worker_sync_initialized = 0;
    }

    sched_condition_destroy(&implementation->lifecycle_condition);
    sched_mutex_destroy(&implementation->lifecycle_mutex);
    concurrent_task_queue_destroy(&implementation->queue);
    free(implementation);
    scheduler->implementation = NULL;
    return SCHEDULER_OK;
}

const char *scheduler_result_name(SchedulerResult result)
{
    switch (result) {
    case SCHEDULER_OK:
        return "OK";
    case SCHEDULER_ERROR_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case SCHEDULER_ERROR_INVALID_STATE:
        return "INVALID_STATE";
    case SCHEDULER_ERROR_ALLOCATION:
        return "ALLOCATION_ERROR";
    case SCHEDULER_ERROR_QUEUE_FULL:
        return "QUEUE_FULL";
    case SCHEDULER_ERROR_SHUTDOWN:
        return "SHUTDOWN";
    case SCHEDULER_ERROR_SYSTEM:
        return "SYSTEM_ERROR";
    default:
        return "UNKNOWN";
    }
}

#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
bool scheduler_profiling_snapshot(
    Scheduler *scheduler,
    SchedulerProfilingSnapshot *snapshot
)
{
    SchedulerImplementation *implementation;

    if (scheduler == NULL || scheduler->implementation == NULL
        || snapshot == NULL) {
        return false;
    }
    implementation = scheduler->implementation;
    if (implementation->state != SCHEDULER_STATE_STOPPED) {
        return false;
    }
    return concurrent_task_queue_profiling_snapshot(
        &implementation->queue,
        implementation->worker_count,
        snapshot
    );
}

size_t scheduler_profiling_current_worker_index(void)
{
    return scheduler_profiling_worker_index;
}
#endif
