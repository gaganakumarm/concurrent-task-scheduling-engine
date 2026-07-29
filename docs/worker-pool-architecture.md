# Worker Pool and Scheduler Lifecycle Architecture

## 1. Purpose

This document defines the Worker Pool and Scheduler Lifecycle design for graceful queue shutdown, a
fixed-size worker pool, callback-based task execution, scheduler lifecycle,
failure cleanup, and deterministic testing. It is an architecture decision and
implementation plan only. It does not introduce production behavior.

## 2. Existing synchronized queue foundation

The synchronized queue foundation provides:

- caller-owned `Task` objects;
- a fixed-capacity, non-thread-safe `TaskQueue`;
- a `ConcurrentTaskQueue` wrapper with a private mutex and not-empty and
  not-full conditions;
- synchronized queries, non-blocking operations, and indefinitely blocking
  enqueue and dequeue;
- an internal mutex, condition-variable, and thread abstraction; and
- a Windows backend that keeps native types private.

The synchronization abstraction already declares
`sched_condition_broadcast`, and the Windows backend implements it with
`WakeAllConditionVariable`. Worker Pool and Scheduler Lifecycle needs broader backend tests for broadcast,
not a second broadcast operation.

Verification step 4.3 implements the queue-level shutdown state and wake-up semantics
described below. Verification step 4.4 audits its linearization, exact accounting,
wraparound, waiter release, and destruction constraints. Scheduler and
worker-pool behavior remains deferred.

## 3. Worker Pool and Scheduler Lifecycle Goals

Worker Pool and Scheduler Lifecycle will provide:

- an irreversible, idempotent queue shutdown request;
- wake-up of every blocked producer and consumer;
- graceful draining of queued Tasks;
- rejection of submissions after shutdown begins;
- a caller-allocated scheduler with private implementation state;
- a fixed number of worker threads and contexts;
- callback execution outside the queue mutex;
- explicit initialization, start, shutdown, join, and destruction;
- cleanup after partial worker startup;
- deterministic joining before queue destruction; and
- explicit Task and resource lifetime rules.

## 4. Non-Goals

Worker Pool and Scheduler Lifecycle excludes dynamic worker scaling, work stealing, thread priorities, CPU
affinity, delayed or recurring scheduling, dependency graphs, retries,
persistence, networking, process pools, lock-free queues, forced termination,
in-flight cancellation, production logging, public metrics, a POSIX backend,
priority scheduling, fairness guarantees, futures, promises, asynchronous
result objects, and scheduler restart.

## 5. Component Ownership

The scheduler owns:

- one `ConcurrentTaskQueue`;
- the fixed arrays of `SchedThread` objects and worker contexts;
- the callback pointer and optional callback context;
- a private lifecycle mutex, worker counts, and private worker exit
  status; and
- its private lifecycle state.

The queue owns its pointer buffer and synchronization resources, but owns no
Task. Workers borrow their contexts and scheduler resources. They do not own
the scheduler, queue, callback context, or Tasks.

The scheduler must outlive its queue, worker arrays, contexts, and all worker
threads. The controlling application must keep the scheduler and callback
context alive until all workers have joined.

## 6. Queue Shutdown Model

`ConcurrentTaskQueue` now has a private `bool shutdown_requested`, protected
by the existing queue mutex. Shutdown is irreversible until destruction and
reinitialization.

The first valid shutdown request:

1. locks the queue mutex;
2. sets `shutdown_requested` to true;
3. broadcasts not-empty;
4. broadcasts not-full; and
5. unlocks the mutex.

Repeated valid requests perform no state change and normally return success.
No poison pill, sentinel Task, null pointer, or forced termination represents
shutdown.

Every valid request attempts both broadcasts, including a repeated request.
This keeps the operation idempotent while permitting another wake attempt if a
previous backend operation reported failure. The implementation must attempt
both broadcasts and the mutex unlock even if one broadcast fails, preserve the
irreversible shutdown state, and return `SYSTEM_ERROR` if any required
synchronization operation fails. A repeated call returns `OK` when its own
broadcasts and unlock succeed.

Storage queries remain independent of shutdown: size and capacity remain
accurate, while empty and full describe only queue storage.

## 7. Drain-Before-Exit Decision

Worker Pool and Scheduler Lifecycle uses graceful draining, not immediate discard.

