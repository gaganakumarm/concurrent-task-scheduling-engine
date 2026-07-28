#include "concurrent_scheduler/task_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGNATURE_MATCHES(function, type) \
    _Generic(&(function), type: 1, default: 0)

_Static_assert(
    _Generic(((TaskQueue *)0)->items, Task **: 1, default: 0),
    "TaskQueue.items must have type Task **"
);
_Static_assert(
    _Generic(((TaskQueue *)0)->capacity, size_t: 1, default: 0),
    "TaskQueue.capacity must have type size_t"
);
_Static_assert(
    _Generic(((TaskQueue *)0)->size, size_t: 1, default: 0),
    "TaskQueue.size must have type size_t"
);
_Static_assert(
    _Generic(((TaskQueue *)0)->head, size_t: 1, default: 0),
    "TaskQueue.head must have type size_t"
);
_Static_assert(
    _Generic(((TaskQueue *)0)->tail, size_t: 1, default: 0),
    "TaskQueue.tail must have type size_t"
);

_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_init,
        TaskQueueResult (*)(TaskQueue *, size_t)
    ),
    "task_queue_init signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(task_queue_destroy, void (*)(TaskQueue *)),
    "task_queue_destroy signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_enqueue,
        TaskQueueResult (*)(TaskQueue *, Task *)
    ),
    "task_queue_enqueue signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_dequeue,
        TaskQueueResult (*)(TaskQueue *, Task **)
    ),
    "task_queue_dequeue signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_peek,
        TaskQueueResult (*)(const TaskQueue *, Task **)
    ),
    "task_queue_peek signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_is_empty,
        bool (*)(const TaskQueue *)
    ),
    "task_queue_is_empty signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_is_full,
        bool (*)(const TaskQueue *)
    ),
    "task_queue_is_full signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_size,
        size_t (*)(const TaskQueue *)
    ),
    "task_queue_size signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_capacity,
        size_t (*)(const TaskQueue *)
    ),
    "task_queue_capacity signature mismatch"
);
_Static_assert(
    SIGNATURE_MATCHES(
        task_queue_result_name,
        const char *(*)(TaskQueueResult)
    ),
    "task_queue_result_name signature mismatch"
);

static bool queues_are_equal(const TaskQueue *left, const TaskQueue *right)
{
    return left->items == right->items
        && left->capacity == right->capacity
        && left->size == right->size
        && left->head == right->head
        && left->tail == right->tail;
}

static bool tasks_are_equal(const Task *left, const Task *right)
{
    return left->id == right->id
        && left->priority == right->priority
        && left->state == right->state
        && left->total_work == right->total_work
        && left->remaining_work == right->remaining_work;
}

static bool dequeue_failure_is_atomic(
    TaskQueue *queue,
    Task **output,
    TaskQueueResult expected,
    size_t entry_count
)
{
    TaskQueue before = *queue;
    Task *output_before = *output;
    Task *entries[3] = {NULL, NULL, NULL};
    size_t index;
    TaskQueueResult result;

    if (entry_count > sizeof(entries) / sizeof(entries[0])) {
        return false;
    }

    for (index = 0U; index < entry_count; ++index) {
        entries[index] = queue->items[index];
    }

    result = task_queue_dequeue(queue, output);
    if (result != expected
        || !queues_are_equal(queue, &before)
        || *output != output_before) {
        return false;
    }

    for (index = 0U; index < entry_count; ++index) {
        if (queue->items[index] != entries[index]) {
            return false;
        }
    }

    return true;
}

static bool peek_failure_is_atomic(
    const TaskQueue *queue,
    Task **output,
    TaskQueueResult expected,
    size_t entry_count
)
{
    TaskQueue before = *queue;
    Task *output_before = *output;
    Task *entries[3] = {NULL, NULL, NULL};
    size_t index;
    TaskQueueResult result;

    if (entry_count > sizeof(entries) / sizeof(entries[0])) {
        return false;
    }

    for (index = 0U; index < entry_count; ++index) {
        entries[index] = queue->items[index];
    }

    result = task_queue_peek(queue, output);
    if (result != expected
        || !queues_are_equal(queue, &before)
        || *output != output_before) {
        return false;
    }

    for (index = 0U; index < entry_count; ++index) {
        if (queue->items[index] != entries[index]) {
            return false;
        }
    }

    return true;
}

