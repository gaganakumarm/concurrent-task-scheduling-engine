#include "benchmark_timer.h"

#include "concurrent_scheduler/scheduler.h"
#include "platform/sync.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DEFAULT_WORKERS = 4,
    DEFAULT_PRODUCERS = 4,
    DEFAULT_CAPACITY = 16,
    DEFAULT_TASKS = 1000,
    DEFAULT_WARMUP = 3,
    DEFAULT_ITERATIONS = 10,
    LIGHT_CPU_OPERATIONS = 128,
    MEDIUM_CPU_OPERATIONS = 4096,
    CSV_BUFFER_SIZE = 2048,
    UTC_BUFFER_SIZE = 32
};

typedef enum {
    PROFILE_NOOP,
    PROFILE_LIGHT_CPU,
    PROFILE_MEDIUM_CPU,
    PROFILE_CONTROLLED_BLOCKING
} CallbackProfile;

typedef enum {
    MODE_VALIDATED,
    MODE_LOW_OVERHEAD
} BenchmarkMode;

typedef struct {
    const char *scenario;
    CallbackProfile profile;
    BenchmarkMode mode;
    size_t workers;
    size_t producers;
    size_t capacity;
    size_t tasks;
    size_t warmup;
    size_t iterations;
    const char *output;
} BenchmarkConfig;

typedef struct {
    size_t count;
    uint64_t minimum;
    uint64_t maximum;
    double mean;
    uint64_t p50;
    uint64_t p95;
    double standard_deviation;
} Statistics;

typedef struct {
    uint64_t submit_duration_ns;
    uint64_t shutdown_duration_ns;
    uint64_t join_duration_ns;
    uint64_t total_duration_ns;
    Statistics submit_latency;
    Statistics end_to_end_latency;
    size_t attempted;
    size_t accepted;
    size_t executed;
    size_t rejected;
    bool correctness_passed;
} IterationResult;

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    size_t ready_count;
    bool released;
    bool aborted;
} StartBarrier;

typedef struct {
    BenchmarkTimer timer;
    SchedMutex mutex;
    SchedCondition blocking_condition;
    Task *tasks;
    size_t task_count;
    unsigned int *execution_counts;
    uint64_t *completion_ticks;
    CallbackProfile profile;
    BenchmarkMode mode;
    size_t execution_count;
    size_t duplicate_count;
    size_t unknown_count;
    uint64_t checksum;
    bool blocking_entered;
    bool blocking_released;
    bool failure;
} CallbackContext;

typedef struct {
    Scheduler *scheduler;
    StartBarrier *barrier;
    BenchmarkTimer *timer;
    Task *tasks;
    size_t begin;
    size_t end;
    uint64_t *submission_ticks;
    uint64_t *submission_latencies;
    size_t accepted;
    size_t rejected;
    SchedulerResult first_error;
    bool thread_succeeded;
} ProducerContext;

static const char *const CSV_HEADER =
    "timestamp_utc,scenario,callback_profile,mode,iteration,workers,"
    "producers,capacity,tasks_attempted,tasks_accepted,tasks_executed,"
    "tasks_rejected,submit_duration_ns,shutdown_duration_ns,"
    "join_duration_ns,total_duration_ns,throughput_tasks_per_second,"
    "mean_submit_latency_ns,p50_submit_latency_ns,p95_submit_latency_ns,"
    "min_submit_latency_ns,max_submit_latency_ns,"
    "mean_end_to_end_latency_ns,p50_end_to_end_latency_ns,"
    "p95_end_to_end_latency_ns,correctness_passed";

static const char *profile_name(CallbackProfile profile)
{
    switch (profile) {
    case PROFILE_NOOP:
        return "noop";
    case PROFILE_LIGHT_CPU:
        return "light_cpu";
    case PROFILE_MEDIUM_CPU:
        return "medium_cpu";
    case PROFILE_CONTROLLED_BLOCKING:
        return "controlled_blocking";
    default:
        return "unknown";
    }
}

static const char *mode_name(BenchmarkMode mode)
{
    return mode == MODE_VALIDATED ? "validated" : "low-overhead";
}

