#ifndef CONCURRENT_SCHEDULER_BENCHMARK_TIMER_H
#define CONCURRENT_SCHEDULER_BENCHMARK_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t frequency;
} BenchmarkTimer;

bool benchmark_timer_init(BenchmarkTimer *timer);
bool benchmark_timer_now(const BenchmarkTimer *timer, uint64_t *ticks);
bool benchmark_timer_duration_ns(
    const BenchmarkTimer *timer,
    uint64_t start,
    uint64_t end,
    uint64_t *duration_ns
);
bool benchmark_timer_utc(char *buffer, size_t buffer_size);

#endif
