#ifndef CONCURRENT_SCHEDULER_INTERNAL_OBSERVABILITY_H
#define CONCURRENT_SCHEDULER_INTERNAL_OBSERVABILITY_H

#include "concurrent_scheduler/concurrent_task_queue.h"
#include "concurrent_scheduler/scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define CONCURRENT_SCHEDULER_SNAPSHOT_VERSION UINT32_C(1)

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

#endif