static bool checked_add_size(size_t left, size_t right, size_t *result)
{
    if (result == NULL || SIZE_MAX - left < right) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    uintmax_t parsed;

    if (text == NULL || value == NULL || text[0] == '\0'
        || text[0] == '-') {
        return false;
    }
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0'
        || parsed == 0U || parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool parse_profile(const char *text, CallbackProfile *profile)
{
    if (strcmp(text, "noop") == 0) {
        *profile = PROFILE_NOOP;
    } else if (strcmp(text, "light_cpu") == 0) {
        *profile = PROFILE_LIGHT_CPU;
    } else if (strcmp(text, "medium_cpu") == 0) {
        *profile = PROFILE_MEDIUM_CPU;
    } else if (strcmp(text, "controlled_blocking") == 0) {
        *profile = PROFILE_CONTROLLED_BLOCKING;
    } else {
        return false;
    }
    return true;
}

static bool parse_mode(const char *text, BenchmarkMode *mode)
{
    if (strcmp(text, "validated") == 0) {
        *mode = MODE_VALIDATED;
    } else if (strcmp(text, "low-overhead") == 0) {
        *mode = MODE_LOW_OVERHEAD;
    } else {
        return false;
    }
    return true;
}

static bool apply_scenario(BenchmarkConfig *config, const char *scenario)
{
    config->scenario = scenario;
    if (strcmp(scenario, "throughput") == 0) {
        return true;
    }
    if (strcmp(scenario, "s1") == 0) {
        config->workers = 1U;
        config->producers = 1U;
        config->profile = PROFILE_NOOP;
    } else if (strcmp(scenario, "s2") == 0) {
        config->workers = 4U;
        config->producers = 1U;
        config->profile = PROFILE_NOOP;
    } else if (strcmp(scenario, "s3") == 0) {
        config->workers = 4U;
        config->producers = 4U;
        config->profile = PROFILE_NOOP;
    } else if (strcmp(scenario, "s4") == 0) {
        config->workers = 8U;
        config->producers = 4U;
        config->profile = PROFILE_NOOP;
    } else if (strcmp(scenario, "s5") == 0) {
        config->workers = 4U;
        config->producers = 4U;
        config->profile = PROFILE_LIGHT_CPU;
    } else if (strcmp(scenario, "s6") == 0) {
        config->workers = 4U;
        config->producers = 4U;
        config->profile = PROFILE_MEDIUM_CPU;
    } else if (strcmp(scenario, "s7") == 0) {
        config->workers = 1U;
        config->producers = 1U;
        config->capacity = 1U;
        config->profile = PROFILE_CONTROLLED_BLOCKING;
    } else if (strcmp(scenario, "s8") == 0) {
        config->workers = 4U;
        config->producers = 4U;
        config->profile = PROFILE_NOOP;
    } else {
        return false;
    }
    return true;
}

static void print_help(void)
{
    puts(
        "Usage: concurrent_scheduler_benchmarks [options]\n"
        "  --help\n"
        "  --self-test\n"
        "  --scenario throughput|s1|s2|s3|s4|s5|s6|s7|s8\n"
        "  --workers N --producers N --capacity N --tasks N\n"
        "  --warmup N --iterations N\n"
        "  --callback-profile noop|light_cpu|medium_cpu|controlled_blocking\n"
        "  --mode validated|low-overhead\n"
        "  --output PATH\n"
        "\nDefaults: throughput, 4 workers, 4 producers, capacity 16,\n"
        "1000 Tasks, 3 warm-ups, 10 measured iterations, noop, validated."
    );
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static double square_root(double value)
{
    double estimate;
    size_t iteration;

    if (value <= 0.0) {
        return 0.0;
    }
    estimate = value > 1.0 ? value : 1.0;
    for (iteration = 0U; iteration < 32U; iteration++) {
        estimate = 0.5 * (estimate + value / estimate);
    }
    return estimate;
}

static bool calculate_statistics(
    const uint64_t *values,
    size_t count,
    Statistics *statistics
)
{
    uint64_t *sorted;
    long double sum = 0.0L;
    long double variance_sum = 0.0L;
    size_t index;
    size_t p50_index;
    size_t p95_index;

    if (values == NULL || count == 0U || statistics == NULL
        || count > SIZE_MAX / sizeof(*sorted)) {
        return false;
    }
    sorted = malloc(count * sizeof(*sorted));
    if (sorted == NULL) {
        return false;
    }
    memcpy(sorted, values, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), compare_u64);
    for (index = 0U; index < count; index++) {
        sum += (long double)values[index];
    }
    statistics->count = count;
    statistics->minimum = sorted[0];
    statistics->maximum = sorted[count - 1U];
    statistics->mean = (double)(sum / (long double)count);
    p50_index = ((50U * count) + 99U) / 100U - 1U;
    p95_index = ((95U * count) + 99U) / 100U - 1U;
    statistics->p50 = sorted[p50_index];
    statistics->p95 = sorted[p95_index];
    for (index = 0U; index < count; index++) {
        long double difference =
            (long double)values[index] - (long double)statistics->mean;
        variance_sum += difference * difference;
    }
    statistics->standard_deviation = square_root(
        (double)(variance_sum / (long double)count)
    );
    free(sorted);
    return true;
}

static bool start_barrier_init(StartBarrier *barrier)
{
    memset(barrier, 0, sizeof(*barrier));
    if (sched_mutex_init(&barrier->mutex) != SCHED_SYNC_OK) {
        return false;
    }
    if (sched_condition_init(&barrier->condition) != SCHED_SYNC_OK) {
        sched_mutex_destroy(&barrier->mutex);
        return false;
    }
    return true;
}

static void start_barrier_destroy(StartBarrier *barrier)
{
    sched_condition_destroy(&barrier->condition);
    sched_mutex_destroy(&barrier->mutex);
}

static int producer_entry(void *argument)
{
    ProducerContext *context = argument;
    size_t index;

    if (sched_mutex_lock(&context->barrier->mutex) != SCHED_SYNC_OK) {
        return 1;
    }
    context->barrier->ready_count++;
    if (sched_condition_broadcast(&context->barrier->condition)
            != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&context->barrier->mutex);
        return 2;
    }
    while (!context->barrier->released && !context->barrier->aborted) {
        if (sched_condition_wait(
                &context->barrier->condition,
                &context->barrier->mutex
            ) != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&context->barrier->mutex);
            return 3;
        }
    }
    if (context->barrier->aborted) {
        (void)sched_mutex_unlock(&context->barrier->mutex);
        return 4;
    }
    if (sched_mutex_unlock(&context->barrier->mutex) != SCHED_SYNC_OK) {
        return 5;
    }

    for (index = context->begin; index < context->end; index++) {
        uint64_t start;
        uint64_t end;
        SchedulerResult result;

        if (!benchmark_timer_now(context->timer, &start)) {
            context->first_error = SCHEDULER_ERROR_SYSTEM;
            return 6;
        }
        context->submission_ticks[index] = start;
        result = scheduler_submit(context->scheduler, &context->tasks[index]);
        if (!benchmark_timer_now(context->timer, &end)
            || !benchmark_timer_duration_ns(
                context->timer,
                start,
                end,
                &context->submission_latencies[index]
            )) {
            context->first_error = SCHEDULER_ERROR_SYSTEM;
            return 7;
        }
        if (result == SCHEDULER_OK) {
            context->accepted++;
        } else if (result == SCHEDULER_ERROR_SHUTDOWN) {
            context->rejected++;
        } else {
            context->first_error = result;
            return 8;
        }
    }
    context->thread_succeeded = true;
    return 0;
}

