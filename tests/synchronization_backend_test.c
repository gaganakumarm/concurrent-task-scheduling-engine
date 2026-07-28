#include "sync.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    EXPECTED_VALUE = 73,
    EXPECTED_THREAD_RESULT = -123456789,
    BROADCAST_WAITER_COUNT = 4,
    BROADCAST_THREAD_RESULT_BASE = -200000000,
    BROADCAST_THREAD_ABORTED = -300000000
};

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    bool waiting;
    bool ready;
    bool observed;
    int value;
} SynchronizationTestContext;

typedef struct {
    SchedMutex mutex;
    SchedCondition release_condition;
    SchedCondition coordination_condition;
    size_t waiting_count;
    size_t completed_count;
    bool release_requested;
    bool abort_requested;
    bool protocol_failure;
    bool completed[BROADCAST_WAITER_COUNT];
} BroadcastTestContext;

typedef struct {
    BroadcastTestContext *shared;
    size_t index;
} BroadcastWaiterContext;

static int synchronization_worker(void *argument)
{
    SynchronizationTestContext *context = argument;

    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return 1;
    }

    context->waiting = true;
    if (sched_condition_signal(&context->condition) != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&context->mutex);
        return 2;
    }

    while (!context->ready) {
        if (sched_condition_wait(&context->condition, &context->mutex)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&context->mutex);
            return 3;
        }
    }

    if (context->value == EXPECTED_VALUE) {
        context->observed = true;
    }

    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return 4;
    }

    return EXPECTED_THREAD_RESULT;
}

static int broadcast_waiter(void *argument)
{
    BroadcastWaiterContext *waiter = argument;
    BroadcastTestContext *context;

    if (waiter == NULL
        || waiter->shared == NULL
        || waiter->index >= BROADCAST_WAITER_COUNT) {
        return 1;
    }

    context = waiter->shared;
    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return 2;
    }

    context->waiting_count++;
    if (sched_condition_signal(&context->coordination_condition)
        != SCHED_SYNC_OK) {
        context->protocol_failure = true;
        (void)sched_mutex_unlock(&context->mutex);
        return 3;
    }

    while (!context->release_requested && !context->abort_requested) {
        if (sched_condition_wait(
                &context->release_condition,
                &context->mutex
            ) != SCHED_SYNC_OK) {
            context->protocol_failure = true;
            (void)sched_mutex_unlock(&context->mutex);
            return 4;
        }
    }

    if (context->abort_requested) {
        (void)sched_mutex_unlock(&context->mutex);
        return BROADCAST_THREAD_ABORTED;
    }

    if (!context->release_requested
        || context->completed[waiter->index]) {
        context->protocol_failure = true;
        (void)sched_mutex_unlock(&context->mutex);
        return 5;
    }

    /*
     * Updating protected state here demonstrates that condition wait returned
     * with the same mutex reacquired.
     */
    context->completed[waiter->index] = true;
    context->completed_count++;

    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return 6;
    }

    return BROADCAST_THREAD_RESULT_BASE + (int)waiter->index;
}

static int test_invalid_arguments(void)
{
    SchedMutex mutex = {0};
    SchedCondition condition = {0};
    SchedThread thread = {0};

    if (sched_mutex_init(NULL) != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_mutex_lock(NULL) != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_mutex_unlock(NULL) != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_condition_init(NULL) != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_condition_wait(NULL, &mutex)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_condition_wait(&condition, NULL)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_condition_signal(NULL)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_condition_broadcast(NULL)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_condition_broadcast(&condition)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_thread_create(NULL, synchronization_worker, NULL)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_thread_create(&thread, NULL, NULL)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT
        || sched_thread_join(NULL, NULL)
            != SCHED_SYNC_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "SYNC-API-001 through SYNC-API-010 failed.\n");
        return EXIT_FAILURE;
    }

    sched_mutex_destroy(NULL);
    sched_condition_destroy(NULL);
    sched_thread_destroy(NULL);

    printf("SYNC-API-001 through SYNC-API-010 passed.\n");
    return EXIT_SUCCESS;
}

