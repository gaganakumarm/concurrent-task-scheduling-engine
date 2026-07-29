#include "concurrent_scheduler/concurrent_task_queue.h"

#include "internal/scheduler_observability.h"
#include "platform/sync.h"

#include <stdint.h>
#include <stdlib.h>
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
#include <string.h>
#endif

#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
#include "internal/scheduler_profiling.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

typedef struct {
    SchedMutex mutex;
    SchedCondition not_empty;
    SchedCondition not_full;
    bool shutdown_requested;
    size_t waiting_consumers;
    size_t waiting_producers;
    size_t high_water_mark;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    SchedulerProfilingSnapshot profiling;
#endif
} ConcurrentTaskQueueImplementation;

#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
static _Thread_local size_t profiling_worker_index =
    SCHEDULER_PROFILING_MAX_WORKERS;

static uint64_t profiling_now(
    ConcurrentTaskQueueImplementation *implementation
)
{
    LARGE_INTEGER value;

    if (!QueryPerformanceCounter(&value) || value.QuadPart < 0) {
        implementation->profiling.timing_failed = true;
        return 0U;
    }
    return (uint64_t)value.QuadPart;
}

static void profiling_add(
    ConcurrentTaskQueueImplementation *implementation,
    uint64_t *target,
    uint64_t value
)
{
    if (UINT64_MAX - *target < value) {
        implementation->profiling.counter_overflow = true;
    } else {
        *target += value;
    }
}

static void profiling_increment(
    ConcurrentTaskQueueImplementation *implementation,
    uint64_t *target
)
{
    profiling_add(implementation, target, 1U);
}

static uint64_t profiling_elapsed(
    ConcurrentTaskQueueImplementation *implementation,
    uint64_t start,
    uint64_t end
)
{
    if (end < start) {
        implementation->profiling.timing_failed = true;
        return 0U;
    }
    return end - start;
}

static bool profiling_inferred_contended(
    ConcurrentTaskQueueImplementation *implementation,
    uint64_t start,
    uint64_t end
)
{
    uint64_t threshold =
        implementation->profiling.timer_frequency / UINT64_C(1000000);

    if (threshold == 0U) {
        threshold = 1U;
    }
    return profiling_elapsed(implementation, start, end) > threshold;
}

static void profiling_record_duration(
    ConcurrentTaskQueueImplementation *implementation,
    uint64_t start,
    uint64_t end,
    uint64_t *total,
    uint64_t *maximum
)
{
    uint64_t duration = profiling_elapsed(implementation, start, end);

    profiling_add(implementation, total, duration);
    if (duration > *maximum) {
        *maximum = duration;
    }
}

static void profiling_record_occupancy(
    ConcurrentTaskQueue *queue,
    ConcurrentTaskQueueImplementation *implementation
)
{
    uint64_t occupancy = (uint64_t)task_queue_size(&queue->queue);
    uint64_t capacity = (uint64_t)task_queue_capacity(&queue->queue);
    SchedulerProfilingSnapshot *profile = &implementation->profiling;

    profiling_increment(implementation, &profile->occupancy_sample_count);
    profiling_add(
        implementation,
        &profile->occupancy_sample_sum,
        occupancy
    );
    if (profile->occupancy_sample_count == 1U
        || occupancy < profile->occupancy_min) {
        profile->occupancy_min = occupancy;
    }
    if (occupancy > profile->occupancy_max) {
        profile->occupancy_max = occupancy;
    }
    if (occupancy == 0U) {
        profiling_increment(
            implementation,
            &profile->occupancy_zero_observations
        );
    }
    if (occupancy == capacity) {
        profiling_increment(
            implementation,
            &profile->occupancy_full_observations
        );
    }
}
#endif

static ConcurrentTaskQueueResult map_task_queue_result(TaskQueueResult result)
{
    switch (result) {
    case TASK_QUEUE_OK:
        return CONCURRENT_TASK_QUEUE_OK;
    case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    case TASK_QUEUE_ERROR_ALLOCATION:
        return CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION;
    default:
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }
}

