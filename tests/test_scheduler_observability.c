#include "concurrent_scheduler/scheduler.h"
#include "internal/scheduler_observability.h"
#include "platform/sync.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    Task *blocking_task;
    Task *failure_task;
    int blocking_started;
    int release_blocking;
} ObservabilityCallbackContext;

static int observed_callback(Task *task, void *context)
{
    ObservabilityCallbackContext *callback_context = context;
    int result;

    if (callback_context == NULL
        || sched_mutex_lock(&callback_context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    if (task == callback_context->blocking_task) {
        callback_context->blocking_started = 1;
        if (sched_condition_broadcast(&callback_context->condition)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&callback_context->mutex);
            return -1;
        }
        while (!callback_context->release_blocking) {
            if (sched_condition_wait(
                    &callback_context->condition,
                    &callback_context->mutex
                ) != SCHED_SYNC_OK) {
                (void)sched_mutex_unlock(&callback_context->mutex);
                return -1;
            }
        }
    }
    result = task == callback_context->failure_task ? 23 : 0;
    if (sched_mutex_unlock(&callback_context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    return result;
}

static int wait_for_blocking_callback(
    ObservabilityCallbackContext *context
)
{
    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    while (!context->blocking_started) {
        if (sched_condition_wait(&context->condition, &context->mutex)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&context->mutex);
            return EXIT_FAILURE;
        }
    }
    return sched_mutex_unlock(&context->mutex) == SCHED_SYNC_OK
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

static int release_blocking_callback(
    ObservabilityCallbackContext *context
)
{
    int result = EXIT_SUCCESS;

    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    context->release_blocking = 1;
    if (sched_condition_broadcast(&context->condition) != SCHED_SYNC_OK) {
        result = EXIT_FAILURE;
    }
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        result = EXIT_FAILURE;
    }
    return result;
}

static int test_checked_counter(void)
{
    uint64_t counter = UINT64_MAX - UINT64_C(1);
    bool overflow = false;
    _Atomic uint64_t atomic_counter = UINT64_MAX - UINT64_C(1);
    atomic_bool atomic_overflow = false;

    scheduler_observability_increment_saturating(&counter, &overflow);
    if (counter != UINT64_MAX || overflow) {
        return EXIT_FAILURE;
    }
    scheduler_observability_increment_saturating(&counter, &overflow);
    if (counter != UINT64_MAX || !overflow) {
        return EXIT_FAILURE;
    }
    scheduler_observability_increment_saturating(&counter, &overflow);
    if (counter != UINT64_MAX || !overflow
        || !scheduler_observability_increment_single_writer_atomic(
            &atomic_counter,
            &atomic_overflow
        )
        || atomic_load(&atomic_counter) != UINT64_MAX
        || atomic_load(&atomic_overflow)
        || scheduler_observability_increment_single_writer_atomic(
            &atomic_counter,
            &atomic_overflow
        )
        || atomic_load(&atomic_counter) != UINT64_MAX
        || !atomic_load(&atomic_overflow)
        || scheduler_observability_increment_single_writer_atomic(
            &atomic_counter,
            &atomic_overflow
        )
        || atomic_load(&atomic_counter) != UINT64_MAX
        || !atomic_load(&atomic_overflow)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int test_snapshot_lifecycle(void)
{
    Scheduler scheduler = {0};
    SchedulerSnapshot snapshot = {0};
    SchedulerSnapshot preserved = {0};
    ObservabilityCallbackContext context = {0};
    Task tasks[4] = {0};
    size_t index;
    int context_initialized = 0;
    int scheduler_initialized = 0;
    int result = EXIT_FAILURE;

    preserved.version = UINT32_C(777);
    snapshot = preserved;
    if (scheduler_capture_snapshot(NULL, &snapshot)
        || snapshot.version != preserved.version
        || scheduler_capture_snapshot(&scheduler, &snapshot)
        || scheduler_capture_snapshot(&scheduler, NULL)) {
        return EXIT_FAILURE;
    }
    if (sched_mutex_init(&context.mutex) != SCHED_SYNC_OK
        || sched_condition_init(&context.condition) != SCHED_SYNC_OK) {
        sched_mutex_destroy(&context.mutex);
        return EXIT_FAILURE;
    }
    context_initialized = 1;
    for (index = 0U; index < 4U; ++index) {
        if (!task_init(
                &tasks[index],
                (uint64_t)(index + 1U),
                TASK_PRIORITY_NORMAL,
                1U
            )) {
            goto cleanup;
        }
    }
    context.blocking_task = &tasks[0];
    context.failure_task = &tasks[2];
    if (scheduler_init(&scheduler, 2U, 1U, observed_callback, &context)
        != SCHEDULER_OK) {
        goto cleanup;
    }
    scheduler_initialized = 1;
    if (!scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.version != CONCURRENT_SCHEDULER_SNAPSHOT_VERSION
        || snapshot.consistency
            != SCHEDULER_SNAPSHOT_CONSISTENCY_DOMAIN_EXACT
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_INITIALIZED
        || snapshot.submissions_open
        || snapshot.shutdown_started
        || snapshot.configured_worker_count != 1U
        || snapshot.created_worker_count != 0U
        || snapshot.queue_capacity != 2U
        || snapshot.queue_current_size != 0U
        || snapshot.queue_high_water_mark != 0U
        || !scheduler_snapshot_validate_basic(&snapshot)
        || !scheduler_snapshot_validate_quiescent(&snapshot)) {
        goto cleanup;
    }
    if (scheduler_start(&scheduler) != SCHEDULER_OK
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_RUNNING
        || !snapshot.submissions_open
        || snapshot.created_worker_count != 1U
        || snapshot.ready_worker_count != 1U
        || snapshot.active_worker_count != 1U) {
        goto cleanup;
    }
    if (scheduler_submit(&scheduler, &tasks[0]) != SCHEDULER_OK
        || wait_for_blocking_callback(&context) != EXIT_SUCCESS
        || scheduler_submit(&scheduler, &tasks[1]) != SCHEDULER_OK
        || scheduler_submit(&scheduler, &tasks[2]) != SCHEDULER_OK
        || scheduler_try_submit(&scheduler, &tasks[3])
            != SCHEDULER_ERROR_QUEUE_FULL
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.submitted_count != 4U
        || snapshot.accepted_count != 3U
        || snapshot.rejected_count != 1U
        || snapshot.dequeued_count != 1U
        || snapshot.callback_started_count != 1U
        || snapshot.currently_running_count != 1U
        || snapshot.callback_succeeded_count != 0U
        || snapshot.callback_failed_count != 0U
        || snapshot.queue_current_size != 2U
        || snapshot.queue_high_water_mark != 2U
        || !scheduler_snapshot_validate_basic(&snapshot)) {
        goto cleanup;
    }
    if (scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_SHUTTING_DOWN
        || snapshot.submissions_open
        || !snapshot.shutdown_started
        || snapshot.active_worker_count != 1U
        || snapshot.currently_running_count != 1U
        || snapshot.queue_current_size != 2U
        || scheduler_try_submit(&scheduler, &tasks[3])
            != SCHEDULER_ERROR_SHUTDOWN) {
        goto cleanup;
    }
    if (release_blocking_callback(&context) != EXIT_SUCCESS
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.state != SCHEDULER_SNAPSHOT_STATE_STOPPED
        || snapshot.submitted_count != 5U
        || snapshot.accepted_count != 3U
        || snapshot.rejected_count != 2U
        || snapshot.dequeued_count != 3U
        || snapshot.callback_started_count != 3U
        || snapshot.callback_succeeded_count != 2U
        || snapshot.callback_failed_count != 1U
        || snapshot.currently_running_count != 0U
        || snapshot.active_worker_count != 0U
        || snapshot.joined_worker_count != 1U
        || snapshot.created_worker_count != 1U
        || snapshot.queue_current_size != 0U
        || snapshot.queue_high_water_mark != 2U
        || snapshot.overflow_detected
        || !scheduler_snapshot_validate_quiescent(&snapshot)) {
        goto cleanup;
    }
    if (scheduler_join(&scheduler) != SCHEDULER_OK
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.joined_worker_count != 1U) {
        goto cleanup;
    }
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    scheduler_initialized = 0;
    snapshot.version = UINT32_C(888);
    if (scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.version != UINT32_C(888)) {
        goto cleanup;
    }
    if (scheduler_init(&scheduler, 1U, 1U, observed_callback, &context)
            != SCHEDULER_OK
        || !scheduler_capture_snapshot(&scheduler, &snapshot)
        || snapshot.submitted_count != 0U
        || snapshot.created_worker_count != 0U
        || snapshot.queue_high_water_mark != 0U
        || scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    scheduler_initialized = 0;
    result = EXIT_SUCCESS;

cleanup:
    (void)release_blocking_callback(&context);
    if (scheduler_initialized) {
        (void)scheduler_shutdown(&scheduler);
        (void)scheduler_join(&scheduler);
        (void)scheduler_destroy(&scheduler);
    }
    if (context_initialized) {
        sched_condition_destroy(&context.condition);
        sched_mutex_destroy(&context.mutex);
    }
    return result;
}

int main(void)
{
    if (test_checked_counter() != EXIT_SUCCESS
        || test_snapshot_lifecycle() != EXIT_SUCCESS) {
        fprintf(stderr, "Scheduler observability tests failed.\n");
        return EXIT_FAILURE;
    }
    printf(
        "OBSERVABILITY-001 through OBSERVABILITY-020 passed.\n"
    );
    return EXIT_SUCCESS;
}
