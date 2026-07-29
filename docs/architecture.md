# Architecture

## Purpose

The project is a C17 bounded concurrent task scheduler with a fixed worker pool
and explicit lifecycle. It provides deterministic queue coordination and
cleanup while keeping `Task` objects and callback data caller-owned.

## Component boundaries

- Public Task APIs validate task metadata, state transitions, and abstract
  work consumption.
- `TaskQueue` owns only its pointer buffer and implements non-thread-safe FIFO
  mechanics.
- `ConcurrentTaskQueue` serializes FIFO access with one private mutex and two
  condition variables.
- `Scheduler` owns the synchronized queue, fixed worker resources, lifecycle
  synchronization, and private accounting.
- The Windows backend owns native critical sections, condition variables, and
  `_beginthreadex` handles behind a platform-neutral internal interface.
- Callbacks execute outside scheduler locks and retain responsibility for
  caller-owned memory and synchronization.

## High-level flow

```text
Producer(s)
    -> Scheduler submission gate
    -> Bounded synchronized FIFO
    -> Fixed worker pool
    -> Caller callback
```

Initialization and worker startup are separate so allocation and partial
thread-creation failures can be handled deterministically:

```text
init -> start -> submit -> shutdown -> join -> destroy
```

The scheduler implementation is published to the public wrapper only after
successful initialization. Start allocates contiguous worker-handle and
worker-context arrays, creates the configured workers, and waits until every
created worker reports ready. Tasks cannot be submitted during startup.

## Ownership and lifetime

The scheduler owns its queue, worker arrays, worker contexts, lifecycle
synchronization, and private counters. The queue owns its pointer buffer and
synchronization resources but never owns a `Task`. Workers borrow their
contexts, scheduler resources, callback, and callback context.

The caller owns every `Task` and callback context and must keep them alive
until `scheduler_join` completes. The scheduler must outlive all worker threads.
Destroy releases resources but does not implicitly stop running workers;
shutdown and join are explicit lifecycle operations.

## Queue coordination and graceful shutdown

The synchronized queue is a bounded FIFO protected by one mutex. Producers wait
on `not_full`; workers wait on `not_empty`. Blocking operations always
re-evaluate their predicates after waking.

Shutdown is an irreversible queue state until destruction and reinitialization.
The first request closes submission and broadcasts both conditions while
holding the queue mutex so every blocked producer and worker can re-evaluate
the terminal predicate. No poison pill, null Task, forced termination, or
sentinel value represents shutdown.

Accepted Tasks remain in FIFO order and drain before workers exit. Producers
cannot insert after shutdown. Consumers continue removing queued Tasks and
return shutdown only when the closed queue is empty. Repeated shutdown, join,
and completed destruction operations follow their documented idempotent
contracts.

## Worker loop and callback contract

Each worker has a stable zero-based index and repeatedly performs a blocking
dequeue. A successful dequeue transitions the Task to running, invokes the
borrowed callback outside all scheduler and queue locks, and records completion
or failure. Queue shutdown with no remaining work terminates the loop.

Callback return values determine Task outcome; they do not terminate the
worker. Callbacks must not destroy the scheduler from a worker thread and are
responsible for their own memory safety, synchronization, cancellation policy,
and external side effects.

## Lifecycle and failure handling

The private lifecycle progresses through initialized, starting, running,
shutting down, stopped, or failed states. A lifecycle mutex protects state and
the active-submitter gate. Submission registers under that gate before entering
the queue, preventing shutdown from racing past a submission already admitted.

Partial worker creation closes the queue, joins every successfully created
worker, frees worker resources, and records failure. Allocation failure frees
any partial allocation and restores initialized state so the caller may destroy
or retry. Join resolves native thread ownership before accepting worker results.
Every validated cleanup path leaves zero active workers and a destroyable
wrapper.

## Synchronization and lock ordering

The design avoids holding scheduler lifecycle locks while invoking callbacks or
performing a potentially blocking queue operation. Queue operations use only
the queue mutex. Worker accounting uses its own domain. Runtime snapshots copy
the lifecycle, worker, and queue domains separately and release one lock before
acquiring the next.

Important linearization points include:

- successful enqueue while holding the queue mutex;
- shutdown publication while holding the queue mutex;
- lifecycle transition while holding the lifecycle mutex;
- worker ready, exit, and join accounting in the worker domain.

Concurrent submission is supported. Concurrent lifecycle control operations
require external serialization except where idempotence is explicitly
documented.

## Internal observability

Private snapshots combine lifecycle, worker, callback, submission, queue, and
failure accounting without changing the public API. Each domain is exact at
its copy point, but the complete snapshot spans a bounded observation window
and is not globally atomic. Live validation therefore checks structural bounds;
exact cross-domain balances are reserved for quiescent states after join.

Hot callback outcomes use cache-line-separated, single-writer C17 atomic slots.
Other fields reuse existing locks. Counters saturate at `UINT64_MAX`, set a
sticky overflow indicator, and never wrap. Snapshot capture performs no
allocation, timestamping, logging, or callback invocation.

Derived health is computed rather than stored. Structural violations or failed
lifecycle state produce failed health; shutdown produces stopping; a valid
joined state produces stopped; advisory failures or incomplete accounting
produce degraded; otherwise initialized and running states are healthy.

## Build-gated diagnostics

Contention profiling and deterministic fault injection are private
compile-time features and default to `OFF`. Disabled builds contain no
fault-plan storage or checks, and normal builds contain no profiling fields.
Neither feature changes installed headers or public scheduler semantics.

The detailed concurrency model is documented in
[threading-architecture.md](threading-architecture.md). Performance,
reliability, robustness, and release evidence are linked from the main README.