static ConcurrentTaskQueueResult map_sync_result(SchedSyncResult result)
{
    switch (result) {
    case SCHED_SYNC_OK:
        return CONCURRENT_TASK_QUEUE_OK;
    case SCHED_SYNC_ERROR_INVALID_ARGUMENT:
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    case SCHED_SYNC_ERROR_SYSTEM:
    default:
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }
}

ConcurrentTaskQueueResult concurrent_task_queue_init(
    ConcurrentTaskQueue *queue,
    size_t capacity
)
{
    TaskQueue temporary_queue = {0};
    ConcurrentTaskQueueImplementation *implementation;
    TaskQueueResult queue_result;
    SchedSyncResult sync_result;

    if (queue == NULL
        || capacity == 0U
        || capacity > SIZE_MAX / sizeof(Task *)) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    queue_result = task_queue_init(&temporary_queue, capacity);
    if (queue_result != TASK_QUEUE_OK) {
        return map_task_queue_result(queue_result);
    }

    implementation = calloc(1U, sizeof(*implementation));
    if (implementation == NULL) {
        task_queue_destroy(&temporary_queue);
        return CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION;
    }
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    {
        LARGE_INTEGER frequency;

        if (!QueryPerformanceFrequency(&frequency)
            || frequency.QuadPart <= 0) {
            free(implementation);
            task_queue_destroy(&temporary_queue);
            return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        }
        implementation->profiling.timer_frequency =
            (uint64_t)frequency.QuadPart;
    }
#endif

    sync_result = sched_mutex_init(&implementation->mutex);
    if (sync_result != SCHED_SYNC_OK) {
        free(implementation);
        task_queue_destroy(&temporary_queue);
        return map_sync_result(sync_result);
    }

    sync_result = sched_condition_init(&implementation->not_empty);
    if (sync_result != SCHED_SYNC_OK) {
        sched_mutex_destroy(&implementation->mutex);
        free(implementation);
        task_queue_destroy(&temporary_queue);
        return map_sync_result(sync_result);
    }

    sync_result = sched_condition_init(&implementation->not_full);
    if (sync_result != SCHED_SYNC_OK) {
        sched_condition_destroy(&implementation->not_empty);
        sched_mutex_destroy(&implementation->mutex);
        free(implementation);
        task_queue_destroy(&temporary_queue);
        return map_sync_result(sync_result);
    }

    queue->queue = temporary_queue;
    queue->implementation = implementation;
    return CONCURRENT_TASK_QUEUE_OK;
}

void concurrent_task_queue_destroy(ConcurrentTaskQueue *queue)
{
    ConcurrentTaskQueueImplementation *implementation;

    if (queue == NULL) {
        return;
    }

    implementation = queue->implementation;
    if (implementation != NULL) {
        sched_condition_destroy(&implementation->not_full);
        sched_condition_destroy(&implementation->not_empty);
        sched_mutex_destroy(&implementation->mutex);
        free(implementation);
    }

    task_queue_destroy(&queue->queue);
    queue->implementation = NULL;
}

