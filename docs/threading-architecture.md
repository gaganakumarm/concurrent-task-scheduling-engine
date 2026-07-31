# Threading Architecture

## Purpose

This document records the C11 threading capability probe, internal Windows
backend, and active synchronization boundary.

## Current Task Queue

`TaskQueue` remains the fixed-capacity, non-thread-safe circular FIFO primitive
implemented in the bounded queue foundation. Its public structure and
operations are unchanged.
Keeping it synchronization-free preserves its focused API, existing behavior,
and direct use where external synchronization is appropriate.

## Architecture Summary

```text
Task producers
      |
      v
ConcurrentTaskQueue
  - mutex
  - not_empty
  - not_full
      |
      v
TaskQueue
  - fixed-capacity FIFO
  - caller-owned Task pointers
```

A blocking producer waits on not-full while the queue is full. A blocking
consumer waits on not-empty while the queue is empty.

## Internal Synchronization Backend

The active MinGW UCRT GCC 16.1.0 toolchain does not provide the standard C
`<threads.h>` header. CMake thread discovery succeeded, but that discovery did
not imply availability of the C standard threading API.

The project therefore uses an internal synchronization abstraction. Its first
backend uses native Windows critical sections, condition variables, and
`_beginthreadex`. Windows types and headers remain confined to the backend
implementation and are not exposed through public project headers. A POSIX
backend is deferred.

The internal condition abstraction provides distinct signal and broadcast
operations. The Windows backend implements them with
`WakeConditionVariable` and `WakeAllConditionVariable`, respectively.
Deterministic backend tests verify that one broadcast releases every waiter in
a fixed set using a shared mutex-protected predicate. Queue shutdown uses that
operation to broadcast both queue conditions.

## Synchronized Wrapper Foundation

A separate public wrapper now owns one `TaskQueue` and a private implementation
containing one mutex plus not-empty and not-full condition variables. Native
synchronization types remain hidden. Initialization, destruction, stable result
names, and mutex-protected empty, full, size, and capacity queries are
implemented.

Non-blocking synchronized enqueue is implemented. It holds the wrapper mutex
through the underlying FIFO mutation, signals not-empty only after successful
insertion, and returns immediately when the queue is full rather than waiting
for capacity. A signaling failure after insertion returns a system error but
does not roll back the stored pointer. Task pointer ownership remains with the
caller.

Non-blocking synchronized dequeue is implemented. It holds the wrapper mutex
through the underlying FIFO removal, inherits FIFO ordering from `TaskQueue`,
signals not-full after a successful removal, and returns immediately when
empty. Output remains unchanged on failure before removal. A signaling or
unlocking failure after removal returns a system error while still publishing
the removed caller-owned pointer; removal is not rolled back.

Non-blocking synchronized peek is implemented. It locks the wrapper mutex while
reading the FIFO front, changes no queue state, signals neither condition, and
returns immediately when empty. Output remains unchanged before a successful
observation. If unlocking fails afterward, the observed caller-owned pointer is
still published while a system error is returned.

The wrapper now has a private irreversible shutdown flag protected by the same
mutex. Shutdown broadcasts both not-empty and not-full. New enqueue operations
are rejected, while already queued Tasks remain available for FIFO draining.
Enqueue does not signal not-full, and dequeue does not signal not-empty.

## Blocking Enqueue

Indefinite blocking enqueue is implemented. While the queue is full, it waits
on not-full inside a mutex-protected `while` predicate loop. The condition wait
atomically releases and reacquires the same wrapper mutex. After capacity is
available, insertion occurs under that mutex and successful insertion signals
not-empty.

The caller retains Task ownership and must keep both the Task and wrapper alive
while blocked. Shutdown releases blocked producers with `SHUTDOWN` and no
insertion. There is no timeout or cancellation path. Destroying a queue with
blocked or active operations is invalid. Fairness among blocked producers is
not guaranteed. Timed operations remain unimplemented.

## Blocking Dequeue

