#ifndef CONCURRENT_SCHEDULER_TASK_H
#define CONCURRENT_SCHEDULER_TASK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TASK_STATE_CREATED,
    TASK_STATE_QUEUED,
    TASK_STATE_RUNNING,
    TASK_STATE_COMPLETED,
    TASK_STATE_FAILED,
    TASK_STATE_CANCELLED,
    TASK_STATE_REJECTED
} TaskState;

typedef enum {
    TASK_PRIORITY_LOW,
    TASK_PRIORITY_NORMAL,
    TASK_PRIORITY_HIGH
} TaskPriority;

/*
 * Priority is Task metadata only. The current scheduler uses a bounded FIFO
 * and does not reorder or select Tasks according to this value.
 *
 * Task fields and Task API operations are not internally synchronized. A
 * caller must not concurrently read or modify one Task without its own
 * synchronization. After successful scheduler submission, the caller must
 * keep the Task alive and avoid unsynchronized access until callback
 * completion is established through caller-owned coordination or successful
 * scheduler_join.
 */
typedef struct {
    uint64_t id;
    TaskPriority priority;
    TaskState state;
    uint64_t total_work;
    uint64_t remaining_work;
} Task;

bool task_state_is_valid(TaskState state);
bool task_priority_is_valid(TaskPriority priority);

/*
 * Initializes caller-provided storage in CREATED state. total_work must be
 * nonzero. Failure leaves the Task unchanged.
 */
bool task_init(
    Task *task,
    uint64_t id,
    TaskPriority priority,
    uint64_t total_work
);

/*
 * Returns true exactly when a non-null Task has no remaining work. This query
 * does not validate or change the Task state.
 */
bool task_is_complete(const Task *task);

/*
 * Consumes a positive number of work units from a RUNNING Task.
 *
 * A partial consumption subtracts work_units from remaining_work and leaves
 * the state RUNNING. Consuming exactly the remaining amount sets
 * remaining_work to zero and transitions the Task to COMPLETED. total_work is
 * never changed.
 *
 * The operation returns false for a null Task, zero work_units, an invalid or
 * non-RUNNING state, an already complete Task, or work_units greater than
 * remaining_work. It never clamps over-consumption. Every failure leaves all
 * Task fields unchanged.
 */
bool task_consume_work(Task *task, uint64_t work_units);

/*
 * Permitted state transitions are:
 *
 *   CREATED -> QUEUED
 *   CREATED -> CANCELLED
 *   CREATED -> REJECTED
 *   QUEUED  -> RUNNING
 *   QUEUED  -> CANCELLED
 *   RUNNING -> COMPLETED
 *   RUNNING -> FAILED
 *
 * COMPLETED, FAILED, CANCELLED, and REJECTED are terminal. Self-transitions
 * are not permitted. A null Task, invalid target state, or disallowed
 * transition returns false and leaves the Task unchanged.
 */
bool task_transition_state(Task *task, TaskState new_state);

/* The name functions return "UNKNOWN" for values outside their enum domain. */
const char *task_state_name(TaskState state);
const char *task_priority_name(TaskPriority priority);

#endif
