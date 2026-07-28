#include "benchmark_timer.h"

#include <windows.h>

#include <inttypes.h>
#include <stdio.h>

enum {
    NANOSECONDS_PER_SECOND = 1000000000
};

bool benchmark_timer_init(BenchmarkTimer *timer)
{
    LARGE_INTEGER frequency;

    if (timer == NULL
        || !QueryPerformanceFrequency(&frequency)
        || frequency.QuadPart <= 0) {
        return false;
    }
    timer->frequency = (uint64_t)frequency.QuadPart;
    return true;
}

bool benchmark_timer_now(const BenchmarkTimer *timer, uint64_t *ticks)
{
    LARGE_INTEGER counter;

    if (timer == NULL
        || timer->frequency == 0U
        || ticks == NULL
        || !QueryPerformanceCounter(&counter)
        || counter.QuadPart < 0) {
        return false;
    }
    *ticks = (uint64_t)counter.QuadPart;
    return true;
}

bool benchmark_timer_duration_ns(
    const BenchmarkTimer *timer,
    uint64_t start,
    uint64_t end,
    uint64_t *duration_ns
)
{
    uint64_t ticks;
    uint64_t seconds;
    uint64_t remainder;
    uint64_t whole_ns;
    uint64_t fractional_ns;

    if (timer == NULL
        || timer->frequency == 0U
        || duration_ns == NULL
        || end < start) {
        return false;
    }

    ticks = end - start;
    seconds = ticks / timer->frequency;
    remainder = ticks % timer->frequency;
    if (seconds > UINT64_MAX / (uint64_t)NANOSECONDS_PER_SECOND) {
        return false;
    }
    whole_ns = seconds * (uint64_t)NANOSECONDS_PER_SECOND;
    fractional_ns = (uint64_t)(
        ((long double)remainder
            * (long double)NANOSECONDS_PER_SECOND)
        / (long double)timer->frequency
    );
    if (UINT64_MAX - whole_ns < fractional_ns) {
        return false;
    }
    *duration_ns = whole_ns + fractional_ns;
    return true;
}

bool benchmark_timer_utc(char *buffer, size_t buffer_size)
{
    SYSTEMTIME value;
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return false;
    }
    GetSystemTime(&value);
    written = snprintf(
        buffer,
        buffer_size,
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        (unsigned int)value.wYear,
        (unsigned int)value.wMonth,
        (unsigned int)value.wDay,
        (unsigned int)value.wHour,
        (unsigned int)value.wMinute,
        (unsigned int)value.wSecond,
        (unsigned int)value.wMilliseconds
    );
    return written > 0 && (size_t)written < buffer_size;
}
