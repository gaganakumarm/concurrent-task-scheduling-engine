#include "concurrent_scheduler/scheduler.h"

#include "concurrent_scheduler/concurrent_task_queue.h"
#include "internal/scheduler_observability.h"
#include "platform/sync.h"
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
#include "internal/scheduler_profiling.h"
static _Thread_local size_t scheduler_profiling_worker_index =
    SCHEDULER_PROFILING_MAX_WORKERS;
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

_Static_assert(
    ATOMIC_LLONG_LOCK_FREE == 2,
    "Runtime callback accounting requires lock-free 64-bit atomics."
);

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
    _Alignas(64) _Atomic uint64_t dequeued_count;
    _Atomic uint64_t callback_started_count;
    _Atomic uint64_t callback_succeeded_count;
    _Atomic uint64_t callback_failed_count;
    _Atomic uint64_t currently_running_count;
    atomic_bool accounting_overflow;
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
    uint64_t submitted_count;
    uint64_t accepted_count;
    uint64_t rejected_count;
    bool lifecycle_accounting_overflow;
    SchedThread *threads;
    WorkerContext *worker_contexts;
    size_t started_worker_count;
    size_t created_worker_count;
    SchedMutex worker_mutex;
    SchedCondition worker_condition;
    size_t ready_worker_count;
    size_t active_worker_count;
    size_t joined_worker_count;
    uint64_t dequeued_count;
    uint64_t callback_started_count;
    uint64_t successful_callback_count;
    uint64_t failed_callback_count;
    uint64_t worker_startup_failure_count;
    uint64_t worker_runtime_failure_count;
    uint64_t join_failure_count;
    bool worker_accounting_overflow;
    int worker_infrastructure_failure;
    int worker_sync_initialized;
};

enum {
    WORKER_RESULT_OK = 0,
    WORKER_RESULT_INVALID_CONTEXT = -1,
    WORKER_RESULT_SYNCHRONIZATION = -2,
    WORKER_RESULT_QUEUE = -3
};

static void add_saturating(
    uint64_t *target,
    uint64_t value,
    bool *overflow_detected
)
{
    if (UINT64_MAX - *target < value) {
        *target = UINT64_MAX;
        *overflow_detected = true;
    } else {
        *target += value;
    }
}