After shutdown becomes visible, queued Tasks remain in FIFO order and may be
removed. Consumers exit only when shutdown is active and the queue is empty.
This avoids silently abandoning accepted Tasks and gives a precise acceptance
boundary: every successful enqueue before shutdown remains eligible for one
callback attempt.

The scheduler cannot promise callback success, only that an accepted Task is
drained to a worker unless an infrastructure failure prevents it. Any Tasks
that cannot be attempted after infrastructure failure remain caller-owned;
Worker Pool and Scheduler Lifecycle must document that the caller needs external lifetime and accounting
to identify such Tasks. It does not add a return-unprocessed API.

## 8. Condition Broadcast Requirement

Shutdown broadcasts both conditions while holding the queue mutex because any
number of producers may be waiting for space and any number of consumers may
be waiting for work. Signal is insufficient for a terminal state change:
every waiter must wake and re-evaluate the shutdown predicate.

The existing backend operation is sufficient:

```c
SchedSyncResult sched_condition_broadcast(SchedCondition *condition);
```

Verification step 4.2 validates the existing operation with invalid-argument and
deterministic multiple-waiter broadcast tests using test-owned predicates and
synchronization. Native Windows types remain confined to the backend. This
backend verification step did not implement queue shutdown. Verification step 4.3 now uses
the validated operation to broadcast both not-empty and not-full.

## 9. Queue API Changes

Verification step 4.3 adds the smallest required public mutation API:

```c
ConcurrentTaskQueueResult concurrent_task_queue_shutdown(
    ConcurrentTaskQueue *queue
);
```

A public `concurrent_task_queue_is_shutdown` query is deferred. It is not
needed for correctness because operation results communicate the terminal
condition, and an observation-only query would become stale immediately.

Operation semantics are:

| Operation | Active queue | Shutdown with items | Shutdown and empty |
|---|---|---|---|
| try enqueue | enqueue or `FULL` | `SHUTDOWN` | `SHUTDOWN` |
| blocking enqueue | enqueue or wait | `SHUTDOWN` | `SHUTDOWN` |
| try dequeue | dequeue or `EMPTY` | dequeue | `SHUTDOWN` |
| blocking dequeue | dequeue or wait | dequeue | `SHUTDOWN` |
| try peek | peek or `EMPTY` | peek | `SHUTDOWN` |

Failed enqueue after shutdown leaves the queue and Task unchanged. A dequeue or
peek that returns `SHUTDOWN` preserves caller output.

Blocking enqueue waits conceptually while the queue is full and shutdown is
false. After waking, shutdown takes precedence over available capacity, so no
insertion can linearize after shutdown.

Blocking dequeue waits conceptually while the queue is empty and shutdown is
false. After waking, an available item takes precedence over shutdown so the
queue drains; otherwise it returns `SHUTDOWN`.

## 10. Scheduler Public API Proposal

Verification step 4.5 adopts these public foundation types:

```c
typedef struct {
    void *implementation;
} Scheduler;

typedef int (*SchedulerTaskExecuteFunction)(
    Task *task,
    void *context
);

typedef enum {
    SCHEDULER_OK,
    SCHEDULER_ERROR_INVALID_ARGUMENT,
    SCHEDULER_ERROR_INVALID_STATE,
    SCHEDULER_ERROR_ALLOCATION,
    SCHEDULER_ERROR_QUEUE_FULL,
    SCHEDULER_ERROR_SHUTDOWN,
    SCHEDULER_ERROR_SYSTEM
} SchedulerResult;

SchedulerResult scheduler_init(
    Scheduler *scheduler,
    size_t queue_capacity,
    size_t worker_count,
    SchedulerTaskExecuteFunction execute,
    void *execute_context
);

SchedulerResult scheduler_start(Scheduler *scheduler);
SchedulerResult scheduler_submit(Scheduler *scheduler, Task *task);
SchedulerResult scheduler_try_submit(Scheduler *scheduler, Task *task);
SchedulerResult scheduler_shutdown(Scheduler *scheduler);
SchedulerResult scheduler_join(Scheduler *scheduler);
SchedulerResult scheduler_destroy(Scheduler *scheduler);
const char *scheduler_result_name(SchedulerResult result);
```

Init, start, blocking and nonblocking submission, graceful shutdown, join,
destroy, and result-name lookup are implemented.

