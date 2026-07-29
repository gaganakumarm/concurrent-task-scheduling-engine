#include "internal/scheduler_fault_injection.h"

#include <limits.h>

static bool scheduler_fault_point_supported(SchedulerFaultPoint point)
{
    return point == SCHEDULER_FAULT_ALLOCATION
        || point == SCHEDULER_FAULT_WORKER_CREATION
        || point == SCHEDULER_FAULT_WORKER_JOIN;
}

void scheduler_fault_plan_init(SchedulerFaultPlan *plan)
{
    if (plan == NULL) {
        return;
    }
    atomic_init(&plan->point, SCHEDULER_FAULT_NONE);
    atomic_init(&plan->trigger_occurrence, UINT64_C(0));
    atomic_init(&plan->observed_occurrence, UINT64_C(0));
    atomic_init(&plan->enabled, false);
    atomic_init(&plan->triggered, false);
    atomic_init(&plan->occurrence_overflow, false);
}

bool scheduler_fault_plan_configure(
    SchedulerFaultPlan *plan,
    SchedulerFaultPoint point,
    uint64_t trigger_occurrence
)
{
    if (plan == NULL || !scheduler_fault_point_supported(point)
        || trigger_occurrence == UINT64_C(0)) {
        return false;
    }
    atomic_store(&plan->enabled, false);
    atomic_store(&plan->point, (int)point);
    atomic_store(&plan->trigger_occurrence, trigger_occurrence);
    atomic_store(&plan->observed_occurrence, UINT64_C(0));
    atomic_store(&plan->triggered, false);
    atomic_store(&plan->occurrence_overflow, false);
    atomic_store(&plan->enabled, true);
    return true;
}

void scheduler_fault_plan_reset(SchedulerFaultPlan *plan)
{
    if (plan == NULL) {
        return;
    }
    atomic_store(&plan->enabled, false);
    atomic_store(&plan->point, SCHEDULER_FAULT_NONE);
    atomic_store(&plan->trigger_occurrence, UINT64_C(0));
    atomic_store(&plan->observed_occurrence, UINT64_C(0));
    atomic_store(&plan->triggered, false);
    atomic_store(&plan->occurrence_overflow, false);
}

bool scheduler_fault_should_trigger(
    SchedulerFaultPlan *plan,
    SchedulerFaultPoint point
)
{
    uint64_t observed;
    uint64_t trigger;

    if (plan == NULL || !atomic_load(&plan->enabled)
        || atomic_load(&plan->point) != (int)point
        || atomic_load(&plan->triggered)) {
        return false;
    }
    observed = atomic_load(&plan->observed_occurrence);
    if (observed == UINT64_MAX) {
        atomic_store(&plan->occurrence_overflow, true);
        return false;
    }
    observed++;
    atomic_store(&plan->observed_occurrence, observed);
    trigger = atomic_load(&plan->trigger_occurrence);
    if (observed == trigger) {
        atomic_store(&plan->triggered, true);
        return true;
    }
    return false;
}

bool scheduler_fault_plan_capture(
    const SchedulerFaultPlan *plan,
    SchedulerFaultPlanSnapshot *snapshot
)
{
    SchedulerFaultPlanSnapshot temporary;

    if (plan == NULL || snapshot == NULL) {
        return false;
    }
    temporary.point =
        (SchedulerFaultPoint)atomic_load(&plan->point);
    temporary.trigger_occurrence =
        atomic_load(&plan->trigger_occurrence);
    temporary.observed_occurrence =
        atomic_load(&plan->observed_occurrence);
    temporary.enabled = atomic_load(&plan->enabled);
    temporary.triggered = atomic_load(&plan->triggered);
    temporary.occurrence_overflow =
        atomic_load(&plan->occurrence_overflow);
    *snapshot = temporary;
    return true;
}

const char *scheduler_fault_point_name(SchedulerFaultPoint point)
{
    switch (point) {
    case SCHEDULER_FAULT_NONE:
        return "NONE";
    case SCHEDULER_FAULT_ALLOCATION:
        return "ALLOCATION";
    case SCHEDULER_FAULT_WORKER_CREATION:
        return "WORKER_CREATION";
    case SCHEDULER_FAULT_WORKER_STARTUP:
        return "WORKER_STARTUP";
    case SCHEDULER_FAULT_WORKER_JOIN:
        return "WORKER_JOIN";
    default:
        return "UNKNOWN";
    }
}