static uint64_t deterministic_work(uint64_t value, size_t operations)
{
    size_t index;

    for (index = 0U; index < operations; index++) {
        value ^= value >> 12U;
        value ^= value << 25U;
        value ^= value >> 27U;
        value *= UINT64_C(2685821657736338717);
    }
    return value;
}

static bool task_index_for_pointer(
    const CallbackContext *context,
    const Task *task,
    size_t *index
)
{
    uintptr_t base;
    uintptr_t address;
    uintptr_t span;
    uintptr_t offset;

    if (context == NULL || context->tasks == NULL || task == NULL
        || index == NULL
        || context->task_count > UINTPTR_MAX / sizeof(*task)) {
        return false;
    }
    base = (uintptr_t)(const void *)context->tasks;
    address = (uintptr_t)(const void *)task;
    span = (uintptr_t)context->task_count * sizeof(*task);
    if (UINTPTR_MAX - base < span
        || address < base
        || address >= base + span) {
        return false;
    }
    offset = address - base;
    if (offset % sizeof(*task) != 0U) {
        return false;
    }
    *index = (size_t)(offset / sizeof(*task));
    return true;
}

static int benchmark_callback(Task *task, void *argument)
{
    CallbackContext *context = argument;
    uint64_t completion;
    uint64_t contribution;
    size_t index = 0U;
    bool known;

    if (task == NULL || context == NULL) {
        return 1;
    }
    contribution = task->id ^ UINT64_C(0x9e3779b97f4a7c15);
    if (context->profile == PROFILE_LIGHT_CPU) {
        contribution = deterministic_work(
            contribution,
            LIGHT_CPU_OPERATIONS
        );
    } else if (context->profile == PROFILE_MEDIUM_CPU) {
        contribution = deterministic_work(
            contribution,
            MEDIUM_CPU_OPERATIONS
        );
    }

    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return 2;
    }
    if (context->profile == PROFILE_CONTROLLED_BLOCKING
        && !context->blocking_released) {
        context->blocking_entered = true;
        if (sched_condition_broadcast(&context->blocking_condition)
                != SCHED_SYNC_OK) {
            context->failure = true;
        }
        while (!context->blocking_released && !context->failure) {
            if (sched_condition_wait(
                    &context->blocking_condition,
                    &context->mutex
                ) != SCHED_SYNC_OK) {
                context->failure = true;
            }
        }
    }
    if (!benchmark_timer_now(&context->timer, &completion)) {
        context->failure = true;
    }

    known = task_index_for_pointer(context, task, &index);
    if (!known) {
        context->unknown_count++;
        context->failure = true;
    }
    if (known && context->mode == MODE_VALIDATED) {
        if (context->execution_counts[index] != 0U) {
            context->duplicate_count++;
            context->failure = true;
        }
        context->execution_counts[index]++;
        context->completion_ticks[index] = completion;
    }
    if (context->execution_count == SIZE_MAX) {
        context->failure = true;
    } else {
        context->execution_count++;
    }
    context->checksum ^= contribution;
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return 3;
    }
    return context->failure ? 4 : 0;
}

static bool callback_context_init(
    CallbackContext *context,
    const BenchmarkConfig *config,
    Task *tasks,
    unsigned int *execution_counts,
    uint64_t *completion_ticks
)
{
    memset(context, 0, sizeof(*context));
    context->tasks = tasks;
    context->task_count = config->tasks;
    context->execution_counts = execution_counts;
    context->completion_ticks = completion_ticks;
    context->profile = config->profile;
    context->mode = config->mode;
    if (!benchmark_timer_init(&context->timer)
        || sched_mutex_init(&context->mutex) != SCHED_SYNC_OK) {
        return false;
    }
    if (sched_condition_init(&context->blocking_condition)
        != SCHED_SYNC_OK) {
        sched_mutex_destroy(&context->mutex);
        return false;
    }
    return true;
}