Initialization and start remain separate. This makes allocation failure
and partial thread-creation failure independently testable, prevents submission
during startup, and preserves the eventual explicit lifecycle:

```text
init -> start -> submit -> shutdown -> join -> destroy
```

`scheduler_submit` delegates to blocking queue enqueue, while
`scheduler_try_submit` delegates to nonblocking enqueue. Both register under
the lifecycle gate before entering the queue and deregister afterward.

### Worker start decision

The private implementation contains:

- one owned `ConcurrentTaskQueue`;
- the borrowed execution callback;
- the borrowed optional callback context;
- the immutable configured worker count;
- contiguous thread and worker-context arrays;
- the started and ready worker counts;
- one worker readiness and accounting mutex and condition;
- one lifecycle mutex and condition with active-submitter count; and
- the private lifecycle state.

Each worker context contains the owning implementation pointer and its stable
zero-based index. The arrays and worker synchronization are allocated during
start, not initialization.

Initialization validates the zero-initialized public wrapper and arguments,
allocates a local zeroed implementation, initializes its empty queue, stores
configuration, sets `INITIALIZED`, and publishes the pointer last. Unexpected
queue invalid-argument or terminal results map to scheduler system error;
queue allocation maps to scheduler allocation error.

Start moves `INITIALIZED` to `STARTING`, creates every fixed worker, and waits
under the worker mutex until all report ready before moving to `RUNNING`.
Workers enter blocking queue dequeue after readiness. Partial creation failure
shuts down the queue and joins all created workers before entering `FAILED`.
Normal destruction never stops workers: shutdown and join are explicit.

## 11. Scheduler Lifecycle States

The private implementation uses one enum:

```text
INITIALIZED -> STARTING -> RUNNING -> SHUTTING_DOWN -> STOPPED
                     \-----------------------------> FAILED
```

`UNINITIALIZED` is represented by a null public implementation pointer, not a
value stored in an unavailable implementation. `STARTING` makes partial
startup explicit. `FAILED` records a completed startup cleanup or worker
infrastructure failure. Both `STOPPED` and `FAILED` have no active workers and
are destructible.

The transitions are:

- successful init publishes `INITIALIZED`;
- start changes `INITIALIZED` to `STARTING`;
- complete worker readiness changes `STARTING` to `RUNNING`;
- startup failure performs full started-worker cleanup, then changes to
  `FAILED`;
- shutdown changes `RUNNING` to `SHUTTING_DOWN`;
- repeated shutdown in `SHUTTING_DOWN` or `STOPPED` succeeds;
- join changes `SHUTTING_DOWN` to `STOPPED` after all joins and handle
  destruction;
- infrastructure failure may cause queue shutdown and is reported by join;
  complete cleanup still reaches `STOPPED`, while a retained unjoined worker
  leaves `FAILED` for a later cleanup attempt.

Start twice returns `INVALID_STATE`. Verification step 4.8 uses all six private
lifecycle states; null implementation represents uninitialized. Lifecycle
calls are externally serialized. The worker mutex protects readiness and
callback accounting. A lifecycle mutex protects state and active-submitter
registration.

Public outcomes for externally observable states are:

| State | start | submit | shutdown | join | destroy |
|---|---|---|---|---|---|
| uninitialized | invalid state | invalid state | invalid state | invalid state | OK |
| `INITIALIZED` | allowed | invalid state | invalid state | invalid state | allowed |
| `STARTING` | invalid state | invalid state | invalid state | invalid state | invalid state |
| `RUNNING` | invalid state | allowed | closes gate | invalid state | invalid state |
| `SHUTTING_DOWN` | invalid state | shutdown result | OK | waits and joins | invalid state |
| `STOPPED` | invalid state | shutdown result | OK | OK | allowed |
| cleaned `FAILED` | invalid state | invalid state | invalid state | cleanup if needed | allowed when no workers remain |

Restart is unsupported.

## 12. Worker Loop

Each worker repeatedly:

1. calls blocking queue dequeue;
2. exits normally on `CONCURRENT_TASK_QUEUE_ERROR_SHUTDOWN`;
3. on another queue error, records infrastructure failure, requests queue
   shutdown, and exits with an infrastructure-error thread result;
4. on success, calls the execution callback with the exact removed Task pointer
   and shared scheduler callback context;
5. records private attempted/succeeded/failed counts; and
6. continues regardless of ordinary callback failure.