Indefinite blocking dequeue is implemented. While the queue is empty, it waits
on not-empty inside a mutex-protected `while` predicate loop. The condition
wait atomically releases and reacquires the wrapper mutex. Successful removal
signals not-full, and the removed pointer is published only after the
underlying mutation succeeds.

Failure before removal preserves caller output. A signaling or unlocking
failure after removal returns a system error while still publishing the removed
caller-owned pointer. Shutdown releases blocked consumers. Consumers drain
queued Tasks, then return `SHUTDOWN` after observing shutdown and empty storage.
Peek follows the same shutdown-and-empty result rule. There is no timeout or
cancellation path. Destruction with blocked or active operations is invalid,
and fairness among blocked consumers is not guaranteed.

Empty, full, size, and capacity continue to describe queue storage rather than
shutdown state. Destruction does not request shutdown or wait; callers must
ensure every active or blocked operation has returned first.

## Queue Shutdown Linearization

Every queue operation uses the same private mutex, giving shutdown and queue
storage mutations a total protected order.

| Operation | Protected observation | Linearization event | Post-event failure |
|---|---|---|---|
| first shutdown | active state | set shutdown true | broadcast or unlock may return system error; state remains true |
| repeated shutdown | shutdown already true | observe true | broadcasts are retried; state remains true |
| successful enqueue | active state and capacity | FIFO insertion | signal or unlock failure does not roll back insertion |
| rejected enqueue | shutdown true | observe shutdown | unlock failure changes result to system error; no insertion |
| successful dequeue | non-empty storage | FIFO removal | signal or unlock failure does not roll back removal |
| empty dequeue | active and empty | observe both | unlock failure may change result to system error |
| shutdown dequeue | shutdown and empty | observe both | unlock failure may change result to system error |
| successful peek | non-empty storage | observe front pointer | unlock failure still publishes the pointer |
| empty peek | active and empty | observe both | unlock failure may change result to system error |
| shutdown peek | shutdown and empty | observe both | unlock failure may change result to system error |
| query | valid protected storage | read requested value | unlock failure returns the already-read logical value |

Insertion before the shutdown transition is accepted and remains drainable.
An enqueue that observes shutdown is rejected. Removal and peek of an existing
item take priority over shutdown; a shutdown result is possible only when the
same critical section observes both shutdown and empty storage.

## Queue Shutdown Race Audit

| Race | Legal protected ordering and outcome | Impossible outcome |
|---|---|---|
| enqueue versus shutdown on empty | insertion wins and is drainable, or shutdown wins and enqueue is rejected | insertion after observing shutdown |
| blocking enqueue versus shutdown on full | shutdown broadcasts not-full and enqueue returns shutdown | blocked producer inserts after wake |
| multiple blocked producers | all recheck shutdown and return shutdown in unspecified order | a producer remains blocked after a successful broadcast protocol |
| dequeue versus shutdown with one item | dequeue removes the item before or after the shutdown transition | shutdown result while the item remains available under the mutex |
| multiple consumers on empty | all wake and return shutdown | a consumer manufactures a Task or changes its output |
| more consumers than queued Tasks | queued Tasks are removed once; remaining consumers return shutdown | duplicate removal or lost accepted pointer |
| producer signal versus shutdown broadcast | mutex serialization makes either wake notification order harmless | notification order overriding the protected predicate |
| consumer signal versus shutdown broadcast | waiters recheck storage and shutdown under the mutex | signal or broadcast granting ownership of an item |
| repeated serialized shutdown | every call observes or sets shutdown and retries both broadcasts | restart or rollback |
| query versus shutdown | query reads instantaneous storage before or after the transition | shutdown encoded as size, empty, full, or capacity |

No ordering implies fairness. Task pointers remain caller-owned in every
outcome. Outputs change only after successful removal or observation.

## Audited Operation Traces

Enqueue wins:

1. producer locks the queue;
2. producer observes active state;
3. producer inserts the Task;
4. producer signals not-empty and unlocks;
5. shutdown locks the queue and sets shutdown;
6. shutdown broadcasts both conditions and unlocks;
7. a consumer drains the accepted Task; and
8. the next empty dequeue returns shutdown.

