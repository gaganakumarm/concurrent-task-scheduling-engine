#include "concurrent_scheduler/concurrent_task_queue.h"

#include "sync.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SMOKE_THREAD_COUNT = 4,
    SMOKE_ITERATION_COUNT = 2000,
    PRODUCER_THREAD_COUNT = 4,
    TASKS_PER_PRODUCER = 8,
    TOTAL_PRODUCER_TASKS = PRODUCER_THREAD_COUNT * TASKS_PER_PRODUCER,
    CONTENTION_THREAD_COUNT = 8,
    CONTENTION_CAPACITY = 3,
    PRODUCER_THREAD_RESULT = 37,
    CONSUMER_THREAD_COUNT = 4,
    CONSUMER_TASK_COUNT = 32,
    CONSUMER_THREAD_RESULT = 41,
    PHASED_TASK_COUNT = 24,
    PEEK_THREAD_COUNT = 8,
    PEEK_ITERATION_COUNT = 2000,
    PEEK_THREAD_RESULT = 43,
    MIXED_THREAD_COUNT = 4,
    BLOCKING_PRODUCER_COUNT = 4,
    BLOCKING_THREAD_RESULT = 47,
    BLOCKING_CONSUMER_COUNT = 4,
    BLOCKING_CONSUMER_RESULT = 53,
    HANDOFF_TASK_COUNT = 16,
    INTEGRATION_PRODUCER_COUNT = 2,
    INTEGRATION_CONSUMER_COUNT = 2,
    INTEGRATION_TASKS_PER_PRODUCER = 8,
    INTEGRATION_TOTAL_TASKS =
        INTEGRATION_PRODUCER_COUNT * INTEGRATION_TASKS_PER_PRODUCER
};

typedef struct {
    ConcurrentTaskQueue *queue;
    size_t expected_capacity;
} QueryThreadContext;

typedef struct {
    ConcurrentTaskQueue *queue;
    Task *tasks;
    ConcurrentTaskQueueResult results[TASKS_PER_PRODUCER];
    int task_count;
    bool executed;
} ProducerContext;

typedef struct {
    SchedMutex mutex;
    Task *items[CONSUMER_TASK_COUNT];
    size_t count;
} TaskRecorder;

typedef struct {
    ConcurrentTaskQueue *queue;
    TaskRecorder *recorder;
    SchedMutex *phase_mutex;
    SchedCondition *phase_condition;
    bool *production_complete;
    ConcurrentTaskQueueResult final_result;
    size_t success_count;
    bool executed;
} ConsumerContext;

typedef struct {
    ConcurrentTaskQueue *queue;
    Task *tasks;
    size_t task_count;
    SchedMutex *phase_mutex;
    SchedCondition *phase_condition;
    bool *production_complete;
    ConcurrentTaskQueueResult operation_result;
    bool executed;
} PhasedProducerContext;

typedef struct {
    ConcurrentTaskQueue *queue;
    Task *output;
    Task *sentinel;
    ConcurrentTaskQueueResult result;
    bool executed;
} EmptyConsumerContext;

typedef struct {
    ConcurrentTaskQueue *queue;
    Task *expected;
    size_t expected_size;
    size_t expected_capacity;
    ConcurrentTaskQueueResult result;
    Task *observed;
    bool executed;
} PeekReaderContext;

typedef struct {
    ConcurrentTaskQueue *queue;
    SchedMutex *mutex;
    SchedCondition *condition;
    bool *peek_complete;
    ConcurrentTaskQueueResult result;
    Task *observed;
    bool executed;
} CoordinatedPeekContext;

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    int started;
    int completed;
} BlockingTestSync;

typedef struct {
    ConcurrentTaskQueue *queue;
    Task *task;
    BlockingTestSync *sync;
    ConcurrentTaskQueueResult result;
    bool executed;
} BlockingProducerContext;

typedef struct {
    ConcurrentTaskQueue *queue;
    BlockingTestSync *sync;
    ConcurrentTaskQueueResult result;
    Task *output;
    bool executed;
} BlockingConsumerContext;

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    int arrived;
    int participants;
    bool released;
    bool aborted;
} IntegrationBarrier;

typedef struct {
    ConcurrentTaskQueue *queue;
    Task *tasks;
    Task **outputs;
    size_t count;
    IntegrationBarrier *barrier;
    ConcurrentTaskQueueResult result;
    bool executed;
} HandoffContext;

