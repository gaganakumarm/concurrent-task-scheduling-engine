#ifndef CONCURRENT_SCHEDULER_INTERNAL_OBSERVABILITY_H
#define CONCURRENT_SCHEDULER_INTERNAL_OBSERVABILITY_H

#include "concurrent_scheduler/concurrent_task_queue.h"
#include "concurrent_scheduler/scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define CONCURRENT_SCHEDULER_SNAPSHOT_VERSION UINT32_C(1)
#define SCHEDULER_VALIDATION_ISSUE_CAPACITY 16U

typedef enum {
    SCHEDULER_SNAPSHOT_CONSISTENCY_DOMAIN_EXACT = 1
} SchedulerSnapshotConsistency;

typedef enum {
    SCHEDULER_SNAPSHOT_STATE_INITIALIZED,
    SCHEDULER_SNAPSHOT_STATE_STARTING,
    SCHEDULER_SNAPSHOT_STATE_RUNNING,
    SCHEDULER_SNAPSHOT_STATE_SHUTTING_DOWN,
    SCHEDULER_SNAPSHOT_STATE_STOPPED,
    SCHEDULER_SNAPSHOT_STATE_FAILED
} SchedulerSnapshotState;

typedef struct {
    size_t capacity;
    size_t current_size;
    size_t high_water_mark;
} ConcurrentTaskQueueRuntimeSnapshot;

typedef struct {
    uint32_t version;
    SchedulerSnapshotConsistency consistency;
    bool overflow_detected;

    SchedulerSnapshotState state;
    bool submissions_open;
    bool shutdown_started;
    size_t configured_worker_count;

    uint64_t submitted_count;
    uint64_t accepted_count;
    uint64_t rejected_count;

    size_t created_worker_count;
    size_t ready_worker_count;
    size_t active_worker_count;
    size_t joined_worker_count;

    uint64_t dequeued_count;
    uint64_t callback_started_count;
    uint64_t callback_succeeded_count;
    uint64_t callback_failed_count;
    uint64_t currently_running_count;

    size_t queue_capacity;
    size_t queue_current_size;
    size_t queue_high_water_mark;

    uint64_t worker_startup_failure_count;
    uint64_t worker_runtime_failure_count;
    uint64_t join_failure_count;
} SchedulerSnapshot;

typedef enum {
    SCHEDULER_VALIDATION_LIVE = 1,
    SCHEDULER_VALIDATION_QUIESCENT = 2
} SchedulerValidationMode;

typedef enum {
    SCHEDULER_VALIDATION_SEVERITY_INFORMATIONAL = 1,
    SCHEDULER_VALIDATION_SEVERITY_ADVISORY,
    SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
    SCHEDULER_VALIDATION_SEVERITY_INCOMPLETE
} SchedulerValidationSeverity;

typedef enum {
    SCHEDULER_VALIDATION_ISSUE_SNAPSHOT_VERSION_UNSUPPORTED = 1,
    SCHEDULER_VALIDATION_ISSUE_CONSISTENCY_UNSUPPORTED,
    SCHEDULER_VALIDATION_ISSUE_ACCOUNTING_OVERFLOW,
    SCHEDULER_VALIDATION_ISSUE_QUEUE_SIZE_EXCEEDS_CAPACITY,
    SCHEDULER_VALIDATION_ISSUE_QUEUE_HIGH_WATER_EXCEEDS_CAPACITY,
    SCHEDULER_VALIDATION_ISSUE_QUEUE_SIZE_EXCEEDS_HIGH_WATER,
    SCHEDULER_VALIDATION_ISSUE_CREATED_EXCEEDS_CONFIGURED,
    SCHEDULER_VALIDATION_ISSUE_READY_EXCEEDS_CREATED,
    SCHEDULER_VALIDATION_ISSUE_ACTIVE_EXCEEDS_CREATED,
    SCHEDULER_VALIDATION_ISSUE_JOINED_EXCEEDS_CREATED,
    SCHEDULER_VALIDATION_ISSUE_ACTIVE_JOINED_EXCEED_CREATED,
    SCHEDULER_VALIDATION_ISSUE_RUNNING_EXCEEDS_ACTIVE,
    SCHEDULER_VALIDATION_ISSUE_CALLBACK_OUTCOMES_EXCEED_STARTS,
    SCHEDULER_VALIDATION_ISSUE_ACCEPTED_EXCEEDS_SUBMITTED,
    SCHEDULER_VALIDATION_ISSUE_REJECTED_EXCEEDS_SUBMITTED,
    SCHEDULER_VALIDATION_ISSUE_DEQUEUED_EXCEEDS_ACCEPTED,
    SCHEDULER_VALIDATION_ISSUE_CALLBACK_STARTS_EXCEED_DEQUEUED,
    SCHEDULER_VALIDATION_ISSUE_LIFECYCLE_FLAGS_INVALID,
    SCHEDULER_VALIDATION_ISSUE_QUIESCENT_MODE_NOT_APPLICABLE,
    SCHEDULER_VALIDATION_ISSUE_SUBMISSION_BALANCE_MISMATCH,
    SCHEDULER_VALIDATION_ISSUE_ACCEPTED_DEQUEUED_MISMATCH,
    SCHEDULER_VALIDATION_ISSUE_DEQUEUED_CALLBACK_MISMATCH,
    SCHEDULER_VALIDATION_ISSUE_CALLBACK_BALANCE_MISMATCH,
    SCHEDULER_VALIDATION_ISSUE_QUIESCENT_QUEUE_NOT_EMPTY,
    SCHEDULER_VALIDATION_ISSUE_QUIESCENT_CALLBACK_RUNNING,
    SCHEDULER_VALIDATION_ISSUE_QUIESCENT_WORKER_ACTIVE,
    SCHEDULER_VALIDATION_ISSUE_STOPPED_WORKERS_NOT_JOINED,
    SCHEDULER_VALIDATION_ISSUE_STARTUP_FAILURES_EXCEED_CONFIGURED
} SchedulerValidationIssue;

