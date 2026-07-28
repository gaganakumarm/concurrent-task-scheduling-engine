#include "concurrent_scheduler/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    TaskState value;
    const char *name;
} StateCase;

typedef struct {
    TaskPriority value;
    const char *name;
} PriorityCase;

typedef struct {
    TaskState initial;
    TaskState next;
    const char *test_id;
} TransitionCase;

static bool tasks_are_equal(const Task *left, const Task *right)
{
    return left->id == right->id
        && left->priority == right->priority
        && left->state == right->state
        && left->total_work == right->total_work
        && left->remaining_work == right->remaining_work;
}

static bool prepare_task_in_state(Task *task, TaskState state)
{
    if (!task_init(
            task,
            UINT64_C(1001),
            TASK_PRIORITY_HIGH,
            UINT64_C(20)
        )) {
        return false;
    }

    switch (state) {
    case TASK_STATE_CREATED:
        return true;
    case TASK_STATE_QUEUED:
        return task_transition_state(task, TASK_STATE_QUEUED);
    case TASK_STATE_RUNNING:
        return task_transition_state(task, TASK_STATE_QUEUED)
            && task_transition_state(task, TASK_STATE_RUNNING);
    case TASK_STATE_COMPLETED:
        return task_transition_state(task, TASK_STATE_QUEUED)
            && task_transition_state(task, TASK_STATE_RUNNING)
            && task_transition_state(task, TASK_STATE_COMPLETED);
    case TASK_STATE_FAILED:
        return task_transition_state(task, TASK_STATE_QUEUED)
            && task_transition_state(task, TASK_STATE_RUNNING)
            && task_transition_state(task, TASK_STATE_FAILED);
    case TASK_STATE_CANCELLED:
        return task_transition_state(task, TASK_STATE_CANCELLED);
    case TASK_STATE_REJECTED:
        return task_transition_state(task, TASK_STATE_REJECTED);
    default:
        return false;
    }
}