static int record_worker_exit(
    SchedulerImplementation *implementation,
    int worker_result
)
{
    if (sched_mutex_lock(&implementation->worker_mutex) != SCHED_SYNC_OK) {
        return WORKER_RESULT_SYNCHRONIZATION;
    }
    if (implementation->active_worker_count == 0U) {
        implementation->worker_infrastructure_failure = 1;
        scheduler_observability_increment_saturating(
            &implementation->worker_runtime_failure_count,
            &implementation->worker_accounting_overflow
        );
        worker_result = WORKER_RESULT_SYNCHRONIZATION;
    } else {
        --implementation->active_worker_count;
    }
    if (worker_result != WORKER_RESULT_OK) {
        implementation->worker_infrastructure_failure = 1;
        scheduler_observability_increment_saturating(
            &implementation->worker_runtime_failure_count,
            &implementation->worker_accounting_overflow
        );
    }
    if (sched_mutex_unlock(&implementation->worker_mutex)
        != SCHED_SYNC_OK) {
        return WORKER_RESULT_SYNCHRONIZATION;
    }
    return worker_result;
}

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
    bool running_accounted;

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
    implementation->active_worker_count++;
    implementation->ready_worker_count++;
    if (sched_condition_broadcast(&implementation->worker_condition)
        != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&implementation->worker_mutex);
        return record_worker_exit(
            implementation,
            WORKER_RESULT_SYNCHRONIZATION
        );
    }
    if (sched_mutex_unlock(&implementation->worker_mutex)
        != SCHED_SYNC_OK) {
        (void)concurrent_task_queue_shutdown(&implementation->queue);
        return record_worker_exit(
            implementation,
            WORKER_RESULT_SYNCHRONIZATION
        );
    }

    for (;;) {
        task = NULL;
        queue_result = concurrent_task_queue_dequeue(
            &implementation->queue,
            &task
        );
        if (queue_result == CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN) {
            return record_worker_exit(implementation, WORKER_RESULT_OK);
        }
        if (queue_result != CONCURRENT_TASK_QUEUE_OK || task == NULL) {
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            return record_worker_exit(
                implementation,
                WORKER_RESULT_QUEUE
            );
        }

        (void)scheduler_observability_increment_single_writer_atomic(
            &context->dequeued_count,
            &context->accounting_overflow
        );
        (void)scheduler_observability_increment_single_writer_atomic(
            &context->callback_started_count,
            &context->accounting_overflow
        );
        running_accounted =
            scheduler_observability_increment_single_writer_atomic(
            &context->currently_running_count,
            &context->accounting_overflow
        );

        callback_result = implementation->execute(
            task,
            implementation->execute_context
        );
        if (callback_result == 0) {
            (void)scheduler_observability_increment_single_writer_atomic(
                &context->callback_succeeded_count,
                &context->accounting_overflow
            );
        } else {
            (void)scheduler_observability_increment_single_writer_atomic(
                &context->callback_failed_count,
                &context->accounting_overflow
            );
        }
        if (running_accounted) {
            atomic_store(
                &context->currently_running_count,
                atomic_load(&context->currently_running_count)
                    - UINT64_C(1)
            );
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
            if (sched_mutex_lock(&implementation->worker_mutex)
                == SCHED_SYNC_OK) {
                scheduler_observability_increment_saturating(
                    &implementation->join_failure_count,
                    &implementation->worker_accounting_overflow
                );
                (void)sched_mutex_unlock(&implementation->worker_mutex);
            }
            result = SCHEDULER_ERROR_SYSTEM;
            all_joined = 0;
            continue;
        }
        if (worker_result != WORKER_RESULT_OK) {
            result = SCHEDULER_ERROR_SYSTEM;
        } else if (sched_mutex_lock(&implementation->worker_mutex)
            != SCHED_SYNC_OK) {
            result = SCHEDULER_ERROR_SYSTEM;
        } else {
            implementation->joined_worker_count++;
            if (sched_mutex_unlock(&implementation->worker_mutex)
                != SCHED_SYNC_OK) {
                result = SCHEDULER_ERROR_SYSTEM;
            }
        }
        sched_thread_destroy(&implementation->threads[index]);
    }

    if (!all_joined) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    if (implementation->worker_infrastructure_failure) {
        result = SCHEDULER_ERROR_SYSTEM;
    }

    if (sched_mutex_lock(&implementation->worker_mutex) != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
    }
    for (index = 0U;
         index < implementation->started_worker_count;
         ++index) {
        WorkerContext *context = &implementation->worker_contexts[index];

        add_saturating(
            &implementation->dequeued_count,
            atomic_load(&context->dequeued_count),
            &implementation->worker_accounting_overflow
        );
        add_saturating(
            &implementation->callback_started_count,
            atomic_load(&context->callback_started_count),
            &implementation->worker_accounting_overflow
        );
        add_saturating(
            &implementation->successful_callback_count,
            atomic_load(&context->callback_succeeded_count),
            &implementation->worker_accounting_overflow
        );
        add_saturating(
            &implementation->failed_callback_count,
            atomic_load(&context->callback_failed_count),
            &implementation->worker_accounting_overflow
        );
        if (atomic_load(&context->accounting_overflow)) {
            implementation->worker_accounting_overflow = true;
        }
    }
    free(implementation->worker_contexts);
    free(implementation->threads);
    implementation->worker_contexts = NULL;
    implementation->threads = NULL;
    implementation->started_worker_count = 0U;
    implementation->ready_worker_count = 0U;
    if (sched_mutex_unlock(&implementation->worker_mutex) != SCHED_SYNC_OK) {
        return SCHEDULER_ERROR_SYSTEM;
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
    scheduler_observability_increment_saturating(
        &(*implementation)->submitted_count,
        &(*implementation)->lifecycle_accounting_overflow
    );
    if ((*implementation)->state != SCHEDULER_STATE_RUNNING) {
        SchedulerResult state_result =
            ((*implementation)->state == SCHEDULER_STATE_SHUTTING_DOWN
                || (*implementation)->state == SCHEDULER_STATE_STOPPED)
            ? SCHEDULER_ERROR_SHUTDOWN
            : SCHEDULER_ERROR_INVALID_STATE;

        scheduler_observability_increment_saturating(
            &(*implementation)->rejected_count,
            &(*implementation)->lifecycle_accounting_overflow
        );
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
    if (operation_result == SCHEDULER_OK) {
        scheduler_observability_increment_saturating(
            &implementation->accepted_count,
            &implementation->lifecycle_accounting_overflow
        );
    } else {
        scheduler_observability_increment_saturating(
            &implementation->rejected_count,
            &implementation->lifecycle_accounting_overflow
        );
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
    if (sched_mutex_lock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        sched_condition_destroy(&implementation->worker_condition);
        sched_mutex_destroy(&implementation->worker_mutex);
        free(implementation->worker_contexts);
        free(implementation->threads);
        implementation->worker_contexts = NULL;
        implementation->threads = NULL;
        (void)set_scheduler_state(
            implementation,
            SCHEDULER_STATE_FAILED
        );
        return SCHEDULER_ERROR_SYSTEM;
    }
    implementation->worker_sync_initialized = 1;
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        (void)concurrent_task_queue_shutdown(&implementation->queue);
        (void)release_workers(implementation);
        (void)set_scheduler_state(implementation, SCHEDULER_STATE_FAILED);
        return SCHEDULER_ERROR_SYSTEM;
    }

    for (index = 0U; index < implementation->worker_count; index++) {
        implementation->worker_contexts[index].scheduler = implementation;
        implementation->worker_contexts[index].worker_index = index;
        if (sched_mutex_lock(&implementation->worker_mutex)
            != SCHED_SYNC_OK) {
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            (void)release_workers(implementation);
            (void)set_scheduler_state(
                implementation,
                SCHEDULER_STATE_FAILED
            );
            return SCHEDULER_ERROR_SYSTEM;
        }
        if (sched_thread_create(
                &implementation->threads[index],
                scheduler_worker_entry,
                &implementation->worker_contexts[index]
            ) != SCHED_SYNC_OK) {
            scheduler_observability_increment_saturating(
                &implementation->worker_startup_failure_count,
                &implementation->worker_accounting_overflow
            );
            (void)sched_mutex_unlock(&implementation->worker_mutex);
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            (void)release_workers(implementation);
            (void)set_scheduler_state(
                implementation,
                SCHEDULER_STATE_FAILED
            );
            return SCHEDULER_ERROR_SYSTEM;
        }
        implementation->started_worker_count++;
        implementation->created_worker_count++;
        if (sched_mutex_unlock(&implementation->worker_mutex)
            != SCHED_SYNC_OK) {
            (void)concurrent_task_queue_shutdown(&implementation->queue);
            (void)release_workers(implementation);
            (void)set_scheduler_state(
                implementation,
                SCHEDULER_STATE_FAILED
            );
            return SCHEDULER_ERROR_SYSTEM;
        }
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

static SchedulerSnapshotState snapshot_state_from_internal(
    SchedulerState state
)
{
    switch (state) {
    case SCHEDULER_STATE_INITIALIZED:
        return SCHEDULER_SNAPSHOT_STATE_INITIALIZED;
    case SCHEDULER_STATE_STARTING:
        return SCHEDULER_SNAPSHOT_STATE_STARTING;
    case SCHEDULER_STATE_RUNNING:
        return SCHEDULER_SNAPSHOT_STATE_RUNNING;
    case SCHEDULER_STATE_SHUTTING_DOWN:
        return SCHEDULER_SNAPSHOT_STATE_SHUTTING_DOWN;
    case SCHEDULER_STATE_STOPPED:
        return SCHEDULER_SNAPSHOT_STATE_STOPPED;
    case SCHEDULER_STATE_FAILED:
    default:
        return SCHEDULER_SNAPSHOT_STATE_FAILED;
    }
}

bool scheduler_capture_snapshot(
    Scheduler *scheduler,
    SchedulerSnapshot *snapshot
)
{
    SchedulerImplementation *implementation;
    SchedulerSnapshot temporary = {0};
    ConcurrentTaskQueueRuntimeSnapshot queue_snapshot;
    size_t index;
    bool worker_sync_available;

    if (scheduler == NULL || scheduler->implementation == NULL
        || snapshot == NULL) {
        return false;
    }
    implementation = scheduler->implementation;
    temporary.version = CONCURRENT_SCHEDULER_SNAPSHOT_VERSION;
    temporary.consistency =
        SCHEDULER_SNAPSHOT_CONSISTENCY_DOMAIN_EXACT;

    /*
     * Hybrid capture order is lifecycle, worker (including per-worker atomic
     * slots), then queue. Mutexes are never nested; the composite is not one
     * instant.
     */
    if (sched_mutex_lock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return false;
    }
    temporary.state = snapshot_state_from_internal(implementation->state);
    temporary.submissions_open =
        implementation->state == SCHEDULER_STATE_RUNNING;
    temporary.shutdown_started =
        implementation->state == SCHEDULER_STATE_SHUTTING_DOWN
        || implementation->state == SCHEDULER_STATE_STOPPED;
    temporary.configured_worker_count = implementation->worker_count;
    temporary.submitted_count = implementation->submitted_count;
    temporary.accepted_count = implementation->accepted_count;
    temporary.rejected_count = implementation->rejected_count;
    temporary.overflow_detected =
        implementation->lifecycle_accounting_overflow;
    worker_sync_available =
        implementation->worker_sync_initialized != 0;
    if (sched_mutex_unlock(&implementation->lifecycle_mutex)
        != SCHED_SYNC_OK) {
        return false;
    }

    if (worker_sync_available) {
        if (sched_mutex_lock(&implementation->worker_mutex)
            != SCHED_SYNC_OK) {
            return false;
        }
        temporary.created_worker_count =
            implementation->created_worker_count;
        temporary.ready_worker_count = implementation->ready_worker_count;
        temporary.active_worker_count = implementation->active_worker_count;
        temporary.joined_worker_count = implementation->joined_worker_count;
        temporary.worker_startup_failure_count =
            implementation->worker_startup_failure_count;
        temporary.worker_runtime_failure_count =
            implementation->worker_runtime_failure_count;
        temporary.join_failure_count = implementation->join_failure_count;
        temporary.dequeued_count = implementation->dequeued_count;
        temporary.callback_started_count =
            implementation->callback_started_count;
        temporary.callback_succeeded_count =
            implementation->successful_callback_count;
        temporary.callback_failed_count =
            implementation->failed_callback_count;
        temporary.overflow_detected = temporary.overflow_detected
            || implementation->worker_accounting_overflow;
        for (index = 0U;
             index < implementation->started_worker_count;
             ++index) {
            WorkerContext *context =
                &implementation->worker_contexts[index];

            add_saturating(
                &temporary.dequeued_count,
                atomic_load(&context->dequeued_count),
                &temporary.overflow_detected
            );
            add_saturating(
                &temporary.callback_started_count,
                atomic_load(&context->callback_started_count),
                &temporary.overflow_detected
            );
            add_saturating(
                &temporary.callback_succeeded_count,
                atomic_load(&context->callback_succeeded_count),
                &temporary.overflow_detected
            );
            add_saturating(
                &temporary.callback_failed_count,
                atomic_load(&context->callback_failed_count),
                &temporary.overflow_detected
            );
            add_saturating(
                &temporary.currently_running_count,
                atomic_load(&context->currently_running_count),
                &temporary.overflow_detected
            );
            if (atomic_load(&context->accounting_overflow)) {
                temporary.overflow_detected = true;
            }
        }
        if (sched_mutex_unlock(&implementation->worker_mutex)
            != SCHED_SYNC_OK) {
            return false;
        }
    }

    if (!concurrent_task_queue_capture_runtime_snapshot(
            &implementation->queue,
            &queue_snapshot
        )) {
        return false;
    }
    temporary.queue_capacity = queue_snapshot.capacity;
    temporary.queue_current_size = queue_snapshot.current_size;
    temporary.queue_high_water_mark = queue_snapshot.high_water_mark;
    *snapshot = temporary;
    return true;
}

bool scheduler_snapshot_validate_basic(const SchedulerSnapshot *snapshot)
{
    if (snapshot == NULL
        || snapshot->version != CONCURRENT_SCHEDULER_SNAPSHOT_VERSION
        || snapshot->consistency
            != SCHEDULER_SNAPSHOT_CONSISTENCY_DOMAIN_EXACT
        || snapshot->queue_current_size > snapshot->queue_capacity
        || snapshot->queue_high_water_mark > snapshot->queue_capacity
        || snapshot->queue_current_size > snapshot->queue_high_water_mark
        || snapshot->created_worker_count
            > snapshot->configured_worker_count
        || snapshot->ready_worker_count > snapshot->created_worker_count
        || snapshot->active_worker_count > snapshot->created_worker_count
        || snapshot->joined_worker_count > snapshot->created_worker_count
        || snapshot->currently_running_count
            > snapshot->active_worker_count
        || snapshot->accepted_count > snapshot->submitted_count
        || snapshot->rejected_count > snapshot->submitted_count
        || snapshot->callback_started_count > snapshot->dequeued_count
        || snapshot->callback_succeeded_count
            > snapshot->callback_started_count
        || snapshot->callback_failed_count
            > snapshot->callback_started_count
                - snapshot->callback_succeeded_count) {
        return false;
    }
    return true;
}

bool scheduler_snapshot_validate_quiescent(
    const SchedulerSnapshot *snapshot
)
{
    if (!scheduler_snapshot_validate_basic(snapshot)
        || snapshot->submitted_count - snapshot->accepted_count
            != snapshot->rejected_count
        || snapshot->accepted_count != snapshot->dequeued_count
        || snapshot->dequeued_count != snapshot->callback_started_count
        || snapshot->callback_started_count
            - snapshot->callback_succeeded_count
            != snapshot->callback_failed_count
        || snapshot->currently_running_count != 0U
        || snapshot->queue_current_size != 0U
        || snapshot->active_worker_count != 0U) {
        return false;
    }
    if (snapshot->state == SCHEDULER_SNAPSHOT_STATE_STOPPED
        && snapshot->joined_worker_count
            != snapshot->created_worker_count) {
        return false;
    }
    return true;
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