The callback runs after dequeue has released the queue mutex. One worker
attempts one Task at a time. Thread return values are fixed signed `int`
constants for normal exit, infrastructure failure, and invalid context.
Detailed status stays in the worker context.

The private counters record callback success and failure and are not exposed as
public metrics or completion results.

## 13. Task Callback Contract

The callback receives:

- the exact Task pointer removed from the queue; and
- the scheduler-level optional context supplied at initialization.

A return value of zero means callback success; nonzero means callback failure.
The callback executes on a worker thread outside the queue mutex. It may run
concurrently with the same callback for other Tasks, so synchronization of
shared callback context is the caller's responsibility.

The scheduler does not interpret callback return values as ownership transfer,
does not free or copy the Task, and does not automatically mutate Task state.
Any future Task lifecycle integration requires a separate contract.

## 14. Task Ownership

The caller owns every Task before submission, while queued, during callback
execution, and after callback return. Submission grants the scheduler only a
temporary reference.

The caller must keep an accepted Task alive until external coordination proves
that its callback attempt has finished or that it remained unprocessed after
an infrastructure failure. Worker Pool and Scheduler Lifecycle exposes no future or per-Task completion
handle. Tests can observe completion through test-owned callback state.

The callback must not invalidate a Task before returning. Neither queue nor
scheduler frees, copies, or transfers ownership of a Task.

## 15. Callback Failure Handling

Callback failure is a task-execution outcome, not a scheduler infrastructure
failure. The worker records it privately and continues to dequeue work. It
does not shut down the queue, terminate the worker, or stop the pool.

Callback failure is not a `SchedulerResult` because submit reports queue
acceptance and join reports worker infrastructure health, not per-Task
execution success. Public aggregate or per-Task results are deferred.

## 16. Infrastructure Failure Handling

A queue `SYSTEM_ERROR`, invalid worker context, failed join, or inconsistent
thread exit is an infrastructure failure. The affected worker stores the first
error and requests queue shutdown before exiting when it can do so safely.
Shutdown rejects producers and wakes peers.

Other workers continue the drain-before-exit protocol. Because one worker has
been lost, remaining workers may drain accepted work; if all workers fail,
queued references can remain. The scheduler never frees those Tasks. Join
attempts every started worker even after one join fails and returns
`SCHEDULER_ERROR_SYSTEM` if any worker or join reports infrastructure failure.

Queue shutdown failure is also preserved as an infrastructure error. Cleanup
must never pretend that active threads are gone; destruction remains forbidden
until every successfully started thread has been joined.

## 17. Partial Startup Failure

Submission is invalid in `INITIALIZED` and `STARTING`, so no user Task can be
queued during startup. If creating worker N fails:

1. stop creating workers;
2. record the exact number already started;
3. request queue shutdown;
4. wake all started workers;
5. join every successfully started worker;
6. destroy every joined thread object;
7. record any cleanup failure;
8. set the scheduler state to `FAILED`; and
9. return `SCHEDULER_ERROR_SYSTEM`.

The arrays and queue remain owned by the scheduler so `scheduler_destroy` can
release them. `scheduler_start` does not return while a successfully started
worker remains active.

Deterministic injection of thread-creation failure must not be added to
production. Test coverage should be achieved at the internal backend boundary
or through a test build that substitutes the internal synchronization
implementation at link time, without public hooks.

## 18. Public Shutdown Sequence

The controlling application performs:

1. stop creating new submitter threads;
2. call `scheduler_shutdown`;
3. allow queue shutdown to reject and wake blocked submitters;
4. join all external submitter threads so none remains inside submit;
5. allow workers to drain accepted Tasks;
6. call `scheduler_join`; and
7. call `scheduler_destroy`.

`scheduler_shutdown` is idempotent and non-joining. It changes scheduler state
before requesting queue shutdown so new scheduler submissions are rejected.
Queue shutdown then linearizes under the queue mutex and broadcasts both
conditions.

## 19. Public Join Sequence

`scheduler_join` is valid only after shutdown has begun. It:

1. joins every successfully started worker, even if an earlier join fails;
2. reads each deterministic thread result and private worker status;
3. destroys each thread handle only after its successful join;
4. records aggregate infrastructure failure;
5. sets `started_worker_count` to zero only for workers proven joined;
6. marks `STOPPED` if all exits were normal, otherwise `FAILED`; and
7. returns `OK` or `SYSTEM_ERROR`.

