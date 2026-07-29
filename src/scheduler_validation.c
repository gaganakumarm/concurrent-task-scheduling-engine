#include "internal/scheduler_observability.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void record_issue(
    SchedulerValidationResult *result,
    SchedulerValidationIssue issue,
    SchedulerValidationSeverity severity,
    uint64_t observed,
    uint64_t expected
)
{
    if (result->issue_count < SCHEDULER_VALIDATION_ISSUE_CAPACITY) {
        SchedulerValidationIssueRecord *record =
            &result->issues[result->issue_count];
        record->issue = issue;
        record->severity = severity;
        record->observed = observed;
        record->expected = expected;
        result->issue_count++;
    } else {
        result->issues_truncated = true;
    }
    if (severity == SCHEDULER_VALIDATION_SEVERITY_VIOLATION) {
        result->violation_count++;
    } else if (severity == SCHEDULER_VALIDATION_SEVERITY_ADVISORY) {
        result->advisory_count++;
    } else if (severity == SCHEDULER_VALIDATION_SEVERITY_INCOMPLETE) {
        result->validation_incomplete = true;
    }
}

static uint64_t size_as_u64(size_t value)
{
    return value > UINT64_MAX ? UINT64_MAX : (uint64_t)value;
}

static bool lifecycle_flags_valid(const SchedulerSnapshot *snapshot)
{
    switch (snapshot->state) {
    case SCHEDULER_SNAPSHOT_STATE_INITIALIZED:
    case SCHEDULER_SNAPSHOT_STATE_STARTING:
    case SCHEDULER_SNAPSHOT_STATE_FAILED:
        return !snapshot->submissions_open && !snapshot->shutdown_started;
    case SCHEDULER_SNAPSHOT_STATE_RUNNING:
        return snapshot->submissions_open && !snapshot->shutdown_started;
    case SCHEDULER_SNAPSHOT_STATE_SHUTTING_DOWN:
    case SCHEDULER_SNAPSHOT_STATE_STOPPED:
        return !snapshot->submissions_open && snapshot->shutdown_started;
    default:
        return false;
    }
}

static bool quiescent_mode_applicable(const SchedulerSnapshot *snapshot)
{
    return snapshot->state == SCHEDULER_SNAPSHOT_STATE_INITIALIZED
        || snapshot->state == SCHEDULER_SNAPSHOT_STATE_STOPPED
        || (snapshot->state == SCHEDULER_SNAPSHOT_STATE_FAILED
            && !snapshot->submissions_open
            && snapshot->currently_running_count == UINT64_C(0)
            && snapshot->active_worker_count == 0U
            && snapshot->queue_current_size == 0U);
}

static void validate_structural(
    const SchedulerSnapshot *snapshot,
    SchedulerValidationResult *result
)
{
#define CHECK_GREATER(left, right, code) \
    do { \
        if ((left) > (right)) { \
            record_issue( \
                result, (code), SCHEDULER_VALIDATION_SEVERITY_VIOLATION, \
                size_as_u64(left), size_as_u64(right) \
            ); \
        } \
    } while (0)

    CHECK_GREATER(snapshot->queue_current_size, snapshot->queue_capacity,
        SCHEDULER_VALIDATION_ISSUE_QUEUE_SIZE_EXCEEDS_CAPACITY);
    CHECK_GREATER(snapshot->queue_high_water_mark, snapshot->queue_capacity,
        SCHEDULER_VALIDATION_ISSUE_QUEUE_HIGH_WATER_EXCEEDS_CAPACITY);
    CHECK_GREATER(snapshot->queue_current_size,
        snapshot->queue_high_water_mark,
        SCHEDULER_VALIDATION_ISSUE_QUEUE_SIZE_EXCEEDS_HIGH_WATER);
    CHECK_GREATER(snapshot->created_worker_count,
        snapshot->configured_worker_count,
        SCHEDULER_VALIDATION_ISSUE_CREATED_EXCEEDS_CONFIGURED);
    CHECK_GREATER(snapshot->ready_worker_count,
        snapshot->created_worker_count,
        SCHEDULER_VALIDATION_ISSUE_READY_EXCEEDS_CREATED);
    CHECK_GREATER(snapshot->active_worker_count,
        snapshot->created_worker_count,
        SCHEDULER_VALIDATION_ISSUE_ACTIVE_EXCEEDS_CREATED);
    CHECK_GREATER(snapshot->joined_worker_count,
        snapshot->created_worker_count,
        SCHEDULER_VALIDATION_ISSUE_JOINED_EXCEEDS_CREATED);
    if (snapshot->active_worker_count
            > snapshot->created_worker_count - (
                snapshot->joined_worker_count
                    > snapshot->created_worker_count
                ? snapshot->created_worker_count
                : snapshot->joined_worker_count
            )) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_ACTIVE_JOINED_EXCEED_CREATED,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            size_as_u64(snapshot->active_worker_count
                + snapshot->joined_worker_count),
            size_as_u64(snapshot->created_worker_count));
    }
    if (snapshot->currently_running_count
        > size_as_u64(snapshot->active_worker_count)) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_RUNNING_EXCEEDS_ACTIVE,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            snapshot->currently_running_count,
            size_as_u64(snapshot->active_worker_count));
    }
    if (snapshot->worker_startup_failure_count
        > size_as_u64(snapshot->configured_worker_count)) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_STARTUP_FAILURES_EXCEED_CONFIGURED,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            snapshot->worker_startup_failure_count,
            size_as_u64(snapshot->configured_worker_count));
    }
    if (!lifecycle_flags_valid(snapshot)) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_LIFECYCLE_FLAGS_INVALID,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            (uint64_t)snapshot->state, UINT64_C(0));
    }
