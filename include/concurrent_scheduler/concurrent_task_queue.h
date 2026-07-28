#ifndef CONCURRENT_SCHEDULER_CONCURRENT_TASK_QUEUE_H
#define CONCURRENT_SCHEDULER_CONCURRENT_TASK_QUEUE_H

#include "concurrent_scheduler/task_queue.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CONCURRENT_TASK_QUEUE_OK,
    CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT,
    CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION,
    CONCURRENT_TASK_QUEUE_ERROR_SYSTEM,
    CONCURRENT_TASK_QUEUE_ERROR_FULL,
    CONCURRENT_TASK_QUEUE_ERROR_EMPTY,
    CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN
} ConcurrentTaskQueueResult;

/*
 * The caller allocates the wrapper. It owns its embedded TaskQueue buffer and
 * private synchronization implementation, but queued Task objects remain
 * caller-owned. Native synchronization details are private.
 */
typedef struct {
    TaskQueue queue;
    void *implementation;
} ConcurrentTaskQueue;

ConcurrentTaskQueueResult concurrent_task_queue_init(
    ConcurrentTaskQueue *queue,
    size_t capacity
);

/*
 * Releases the private synchronization implementation and TaskQueue buffer,
 * but never queued Task objects. Accepts null and zero-initialized wrappers
 * and is safe to repeat after a successful initialization and destruction.
 * Destruction must not race with another user of the wrapper.
 */
void concurrent_task_queue_destroy(ConcurrentTaskQueue *queue);

/*
 * Attempts one insertion while holding the private mutex and never waits for
 * capacity. The exact caller-owned Task pointer is stored. Successful
 * insertion signals the not-empty condition before unlocking.
 *
 * A signaling or unlocking failure after insertion returns SYSTEM_ERROR but
 * does not roll back the inserted Task. An unlocking failure takes precedence
 * over an underlying enqueue failure. No Task ownership is transferred.
 */
ConcurrentTaskQueueResult concurrent_task_queue_try_enqueue(
    ConcurrentTaskQueue *queue,
    Task *task
);

/*
 * Waits indefinitely while the queue is full, then stores the exact
 * caller-owned Task pointer. The caller must keep both the Task and wrapper
 * valid until this call returns.
 *
 * There is no timeout, cancellation, or shutdown path. Destroying the wrapper
 * while this or another operation is blocked or active is invalid. A signaling
 * or unlocking failure after insertion returns SYSTEM_ERROR without rolling
 * back the stored pointer.
 */
ConcurrentTaskQueueResult concurrent_task_queue_enqueue(
    ConcurrentTaskQueue *queue,
    Task *task
);

/*
 * Attempts one removal while holding the private mutex and never waits for an
 * item. On pre-removal failure, the caller's output remains unchanged.
 *
 * Successful removal signals the not-full condition. If signaling or
 * unlocking then fails, the removed pointer is still published and
 * SYSTEM_ERROR is returned because the completed removal is not rolled back.
 * The returned Task remains caller-owned.
 */
ConcurrentTaskQueueResult concurrent_task_queue_try_dequeue(
    ConcurrentTaskQueue *queue,
    Task **task
);

/*
 * Waits indefinitely while the queue is empty, then removes its oldest
 * caller-owned Task pointer. The caller must keep the wrapper and output
 * storage valid until this call returns.
 *
 * There is no timeout, cancellation, or shutdown path. Destroying the wrapper
 * while this or another operation is blocked or active is invalid. Failure
 * before removal preserves the output. A signaling or unlocking failure after
 * removal returns SYSTEM_ERROR while still publishing the removed pointer.
 */
ConcurrentTaskQueueResult concurrent_task_queue_dequeue(
    ConcurrentTaskQueue *queue,
    Task **task
);

/*
 * Observes the oldest pointer while holding the private mutex without
 * removing it or signaling either condition. On failure before a successful
 * observation, the caller's output remains unchanged.
 *
 * If unlocking fails after a successful observation, the pointer is still
 * published and SYSTEM_ERROR is returned. The Task remains caller-owned.
 */
ConcurrentTaskQueueResult concurrent_task_queue_try_peek(
    ConcurrentTaskQueue *queue,
    Task **task
);

/*
 * These helpers are logically read-only, but acquire the private mutex.
 * Null and uninitialized wrappers return safe defaults. A locking failure
 * also returns the corresponding safe default; an unlocking failure returns
 * the logical value already read. Operation APIs report synchronization
 * failures as SYSTEM_ERROR.
 */
bool concurrent_task_queue_is_empty(ConcurrentTaskQueue *queue);
bool concurrent_task_queue_is_full(ConcurrentTaskQueue *queue);
size_t concurrent_task_queue_size(ConcurrentTaskQueue *queue);
size_t concurrent_task_queue_capacity(ConcurrentTaskQueue *queue);

const char *concurrent_task_queue_result_name(
    ConcurrentTaskQueueResult result
);

#endif
