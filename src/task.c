#include "concurrent_scheduler/task.h"

#include <stddef.h>

static bool task_transition_is_valid(TaskState current, TaskState next)
{
    switch (current) {
    case TASK_STATE_CREATED:
        return next == TASK_STATE_QUEUED
            || next == TASK_STATE_CANCELLED
            || next == TASK_STATE_REJECTED;
    case TASK_STATE_QUEUED:
        return next == TASK_STATE_RUNNING
            || next == TASK_STATE_CANCELLED;
    case TASK_STATE_RUNNING:
        return next == TASK_STATE_COMPLETED
            || next == TASK_STATE_FAILED;
    case TASK_STATE_COMPLETED:
    case TASK_STATE_FAILED:
    case TASK_STATE_CANCELLED:
    case TASK_STATE_REJECTED:
    default:
        return false;
    }
}

bool task_state_is_valid(TaskState state)
{
    switch (state) {
    case TASK_STATE_CREATED:
    case TASK_STATE_QUEUED:
    case TASK_STATE_RUNNING:
    case TASK_STATE_COMPLETED:
    case TASK_STATE_FAILED:
    case TASK_STATE_CANCELLED:
    case TASK_STATE_REJECTED:
        return true;
    default:
        return false;
    }
}

bool task_priority_is_valid(TaskPriority priority)
{
    switch (priority) {
    case TASK_PRIORITY_LOW:
    case TASK_PRIORITY_NORMAL:
    case TASK_PRIORITY_HIGH:
        return true;
    default:
        return false;
    }
}

bool task_init(
    Task *task,
    uint64_t id,
    TaskPriority priority,
    uint64_t total_work
)
{
    if (task == NULL
        || !task_priority_is_valid(priority)
        || total_work == UINT64_C(0)) {
        return false;
    }

    task->id = id;
    task->priority = priority;
    task->state = TASK_STATE_CREATED;
    task->total_work = total_work;
    task->remaining_work = total_work;

    return true;
}

bool task_is_complete(const Task *task)
{
    return task != NULL && task->remaining_work == UINT64_C(0);
}

bool task_consume_work(Task *task, uint64_t work_units)
{
    if (task == NULL
        || work_units == UINT64_C(0)
        || !task_state_is_valid(task->state)
        || task->state != TASK_STATE_RUNNING
        || task->remaining_work == UINT64_C(0)
        || work_units > task->remaining_work) {
        return false;
    }

    if (work_units == task->remaining_work) {
        if (!task_transition_state(task, TASK_STATE_COMPLETED)) {
            return false;
        }

        task->remaining_work = UINT64_C(0);
        return true;
    }

    task->remaining_work -= work_units;
    return true;
}

bool task_transition_state(Task *task, TaskState new_state)
{
    if (task == NULL
        || !task_state_is_valid(new_state)
        || !task_transition_is_valid(task->state, new_state)) {
        return false;
    }

    task->state = new_state;
    return true;
}

const char *task_state_name(TaskState state)
{
    switch (state) {
    case TASK_STATE_CREATED:
        return "CREATED";
    case TASK_STATE_QUEUED:
        return "QUEUED";
    case TASK_STATE_RUNNING:
        return "RUNNING";
    case TASK_STATE_COMPLETED:
        return "COMPLETED";
    case TASK_STATE_FAILED:
        return "FAILED";
    case TASK_STATE_CANCELLED:
        return "CANCELLED";
    case TASK_STATE_REJECTED:
        return "REJECTED";
    default:
        return "UNKNOWN";
    }
}

const char *task_priority_name(TaskPriority priority)
{
    switch (priority) {
    case TASK_PRIORITY_LOW:
        return "LOW";
    case TASK_PRIORITY_NORMAL:
        return "NORMAL";
    case TASK_PRIORITY_HIGH:
        return "HIGH";
    default:
        return "UNKNOWN";
    }
}