Join does not report callback failure. Repeated join after a completed join may
return `OK` idempotently; join before start or before shutdown returns
`INVALID_STATE`.

## 20. Destruction After Join

Destroy accepts `INITIALIZED`, `STOPPED`, or safely cleaned `FAILED` state.
It rejects `STARTING`, `RUNNING`, and `SHUTTING_DOWN`, never closes the gate,
never shuts down the queue, and never joins workers. After join has released
worker resources, destroy releases lifecycle synchronization, the shutdown
queue, private implementation storage, and finally clears the wrapper.

## 21. Thread-Safety Guarantees

After successful start:

- multiple threads may call `scheduler_submit` and
  `scheduler_try_submit`;
- queue operations remain synchronized;
- workers may execute callbacks concurrently;
- shutdown wakes blocked scheduler submitters through queue shutdown; and
- successful submission linearizes before shutdown or is rejected after it.

The scheduler provides no Task-memory synchronization beyond passing the
pointer to a worker through the synchronized queue. The caller must coordinate
all other Task access and callback-context access.

## 22. Unsupported Concurrent Lifecycle Operations

`scheduler_start` and `scheduler_destroy` are externally serialized and must
not race with one another. The private lifecycle mutex protects state and
active-submitter count. Submission holds it only to register while `RUNNING`
and to deregister after the queue call; it is never held while acquiring or
waiting on the queue mutex.

Destroy closes the gate, shuts down the queue, and waits on the lifecycle
condition until registered submitters return. The application still owns and
joins external submitter thread handles.

## 23. Result Mapping

Queue results map explicitly. Try-submit maps `OK`, `FULL`, and `SHUTDOWN` to
their scheduler equivalents. Blocking submit maps `OK` and `SHUTDOWN`.
Because public arguments are validated first, queue `INVALID_ARGUMENT`,
`ALLOCATION_ERROR`, `EMPTY`, `SYSTEM_ERROR`, unexpected blocking `FULL`, and
unknown values map to `SCHEDULER_ERROR_SYSTEM`. Enum numeric values are never
assumed equivalent. Callback results are not mapped.

## 24. Memory and Resource Ownership

| Resource | Owner | Lifetime and release |
|---|---|---|
| public `Scheduler` storage | caller | remains valid through destroy |
| scheduler private implementation | scheduler | init to successful destroy |
| scheduler lifecycle mutex and condition | scheduler implementation | init through final destroy |
| `ConcurrentTaskQueue` | scheduler implementation | init to destroy after joins |
| queue pointer buffer | queue | released by queue destroy |
| queue mutex and conditions | queue | released by queue destroy |
| worker mutex and condition | scheduler implementation | start to post-join cleanup |
| thread array | scheduler implementation | allocated at start, freed after joins |
| worker-context array | scheduler implementation | allocated at start, freed after joins |
| native thread handles | scheduler | create to destroy after join |
| callback pointer | borrowed immutable value | init to destroy |
| callback context | caller | must outlive all callbacks and joins |
| `Task` objects | caller | never freed or copied by scheduler |

The scheduler uses overflow-checked allocation for both arrays. A private
worker context refers back to the scheduler implementation and stores its
stable worker index; neither value conveys ownership.

## 25. Linearization Points

- Queue shutdown: writing `shutdown_requested = true` under the queue mutex.
- Successful enqueue: the underlying FIFO insertion under the queue mutex.
- Rejected enqueue: observing shutdown under the queue mutex.
- Successful dequeue: the underlying FIFO removal under the queue mutex.
- Shutdown dequeue/peek: observing both shutdown and empty under the mutex.
- Scheduler start: changing `STARTING` to `RUNNING` after every worker reports
  ready; public lifecycle calls are externally serialized.
- Worker readiness: incrementing `ready_worker_count` under the worker mutex.
- Active submitter registration and deregistration: changing the count under
  the lifecycle mutex.
- Scheduler shutdown: the queue shutdown write; the scheduler state changes
  first under the lifecycle mutex only to close the public submission gate.
- Successful scheduler submit: the queue insertion, not the callback.
- Worker task attempt: entry into the callback after dequeue.
- Worker exit: return from the worker thread function.
- Join completion: successful join of the final started worker.
- Destroy completion: clearing the public implementation pointer after all
  owned resources are released.

