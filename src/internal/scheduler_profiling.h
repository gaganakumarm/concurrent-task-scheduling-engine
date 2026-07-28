#ifndef CONCURRENT_SCHEDULER_INTERNAL_PROFILING_H
#define CONCURRENT_SCHEDULER_INTERNAL_PROFILING_H

#if !defined(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
#error "Internal profiling is available only in profiling builds."
#endif

#include "concurrent_scheduler/concurrent_task_queue.h"
#include "concurrent_scheduler/scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SCHEDULER_PROFILING_MAX_WORKERS = 64
};

typedef struct {
    uint64_t enqueue_attempts;
    uint64_t enqueue_immediate_successes;
    uint64_t enqueue_lock_attempts;
    uint64_t enqueue_lock_wait_ticks;
    uint64_t enqueue_lock_max_wait_ticks;
    uint64_t enqueue_inferred_contended;
    uint64_t enqueue_wait_count;
    uint64_t enqueue_wait_ticks;
    uint64_t enqueue_wait_max_ticks;
    uint64_t enqueue_wait_returns;
    uint64_t enqueue_predicate_false_wakeups;
    uint64_t enqueue_successes_after_wait;
    uint64_t queue_full_observations;
    uint64_t dequeue_attempts;
    uint64_t dequeue_immediate_successes;
    uint64_t dequeue_lock_attempts;
    uint64_t dequeue_lock_wait_ticks;
    uint64_t dequeue_lock_max_wait_ticks;
    uint64_t dequeue_inferred_contended;
    uint64_t dequeue_wait_count;
    uint64_t dequeue_wait_ticks;
    uint64_t dequeue_wait_max_ticks;
    uint64_t dequeue_wait_returns;
    uint64_t dequeue_predicate_false_wakeups;
    uint64_t dequeue_successes_after_wait;
    uint64_t queue_empty_observations;
    uint64_t not_empty_signals;
    uint64_t not_full_signals;
    uint64_t shutdown_broadcasts;
    uint64_t occupancy_sample_count;
    uint64_t occupancy_sample_sum;
    uint64_t occupancy_min;
    uint64_t occupancy_max;
    uint64_t occupancy_zero_observations;
    uint64_t occupancy_full_observations;
    uint64_t worker_tasks[SCHEDULER_PROFILING_MAX_WORKERS];
    uint64_t worker_dequeue_wait_count[SCHEDULER_PROFILING_MAX_WORKERS];
    uint64_t worker_dequeue_wait_ticks[SCHEDULER_PROFILING_MAX_WORKERS];
    uint64_t worker_dequeue_lock_ticks[SCHEDULER_PROFILING_MAX_WORKERS];
    size_t worker_count;
    uint64_t timer_frequency;
    bool timing_failed;
    bool counter_overflow;
} SchedulerProfilingSnapshot;

void concurrent_task_queue_profiling_set_worker_index(size_t worker_index);
bool concurrent_task_queue_profiling_snapshot(
    ConcurrentTaskQueue *queue,
    size_t worker_count,
    SchedulerProfilingSnapshot *snapshot
);
bool scheduler_profiling_snapshot(
    Scheduler *scheduler,
    SchedulerProfilingSnapshot *snapshot
);
size_t scheduler_profiling_current_worker_index(void);

#endif
