#ifndef CONCURRENT_SCHEDULER_SCHEDULER_H
#define CONCURRENT_SCHEDULER_SCHEDULER_H

#include "concurrent_scheduler/task.h"

#include <stddef.h>

typedef enum {
    SCHEDULER_OK,
    SCHEDULER_ERROR_INVALID_ARGUMENT,
    SCHEDULER_ERROR_INVALID_STATE,
    SCHEDULER_ERROR_ALLOCATION,
    SCHEDULER_ERROR_QUEUE_FULL,
    SCHEDULER_ERROR_SHUTDOWN,
    SCHEDULER_ERROR_SYSTEM
} SchedulerResult;

/*
 * Worker threads call this function with the exact caller-owned
 * Task pointer and the optional shared context supplied to scheduler_init.
 * Zero reports task success and nonzero reports task failure. The scheduler
 * never owns either argument.
 */
typedef int (*SchedulerTaskExecuteFunction)(
    Task *task,
    void *context
);

/*
 * Caller-allocated opaque wrapper. Zero-initialize it before first use.
 * Lifecycle operations on one Scheduler require external serialization.
 * The normal lifecycle is init, start, submit, shutdown, join, then destroy.
 * Restart is unsupported. After destroy resets the wrapper, init begins a new
 * independent lifetime.
 */
typedef struct {
    void *implementation;
} Scheduler;

/*
 * Initializes private scheduler configuration and an empty bounded queue.
 * This does not create workers, invoke the callback, or accept Tasks.
 * execute must remain callable, and execute_context must remain valid whenever
 * workers may use them during this initialized scheduler lifetime.
 */
SchedulerResult scheduler_init(
    Scheduler *scheduler,
    size_t queue_capacity,
    size_t worker_count,
    SchedulerTaskExecuteFunction execute,
    void *execute_context
);

/*
 * Creates the configured fixed worker set and waits until every worker reports
 * ready. The scheduler must be initialized, and start may succeed only once
 * per initialized lifetime. This operation does not accept or execute Tasks
 * and does not invoke the callback. No worker handle is exposed.
 *
 * Start and all lifecycle operations require external serialization. A failed
 * start leaves the scheduler safely destructible.
 */
SchedulerResult scheduler_start(Scheduler *scheduler);

/*
 * Submits the exact caller-owned Task pointer to a running scheduler. Blocking
 * submit waits for queue capacity; try-submit returns QUEUE_FULL immediately.
 * Success means queue acceptance, not callback completion or success.
 *
 * The Task must remain valid until its callback finishes. The callback may run
 * before or after submission returns and may run concurrently with callbacks
 * on other workers. The caller owns synchronization for Task data, callback
 * behavior, and shared callback context. Neither operation transfers ownership.
 * Once graceful shutdown closes the gate, submission returns SHUTDOWN.
 */
SchedulerResult scheduler_submit(Scheduler *scheduler, Task *task);
SchedulerResult scheduler_try_submit(Scheduler *scheduler, Task *task);

/*
 * Gracefully closes submission and requests internal queue shutdown. Blocked
 * submitters are released, registered submit operations finish before return,
 * and accepted Tasks remain available for worker draining. This operation does
 * not join workers; callbacks may continue after it returns. Repeated shutdown
 * after a successful start is idempotent.
 */
SchedulerResult scheduler_shutdown(Scheduler *scheduler);

/*
 * Waits for and validates every worker after shutdown, destroys joined thread
 * handles, and releases worker arrays and contexts. It does not request
 * shutdown, destroy the queue, or reset the public wrapper. Repeated join after
 * successful completion is idempotent. No callback can run after OK is
 * returned. Do not invoke join from a scheduler callback.
 */
SchedulerResult scheduler_join(Scheduler *scheduler);

/*
 * Destroys only an initialized scheduler that was never started, a stopped
 * scheduler after join, or a safely cleaned failed scheduler. Running,
 * starting, and shutting-down schedulers are rejected; destroy never performs
 * implicit shutdown or join. Null is invalid, while an uninitialized or
 * already-destroyed wrapper is accepted.
 *
 * Lifecycle calls require external serialization.
 */
SchedulerResult scheduler_destroy(Scheduler *scheduler);

const char *scheduler_result_name(SchedulerResult result);

#endif