These points define race expectations without depending on wake order.

| Operation | Entry/lifecycle observation | Linearization | Possible later failure |
|---|---|---|---|
| init | validate zero wrapper | publish complete implementation | none after publication |
| start | observe `INITIALIZED` | set `STARTING`, then `RUNNING` after readiness | cleanup may report system failure before RUNNING |
| submit | register while RUNNING | queue insertion | signaling/unlock may fail after acceptance |
| try-submit full | register while RUNNING | observe full queue | deregistration may report system failure |
| shutdown | observe RUNNING | close gate, then set queue shutdown | broadcast/unlock may fail without reopening |
| join | observe SHUTTING_DOWN | each native join; final STOPPED publication | worker result or join cleanup may report system failure |
| destroy | observe inactive eligible state | reset wrapper after cleanup | no private ownership remains afterward |
| worker ready | valid stable context | increment ready count under worker mutex | notification failure prevents RUNNING |
| worker dequeue | blocking queue call | FIFO removal under queue mutex | post-removal signaling failure preserves ownership |
| callback | successful dequeue | callback entry is an execution attempt, not submission | nonzero result is private task outcome |

## Scheduler Race Audit

| Race | Protected ordering and legal outcome |
|---|---|
| concurrent submitter registration | lifecycle mutex serializes count increments; both may register while RUNNING |
| blocking submit versus gate closure | registration wins and queue decides acceptance/shutdown, or gate wins and submit returns shutdown |
| try-submit versus gate closure | registration wins and queue returns OK/full/shutdown, or gate wins |
| insertion immediately before queue shutdown | accepted pointer remains drainable exactly once |
| registered submit observes shutdown | no insertion; exact pointer remains caller-owned |
| dequeue versus shutdown with work | removal wins and callback runs |
| dequeue versus shutdown empty | worker observes shutdown and exits |
| callback versus shutdown return | callback may continue; shutdown is non-joining |
| callback versus join | join waits without holding worker-required locks |
| final callback versus worker exit | callback returns before the worker’s next shutdown dequeue and exit |
| repeated shutdown | SHUTTING_DOWN/STOPPED returns OK without reopening |
| repeated join | STOPPED returns OK without backend calls |
| destroy while RUNNING | rejected without cleanup |
| destroy while SHUTTING_DOWN | rejected until join |
| destroy after STOPPED | final cleanup and wrapper reset |
| worker infrastructure failure versus shutdown | queue shutdown is idempotent; join reports infrastructure result |
| callback nonzero versus later work | private failure count increments; worker continues |
| adjacent multi-worker dequeues | FIFO removals serialize; callback completion order is unrestricted |
| submission versus duplicate start | duplicate start observes RUNNING and fails; submission remains valid |
| reinitialize after destroy | null wrapper begins a new independent lifetime |

## 26. Test Strategy

Queue shutdown tests will cover:

- empty and non-empty shutdown;
- repeated shutdown;
- enqueue rejection and unchanged Task/queue state;
- drain of queued Tasks in FIFO order;
- dequeue and peek results for empty shutdown;
- wake-up of one and many blocked producers;
- wake-up of one and many blocked consumers;
- output preservation on failure;
- synchronized queries after shutdown; and
- no Task mutation or ownership transfer.

Scheduler tests will cover:

- argument, allocation-overflow, and state validation;
- exact fixed worker creation and start-twice rejection;
- blocking and non-blocking submission;
- exact-once callback attempts across multiple workers;
- callback execution outside the queue lock;
- callback failure followed by successful later work;
- graceful draining and worker exit;
- repeated shutdown and join;
- submit rejection and blocked-submit wake-up;
- partial-start cleanup;
- infrastructure-error aggregation;
- destruction constraints; and
- absence of resource and Task ownership leaks.

All concurrent tests use test-owned mutexes, conditions, predicate loops,
barriers, and fixed counts. They use no sleep, elapsed-time threshold, busy
wait, forced termination, or private production hook.

## 27. Implementation sequence

1. **Verification step 4.1:** approve this architecture only.
2. **Verification step 4.2:** audit existing broadcast support and add backend
   multiple-waiter and invalid-argument tests.
3. **Verification step 4.3:** implemented private queue shutdown state, the idempotent
   public shutdown operation, shutdown-aware enqueue/dequeue/peek predicates,
   and focused deterministic wake-up and drain tests.