static int test_synchronization_integration(void)
{
    SynchronizationTestContext context = {
        .mutex = {0},
        .condition = {0},
        .waiting = false,
        .ready = false,
        .observed = false,
        .value = 0
    };
    SchedThread thread = {0};
    int thread_result = 0;
    int status = EXIT_FAILURE;

    if (sched_mutex_init(&context.mutex) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-MUTEX-001 failed.\n");
        return EXIT_FAILURE;
    }

    if (sched_condition_init(&context.condition) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-CONDITION-001 failed.\n");
        sched_mutex_destroy(&context.mutex);
        return EXIT_FAILURE;
    }

    if (sched_thread_create(
            &thread,
            synchronization_worker,
            &context
        ) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-THREAD-001 failed.\n");
        sched_condition_destroy(&context.condition);
        sched_mutex_destroy(&context.mutex);
        return EXIT_FAILURE;
    }

    if (sched_mutex_lock(&context.mutex) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-MUTEX-002 failed.\n");
        goto cleanup_thread;
    }

    while (!context.waiting) {
        if (sched_condition_wait(&context.condition, &context.mutex)
            != SCHED_SYNC_OK) {
            fprintf(stderr, "Synchronization handshake wait failed.\n");
            (void)sched_mutex_unlock(&context.mutex);
            goto cleanup_thread;
        }
    }

    context.value = EXPECTED_VALUE;
    context.ready = true;

    if (sched_condition_signal(&context.condition) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-CONDITION-003 failed.\n");
        (void)sched_mutex_unlock(&context.mutex);
        goto cleanup_thread;
    }

    if (sched_mutex_unlock(&context.mutex) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-MUTEX-003 failed.\n");
        goto cleanup_thread;
    }

    if (sched_thread_join(&thread, &thread_result) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-THREAD-002 failed.\n");
        goto cleanup_thread;
    }

    if (thread_result != EXPECTED_THREAD_RESULT) {
        fprintf(stderr, "SYNC-THREAD-003 failed: result %d.\n", thread_result);
        goto cleanup_thread;
    }

    if (!context.observed
        || context.value != EXPECTED_VALUE
        || !context.ready
        || !context.waiting) {
        fprintf(stderr, "SYNC-THREAD-004 or SYNC-INTEGRATION-001 failed.\n");
        goto cleanup_thread;
    }

    if (sched_condition_broadcast(&context.condition) != SCHED_SYNC_OK) {
        fprintf(stderr, "SYNC-CONDITION-004 failed.\n");
        goto cleanup_thread;
    }

    status = EXIT_SUCCESS;

cleanup_thread:
    sched_thread_destroy(&thread);
    sched_condition_destroy(&context.condition);
    sched_mutex_destroy(&context.mutex);

    if (status == EXIT_SUCCESS) {
        printf(
            "SYNC-MUTEX, SYNC-CONDITION, SYNC-THREAD, and "
            "SYNC-INTEGRATION tests passed.\n"
        );
    }

    return status;
}

