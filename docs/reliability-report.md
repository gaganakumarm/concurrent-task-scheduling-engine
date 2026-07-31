# Scheduler Reliability Report

## Overview

The reliability layer combines lifetime-scoped runtime accounting,
domain-consistent snapshots, lifecycle-invariant validation, derived health,
and compile-time-gated deterministic fault injection. It changes no public
header, callback signature, Task representation, ownership rule, or ordinary
scheduler behavior.

## Runtime snapshots

The private `SchedulerSnapshot` captures:

- schema and consistency identifiers;
- lifecycle state, shutdown state, and active submitters;
- configured, created, ready, active, and joined workers;
- submitted, accepted, rejected, and dequeued Tasks;
- callback starts, successes, failures, and currently running callbacks;
- queue size, capacity, and high-water mark;
- startup, join, and worker-runtime failures; and
- sticky overflow state.

Capture copies lifecycle, worker, and queue domains separately, releasing each
mutex before acquiring the next. Each domain is exact at its copy point, while
the combined snapshot spans an observation window. Live cross-domain
relationships are therefore advisory; exact balances are checked only after
the scheduler is quiescent.

Hot callback outcomes use cache-line-separated per-worker C17 atomic slots with
one writer and snapshot readers. Other values reuse existing synchronization.
Unsigned counters saturate at `UINT64_MAX`, permanently record overflow, and
never wrap. Capture allocates no memory and emits no output.

## Lifecycle invariant validation

Validation is a private, pure operation over caller-supplied snapshots. Live
mode checks bounds that remain valid across a hybrid capture window, including:

- queue size and high-water mark do not exceed capacity;
- created, ready, active, and joined workers remain within configured bounds;
- active and joined worker states are compatible;
- callback outcomes do not exceed starts;
- dequeues and callback starts do not exceed accepted work; and
- lifecycle, submission gate, and shutdown fields are compatible.

Quiescent mode additionally requires, when counters have not overflowed:

```text
submitted = accepted + rejected
accepted  = dequeued
dequeued  = callback starts
starts    = callback successes + callback failures
```

Queue size, running callbacks, and active workers must be zero, and a stopped
scheduler must have joined every created worker. Overflow marks accounting
incomplete rather than claiming corruption.

Validation records stable issue codes with informational, advisory, violation,
or incomplete severity. Derived health is:

- `FAILED` for failed lifecycle or structural violation;
- `STOPPING` while shutdown is in progress;
- `STOPPED` for a valid joined scheduler;
- `DEGRADED` for advisory failures or incomplete accounting; and
- `HEALTHY` for valid initialized or running state.

Diagnostic formatting writes deterministic names and values into caller
storage, reports the required size, safely truncates, and emits no addresses,
timestamps, allocation, logging, or external data.

## Deterministic fault injection

`CONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION` defaults to `OFF`. Enabled builds
compile a private per-scheduler fault plan and add one dedicated test
executable. Disabled builds contain no fault-plan field, branch, atomic
operation, lock, or function call.

Plans use one-based occurrence numbers. Only a matching fault point advances
its counter, the selected occurrence fires once, and resetting or replacing a
plan clears its observation state. C17 atomics make plan observation data-race
safe, while configuration remains an externally serialized lifecycle action.
There is no process-global mutable plan.

Implemented seams are:

| Fault point | Injection location | Result |
|---|---|---|
| Allocation | Either worker-array allocation during start | Behaves as allocation failure |
| Worker creation | Immediately before a native thread-create call | Uses normal partial-start cleanup |
| Worker join | After native join, before result acceptance | Rejects the result after handle ownership is resolved |

Worker-readiness injection is deliberately deferred because readiness order is
scheduler-dependent. Queue corruption, callback corruption, arbitrary backend
failure, random faults, thread termination, and runtime activation are not
supported.

`SCHEDULER_FAULT_WORKER_STARTUP` remains a reserved internal enum value for
stable diagnostic naming, but fault-plan validation rejects it and no runtime
injection site implements it. The implemented seams are exactly the three rows
listed above.

## Cleanup and recovery guarantees

Partial worker creation closes the queue and joins every created worker before
returning. A later join is safe and moves the cleaned failed scheduler to
stopped state. Worker-array allocation failure frees a successful partial
allocation, restores initialized state, and permits deterministic retry after
the fault plan is reset. Injected join-result rejection still destroys native
handles and frees worker arrays.

Lifecycle misuse returns existing error codes without mutating valid state:
start cannot run twice, shutdown and join reject initialized state, destroy
rejects running state, and join rejects a destroyed wrapper. Shutdown and join
are idempotent after successful completion, destroy is idempotent after the
wrapper is cleared, and wrappers can be reused after a completed or safely
failed lifetime.

Every validated failure path leaves zero active workers and a destroyable
wrapper. Cleanup never restarts workers or reopens submission.

## Observability after failure

Worker-creation failure produces exact created/joined counts, zero active
workers, one startup failure, and valid quiescent accounting. Health remains
failed until explicit join cleanup completes, after which the structurally
valid stopped snapshot derives stopped health.

Allocation failure leaves an initialized, healthy snapshot with no workers.
Successful retry and cleanup yield joined equal to created. Join-result
rejection retains no live native thread but reports the deterministic
stopped-workers-not-joined issue and failed health.

## Test coverage

The normal build registers seven CTests. A fault-enabled build registers an
eighth test containing 50 deterministic checks. Coverage includes:

- first, middle, and last worker-creation failure;
- either worker-array allocation failure and retry;
- duplicate initialization and double start;
- shutdown and join before start;
- destroy while running and join after destroy;
- repeated shutdown, join, and destroy;
- wrapper reuse after successful and failed lifetimes;
- safe post-native-join result rejection;
- snapshot saturation, overflow, validation, formatting, and health; and
- partial-cleanup accounting.

Tests use small bounded worker sets and explicit mutex/condition coordination,
not arbitrary sleeps or timing oracles. Normal, profiling, fault-enabled, and
combined builds passed their applicable suites without compiler warnings.

## Production compatibility

Snapshots and validators are private and on demand. Fault injection is not
installed or reachable through the public API, command line, environment, or
runtime input. Normal scheduler operations retain their public behavior and
fault injection remains compiled out.

## Known limitations

Hybrid live snapshots cannot prove cross-domain equality. Counters summarize
outcomes rather than identifying individual Tasks or preserving callback
status codes. The health model has no timeout signal. Tests establish
deterministic control-flow cleanup but do not replace heap, handle, sanitizer,
or long-duration leak tooling. Unsupported platform failure paths remain
unverified.

## Conclusion

The scheduler has deterministic evidence for lifecycle accounting, structural
and quiescent invariants, partial-start cleanup, allocation recovery, native
join ownership, repeated cleanup, and production-disabled diagnostic features.