static void callback_context_destroy(CallbackContext *context)
{
    sched_condition_destroy(&context->blocking_condition);
    sched_mutex_destroy(&context->mutex);
}

static bool release_controlled_callback(CallbackContext *context)
{
    if (context->profile != PROFILE_CONTROLLED_BLOCKING) {
        return true;
    }
    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return false;
    }
    while (!context->blocking_entered && !context->failure) {
        if (sched_condition_wait(
                &context->blocking_condition,
                &context->mutex
            ) != SCHED_SYNC_OK) {
            context->failure = true;
        }
    }
    context->blocking_released = true;
    if (sched_condition_broadcast(&context->blocking_condition)
            != SCHED_SYNC_OK) {
        context->failure = true;
    }
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return false;
    }
    return !context->failure;
}

static bool validate_execution(
    const BenchmarkConfig *config,
    const CallbackContext *callback,
    const ProducerContext *producers,
    const uint64_t *submission_ticks,
    const uint64_t *completion_ticks,
    uint64_t *end_to_end,
    IterationResult *result
)
{
    size_t producer;
    size_t index;

    result->attempted = config->tasks;
    for (producer = 0U; producer < config->producers; producer++) {
        if (!checked_add_size(
                result->accepted,
                producers[producer].accepted,
                &result->accepted
            )
            || !checked_add_size(
                result->rejected,
                producers[producer].rejected,
                &result->rejected
            )
            || !producers[producer].thread_succeeded
            || producers[producer].first_error != SCHEDULER_OK) {
            return false;
        }
    }
    result->executed = callback->execution_count;
    if (callback->failure || callback->unknown_count != 0U
        || callback->duplicate_count != 0U
        || result->accepted != result->executed
        || result->attempted != result->accepted + result->rejected) {
        return false;
    }
    if (config->mode == MODE_VALIDATED) {
        for (index = 0U; index < config->tasks; index++) {
            if (callback->execution_counts[index] != 1U
                || completion_ticks[index] < submission_ticks[index]
                || !benchmark_timer_duration_ns(
                    &callback->timer,
                    submission_ticks[index],
                    completion_ticks[index],
                    &end_to_end[index]
                )) {
                return false;
            }
        }
    }
    return true;
}

