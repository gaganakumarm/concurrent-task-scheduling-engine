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

typedef struct {
    uint64_t id;
    TaskPriority priority;
    TaskState state;
    uint64_t total_work;
    uint64_t remaining_work;
} Task;

bool task_state_is_valid(TaskState state);
bool task_priority_is_valid(TaskPriority priority);
bool task_init(
    Task *task,
    uint64_t id,
    TaskPriority priority,
    uint64_t total_work
);
bool task_is_complete(const Task *task);
bool task_consume_work(Task *task, uint64_t work_units);
bool task_transition_state(Task *task, TaskState new_state);

/* The name functions return "UNKNOWN" for values outside their enum domain. */
const char *task_state_name(TaskState state);
const char *task_priority_name(TaskPriority priority);

#endif
