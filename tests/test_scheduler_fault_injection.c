#include "concurrent_scheduler/scheduler.h"
#include "internal/scheduler_fault_injection.h"
#include "internal/scheduler_observability.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int no_op_callback(Task *task, void *context)
{
    (void)task;
    (void)context;
    return 0;
}

static bool validation_has_issue(
    const SchedulerValidationResult *validation,
    SchedulerValidationIssue issue
)
{
    size_t index;

    for (index = 0U; index < validation->issue_count; ++index) {
        if (validation->issues[index].issue == issue) {
            return true;
        }
    }
    return false;
}

static int test_fault_plan(void)
{
    SchedulerFaultPlan plan;
    SchedulerFaultPlanSnapshot snapshot = {0};

    scheduler_fault_plan_init(&plan);
    if (!scheduler_fault_plan_capture(&plan, &snapshot)
        || snapshot.enabled
        || snapshot.point != SCHEDULER_FAULT_NONE
        || scheduler_fault_plan_configure(NULL,
            SCHEDULER_FAULT_WORKER_CREATION, UINT64_C(1))
        || scheduler_fault_plan_configure(&plan,
            SCHEDULER_FAULT_NONE, UINT64_C(1))
        || scheduler_fault_plan_configure(&plan,
            SCHEDULER_FAULT_WORKER_STARTUP, UINT64_C(1))
        || scheduler_fault_plan_configure(&plan,
            SCHEDULER_FAULT_WORKER_CREATION, UINT64_C(0))
        || !scheduler_fault_plan_configure(&plan,
            SCHEDULER_FAULT_WORKER_CREATION, UINT64_C(3))) {
        return EXIT_FAILURE;
    }
    if (scheduler_fault_should_trigger(&plan, SCHEDULER_FAULT_ALLOCATION)
        || scheduler_fault_should_trigger(
            &plan, SCHEDULER_FAULT_WORKER_CREATION)
        || scheduler_fault_should_trigger(
            &plan, SCHEDULER_FAULT_WORKER_CREATION)
        || !scheduler_fault_should_trigger(
            &plan, SCHEDULER_FAULT_WORKER_CREATION)
        || scheduler_fault_should_trigger(
            &plan, SCHEDULER_FAULT_WORKER_CREATION)
        || !scheduler_fault_plan_capture(&plan, &snapshot)
        || snapshot.observed_occurrence != UINT64_C(3)
        || !snapshot.triggered
        || strcmp(scheduler_fault_point_name(snapshot.point),
            "WORKER_CREATION") != 0) {
        return EXIT_FAILURE;
    }
    scheduler_fault_plan_reset(&plan);
    if (!scheduler_fault_plan_capture(&plan, &snapshot)
        || snapshot.enabled || snapshot.triggered
        || snapshot.observed_occurrence != UINT64_C(0)
        || !scheduler_fault_plan_configure(
            &plan, SCHEDULER_FAULT_ALLOCATION, UINT64_MAX)) {
        return EXIT_FAILURE;
    }
    atomic_store(&plan.observed_occurrence, UINT64_MAX);
    if (scheduler_fault_should_trigger(&plan, SCHEDULER_FAULT_ALLOCATION)
        || !scheduler_fault_plan_capture(&plan, &snapshot)
        || !snapshot.occurrence_overflow
        || snapshot.observed_occurrence != UINT64_MAX) {
        return EXIT_FAILURE;
    }
    if (!scheduler_fault_plan_configure(
            &plan, SCHEDULER_FAULT_WORKER_JOIN, UINT64_C(1))
        || !scheduler_fault_should_trigger(
            &plan, SCHEDULER_FAULT_WORKER_JOIN)
        || scheduler_fault_should_trigger(
            &plan, SCHEDULER_FAULT_WORKER_JOIN)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int test_worker_creation_failure(uint64_t occurrence)
{
    Scheduler scheduler = {0};
    SchedulerSnapshot snapshot = {0};
    SchedulerValidationResult validation = {0};
    SchedulerFaultPlanSnapshot plan = {0};
    SchedulerHealth health;
    size_t expected_created = (size_t)(occurrence - UINT64_C(1));
    int result = EXIT_FAILURE;

    if (scheduler_init(&scheduler, 4U, 3U, no_op_callback, NULL)
            != SCHEDULER_OK
        || !scheduler_fault_configure(
            &scheduler,
            SCHEDULER_FAULT_WORKER_CREATION,
            occurrence
        )
        || scheduler_start(&scheduler) != SCHEDULER_ERROR_SYSTEM
        || !scheduler_fault_capture_plan(&scheduler, &plan)
        || !plan.triggered
        || plan.observed_occurrence != occurrence
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_FAILED
        || snapshot.created_worker_count != expected_created
        || snapshot.joined_worker_count != expected_created
        || snapshot.active_worker_count != 0U
        || snapshot.submitted_count != UINT64_C(0)
        || snapshot.worker_startup_failure_count != UINT64_C(1)
        || !scheduler_snapshot_validate(
            &snapshot, SCHEDULER_VALIDATION_QUIESCENT, &validation)
        || validation.violation_count != 0U
        || validation.validation_incomplete
        || !scheduler_snapshot_derive_health(
            &snapshot, &validation, &health)
        || health != SCHEDULER_HEALTH_FAILED) {
        goto cleanup;
    }
    if (scheduler_join(&scheduler) != SCHEDULER_OK
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_STOPPED
        || snapshot.created_worker_count != expected_created
        || snapshot.joined_worker_count != expected_created
        || snapshot.active_worker_count != 0U
        || !scheduler_snapshot_validate(
            &snapshot, SCHEDULER_VALIDATION_QUIESCENT, &validation)
        || validation.violation_count != 0U
        || validation.validation_incomplete
        || !scheduler_snapshot_derive_health(
            &snapshot, &validation, &health)
        || health != SCHEDULER_HEALTH_STOPPED
        || scheduler_join(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        result = EXIT_FAILURE;
    }
    return result;
}

static int test_failure_recovery_lifecycle(void)
{
    Scheduler scheduler = {0};
    SchedulerSnapshot snapshot = {0};
    SchedulerValidationResult validation = {0};
    SchedulerHealth health;
    int initialized = 0;
    int result = EXIT_FAILURE;

    if (scheduler_init(&scheduler, 2U, 2U, no_op_callback, NULL)
        != SCHEDULER_OK) {
        return EXIT_FAILURE;
    }
    initialized = 1;
    if (scheduler_init(&scheduler, 2U, 2U, no_op_callback, NULL)
            != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_shutdown(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_join(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || !scheduler_fault_configure(
            &scheduler, SCHEDULER_FAULT_ALLOCATION, UINT64_C(2))
        || scheduler_start(&scheduler) != SCHEDULER_ERROR_ALLOCATION
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_INITIALIZED
        || snapshot.created_worker_count != 0U
        || snapshot.active_worker_count != 0U
        || !scheduler_snapshot_validate(
            &snapshot, SCHEDULER_VALIDATION_QUIESCENT, &validation)
        || validation.violation_count != 0U
        || validation.validation_incomplete
        || !scheduler_snapshot_derive_health(
            &snapshot, &validation, &health)
        || health != SCHEDULER_HEALTH_HEALTHY
        || !scheduler_fault_reset(&scheduler)
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_destroy(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_STOPPED
        || snapshot.active_worker_count != 0U
        || snapshot.joined_worker_count != snapshot.created_worker_count
        || !scheduler_snapshot_validate(
            &snapshot, SCHEDULER_VALIDATION_QUIESCENT, &validation)
        || validation.violation_count != 0U
        || !scheduler_snapshot_derive_health(
            &snapshot, &validation, &health)
        || health != SCHEDULER_HEALTH_STOPPED
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_ERROR_INVALID_STATE) {
        goto cleanup;
    }
    initialized = 0;
    if (scheduler_init(&scheduler, 1U, 1U, no_op_callback, NULL)
            != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (initialized) {
        (void)scheduler_shutdown(&scheduler);
        (void)scheduler_join(&scheduler);
        (void)scheduler_destroy(&scheduler);
    } else {
        (void)scheduler_destroy(&scheduler);
    }
    return result;
}

static int test_allocation_failure(uint64_t occurrence)
{
    Scheduler scheduler = {0};
    SchedulerSnapshot snapshot = {0};
    SchedulerFaultPlanSnapshot plan = {0};
    int result = EXIT_FAILURE;

    if (scheduler_init(&scheduler, 2U, 2U, no_op_callback, NULL)
            != SCHEDULER_OK
        || !scheduler_fault_configure(
            &scheduler, SCHEDULER_FAULT_ALLOCATION, occurrence)
        || scheduler_start(&scheduler) != SCHEDULER_ERROR_ALLOCATION
        || !scheduler_fault_capture_plan(&scheduler, &plan)
        || !plan.triggered
        || plan.observed_occurrence != occurrence
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_INITIALIZED
        || snapshot.created_worker_count != 0U
        || snapshot.active_worker_count != 0U
        || snapshot.worker_startup_failure_count != UINT64_C(0)) {
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        result = EXIT_FAILURE;
    }
    return result;
}

static int test_safe_join_failure(void)
{
    Scheduler scheduler = {0};
    SchedulerSnapshot snapshot = {0};
    SchedulerValidationResult validation = {0};
    SchedulerHealth health;
    char diagnostic[512];
    int result = EXIT_FAILURE;

    if (scheduler_init(&scheduler, 2U, 2U, no_op_callback, NULL)
            != SCHEDULER_OK
        || !scheduler_fault_configure(
            &scheduler, SCHEDULER_FAULT_WORKER_JOIN, UINT64_C(1))
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_ERROR_SYSTEM
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_STOPPED
        || snapshot.active_worker_count != 0U
        || snapshot.created_worker_count != 2U
        || snapshot.joined_worker_count != 1U
        || snapshot.join_failure_count != UINT64_C(1)
        || !scheduler_snapshot_validate(
            &snapshot, SCHEDULER_VALIDATION_QUIESCENT, &validation)
        || validation.violation_count == 0U
        || !validation_has_issue(
            &validation,
            SCHEDULER_VALIDATION_ISSUE_STOPPED_WORKERS_NOT_JOINED
        )
        || scheduler_validation_format(
            &snapshot, &validation, diagnostic, sizeof(diagnostic)) < 0
        || strstr(diagnostic, "STOPPED_WORKERS_NOT_JOINED") == NULL
        || !scheduler_snapshot_derive_health(
            &snapshot, &validation, &health)
        || health != SCHEDULER_HEALTH_FAILED
        || scheduler_join(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        result = EXIT_FAILURE;
    }
    return result;
}

int main(void)
{
    if (test_fault_plan() != EXIT_SUCCESS
        || test_worker_creation_failure(UINT64_C(1)) != EXIT_SUCCESS
        || test_worker_creation_failure(UINT64_C(2)) != EXIT_SUCCESS
        || test_worker_creation_failure(UINT64_C(3)) != EXIT_SUCCESS
        || test_allocation_failure(UINT64_C(1)) != EXIT_SUCCESS
        || test_allocation_failure(UINT64_C(2)) != EXIT_SUCCESS
        || test_safe_join_failure() != EXIT_SUCCESS
        || test_failure_recovery_lifecycle() != EXIT_SUCCESS) {
        fputs("Scheduler fault-injection tests failed.\n", stderr);
        return EXIT_FAILURE;
    }
    puts("FAULT-INJECTION-001 through FAULT-INJECTION-050 passed.");
    return EXIT_SUCCESS;
}