static bool run_iteration(
    const BenchmarkConfig *config,
    IterationResult *result
)
{
    Scheduler scheduler = {0};
    StartBarrier barrier;
    CallbackContext callback;
    Task *tasks = NULL;
    unsigned int *execution_counts = NULL;
    uint64_t *submission_ticks = NULL;
    uint64_t *completion_ticks = NULL;
    uint64_t *submission_latencies = NULL;
    uint64_t *end_to_end = NULL;
    ProducerContext *producers = NULL;
    SchedThread *threads = NULL;
    size_t created = 0U;
    size_t joined = 0U;
    size_t execution_count_after_join = 0U;
    size_t index;
    uint64_t total_start = 0U;
    uint64_t submit_end = 0U;
    uint64_t shutdown_start = 0U;
    uint64_t shutdown_end = 0U;
    uint64_t join_start = 0U;
    uint64_t total_end = 0U;
    bool barrier_initialized = false;
    bool callback_initialized = false;
    bool scheduler_initialized = false;
    bool scheduler_started = false;
    bool success = false;

    memset(result, 0, sizeof(*result));
    if (config->tasks > SIZE_MAX / sizeof(*tasks)
        || config->tasks > SIZE_MAX / sizeof(*execution_counts)
        || config->tasks > SIZE_MAX / sizeof(*submission_ticks)
        || config->producers > SIZE_MAX / sizeof(*producers)
        || config->producers > SIZE_MAX / sizeof(*threads)) {
        goto cleanup;
    }
    tasks = calloc(config->tasks, sizeof(*tasks));
    execution_counts = calloc(config->tasks, sizeof(*execution_counts));
    submission_ticks = calloc(config->tasks, sizeof(*submission_ticks));
    completion_ticks = calloc(config->tasks, sizeof(*completion_ticks));
    submission_latencies = calloc(
        config->tasks,
        sizeof(*submission_latencies)
    );
    end_to_end = calloc(config->tasks, sizeof(*end_to_end));
    producers = calloc(config->producers, sizeof(*producers));
    threads = calloc(config->producers, sizeof(*threads));
    if (tasks == NULL || execution_counts == NULL
        || submission_ticks == NULL || completion_ticks == NULL
        || submission_latencies == NULL || end_to_end == NULL
        || producers == NULL || threads == NULL) {
        goto cleanup;
    }
    for (index = 0U; index < config->tasks; index++) {
        if (!task_init(
                &tasks[index],
                (uint64_t)index + UINT64_C(1),
                TASK_PRIORITY_NORMAL,
                UINT64_C(1)
            )) {
            goto cleanup;
        }
    }
    if (!start_barrier_init(&barrier)) {
        goto cleanup;
    }
    barrier_initialized = true;
    if (!callback_context_init(
            &callback,
            config,
            tasks,
            execution_counts,
            completion_ticks
        )) {
        goto cleanup;
    }
    callback_initialized = true;
    if (scheduler_init(
            &scheduler,
            config->capacity,
            config->workers,
            benchmark_callback,
            &callback
        ) != SCHEDULER_OK) {
        goto cleanup;
    }
    scheduler_initialized = true;
    if (scheduler_start(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    scheduler_started = true;

    for (index = 0U; index < config->producers; index++) {
        size_t base = config->tasks / config->producers;
        size_t remainder = config->tasks % config->producers;

        producers[index].scheduler = &scheduler;
        producers[index].barrier = &barrier;
        producers[index].timer = &callback.timer;
        producers[index].tasks = tasks;
        producers[index].begin = (base * index)
            + (index < remainder ? index : remainder);
        producers[index].end = producers[index].begin
            + base
            + (index < remainder ? 1U : 0U);
        producers[index].submission_ticks = submission_ticks;
        producers[index].submission_latencies = submission_latencies;
        producers[index].first_error = SCHEDULER_OK;
        if (sched_thread_create(
                &threads[index],
                producer_entry,
                &producers[index]
            ) != SCHED_SYNC_OK) {
            goto abort_producers;
        }
        created++;
    }
    if (sched_mutex_lock(&barrier.mutex) != SCHED_SYNC_OK) {
        goto abort_producers;
    }
    while (barrier.ready_count < config->producers) {
        if (sched_condition_wait(&barrier.condition, &barrier.mutex)
                != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&barrier.mutex);
            goto abort_producers;
        }
    }
    if (!benchmark_timer_now(&callback.timer, &total_start)) {
        (void)sched_mutex_unlock(&barrier.mutex);
        goto abort_producers;
    }
    barrier.released = true;
    if (sched_condition_broadcast(&barrier.condition) != SCHED_SYNC_OK
        || sched_mutex_unlock(&barrier.mutex) != SCHED_SYNC_OK) {
        goto abort_producers;
    }
    if (!release_controlled_callback(&callback)) {
        goto abort_producers;
    }
    for (index = 0U; index < created; index++) {
        int thread_result = 1;
        SchedSyncResult join_result;

        join_result = sched_thread_join(&threads[index], &thread_result);
        if (join_result == SCHED_SYNC_OK) {
            joined++;
        }
        if (join_result != SCHED_SYNC_OK || thread_result != 0) {
            goto abort_producers;
        }
    }
    if (!benchmark_timer_now(&callback.timer, &submit_end)
        || !benchmark_timer_now(&callback.timer, &shutdown_start)
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || !benchmark_timer_now(&callback.timer, &shutdown_end)
        || !benchmark_timer_now(&callback.timer, &join_start)
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || !benchmark_timer_now(&callback.timer, &total_end)) {
        goto cleanup;
    }
    scheduler_started = false;
    execution_count_after_join = callback.execution_count;
    if (!benchmark_timer_duration_ns(
            &callback.timer,
            total_start,
            submit_end,
            &result->submit_duration_ns
        )
        || !benchmark_timer_duration_ns(
            &callback.timer,
            shutdown_start,
            shutdown_end,
            &result->shutdown_duration_ns
        )
        || !benchmark_timer_duration_ns(
            &callback.timer,
            join_start,
            total_end,
            &result->join_duration_ns
        )
        || !benchmark_timer_duration_ns(
            &callback.timer,
            total_start,
            total_end,
            &result->total_duration_ns
        )
        || !validate_execution(
            config,
            &callback,
            producers,
            submission_ticks,
            completion_ticks,
            end_to_end,
            result
        )
        || !calculate_statistics(
            submission_latencies,
            config->tasks,
            &result->submit_latency
        )
        || (config->mode == MODE_VALIDATED
            && !calculate_statistics(
                end_to_end,
                config->tasks,
                &result->end_to_end_latency
            ))) {
        goto cleanup;
    }
    result->correctness_passed = true;
    success = true;
    goto cleanup;

abort_producers:
    if (barrier_initialized
        && sched_mutex_lock(&barrier.mutex) == SCHED_SYNC_OK) {
        barrier.aborted = true;
        barrier.released = true;
        (void)sched_condition_broadcast(&barrier.condition);
        (void)sched_mutex_unlock(&barrier.mutex);
    }
    if (callback_initialized
        && sched_mutex_lock(&callback.mutex) == SCHED_SYNC_OK) {
        callback.failure = true;
        callback.blocking_released = true;
        (void)sched_condition_broadcast(&callback.blocking_condition);
        (void)sched_mutex_unlock(&callback.mutex);
    }

cleanup:
    if (scheduler_started) {
        (void)scheduler_shutdown(&scheduler);
    }
    for (index = joined; index < created; index++) {
        (void)sched_thread_join(&threads[index], NULL);
    }
    for (index = 0U; index < created; index++) {
        sched_thread_destroy(&threads[index]);
    }
    if (scheduler_started) {
        (void)scheduler_join(&scheduler);
    }
    if (scheduler_initialized) {
        if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
            success = false;
        }
        if (success
            && callback_initialized
            && callback.execution_count != execution_count_after_join) {
            success = false;
        }
    }
    if (callback_initialized) {
        callback_context_destroy(&callback);
    }
    if (barrier_initialized) {
        start_barrier_destroy(&barrier);
    }
    free(threads);
    free(producers);
    free(end_to_end);
    free(submission_latencies);
    free(completion_ticks);
    free(submission_ticks);
    free(execution_counts);
    free(tasks);
    return success;
}

