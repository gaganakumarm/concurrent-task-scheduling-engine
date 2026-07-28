#include "concurrent_scheduler/task_queue.h"

#include <stdint.h>
#include <stdlib.h>

TaskQueueResult task_queue_init(TaskQueue *queue, size_t capacity)
{
    Task **items;

    if (queue == NULL
        || capacity == 0U
        || capacity > SIZE_MAX / sizeof(Task *)) {
        return TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    items = calloc(capacity, sizeof(*items));
    if (items == NULL) {
        return TASK_QUEUE_ERROR_ALLOCATION;
    }

    queue->items = items;
    queue->capacity = capacity;
    queue->size = 0U;
    queue->head = 0U;
    queue->tail = 0U;

    return TASK_QUEUE_OK;
}

void task_queue_destroy(TaskQueue *queue)
{
    if (queue == NULL) {
        return;
    }

    free(queue->items);
    queue->items = NULL;
    queue->capacity = 0U;
    queue->size = 0U;
    queue->head = 0U;
    queue->tail = 0U;
}

TaskQueueResult task_queue_enqueue(TaskQueue *queue, Task *task)
{
    if (queue == NULL
        || task == NULL
        || queue->items == NULL
        || queue->capacity == 0U
        || queue->size > queue->capacity
        || queue->head >= queue->capacity
        || queue->tail >= queue->capacity) {
        return TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    if (queue->size == queue->capacity) {
        return TASK_QUEUE_ERROR_FULL;
    }

    queue->items[queue->tail] = task;
    if (queue->tail == queue->capacity - 1U) {
        queue->tail = 0U;
    } else {
        ++queue->tail;
    }
    ++queue->size;

    return TASK_QUEUE_OK;
}

TaskQueueResult task_queue_dequeue(TaskQueue *queue, Task **task)
{
    Task *oldest;

    if (queue == NULL
        || task == NULL
        || queue->items == NULL
        || queue->capacity == 0U
        || queue->size > queue->capacity
        || queue->head >= queue->capacity
        || queue->tail >= queue->capacity) {
        return TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    if (queue->size == 0U) {
        return TASK_QUEUE_ERROR_EMPTY;
    }

    if (queue->items[queue->head] == NULL) {
        return TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    oldest = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    if (queue->head == queue->capacity - 1U) {
        queue->head = 0U;
    } else {
        ++queue->head;
    }
    --queue->size;
    *task = oldest;

    return TASK_QUEUE_OK;
}

TaskQueueResult task_queue_peek(const TaskQueue *queue, Task **task)
{
    if (queue == NULL
        || task == NULL
        || queue->items == NULL
        || queue->capacity == 0U
        || queue->size > queue->capacity
        || queue->head >= queue->capacity
        || queue->tail >= queue->capacity) {
        return TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    if (queue->size == 0U) {
        return TASK_QUEUE_ERROR_EMPTY;
    }

    if (queue->items[queue->head] == NULL) {
        return TASK_QUEUE_ERROR_INVALID_ARGUMENT;
    }

    *task = queue->items[queue->head];
    return TASK_QUEUE_OK;
}

bool task_queue_is_empty(const TaskQueue *queue)
{
    return queue == NULL || queue->size == 0U;
}

bool task_queue_is_full(const TaskQueue *queue)
{
    return queue != NULL
        && queue->capacity != 0U
        && queue->size <= queue->capacity
        && queue->size == queue->capacity;
}

size_t task_queue_size(const TaskQueue *queue)
{
    return queue == NULL ? 0U : queue->size;
}

size_t task_queue_capacity(const TaskQueue *queue)
{
    return queue == NULL ? 0U : queue->capacity;
}

const char *task_queue_result_name(TaskQueueResult result)
{
    switch (result) {
    case TASK_QUEUE_OK:
        return "OK";
    case TASK_QUEUE_ERROR_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case TASK_QUEUE_ERROR_ALLOCATION:
        return "ALLOCATION_ERROR";
    case TASK_QUEUE_ERROR_FULL:
        return "FULL";
    case TASK_QUEUE_ERROR_EMPTY:
        return "EMPTY";
    default:
        return "UNKNOWN";
    }
}