#undef CHECK_GREATER
}

static bool add_exceeds(uint64_t left, uint64_t right, uint64_t limit)
{
    return left > limit || right > limit - left;
}

static void validate_accounting_bounds(
    const SchedulerSnapshot *snapshot,
    SchedulerValidationResult *result
)
{
    if (add_exceeds(snapshot->callback_succeeded_count,
            snapshot->callback_failed_count,
            snapshot->callback_started_count)) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_CALLBACK_OUTCOMES_EXCEED_STARTS,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            snapshot->callback_succeeded_count,
            snapshot->callback_started_count);
    }
#define CHECK_U64(left, right, code) \
    do { \
        if ((left) > (right)) { \
            record_issue(result, (code), \
                SCHEDULER_VALIDATION_SEVERITY_VIOLATION, (left), (right)); \
        } \
    } while (0)
    CHECK_U64(snapshot->accepted_count, snapshot->submitted_count,
        SCHEDULER_VALIDATION_ISSUE_ACCEPTED_EXCEEDS_SUBMITTED);
    CHECK_U64(snapshot->rejected_count, snapshot->submitted_count,
        SCHEDULER_VALIDATION_ISSUE_REJECTED_EXCEEDS_SUBMITTED);
    CHECK_U64(snapshot->dequeued_count, snapshot->accepted_count,
        SCHEDULER_VALIDATION_ISSUE_DEQUEUED_EXCEEDS_ACCEPTED);
    CHECK_U64(snapshot->callback_started_count, snapshot->dequeued_count,
        SCHEDULER_VALIDATION_ISSUE_CALLBACK_STARTS_EXCEED_DEQUEUED);
#undef CHECK_U64
}

static void validate_quiescent(
    const SchedulerSnapshot *snapshot,
    SchedulerValidationResult *result
)
{
    if (snapshot->submitted_count
        != snapshot->accepted_count + snapshot->rejected_count) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_SUBMISSION_BALANCE_MISMATCH,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            snapshot->submitted_count,
            snapshot->accepted_count + snapshot->rejected_count);
    }
#define CHECK_EQUAL(left, right, code) \
    do { \
        if ((left) != (right)) { \
            record_issue(result, (code), \
                SCHEDULER_VALIDATION_SEVERITY_VIOLATION, (left), (right)); \
        } \
    } while (0)
    CHECK_EQUAL(snapshot->accepted_count, snapshot->dequeued_count,
        SCHEDULER_VALIDATION_ISSUE_ACCEPTED_DEQUEUED_MISMATCH);
    CHECK_EQUAL(snapshot->dequeued_count, snapshot->callback_started_count,
        SCHEDULER_VALIDATION_ISSUE_DEQUEUED_CALLBACK_MISMATCH);
    if (snapshot->callback_started_count
        != snapshot->callback_succeeded_count
            + snapshot->callback_failed_count) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_CALLBACK_BALANCE_MISMATCH,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            snapshot->callback_started_count,
            snapshot->callback_succeeded_count
                + snapshot->callback_failed_count);
    }