static bool write_csv_row(
    FILE *file,
    const BenchmarkConfig *config,
    size_t iteration,
    const IterationResult *result
)
{
    char utc[UTC_BUFFER_SIZE];
    double throughput;
    int written;

    if (!benchmark_timer_utc(utc, sizeof(utc))
        || result->total_duration_ns == 0U) {
        return false;
    }
    throughput = ((double)result->executed * 1000000000.0)
        / (double)result->total_duration_ns;
    written = fprintf(
        file,
        "%s,%s,%s,%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,"
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%.6f,%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",",
        utc,
        config->scenario,
        profile_name(config->profile),
        mode_name(config->mode),
        iteration,
        config->workers,
        config->producers,
        config->capacity,
        result->attempted,
        result->accepted,
        result->executed,
        result->rejected,
        result->submit_duration_ns,
        result->shutdown_duration_ns,
        result->join_duration_ns,
        result->total_duration_ns,
        throughput,
        result->submit_latency.mean,
        result->submit_latency.p50,
        result->submit_latency.p95,
        result->submit_latency.minimum,
        result->submit_latency.maximum
    );
    if (written < 0) {
        return false;
    }
    if (config->mode == MODE_VALIDATED) {
        written = fprintf(
            file,
            "%.3f,%" PRIu64 ",%" PRIu64 ",true\n",
            result->end_to_end_latency.mean,
            result->end_to_end_latency.p50,
            result->end_to_end_latency.p95
        );
    } else {
        written = fprintf(file, ",,,true\n");
    }
    return written >= 0;
}

static void print_configuration(const BenchmarkConfig *config)
{
    printf(
        "Resolved configuration:\n"
        "  scenario: %s\n"
        "  callback profile: %s\n"
        "  mode: %s\n"
        "  workers: %zu\n"
        "  producers: %zu\n"
        "  capacity: %zu\n"
        "  Tasks: %zu\n"
        "  warm-up iterations: %zu\n"
        "  measured iterations: %zu\n"
        "  output: %s\n"
        "  platform: Windows\n"
        "  compiler: %s\n",
        config->scenario,
        profile_name(config->profile),
        mode_name(config->mode),
        config->workers,
        config->producers,
        config->capacity,
        config->tasks,
        config->warmup,
        config->iterations,
        config->output,
#if defined(__clang__)
        "Clang"
#elif defined(__GNUC__)
        "GNU C"
#elif defined(_MSC_VER)
        "MSVC"
#else
        "unknown"
#endif
    );
}