4. **Verification step 4.4:** add deterministic queue shutdown concurrency tests and
   complete a queue-level audit. This audit is now complete locally, including
   mixed producer-consumer drain and 32-Task exact-accounting stress coverage.
5. **Verification step 4.5:** implemented scheduler public types, result mapping,
   failure-atomic initialization, inactive destruction, tests, and private
   layout without workers.
6. **Verification step 4.6:** implemented fixed worker creation, stable contexts,
   readiness coordination, lifecycle transitions, partial-start rollback, and
   the then-required idle-worker cleanup without callback execution.
7. **Verification step 4.7:** implemented submission gates, blocking and nonblocking
   submission, the dequeue/callback worker loop, callback accounting, and the
   temporary graceful running-destroy bridge.
8. **Verification step 4.8:** implemented public graceful shutdown, active-submitter
   completion, public worker join, `STOPPED`, and strict destruction behavior.
9. **Verification step 4.9:** completed deterministic worker-pool concurrency,
   lifecycle, failure, stress, ownership, and release-readiness audits.
10. **Verification step 4.10:** perform final Worker Pool and Scheduler Lifecycle repository validation, commit,
    and push.

This sequence keeps backend validation, queue terminal behavior, scheduler
allocation, thread startup, execution, and coordinated teardown independently
reviewable.

## 28. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| missed wake-up during shutdown | set state under mutex and broadcast both conditions |
| enqueue slips past shutdown | check shutdown under the same mutex before insertion |
| queued Tasks abandoned on normal shutdown | drain before worker exit |
| destroy races with blocked submitter | wake on shutdown and require external submitter join |
| partial startup leaks workers | track each successful create, then shutdown/join/destroy all |
| callback blocks forever | document cooperative callback requirement; no forced termination |
| callback failure kills capacity | record failure and continue worker loop |
| infrastructure error is hidden | store worker status, request shutdown, aggregate during join |
| Task use-after-free | caller retains ownership and must externally observe completion |
| lifecycle race | externally serialize lifecycle operations |
| submit/shutdown state data race | protect scheduler state with a short-held lifecycle mutex |
| native result sign loss | retain signed `int` through the Windows abstraction |
| stale shutdown query | omit public query; use operation results |
| failed join leaves active thread | retain active count and reject destruction |

Graceful shutdown cannot complete while a callback blocks forever. This is an
explicit cooperative-execution limitation, not a reason to force termination.

## 29. Deferred Features

Deferred work includes restart, immediate discard, returned-unprocessed Tasks,
per-Task completion, futures, public callback statistics, task-state
integration, cancellation, timed waits, blocking peek, dynamic pools, worker
replacement, fairness enforcement, priorities, retries, persistence, metrics,
logging, and non-Windows backends.

## 30. Architecture Decision Summary

Worker Pool and Scheduler Lifecycle adopts a caller-allocated scheduler with an opaque private
implementation. It owns one shutdown-aware synchronized queue, fixed worker
and context arrays, callback configuration, and lifecycle state. Tasks remain
caller-owned.

Queue shutdown is irreversible and idempotent. It linearizes under the queue
mutex, rejects every later enqueue, broadcasts both conditions, drains queued
Tasks, and returns `SHUTDOWN` for dequeue and peek only when shutdown and empty
are both observed. There is no public shutdown query.

Workers block on dequeue, execute callbacks outside the queue lock, continue
after callback failure, and request pool shutdown on infrastructure failure.
Shutdown and join are separate. Partial startup leaves no running worker.
Destroy reports misuse and never tears down active resources.

Lifecycle operations are externally serialized; concurrent submission remains
supported and may race safely with shutdown through the lifecycle and queue
mutexes. Scheduler restart is not supported. Completion observation and richer
execution results remain future design work.

## 31. Architecture outcome

**Worker Pool and Scheduler Lifecycle — Fixed Worker Pool and Scheduler Lifecycle**

Status: **Complete**

Completion date: 2026-07-29

Evidence includes a clean warning-free C17 build, six of six CTests, direct
execution of every test target and the main regression, 3,000 scheduler-suite
runs, 2,000 synchronized-queue-suite runs, 1,000 backend-suite runs, and the
deterministic exact-accounting workloads described above. This closes
lifecycle implementation; it does not claim production deployment readiness.
Performance Evaluation is reserved for benchmarking and performance analysis.
