#include "concurrent_scheduler/scheduler.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int invocation_count;
    int marker;
} CallbackContext;

enum {
    EXECUTION_TASK_LIMIT = 300
};

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    Task *expected[EXECUTION_TASK_LIMIT];
    Task *observed[EXECUTION_TASK_LIMIT];
    size_t expected_count;
    size_t observed_count;
    size_t execution_count[EXECUTION_TASK_LIMIT];
    size_t invocation_count;
    size_t active_count;
    size_t maximum_active_count;
    int release_callbacks;
    int context_mismatch;
    Task *failure_task;
    void *expected_context;
} ExecutionContext;

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    Scheduler *scheduler;
    Task *task;
    SchedulerResult result;
    int started;
    int completed;
} SubmitterContext;

typedef struct {
    SchedMutex mutex;
    SchedCondition condition;
    Scheduler *scheduler;
    SchedulerResult result;
    int started;
    int completed;
} JoinContext;

typedef struct {
    Scheduler *scheduler;
    Task *tasks;
    size_t task_count;
    SchedulerResult result;
} AuditProducerContext;

static int test_execute(Task *task, void *context)
{
    CallbackContext *callback_context = context;

    if (callback_context != NULL) {
        callback_context->invocation_count++;
    }
    return task == NULL ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int execution_context_init(ExecutionContext *context)
{
    if (sched_mutex_init(&context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    if (sched_condition_init(&context->condition) != SCHED_SYNC_OK) {
        sched_mutex_destroy(&context->mutex);
        return EXIT_FAILURE;
    }
    context->expected_context = context;
    return EXIT_SUCCESS;
}

static void execution_context_destroy(ExecutionContext *context)
{
    sched_condition_destroy(&context->condition);
    sched_mutex_destroy(&context->mutex);
}

static int controlled_execute(Task *task, void *argument)
{
    ExecutionContext *context = argument;
    size_t index;
    int callback_result;

    if (context == NULL
        || sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    if (argument != context->expected_context) {
        context->context_mismatch = 1;
    }
    for (index = 0U; index < context->expected_count; index++) {
        if (context->expected[index] == task) {
            context->execution_count[index]++;
            break;
        }
    }
    if (index == context->expected_count) {
        context->context_mismatch = 1;
    }
    if (context->observed_count < EXECUTION_TASK_LIMIT) {
        context->observed[context->observed_count] = task;
        context->observed_count++;
    } else {
        context->context_mismatch = 1;
    }

    context->invocation_count++;
    context->active_count++;
    if (context->active_count > context->maximum_active_count) {
        context->maximum_active_count = context->active_count;
    }
    (void)sched_condition_broadcast(&context->condition);
    while (!context->release_callbacks) {
        if (sched_condition_wait(&context->condition, &context->mutex)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&context->mutex);
            return -1;
        }
    }
    context->active_count--;
    callback_result = task == context->failure_task ? 37 : 0;
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    return callback_result;
}

static int wait_for_invocations(ExecutionContext *context, size_t target)
{
    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    while (context->invocation_count < target) {
        if (sched_condition_wait(&context->condition, &context->mutex)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&context->mutex);
            return EXIT_FAILURE;
        }
    }
    return sched_mutex_unlock(&context->mutex) == SCHED_SYNC_OK
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

static int release_callbacks(ExecutionContext *context)
{
    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    context->release_callbacks = 1;
    if (sched_condition_broadcast(&context->condition) != SCHED_SYNC_OK) {
        (void)sched_mutex_unlock(&context->mutex);
        return EXIT_FAILURE;
    }
    return sched_mutex_unlock(&context->mutex) == SCHED_SYNC_OK
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

static int blocking_submitter(void *argument)
{
    SubmitterContext *context = argument;

    if (context == NULL
        || sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    context->started = 1;
    (void)sched_condition_broadcast(&context->condition);
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }

    context->result = scheduler_submit(context->scheduler, context->task);

    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    context->completed = 1;
    (void)sched_condition_broadcast(&context->condition);
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    return 0;
}

static int joining_thread(void *argument)
{
    JoinContext *context = argument;

    if (context == NULL
        || sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    context->started = 1;
    (void)sched_condition_broadcast(&context->condition);
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }

    context->result = scheduler_join(context->scheduler);

    if (sched_mutex_lock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    context->completed = 1;
    (void)sched_condition_broadcast(&context->condition);
    if (sched_mutex_unlock(&context->mutex) != SCHED_SYNC_OK) {
        return -1;
    }
    return 0;
}

static int audit_producer(void *argument)
{
    AuditProducerContext *context = argument;
    size_t index;

    if (context == NULL || context->scheduler == NULL) {
        return -1;
    }
    context->result = SCHEDULER_OK;
    for (index = 0U; index < context->task_count; index++) {
        context->result = scheduler_submit(
            context->scheduler,
            &context->tasks[index]
        );
        if (context->result != SCHEDULER_OK) {
            return -2;
        }
    }
    return 0;
}

static void cleanup_scheduler(Scheduler *scheduler)
{
    (void)scheduler_shutdown(scheduler);
    (void)scheduler_join(scheduler);
    (void)scheduler_destroy(scheduler);
}

static int test_public_api_and_result_names(void)
{
    SchedulerResult (*init_function)(
        Scheduler *,
        size_t,
        size_t,
        SchedulerTaskExecuteFunction,
        void *
    ) = scheduler_init;
    SchedulerResult (*start_function)(Scheduler *) = scheduler_start;
    SchedulerResult (*shutdown_function)(Scheduler *) = scheduler_shutdown;
    SchedulerResult (*join_function)(Scheduler *) = scheduler_join;
    SchedulerResult (*destroy_function)(Scheduler *) = scheduler_destroy;
    const char *(*name_function)(SchedulerResult) = scheduler_result_name;
    const struct {
        SchedulerResult result;
        const char *name;
    } cases[] = {
        {SCHEDULER_OK, "OK"},
        {SCHEDULER_ERROR_INVALID_ARGUMENT, "INVALID_ARGUMENT"},
        {SCHEDULER_ERROR_INVALID_STATE, "INVALID_STATE"},
        {SCHEDULER_ERROR_ALLOCATION, "ALLOCATION_ERROR"},
        {SCHEDULER_ERROR_QUEUE_FULL, "QUEUE_FULL"},
        {SCHEDULER_ERROR_SHUTDOWN, "SHUTDOWN"},
        {SCHEDULER_ERROR_SYSTEM, "SYSTEM_ERROR"}
    };
    size_t index;

    if (init_function == NULL
        || start_function == NULL
        || shutdown_function == NULL
        || join_function == NULL
        || destroy_function == NULL
        || name_function == NULL) {
        fprintf(stderr, "SCHEDULER-API-001 failed.\n");
        return EXIT_FAILURE;
    }
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (strcmp(
                scheduler_result_name(cases[index].result),
                cases[index].name
            ) != 0) {
            fprintf(stderr, "SCHEDULER-RESULT-001 failed.\n");
            return EXIT_FAILURE;
        }
    }
    if (strcmp(scheduler_result_name((SchedulerResult)-1), "UNKNOWN") != 0
        || strcmp(scheduler_result_name((SchedulerResult)999), "UNKNOWN")
            != 0) {
        fprintf(stderr, "SCHEDULER-RESULT-002 failed.\n");
        return EXIT_FAILURE;
    }

    printf("SCHEDULER-API-001 and SCHEDULER-RESULT-001 through -002 passed.\n");
    return EXIT_SUCCESS;
}

static int test_initialization_validation(void)
{
    Scheduler scheduler = {0};
    CallbackContext context = {0, 73};

    if (scheduler_init(NULL, 2U, 1U, test_execute, &context)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_init(&scheduler, 0U, 1U, test_execute, &context)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler.implementation != NULL
        || scheduler_init(&scheduler, 2U, 0U, test_execute, &context)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler.implementation != NULL
        || scheduler_init(&scheduler, 2U, 1U, NULL, &context)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler.implementation != NULL
        || context.invocation_count != 0
        || context.marker != 73) {
        fprintf(stderr, "SCHEDULER-INIT-002 through -006 failed.\n");
        return EXIT_FAILURE;
    }

    printf("SCHEDULER-INIT-002 through -006 passed.\n");
    return EXIT_SUCCESS;
}

static int test_initialization_and_duplicate(void)
{
    Scheduler scheduler = {0};
    CallbackContext context = {0, 91};
    void *published_implementation;
    Task task;
    Task snapshot;
    int status = EXIT_FAILURE;

    if (!task_init(&task, 15000U, TASK_PRIORITY_HIGH, 12U)) {
        return EXIT_FAILURE;
    }
    snapshot = task;

    if (scheduler_init(&scheduler, 3U, 4U, test_execute, NULL)
            != SCHEDULER_OK
        || scheduler.implementation == NULL
        || context.invocation_count != 0
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        fprintf(stderr, "SCHEDULER-INIT-001, -007, or -011 through -016 failed.\n");
        goto cleanup;
    }
    published_implementation = scheduler.implementation;
    if (scheduler_init(&scheduler, 5U, 2U, test_execute, &context)
            != SCHEDULER_ERROR_INVALID_STATE
        || scheduler.implementation != published_implementation
        || context.invocation_count != 0
        || context.marker != 91
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        fprintf(stderr, "SCHEDULER-INIT-008 through -010 failed.\n");
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        status = EXIT_FAILURE;
    }
    if (status == EXIT_SUCCESS) {
        printf("SCHEDULER-INIT-001 and -007 through -016 passed.\n");
    }
    return status;
}

static int test_destroy_and_reinitialize(void)
{
    Scheduler scheduler = {0};
    CallbackContext context = {0, 117};
    Task task;
    Task snapshot;

    if (!task_init(&task, 16000U, TASK_PRIORITY_NORMAL, 4U)) {
        return EXIT_FAILURE;
    }
    snapshot = task;

    if (scheduler_destroy(NULL) != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler_init(&scheduler, 2U, 2U, test_execute, &context)
            != SCHEDULER_OK
        || scheduler.implementation == NULL
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler.implementation != NULL
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler_init(&scheduler, 4U, 3U, test_execute, &context)
            != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler.implementation != NULL
        || context.invocation_count != 0
        || context.marker != 117
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        fprintf(
            stderr,
            "SCHEDULER-DESTROY-001 through -007 or "
            "SCHEDULER-OWNERSHIP-001 through -002 failed.\n"
        );
        (void)scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    printf(
        "SCHEDULER-DESTROY-001 through -007 and "
        "SCHEDULER-OWNERSHIP-001 through -002 passed.\n"
    );
    return EXIT_SUCCESS;
}

static int test_start_validation(void)
{
    Scheduler scheduler = {0};

    if (scheduler_start(NULL) != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_start(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler.implementation != NULL) {
        fprintf(stderr, "SCHEDULER-START-007 through -008 failed.\n");
        return EXIT_FAILURE;
    }

    printf("SCHEDULER-START-007 through -008 passed.\n");
    return EXIT_SUCCESS;
}

static int test_start_lifecycle(void)
{
    Scheduler scheduler = {0};
    CallbackContext context = {0, 211};
    Task task;
    Task snapshot;
    void *running_implementation;
    int status = EXIT_FAILURE;

    if (!task_init(&task, 17000U, TASK_PRIORITY_HIGH, 9U)) {
        return EXIT_FAILURE;
    }
    snapshot = task;

    if (scheduler_init(&scheduler, 4U, 5U, test_execute, &context)
            != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler.implementation == NULL
        || context.invocation_count != 0
        || context.marker != 211
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        fprintf(stderr, "SCHEDULER-START-001 through -006 failed.\n");
        goto cleanup;
    }

    running_implementation = scheduler.implementation;
    if (scheduler_start(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler.implementation != running_implementation
        || context.invocation_count != 0
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        fprintf(stderr, "SCHEDULER-START-009 through -010 failed.\n");
        goto cleanup;
    }

    if (scheduler_destroy(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler.implementation != NULL
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || context.invocation_count != 0
        || context.marker != 211
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        fprintf(stderr, "SCHEDULER-START-011 through -012 failed.\n");
        return EXIT_FAILURE;
    }

    if (scheduler_init(&scheduler, 3U, 3U, test_execute, &context)
            != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler.implementation != NULL
        || context.invocation_count != 0
        || context.marker != 211
        || memcmp(&task, &snapshot, sizeof(task)) != 0) {
        fprintf(stderr, "SCHEDULER-START-013 through -020 failed.\n");
        return EXIT_FAILURE;
    }

    printf("SCHEDULER-START-001 through -020 passed.\n");
    return EXIT_SUCCESS;

cleanup:
    cleanup_scheduler(&scheduler);
    return status;
}

static int test_submission_validation(void)
{
    Scheduler scheduler = {0};
    Task task;

    if (!task_init(&task, 18000U, TASK_PRIORITY_NORMAL, 2U)
        || scheduler_submit(NULL, &task)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_try_submit(NULL, &task)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_submit(&scheduler, NULL)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_try_submit(&scheduler, NULL)
            != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_submit(&scheduler, &task)
            != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_try_submit(&scheduler, &task)
            != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_init(&scheduler, 2U, 1U, test_execute, NULL)
            != SCHEDULER_OK
        || scheduler_submit(&scheduler, &task)
            != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_try_submit(&scheduler, &task)
            != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        (void)scheduler_destroy(&scheduler);
        fprintf(stderr, "SCHEDULER-SUBMIT-001 through -004 failed.\n");
        return EXIT_FAILURE;
    }

    printf("SCHEDULER-SUBMIT-001 through -004 passed.\n");
    return EXIT_SUCCESS;
}

static int test_concurrent_callbacks(void)
{
    Scheduler scheduler = {0};
    ExecutionContext context = {0};
    Task tasks[2];
    Task snapshots[2];
    int status = EXIT_FAILURE;

    if (!task_init(&tasks[0], 18100U, TASK_PRIORITY_NORMAL, 3U)
        || !task_init(&tasks[1], 18101U, TASK_PRIORITY_HIGH, 4U)) {
        return EXIT_FAILURE;
    }
    memcpy(snapshots, tasks, sizeof(tasks));
    context.expected[0] = &tasks[0];
    context.expected[1] = &tasks[1];
    context.expected_count = 2U;

    if (execution_context_init(&context) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (scheduler_init(&scheduler, 2U, 2U, controlled_execute, &context)
            != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_submit(&scheduler, &tasks[0]) != SCHEDULER_OK
        || scheduler_submit(&scheduler, &tasks[1]) != SCHEDULER_OK
        || wait_for_invocations(&context, 2U) != EXIT_SUCCESS
        || context.maximum_active_count < 2U
        || context.context_mismatch
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(stderr, "SCHEDULER-WORKER-001 through -006 failed.\n");
        goto cleanup;
    }
    if (release_callbacks(&context) != EXIT_SUCCESS
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler.implementation != NULL
        || context.execution_count[0] != 1U
        || context.execution_count[1] != 1U
        || context.invocation_count != 2U
        || context.active_count != 0U
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0
        || scheduler_submit(&scheduler, &tasks[0])
            != SCHEDULER_ERROR_INVALID_STATE) {
        fprintf(stderr, "SCHEDULER-WORKER-007 through -018 failed.\n");
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    (void)release_callbacks(&context);
    cleanup_scheduler(&scheduler);
    execution_context_destroy(&context);
    if (status == EXIT_SUCCESS) {
        printf("SCHEDULER-WORKER-001 through -018 passed.\n");
    }
    return status;
}

static int test_submit_capacity_and_blocking(void)
{
    Scheduler scheduler = {0};
    ExecutionContext context = {0};
    SubmitterContext submitter = {0};
    JoinContext join_context = {0};
    SchedThread submitter_thread = {0};
    SchedThread join_thread = {0};
    Task tasks[4];
    Task snapshots[4];
    int submitter_result = -1;
    int submitter_created = 0;
    int join_created = 0;
    int join_result = -1;
    int status = EXIT_FAILURE;
    size_t index;

    for (index = 0U; index < 4U; index++) {
        if (!task_init(
                &tasks[index],
                18200U + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                5U
            )) {
            return EXIT_FAILURE;
        }
        context.expected[index] = &tasks[index];
    }
    memcpy(snapshots, tasks, sizeof(tasks));
    context.expected_count = 4U;

    if (execution_context_init(&context) != EXIT_SUCCESS
        || sched_mutex_init(&submitter.mutex) != SCHED_SYNC_OK
        || sched_condition_init(&submitter.condition) != SCHED_SYNC_OK
        || sched_mutex_init(&join_context.mutex) != SCHED_SYNC_OK
        || sched_condition_init(&join_context.condition) != SCHED_SYNC_OK) {
        return EXIT_FAILURE;
    }
    submitter.scheduler = &scheduler;
    submitter.task = &tasks[3];
    join_context.scheduler = &scheduler;

    if (scheduler_init(&scheduler, 1U, 1U, controlled_execute, &context)
            != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_try_submit(&scheduler, &tasks[0]) != SCHEDULER_OK
        || wait_for_invocations(&context, 1U) != EXIT_SUCCESS
        || scheduler_submit(&scheduler, &tasks[1]) != SCHEDULER_OK
        || scheduler_try_submit(&scheduler, &tasks[2])
            != SCHEDULER_ERROR_QUEUE_FULL
        || sched_thread_create(
                &submitter_thread,
                blocking_submitter,
                &submitter
            ) != SCHED_SYNC_OK) {
        fprintf(stderr, "SCHEDULER-SUBMIT-005 through -010 failed.\n");
        goto cleanup;
    }
    submitter_created = 1;

    if (sched_mutex_lock(&submitter.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    while (!submitter.started) {
        if (sched_condition_wait(&submitter.condition, &submitter.mutex)
            != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&submitter.mutex);
            goto cleanup;
        }
    }
    if (submitter.completed
        || sched_mutex_unlock(&submitter.mutex) != SCHED_SYNC_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || sched_thread_join(&submitter_thread, &submitter_result)
            != SCHED_SYNC_OK
        || submitter_result != 0
        || submitter.result != SCHEDULER_ERROR_SHUTDOWN
        || context.active_count != 1U
        || scheduler_submit(&scheduler, &tasks[2])
            != SCHEDULER_ERROR_SHUTDOWN
        || scheduler_try_submit(&scheduler, &tasks[2])
            != SCHEDULER_ERROR_SHUTDOWN
        || scheduler_destroy(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || sched_thread_create(
                &join_thread,
                joining_thread,
                &join_context
            ) != SCHED_SYNC_OK) {
        fprintf(stderr, "SCHEDULER-SHUTDOWN-004 through -017 failed.\n");
        goto cleanup;
    }
    join_created = 1;

    if (sched_mutex_lock(&join_context.mutex) != SCHED_SYNC_OK) {
        goto cleanup;
    }
    while (!join_context.started) {
        if (sched_condition_wait(
                &join_context.condition,
                &join_context.mutex
            ) != SCHED_SYNC_OK) {
            (void)sched_mutex_unlock(&join_context.mutex);
            goto cleanup;
        }
    }
    if (join_context.completed
        || sched_mutex_unlock(&join_context.mutex) != SCHED_SYNC_OK
        || release_callbacks(&context) != EXIT_SUCCESS
        || sched_thread_join(&join_thread, &join_result) != SCHED_SYNC_OK
        || join_result != 0
        || join_context.result != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || context.execution_count[0] != 1U
        || context.execution_count[1] != 1U
        || context.execution_count[2] != 0U
        || context.execution_count[3] != 0U
        || context.invocation_count != 2U
        || context.context_mismatch
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        fprintf(stderr, "SCHEDULER-JOIN-005 through -018 failed.\n");
        goto cleanup;
    }
    sched_thread_destroy(&submitter_thread);
    sched_thread_destroy(&join_thread);
    status = EXIT_SUCCESS;

cleanup:
    (void)release_callbacks(&context);
    if (submitter_created && submitter_thread.implementation != NULL) {
        (void)sched_thread_join(&submitter_thread, NULL);
        sched_thread_destroy(&submitter_thread);
    }
    if (join_created && join_thread.implementation != NULL) {
        (void)sched_thread_join(&join_thread, NULL);
        sched_thread_destroy(&join_thread);
    }
    cleanup_scheduler(&scheduler);
    sched_condition_destroy(&join_context.condition);
    sched_mutex_destroy(&join_context.mutex);
    sched_condition_destroy(&submitter.condition);
    sched_mutex_destroy(&submitter.mutex);
    execution_context_destroy(&context);
    if (status == EXIT_SUCCESS) {
        printf("SCHEDULER-SUBMIT-005 through -012 passed.\n");
    }
    return status;
}

static int test_callback_failure_and_reuse(void)
{
    Scheduler scheduler = {0};
    ExecutionContext context = {0};
    Task tasks[3];
    size_t index;
    int status = EXIT_FAILURE;

    for (index = 0U; index < 3U; index++) {
        if (!task_init(
                &tasks[index],
                18300U + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                6U
            )) {
            return EXIT_FAILURE;
        }
        context.expected[index] = &tasks[index];
    }
    context.expected_count = 3U;
    context.failure_task = &tasks[1];
    context.release_callbacks = 1;

    if (execution_context_init(&context) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (scheduler_init(&scheduler, 3U, 1U, controlled_execute, &context)
            != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_submit(&scheduler, &tasks[0]) != SCHEDULER_OK
        || scheduler_submit(&scheduler, &tasks[1]) != SCHEDULER_OK
        || scheduler_submit(&scheduler, &tasks[2]) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || context.execution_count[0] != 1U
        || context.execution_count[1] != 1U
        || context.execution_count[2] != 1U
        || context.invocation_count != 3U
        || scheduler_init(&scheduler, 1U, 1U, controlled_execute, &context)
            != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_submit(&scheduler, &tasks[0]) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || context.execution_count[0] != 2U
        || context.invocation_count != 4U) {
        fprintf(stderr, "SCHEDULER-WORKER-009 through -020 failed.\n");
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    cleanup_scheduler(&scheduler);
    execution_context_destroy(&context);
    if (status == EXIT_SUCCESS) {
        printf("SCHEDULER-WORKER-009 through -020 passed.\n");
    }
    return status;
}

static int test_shutdown_join_validation(void)
{
    Scheduler scheduler = {0};

    if (scheduler_shutdown(NULL) != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_join(NULL) != SCHEDULER_ERROR_INVALID_ARGUMENT
        || scheduler_shutdown(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_join(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_init(&scheduler, 2U, 2U, test_execute, NULL)
            != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_join(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_start(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_destroy(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_ERROR_INVALID_STATE
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK
        || scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        cleanup_scheduler(&scheduler);
        fprintf(
            stderr,
            "SCHEDULER-SHUTDOWN-001 through -008 or "
            "SCHEDULER-JOIN-001 through -004 failed.\n"
        );
        return EXIT_FAILURE;
    }

    printf(
        "SCHEDULER-SHUTDOWN-001 through -008 and "
        "SCHEDULER-JOIN-001 through -004 passed.\n"
    );
    return EXIT_SUCCESS;
}

static int test_multi_worker_shutdown_drain(void)
{
    enum {
        TASK_COUNT = 64
    };
    Scheduler scheduler = {0};
    ExecutionContext context = {0};
    Task tasks[TASK_COUNT];
    Task snapshots[TASK_COUNT];
    size_t index;
    size_t invocations_after_join;
    int status = EXIT_FAILURE;

    for (index = 0U; index < TASK_COUNT; index++) {
        if (!task_init(
                &tasks[index],
                19000U + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                7U
            )) {
            return EXIT_FAILURE;
        }
        context.expected[index] = &tasks[index];
    }
    memcpy(snapshots, tasks, sizeof(tasks));
    context.expected_count = TASK_COUNT;
    context.failure_task = &tasks[7];
    context.release_callbacks = 1;

    if (execution_context_init(&context) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (scheduler_init(
            &scheduler,
            TASK_COUNT,
            4U,
            controlled_execute,
            &context
        ) != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    for (index = 0U; index < TASK_COUNT; index++) {
        if (scheduler_submit(&scheduler, &tasks[index]) != SCHEDULER_OK) {
            goto cleanup;
        }
    }
    if (scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || context.invocation_count != TASK_COUNT
        || context.active_count != 0U
        || context.context_mismatch
        || memcmp(tasks, snapshots, sizeof(tasks)) != 0) {
        goto cleanup;
    }
    for (index = 0U; index < TASK_COUNT; index++) {
        if (context.execution_count[index] != 1U) {
            goto cleanup;
        }
    }
    invocations_after_join = context.invocation_count;
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK
        || context.invocation_count != invocations_after_join) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    cleanup_scheduler(&scheduler);
    execution_context_destroy(&context);
    if (status == EXIT_SUCCESS) {
        printf("SCHEDULER-SHUTDOWN-018 through -020 passed.\n");
        printf("SCHEDULER-JOIN-019 through -020 passed.\n");
    }
    return status;
}

static int test_scheduler_wraparound(void)
{
    enum {
        TASK_COUNT = 12
    };
    Scheduler scheduler = {0};
    ExecutionContext context = {0};
    Task tasks[TASK_COUNT];
    size_t index;
    int status = EXIT_FAILURE;

    for (index = 0U; index < TASK_COUNT; index++) {
        if (!task_init(
                &tasks[index],
                20000U + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                3U
            )) {
            return EXIT_FAILURE;
        }
        context.expected[index] = &tasks[index];
    }
    context.expected_count = TASK_COUNT;
    context.release_callbacks = 1;

    if (execution_context_init(&context) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (scheduler_init(&scheduler, 3U, 1U, controlled_execute, &context)
            != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    for (index = 0U; index < TASK_COUNT; index++) {
        if (scheduler_submit(&scheduler, &tasks[index]) != SCHEDULER_OK) {
            goto cleanup;
        }
    }
    if (scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || context.observed_count != TASK_COUNT) {
        goto cleanup;
    }
    for (index = 0U; index < TASK_COUNT; index++) {
        if (context.observed[index] != &tasks[index]
            || context.execution_count[index] != 1U) {
            goto cleanup;
        }
    }
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    cleanup_scheduler(&scheduler);
    execution_context_destroy(&context);
    if (status == EXIT_SUCCESS) {
        printf("PHASE4-AUDIT-001 and PHASE4-AUDIT-010 passed.\n");
    }
    return status;
}

static int test_multi_producer_worker_accounting(void)
{
    enum {
        PRODUCER_COUNT = 4,
        TASKS_PER_PRODUCER = 64,
        TOTAL_TASKS = PRODUCER_COUNT * TASKS_PER_PRODUCER
    };
    Scheduler scheduler = {0};
    ExecutionContext context = {0};
    Task tasks[PRODUCER_COUNT][TASKS_PER_PRODUCER];
    AuditProducerContext producers[PRODUCER_COUNT] = {0};
    SchedThread producer_threads[PRODUCER_COUNT] = {0};
    int producer_results[PRODUCER_COUNT] = {0};
    size_t producer;
    size_t task_index;
    size_t flat_index;
    size_t created = 0U;
    size_t joined = 0U;
    int status = EXIT_FAILURE;

    for (producer = 0U; producer < PRODUCER_COUNT; producer++) {
        for (task_index = 0U;
             task_index < TASKS_PER_PRODUCER;
             task_index++) {
            flat_index = producer * TASKS_PER_PRODUCER + task_index;
            if (!task_init(
                    &tasks[producer][task_index],
                    21000U + (uint64_t)flat_index,
                    TASK_PRIORITY_NORMAL,
                    4U
                )) {
                return EXIT_FAILURE;
            }
            context.expected[flat_index] = &tasks[producer][task_index];
        }
    }
    context.expected_count = TOTAL_TASKS;
    context.release_callbacks = 1;

    if (execution_context_init(&context) != EXIT_SUCCESS
        || scheduler_init(
            &scheduler,
            16U,
            4U,
            controlled_execute,
            &context
        ) != SCHEDULER_OK
        || scheduler_start(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }

    for (producer = 0U; producer < PRODUCER_COUNT; producer++) {
        producers[producer].scheduler = &scheduler;
        producers[producer].tasks = tasks[producer];
        producers[producer].task_count = TASKS_PER_PRODUCER;
        if (sched_thread_create(
                &producer_threads[producer],
                audit_producer,
                &producers[producer]
            ) != SCHED_SYNC_OK) {
            goto cleanup;
        }
        created++;
    }
    for (producer = 0U; producer < created; producer++) {
        if (sched_thread_join(
                &producer_threads[producer],
                &producer_results[producer]
            ) != SCHED_SYNC_OK
            || producer_results[producer] != 0
            || producers[producer].result != SCHEDULER_OK) {
            goto cleanup;
        }
        joined++;
    }
    if (scheduler_shutdown(&scheduler) != SCHEDULER_OK
        || scheduler_join(&scheduler) != SCHEDULER_OK
        || context.invocation_count != TOTAL_TASKS
        || context.observed_count != TOTAL_TASKS
        || context.context_mismatch) {
        goto cleanup;
    }
    for (flat_index = 0U; flat_index < TOTAL_TASKS; flat_index++) {
        if (context.execution_count[flat_index] != 1U) {
            goto cleanup;
        }
    }
    if (scheduler_destroy(&scheduler) != SCHEDULER_OK) {
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (created > joined) {
        (void)scheduler_shutdown(&scheduler);
    }
    for (producer = joined; producer < created; producer++) {
        (void)sched_thread_join(&producer_threads[producer], NULL);
    }
    for (producer = 0U; producer < created; producer++) {
        sched_thread_destroy(&producer_threads[producer]);
    }
    cleanup_scheduler(&scheduler);
    execution_context_destroy(&context);
    if (status == EXIT_SUCCESS) {
        printf("PHASE4-AUDIT-002 and PHASE4-AUDIT-009 passed.\n");
    }
    return status;
}

int main(void)
{
    if (test_public_api_and_result_names() != EXIT_SUCCESS
        || test_initialization_validation() != EXIT_SUCCESS
        || test_initialization_and_duplicate() != EXIT_SUCCESS
        || test_destroy_and_reinitialize() != EXIT_SUCCESS
        || test_start_validation() != EXIT_SUCCESS
        || test_start_lifecycle() != EXIT_SUCCESS
        || test_submission_validation() != EXIT_SUCCESS
        || test_concurrent_callbacks() != EXIT_SUCCESS
        || test_submit_capacity_and_blocking() != EXIT_SUCCESS
        || test_callback_failure_and_reuse() != EXIT_SUCCESS
        || test_shutdown_join_validation() != EXIT_SUCCESS
        || test_multi_worker_shutdown_drain() != EXIT_SUCCESS
        || test_scheduler_wraparound() != EXIT_SUCCESS
        || test_multi_producer_worker_accounting() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