static bool run_benchmark(const BenchmarkConfig *config)
{
    IterationResult result;
    uint64_t *throughputs;
    uint64_t *shutdowns;
    uint64_t *joins;
    uint64_t *submit_means;
    uint64_t *end_to_end_means;
    Statistics throughput_stats;
    Statistics shutdown_stats;
    Statistics join_stats;
    Statistics submit_stats;
    Statistics end_to_end_stats;
    FILE *output;
    size_t index;
    bool success = false;

    throughputs = calloc(config->iterations, sizeof(*throughputs));
    shutdowns = calloc(config->iterations, sizeof(*shutdowns));
    joins = calloc(config->iterations, sizeof(*joins));
    submit_means = calloc(config->iterations, sizeof(*submit_means));
    end_to_end_means = calloc(config->iterations, sizeof(*end_to_end_means));
    if (throughputs == NULL || shutdowns == NULL || joins == NULL
        || submit_means == NULL || end_to_end_means == NULL) {
        goto cleanup;
    }
    print_configuration(config);
    for (index = 0U; index < config->warmup; index++) {
        if (!run_iteration(config, &result)) {
            fprintf(stderr, "Warm-up iteration %zu failed.\n", index + 1U);
            goto cleanup;
        }
    }
    output = fopen(config->output, "w");
    if (output == NULL) {
        fprintf(stderr, "Unable to open CSV output: %s\n", config->output);
        goto cleanup;
    }
    if (fprintf(output, "%s\n", CSV_HEADER) < 0) {
        fclose(output);
        goto cleanup;
    }
    for (index = 0U; index < config->iterations; index++) {
        if (!run_iteration(config, &result)
            || !result.correctness_passed) {
            fprintf(stderr, "Measured iteration %zu failed.\n", index + 1U);
            fclose(output);
            goto cleanup;
        }
        throughputs[index] = result.total_duration_ns == 0U
            ? 0U
            : (uint64_t)(
                ((long double)result.executed
                    * (long double)UINT64_C(1000000000))
                / (long double)result.total_duration_ns
            );
        shutdowns[index] = result.shutdown_duration_ns;
        joins[index] = result.join_duration_ns;
        submit_means[index] = (uint64_t)result.submit_latency.mean;
        end_to_end_means[index] =
            (uint64_t)result.end_to_end_latency.mean;
        if (!write_csv_row(output, config, index + 1U, &result)) {
            fclose(output);
            goto cleanup;
        }
        printf(
            "Iteration %zu: correctness=passed accepted=%zu executed=%zu "
            "rejected=%zu\n",
            index + 1U,
            result.accepted,
            result.executed,
            result.rejected
        );
    }
    if (fclose(output) != 0
        || !calculate_statistics(
            throughputs,
            config->iterations,
            &throughput_stats
        )
        || !calculate_statistics(
            shutdowns,
            config->iterations,
            &shutdown_stats
        )
        || !calculate_statistics(
            joins,
            config->iterations,
            &join_stats
        )
        || !calculate_statistics(
            submit_means,
            config->iterations,
            &submit_stats
        )
        || (config->mode == MODE_VALIDATED
            && !calculate_statistics(
                end_to_end_means,
                config->iterations,
                &end_to_end_stats
            ))) {
        goto cleanup;
    }
    printf(
        "Correctness: passed\n"
        "Throughput Tasks/s: min=%" PRIu64 " mean=%.3f p50=%" PRIu64
        " p95=%" PRIu64 " max=%" PRIu64 " stddev=%.3f\n"
        "Mean submission latency ns/iteration: min=%" PRIu64
        " mean=%.3f p50=%" PRIu64 " p95=%" PRIu64
        " max=%" PRIu64 "\n"
        "Shutdown ns: min=%" PRIu64 " mean=%.3f p50=%" PRIu64
        " p95=%" PRIu64 " max=%" PRIu64 "\n"
        "Join ns: min=%" PRIu64 " mean=%.3f p50=%" PRIu64
        " p95=%" PRIu64 " max=%" PRIu64 "\n"
        "CSV: %s\n",
        throughput_stats.minimum,
        throughput_stats.mean,
        throughput_stats.p50,
        throughput_stats.p95,
        throughput_stats.maximum,
        throughput_stats.standard_deviation,
        submit_stats.minimum,
        submit_stats.mean,
        submit_stats.p50,
        submit_stats.p95,
        submit_stats.maximum,
        shutdown_stats.minimum,
        shutdown_stats.mean,
        shutdown_stats.p50,
        shutdown_stats.p95,
        shutdown_stats.maximum,
        join_stats.minimum,
        join_stats.mean,
        join_stats.p50,
        join_stats.p95,
        join_stats.maximum,
        config->output
    );
    if (config->mode == MODE_VALIDATED) {
        printf(
            "Mean end-to-end latency ns/iteration: min=%" PRIu64
            " mean=%.3f p50=%" PRIu64 " p95=%" PRIu64
            " max=%" PRIu64 "\n",
            end_to_end_stats.minimum,
            end_to_end_stats.mean,
            end_to_end_stats.p50,
            end_to_end_stats.p95,
            end_to_end_stats.maximum
        );
    } else {
        puts("End-to-end latency: unavailable in low-overhead mode");
    }
    success = true;

cleanup:
    free(end_to_end_means);
    free(submit_means);
    free(joins);
    free(shutdowns);
    free(throughputs);
    return success;
}

static bool self_test(void)
{
    BenchmarkTimer timer;
    uint64_t start;
    uint64_t end;
    uint64_t duration;
    uint64_t values[] = {5U, 1U, 4U, 2U, 3U};
    Statistics statistics;
    BenchmarkConfig config = {
        "throughput",
        PROFILE_NOOP,
        MODE_VALIDATED,
        1U,
        1U,
        2U,
        8U,
        1U,
        1U,
        "self-test-unused.csv"
    };
    IterationResult result;
    size_t checked;
    CallbackContext accounting;
    Task accounting_tasks[1];
    Task unknown_task;
    unsigned int execution_counts[1] = {0U};
    uint64_t completion_ticks[1] = {0U};
    bool accounting_initialized = false;

    if (!benchmark_timer_init(&timer)
        || !benchmark_timer_now(&timer, &start)
        || !benchmark_timer_now(&timer, &end)
        || end < start
        || !benchmark_timer_duration_ns(&timer, start, end, &duration)
        || benchmark_timer_duration_ns(
            &timer,
            UINT64_C(2),
            UINT64_C(1),
            &duration
        )
        || !checked_add_size(1U, 2U, &checked)
        || checked != 3U
        || checked_add_size(SIZE_MAX, 1U, &checked)
        || !calculate_statistics(values, 5U, &statistics)
        || statistics.minimum != 1U
        || statistics.maximum != 5U
        || statistics.p50 != 3U
        || statistics.p95 != 5U
        || strchr(CSV_HEADER, '\n') != NULL
        || !apply_scenario(&config, "throughput")
        || apply_scenario(&config, "invalid")
        || !parse_size("42", &checked)
        || parse_size("0", &checked)
        || parse_size("-1", &checked)
        || parse_size("12x", &checked)) {
        fputs("Benchmark self-test failed.\n", stderr);
        return false;
    }
    if (!task_init(
            &accounting_tasks[0],
            UINT64_C(1),
            TASK_PRIORITY_NORMAL,
            UINT64_C(1)
        )
        || !task_init(
            &unknown_task,
            UINT64_C(2),
            TASK_PRIORITY_NORMAL,
            UINT64_C(1)
        )
        ) {
        fputs("Benchmark self-test failed.\n", stderr);
        return false;
    }
    config.tasks = 1U;
    if (!callback_context_init(
            &accounting,
            &config,
            accounting_tasks,
            execution_counts,
            completion_ticks
        )) {
        fputs("Benchmark self-test failed.\n", stderr);
        return false;
    }
    accounting_initialized = true;
    if (benchmark_callback(&accounting_tasks[0], &accounting) != 0
        || benchmark_callback(&accounting_tasks[0], &accounting) == 0
        || benchmark_callback(&unknown_task, &accounting) == 0
        || accounting.duplicate_count != 1U
        || accounting.unknown_count != 1U
        || accounting.execution_count != 3U) {
        fprintf(
            stderr,
            "Accounting guard details: executions=%zu duplicates=%zu "
            "unknown=%zu failure=%d.\n",
            accounting.execution_count,
            accounting.duplicate_count,
            accounting.unknown_count,
            accounting.failure ? 1 : 0
        );
        callback_context_destroy(&accounting);
        fputs("Benchmark self-test failed.\n", stderr);
        return false;
    }
    if (accounting_initialized) {
        callback_context_destroy(&accounting);
    }
    config.tasks = 8U;
    if (!run_iteration(&config, &result)
        || !result.correctness_passed
        || result.accepted != config.tasks
        || result.executed != config.tasks
        || result.rejected != 0U) {
        fprintf(
            stderr,
            "Lifecycle details: correctness=%d accepted=%zu executed=%zu "
            "rejected=%zu.\n",
            result.correctness_passed ? 1 : 0,
            result.accepted,
            result.executed,
            result.rejected
        );
        fputs("Benchmark self-test failed.\n", stderr);
        return false;
    }
    puts(
        "Benchmark self-test passed: timer, duration, checked arithmetic, "
        "configuration, percentiles, CSV schema, lifecycle, exact "
        "accounting, duplicate/unknown guards, and checksum."
    );
    return true;
}