typedef struct {
    SchedulerValidationIssue issue;
    SchedulerValidationSeverity severity;
    uint64_t observed;
    uint64_t expected;
} SchedulerValidationIssueRecord;

typedef struct {
    SchedulerValidationMode mode;
    uint32_t snapshot_version;
    size_t issue_count;
    size_t violation_count;
    size_t advisory_count;
    bool validation_incomplete;
    bool issues_truncated;
    SchedulerValidationIssueRecord
        issues[SCHEDULER_VALIDATION_ISSUE_CAPACITY];
} SchedulerValidationResult;

typedef enum {
    SCHEDULER_HEALTH_HEALTHY = 1,
    SCHEDULER_HEALTH_DEGRADED,
    SCHEDULER_HEALTH_STOPPING,
    SCHEDULER_HEALTH_STOPPED,
    SCHEDULER_HEALTH_FAILED
} SchedulerHealth;

static inline void scheduler_observability_increment_saturating(
    uint64_t *counter,
    bool *overflow_detected
)
{
    if (counter == NULL || overflow_detected == NULL) {
        return;
    }
    if (*counter == UINT64_MAX) {
        *overflow_detected = true;
        return;
    }
    ++(*counter);
}

/*
 * The owning worker is the only writer; snapshot capture performs atomic
 * reads. This avoids a contended read-modify-write instruction.
 */
static inline bool
scheduler_observability_increment_single_writer_atomic(
    _Atomic uint64_t *counter,
    atomic_bool *overflow_detected
)
{
    uint64_t current;

    if (counter == NULL || overflow_detected == NULL) {
        return false;
    }
    current = atomic_load(counter);
    if (current == UINT64_MAX) {
        atomic_store(overflow_detected, true);
        return false;
    }
    atomic_store(counter, current + UINT64_C(1));
    return true;
}

bool concurrent_task_queue_capture_runtime_snapshot(
    ConcurrentTaskQueue *queue,
    ConcurrentTaskQueueRuntimeSnapshot *snapshot
);

bool scheduler_capture_snapshot(
    Scheduler *scheduler,
    SchedulerSnapshot *snapshot
);

bool scheduler_snapshot_validate_basic(const SchedulerSnapshot *snapshot);
bool scheduler_snapshot_validate_quiescent(
    const SchedulerSnapshot *snapshot
);
bool scheduler_snapshot_validate(
    const SchedulerSnapshot *snapshot,
    SchedulerValidationMode mode,
    SchedulerValidationResult *result
);
const char *scheduler_validation_issue_name(
    SchedulerValidationIssue issue
);
const char *scheduler_health_name(SchedulerHealth health);
bool scheduler_snapshot_derive_health(
    const SchedulerSnapshot *snapshot,
    const SchedulerValidationResult *result,
    SchedulerHealth *health
);
int scheduler_validation_format(
    const SchedulerSnapshot *snapshot,
    const SchedulerValidationResult *result,
    char *buffer,
    size_t buffer_size
);

#endif
