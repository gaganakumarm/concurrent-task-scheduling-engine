#include "sync.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    EXPECTED_VALUE = 73,
    EXPECTED_THREAD_RESULT = -123456789
};

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    bool waiting;
    bool ready;
    bool observed;
    int value;
} SynchronizationTestContext;

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

int main(void)
{
    if (test_invalid_arguments() != EXIT_SUCCESS
        || test_synchronization_integration() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