#undef CHECK_EQUAL
    if (snapshot->queue_current_size != 0U) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_QUIESCENT_QUEUE_NOT_EMPTY,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            size_as_u64(snapshot->queue_current_size), UINT64_C(0));
    }
    if (snapshot->currently_running_count != UINT64_C(0)) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_QUIESCENT_CALLBACK_RUNNING,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            snapshot->currently_running_count, UINT64_C(0));
    }
    if (snapshot->active_worker_count != 0U) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_QUIESCENT_WORKER_ACTIVE,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            size_as_u64(snapshot->active_worker_count), UINT64_C(0));
    }
    if (snapshot->state == SCHEDULER_SNAPSHOT_STATE_STOPPED
        && snapshot->joined_worker_count != snapshot->created_worker_count) {
        record_issue(result,
            SCHEDULER_VALIDATION_ISSUE_STOPPED_WORKERS_NOT_JOINED,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            size_as_u64(snapshot->joined_worker_count),
            size_as_u64(snapshot->created_worker_count));
    }
}

bool scheduler_snapshot_validate(
    const SchedulerSnapshot *snapshot,
    SchedulerValidationMode mode,
    SchedulerValidationResult *result
)
{
    SchedulerValidationResult temporary = {0};

    if (snapshot == NULL || result == NULL
        || (mode != SCHEDULER_VALIDATION_LIVE
            && mode != SCHEDULER_VALIDATION_QUIESCENT)) {
        return false;
    }
    temporary.mode = mode;
    temporary.snapshot_version = snapshot->version;
    if (snapshot->version != CONCURRENT_SCHEDULER_SNAPSHOT_VERSION) {
        record_issue(&temporary,
            SCHEDULER_VALIDATION_ISSUE_SNAPSHOT_VERSION_UNSUPPORTED,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            snapshot->version, CONCURRENT_SCHEDULER_SNAPSHOT_VERSION);
    }
    if (snapshot->consistency
        != SCHEDULER_SNAPSHOT_CONSISTENCY_DOMAIN_EXACT) {
        record_issue(&temporary,
            SCHEDULER_VALIDATION_ISSUE_CONSISTENCY_UNSUPPORTED,
            SCHEDULER_VALIDATION_SEVERITY_VIOLATION,
            (uint64_t)snapshot->consistency,
            SCHEDULER_SNAPSHOT_CONSISTENCY_DOMAIN_EXACT);
    }
    if (snapshot->overflow_detected) {
        record_issue(&temporary,
            SCHEDULER_VALIDATION_ISSUE_ACCOUNTING_OVERFLOW,
            SCHEDULER_VALIDATION_SEVERITY_INCOMPLETE,
            UINT64_C(1), UINT64_C(0));
    }
    validate_structural(snapshot, &temporary);
    if (!snapshot->overflow_detected) {
        validate_accounting_bounds(snapshot, &temporary);
    }
    if (mode == SCHEDULER_VALIDATION_QUIESCENT) {
        if (!quiescent_mode_applicable(snapshot)) {
            record_issue(&temporary,
                SCHEDULER_VALIDATION_ISSUE_QUIESCENT_MODE_NOT_APPLICABLE,
                SCHEDULER_VALIDATION_SEVERITY_INCOMPLETE,
                (uint64_t)snapshot->state,
                SCHEDULER_SNAPSHOT_STATE_STOPPED);
        } else if (!snapshot->overflow_detected) {
            validate_quiescent(snapshot, &temporary);
        }
    }
    *result = temporary;
    return true;
}

const char *scheduler_validation_issue_name(
    SchedulerValidationIssue issue
)
{
    static const char *const names[] = {
        "UNKNOWN", "SNAPSHOT_VERSION_UNSUPPORTED", "CONSISTENCY_UNSUPPORTED",
        "ACCOUNTING_OVERFLOW", "QUEUE_SIZE_EXCEEDS_CAPACITY",
        "QUEUE_HIGH_WATER_EXCEEDS_CAPACITY",
        "QUEUE_SIZE_EXCEEDS_HIGH_WATER", "CREATED_EXCEEDS_CONFIGURED",
        "READY_EXCEEDS_CREATED", "ACTIVE_EXCEEDS_CREATED",
        "JOINED_EXCEEDS_CREATED", "ACTIVE_JOINED_EXCEED_CREATED",
        "RUNNING_EXCEEDS_ACTIVE", "CALLBACK_OUTCOMES_EXCEED_STARTS",
        "ACCEPTED_EXCEEDS_SUBMITTED", "REJECTED_EXCEEDS_SUBMITTED",
        "DEQUEUED_EXCEEDS_ACCEPTED", "CALLBACK_STARTS_EXCEED_DEQUEUED",
        "LIFECYCLE_FLAGS_INVALID", "QUIESCENT_MODE_NOT_APPLICABLE",
        "SUBMISSION_BALANCE_MISMATCH", "ACCEPTED_DEQUEUED_MISMATCH",
        "DEQUEUED_CALLBACK_MISMATCH", "CALLBACK_BALANCE_MISMATCH",
        "QUIESCENT_QUEUE_NOT_EMPTY", "QUIESCENT_CALLBACK_RUNNING",
        "QUIESCENT_WORKER_ACTIVE", "STOPPED_WORKERS_NOT_JOINED",
        "STARTUP_FAILURES_EXCEED_CONFIGURED"
    };
    size_t index = (size_t)issue;
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : names[0];
}