static int query_worker(void *argument)
{
    QueryThreadContext *context = argument;
    int iteration;

    for (iteration = 0; iteration < SMOKE_ITERATION_COUNT; ++iteration) {
        if (!concurrent_task_queue_is_empty(context->queue)
            || concurrent_task_queue_is_full(context->queue)
            || concurrent_task_queue_size(context->queue) != 0U
            || concurrent_task_queue_capacity(context->queue)
                != context->expected_capacity) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

static int producer_worker(void *argument)
{
    ProducerContext *context = argument;
    int index;

    for (index = 0; index < context->task_count; ++index) {
        context->results[index] = concurrent_task_queue_try_enqueue(
            context->queue,
            &context->tasks[index]
        );
    }
    context->executed = true;
    return PRODUCER_THREAD_RESULT;
}

static int record_task(TaskRecorder *recorder, Task *task)
{
    if (sched_mutex_lock(&recorder->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    if (recorder->count >= CONSUMER_TASK_COUNT) {
        (void)sched_mutex_unlock(&recorder->mutex);
        return EXIT_FAILURE;
    }
    recorder->items[recorder->count] = task;
    ++recorder->count;
    if (sched_mutex_unlock(&recorder->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int consumer_worker(void *argument)
{
    ConsumerContext *context = argument;
    Task *task;

    if (context->phase_mutex != NULL) {
        if (sched_mutex_lock(context->phase_mutex) != SCHED_SYNC_OK) {
            return EXIT_FAILURE;
        }
        while (!*context->production_complete) {
            if (sched_condition_wait(
                    context->phase_condition,
                    context->phase_mutex
                ) != SCHED_SYNC_OK) {
                (void)sched_mutex_unlock(context->phase_mutex);
                return EXIT_FAILURE;
            }
        }
        if (sched_mutex_unlock(context->phase_mutex) != SCHED_SYNC_OK) {
            return EXIT_FAILURE;
        }
    }

    for (;;) {
        task = NULL;
        context->final_result = concurrent_task_queue_try_dequeue(
            context->queue,
            &task
        );
        if (context->final_result == CONCURRENT_TASK_QUEUE_ERROR_EMPTY) {
            break;
        }
        if (context->final_result != CONCURRENT_TASK_QUEUE_OK
            || task == NULL
            || record_task(context->recorder, task) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        ++context->success_count;
    }

    context->executed = true;
    return CONSUMER_THREAD_RESULT;
}

static int phased_producer_worker(void *argument)
{
    PhasedProducerContext *context = argument;
    size_t index;

    context->operation_result = CONCURRENT_TASK_QUEUE_OK;
    for (index = 0U; index < context->task_count; ++index) {
        context->operation_result = concurrent_task_queue_try_enqueue(
            context->queue,
            &context->tasks[index]
        );
        if (context->operation_result != CONCURRENT_TASK_QUEUE_OK) {
            return EXIT_FAILURE;
        }
    }

    if (sched_mutex_lock(context->phase_mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    *context->production_complete = true;
    if (sched_condition_broadcast(context->phase_condition) != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(context->phase_mutex);
        return EXIT_FAILURE;
    }
    if (sched_mutex_unlock(context->phase_mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }

    context->executed = true;
    return PRODUCER_THREAD_RESULT;
}

static int empty_consumer_worker(void *argument)
{
    EmptyConsumerContext *context = argument;

    context->result = concurrent_task_queue_try_dequeue(
        context->queue,
        &context->output
    );
    context->executed = true;
    return CONSUMER_THREAD_RESULT;
}

static int empty_peek_worker(void *argument)
{
    EmptyConsumerContext *context = argument;

    context->result = concurrent_task_queue_try_peek(
        context->queue,
        &context->output
    );
    context->executed = true;
    return PEEK_THREAD_RESULT;
}

static int peek_reader_worker(void *argument)
{
    PeekReaderContext *context = argument;
    int iteration;

    for (iteration = 0; iteration < PEEK_ITERATION_COUNT; ++iteration) {
        context->observed = NULL;
        context->result = concurrent_task_queue_try_peek(
            context->queue,
            &context->observed
        );
        if (context->result != CONCURRENT_TASK_QUEUE_OK
            || context->observed != context->expected) {
            return EXIT_FAILURE;
        }
    }
    context->executed = true;
    return PEEK_THREAD_RESULT;
}

static int mixed_reader_worker(void *argument)
{
    PeekReaderContext *context = argument;
    int iteration;

    for (iteration = 0; iteration < PEEK_ITERATION_COUNT; ++iteration) {
        context->observed = NULL;
        context->result = concurrent_task_queue_try_peek(
            context->queue,
            &context->observed
        );
        if (context->result != CONCURRENT_TASK_QUEUE_OK
            || context->observed != context->expected
            || concurrent_task_queue_is_empty(context->queue)
            || concurrent_task_queue_is_full(context->queue)
            || concurrent_task_queue_size(context->queue)
                != context->expected_size
            || concurrent_task_queue_capacity(context->queue)
                != context->expected_capacity) {
            return EXIT_FAILURE;
        }
    }
    context->executed = true;
    return PEEK_THREAD_RESULT;
}

static int coordinated_peek_worker(void *argument)
{
    CoordinatedPeekContext *context = argument;

    context->observed = NULL;
    context->result = concurrent_task_queue_try_peek(
        context->queue,
        &context->observed
    );
    if (sched_mutex_lock(context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    *context->peek_complete = true;
    if (sched_condition_signal(context->condition) != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(context->mutex);
        return EXIT_FAILURE;
    }
    if (sched_mutex_unlock(context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    context->executed = true;
    return PEEK_THREAD_RESULT;
}

static int coordinated_dequeue_worker(void *argument)
{
    CoordinatedPeekContext *context = argument;

    if (sched_mutex_lock(context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    while (!*context->peek_complete) {
        if (sched_condition_wait(context->condition, context->mutex)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(context->mutex);
            return EXIT_FAILURE;
        }
    }
    if (sched_mutex_unlock(context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }

    context->observed = NULL;
    context->result = concurrent_task_queue_try_dequeue(
        context->queue,
        &context->observed
    );
    context->executed = true;
    return CONSUMER_THREAD_RESULT;
}

static int blocking_producer_worker(void *argument)
{
    BlockingProducerContext *context = argument;

    if (sched_mutex_lock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    ++context->sync->started;
    if (sched_condition_broadcast(&context->sync->condition)
        != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&context->sync->mutex);
        return EXIT_FAILURE;
    }
    if (sched_mutex_unlock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }

    context->result = concurrent_task_queue_enqueue(
        context->queue,
        context->task
    );

    if (sched_mutex_lock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    ++context->sync->completed;
    if (sched_condition_broadcast(&context->sync->condition)
        != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&context->sync->mutex);
        return EXIT_FAILURE;
    }
    if (sched_mutex_unlock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }

    context->executed = true;
    return BLOCKING_THREAD_RESULT;
}

static int blocking_consumer_worker(void *argument)
{
    BlockingConsumerContext *context = argument;

    if (sched_mutex_lock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    ++context->sync->started;
    if (sched_condition_broadcast(&context->sync->condition)
        != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&context->sync->mutex);
        return EXIT_FAILURE;
    }
    if (sched_mutex_unlock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }

    context->result = concurrent_task_queue_dequeue(
        context->queue,
        &context->output
    );

    if (sched_mutex_lock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    ++context->sync->completed;
    if (sched_condition_broadcast(&context->sync->condition)
        != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&context->sync->mutex);
        return EXIT_FAILURE;
    }
    if (sched_mutex_unlock(&context->sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }

    context->executed = true;
    return BLOCKING_CONSUMER_RESULT;
}

static int integration_barrier_wait(IntegrationBarrier *barrier)
{
    if (barrier == NULL) {
        return EXIT_SUCCESS;
    }
    if (sched_mutex_lock(&barrier->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    ++barrier->arrived;
    if (barrier->arrived == barrier->participants) {
        barrier->released = true;
        if (sched_condition_broadcast(&barrier->condition)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&barrier->mutex);
            return EXIT_FAILURE;
        }
    } else {
        while (!barrier->released) {
            if (sched_condition_wait(&barrier->condition, &barrier->mutex)
                != SCHED_SYNC_OK) {
                (void)sched_mutex_unlock(&barrier->mutex);
                return EXIT_FAILURE;
            }
        }
    }
    if (sched_mutex_unlock(&barrier->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    return barrier->aborted ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int integration_barrier_abort(IntegrationBarrier *barrier)
{
    if (sched_mutex_lock(&barrier->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    barrier->aborted = true;
    barrier->released = true;
    if (sched_condition_broadcast(&barrier->condition) != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&barrier->mutex);
        return EXIT_FAILURE;
    }
    if (sched_mutex_unlock(&barrier->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int handoff_producer_worker(void *argument)
{
    HandoffContext *context = argument;
    size_t index;

    if (integration_barrier_wait(context->barrier) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    context->result = CONCURRENT_TASK_QUEUE_OK;
    for (index = 0U; index < context->count; ++index) {
        context->result = concurrent_task_queue_enqueue(
            context->queue,
            &context->tasks[index]
        );
        if (context->result != CONCURRENT_TASK_QUEUE_OK) {
            return EXIT_FAILURE;
        }
    }
    context->executed = true;
    return BLOCKING_THREAD_RESULT;
}

static int handoff_consumer_worker(void *argument)
{
    HandoffContext *context = argument;
    size_t index;

    if (integration_barrier_wait(context->barrier) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    context->result = CONCURRENT_TASK_QUEUE_OK;
    for (index = 0U; index < context->count; ++index) {
        context->result = concurrent_task_queue_dequeue(
            context->queue,
            &context->outputs[index]
        );
        if (context->result != CONCURRENT_TASK_QUEUE_OK) {
            return EXIT_FAILURE;
        }
    }
    context->executed = true;
    return BLOCKING_CONSUMER_RESULT;
}

static int test_public_api(void)
{
    ConcurrentTaskQueue queue = {0};
    ConcurrentTaskQueueResult (*init_function)(
        ConcurrentTaskQueue *,
        size_t
    ) = concurrent_task_queue_init;
    void (*destroy_function)(ConcurrentTaskQueue *) =
        concurrent_task_queue_destroy;
    bool (*empty_function)(ConcurrentTaskQueue *) =
        concurrent_task_queue_is_empty;
    bool (*full_function)(ConcurrentTaskQueue *) =
        concurrent_task_queue_is_full;
    size_t (*size_function)(ConcurrentTaskQueue *) =
        concurrent_task_queue_size;
    size_t (*capacity_function)(ConcurrentTaskQueue *) =
        concurrent_task_queue_capacity;
    const char *(*name_function)(ConcurrentTaskQueueResult) =
        concurrent_task_queue_result_name;
    ConcurrentTaskQueueResult (*enqueue_function)(
        ConcurrentTaskQueue *,
        Task *
    ) = concurrent_task_queue_try_enqueue;
    ConcurrentTaskQueueResult (*dequeue_function)(
        ConcurrentTaskQueue *,
        Task **
    ) = concurrent_task_queue_try_dequeue;
    ConcurrentTaskQueueResult (*peek_function)(
        ConcurrentTaskQueue *,
        Task **
    ) = concurrent_task_queue_try_peek;
    ConcurrentTaskQueueResult (*blocking_enqueue_function)(
        ConcurrentTaskQueue *,
        Task *
    ) = concurrent_task_queue_enqueue;
    ConcurrentTaskQueueResult (*blocking_dequeue_function)(
        ConcurrentTaskQueue *,
        Task **
    ) = concurrent_task_queue_dequeue;

    if (queue.implementation != NULL
        || init_function == NULL
        || destroy_function == NULL
        || empty_function == NULL
        || full_function == NULL
        || size_function == NULL
        || capacity_function == NULL
        || name_function == NULL
        || enqueue_function == NULL
        || dequeue_function == NULL
        || peek_function == NULL
        || blocking_enqueue_function == NULL
        || blocking_dequeue_function == NULL) {
        fprintf(stderr, "CONCURRENT-API-001 through 004 failed.\n");
        return EXIT_FAILURE;
    }

    printf("CONCURRENT-API-001 through 004 passed.\n");
    return EXIT_SUCCESS;
}

static int test_sequential_enqueue(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[5];
    Task snapshots[5];
    Task *removed = NULL;
    size_t index;

    for (index = 0U; index < 5U; ++index) {
        if (!task_init(
                &tasks[index],
                (uint64_t)(100U + index),
                TASK_PRIORITY_NORMAL,
                10U
            )) {
            fprintf(stderr, "CONCURRENT-ENQUEUE test setup failed.\n");
            return EXIT_FAILURE;
        }
    }
    memcpy(snapshots, tasks, sizeof(tasks));

    if (concurrent_task_queue_try_enqueue(NULL, &tasks[0])
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_try_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, NULL)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || queue.queue.size != 0U
        || concurrent_task_queue_try_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || queue.queue.items[0] != &tasks[0]
        || concurrent_task_queue_size(&queue) != 1U
        || concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_capacity(&queue) != 1U
        || !concurrent_task_queue_is_full(&queue)
        || concurrent_task_queue_try_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_ERROR_FULL
        || queue.queue.items[0] != &tasks[0]
        || queue.queue.size != 1U
        || queue.queue.head != 0U
        || queue.queue.tail != 0U
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(stderr, "CONCURRENT-ENQUEUE-001 through 013 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    concurrent_task_queue_destroy(&queue);

    if (concurrent_task_queue_init(&queue, 3U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[2])
            != CONCURRENT_TASK_QUEUE_OK
        || queue.queue.items[0] != &tasks[0]
        || queue.queue.items[1] != &tasks[1]
        || queue.queue.items[2] != &tasks[2]
        || concurrent_task_queue_try_dequeue(&queue, &removed)
            != CONCURRENT_TASK_QUEUE_OK
        || removed != &tasks[0]
        || concurrent_task_queue_try_dequeue(&queue, &removed)
            != CONCURRENT_TASK_QUEUE_OK
        || removed != &tasks[1]
        || concurrent_task_queue_try_enqueue(&queue, &tasks[3])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[4])
            != CONCURRENT_TASK_QUEUE_OK
        || queue.queue.items[2] != &tasks[2]
        || queue.queue.items[0] != &tasks[3]
        || queue.queue.items[1] != &tasks[4]
        || queue.queue.head != 2U
        || queue.queue.tail != 2U
        || queue.queue.size != 3U
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(stderr, "CONCURRENT-ENQUEUE-014 or 015 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    concurrent_task_queue_destroy(&queue);
    printf("CONCURRENT-ENQUEUE-001 through 015 passed.\n");
    return EXIT_SUCCESS;
}

static int initialize_tasks(Task *tasks, Task *snapshots, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (!task_init(
                &tasks[index],
                1000U + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                15U
            )) {
            return EXIT_FAILURE;
        }
    }
    memcpy(snapshots, tasks, count * sizeof(*tasks));
    return EXIT_SUCCESS;
}

static int test_sequential_dequeue(void)
{
    ConcurrentTaskQueue queue = {0};
    ConcurrentTaskQueue uninitialized = {0};
    Task tasks[5];
    Task snapshots[5];
    Task sentinel;
    Task *output = &sentinel;
    size_t index;

    if (initialize_tasks(tasks, snapshots, 5U) != EXIT_SUCCESS
        || concurrent_task_queue_try_dequeue(NULL, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != &sentinel
        || concurrent_task_queue_try_dequeue(&uninitialized, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != &sentinel
        || concurrent_task_queue_init(&queue, 3U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_dequeue(&queue, NULL)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
        || output != &sentinel) {
        fprintf(stderr, "CONCURRENT-DEQUEUE-007 through 012 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    for (index = 0U; index < 3U; ++index) {
        if (concurrent_task_queue_try_enqueue(&queue, &tasks[index])
            != CONCURRENT_TASK_QUEUE_OK) {
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    for (index = 0U; index < 3U; ++index) {
        output = NULL;
        if (concurrent_task_queue_try_dequeue(&queue, &output)
                != CONCURRENT_TASK_QUEUE_OK
            || output != &tasks[index]
            || concurrent_task_queue_size(&queue) != 2U - index
            || concurrent_task_queue_capacity(&queue) != 3U
            || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
            fprintf(stderr, "CONCURRENT-DEQUEUE-001 through 014 failed.\n");
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    if (!concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_is_full(&queue)) {
        fprintf(stderr, "CONCURRENT-DEQUEUE-005 or 006 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    for (index = 0U; index < 3U; ++index) {
        if (concurrent_task_queue_try_enqueue(&queue, &tasks[index])
            != CONCURRENT_TASK_QUEUE_OK) {
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    output = NULL;
    if (concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_try_enqueue(&queue, &tasks[3])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[4])
            != CONCURRENT_TASK_QUEUE_OK) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    for (index = 2U; index < 5U; ++index) {
        output = NULL;
        if (concurrent_task_queue_try_dequeue(&queue, &output)
                != CONCURRENT_TASK_QUEUE_OK
            || output != &tasks[index]) {
            fprintf(stderr, "CONCURRENT-DEQUEUE-015 or 016 failed.\n");
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    concurrent_task_queue_destroy(&queue);

    if (concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || !concurrent_task_queue_is_empty(&queue)
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(stderr, "CONCURRENT-DEQUEUE-017 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    concurrent_task_queue_destroy(&queue);
    printf("CONCURRENT-DEQUEUE-001 through 017 passed.\n");
    return EXIT_SUCCESS;
}

static int test_sequential_peek(void)
{
    ConcurrentTaskQueue queue = {0};
    ConcurrentTaskQueue uninitialized = {0};
    Task tasks[5];
    Task snapshots[5];
    Task sentinel;
    Task *output = &sentinel;
    Task *item_snapshot[3];
    size_t size_snapshot;
    size_t capacity_snapshot;
    size_t head_snapshot;
    size_t tail_snapshot;
    size_t index;

    if (initialize_tasks(tasks, snapshots, 5U) != EXIT_SUCCESS
        || concurrent_task_queue_try_peek(NULL, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != &sentinel
        || concurrent_task_queue_try_peek(&uninitialized, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != &sentinel
        || concurrent_task_queue_init(&queue, 3U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_peek(&queue, NULL)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
        || output != &sentinel) {
        fprintf(stderr, "CONCURRENT-PEEK-010 through 015 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    for (index = 0U; index < 3U; ++index) {
        if (concurrent_task_queue_try_enqueue(&queue, &tasks[index])
            != CONCURRENT_TASK_QUEUE_OK) {
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    memcpy(item_snapshot, queue.queue.items, sizeof(item_snapshot));
    size_snapshot = queue.queue.size;
    capacity_snapshot = queue.queue.capacity;
    head_snapshot = queue.queue.head;
    tail_snapshot = queue.queue.tail;

    output = NULL;
    if (concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || queue.queue.size != size_snapshot
        || queue.queue.capacity != capacity_snapshot
        || queue.queue.head != head_snapshot
        || queue.queue.tail != tail_snapshot
        || memcmp(
            queue.queue.items,
            item_snapshot,
            sizeof(item_snapshot)
        ) != 0
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(stderr, "CONCURRENT-PEEK-001 through 016 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_try_enqueue(&queue, &tasks[3])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[4])
            != CONCURRENT_TASK_QUEUE_OK) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    size_snapshot = queue.queue.size;
    head_snapshot = queue.queue.head;
    tail_snapshot = queue.queue.tail;
    output = NULL;
    if (concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[2]
        || queue.queue.size != size_snapshot
        || queue.queue.head != head_snapshot
        || queue.queue.tail != tail_snapshot) {
        fprintf(stderr, "CONCURRENT-PEEK-017 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    for (index = 2U; index < 5U; ++index) {
        if (concurrent_task_queue_try_dequeue(&queue, &output)
                != CONCURRENT_TASK_QUEUE_OK
            || output != &tasks[index]) {
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    concurrent_task_queue_destroy(&queue);

    if (concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(stderr, "CONCURRENT-PEEK-018 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    concurrent_task_queue_destroy(&queue);
    printf("CONCURRENT-PEEK-001 through 018 passed.\n");
    return EXIT_SUCCESS;
}

static int test_blocking_enqueue_immediate(void)
{
    ConcurrentTaskQueue queue = {0};
    ConcurrentTaskQueue uninitialized = {0};
    Task tasks[3];
    Task snapshots[3];
    Task *output = NULL;

    if (initialize_tasks(tasks, snapshots, 3U) != EXIT_SUCCESS
        || concurrent_task_queue_enqueue(NULL, &tasks[0])
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_enqueue(&uninitialized, &tasks[0])
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_init(&queue, 3U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_enqueue(&queue, NULL)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_size(&queue) != 0U
        || concurrent_task_queue_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || queue.queue.items[0] != &tasks[0]
        || concurrent_task_queue_size(&queue) != 1U
        || concurrent_task_queue_capacity(&queue) != 3U
        || concurrent_task_queue_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_enqueue(&queue, &tasks[2])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[2]
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(
            stderr,
            "CONCURRENT-BLOCKING-ENQUEUE-001 through 013 failed.\n"
        );
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    concurrent_task_queue_destroy(&queue);

    if (concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || !concurrent_task_queue_is_full(&queue)) {
        fprintf(stderr, "CONCURRENT-BLOCKING-ENQUEUE-005 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    concurrent_task_queue_destroy(&queue);
    printf("CONCURRENT-BLOCKING-ENQUEUE immediate tests passed.\n");
    return EXIT_SUCCESS;
}

static int test_blocking_dequeue_immediate(void)
{
    ConcurrentTaskQueue queue = {0};
    ConcurrentTaskQueue uninitialized = {0};
    Task tasks[3];
    Task snapshots[3];
    Task sentinel;
    Task *output = &sentinel;
    size_t index;

    if (initialize_tasks(tasks, snapshots, 3U) != EXIT_SUCCESS
        || concurrent_task_queue_dequeue(NULL, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != &sentinel
        || concurrent_task_queue_dequeue(&uninitialized, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != &sentinel
        || concurrent_task_queue_init(&queue, 3U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_dequeue(&queue, NULL)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_size(&queue) != 0U) {
        fprintf(stderr, "CONCURRENT-BLOCKING-DEQUEUE validation failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    for (index = 0U; index < 3U; ++index) {
        if (concurrent_task_queue_enqueue(&queue, &tasks[index])
            != CONCURRENT_TASK_QUEUE_OK) {
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    for (index = 0U; index < 3U; ++index) {
        output = NULL;
        if (concurrent_task_queue_dequeue(&queue, &output)
                != CONCURRENT_TASK_QUEUE_OK
            || output != &tasks[index]
            || concurrent_task_queue_size(&queue) != 2U - index
            || concurrent_task_queue_capacity(&queue) != 3U
            || concurrent_task_queue_is_full(&queue)
            || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
            fprintf(
                stderr,
                "CONCURRENT-BLOCKING-DEQUEUE-001 through 014 failed.\n"
            );
            concurrent_task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }
    if (!concurrent_task_queue_is_empty(&queue)) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    concurrent_task_queue_destroy(&queue);
    printf("CONCURRENT-BLOCKING-DEQUEUE immediate tests passed.\n");
    return EXIT_SUCCESS;
}

static int test_initialization(void)
{
    ConcurrentTaskQueue queue = {0};
    ConcurrentTaskQueue preserved = {
        .queue = {
            .items = (Task **)(uintptr_t)1U,
            .capacity = 11U,
            .size = 7U,
            .head = 3U,
            .tail = 5U
        },
        .implementation = (void *)(uintptr_t)2U
    };
    ConcurrentTaskQueue snapshot = preserved;

    if (concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || queue.queue.items == NULL
        || queue.queue.capacity != 1U
        || queue.queue.size != 0U
        || queue.queue.head != 0U
        || queue.queue.tail != 0U
        || queue.implementation == NULL) {
        fprintf(stderr, "CONCURRENT-INIT-001 through 005 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    concurrent_task_queue_destroy(&queue);

    if (concurrent_task_queue_init(NULL, 1U)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || concurrent_task_queue_init(&preserved, 0U)
            != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || memcmp(&preserved, &snapshot, sizeof(preserved)) != 0
        || concurrent_task_queue_init(
                &preserved,
                SIZE_MAX / sizeof(Task *) + 1U
            ) != CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || memcmp(&preserved, &snapshot, sizeof(preserved)) != 0) {
        fprintf(stderr, "CONCURRENT-INIT-006 through 010 failed.\n");
        return EXIT_FAILURE;
    }

    printf("CONCURRENT-INIT-001 through 010 passed.\n");
    return EXIT_SUCCESS;
}

static int test_destruction_and_queries(void)
{
    ConcurrentTaskQueue queue = {0};
    ConcurrentTaskQueue zero_queue = {0};
    Task task = {
        .id = 91U,
        .priority = TASK_PRIORITY_HIGH,
        .state = TASK_STATE_CREATED,
        .total_work = 12U,
        .remaining_work = 12U
    };
    Task task_snapshot = task;
    Task **items;

    concurrent_task_queue_destroy(NULL);
    concurrent_task_queue_destroy(&zero_queue);
    concurrent_task_queue_destroy(&zero_queue);

    if (concurrent_task_queue_init(&queue, 3U)
            != CONCURRENT_TASK_QUEUE_OK
        || !concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_is_full(&queue)
        || concurrent_task_queue_size(&queue) != 0U
        || concurrent_task_queue_capacity(&queue) != 3U) {
        fprintf(stderr, "CONCURRENT-QUERY-001 through 004 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    items = queue.queue.items;
    if (concurrent_task_queue_is_empty(&queue) != true
        || queue.queue.items != items
        || queue.queue.capacity != 3U
        || queue.queue.size != 0U
        || queue.queue.head != 0U
        || queue.queue.tail != 0U) {
        fprintf(stderr, "CONCURRENT-QUERY-007 failed.\n");
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    queue.queue.items[0] = &task;
    queue.queue.size = 1U;
    concurrent_task_queue_destroy(&queue);

    if (queue.queue.items != NULL
        || queue.queue.capacity != 0U
        || queue.queue.size != 0U
        || queue.queue.head != 0U
        || queue.queue.tail != 0U
        || queue.implementation != NULL
        || memcmp(&task, &task_snapshot, sizeof(task)) != 0) {
        fprintf(stderr, "CONCURRENT-DESTROY-001, 002, or 006 failed.\n");
        return EXIT_FAILURE;
    }

    concurrent_task_queue_destroy(&queue);
    if (!concurrent_task_queue_is_empty(NULL)
        || concurrent_task_queue_is_full(NULL)
        || concurrent_task_queue_size(NULL) != 0U
        || concurrent_task_queue_capacity(NULL) != 0U
        || !concurrent_task_queue_is_empty(&zero_queue)
        || concurrent_task_queue_is_full(&zero_queue)
        || concurrent_task_queue_size(&zero_queue) != 0U
        || concurrent_task_queue_capacity(&zero_queue) != 0U) {
        fprintf(stderr, "CONCURRENT-QUERY-005 or 006 failed.\n");
        return EXIT_FAILURE;
    }

    printf(
        "CONCURRENT-DESTROY-001 through 006 and "
        "CONCURRENT-QUERY-001 through 007 passed.\n"
    );
    return EXIT_SUCCESS;
}

static int test_result_names(void)
{
    static const struct {
        ConcurrentTaskQueueResult result;
        const char *name;
    } cases[] = {
        {CONCURRENT_TASK_QUEUE_OK, "OK"},
        {CONCURRENT_TASK_QUEUE_ERROR_INVALID_ARGUMENT, "INVALID_ARGUMENT"},
        {CONCURRENT_TASK_QUEUE_ERROR_ALLOCATION, "ALLOCATION_ERROR"},
        {CONCURRENT_TASK_QUEUE_ERROR_SYSTEM, "SYSTEM_ERROR"},
        {CONCURRENT_TASK_QUEUE_ERROR_FULL, "FULL"},
        {CONCURRENT_TASK_QUEUE_ERROR_EMPTY, "EMPTY"},
        {CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN, "SHUTDOWN"}
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (strcmp(
                concurrent_task_queue_result_name(cases[index].result),
                cases[index].name
            ) != 0) {
            fprintf(stderr, "CONCURRENT-RESULT-001 failed.\n");
            return EXIT_FAILURE;
        }
    }

    if (strcmp(
            concurrent_task_queue_result_name(
                (ConcurrentTaskQueueResult)-1
            ),
            "UNKNOWN"
        ) != 0
        || strcmp(
            concurrent_task_queue_result_name(
                (ConcurrentTaskQueueResult)1000
            ),
            "UNKNOWN"
        ) != 0) {
        fprintf(stderr, "CONCURRENT-RESULT-002 or 003 failed.\n");
        return EXIT_FAILURE;
    }

    printf("CONCURRENT-RESULT-001 through 003 passed.\n");
    return EXIT_SUCCESS;
}

static int test_query_concurrency(void)
{
    ConcurrentTaskQueue queue = {0};
    QueryThreadContext context;
    SchedThread threads[SMOKE_THREAD_COUNT] = {0};
    int created = 0;
    int index;
    int result;
    int status = EXIT_FAILURE;

    if (concurrent_task_queue_init(&queue, 8U)
        != CONCURRENT_TASK_QUEUE_OK) {
        fprintf(stderr, "CONCURRENT-THREAD-001 initialization failed.\n");
        return EXIT_FAILURE;
    }

    context.queue = &queue;
    context.expected_capacity = 8U;
    for (index = 0; index < SMOKE_THREAD_COUNT; ++index) {
        if (sched_thread_create(&threads[index], query_worker, &context)
            != SCHED_SYNC_OK) {
            fprintf(stderr, "CONCURRENT-THREAD-001 creation failed.\n");
            break;
        }
        ++created;
    }

    for (index = 0; index < created; ++index) {
        result = EXIT_FAILURE;
        if (sched_thread_join(&threads[index], &result) != SCHED_SYNC_OK
            || result != EXIT_SUCCESS) {
            fprintf(stderr, "CONCURRENT-THREAD-001 join failed.\n");
            goto cleanup;
        }
    }

    if (created != SMOKE_THREAD_COUNT
        || !concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_is_full(&queue)
        || concurrent_task_queue_size(&queue) != 0U
        || concurrent_task_queue_capacity(&queue) != 8U) {
        fprintf(stderr, "CONCURRENT-THREAD-001 state check failed.\n");
        goto cleanup;
    }

    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    concurrent_task_queue_destroy(&queue);

    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-THREAD-001 passed.\n");
    }
    return status;
}

static int pointer_occurrences(
    const ConcurrentTaskQueue *queue,
    const Task *task
)
{
    size_t index;
    int occurrences = 0;

    for (index = 0U; index < queue->queue.capacity; ++index) {
        if (queue->queue.items[index] == task) {
            ++occurrences;
        }
    }
    return occurrences;
}

static int test_multiple_producers(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[PRODUCER_THREAD_COUNT][TASKS_PER_PRODUCER];
    Task snapshots[PRODUCER_THREAD_COUNT][TASKS_PER_PRODUCER];
    ProducerContext contexts[PRODUCER_THREAD_COUNT] = {0};
    SchedThread threads[PRODUCER_THREAD_COUNT] = {0};
    int created = 0;
    int producer;
    int task_index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (concurrent_task_queue_init(&queue, TOTAL_PRODUCER_TASKS)
        != CONCURRENT_TASK_QUEUE_OK) {
        return EXIT_FAILURE;
    }

    for (producer = 0; producer < PRODUCER_THREAD_COUNT; ++producer) {
        for (task_index = 0; task_index < TASKS_PER_PRODUCER; ++task_index) {
            if (!task_init(
                    &tasks[producer][task_index],
                    (uint64_t)(producer * 100 + task_index),
                    TASK_PRIORITY_NORMAL,
                    20U
                )) {
                goto cleanup;
            }
        }
        memcpy(snapshots[producer], tasks[producer], sizeof(tasks[producer]));
        contexts[producer].queue = &queue;
        contexts[producer].tasks = tasks[producer];
        contexts[producer].task_count = TASKS_PER_PRODUCER;
        if (sched_thread_create(
                &threads[producer],
                producer_worker,
                &contexts[producer]
            ) != SCHED_SYNC_OK) {
            goto join_threads;
        }
        ++created;
    }

join_threads:
    for (producer = 0; producer < created; ++producer) {
        thread_result = 0;
        if (sched_thread_join(&threads[producer], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != PRODUCER_THREAD_RESULT) {
            goto cleanup;
        }
    }
    if (created != PRODUCER_THREAD_COUNT
        || concurrent_task_queue_size(&queue) != TOTAL_PRODUCER_TASKS
        || concurrent_task_queue_capacity(&queue) != TOTAL_PRODUCER_TASKS) {
        goto cleanup;
    }

    for (producer = 0; producer < PRODUCER_THREAD_COUNT; ++producer) {
        if (!contexts[producer].executed
            || memcmp(
                tasks[producer],
                snapshots[producer],
                sizeof(tasks[producer])
            ) != 0) {
            goto cleanup;
        }
        for (task_index = 0; task_index < TASKS_PER_PRODUCER; ++task_index) {
            if (contexts[producer].results[task_index]
                    != CONCURRENT_TASK_QUEUE_OK
                || pointer_occurrences(
                    &queue,
                    &tasks[producer][task_index]
                ) != 1) {
                goto cleanup;
            }
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    for (producer = 0; producer < created; ++producer) {
        sched_thread_destroy(&threads[producer]);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-PRODUCER-001 passed.\n");
    }
    return status;
}

static int test_full_contention(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[CONTENTION_THREAD_COUNT];
    Task snapshots[CONTENTION_THREAD_COUNT];
    ProducerContext contexts[CONTENTION_THREAD_COUNT] = {0};
    SchedThread threads[CONTENTION_THREAD_COUNT] = {0};
    int created = 0;
    int index;
    int thread_result;
    int success_count = 0;
    int full_count = 0;
    int status = EXIT_FAILURE;

    if (concurrent_task_queue_init(&queue, CONTENTION_CAPACITY)
        != CONCURRENT_TASK_QUEUE_OK) {
        return EXIT_FAILURE;
    }

    for (index = 0; index < CONTENTION_THREAD_COUNT; ++index) {
        if (!task_init(
                &tasks[index],
                500U + (uint64_t)index,
                TASK_PRIORITY_HIGH,
                5U
            )) {
            goto cleanup;
        }
        snapshots[index] = tasks[index];
        contexts[index].queue = &queue;
        contexts[index].tasks = &tasks[index];
        contexts[index].task_count = 1;
        if (sched_thread_create(
                &threads[index],
                producer_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            goto join_threads;
        }
        ++created;
    }

join_threads:
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != PRODUCER_THREAD_RESULT) {
            goto cleanup;
        }
    }
    if (created != CONTENTION_THREAD_COUNT
        || concurrent_task_queue_size(&queue) != CONTENTION_CAPACITY) {
        goto cleanup;
    }

    for (index = 0; index < CONTENTION_THREAD_COUNT; ++index) {
        if (!contexts[index].executed
            || memcmp(&tasks[index], &snapshots[index], sizeof(tasks[index]))
                != 0) {
            goto cleanup;
        }
        if (contexts[index].results[0] == CONCURRENT_TASK_QUEUE_OK) {
            ++success_count;
            if (pointer_occurrences(&queue, &tasks[index]) != 1) {
                goto cleanup;
            }
        } else if (contexts[index].results[0]
            == CONCURRENT_TASK_QUEUE_ERROR_FULL) {
            ++full_count;
            if (pointer_occurrences(&queue, &tasks[index]) != 0) {
                goto cleanup;
            }
        } else {
            goto cleanup;
        }
    }

    if (success_count != CONTENTION_CAPACITY
        || full_count != CONTENTION_THREAD_COUNT - CONTENTION_CAPACITY) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-CONTENTION-001 passed.\n");
    }
    return status;
}

static int recorder_occurrences(const TaskRecorder *recorder, const Task *task)
{
    size_t index;
    int occurrences = 0;

    for (index = 0U; index < recorder->count; ++index) {
        if (recorder->items[index] == task) {
            ++occurrences;
        }
    }
    return occurrences;
}

static int test_multiple_consumers(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[CONSUMER_TASK_COUNT];
    Task snapshots[CONSUMER_TASK_COUNT];
    TaskRecorder recorder = {{0}, {0}, 0U};
    ConsumerContext contexts[CONSUMER_THREAD_COUNT] = {0};
    SchedThread threads[CONSUMER_THREAD_COUNT] = {0};
    int created = 0;
    int index;
    int thread_result;
    size_t total_success = 0U;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, CONSUMER_TASK_COUNT)
            != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, CONSUMER_TASK_COUNT)
            != CONCURRENT_TASK_QUEUE_OK
        || sched_mutex_init(&recorder.mutex) != SCHED_SYNC_OK) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    for (index = 0; index < CONSUMER_TASK_COUNT; ++index) {
        if (concurrent_task_queue_try_enqueue(&queue, &tasks[index])
            != CONCURRENT_TASK_QUEUE_OK) {
            goto cleanup;
        }
    }

    for (index = 0; index < CONSUMER_THREAD_COUNT; ++index) {
        contexts[index].queue = &queue;
        contexts[index].recorder = &recorder;
        if (sched_thread_create(
                &threads[index],
                consumer_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            break;
        }
        ++created;
    }
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != CONSUMER_THREAD_RESULT) {
            goto cleanup;
        }
    }
    if (created != CONSUMER_THREAD_COUNT
        || recorder.count != CONSUMER_TASK_COUNT
        || concurrent_task_queue_size(&queue) != 0U
        || !concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_capacity(&queue) != CONSUMER_TASK_COUNT) {
        goto cleanup;
    }
    for (index = 0; index < CONSUMER_THREAD_COUNT; ++index) {
        if (!contexts[index].executed
            || contexts[index].final_result
                != CONCURRENT_TASK_QUEUE_ERROR_EMPTY) {
            goto cleanup;
        }
        total_success += contexts[index].success_count;
    }
    if (total_success != CONSUMER_TASK_COUNT) {
        goto cleanup;
    }
    for (index = 0; index < CONSUMER_TASK_COUNT; ++index) {
        if (recorder_occurrences(&recorder, &tasks[index]) != 1
            || memcmp(&tasks[index], &snapshots[index], sizeof(tasks[index]))
                != 0) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    sched_mutex_destroy(&recorder.mutex);
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-CONSUMER-001 passed.\n");
    }
    return status;
}

static int test_empty_contention(void)
{
    ConcurrentTaskQueue queue = {0};
    Task sentinels[CONSUMER_THREAD_COUNT];
    Task snapshots[CONSUMER_THREAD_COUNT];
    EmptyConsumerContext contexts[CONSUMER_THREAD_COUNT] = {0};
    SchedThread threads[CONSUMER_THREAD_COUNT] = {0};
    int created = 0;
    int index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (initialize_tasks(sentinels, snapshots, CONSUMER_THREAD_COUNT)
            != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, CONSUMER_THREAD_COUNT)
            != CONCURRENT_TASK_QUEUE_OK) {
        return EXIT_FAILURE;
    }
    for (index = 0; index < CONSUMER_THREAD_COUNT; ++index) {
        contexts[index].queue = &queue;
        contexts[index].sentinel = &sentinels[index];
        contexts[index].output = contexts[index].sentinel;
        if (sched_thread_create(
                &threads[index],
                empty_consumer_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            break;
        }
        ++created;
    }
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != CONSUMER_THREAD_RESULT) {
            goto cleanup;
        }
    }
    if (created != CONSUMER_THREAD_COUNT
        || concurrent_task_queue_size(&queue) != 0U
        || !concurrent_task_queue_is_empty(&queue)) {
        goto cleanup;
    }
    for (index = 0; index < CONSUMER_THREAD_COUNT; ++index) {
        if (!contexts[index].executed
            || contexts[index].result
                != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
            || contexts[index].output != contexts[index].sentinel
            || memcmp(
                &sentinels[index],
                &snapshots[index],
                sizeof(sentinels[index])
            ) != 0) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-EMPTY-CONTENTION-001 passed.\n");
    }
    return status;
}

static int test_phased_producer_consumer(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[PHASED_TASK_COUNT];
    Task snapshots[PHASED_TASK_COUNT];
    TaskRecorder recorder = {{0}, {0}, 0U};
    SchedMutex phase_mutex = {0};
    SchedCondition phase_condition = {0};
    bool production_complete = false;
    PhasedProducerContext producer_context = {0};
    ConsumerContext consumer_contexts[CONSUMER_THREAD_COUNT] = {0};
    SchedThread producer_thread = {0};
    SchedThread consumer_threads[CONSUMER_THREAD_COUNT] = {0};
    bool producer_created = false;
    int consumers_created = 0;
    int index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, PHASED_TASK_COUNT)
            != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, PHASED_TASK_COUNT)
            != CONCURRENT_TASK_QUEUE_OK
        || sched_mutex_init(&recorder.mutex) != SCHED_SYNC_OK
        || sched_mutex_init(&phase_mutex) != SCHED_SYNC_OK
        || sched_condition_init(&phase_condition) != SCHED_SYNC_OK) {
        goto cleanup;
    }

    producer_context.queue = &queue;
    producer_context.tasks = tasks;
    producer_context.task_count = PHASED_TASK_COUNT;
    producer_context.phase_mutex = &phase_mutex;
    producer_context.phase_condition = &phase_condition;
    producer_context.production_complete = &production_complete;
    if (sched_thread_create(
            &producer_thread,
            phased_producer_worker,
            &producer_context
        ) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    producer_created = true;

    for (index = 0; index < CONSUMER_THREAD_COUNT; ++index) {
        consumer_contexts[index].queue = &queue;
        consumer_contexts[index].recorder = &recorder;
        consumer_contexts[index].phase_mutex = &phase_mutex;
        consumer_contexts[index].phase_condition = &phase_condition;
        consumer_contexts[index].production_complete =
            &production_complete;
        if (sched_thread_create(
                &consumer_threads[index],
                consumer_worker,
                &consumer_contexts[index]
            ) != SCHED_SYNC_OK) {
            break;
        }
        ++consumers_created;
    }

    thread_result = 0;
    if (sched_thread_join(&producer_thread, &thread_result) != SCHED_SYNC_OK
        || thread_result != PRODUCER_THREAD_RESULT
        || !producer_context.executed
        || producer_context.operation_result != CONCURRENT_TASK_QUEUE_OK) {
        goto cleanup;
    }
    for (index = 0; index < consumers_created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&consumer_threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != CONSUMER_THREAD_RESULT) {
            goto cleanup;
        }
    }
    if (consumers_created != CONSUMER_THREAD_COUNT
        || recorder.count != PHASED_TASK_COUNT
        || concurrent_task_queue_size(&queue) != 0U
        || !concurrent_task_queue_is_empty(&queue)) {
        goto cleanup;
    }
    for (index = 0; index < PHASED_TASK_COUNT; ++index) {
        if (recorder_occurrences(&recorder, &tasks[index]) != 1
            || memcmp(&tasks[index], &snapshots[index], sizeof(tasks[index]))
                != 0) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    if (producer_created) {
        sched_thread_destroy(&producer_thread);
    }
    for (index = 0; index < consumers_created; ++index) {
        sched_thread_destroy(&consumer_threads[index]);
    }
    sched_condition_destroy(&phase_condition);
    sched_mutex_destroy(&phase_mutex);
    sched_mutex_destroy(&recorder.mutex);
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-PHASED-001 passed.\n");
    }
    return status;
}

static int test_concurrent_peek_readers(void)
{
    ConcurrentTaskQueue queue = {0};
    Task task;
    Task snapshot;
    PeekReaderContext contexts[PEEK_THREAD_COUNT] = {0};
    SchedThread threads[PEEK_THREAD_COUNT] = {0};
    Task *item_snapshot[4];
    Task **items;
    size_t size;
    size_t capacity;
    size_t head;
    size_t tail;
    int created = 0;
    int index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (!task_init(&task, 2000U, TASK_PRIORITY_HIGH, 9U)
        || concurrent_task_queue_init(&queue, 4U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &task)
            != CONCURRENT_TASK_QUEUE_OK) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    snapshot = task;
    items = queue.queue.items;
    memcpy(item_snapshot, queue.queue.items, sizeof(item_snapshot));
    size = queue.queue.size;
    capacity = queue.queue.capacity;
    head = queue.queue.head;
    tail = queue.queue.tail;

    for (index = 0; index < PEEK_THREAD_COUNT; ++index) {
        contexts[index].queue = &queue;
        contexts[index].expected = &task;
        if (sched_thread_create(
                &threads[index],
                peek_reader_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            break;
        }
        ++created;
    }
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != PEEK_THREAD_RESULT
            || !contexts[index].executed) {
            goto cleanup;
        }
    }
    if (created != PEEK_THREAD_COUNT
        || queue.queue.items != items
        || memcmp(
            queue.queue.items,
            item_snapshot,
            sizeof(item_snapshot)
        ) != 0
        || queue.queue.size != size
        || queue.queue.capacity != capacity
        || queue.queue.head != head
        || queue.queue.tail != tail
        || memcmp(&task, &snapshot, sizeof(task)) != 0
        || concurrent_task_queue_is_empty(&queue)) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-PEEK-READERS-001 passed.\n");
    }
    return status;
}

static int test_mixed_peek_queries(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[3];
    Task snapshots[3];
    PeekReaderContext contexts[MIXED_THREAD_COUNT] = {0};
    SchedThread threads[MIXED_THREAD_COUNT] = {0};
    Task *item_snapshot[5];
    size_t size;
    size_t capacity;
    size_t head;
    size_t tail;
    int created = 0;
    int index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, 3U) != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 5U)
            != CONCURRENT_TASK_QUEUE_OK) {
        return EXIT_FAILURE;
    }
    for (index = 0; index < 3; ++index) {
        if (concurrent_task_queue_try_enqueue(&queue, &tasks[index])
            != CONCURRENT_TASK_QUEUE_OK) {
            goto cleanup;
        }
    }
    memcpy(item_snapshot, queue.queue.items, sizeof(item_snapshot));
    size = queue.queue.size;
    capacity = queue.queue.capacity;
    head = queue.queue.head;
    tail = queue.queue.tail;

    for (index = 0; index < MIXED_THREAD_COUNT; ++index) {
        contexts[index].queue = &queue;
        contexts[index].expected = &tasks[0];
        contexts[index].expected_size = size;
        contexts[index].expected_capacity = capacity;
        if (sched_thread_create(
                &threads[index],
                mixed_reader_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            break;
        }
        ++created;
    }
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != PEEK_THREAD_RESULT
            || !contexts[index].executed) {
            goto cleanup;
        }
    }
    if (created != MIXED_THREAD_COUNT
        || queue.queue.size != size
        || queue.queue.capacity != capacity
        || queue.queue.head != head
        || queue.queue.tail != tail
        || memcmp(
            queue.queue.items,
            item_snapshot,
            sizeof(item_snapshot)
        ) != 0
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-PEEK-MIXED-001 passed.\n");
    }
    return status;
}

static int test_coordinated_peek_dequeue(void)
{
    ConcurrentTaskQueue queue = {0};
    Task task;
    Task snapshot;
    SchedMutex mutex = {0};
    SchedCondition condition = {0};
    bool peek_complete = false;
    CoordinatedPeekContext reader = {0};
    CoordinatedPeekContext consumer = {0};
    SchedThread reader_thread = {0};
    SchedThread consumer_thread = {0};
    bool reader_created = false;
    bool consumer_created = false;
    int thread_result;
    int status = EXIT_FAILURE;

    if (!task_init(&task, 3000U, TASK_PRIORITY_NORMAL, 7U)
        || concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &task)
            != CONCURRENT_TASK_QUEUE_OK
        || sched_mutex_init(&mutex) != SCHED_SYNC_OK
        || sched_condition_init(&condition) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    snapshot = task;
    reader.queue = &queue;
    reader.mutex = &mutex;
    reader.condition = &condition;
    reader.peek_complete = &peek_complete;
    consumer = reader;

    if (sched_thread_create(
            &consumer_thread,
            coordinated_dequeue_worker,
            &consumer
        ) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    consumer_created = true;
    if (sched_thread_create(
            &reader_thread,
            coordinated_peek_worker,
            &reader
        ) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    reader_created = true;

    thread_result = 0;
    if (sched_thread_join(&reader_thread, &thread_result) != SCHED_SYNC_OK
        || thread_result != PEEK_THREAD_RESULT) {
        goto cleanup;
    }
    thread_result = 0;
    if (sched_thread_join(&consumer_thread, &thread_result) != SCHED_SYNC_OK
        || thread_result != CONSUMER_THREAD_RESULT
        || !reader.executed
        || !consumer.executed
        || reader.result != CONCURRENT_TASK_QUEUE_OK
        || consumer.result != CONCURRENT_TASK_QUEUE_OK
        || reader.observed != &task
        || consumer.observed != &task
        || !concurrent_task_queue_is_empty(&queue)
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (reader_created) {
        sched_thread_destroy(&reader_thread);
    }
    if (consumer_created) {
        sched_thread_destroy(&consumer_thread);
    }
    sched_condition_destroy(&condition);
    sched_mutex_destroy(&mutex);
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-PEEK-DEQUEUE-001 passed.\n");
    }
    return status;
}

static int test_empty_peek_contention(void)
{
    ConcurrentTaskQueue queue = {0};
    Task sentinels[PEEK_THREAD_COUNT];
    Task snapshots[PEEK_THREAD_COUNT];
    EmptyConsumerContext contexts[PEEK_THREAD_COUNT] = {0};
    SchedThread threads[PEEK_THREAD_COUNT] = {0};
    int created = 0;
    int index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (initialize_tasks(sentinels, snapshots, PEEK_THREAD_COUNT)
            != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, PEEK_THREAD_COUNT)
            != CONCURRENT_TASK_QUEUE_OK) {
        return EXIT_FAILURE;
    }
    for (index = 0; index < PEEK_THREAD_COUNT; ++index) {
        contexts[index].queue = &queue;
        contexts[index].sentinel = &sentinels[index];
        contexts[index].output = contexts[index].sentinel;
        if (sched_thread_create(
                &threads[index],
                empty_peek_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            break;
        }
        ++created;
    }
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != PEEK_THREAD_RESULT) {
            goto cleanup;
        }
    }
    if (created != PEEK_THREAD_COUNT
        || concurrent_task_queue_size(&queue) != 0U
        || !concurrent_task_queue_is_empty(&queue)) {
        goto cleanup;
    }
    for (index = 0; index < PEEK_THREAD_COUNT; ++index) {
        if (!contexts[index].executed
            || contexts[index].result
                != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
            || contexts[index].output != contexts[index].sentinel
            || memcmp(
                &sentinels[index],
                &snapshots[index],
                sizeof(sentinels[index])
            ) != 0) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-PEEK-EMPTY-001 passed.\n");
    }
    return status;
}

static int blocking_sync_init(BlockingTestSync *sync)
{
    if (sched_mutex_init(&sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    if (sched_condition_init(&sync->condition) != SCHED_SYNC_OK) {
        sched_mutex_destroy(&sync->mutex);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static void blocking_sync_destroy(BlockingTestSync *sync)
{
    sched_condition_destroy(&sync->condition);
    sched_mutex_destroy(&sync->mutex);
}

static int blocking_sync_wait_for(
    BlockingTestSync *sync,
    int *value,
    int target
)
{
    if (sched_mutex_lock(&sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    while (*value < target) {
        if (sched_condition_wait(&sync->condition, &sync->mutex)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&sync->mutex);
            return EXIT_FAILURE;
        }
    }
    if (sched_mutex_unlock(&sync->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int test_capacity_one_blocked_producer(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[2];
    Task snapshots[2];
    BlockingTestSync sync = {{0}, {0}, 0, 0};
    BlockingProducerContext context = {0};
    SchedThread thread = {0};
    bool sync_initialized = false;
    bool thread_created = false;
    Task *output = NULL;
    int thread_result = 0;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, 2U) != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_init(&sync) != EXIT_SUCCESS) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    sync_initialized = true;
    if (concurrent_task_queue_try_enqueue(&queue, &tasks[0])
        != CONCURRENT_TASK_QUEUE_OK) {
        goto cleanup;
    }

    context.queue = &queue;
    context.task = &tasks[1];
    context.sync = &sync;
    if (sched_thread_create(&thread, blocking_producer_worker, &context)
        != SCHED_SYNC_OK) {
        goto cleanup;
    }
    thread_created = true;

    if (blocking_sync_wait_for(&sync, &sync.started, 1) != EXIT_SUCCESS
        || sched_mutex_lock(&sync.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    if (sync.completed != 0) {
        (void)sched_mutex_unlock(&sync.mutex);
        goto cleanup;
    }
    if (sched_mutex_unlock(&sync.mutex) != SCHED_SYNC_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_ERROR_FULL
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || blocking_sync_wait_for(&sync, &sync.completed, 1)
            != EXIT_SUCCESS
        || sched_thread_join(&thread, &thread_result) != SCHED_SYNC_OK
        || thread_result != BLOCKING_THREAD_RESULT
        || !context.executed
        || context.result != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_size(&queue) != 1U
        || !concurrent_task_queue_is_full(&queue)
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || !concurrent_task_queue_is_empty(&queue)
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (thread_created) {
        sched_thread_destroy(&thread);
    }
    if (sync_initialized) {
        blocking_sync_destroy(&sync);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-BLOCKING-ENQUEUE capacity-one test passed.\n");
    }
    return status;
}

static int task_pointer_count(Task *const *items, size_t count, Task *task)
{
    size_t index;
    int occurrences = 0;

    for (index = 0U; index < count; ++index) {
        if (items[index] == task) {
            ++occurrences;
        }
    }
    return occurrences;
}

static int test_multiple_blocked_producers(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[2 + BLOCKING_PRODUCER_COUNT];
    Task snapshots[2 + BLOCKING_PRODUCER_COUNT];
    Task *removed[2 + BLOCKING_PRODUCER_COUNT] = {0};
    BlockingTestSync sync = {{0}, {0}, 0, 0};
    BlockingProducerContext contexts[BLOCKING_PRODUCER_COUNT] = {0};
    SchedThread threads[BLOCKING_PRODUCER_COUNT] = {0};
    bool sync_initialized = false;
    int created = 0;
    int index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (initialize_tasks(
            tasks,
            snapshots,
            2U + BLOCKING_PRODUCER_COUNT
        ) != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 2U)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_init(&sync) != EXIT_SUCCESS) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    sync_initialized = true;
    if (concurrent_task_queue_try_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_OK) {
        goto cleanup;
    }

    for (index = 0; index < BLOCKING_PRODUCER_COUNT; ++index) {
        contexts[index].queue = &queue;
        contexts[index].task = &tasks[2 + index];
        contexts[index].sync = &sync;
        if (sched_thread_create(
                &threads[index],
                blocking_producer_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            goto cleanup;
        }
        ++created;
    }
    if (blocking_sync_wait_for(
            &sync,
            &sync.started,
            BLOCKING_PRODUCER_COUNT
        ) != EXIT_SUCCESS
        || sched_mutex_lock(&sync.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    if (sync.completed != 0) {
        (void)sched_mutex_unlock(&sync.mutex);
        goto cleanup;
    }
    if (sched_mutex_unlock(&sync.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }

    for (index = 0; index < BLOCKING_PRODUCER_COUNT; ++index) {
        if (concurrent_task_queue_try_dequeue(&queue, &removed[index])
                != CONCURRENT_TASK_QUEUE_OK
            || blocking_sync_wait_for(
                &sync,
                &sync.completed,
                index + 1
            ) != EXIT_SUCCESS
            || concurrent_task_queue_size(&queue) != 2U) {
            goto cleanup;
        }
    }
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != BLOCKING_THREAD_RESULT
            || !contexts[index].executed
            || contexts[index].result != CONCURRENT_TASK_QUEUE_OK) {
            goto cleanup;
        }
    }
    if (concurrent_task_queue_try_dequeue(
            &queue,
            &removed[BLOCKING_PRODUCER_COUNT]
        ) != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_dequeue(
            &queue,
            &removed[BLOCKING_PRODUCER_COUNT + 1]
        ) != CONCURRENT_TASK_QUEUE_OK
        || !concurrent_task_queue_is_empty(&queue)
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    for (index = 0; index < 2 + BLOCKING_PRODUCER_COUNT; ++index) {
        if (task_pointer_count(
                removed,
                2U + BLOCKING_PRODUCER_COUNT,
                &tasks[index]
            ) != 1) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    if (sync_initialized) {
        blocking_sync_destroy(&sync);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-BLOCKING-ENQUEUE multi-producer test passed.\n");
    }
    return status;
}

static int test_mixed_blocking_enqueue(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[3];
    Task snapshots[3];
    BlockingTestSync sync = {{0}, {0}, 0, 0};
    BlockingProducerContext context = {0};
    SchedThread thread = {0};
    bool sync_initialized = false;
    bool thread_created = false;
    Task *output = NULL;
    int thread_result = 0;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, 3U) != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_init(&sync) != EXIT_SUCCESS) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    sync_initialized = true;
    if (concurrent_task_queue_try_enqueue(&queue, &tasks[0])
        != CONCURRENT_TASK_QUEUE_OK) {
        goto cleanup;
    }
    context.queue = &queue;
    context.task = &tasks[1];
    context.sync = &sync;
    if (sched_thread_create(&thread, blocking_producer_worker, &context)
        != SCHED_SYNC_OK) {
        goto cleanup;
    }
    thread_created = true;
    if (blocking_sync_wait_for(&sync, &sync.started, 1) != EXIT_SUCCESS
        || concurrent_task_queue_try_enqueue(&queue, &tasks[2])
            != CONCURRENT_TASK_QUEUE_ERROR_FULL
        || !concurrent_task_queue_is_full(&queue)
        || concurrent_task_queue_size(&queue) != 1U
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || blocking_sync_wait_for(&sync, &sync.completed, 1)
            != EXIT_SUCCESS
        || sched_thread_join(&thread, &thread_result) != SCHED_SYNC_OK
        || thread_result != BLOCKING_THREAD_RESULT
        || context.result != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_try_enqueue(&queue, &tasks[2])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_size(&queue) != 1U
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[2]
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (thread_created) {
        sched_thread_destroy(&thread);
    }
    if (sync_initialized) {
        blocking_sync_destroy(&sync);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-BLOCKING-ENQUEUE mixed test passed.\n");
    }
    return status;
}

static int test_capacity_one_blocked_consumer(void)
{
    ConcurrentTaskQueue queue = {0};
    Task task;
    Task snapshot;
    Task sentinel;
    BlockingTestSync sync = {{0}, {0}, 0, 0};
    BlockingConsumerContext context = {0};
    SchedThread thread = {0};
    bool sync_initialized = false;
    bool thread_created = false;
    int thread_result = 0;
    int status = EXIT_FAILURE;

    if (!task_init(&task, 4000U, TASK_PRIORITY_NORMAL, 6U)
        || concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_init(&sync) != EXIT_SUCCESS) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    snapshot = task;
    sync_initialized = true;
    context.queue = &queue;
    context.sync = &sync;
    context.output = &sentinel;
    if (sched_thread_create(&thread, blocking_consumer_worker, &context)
        != SCHED_SYNC_OK) {
        goto cleanup;
    }
    thread_created = true;

    if (blocking_sync_wait_for(&sync, &sync.started, 1) != EXIT_SUCCESS
        || sched_mutex_lock(&sync.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    if (sync.completed != 0 || context.output != &sentinel) {
        (void)sched_mutex_unlock(&sync.mutex);
        goto cleanup;
    }
    if (sched_mutex_unlock(&sync.mutex) != SCHED_SYNC_OK
        || concurrent_task_queue_try_dequeue(&queue, &context.output)
            != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
        || context.output != &sentinel
        || concurrent_task_queue_enqueue(&queue, &task)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_wait_for(&sync, &sync.completed, 1)
            != EXIT_SUCCESS
        || sched_thread_join(&thread, &thread_result) != SCHED_SYNC_OK
        || thread_result != BLOCKING_CONSUMER_RESULT
        || !context.executed
        || context.result != CONCURRENT_TASK_QUEUE_OK
        || context.output != &task
        || concurrent_task_queue_size(&queue) != 0U
        || concurrent_task_queue_capacity(&queue) != 1U
        || !concurrent_task_queue_is_empty(&queue)
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (thread_created) {
        sched_thread_destroy(&thread);
    }
    if (sync_initialized) {
        blocking_sync_destroy(&sync);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-BLOCKING-DEQUEUE capacity-one test passed.\n");
    }
    return status;
}

static int test_multiple_blocked_consumers(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[BLOCKING_CONSUMER_COUNT];
    Task snapshots[BLOCKING_CONSUMER_COUNT];
    Task *outputs[BLOCKING_CONSUMER_COUNT] = {0};
    BlockingTestSync sync = {{0}, {0}, 0, 0};
    BlockingConsumerContext contexts[BLOCKING_CONSUMER_COUNT] = {0};
    SchedThread threads[BLOCKING_CONSUMER_COUNT] = {0};
    bool sync_initialized = false;
    int created = 0;
    int index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (initialize_tasks(
            tasks,
            snapshots,
            BLOCKING_CONSUMER_COUNT
        ) != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 2U)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_init(&sync) != EXIT_SUCCESS) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    sync_initialized = true;
    for (index = 0; index < BLOCKING_CONSUMER_COUNT; ++index) {
        contexts[index].queue = &queue;
        contexts[index].sync = &sync;
        if (sched_thread_create(
                &threads[index],
                blocking_consumer_worker,
                &contexts[index]
            ) != SCHED_SYNC_OK) {
            goto cleanup;
        }
        ++created;
    }
    if (blocking_sync_wait_for(
            &sync,
            &sync.started,
            BLOCKING_CONSUMER_COUNT
        ) != EXIT_SUCCESS
        || sched_mutex_lock(&sync.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    if (sync.completed != 0) {
        (void)sched_mutex_unlock(&sync.mutex);
        goto cleanup;
    }
    if (sched_mutex_unlock(&sync.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }

    for (index = 0; index < BLOCKING_CONSUMER_COUNT; ++index) {
        if (concurrent_task_queue_enqueue(&queue, &tasks[index])
                != CONCURRENT_TASK_QUEUE_OK
            || blocking_sync_wait_for(
                &sync,
                &sync.completed,
                index + 1
            ) != EXIT_SUCCESS
            || concurrent_task_queue_size(&queue) != 0U) {
            goto cleanup;
        }
    }
    for (index = 0; index < created; ++index) {
        thread_result = 0;
        if (sched_thread_join(&threads[index], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != BLOCKING_CONSUMER_RESULT
            || !contexts[index].executed
            || contexts[index].result != CONCURRENT_TASK_QUEUE_OK) {
            goto cleanup;
        }
        outputs[index] = contexts[index].output;
    }
    for (index = 0; index < BLOCKING_CONSUMER_COUNT; ++index) {
        if (task_pointer_count(
                outputs,
                BLOCKING_CONSUMER_COUNT,
                &tasks[index]
            ) != 1
            || memcmp(&tasks[index], &snapshots[index], sizeof(tasks[index]))
                != 0) {
            goto cleanup;
        }
    }
    if (!concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_capacity(&queue) != 2U) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    for (index = 0; index < created; ++index) {
        sched_thread_destroy(&threads[index]);
    }
    if (sync_initialized) {
        blocking_sync_destroy(&sync);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-BLOCKING-DEQUEUE multi-consumer test passed.\n");
    }
    return status;
}

static int test_blocking_fifo_handoff(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[HANDOFF_TASK_COUNT];
    Task snapshots[HANDOFF_TASK_COUNT];
    Task *outputs[HANDOFF_TASK_COUNT] = {0};
    HandoffContext producer = {0};
    HandoffContext consumer = {0};
    SchedThread producer_thread = {0};
    SchedThread consumer_thread = {0};
    bool producer_created = false;
    bool consumer_created = false;
    int producer_result = 0;
    int consumer_result = 0;
    int index;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, HANDOFF_TASK_COUNT)
            != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 2U)
            != CONCURRENT_TASK_QUEUE_OK) {
        return EXIT_FAILURE;
    }
    producer.queue = &queue;
    producer.tasks = tasks;
    producer.count = HANDOFF_TASK_COUNT;
    consumer.queue = &queue;
    consumer.outputs = outputs;
    consumer.count = HANDOFF_TASK_COUNT;

    if (sched_thread_create(
            &consumer_thread,
            handoff_consumer_worker,
            &consumer
        ) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    consumer_created = true;
    if (sched_thread_create(
            &producer_thread,
            handoff_producer_worker,
            &producer
        ) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    producer_created = true;

    if (sched_thread_join(&producer_thread, &producer_result)
            != SCHED_SYNC_OK
        || sched_thread_join(&consumer_thread, &consumer_result)
            != SCHED_SYNC_OK
        || producer_result != BLOCKING_THREAD_RESULT
        || consumer_result != BLOCKING_CONSUMER_RESULT
        || !producer.executed
        || !consumer.executed
        || producer.result != CONCURRENT_TASK_QUEUE_OK
        || consumer.result != CONCURRENT_TASK_QUEUE_OK
        || !concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_size(&queue) != 0U
        || concurrent_task_queue_capacity(&queue) != 2U
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    for (index = 0; index < HANDOFF_TASK_COUNT; ++index) {
        if (outputs[index] != &tasks[index]) {
            goto cleanup;
        }
    }
    status = EXIT_SUCCESS;

cleanup:
    if (producer_created) {
        sched_thread_destroy(&producer_thread);
    }
    if (consumer_created) {
        sched_thread_destroy(&consumer_thread);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-BLOCKING-HANDOFF-001 passed.\n");
    }
    return status;
}

static int test_mixed_blocking_dequeue(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[3];
    Task snapshots[3];
    Task sentinel;
    Task *output = &sentinel;
    BlockingTestSync sync = {{0}, {0}, 0, 0};
    BlockingConsumerContext context = {0};
    SchedThread thread = {0};
    bool sync_initialized = false;
    bool thread_created = false;
    int thread_result = 0;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, 3U) != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 1U)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_init(&sync) != EXIT_SUCCESS) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    sync_initialized = true;
    if (concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
        || output != &sentinel) {
        goto cleanup;
    }
    context.queue = &queue;
    context.sync = &sync;
    context.output = &sentinel;
    if (sched_thread_create(&thread, blocking_consumer_worker, &context)
        != SCHED_SYNC_OK) {
        goto cleanup;
    }
    thread_created = true;
    if (blocking_sync_wait_for(&sync, &sync.started, 1) != EXIT_SUCCESS
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
        || output != &sentinel
        || concurrent_task_queue_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_wait_for(&sync, &sync.completed, 1)
            != EXIT_SUCCESS
        || sched_thread_join(&thread, &thread_result) != SCHED_SYNC_OK
        || thread_result != BLOCKING_CONSUMER_RESULT
        || context.output != &tasks[0]
        || concurrent_task_queue_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[2])
            != CONCURRENT_TASK_QUEUE_ERROR_FULL
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_try_enqueue(&queue, &tasks[2])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[2]
        || !concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_size(&queue) != 0U
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (thread_created) {
        sched_thread_destroy(&thread);
    }
    if (sync_initialized) {
        blocking_sync_destroy(&sync);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-BLOCKING-DEQUEUE mixed test passed.\n");
    }
    return status;
}

static int test_phase_three_integration_stress(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[INTEGRATION_PRODUCER_COUNT]
        [INTEGRATION_TASKS_PER_PRODUCER];
    Task snapshots[INTEGRATION_PRODUCER_COUNT]
        [INTEGRATION_TASKS_PER_PRODUCER];
    Task *outputs[INTEGRATION_CONSUMER_COUNT]
        [INTEGRATION_TASKS_PER_PRODUCER] = {0};
    IntegrationBarrier barrier = {{0}, {0}, 0, 4, false, false};
    HandoffContext producers[INTEGRATION_PRODUCER_COUNT] = {0};
    HandoffContext consumers[INTEGRATION_CONSUMER_COUNT] = {0};
    SchedThread producer_threads[INTEGRATION_PRODUCER_COUNT] = {0};
    SchedThread consumer_threads[INTEGRATION_CONSUMER_COUNT] = {0};
    bool barrier_mutex_initialized = false;
    bool barrier_condition_initialized = false;
    int producers_created = 0;
    int consumers_created = 0;
    int producer;
    int consumer;
    int task_index;
    int thread_result;
    int status = EXIT_FAILURE;

    if (concurrent_task_queue_init(&queue, 3U)
        != CONCURRENT_TASK_QUEUE_OK) {
        return EXIT_FAILURE;
    }
    for (producer = 0;
         producer < INTEGRATION_PRODUCER_COUNT;
         ++producer) {
        for (task_index = 0;
             task_index < INTEGRATION_TASKS_PER_PRODUCER;
             ++task_index) {
            if (!task_init(
                    &tasks[producer][task_index],
                    5000U
                        + (uint64_t)(
                            producer * INTEGRATION_TASKS_PER_PRODUCER
                            + task_index
                        ),
                    TASK_PRIORITY_NORMAL,
                    8U
                )) {
                goto cleanup;
            }
        }
        memcpy(
            snapshots[producer],
            tasks[producer],
            sizeof(tasks[producer])
        );
    }
    if (sched_mutex_init(&barrier.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    barrier_mutex_initialized = true;
    if (sched_condition_init(&barrier.condition) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    barrier_condition_initialized = true;

    for (consumer = 0;
         consumer < INTEGRATION_CONSUMER_COUNT;
         ++consumer) {
        consumers[consumer].queue = &queue;
        consumers[consumer].outputs = outputs[consumer];
        consumers[consumer].count = INTEGRATION_TASKS_PER_PRODUCER;
        consumers[consumer].barrier = &barrier;
        if (sched_thread_create(
                &consumer_threads[consumer],
                handoff_consumer_worker,
                &consumers[consumer]
            ) != SCHED_SYNC_OK) {
            goto abort_threads;
        }
        ++consumers_created;
    }
    for (producer = 0;
         producer < INTEGRATION_PRODUCER_COUNT;
         ++producer) {
        producers[producer].queue = &queue;
        producers[producer].tasks = tasks[producer];
        producers[producer].count = INTEGRATION_TASKS_PER_PRODUCER;
        producers[producer].barrier = &barrier;
        if (sched_thread_create(
                &producer_threads[producer],
                handoff_producer_worker,
                &producers[producer]
            ) != SCHED_SYNC_OK) {
            goto abort_threads;
        }
        ++producers_created;
    }

    for (producer = 0; producer < producers_created; ++producer) {
        thread_result = 0;
        if (sched_thread_join(&producer_threads[producer], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != BLOCKING_THREAD_RESULT
            || !producers[producer].executed
            || producers[producer].result != CONCURRENT_TASK_QUEUE_OK) {
            goto cleanup;
        }
    }
    for (consumer = 0; consumer < consumers_created; ++consumer) {
        thread_result = 0;
        if (sched_thread_join(&consumer_threads[consumer], &thread_result)
                != SCHED_SYNC_OK
            || thread_result != BLOCKING_CONSUMER_RESULT
            || !consumers[consumer].executed
            || consumers[consumer].result != CONCURRENT_TASK_QUEUE_OK) {
            goto cleanup;
        }
    }
    if (producers_created != INTEGRATION_PRODUCER_COUNT
        || consumers_created != INTEGRATION_CONSUMER_COUNT
        || !concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_size(&queue) != 0U
        || concurrent_task_queue_capacity(&queue) != 3U) {
        goto cleanup;
    }
    for (producer = 0;
         producer < INTEGRATION_PRODUCER_COUNT;
         ++producer) {
        for (task_index = 0;
             task_index < INTEGRATION_TASKS_PER_PRODUCER;
             ++task_index) {
            if (task_pointer_count(
                    &outputs[0][0],
                    INTEGRATION_TOTAL_TASKS,
                    &tasks[producer][task_index]
                ) != 1
                || memcmp(
                    &tasks[producer][task_index],
                    &snapshots[producer][task_index],
                    sizeof(tasks[producer][task_index])
                ) != 0) {
                goto cleanup;
            }
        }
    }
    status = EXIT_SUCCESS;
    goto cleanup;

abort_threads:
    (void)integration_barrier_abort(&barrier);
    for (producer = 0; producer < producers_created; ++producer) {
        (void)sched_thread_join(&producer_threads[producer], NULL);
    }
    for (consumer = 0; consumer < consumers_created; ++consumer) {
        (void)sched_thread_join(&consumer_threads[consumer], NULL);
    }

cleanup:
    for (producer = 0; producer < producers_created; ++producer) {
        sched_thread_destroy(&producer_threads[producer]);
    }
    for (consumer = 0; consumer < consumers_created; ++consumer) {
        sched_thread_destroy(&consumer_threads[consumer]);
    }
    if (barrier_condition_initialized) {
        sched_condition_destroy(&barrier.condition);
    }
    if (barrier_mutex_initialized) {
        sched_mutex_destroy(&barrier.mutex);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-PHASE3-INTEGRATION-001 passed.\n");
    }
    return status;
}

static int test_linearization_trace(void)
{
    ConcurrentTaskQueue queue = {0};
    Task tasks[4];
    Task snapshots[4];
    Task sentinel;
    Task *output = &sentinel;
    BlockingTestSync producer_sync = {{0}, {0}, 0, 0};
    BlockingTestSync consumer_sync = {{0}, {0}, 0, 0};
    BlockingProducerContext producer = {0};
    BlockingConsumerContext consumer = {0};
    SchedThread producer_thread = {0};
    SchedThread consumer_thread = {0};
    bool producer_sync_initialized = false;
    bool consumer_sync_initialized = false;
    bool producer_created = false;
    bool consumer_created = false;
    int thread_result = 0;
    int status = EXIT_FAILURE;

    if (initialize_tasks(tasks, snapshots, 4U) != EXIT_SUCCESS
        || concurrent_task_queue_init(&queue, 2U)
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_init(&producer_sync) != EXIT_SUCCESS) {
        concurrent_task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    producer_sync_initialized = true;
    if (blocking_sync_init(&consumer_sync) != EXIT_SUCCESS) {
        goto cleanup;
    }
    consumer_sync_initialized = true;

    if (concurrent_task_queue_enqueue(&queue, &tasks[0])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_enqueue(&queue, &tasks[1])
            != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_enqueue(&queue, &tasks[2])
            != CONCURRENT_TASK_QUEUE_ERROR_FULL) {
        goto cleanup;
    }
    producer.queue = &queue;
    producer.task = &tasks[2];
    producer.sync = &producer_sync;
    if (sched_thread_create(
            &producer_thread,
            blocking_producer_worker,
            &producer
        ) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    producer_created = true;
    if (blocking_sync_wait_for(
            &producer_sync,
            &producer_sync.started,
            1
        ) != EXIT_SUCCESS
        || concurrent_task_queue_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[0]
        || blocking_sync_wait_for(
            &producer_sync,
            &producer_sync.completed,
            1
        ) != EXIT_SUCCESS
        || sched_thread_join(&producer_thread, &thread_result)
            != SCHED_SYNC_OK
        || thread_result != BLOCKING_THREAD_RESULT
        || producer.result != CONCURRENT_TASK_QUEUE_OK
        || concurrent_task_queue_try_peek(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[1]
        || concurrent_task_queue_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_OK
        || output != &tasks[2]
        || concurrent_task_queue_try_dequeue(&queue, &output)
            != CONCURRENT_TASK_QUEUE_ERROR_EMPTY
        || output != &tasks[2]) {
        goto cleanup;
    }

    consumer.queue = &queue;
    consumer.sync = &consumer_sync;
    consumer.output = &sentinel;
    if (sched_thread_create(
            &consumer_thread,
            blocking_consumer_worker,
            &consumer
        ) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    consumer_created = true;
    if (blocking_sync_wait_for(
            &consumer_sync,
            &consumer_sync.started,
            1
        ) != EXIT_SUCCESS
        || concurrent_task_queue_enqueue(&queue, &tasks[3])
            != CONCURRENT_TASK_QUEUE_OK
        || blocking_sync_wait_for(
            &consumer_sync,
            &consumer_sync.completed,
            1
        ) != EXIT_SUCCESS
        || sched_thread_join(&consumer_thread, &thread_result)
            != SCHED_SYNC_OK
        || thread_result != BLOCKING_CONSUMER_RESULT
        || consumer.result != CONCURRENT_TASK_QUEUE_OK
        || consumer.output != &tasks[3]
        || !concurrent_task_queue_is_empty(&queue)
        || concurrent_task_queue_size(&queue) != 0U
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (producer_created) {
        sched_thread_destroy(&producer_thread);
    }
    if (consumer_created) {
        sched_thread_destroy(&consumer_thread);
    }
    if (producer_sync_initialized) {
        blocking_sync_destroy(&producer_sync);
    }
    if (consumer_sync_initialized) {
        blocking_sync_destroy(&consumer_sync);
    }
    concurrent_task_queue_destroy(&queue);
    if (status == EXIT_SUCCESS) {
        printf("CONCURRENT-LINEARIZATION-TRACE-001 passed.\n");
    }
    return status;
}

int main(void)
{
    if (test_public_api() != EXIT_SUCCESS
        || test_initialization() != EXIT_SUCCESS
        || test_destruction_and_queries() != EXIT_SUCCESS
        || test_result_names() != EXIT_SUCCESS
        || test_sequential_enqueue() != EXIT_SUCCESS
        || test_sequential_dequeue() != EXIT_SUCCESS
        || test_sequential_peek() != EXIT_SUCCESS
        || test_blocking_enqueue_immediate() != EXIT_SUCCESS
        || test_blocking_dequeue_immediate() != EXIT_SUCCESS
        || test_query_concurrency() != EXIT_SUCCESS
        || test_multiple_producers() != EXIT_SUCCESS
        || test_full_contention() != EXIT_SUCCESS
        || test_multiple_consumers() != EXIT_SUCCESS
        || test_empty_contention() != EXIT_SUCCESS
        || test_phased_producer_consumer() != EXIT_SUCCESS
        || test_concurrent_peek_readers() != EXIT_SUCCESS
        || test_mixed_peek_queries() != EXIT_SUCCESS
        || test_coordinated_peek_dequeue() != EXIT_SUCCESS
        || test_empty_peek_contention() != EXIT_SUCCESS
        || test_capacity_one_blocked_producer() != EXIT_SUCCESS
        || test_multiple_blocked_producers() != EXIT_SUCCESS
        || test_mixed_blocking_enqueue() != EXIT_SUCCESS
        || test_capacity_one_blocked_consumer() != EXIT_SUCCESS
        || test_multiple_blocked_consumers() != EXIT_SUCCESS
        || test_blocking_fifo_handoff() != EXIT_SUCCESS
        || test_mixed_blocking_dequeue() != EXIT_SUCCESS
        || test_phase_three_integration_stress() != EXIT_SUCCESS
        || test_linearization_trace() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