Shutdown wins:

1. shutdown locks the queue and sets shutdown;
2. shutdown broadcasts both conditions and unlocks;
3. producer locks the queue and observes shutdown;
4. producer unlocks and returns shutdown; and
5. its Task pointer was never inserted.

Multi-consumer drain:

1. three caller-owned Tasks are accepted into a capacity-three queue;
2. four consumers are ready behind test-owned coordination;
3. shutdown sets its state and wakes queue waiters;
4. consumers serialize through the queue mutex;
5. exactly three consumers remove the three FIFO pointers;
6. the remaining consumer observes shutdown and empty storage; and
7. all consumers join before queue destruction.

## Destruction Lifecycle

Shutdown is not destruction and does not wait for awakened callers to return.
Direct queue users must stop new calls, request shutdown, join producer and
consumer threads, and only then destroy the queue. Destroy is not safe
concurrently with shutdown, enqueue, dequeue, peek, or a query. There is no
public active-operation counter.

Destroying an inactive shutdown queue with undrained pointers releases only
queue-owned pointer storage and synchronization resources. It never frees the
caller-owned Tasks. Direct users remain responsible for accounting for those
Tasks. The implemented `Scheduler` avoids this condition through its required
shutdown-and-join sequence, which drains accepted Tasks before scheduler-owned
queue destruction.

| Resource | Owner | Lifetime | Release restriction |
|---|---|---|---|
| public queue wrapper | caller | caller allocation through destroy | must outlive all operations |
| private implementation | queue | successful init through destroy | no active operation |
| FIFO pointer storage | queue | TaskQueue init through destroy | queued Tasks remain caller-owned |
| queue mutex | queue | private init through destroy | no thread may hold or wait on it |
| not-empty condition | queue | private init through destroy | no consumer may remain waiting |
| not-full condition | queue | private init through destroy | no producer may remain waiting |
| shutdown flag | queue | private init through destroy | accessed only under queue mutex |
| test mutexes and conditions | test | test setup through joined threads | destroyed after every join |
| thread contexts | test | before create through join | stack storage must remain alive |
| thread handles | test | successful create through destroy | joined before handle destruction |
| Task objects | caller | caller-defined | never freed, copied, or mutated by queue |

## Scheduler Queue-Driven Execution

The public `Scheduler` is a caller-allocated opaque wrapper containing only a
private implementation pointer. Callers must zero-initialize it before first
use. Successful initialization privately owns one empty
`ConcurrentTaskQueue`, stores a nonzero fixed worker count, and borrows an
execution callback plus optional callback context.

Initialization creates a lifecycle mutex and condition. Start allocates stable
thread and worker-context arrays and retains the readiness handshake: each
worker increments `ready_worker_count` under the worker mutex before entering
its blocking queue-dequeue loop. Start publishes `RUNNING` only after all
workers report ready.

Each successful dequeue returns the exact caller-owned Task pointer. The worker
invokes the configured callback only after dequeue releases the queue mutex,
and without holding the lifecycle or worker mutex. It then briefly locks the
worker mutex to record private success or failure accounting. A nonzero
callback result does not stop the worker and is never reported as submission
failure.

Submitters lock the lifecycle mutex, require `RUNNING`, increment
`active_submitter_count`, then unlock before any blocking or nonblocking queue
operation. They relock only to deregister and notify a waiting destroy. The
lock-order rule is that scheduler code never holds the lifecycle mutex while
entering the queue; queue code never enters scheduler synchronization.

Shutdown closes the gate by changing `RUNNING` to `SHUTTING_DOWN` under the
lifecycle mutex, then releases that mutex before requesting queue shutdown.
These are separate linearization events. Queue shutdown wakes blocked
submitters, rejects later queue insertion, and preserves accepted Tasks for
draining. Shutdown waits on the lifecycle condition until every registered
submitter deregisters, but it does not wait for callbacks or workers.

