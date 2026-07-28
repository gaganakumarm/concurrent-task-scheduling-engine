#include "concurrent_scheduler/concurrent_task_queue.h"

#include "platform/sync.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct {
    SchedMutex mutex;
    SchedCondition not_empty;
    SchedCondition not_full;
} ConcurrentTaskQueueImplementation;

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

ConcurrentTaskQueueResult concurrent_task_queue_try_enqueue(
    ConcurrentTaskQueue *queue,
    Task *task
)
{
    ConcurrentTaskQueueImplementation *implementation;
    TaskQueueResult queue_result;
    ConcurrentTaskQueueResult result;

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    queue_result = task_queue_enqueue(&queue->queue, task);
    switch (queue_result) {
    case TASK_QUEUE_OK:
        result = CONCURRENT_TASK_QUEUE_OK;
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

    if (queue_result == TASK_QUEUE_OK
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

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    while (task_queue_is_full(&queue->queue)) {
        if (sched_condition_wait(
                &implementation->not_full,
                &implementation->mutex
            ) != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&implementation->mutex);
            return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        }
    }

    queue_result = task_queue_enqueue(&queue->queue, task);
    switch (queue_result) {
    case TASK_QUEUE_OK:
        result = CONCURRENT_TASK_QUEUE_OK;
        break;
    case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
        result = CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
        break;
    case TASK_QUEUE_ERROR_FULL:
    default:
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        break;
    }

    if (queue_result == TASK_QUEUE_OK
        && sched_condition_signal(&implementation->not_empty)
            != SCHED_SYNC_OK) {
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

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

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

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
        result = CONCURRENT_TASK_QUEUE_ERROR_EMPTY;
        break;
    default:
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        break;
    }

    if (removed
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

    if (queue == NULL || queue->implementation == NULL || task == NULL) {
        return CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    implementation = queue->implementation;
    if (sched_mutex_lock(&implementation->mutex) != SCHED_SYNC_OK) {
        return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
    }

    while (task_queue_is_empty(&queue->queue)) {
        if (sched_condition_wait(
                &implementation->not_empty,
                &implementation->mutex
            ) != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&implementation->mutex);
            return CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        }
    }

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

    if (removed
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
        result = CONCURRENT_TASK_QUEUE_ERROR_EMPTY;
        break;
    default:
        result = CONCURRENT_TASK_QUEUE_ERROR_SYSTEM;
        break;
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