ConcurrentTaskQueueResult concurrent_task_queue_shutdown(
    ConcurrentTaskQueue *queue
)
{
    ConcurrentTaskQueueImplementation *implementation;
    bool synchronization_failed = false;

    if (queue == NULL || queue->implementation == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    implementation->shutdown_requested = true;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    profiling_add(
        implementation,
        &implementation->profiling.shutdown_broadcasts,
        2U
    );
#endif
    if (sched_condition_broadcast(&implementation->not_empty)
        != SCHED_SYNC_OK) {
        synchronization_failed = true;
    }
    if (sched_condition_broadcast(&implementation->not_full)
        != SCHED_SYNC_OK) {
        synchronization_failed = true;
    }
    if (sched_mutex_unlock(&implementation->mutex) != SCHED_SYNC_OK) {
        synchronization_failed = true;
    }

    return synchronization_failed
        ? CONCURRENT_TASK_QUEUE_ERROR_SYSTEM
        : CONCURRENT_TASK_QUEUE_OK;
}

ConcurrentTaskQueueResult concurrent_task_queue_try_enqueue(
    ConcurrentTaskQueue *queue,
    Task *task
)
{
    ConcurrentTaskQueueImplementation *implementation;
    TaskQueueResult queue_result;
    ConcurrentTaskQueueResult result;
    bool inserted = false;
    bool was_empty = false;
#if !CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
    (void)was_empty;
#endif

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (implementation->shutdown_requested) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN;
    } else {
        was_empty = task_queue_is_empty(&queue->queue);
        queue_result = task_queue_enqueue(&queue->queue, task);
        switch (queue_result) {
        case TASK_QUEUE_OK:
            result = CONCURRENT_TASK_QUEUE_OK;
            inserted = true;
            if (task_queue_size(&queue->queue)
                > implementation->high_water_mark) {
                implementation->high_water_mark =
                    task_queue_size(&queue->queue);
            }
            break;
        case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
            result = CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
            break;
        case TASK_QUEUE_ERROR_FULL:
            result = CONCURRENT_TASK_QUEUE_ERROR_FULL;
            break;
        default:
            result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
            break;
        }
    }

    if (inserted
#if CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
        && (was_empty || implementation->waiting_consumers > 0U)
#endif
        && sched_condition_signal(&implementation->not_empty)
            != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (sched_mutex_unlock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    return result;
}

ConcurrentTaskQueueResult concurrent_task_queue_enqueue(
    ConcurrentTaskQueue *queue,
    Task *task
)
{
    ConcurrentTaskQueueImplementation *implementation;
    TaskQueueResult queue_result;
    ConcurrentTaskQueueResult result;
    bool inserted = false;
    bool was_empty = false;
#if !CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
    (void)was_empty;
#endif
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    uint64_t lock_start;
    uint64_t lock_end;
    uint64_t wait_start;
    uint64_t wait_end;
    bool waited = false;
#endif

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    lock_start = profiling_now(implementation);
#endif
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    lock_end = profiling_now(implementation);
    profiling_increment(
        implementation,
        &implementation->profiling.enqueue_attempts
    );
    profiling_increment(
        implementation,
        &implementation->profiling.enqueue_lock_attempts
    );
    profiling_record_duration(
        implementation,
        lock_start,
        lock_end,
        &implementation->profiling.enqueue_lock_wait_ticks,
        &implementation->profiling.enqueue_lock_max_wait_ticks
    );
    if (profiling_inferred_contended(
            implementation,
            lock_start,
            lock_end
        )) {
        profiling_increment(
            implementation,
            &implementation->profiling.enqueue_inferred_contended
        );
    }
#endif

    while (!implementation->shutdown_requested
        && task_queue_is_full(&queue->queue)) {
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
        waited = true;
        profiling_increment(
            implementation,
            &implementation->profiling.queue_full_observations
        );
        profiling_increment(
            implementation,
            &implementation->profiling.enqueue_wait_count
        );
        wait_start = profiling_now(implementation);
#endif
        ++implementation->waiting_producers;
        if (sched_condition_wait(
                &implementation->not_full,
                &implementation->mutex
            ) != SCHED_SYNC_OK) {
            --implementation->waiting_producers;
            (void)sched_mutex_unlock(&implementation->mutex);
            return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        }
        --implementation->waiting_producers;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
        wait_end = profiling_now(implementation);
        profiling_increment(
            implementation,
            &implementation->profiling.enqueue_wait_returns
        );
        profiling_record_duration(
            implementation,
            wait_start,
            wait_end,
            &implementation->profiling.enqueue_wait_ticks,
            &implementation->profiling.enqueue_wait_max_ticks
        );
        if (!implementation->shutdown_requested
            && task_queue_is_full(&queue->queue)) {
            profiling_increment(
                implementation,
                &implementation->profiling
                    .enqueue_predicate_false_wakeups
            );
        }
#endif
    }

    if (implementation->shutdown_requested) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN;
    } else {
        was_empty = task_queue_is_empty(&queue->queue);
        queue_result = task_queue_enqueue(&queue->queue, task);
        switch (queue_result) {
        case TASK_QUEUE_OK:
            result = CONCURRENT_TASK_QUEUE_OK;
            inserted = true;
            if (task_queue_size(&queue->queue)
                > implementation->high_water_mark) {
                implementation->high_water_mark =
                    task_queue_size(&queue->queue);
            }
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
            if (waited) {
                profiling_increment(
                    implementation,
                    &implementation->profiling.enqueue_successes_after_wait
                );
            } else {
                profiling_increment(
                    implementation,
                    &implementation->profiling.enqueue_immediate_successes
                );
            }
            profiling_record_occupancy(queue, implementation);
#endif
            break;
        case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
            result = CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
            break;
        case TASK_QUEUE_ERROR_FULL:
        default:
            result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
            break;
        }
    }

    if (inserted
#if CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
        && (was_empty || implementation->waiting_consumers > 0U)
#endif
        && sched_condition_signal(&implementation->not_empty)
            != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    if (inserted && (
#if CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
            (was_empty || implementation->waiting_consumers > 0U)
#else
            true
#endif
        )) {
        profiling_increment(
            implementation,
            &implementation->profiling.not_empty_signals
        );
    } else if (inserted) {
        profiling_increment(
            implementation,
            &implementation->profiling.avoided_not_empty_signals
        );
    }
#endif

    if (sched_mutex_unlock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    return result;
}

ConcurrentTaskQueueResult concurrent_task_queue_try_dequeue(
    ConcurrentTaskQueue *queue,
    Task **task
)
{
    ConcurrentTaskQueueImplementation *implementation;
    Task *removed_task = NULL;
    TaskQueueResult queue_result;
    ConcurrentTaskQueueResult result;
    bool removed = false;
    bool was_full = false;
#if !CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
    (void)was_full;
#endif

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (task_queue_is_empty(&queue->queue)) {
        queue_result = TASK_QUEUE_ERROR_EMPTY;
        result = implementation->shutdown_requested
            ? CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN
            : CONCURRENT_TASK_QUEUE_ERROR_EMPTY;
    } else {
        was_full = task_queue_is_full(&queue->queue);
        queue_result = task_queue_dequeue(&queue->queue, &removed_task);
        switch (queue_result) {
        case TASK_QUEUE_OK:
            result = CONCURRENT_TASK_QUEUE_OK;
            removed = true;
            break;
        case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
            result = CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
            break;
        case TASK_QUEUE_ERROR_EMPTY:
        default:
            result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
            break;
        }
    }

    if (removed
#if CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
        && (was_full || implementation->waiting_producers > 0U)
#endif
        && sched_condition_signal(&implementation->not_full)
            != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (sched_mutex_unlock(&implementation->mutex) != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (removed) {
        *task = removed_task;
    }
    return result;
}

ConcurrentTaskQueueResult concurrent_task_queue_dequeue(
    ConcurrentTaskQueue *queue,
    Task **task
)
{
    ConcurrentTaskQueueImplementation *implementation;
    Task *removed_task = NULL;
    TaskQueueResult queue_result;
    ConcurrentTaskQueueResult result;
    bool removed = false;
    bool was_full = false;
#if !CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
    (void)was_full;
#endif
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    uint64_t lock_start;
    uint64_t lock_end;
    uint64_t wait_start;
    uint64_t wait_end;
    bool waited = false;
    size_t worker_index;
#endif

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    worker_index = profiling_worker_index;
    lock_start = profiling_now(implementation);
#endif
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    lock_end = profiling_now(implementation);
    profiling_increment(
        implementation,
        &implementation->profiling.dequeue_attempts
    );
    profiling_increment(
        implementation,
        &implementation->profiling.dequeue_lock_attempts
    );
    profiling_record_duration(
        implementation,
        lock_start,
        lock_end,
        &implementation->profiling.dequeue_lock_wait_ticks,
        &implementation->profiling.dequeue_lock_max_wait_ticks
    );
    if (profiling_inferred_contended(
            implementation,
            lock_start,
            lock_end
        )) {
        profiling_increment(
            implementation,
            &implementation->profiling.dequeue_inferred_contended
        );
    }
    if (worker_index < SCHEDULER_PROFILING_MAX_WORKERS) {
        profiling_add(
            implementation,
            &implementation->profiling.worker_dequeue_lock_ticks[
                worker_index
            ],
            profiling_elapsed(implementation, lock_start, lock_end)
        );
    }
#endif

    while (!implementation->shutdown_requested
        && task_queue_is_empty(&queue->queue)) {
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
        waited = true;
        profiling_increment(
            implementation,
            &implementation->profiling.queue_empty_observations
        );
        profiling_increment(
            implementation,
            &implementation->profiling.dequeue_wait_count
        );
        if (worker_index < SCHEDULER_PROFILING_MAX_WORKERS) {
            profiling_increment(
                implementation,
                &implementation->profiling.worker_dequeue_wait_count[
                    worker_index
                ]
            );
        }
        wait_start = profiling_now(implementation);
#endif
        ++implementation->waiting_consumers;
        if (sched_condition_wait(
                &implementation->not_empty,
                &implementation->mutex
            ) != SCHED_SYNC_OK) {
            --implementation->waiting_consumers;
            (void)sched_mutex_unlock(&implementation->mutex);
            return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        }
        --implementation->waiting_consumers;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
        wait_end = profiling_now(implementation);
        profiling_increment(
            implementation,
            &implementation->profiling.dequeue_wait_returns
        );
        profiling_record_duration(
            implementation,
            wait_start,
            wait_end,
            &implementation->profiling.dequeue_wait_ticks,
            &implementation->profiling.dequeue_wait_max_ticks
        );
        if (worker_index < SCHEDULER_PROFILING_MAX_WORKERS) {
            profiling_add(
                implementation,
                &implementation->profiling.worker_dequeue_wait_ticks[
                    worker_index
                ],
                profiling_elapsed(implementation, wait_start, wait_end)
            );
        }
        if (!implementation->shutdown_requested
            && task_queue_is_empty(&queue->queue)) {
            profiling_increment(
                implementation,
                &implementation->profiling
                    .dequeue_predicate_false_wakeups
            );
        }
#endif
    }

    if (task_queue_is_empty(&queue->queue)) {
        queue_result = TASK_QUEUE_ERROR_EMPTY;
        result = CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN;
    } else {
        was_full = task_queue_is_full(&queue->queue);
        queue_result = task_queue_dequeue(&queue->queue, &removed_task);
        switch (queue_result) {
        case TASK_QUEUE_OK:
            result = CONCURRENT_TASK_QUEUE_OK;
            removed = true;
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
            if (waited) {
                profiling_increment(
                    implementation,
                    &implementation->profiling.dequeue_successes_after_wait
                );
            } else {
                profiling_increment(
                    implementation,
                    &implementation->profiling.dequeue_immediate_successes
                );
            }
            if (worker_index < SCHEDULER_PROFILING_MAX_WORKERS) {
                profiling_increment(
                    implementation,
                    &implementation->profiling.worker_tasks[worker_index]
                );
            }
            profiling_record_occupancy(queue, implementation);
#endif
            break;
        case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
            result = CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
            break;
        case TASK_QUEUE_ERROR_EMPTY:
        default:
            result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
            break;
        }
    }

    if (removed
#if CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
        && (was_full || implementation->waiting_producers > 0U)
#endif
        && sched_condition_signal(&implementation->not_full)
            != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }
#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
    if (removed && (
#if CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
            (was_full || implementation->waiting_producers > 0U)
#else
            true
#endif
        )) {
        profiling_increment(
            implementation,
            &implementation->profiling.not_full_signals
        );
    } else if (removed) {
        profiling_increment(
            implementation,
            &implementation->profiling.avoided_not_full_signals
        );
    }
#endif

    if (sched_mutex_unlock(&implementation->mutex) != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (removed) {
        *task = removed_task;
    }
    return result;
}

ConcurrentTaskQueueResult concurrent_task_queue_try_peek(
    ConcurrentTaskQueue *queue,
    Task **task
)
{
    ConcurrentTaskQueueImplementation *implementation;
    Task *peeked_task = NULL;
    TaskQueueResult queue_result;
    ConcurrentTaskQueueResult result;
    bool peeked = false;

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (task_queue_is_empty(&queue->queue)) {
        queue_result = TASK_QUEUE_ERROR_EMPTY;
        result = implementation->shutdown_requested
            ? CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN
            : CONCURRENT_TASK_QUEUE_ERROR_EMPTY;
    } else {
        queue_result = task_queue_peek(&queue->queue, &peeked_task);
        switch (queue_result) {
        case TASK_QUEUE_OK:
            result = CONCURRENT_TASK_QUEUE_OK;
            peeked = true;
            break;
        case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
            result = CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
            break;
        case TASK_QUEUE_ERROR_EMPTY:
        default:
            result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
            break;
        }
    }

    if (sched_mutex_unlock(&implementation->mutex) != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    if (peeked) {
        *task = peeked_task;
    }
    return result;
}

bool concurrent_task_queue_is_empty(ConcurrentTaskQueue *queue)
{
    ConcurrentTaskQueueImplementation *implementation;
    bool result;

    if (queue == NULL || queue->implementation == NULL) {
        return true;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return true;
    }

    result = task_queue_is_empty(&queue->queue);
    (void)sched_mutex_unlock(&implementation->mutex);
    return result;
}

bool concurrent_task_queue_is_full(ConcurrentTaskQueue *queue)
{
    ConcurrentTaskQueueImplementation *implementation;
    bool result;

    if (queue == NULL || queue->implementation == NULL) {
        return false;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return false;
    }

    result = task_queue_is_full(&queue->queue);
    (void)sched_mutex_unlock(&implementation->mutex);
    return result;
}

size_t concurrent_task_queue_size(ConcurrentTaskQueue *queue)
{
    ConcurrentTaskQueueImplementation *implementation;
    size_t result;

    if (queue == NULL || queue->implementation == NULL) {
        return 0U;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return 0U;
    }

    result = task_queue_size(&queue->queue);
    (void)sched_mutex_unlock(&implementation->mutex);
    return result;
}

size_t concurrent_task_queue_capacity(ConcurrentTaskQueue *queue)
{
    ConcurrentTaskQueueImplementation *implementation;
    size_t result;

    if (queue == NULL || queue->implementation == NULL) {
        return 0U;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return 0U;
    }

    result = task_queue_capacity(&queue->queue);
    (void)sched_mutex_unlock(&implementation->mutex);
    return result;
}

bool concurrent_task_queue_capture_runtime_snapshot(
    ConcurrentTaskQueue *queue,
    ConcurrentTaskQueueRuntimeSnapshot *snapshot
)
{
    ConcurrentTaskQueueImplementation *implementation;
    ConcurrentTaskQueueRuntimeSnapshot temporary;

    if (queue == NULL || queue->implementation == NULL || snapshot == NULL) {
        return false;
    }
    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return false;
    }
    temporary.capacity = task_queue_capacity(&queue->queue);
    temporary.current_size = task_queue_size(&queue->queue);
    temporary.high_water_mark = implementation->high_water_mark;
    if (sched_mutex_unlock(&implementation->mutex) != SCHED_SYNC_OK) {
        return false;
    }
    *snapshot = temporary;
    return true;
}

#if defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
void concurrent_task_queue_profiling_set_worker_index(size_t worker_index)
{
    profiling_worker_index = worker_index;
}

bool concurrent_task_queue_profiling_snapshot(
    ConcurrentTaskQueue *queue,
    size_t worker_count,
    SchedulerProfilingSnapshot *snapshot
)
{
    ConcurrentTaskQueueImplementation *implementation;

    if (queue == NULL || queue->implementation == NULL || snapshot == NULL
        || worker_count > SCHEDULER_PROFILING_MAX_WORKERS) {
        return false;
    }
    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return false;
    }
    implementation->profiling.worker_count = worker_count;
    memcpy(snapshot, &implementation->profiling, sizeof(*snapshot));
    return sched_mutex_unlock(&implementation->mutex) == SCHED_SYNC_OK;
}
#endif

const char *concurrent_task_queue_result_name(
    ConcurrentTaskQueueResult result
)
{
    switch (result) {
    case CONCURRENT_TASK_QUEUE_OK:
        return "OK";
    case CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION:
        return "ALLOCATION_ERROR";
    case CONCURRENT_TASK_QUEUE_ERROR_SYSTEM:
        return "SYSTEM_ERROR";
    case CONCURRENT_TASK_QUEUE_ERROR_FULL:
        return "FULL";
    case CONCURRENT_TASK_QUEUE_ERROR_EMPTY:
        return "EMPTY";
    case CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN:
        return "SHUTDOWN";
    default:
        return "UNKNOWN";
    }
}