Join is valid only after shutdown. It holds no lifecycle, queue, worker, or
callback mutex while waiting on threads. Workers drain accepted Tasks, receive
queue `SHUTDOWN` only after it is empty, and exit. Join checks every signed
result, destroys joined handles, releases worker arrays and contexts, destroys
worker synchronization, and publishes `STOPPED`. No callback can execute after
successful join returns.

A worker queue or synchronization failure requests queue shutdown and returns
a deterministic infrastructure-failure result. Callback failure does not.
Task and callback-context storage must remain valid through callback completion
and worker join. Lifecycle calls remain externally serialized. Destroy accepts
only `INITIALIZED`, `STOPPED`, or safely cleaned `FAILED`; it never shuts down
or joins implicitly.

| Operation | Locks held while blocking | Ordering rule |
|---|---|---|
| submit registration | lifecycle mutex briefly | release before queue enqueue |
| blocking enqueue/dequeue | queue-private mutex only | queue never enters scheduler synchronization |
| callback | caller callback locks only | no scheduler or queue lock is held |
| callback accounting | worker mutex briefly | acquire only after callback returns |
| shutdown | lifecycle gate, then queue shutdown separately | never hold both mutexes |
| join | no scheduler mutex while joining | workers retain queue and accounting access |
| destroy | lifecycle mutex for validation only | only after worker and submitter activity ends |

Scheduler race outcomes follow the protected predicates rather than wake
timing:

| Participants | Ordering |
|---|---|
| submitters versus submitters | lifecycle registration and queue insertion each serialize independently |
| submitter versus shutdown | gate closure prevents later registration; registered calls finish through queue shutdown |
| worker dequeue versus shutdown | stored Tasks drain before shutdown is returned |
| callback versus shutdown | callback may remain active because shutdown does not join |
| callback versus join | join waits for the worker containing that callback |
| repeated shutdown/join | lifecycle state makes both operations idempotent |
| destroy versus active lifecycle | RUNNING and SHUTTING_DOWN reject destroy |
| worker failure versus controller shutdown | irreversible queue shutdown composes idempotently |
| callback failure versus later dequeue | accounting records failure without changing worker-loop control |
| destroy followed by init | wrapper reset ends ownership before a new lifetime begins |

| Scheduler resource | Owner | Provision and lifetime | Cleanup |
|---|---|---|---|
| public wrapper | caller | zero-initialized caller storage | remains caller storage |
| private implementation | scheduler | allocated during init, published last | freed by destroy |
| concurrent queue | scheduler implementation | initialized during init, shutdown explicitly | destroyed after join by destroy |
| lifecycle mutex and condition | scheduler implementation | initialized during init | retained through join, destroyed by destroy |
| callback function | caller, borrowed by scheduler | stored for initialized lifetime | invoked outside scheduler locks, never freed |
| callback context | caller, borrowed by scheduler | optional stored pointer | passed unchanged, never freed |
| worker count | scheduler implementation | immutable configuration | discarded with implementation |
| worker arrays | scheduler implementation | allocated during start | freed after every worker joins |
| worker mutex and condition | scheduler implementation | initialized during start | protects readiness and accounting; destroyed after joins |
| Task objects | caller | borrowed from acceptance through callback | scheduler never copies, frees, or directly mutates |

## Ownership

The synchronized wrapper owns its queue buffer and synchronization objects,
but it does not copy or own `Task` objects. Task lifetime remains the caller's
responsibility. Destruction must not race with other users of the wrapper.

## Backend Capability Probe

The isolated synchronization backend test validates the internal mutex,
condition-variable, and thread abstractions without including Windows headers.
It uses predicate-loop waits and a mutex-protected handshake rather than sleeps
or busy waiting. The backend builds and passes this test with the active MinGW
UCRT GCC 16.1.0 toolchain.

## Deferred Functionality

The current design does not implement blocking peek, timeout lifecycle APIs,
cancellation, restart, or completion-result APIs. `TaskQueue` remains
non-thread-safe.