static int test_condition_broadcast(void)
{
    BroadcastTestContext context = {
        .mutex = {0},
        .release_condition = {0},
        .coordination_condition = {0},
        .waiting_count = 0U,
        .completed_count = 0U,
        .release_requested = false,
        .abort_requested = false,
        .protocol_failure = false,
        .completed = {false}
    };
    SchedThread threads[BROADCAST_WAITER_COUNT] = {{0}};
    BroadcastWaiterContext waiters[BROADCAST_WAITER_COUNT];
    int thread_results[BROADCAST_WAITER_COUNT] = {0};
    size_t created_count = 0U;
    size_t joined_count = 0U;
    size_t index;
    size_t broadcast_count = 0U;
    bool mutex_initialized = false;
    bool release_condition_initialized = false;
    bool coordination_condition_initialized = false;
    bool mutex_locked = false;
    int status = EXIT_FAILURE;

    if (sched_mutex_init(&context.mutex) != SCHED_SYNC_OK) {
        fprintf(stderr, "BROADCAST-BACKEND-010 failed: mutex init.\n");
        goto cleanup;
    }
    mutex_initialized = true;

    if (sched_condition_init(&context.release_condition) != SCHED_SYNC_OK) {
        fprintf(
            stderr,
            "BROADCAST-BACKEND-001 or BROADCAST-BACKEND-010 failed.\n"
        );
        goto cleanup;
    }
    release_condition_initialized = true;

    if (sched_condition_init(&context.coordination_condition)
        != SCHED_SYNC_OK) {
        fprintf(stderr, "BROADCAST-BACKEND-010 failed: coordination init.\n");
        goto cleanup;
    }
    coordination_condition_initialized = true;

    for (index = 0U; index < BROADCAST_WAITER_COUNT; index++) {
        waiters[index].shared = &context;
        waiters[index].index = index;
        if (sched_thread_create(
                &threads[index],
                broadcast_waiter,
                &waiters[index]
            ) != SCHED_SYNC_OK) {
            fprintf(
                stderr,
                "BROADCAST-BACKEND-003 or BROADCAST-BACKEND-009 failed "
                "during thread creation at index %zu.\n",
                index
            );
            goto release_waiters;
        }
        created_count++;
    }

    if (sched_mutex_lock(&context.mutex) != SCHED_SYNC_OK) {
        fprintf(stderr, "BROADCAST-BACKEND-004 failed: main lock.\n");
        goto release_waiters;
    }
    mutex_locked = true;

    while (context.waiting_count < BROADCAST_WAITER_COUNT) {
        if (sched_condition_wait(
                &context.coordination_condition,
                &context.mutex
            ) != SCHED_SYNC_OK) {
            fprintf(stderr, "BROADCAST-BACKEND-004 failed: readiness wait.\n");
            goto release_waiters;
        }
    }

    if (context.completed_count != 0U) {
        fprintf(stderr, "BROADCAST-BACKEND-006 failed.\n");
        goto release_waiters;
    }

    context.release_requested = true;
    if (sched_condition_broadcast(&context.release_condition)
        != SCHED_SYNC_OK) {
        fprintf(stderr, "BROADCAST-BACKEND-001 or -002 failed.\n");
        goto release_waiters;
    }
    broadcast_count++;

    if (sched_mutex_unlock(&context.mutex) != SCHED_SYNC_OK) {
        mutex_locked = false;
        fprintf(stderr, "BROADCAST-BACKEND-007 failed: main unlock.\n");
        goto release_waiters;
    }
    mutex_locked = false;

    for (index = 0U; index < created_count; index++) {
        if (sched_thread_join(&threads[index], &thread_results[index])
            != SCHED_SYNC_OK) {
            fprintf(
                stderr,
                "BROADCAST-BACKEND-009 failed: join index %zu.\n",
                index
            );
            goto cleanup_threads;
        }
        joined_count++;
    }

    if (sched_mutex_lock(&context.mutex) != SCHED_SYNC_OK) {
        fprintf(stderr, "BROADCAST-BACKEND-007 failed: verification lock.\n");
        goto cleanup_threads;
    }
    mutex_locked = true;

    if (context.waiting_count != BROADCAST_WAITER_COUNT
        || context.completed_count != BROADCAST_WAITER_COUNT
        || !context.release_requested
        || context.abort_requested
        || context.protocol_failure
        || broadcast_count != 1U) {
        fprintf(stderr, "BROADCAST-BACKEND-002 through -013 failed.\n");
        goto release_waiters;
    }

    for (index = 0U; index < BROADCAST_WAITER_COUNT; index++) {
        if (!context.completed[index]
            || thread_results[index]
                != BROADCAST_THREAD_RESULT_BASE + (int)index) {
            fprintf(
                stderr,
                "BROADCAST-BACKEND-005 or -008 failed at index %zu.\n",
                index
            );
            goto release_waiters;
        }
    }

    if (sched_mutex_unlock(&context.mutex) != SCHED_SYNC_OK) {
        mutex_locked = false;
        fprintf(stderr, "BROADCAST-BACKEND-007 failed: verification unlock.\n");
        goto cleanup_threads;
    }
    mutex_locked = false;
    status = EXIT_SUCCESS;
    goto cleanup_threads;

release_waiters:
    if (!mutex_locked && mutex_initialized
        && sched_mutex_lock(&context.mutex) == SCHED_SYNC_OK) {
        mutex_locked = true;
    }
    if (mutex_locked) {
        context.abort_requested = true;
        context.release_requested = true;
        if (release_condition_initialized) {
            (void)sched_condition_broadcast(&context.release_condition);
        }
        (void)sched_mutex_unlock(&context.mutex);
        mutex_locked = false;
    }

cleanup_threads:
    for (index = joined_count; index < created_count; index++) {
        if (sched_thread_join(&threads[index], &thread_results[index])
            != SCHED_SYNC_OK) {
            fprintf(
                stderr,
                "BROADCAST-BACKEND-009 failed during cleanup at index %zu.\n",
                index
            );
        }
    }
    for (index = 0U; index < created_count; index++) {
        sched_thread_destroy(&threads[index]);
    }

cleanup:
    if (coordination_condition_initialized) {
        sched_condition_destroy(&context.coordination_condition);
    }
    if (release_condition_initialized) {
        sched_condition_destroy(&context.release_condition);
    }
    if (mutex_initialized) {
        sched_mutex_destroy(&context.mutex);
    }

    if (status == EXIT_SUCCESS) {
        printf("BROADCAST-BACKEND-001 through -015 passed.\n");
    }
    return status;
}

int main(void)
{
    if (test_invalid_arguments() != EXIT_SUCCESS
        || test_synchronization_integration() != EXIT_SUCCESS
        || test_condition_broadcast() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
