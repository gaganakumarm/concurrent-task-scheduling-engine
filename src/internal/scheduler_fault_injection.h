#ifndef CONCURRENT_SCHEDULER_INTERNAL_FAULT_INJECTION_H
#define CONCURRENT_SCHEDULER_INTERNAL_FAULT_INJECTION_H

#if !defined(CONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION)
#error "Private fault injection is available only in enabled test builds."
#endif

#include "concurrent_scheduler/scheduler.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    SCHEDULER_FAULT_NONE = 0,
    SCHEDULER_FAULT_ALLOCATION,
    SCHEDULER_FAULT_WORKER_CREATION,
    SCHEDULER_FAULT_WORKER_STARTUP,
    SCHEDULER_FAULT_WORKER_JOIN
} SchedulerFaultPoint;

typedef struct {
    atomic_int point;
    _Atomic uint64_t trigger_occurrence;
    _Atomic uint64_t observed_occurrence;
    atomic_bool enabled;
    atomic_bool triggered;
    atomic_bool occurrence_overflow;
} SchedulerFaultPlan;

typedef struct {
    SchedulerFaultPoint point;
    uint64_t trigger_occurrence;
    uint64_t observed_occurrence;
    bool enabled;
    bool triggered;
    bool occurrence_overflow;
} SchedulerFaultPlanSnapshot;

void scheduler_fault_plan_init(SchedulerFaultPlan *plan);
bool scheduler_fault_plan_configure(
    SchedulerFaultPlan *plan,
    SchedulerFaultPoint point,
    uint64_t trigger_occurrence
);
void scheduler_fault_plan_reset(SchedulerFaultPlan *plan);
bool scheduler_fault_should_trigger(
    SchedulerFaultPlan *plan,
    SchedulerFaultPoint point
);
bool scheduler_fault_plan_capture(
    const SchedulerFaultPlan *plan,
    SchedulerFaultPlanSnapshot *snapshot
);
const char *scheduler_fault_point_name(SchedulerFaultPoint point);

bool scheduler_fault_configure(
    Scheduler *scheduler,
    SchedulerFaultPoint point,
    uint64_t trigger_occurrence
);
bool scheduler_fault_reset(Scheduler *scheduler);
bool scheduler_fault_capture_plan(
    Scheduler *scheduler,
    SchedulerFaultPlanSnapshot *snapshot
);

#endif