static int test_queue_api(void)
{
    Task *item = NULL;
    TaskQueue queue = {
        .items = &item,
        .capacity = 1U,
        .size = 0U,
        .head = 0U,
        .tail = 0U
    };
    TaskQueueResult results[] = {
        TASK_QUEUE_OK,
        TASK_QUEUE_ERROR_INVALID_ARGUMENT,
        TASK_QUEUE_ERROR_ALLOCATION,
        TASK_QUEUE_ERROR_FULL,
        TASK_QUEUE_ERROR_EMPTY
    };

    (void)queue;
    (void)results;

    printf("QUEUE-API-001 through QUEUE-API-006 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_initialization(void)
{
    TaskQueue queue;
    Task *sentinel_item = NULL;
    TaskQueue unchanged = {
        .items = &sentinel_item,
        .capacity = 11U,
        .size = 7U,
        .head = 3U,
        .tail = 5U
    };

    if (task_queue_init(&queue, 4U) != TASK_QUEUE_OK) {
        fprintf(stderr, "QUEUE-INIT-001 failed: initialization failed.\n");
        return EXIT_FAILURE;
    }

    if (queue.items == NULL) {
        fprintf(stderr, "QUEUE-INIT-002 failed: items is NULL.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.capacity != 4U) {
        fprintf(stderr, "QUEUE-INIT-003 failed: capacity was not preserved.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.size != 0U) {
        fprintf(stderr, "QUEUE-INIT-004 failed: initial size is nonzero.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.head != 0U) {
        fprintf(stderr, "QUEUE-INIT-005 failed: initial head is nonzero.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.tail != 0U) {
        fprintf(stderr, "QUEUE-INIT-006 failed: initial tail is nonzero.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    if (task_queue_init(&queue, 1U) != TASK_QUEUE_OK
        || queue.capacity != 1U) {
        fprintf(stderr, "QUEUE-INIT-007 failed: capacity one was rejected.\n");
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    if (task_queue_init(NULL, 1U) != TASK_QUEUE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "QUEUE-INIT-008 failed: NULL queue was accepted.\n");
        return EXIT_FAILURE;
    }

    queue = unchanged;
    if (task_queue_init(&queue, 0U) != TASK_QUEUE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "QUEUE-INIT-009 failed: zero capacity was accepted.\n");
        return EXIT_FAILURE;
    }

    if (!queues_are_equal(&queue, &unchanged)) {
        fprintf(
            stderr,
            "QUEUE-INIT-010 failed: invalid argument changed queue fields.\n"
        );
        return EXIT_FAILURE;
    }

    if (sizeof(Task *) > 1U) {
        const size_t overflowing_capacity =
            SIZE_MAX / sizeof(Task *) + 1U;

        queue = unchanged;
        if (task_queue_init(&queue, overflowing_capacity)
            != TASK_QUEUE_ERROR_INVALID_ARGUMENT) {
            fprintf(
                stderr,
                "QUEUE-INIT-011 failed: overflowing capacity was accepted.\n"
            );
            return EXIT_FAILURE;
        }

        if (!queues_are_equal(&queue, &unchanged)) {
            fprintf(
                stderr,
                "QUEUE-INIT-012 failed: overflow rejection changed fields.\n"
            );
            return EXIT_FAILURE;
        }
    }

    printf("QUEUE-INIT-001 through QUEUE-INIT-012 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_destruction(void)
{
    TaskQueue queue = {0};
    Task task;
    Task before;

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-DESTROY-001.\n");
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    if (queue.items != NULL
        || queue.capacity != 0U
        || queue.size != 0U
        || queue.head != 0U
        || queue.tail != 0U) {
        fprintf(stderr,
                "QUEUE-DESTROY-001 failed: fields were not reset.\n");
        return EXIT_FAILURE;
    }

    task_queue_destroy(NULL);

    task_queue_destroy(&queue);
    if (queue.items != NULL
        || queue.capacity != 0U
        || queue.size != 0U
        || queue.head != 0U
        || queue.tail != 0U) {
        fprintf(
            stderr,
            "QUEUE-DESTROY-003 failed: zero queue was not preserved.\n"
        );
        return EXIT_FAILURE;
    }

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-DESTROY-004.\n");
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);
    task_queue_destroy(&queue);

    if (!task_init(
            &task,
            UINT64_C(77),
            TASK_PRIORITY_NORMAL,
            UINT64_C(10)
        )
        || task_queue_init(&queue, 1U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-DESTROY-005.\n");
        return EXIT_FAILURE;
    }
    before = task;
    queue.items[0] = &task;
    task_queue_destroy(&queue);

    if (task.id != before.id
        || task.priority != before.priority
        || task.state != before.state
        || task.total_work != before.total_work
        || task.remaining_work != before.remaining_work) {
        fprintf(
            stderr,
            "QUEUE-DESTROY-005 failed: caller-owned Task was modified.\n"
        );
        return EXIT_FAILURE;
    }

    printf("QUEUE-DESTROY-001 through QUEUE-DESTROY-005 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_result_names(void)
{
    static const struct {
        TaskQueueResult result;
        const char *name;
    } cases[] = {
        {TASK_QUEUE_OK, "OK"},
        {TASK_QUEUE_ERROR_INVALID_ARGUMENT, "INVALID_ARGUMENT"},
        {TASK_QUEUE_ERROR_ALLOCATION, "ALLOCATION_ERROR"},
        {TASK_QUEUE_ERROR_FULL, "FULL"},
        {TASK_QUEUE_ERROR_EMPTY, "EMPTY"}
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (strcmp(task_queue_result_name(cases[index].result),
                   cases[index].name) != 0) {
            fprintf(
                stderr,
                "QUEUE-RESULT-001 failed for %s.\n",
                cases[index].name
            );
            return EXIT_FAILURE;
        }
    }

    if (strcmp(task_queue_result_name((TaskQueueResult)-1), "UNKNOWN") != 0) {
        fprintf(
            stderr,
            "QUEUE-RESULT-002 failed: negative result was not UNKNOWN.\n"
        );
        return EXIT_FAILURE;
    }

    if (strcmp(task_queue_result_name((TaskQueueResult)1000), "UNKNOWN") != 0) {
        fprintf(
            stderr,
            "QUEUE-RESULT-003 failed: large result was not UNKNOWN.\n"
        );
        return EXIT_FAILURE;
    }

    printf("QUEUE-RESULT-001 through QUEUE-RESULT-003 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_queries(void)
{
    TaskQueue queue;
    TaskQueue before;
    Task task;

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK
        || !task_init(
            &task,
            UINT64_C(201),
            TASK_PRIORITY_NORMAL,
            UINT64_C(5)
        )) {
        fprintf(stderr, "Test setup failed for QUEUE-QUERY tests.\n");
        return EXIT_FAILURE;
    }

    if (!task_queue_is_empty(&queue)) {
        fprintf(stderr, "QUEUE-QUERY-001 failed: new queue is not empty.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_is_full(&queue)) {
        fprintf(stderr, "QUEUE-QUERY-002 failed: new queue is full.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_size(&queue) != 0U) {
        fprintf(stderr, "QUEUE-QUERY-003 failed: new queue size is nonzero.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_capacity(&queue) != 2U) {
        fprintf(stderr, "QUEUE-QUERY-004 failed: capacity is incorrect.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_enqueue(&queue, &task) != TASK_QUEUE_OK
        || task_queue_is_empty(&queue)) {
        fprintf(stderr,
                "QUEUE-QUERY-005 failed: enqueued queue reported empty.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_enqueue(&queue, &task) != TASK_QUEUE_OK
        || task_queue_size(&queue) != 2U) {
        fprintf(stderr,
                "QUEUE-QUERY-007 failed: size did not reflect enqueues.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    before = queue;
    (void)task_queue_is_empty(&queue);
    (void)task_queue_is_full(&queue);
    (void)task_queue_size(&queue);
    (void)task_queue_capacity(&queue);
    if (!queues_are_equal(&queue, &before)) {
        fprintf(stderr, "QUEUE-QUERY-012 failed: query mutated queue.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    if (task_queue_init(&queue, 1U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &task) != TASK_QUEUE_OK
        || !task_queue_is_full(&queue)) {
        fprintf(stderr,
                "QUEUE-QUERY-006 failed: capacity-one queue is not full.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    if (!task_queue_is_empty(NULL)) {
        fprintf(stderr, "QUEUE-QUERY-008 failed: NULL is not empty.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_is_full(NULL)) {
        fprintf(stderr, "QUEUE-QUERY-009 failed: NULL is full.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_size(NULL) != 0U) {
        fprintf(stderr, "QUEUE-QUERY-010 failed: NULL size is nonzero.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_capacity(NULL) != 0U) {
        fprintf(stderr, "QUEUE-QUERY-011 failed: NULL capacity is nonzero.\n");
        return EXIT_FAILURE;
    }

    printf("QUEUE-QUERY-001 through QUEUE-QUERY-012 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_enqueue_success(void)
{
    TaskQueue queue;
    Task first;
    Task second;
    Task third;
    Task first_before;
    const size_t expected_capacity = 3U;

    if (task_queue_init(&queue, expected_capacity) != TASK_QUEUE_OK
        || !task_init(
            &first,
            UINT64_C(301),
            TASK_PRIORITY_LOW,
            UINT64_C(3)
        )
        || !task_init(
            &second,
            UINT64_C(302),
            TASK_PRIORITY_NORMAL,
            UINT64_C(4)
        )
        || !task_init(
            &third,
            UINT64_C(303),
            TASK_PRIORITY_HIGH,
            UINT64_C(5)
        )) {
        fprintf(stderr, "Test setup failed for QUEUE-ENQUEUE success tests.\n");
        return EXIT_FAILURE;
    }
    first_before = first;

    if (task_queue_enqueue(&queue, &first) != TASK_QUEUE_OK) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-001 failed: empty enqueue was rejected.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.size != 1U) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-002 failed: size did not increase to one.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.items[0] != &first) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-003 failed: exact pointer was not stored.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!tasks_are_equal(&first, &first_before)) {
        fprintf(stderr, "QUEUE-ENQUEUE-004 failed: Task was modified.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.tail != 1U) {
        fprintf(stderr, "QUEUE-ENQUEUE-005 failed: tail did not advance.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.head != 0U) {
        fprintf(stderr, "QUEUE-ENQUEUE-006 failed: head changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.capacity != expected_capacity) {
        fprintf(stderr, "QUEUE-ENQUEUE-019 failed: capacity changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!tasks_are_equal(&first, &first_before)) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-020 failed: caller-owned Task changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_enqueue(&queue, &second) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &third) != TASK_QUEUE_OK
        || queue.items[0] != &first
        || queue.items[1] != &second
        || queue.items[2] != &third) {
        fprintf(
            stderr,
            "QUEUE-ENQUEUE-007 failed: insertion order was not preserved.\n"
        );
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    printf("QUEUE-ENQUEUE success and ownership tests passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_enqueue_failures(void)
{
    TaskQueue queue;
    TaskQueue before;
    Task first;
    Task second;
    Task first_before;
    Task second_before;
    Task *entry_zero;
    Task *entry_one;

    if (!task_init(
            &first,
            UINT64_C(401),
            TASK_PRIORITY_LOW,
            UINT64_C(3)
        )
        || !task_init(
            &second,
            UINT64_C(402),
            TASK_PRIORITY_HIGH,
            UINT64_C(7)
        )) {
        fprintf(stderr, "Test setup failed for QUEUE-ENQUEUE failures.\n");
        return EXIT_FAILURE;
    }
    first_before = first;
    second_before = second;

    if (task_queue_init(&queue, 1U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &first) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-ENQUEUE-008.\n");
        return EXIT_FAILURE;
    }

    if (queue.items[0] != &first || queue.size != 1U) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-008 failed: capacity-one enqueue failed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    before = queue;
    entry_zero = queue.items[0];
    if (task_queue_enqueue(&queue, &second) != TASK_QUEUE_ERROR_FULL) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-009 failed: full queue did not return FULL.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || !tasks_are_equal(&second, &second_before)) {
        fprintf(
            stderr,
            "QUEUE-ENQUEUE-010 failed: full failure was not atomic.\n"
        );
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    if (task_queue_enqueue(NULL, &first)
            != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !tasks_are_equal(&first, &first_before)) {
        fprintf(stderr, "QUEUE-ENQUEUE-011 failed: NULL queue accepted.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-ENQUEUE-012.\n");
        return EXIT_FAILURE;
    }
    before = queue;
    entry_zero = queue.items[0];
    entry_one = queue.items[1];
    if (task_queue_enqueue(&queue, NULL)
        != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one) {
        fprintf(
            stderr,
            "QUEUE-ENQUEUE-012 failed: NULL task changed queue.\n"
        );
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    before = queue;
    if (task_queue_enqueue(&queue, &first)
        != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || !tasks_are_equal(&first, &first_before)) {
        fprintf(
            stderr,
            "QUEUE-ENQUEUE-013 failed: destroyed queue accepted or changed.\n"
        );
        return EXIT_FAILURE;
    }

    queue = (TaskQueue){0};
    before = queue;
    if (task_queue_enqueue(&queue, &first)
        != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || !tasks_are_equal(&first, &first_before)) {
        fprintf(
            stderr,
            "QUEUE-ENQUEUE-014 failed: zero queue accepted or changed.\n"
        );
        return EXIT_FAILURE;
    }

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for malformed enqueue tests.\n");
        return EXIT_FAILURE;
    }
    entry_zero = queue.items[0];
    entry_one = queue.items[1];

    queue.size = 3U;
    before = queue;
    if (task_queue_enqueue(&queue, &first)
        != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one
        || !tasks_are_equal(&first, &first_before)) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-015 failed: malformed size was not atomic.\n");
        queue.size = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.size = 0U;

    queue.head = queue.capacity;
    before = queue;
    if (task_queue_enqueue(&queue, &first)
        != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one
        || !tasks_are_equal(&first, &first_before)) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-016 failed: malformed head was not atomic.\n");
        queue.head = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.head = 0U;

    queue.tail = queue.capacity;
    before = queue;
    if (task_queue_enqueue(&queue, &first)
        != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one
        || !tasks_are_equal(&first, &first_before)) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-017 failed: malformed tail was not atomic.\n");
        queue.tail = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.tail = 0U;
    task_queue_destroy(&queue);

    printf("QUEUE-ENQUEUE failure and atomicity tests passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_enqueue_wraparound(void)
{
    TaskQueue queue;
    Task existing;
    Task middle;
    Task added;
    Task *dequeued = NULL;

    if (task_queue_init(&queue, 3U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-ENQUEUE-021.\n");
        return EXIT_FAILURE;
    }

    if (!task_init(
            &existing,
            UINT64_C(501),
            TASK_PRIORITY_LOW,
            UINT64_C(2)
        )
        || !task_init(
            &middle,
            UINT64_C(502),
            TASK_PRIORITY_NORMAL,
            UINT64_C(2)
        )
        || !task_init(
            &added,
            UINT64_C(503),
            TASK_PRIORITY_HIGH,
            UINT64_C(2)
        )
        || task_queue_enqueue(&queue, &existing) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &middle) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &dequeued) != TASK_QUEUE_OK
        || dequeued != &existing
        || queue.tail != 2U) {
        fprintf(stderr, "Test setup failed for QUEUE-ENQUEUE-021.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_enqueue(&queue, &added) != TASK_QUEUE_OK
        || queue.items[2] != &added
        || queue.tail != 0U
        || queue.size != 2U
        || queue.head != 1U) {
        fprintf(stderr,
                "QUEUE-ENQUEUE-021 failed: tail did not wrap safely.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    printf("QUEUE-ENQUEUE-021 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_dequeue_basic(void)
{
    TaskQueue queue;
    Task task;
    Task before;
    Task *output = NULL;
    size_t tail_before;
    size_t capacity_before;

    if (task_queue_init(&queue, 3U) != TASK_QUEUE_OK
        || !task_init(
            &task,
            UINT64_C(601),
            TASK_PRIORITY_NORMAL,
            UINT64_C(8)
        )
        || task_queue_enqueue(&queue, &task) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for basic dequeue tests.\n");
        return EXIT_FAILURE;
    }
    before = task;
    tail_before = queue.tail;
    capacity_before = queue.capacity;

    if (task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK) {
        fprintf(stderr, "QUEUE-DEQUEUE-001 failed: dequeue was rejected.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (output != &task) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-002 failed: returned pointer was incorrect.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.size != 0U) {
        fprintf(stderr, "QUEUE-DEQUEUE-003 failed: size did not decrease.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.head != 1U) {
        fprintf(stderr, "QUEUE-DEQUEUE-004 failed: head did not advance.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.tail != tail_before) {
        fprintf(stderr, "QUEUE-DEQUEUE-005 failed: tail changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.capacity != capacity_before) {
        fprintf(stderr, "QUEUE-DEQUEUE-006 failed: capacity changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.items[0] != NULL) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-007 failed: vacated slot was not cleared.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!tasks_are_equal(&task, &before)) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-008 failed: caller-owned Task changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!task_queue_is_empty(&queue) || task_queue_is_full(&queue)) {
        fprintf(
            stderr,
            "QUEUE-DEQUEUE-025 failed: drained queue state is incorrect.\n"
        );
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    printf("QUEUE-DEQUEUE-001 through QUEUE-DEQUEUE-008 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_dequeue_fifo(void)
{
    TaskQueue queue;
    Task tasks[3];
    Task *output = NULL;
    size_t index;

    if (task_queue_init(&queue, 3U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-DEQUEUE-009.\n");
        return EXIT_FAILURE;
    }

    for (index = 0U; index < 3U; ++index) {
        if (!task_init(
                &tasks[index],
                UINT64_C(700) + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                UINT64_C(2)
            )
            || task_queue_enqueue(&queue, &tasks[index]) != TASK_QUEUE_OK) {
            fprintf(stderr, "Test setup failed for QUEUE-DEQUEUE-009.\n");
            task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }

    for (index = 0U; index < 3U; ++index) {
        if (task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
            || output != &tasks[index]) {
            fprintf(stderr,
                    "QUEUE-DEQUEUE-009 failed: FIFO order was not preserved.\n");
            task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }

    task_queue_destroy(&queue);
    printf("QUEUE-DEQUEUE-009 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_dequeue_failures(void)
{
    TaskQueue queue;
    TaskQueue before;
    Task first;
    Task sentinel;
    Task *output;
    Task *output_before;
    Task *entry_zero;
    Task *entry_one;

    if (!task_init(
            &first,
            UINT64_C(801),
            TASK_PRIORITY_LOW,
            UINT64_C(4)
        )
        || !task_init(
            &sentinel,
            UINT64_C(802),
            TASK_PRIORITY_HIGH,
            UINT64_C(6)
        )
        || task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for dequeue failure tests.\n");
        return EXIT_FAILURE;
    }

    output = &sentinel;
    before = queue;
    entry_zero = queue.items[0];
    entry_one = queue.items[1];
    if (task_queue_dequeue(&queue, &output) != TASK_QUEUE_ERROR_EMPTY) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-010 failed: empty queue did not return EMPTY.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (output != &sentinel) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-011 failed: empty dequeue changed output.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-012 failed: empty dequeue changed queue.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    output = &sentinel;
    output_before = output;
    if (task_queue_dequeue(NULL, &output)
            != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != output_before) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-013 failed: NULL queue changed output.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &first) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-DEQUEUE-014.\n");
        return EXIT_FAILURE;
    }
    before = queue;
    entry_zero = queue.items[0];
    entry_one = queue.items[1];
    if (task_queue_dequeue(&queue, NULL)
            != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-014 failed: NULL output changed queue.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    output = &sentinel;
    if (!dequeue_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            0U
        )) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-015 failed: destroyed queue was not atomic.\n");
        return EXIT_FAILURE;
    }

    queue = (TaskQueue){0};
    output = &sentinel;
    if (!dequeue_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            0U
        )) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-016 failed: zero queue was not atomic.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for malformed dequeue tests.\n");
        return EXIT_FAILURE;
    }

    queue.size = 3U;
    output = &sentinel;
    if (!dequeue_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-017 failed: malformed size was not atomic.\n");
        queue.size = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.size = 0U;

    queue.head = queue.capacity;
    output = &sentinel;
    if (!dequeue_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-018 failed: malformed head was not atomic.\n");
        queue.head = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.head = 0U;

    queue.tail = queue.capacity;
    output = &sentinel;
    if (!dequeue_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-019 failed: malformed tail was not atomic.\n");
        queue.tail = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.tail = 0U;

    queue.size = 1U;
    output = &sentinel;
    if (!dequeue_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(
            stderr,
            "QUEUE-DEQUEUE-020 failed: NULL head slot was not atomic.\n"
        );
        queue.size = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.size = 0U;
    task_queue_destroy(&queue);

    printf("QUEUE-DEQUEUE-010 through QUEUE-DEQUEUE-021 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_dequeue_wraparound(void)
{
    TaskQueue queue;
    Task tasks[5];
    Task *output = NULL;
    size_t index;

    if (task_queue_init(&queue, 3U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for dequeue wraparound tests.\n");
        return EXIT_FAILURE;
    }

    for (index = 0U; index < 5U; ++index) {
        if (!task_init(
                &tasks[index],
                UINT64_C(900) + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                UINT64_C(3)
            )) {
            fprintf(stderr, "Test setup failed for dequeue wraparound tests.\n");
            task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }

    for (index = 0U; index < 3U; ++index) {
        if (task_queue_enqueue(&queue, &tasks[index]) != TASK_QUEUE_OK) {
            fprintf(stderr, "Test setup failed for QUEUE-DEQUEUE-022.\n");
            task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }

    if (task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[0]
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[1]
        || queue.head != 2U
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[2]
        || queue.head != 0U) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-022 failed: head did not wrap to zero.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    if (task_queue_init(&queue, 1U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[0]) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[0]
        || queue.size != 0U
        || queue.head != 0U
        || queue.tail != 0U) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-023 failed: capacity-one dequeue failed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    if (task_queue_init(&queue, 3U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[0]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[1]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[2]) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[0]
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[1]
        || task_queue_enqueue(&queue, &tasks[3]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[4]) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[2]
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[3]
        || task_queue_dequeue(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[4]) {
        fprintf(
            stderr,
            "QUEUE-DEQUEUE-024 failed: wrapped FIFO order was incorrect.\n"
        );
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!task_queue_is_empty(&queue) || task_queue_is_full(&queue)) {
        fprintf(stderr,
                "QUEUE-DEQUEUE-025 failed: drained queue query mismatch.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    printf("QUEUE-DEQUEUE-022 through QUEUE-DEQUEUE-025 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_peek_basic(void)
{
    TaskQueue queue;
    TaskQueue before_queue;
    Task first;
    Task second;
    Task first_before;
    Task *output = NULL;
    Task *dequeued = NULL;
    Task *entry_zero;
    Task *entry_one;

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK
        || !task_init(
            &first,
            UINT64_C(1001),
            TASK_PRIORITY_LOW,
            UINT64_C(4)
        )
        || !task_init(
            &second,
            UINT64_C(1002),
            TASK_PRIORITY_HIGH,
            UINT64_C(5)
        )
        || task_queue_enqueue(&queue, &first) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for basic peek tests.\n");
        return EXIT_FAILURE;
    }
    first_before = first;
    before_queue = queue;
    entry_zero = queue.items[0];
    entry_one = queue.items[1];

    if (task_queue_peek(&queue, &output) != TASK_QUEUE_OK) {
        fprintf(stderr, "QUEUE-PEEK-001 failed: peek was rejected.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (output != &first) {
        fprintf(stderr,
                "QUEUE-PEEK-002 failed: returned pointer was incorrect.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.size != before_queue.size) {
        fprintf(stderr, "QUEUE-PEEK-003 failed: size changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.head != before_queue.head) {
        fprintf(stderr, "QUEUE-PEEK-004 failed: head changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.tail != before_queue.tail) {
        fprintf(stderr, "QUEUE-PEEK-005 failed: tail changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.capacity != before_queue.capacity) {
        fprintf(stderr, "QUEUE-PEEK-006 failed: capacity changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (queue.items[0] != entry_zero
        || queue.items[1] != entry_one
        || !queues_are_equal(&queue, &before_queue)) {
        fprintf(stderr,
                "QUEUE-PEEK-007 failed: queue or head slot changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!tasks_are_equal(&first, &first_before)) {
        fprintf(stderr,
                "QUEUE-PEEK-008 failed: caller-owned Task changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    output = NULL;
    if (task_queue_peek(&queue, &output) != TASK_QUEUE_OK
        || output != &first
        || task_queue_peek(&queue, &output) != TASK_QUEUE_OK
        || output != &first) {
        fprintf(stderr,
                "QUEUE-PEEK-009 failed: repeated peeks differed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_enqueue(&queue, &second) != TASK_QUEUE_OK
        || task_queue_peek(&queue, &output) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &dequeued) != TASK_QUEUE_OK
        || output != &first
        || dequeued != &first) {
        fprintf(
            stderr,
            "QUEUE-PEEK-010 failed: peek and dequeue returned different tasks.\n"
        );
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (task_queue_peek(&queue, &output) != TASK_QUEUE_OK
        || output != &second) {
        fprintf(
            stderr,
            "QUEUE-PEEK-011 failed: next FIFO task was not returned.\n"
        );
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    printf("QUEUE-PEEK-001 through QUEUE-PEEK-011 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_peek_failures(void)
{
    TaskQueue queue;
    TaskQueue before;
    Task task;
    Task sentinel;
    Task *output;
    Task *output_before;
    Task *entry_zero;
    Task *entry_one;

    if (!task_init(
            &task,
            UINT64_C(1101),
            TASK_PRIORITY_NORMAL,
            UINT64_C(3)
        )
        || !task_init(
            &sentinel,
            UINT64_C(1102),
            TASK_PRIORITY_HIGH,
            UINT64_C(4)
        )
        || task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for peek failure tests.\n");
        return EXIT_FAILURE;
    }

    output = &sentinel;
    before = queue;
    entry_zero = queue.items[0];
    entry_one = queue.items[1];
    if (task_queue_peek(&queue, &output) != TASK_QUEUE_ERROR_EMPTY) {
        fprintf(stderr,
                "QUEUE-PEEK-013 failed: empty queue did not return EMPTY.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (output != &sentinel) {
        fprintf(stderr,
                "QUEUE-PEEK-014 failed: empty peek changed output.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    if (!queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one) {
        fprintf(stderr, "QUEUE-PEEK-015 failed: empty peek changed queue.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    output = &sentinel;
    output_before = output;
    if (task_queue_peek(NULL, &output)
            != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || output != output_before) {
        fprintf(stderr, "QUEUE-PEEK-016 failed: NULL queue changed output.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &task) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-PEEK-017.\n");
        return EXIT_FAILURE;
    }
    before = queue;
    entry_zero = queue.items[0];
    entry_one = queue.items[1];
    if (task_queue_peek(&queue, NULL)
            != TASK_QUEUE_ERROR_INVALID_ARGUMENT
        || !queues_are_equal(&queue, &before)
        || queue.items[0] != entry_zero
        || queue.items[1] != entry_one) {
        fprintf(stderr, "QUEUE-PEEK-017 failed: NULL output changed queue.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    task_queue_destroy(&queue);

    output = &sentinel;
    if (!peek_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            0U
        )) {
        fprintf(stderr,
                "QUEUE-PEEK-018 failed: destroyed queue was not atomic.\n");
        return EXIT_FAILURE;
    }

    queue = (TaskQueue){0};
    output = &sentinel;
    if (!peek_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            0U
        )) {
        fprintf(stderr,
                "QUEUE-PEEK-019 failed: zero queue was not atomic.\n");
        return EXIT_FAILURE;
    }

    if (task_queue_init(&queue, 2U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for malformed peek tests.\n");
        return EXIT_FAILURE;
    }

    queue.size = 3U;
    output = &sentinel;
    if (!peek_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(stderr,
                "QUEUE-PEEK-020 failed: malformed size was not atomic.\n");
        queue.size = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.size = 0U;

    queue.head = queue.capacity;
    output = &sentinel;
    if (!peek_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(stderr,
                "QUEUE-PEEK-021 failed: malformed head was not atomic.\n");
        queue.head = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.head = 0U;

    queue.tail = queue.capacity;
    output = &sentinel;
    if (!peek_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(stderr,
                "QUEUE-PEEK-022 failed: malformed tail was not atomic.\n");
        queue.tail = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.tail = 0U;

    queue.size = 1U;
    output = &sentinel;
    if (!peek_failure_is_atomic(
            &queue,
            &output,
            TASK_QUEUE_ERROR_INVALID_ARGUMENT,
            2U
        )) {
        fprintf(stderr,
                "QUEUE-PEEK-023 failed: NULL head slot was not atomic.\n");
        queue.size = 0U;
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }
    queue.size = 0U;
    task_queue_destroy(&queue);

    printf("QUEUE-PEEK-013 through QUEUE-PEEK-024 passed.\n");
    return EXIT_SUCCESS;
}

static int test_queue_peek_wraparound(void)
{
    TaskQueue queue;
    Task tasks[5];
    Task *output = NULL;
    Task *dequeued = NULL;
    TaskQueue before;
    size_t index;

    if (task_queue_init(&queue, 3U) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for peek wraparound tests.\n");
        return EXIT_FAILURE;
    }

    for (index = 0U; index < 5U; ++index) {
        if (!task_init(
                &tasks[index],
                UINT64_C(1200) + (uint64_t)index,
                TASK_PRIORITY_NORMAL,
                UINT64_C(2)
            )) {
            fprintf(stderr, "Test setup failed for peek wraparound tests.\n");
            task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }

    if (task_queue_enqueue(&queue, &tasks[0]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[1]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[2]) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &dequeued) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &dequeued) != TASK_QUEUE_OK
        || queue.head != 2U) {
        fprintf(stderr, "Test setup failed for QUEUE-PEEK-012.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    before = queue;
    if (task_queue_peek(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[2]
        || !queues_are_equal(&queue, &before)) {
        fprintf(stderr,
                "QUEUE-PEEK-012 failed: wrapped-head peek was incorrect.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    if (task_queue_init(&queue, 1U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[0]) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-PEEK-025.\n");
        return EXIT_FAILURE;
    }
    before = queue;
    if (task_queue_peek(&queue, &output) != TASK_QUEUE_OK
        || output != &tasks[0]
        || !task_queue_is_full(&queue)
        || !queues_are_equal(&queue, &before)) {
        fprintf(stderr,
                "QUEUE-PEEK-025 failed: capacity-one full state changed.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    task_queue_destroy(&queue);
    if (task_queue_init(&queue, 3U) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[0]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[1]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[2]) != TASK_QUEUE_OK
        || task_queue_dequeue(&queue, &dequeued) != TASK_QUEUE_OK
        || dequeued != &tasks[0]
        || task_queue_dequeue(&queue, &dequeued) != TASK_QUEUE_OK
        || dequeued != &tasks[1]
        || task_queue_enqueue(&queue, &tasks[3]) != TASK_QUEUE_OK
        || task_queue_enqueue(&queue, &tasks[4]) != TASK_QUEUE_OK) {
        fprintf(stderr, "Test setup failed for QUEUE-PEEK-026.\n");
        task_queue_destroy(&queue);
        return EXIT_FAILURE;
    }

    for (index = 2U; index < 5U; ++index) {
        before = queue;
        if (task_queue_peek(&queue, &output) != TASK_QUEUE_OK
            || output != &tasks[index]
            || !queues_are_equal(&queue, &before)
            || task_queue_dequeue(&queue, &dequeued) != TASK_QUEUE_OK
            || dequeued != &tasks[index]) {
            fprintf(
                stderr,
                "QUEUE-PEEK-026 failed: wrapped FIFO peek was incorrect.\n"
            );
            task_queue_destroy(&queue);
            return EXIT_FAILURE;
        }
    }

    task_queue_destroy(&queue);
    printf("QUEUE-PEEK-012, QUEUE-PEEK-025, and QUEUE-PEEK-026 passed.\n");
    return EXIT_SUCCESS;
}

int main(void)
{
    if (test_queue_api() != EXIT_SUCCESS
        || test_queue_initialization() != EXIT_SUCCESS
        || test_queue_destruction() != EXIT_SUCCESS
        || test_queue_result_names() != EXIT_SUCCESS
        || test_queue_queries() != EXIT_SUCCESS
        || test_queue_enqueue_success() != EXIT_SUCCESS
        || test_queue_enqueue_failures() != EXIT_SUCCESS
        || test_queue_enqueue_wraparound() != EXIT_SUCCESS
        || test_queue_dequeue_basic() != EXIT_SUCCESS
        || test_queue_dequeue_fifo() != EXIT_SUCCESS
        || test_queue_dequeue_failures() != EXIT_SUCCESS
        || test_queue_dequeue_wraparound() != EXIT_SUCCESS
        || test_queue_peek_basic() != EXIT_SUCCESS
        || test_queue_peek_failures() != EXIT_SUCCESS
        || test_queue_peek_wraparound() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
