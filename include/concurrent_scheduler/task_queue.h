#ifndef CONCURRENT_SCHEDULER_TASK_QUEUE_H
#define CONCURRENT_SCHEDULER_TASK_QUEUE_H

#include "concurrent_scheduler/task.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TASK_QUEUE_OK,
    TASK_QUEUE_ERROR_INVALID_ARGUMENT,
    TASK_QUEUE_ERROR_ALLOCATION,
    TASK_QUEUE_ERROR_FULL,
    TASK_QUEUE_ERROR_EMPTY
} TaskQueueResult;

/*
 * A TaskQueue is a fixed-capacity, non-thread-safe circular FIFO that stores
 * non-owning Task pointers. Callers must keep queued Task objects valid while
 * they remain in the queue. The queue owns only its internal pointer buffer;
 * destroying a queue must not free Task objects. Operations are non-blocking.
 */
typedef struct {
    Task **items;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
} TaskQueue;

/*
 * Initializes caller-provided queue storage with the requested nonzero
 * capacity. The implementation will allocate an internal Task pointer array,
 * and successful initialization will create an empty queue.
 */
TaskQueueResult task_queue_init(TaskQueue *queue, size_t capacity);

/*
 * Releases only the internal pointer array, never queued Task objects.
 * Accepts null and zero-initialized queues and is safe to repeat after a
 * successful initialization and destruction.
 */
void task_queue_destroy(TaskQueue *queue);

/*
 * Stores a non-null Task pointer without copying or taking ownership of the
 * Task. Returns TASK_QUEUE_ERROR_FULL when capacity is exhausted.
 */
TaskQueueResult task_queue_enqueue(TaskQueue *queue, Task *task);

/*
 * Returns and removes the oldest Task pointer in FIFO order. Returns
 * TASK_QUEUE_ERROR_EMPTY when no Task is queued.
 */
TaskQueueResult task_queue_dequeue(TaskQueue *queue, Task **task);

/*
 * Returns the oldest caller-owned Task pointer without removing it. Returns
 * TASK_QUEUE_ERROR_EMPTY when no Task is queued. The output pointer remains
 * unchanged on failure.
 */
TaskQueueResult task_queue_peek(const TaskQueue *queue, Task **task);

/*
 * Query helpers do not modify the queue. A null queue is empty, not full, and
 * has size and capacity zero. For non-null queues, size and capacity return
 * the stored values; empty tests size == 0, while full returns false for zero
 * capacity or size greater than capacity.
 */
bool task_queue_is_empty(const TaskQueue *queue);
bool task_queue_is_full(const TaskQueue *queue);
size_t task_queue_size(const TaskQueue *queue);
size_t task_queue_capacity(const TaskQueue *queue);

/*
 * Returns a stable readable result name. Invalid values return "UNKNOWN".
 */
const char *task_queue_result_name(TaskQueueResult result);

#endif