const char *scheduler_health_name(SchedulerHealth health)
{
    switch (health) {
    case SCHEDULER_HEALTH_HEALTHY: return "HEALTHY";
    case SCHEDULER_HEALTH_DEGRADED: return "DEGRADED";
    case SCHEDULER_HEALTH_STOPPING: return "STOPPING";
    case SCHEDULER_HEALTH_STOPPED: return "STOPPED";
    case SCHEDULER_HEALTH_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

bool scheduler_snapshot_derive_health(
    const SchedulerSnapshot *snapshot,
    const SchedulerValidationResult *result,
    SchedulerHealth *health
)
{
    if (snapshot == NULL || result == NULL || health == NULL
        || result->snapshot_version != snapshot->version) {
        return false;
    }
    if (snapshot->state == SCHEDULER_SNAPSHOT_STATE_FAILED
        || result->violation_count != 0U) {
        *health = SCHEDULER_HEALTH_FAILED;
    } else if (snapshot->state == SCHEDULER_SNAPSHOT_STATE_SHUTTING_DOWN) {
        *health = SCHEDULER_HEALTH_STOPPING;
    } else if (snapshot->state == SCHEDULER_SNAPSHOT_STATE_STOPPED) {
        *health = SCHEDULER_HEALTH_STOPPED;
    } else if (result->validation_incomplete
        || result->advisory_count != 0U
        || snapshot->callback_failed_count != UINT64_C(0)
        || snapshot->worker_startup_failure_count != UINT64_C(0)
        || snapshot->worker_runtime_failure_count != UINT64_C(0)
        || snapshot->join_failure_count != UINT64_C(0)) {
        *health = SCHEDULER_HEALTH_DEGRADED;
    } else {
        *health = SCHEDULER_HEALTH_HEALTHY;
    }
    return true;
}

static void append_format(
    char *buffer,
    size_t buffer_size,
    size_t *required,
    const char *format,
    ...
)
{
    va_list arguments;
    int count;
    size_t offset = *required < buffer_size ? *required : buffer_size;
    size_t available = offset < buffer_size ? buffer_size - offset : 0U;

    va_start(arguments, format);
    count = vsnprintf(
        available != 0U ? buffer + offset : NULL,
        available,
        format,
        arguments
    );
    va_end(arguments);
    if (count > 0) {
        *required += (size_t)count;
    }
}

int scheduler_validation_format(
    const SchedulerSnapshot *snapshot,
    const SchedulerValidationResult *result,
    char *buffer,
    size_t buffer_size
)
{
    size_t required = 0U;
    size_t index;

    if (snapshot == NULL || result == NULL
        || (buffer == NULL && buffer_size != 0U)) {
        return -1;
    }
    append_format(buffer, buffer_size, &required,
        "mode=%s version=%" PRIu32 " issues=%zu violations=%zu "
        "advisories=%zu incomplete=%u truncated=%u",
        result->mode == SCHEDULER_VALIDATION_LIVE ? "LIVE" : "QUIESCENT",
        result->snapshot_version, result->issue_count,
        result->violation_count, result->advisory_count,
        result->validation_incomplete ? 1U : 0U,
        result->issues_truncated ? 1U : 0U);
    for (index = 0U; index < result->issue_count; ++index) {
        append_format(buffer, buffer_size, &required,
            "\nissue=%s severity=%u observed=%" PRIu64
            " expected=%" PRIu64,
            scheduler_validation_issue_name(result->issues[index].issue),
            (unsigned)result->issues[index].severity,
            result->issues[index].observed,
            result->issues[index].expected);
    }
    if (buffer_size != 0U) {
        buffer[buffer_size - 1U] = '\0';
    }
    return required > (size_t)INT_MAX ? INT_MAX : (int)required;
}