static bool parse_arguments(
    int argument_count,
    char **arguments,
    BenchmarkConfig *config,
    bool *help_requested,
    bool *self_test_requested
)
{
    int index;

    for (index = 1; index < argument_count; index++) {
        const char *option = arguments[index];
        const char *value;

        if (strcmp(option, "--help") == 0) {
            *help_requested = true;
            continue;
        }
        if (strcmp(option, "--self-test") == 0) {
            *self_test_requested = true;
            continue;
        }
        if (index + 1 >= argument_count) {
            fprintf(stderr, "Missing value for %s.\n", option);
            return false;
        }
        value = arguments[++index];
        if (strcmp(option, "--scenario") == 0) {
            if (!apply_scenario(config, value)) {
                fprintf(stderr, "Unsupported scenario: %s\n", value);
                return false;
            }
        } else if (strcmp(option, "--workers") == 0) {
            if (!parse_size(value, &config->workers)) {
                return false;
            }
        } else if (strcmp(option, "--producers") == 0) {
            if (!parse_size(value, &config->producers)) {
                return false;
            }
        } else if (strcmp(option, "--capacity") == 0) {
            if (!parse_size(value, &config->capacity)) {
                return false;
            }
        } else if (strcmp(option, "--tasks") == 0) {
            if (!parse_size(value, &config->tasks)) {
                return false;
            }
        } else if (strcmp(option, "--warmup") == 0) {
            if (!parse_size(value, &config->warmup)) {
                return false;
            }
        } else if (strcmp(option, "--iterations") == 0) {
            if (!parse_size(value, &config->iterations)) {
                return false;
            }
        } else if (strcmp(option, "--callback-profile") == 0) {
            if (!parse_profile(value, &config->profile)) {
                fprintf(stderr, "Unsupported callback profile: %s\n", value);
                return false;
            }
        } else if (strcmp(option, "--mode") == 0) {
            if (!parse_mode(value, &config->mode)) {
                fprintf(stderr, "Unsupported mode: %s\n", value);
                return false;
            }
        } else if (strcmp(option, "--output") == 0) {
            if (value[0] == '\0') {
                return false;
            }
            config->output = value;
        } else {
            fprintf(stderr, "Unknown option: %s\n", option);
            return false;
        }
    }
    return config->tasks >= config->producers;
}

int main(int argument_count, char **arguments)
{
    BenchmarkConfig config = {
        "throughput",
        PROFILE_NOOP,
        MODE_VALIDATED,
        DEFAULT_WORKERS,
        DEFAULT_PRODUCERS,
        DEFAULT_CAPACITY,
        DEFAULT_TASKS,
        DEFAULT_WARMUP,
        DEFAULT_ITERATIONS,
        "benchmark-results.csv"
    };
    bool help_requested = false;
    bool self_test_requested = false;

    if (!parse_arguments(
            argument_count,
            arguments,
            &config,
            &help_requested,
            &self_test_requested
        )) {
        fputs("Invalid benchmark configuration. Use --help.\n", stderr);
        return EXIT_FAILURE;
    }
    if (help_requested) {
        print_help();
        return EXIT_SUCCESS;
    }
    if (self_test_requested) {
        return self_test() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    return run_benchmark(&config) ? EXIT_SUCCESS : EXIT_FAILURE;
}