static int test_states(void)
{
    static const StateCase cases[] = {
        {TASK_STATE_CREATED, "CREATED"},
        {TASK_STATE_QUEUED, "QUEUED"},
        {TASK_STATE_RUNNING, "RUNNING"},
        {TASK_STATE_COMPLETED, "COMPLETED"},
        {TASK_STATE_FAILED, "FAILED"},
        {TASK_STATE_CANCELLED, "CANCELLED"},
        {TASK_STATE_REJECTED, "REJECTED"}
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (!task_state_is_valid(cases[index].value)) {
            fprintf(stderr, "STATE-001 failed for %s.\n", cases[index].name);
            return EXIT_FAILURE;
        }

        if (strcmp(task_state_name(cases[index].value), cases[index].name) != 0) {
            fprintf(stderr, "STATE-004 failed for %s: received %s.\n",
                    cases[index].name, task_state_name(cases[index].value));
            return EXIT_FAILURE;
        }
    }

    if (task_state_is_valid((TaskState)-1)) {
        fprintf(stderr, "STATE-002 failed: negative state was accepted.\n");
        return EXIT_FAILURE;
    }

    if (task_state_is_valid((TaskState)1000)) {
        fprintf(stderr, "STATE-003 failed: large state was accepted.\n");
        return EXIT_FAILURE;
    }

    if (strcmp(task_state_name((TaskState)-1), "UNKNOWN") != 0
        || strcmp(task_state_name((TaskState)1000), "UNKNOWN") != 0) {
        fprintf(stderr, "STATE-005 failed: invalid state name was not UNKNOWN.\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int test_priorities(void)
{
    static const PriorityCase cases[] = {
        {TASK_PRIORITY_LOW, "LOW"},
        {TASK_PRIORITY_NORMAL, "NORMAL"},
        {TASK_PRIORITY_HIGH, "HIGH"}
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (!task_priority_is_valid(cases[index].value)) {
            fprintf(stderr, "PRIORITY-001 failed for %s.\n", cases[index].name);
            return EXIT_FAILURE;
        }

        if (strcmp(task_priority_name(cases[index].value),
                   cases[index].name) != 0) {
            fprintf(stderr, "PRIORITY-004 failed for %s: received %s.\n",
                    cases[index].name,
                    task_priority_name(cases[index].value));
            return EXIT_FAILURE;
        }
    }

    if (task_priority_is_valid((TaskPriority)-1)) {
        fprintf(stderr, "PRIORITY-002 failed: negative priority was accepted.\n");
        return EXIT_FAILURE;
    }

    if (task_priority_is_valid((TaskPriority)1000)) {
        fprintf(stderr, "PRIORITY-003 failed: large priority was accepted.\n");
        return EXIT_FAILURE;
    }

    if (strcmp(task_priority_name((TaskPriority)-1), "UNKNOWN") != 0
        || strcmp(task_priority_name((TaskPriority)1000), "UNKNOWN") != 0) {
        fprintf(stderr,
                "PRIORITY-005 failed: invalid priority name was not UNKNOWN.\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int test_task_structure(void)
{
    Task first = {
        .id = UINT64_C(4294967296),
        .priority = TASK_PRIORITY_HIGH,
        .state = TASK_STATE_QUEUED,
        .total_work = UINT64_C(100),
        .remaining_work = UINT64_C(75)
    };
    Task second = {
        .id = UINT64_C(17),
        .priority = TASK_PRIORITY_LOW,
        .state = TASK_STATE_CREATED,
        .total_work = UINT64_C(20),
        .remaining_work = UINT64_C(20)
    };

    if (first.id != UINT64_C(4294967296)) {
        fprintf(stderr, "TASK-STRUCT-002 failed: 64-bit ID was not preserved.\n");
        return EXIT_FAILURE;
    }

    if (first.priority != TASK_PRIORITY_HIGH) {
        fprintf(stderr, "TASK-STRUCT-003 failed: priority was not preserved.\n");
        return EXIT_FAILURE;
    }

    if (first.state != TASK_STATE_QUEUED) {
        fprintf(stderr, "TASK-STRUCT-004 failed: state was not preserved.\n");
        return EXIT_FAILURE;
    }

    if (second.id != UINT64_C(17)
        || second.priority != TASK_PRIORITY_LOW
        || second.state != TASK_STATE_CREATED) {
        fprintf(stderr,
                "TASK-STRUCT-005 failed: tasks did not retain independent values.\n");
        return EXIT_FAILURE;
    }

    printf("TASK-STRUCT-001 through TASK-STRUCT-006 passed.\n");
    return EXIT_SUCCESS;
}

static int test_task_initialization(void)
{
    Task task = {
        .id = UINT64_C(99),
        .priority = TASK_PRIORITY_LOW,
        .state = TASK_STATE_FAILED,
        .total_work = UINT64_C(12),
        .remaining_work = UINT64_C(3)
    };
    const Task unchanged = task;
    const uint64_t expected_id = UINT64_C(4294967297);

    if (task_init(NULL, expected_id, TASK_PRIORITY_NORMAL, UINT64_C(10))) {
        fprintf(stderr, "TASK-INIT-005 failed: NULL task was accepted.\n");
        return EXIT_FAILURE;
    }

    if (task_init(
            &task,
            expected_id,
            (TaskPriority)1000,
            UINT64_C(10)
        )) {
        fprintf(stderr, "TASK-INIT-006 failed: invalid priority was accepted.\n");
        return EXIT_FAILURE;
    }

    if (task.id != unchanged.id
        || task.priority != unchanged.priority
        || task.state != unchanged.state
        || task.total_work != unchanged.total_work
        || task.remaining_work != unchanged.remaining_work) {
        fprintf(stderr,
                "TASK-INIT-007 failed: rejected initialization changed the task.\n");
        return EXIT_FAILURE;
    }

    if (!task_init(
            &task,
            expected_id,
            TASK_PRIORITY_HIGH,
            UINT64_C(10)
        )) {
        fprintf(stderr, "TASK-INIT-001 failed: valid initialization failed.\n");
        return EXIT_FAILURE;
    }

    if (task.id != expected_id) {
        fprintf(stderr, "TASK-INIT-002 failed: ID was not stored correctly.\n");
        return EXIT_FAILURE;
    }

    if (task.priority != TASK_PRIORITY_HIGH) {
        fprintf(stderr,
                "TASK-INIT-003 failed: priority was not stored correctly.\n");
        return EXIT_FAILURE;
    }

    if (task.state != TASK_STATE_CREATED) {
        fprintf(stderr,
                "TASK-INIT-004 failed: initial state was not CREATED.\n");
        return EXIT_FAILURE;
    }

    printf("TASK-INIT-001 through TASK-INIT-007 passed.\n");
    return EXIT_SUCCESS;
}

static int test_task_state_transitions(void)
{
    static const TransitionCase permitted[] = {
        {TASK_STATE_CREATED, TASK_STATE_QUEUED, "001"},
        {TASK_STATE_CREATED, TASK_STATE_CANCELLED, "002"},
        {TASK_STATE_CREATED, TASK_STATE_REJECTED, "003"},
        {TASK_STATE_QUEUED, TASK_STATE_RUNNING, "004"},
        {TASK_STATE_QUEUED, TASK_STATE_CANCELLED, "005"},
        {TASK_STATE_RUNNING, TASK_STATE_COMPLETED, "006"},
        {TASK_STATE_RUNNING, TASK_STATE_FAILED, "007"}
    };
    static const TaskState terminal_states[] = {
        TASK_STATE_COMPLETED,
        TASK_STATE_FAILED,
        TASK_STATE_CANCELLED,
        TASK_STATE_REJECTED
    };
    Task task = {
        .id = UINT64_C(1),
        .priority = TASK_PRIORITY_NORMAL,
        .state = TASK_STATE_CREATED,
        .total_work = UINT64_C(10),
        .remaining_work = UINT64_C(10)
    };
    size_t index;

    for (index = 0U;
         index < sizeof(permitted) / sizeof(permitted[0]);
         ++index) {
        task.state = permitted[index].initial;

        if (!task_transition_state(&task, permitted[index].next)
            || task.state != permitted[index].next) {
            fprintf(
                stderr,
                "TASK-STATE-TRANSITION-%s failed: %s to %s was rejected.\n",
                permitted[index].test_id,
                task_state_name(permitted[index].initial),
                task_state_name(permitted[index].next)
            );
            return EXIT_FAILURE;
        }
    }

    task.state = TASK_STATE_CREATED;
    if (task_transition_state(&task, TASK_STATE_CREATED)) {
        fprintf(stderr,
                "TASK-STATE-TRANSITION-008 failed: self-transition succeeded.\n");
        return EXIT_FAILURE;
    }

    for (index = 0U;
         index < sizeof(terminal_states) / sizeof(terminal_states[0]);
         ++index) {
        task.state = terminal_states[index];
        if (task_transition_state(&task, TASK_STATE_QUEUED)) {
            fprintf(
                stderr,
                "TASK-STATE-TRANSITION-009 failed: terminal state %s changed.\n",
                task_state_name(terminal_states[index])
            );
            return EXIT_FAILURE;
        }
    }

    task.state = TASK_STATE_CREATED;
    if (task_transition_state(&task, (TaskState)1000)) {
        fprintf(
            stderr,
            "TASK-STATE-TRANSITION-010 failed: invalid state was accepted.\n"
        );
        return EXIT_FAILURE;
    }

    if (task_transition_state(NULL, TASK_STATE_QUEUED)) {
        fprintf(
            stderr,
            "TASK-STATE-TRANSITION-011 failed: NULL task was accepted.\n"
        );
        return EXIT_FAILURE;
    }

    task.state = TASK_STATE_CREATED;
    if (task_transition_state(&task, TASK_STATE_COMPLETED)
        || task.state != TASK_STATE_CREATED) {
        fprintf(
            stderr,
            "TASK-STATE-TRANSITION-012 failed: rejected transition changed state.\n"
        );
        return EXIT_FAILURE;
    }

    printf("TASK-STATE-TRANSITION-001 through -012 passed.\n");
    return EXIT_SUCCESS;
}

static int test_task_work_metadata(void)
{
    Task task = {
        .id = UINT64_C(41),
        .priority = TASK_PRIORITY_LOW,
        .state = TASK_STATE_FAILED,
        .total_work = UINT64_C(9),
        .remaining_work = UINT64_C(4)
    };
    const Task unchanged = task;
    Task maximum;
    Task first;
    Task second;
    const uint64_t expected_work = UINT64_C(250);

    if (!task_init(
            &task,
            UINT64_C(42),
            TASK_PRIORITY_NORMAL,
            expected_work
        )) {
        fprintf(stderr, "TASK-WORK-001 failed: initialization failed.\n");
        return EXIT_FAILURE;
    }

    if (task.total_work != expected_work) {
        fprintf(stderr, "TASK-WORK-001 failed: total work was not preserved.\n");
        return EXIT_FAILURE;
    }

    if (task.remaining_work != expected_work) {
        fprintf(stderr,
                "TASK-WORK-002 failed: remaining work differs from total work.\n");
        return EXIT_FAILURE;
    }

    task = unchanged;
    if (task_init(&task, UINT64_C(42), TASK_PRIORITY_HIGH, UINT64_C(0))) {
        fprintf(stderr, "TASK-WORK-003 failed: zero total work was accepted.\n");
        return EXIT_FAILURE;
    }

    if (task.id != unchanged.id
        || task.priority != unchanged.priority
        || task.state != unchanged.state
        || task.total_work != unchanged.total_work
        || task.remaining_work != unchanged.remaining_work) {
        fprintf(stderr,
                "TASK-WORK-004 failed: zero-work rejection changed the task.\n");
        return EXIT_FAILURE;
    }

    if (!task_init(
            &maximum,
            UINT64_C(43),
            TASK_PRIORITY_HIGH,
            UINT64_MAX
        )
        || maximum.total_work != UINT64_MAX
        || maximum.remaining_work != UINT64_MAX) {
        fprintf(stderr, "TASK-WORK-005 failed: UINT64_MAX was not preserved.\n");
        return EXIT_FAILURE;
    }

    if (task_is_complete(&maximum)) {
        fprintf(stderr,
                "TASK-WORK-006 failed: initialized task reported complete.\n");
        return EXIT_FAILURE;
    }

    maximum.remaining_work = UINT64_C(0);
    if (!task_is_complete(&maximum)) {
        fprintf(stderr,
                "TASK-WORK-007 failed: exhausted task reported incomplete.\n");
        return EXIT_FAILURE;
    }

    if (task_is_complete(NULL)) {
        fprintf(stderr, "TASK-WORK-008 failed: NULL task reported complete.\n");
        return EXIT_FAILURE;
    }

    if (!task_init(
            &task,
            UINT64_C(44),
            TASK_PRIORITY_NORMAL,
            expected_work
        )
        || !task_transition_state(&task, TASK_STATE_QUEUED)
        || task.total_work != expected_work
        || task.remaining_work != expected_work) {
        fprintf(
            stderr,
            "TASK-WORK-009 failed: state transition changed work metadata.\n"
        );
        return EXIT_FAILURE;
    }

    if (!task_init(&first, UINT64_C(45), TASK_PRIORITY_LOW, UINT64_C(3))
        || !task_init(&second, UINT64_C(46), TASK_PRIORITY_HIGH, UINT64_C(7))
        || first.total_work != UINT64_C(3)
        || first.remaining_work != UINT64_C(3)
        || second.total_work != UINT64_C(7)
        || second.remaining_work != UINT64_C(7)) {
        fprintf(
            stderr,
            "TASK-WORK-010 failed: tasks did not retain independent work.\n"
        );
        return EXIT_FAILURE;
    }

    printf("TASK-WORK-001 through TASK-WORK-010 passed.\n");
    return EXIT_SUCCESS;
}

static int test_task_work_consumption(void)
{
    static const TaskState disallowed_states[] = {
        TASK_STATE_CREATED,
        TASK_STATE_QUEUED,
        TASK_STATE_COMPLETED,
        TASK_STATE_FAILED,
        TASK_STATE_CANCELLED,
        TASK_STATE_REJECTED
    };
    static const char *const disallowed_test_ids[] = {
        "011", "012", "013", "014", "015", "016"
    };
    Task task;
    Task before;
    size_t index;

    if (!prepare_task_in_state(&task, TASK_STATE_RUNNING)
        || !task_consume_work(&task, UINT64_C(7))) {
        fprintf(stderr,
                "TASK-CONSUME-001 failed: partial consumption was rejected.\n");
        return EXIT_FAILURE;
    }

    if (task.remaining_work != UINT64_C(13)) {
        fprintf(stderr,
                "TASK-CONSUME-002 failed: incorrect remaining work.\n");
        return EXIT_FAILURE;
    }

    if (task.state != TASK_STATE_RUNNING) {
        fprintf(stderr,
                "TASK-CONSUME-003 failed: partial work changed the state.\n");
        return EXIT_FAILURE;
    }

    if (task.id != UINT64_C(1001)
        || task.priority != TASK_PRIORITY_HIGH
        || task.total_work != UINT64_C(20)) {
        fprintf(stderr,
                "TASK-CONSUME-021 failed: immutable fields changed.\n");
        return EXIT_FAILURE;
    }

    if (task_is_complete(&task)) {
        fprintf(stderr,
                "TASK-CONSUME-023 failed: partial work reported complete.\n");
        return EXIT_FAILURE;
    }

    if (!prepare_task_in_state(&task, TASK_STATE_RUNNING)
        || !task_consume_work(&task, UINT64_C(20))) {
        fprintf(stderr,
                "TASK-CONSUME-004 failed: exact consumption was rejected.\n");
        return EXIT_FAILURE;
    }

    if (task.remaining_work != UINT64_C(0)) {
        fprintf(stderr,
                "TASK-CONSUME-005 failed: exact consumption left work.\n");
        return EXIT_FAILURE;
    }

    if (task.state != TASK_STATE_COMPLETED) {
        fprintf(stderr,
                "TASK-CONSUME-006 failed: exhaustion did not complete task.\n");
        return EXIT_FAILURE;
    }

    if (!task_is_complete(&task)) {
        fprintf(stderr,
                "TASK-CONSUME-022 failed: exhausted task reported incomplete.\n");
        return EXIT_FAILURE;
    }

    if (task_consume_work(NULL, UINT64_C(1))) {
        fprintf(stderr, "TASK-CONSUME-007 failed: NULL task was accepted.\n");
        return EXIT_FAILURE;
    }

    if (!prepare_task_in_state(&task, TASK_STATE_RUNNING)) {
        fprintf(stderr, "Test setup failed for TASK-CONSUME-008.\n");
        return EXIT_FAILURE;
    }
    before = task;
    if (task_consume_work(&task, UINT64_C(0))
        || !tasks_are_equal(&task, &before)) {
        fprintf(stderr,
                "TASK-CONSUME-008 failed: zero work was accepted or mutated.\n");
        return EXIT_FAILURE;
    }

    before = task;
    if (task_consume_work(&task, UINT64_C(21))) {
        fprintf(stderr,
                "TASK-CONSUME-009 failed: excessive work was accepted.\n");
        return EXIT_FAILURE;
    }

    if (!tasks_are_equal(&task, &before)) {
        fprintf(
            stderr,
            "TASK-CONSUME-010 failed: excessive rejection changed fields.\n"
        );
        return EXIT_FAILURE;
    }

    for (index = 0U;
         index < sizeof(disallowed_states) / sizeof(disallowed_states[0]);
         ++index) {
        if (!prepare_task_in_state(&task, disallowed_states[index])) {
            fprintf(
                stderr,
                "Test setup failed for TASK-CONSUME-%s.\n",
                disallowed_test_ids[index]
            );
            return EXIT_FAILURE;
        }

        before = task;
        if (task_consume_work(&task, UINT64_C(1))
            || !tasks_are_equal(&task, &before)) {
            fprintf(
                stderr,
                "TASK-CONSUME-%s failed: state %s allowed work or mutated.\n",
                disallowed_test_ids[index],
                task_state_name(disallowed_states[index])
            );
            return EXIT_FAILURE;
        }
    }

    if (!prepare_task_in_state(&task, TASK_STATE_RUNNING)) {
        fprintf(stderr, "Test setup failed for TASK-CONSUME-017.\n");
        return EXIT_FAILURE;
    }
    task.remaining_work = UINT64_C(0);
    before = task;
    if (task_consume_work(&task, UINT64_C(1))
        || !tasks_are_equal(&task, &before)) {
        fprintf(
            stderr,
            "TASK-CONSUME-017 failed: exhausted task allowed work or mutated.\n"
        );
        return EXIT_FAILURE;
    }

    if (!prepare_task_in_state(&task, TASK_STATE_RUNNING)) {
        fprintf(stderr, "Test setup failed for TASK-CONSUME-018.\n");
        return EXIT_FAILURE;
    }
    task.state = (TaskState)1000;
    before = task;
    if (task_consume_work(&task, UINT64_C(1))
        || !tasks_are_equal(&task, &before)) {
        fprintf(
            stderr,
            "TASK-CONSUME-018 failed: invalid state allowed work or mutated.\n"
        );
        return EXIT_FAILURE;
    }

    if (!prepare_task_in_state(&task, TASK_STATE_RUNNING)
        || !task_consume_work(&task, UINT64_C(3))
        || !task_consume_work(&task, UINT64_C(5))
        || task.remaining_work != UINT64_C(12)
        || task.state != TASK_STATE_RUNNING) {
        fprintf(
            stderr,
            "TASK-CONSUME-019 failed: partial work did not accumulate.\n"
        );
        return EXIT_FAILURE;
    }

    if (!task_init(
            &task,
            UINT64_C(1002),
            TASK_PRIORITY_NORMAL,
            UINT64_MAX
        )
        || !task_transition_state(&task, TASK_STATE_QUEUED)
        || !task_transition_state(&task, TASK_STATE_RUNNING)
        || !task_consume_work(&task, UINT64_MAX - UINT64_C(1))
        || task.remaining_work != UINT64_C(1)
        || !task_consume_work(&task, UINT64_C(1))
        || task.remaining_work != UINT64_C(0)
        || task.state != TASK_STATE_COMPLETED) {
        fprintf(
            stderr,
            "TASK-CONSUME-020 failed: UINT64_MAX consumption was unsafe.\n"
        );
        return EXIT_FAILURE;
    }

    printf("TASK-CONSUME-001 through TASK-CONSUME-023 passed.\n");
    return EXIT_SUCCESS;
}

int main(void)
{
    if (test_states() != EXIT_SUCCESS
        || test_priorities() != EXIT_SUCCESS
        || test_task_structure() != EXIT_SUCCESS
        || test_task_initialization() != EXIT_SUCCESS
        || test_task_state_transitions() != EXIT_SUCCESS
        || test_task_work_metadata() != EXIT_SUCCESS
        || test_task_work_consumption() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    printf("Task state and priority domain tests passed.\n");
    return EXIT_SUCCESS;
}
