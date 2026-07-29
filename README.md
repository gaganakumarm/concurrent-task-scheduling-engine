# Concurrent Task Scheduling Engine

This project will explore concurrent task execution, thread-safe task queues,
configurable scheduling policies, worker threads, synchronization,
backpressure, and performance measurement.

Current status: Phase 3 synchronized queue integration is complete, and Phase 4
architecture planning and condition-broadcast backend validation are complete.
Graceful synchronized-queue shutdown is implemented and has completed its
local concurrency and lifecycle audit. The scheduler API foundation now
supports private initialization, configuration, result naming, and destruction
before or after its running lifecycle. Fixed workers start deterministically,
and blocking and non-blocking scheduler submission APIs accept caller-owned
Task pointers. Workers block on the owned queue and invoke callbacks outside
scheduler and queue locks. Callback failure is recorded privately and does not
stop the pool. Running destroy is no longer the lifecycle mechanism. Public
graceful shutdown closes
submission and releases blocked submitters; public join waits for the accepted
Task drain and releases worker resources. Destroy requires an initialized
never-started or joined scheduler. Dynamic scaling, cancellation, retries, and
completion-result APIs do not exist.

Phase 4 — Fixed Worker Pool and Scheduler Lifecycle is complete as of
2026-07-29. The supported backend is Windows through the project’s
platform-neutral synchronization abstraction. Validation covers the six CTest
targets at 6/6, plus 3,000/3,000 scheduler, 2,000/2,000 synchronized-queue,
and 1,000/1,000 backend lifecycle runs.

The C17 core library foundation is available with a minimal project version
API and runnable initialization executable. Public task state and priority
definitions provide validation and readable name APIs. A basic public task
record stores an ID, priority, and state, and can be initialized without
dynamic allocation. It also stores total and remaining work as abstract units.
Its lifecycle supports explicit, validated state transitions without changing
work metadata. Running tasks can consume abstract work units, and exact work
exhaustion completes the task. This models deterministic work accounting, not
CPU execution or elapsed time. Scheduler callbacks are caller-provided and do
not automatically alter the Task lifecycle.

The public API for a configurable, fixed-capacity FIFO task queue is defined.
It specifies circular-buffer metadata and stores non-owning Task pointers.
Queue initialization now allocates its internal pointer buffer, and destruction
releases only that buffer. Enqueue rejects null tasks and full or malformed
queues, advances the tail with circular wraparound, and never copies a Task.
Read-only empty, full, size, and capacity queries are also implemented. Task
objects remain caller-owned. Dequeue returns pointers in FIFO order, clears
vacated slots, and advances the head with circular wraparound. Non-mutating
peek returns the oldest FIFO pointer without removal. All basic non-thread-safe
FIFO operations are implemented; blocking and synchronized behavior are not.

CTest verifies the public project version API, task domain, FIFO queue,
internal synchronization backend, synchronized queue wrapper, and scheduler
initialization, submission, and callback-execution foundation.

A native Windows synchronization backend is validated for Phase 3. A public
synchronized queue wrapper foundation now composes the existing non-thread-safe
`TaskQueue` with private synchronization state. Initialization, destruction,
stable result names, and mutex-protected queries are implemented. Task objects
remain caller-owned. Thread-safe non-blocking enqueue is also implemented:
full queues return immediately and successful insertion signals the private
not-empty condition. Thread-safe non-blocking dequeue preserves FIFO order,
returns immediately when empty, and signals the private not-full condition
after removal. Thread-safe non-blocking peek and synchronized queries read under
the private mutex without changing queue state. Indefinite blocking enqueue now
waits for capacity using the private not-full condition. Blocking dequeue now
waits indefinitely for work using the private not-empty condition. Together
they support bounded producer-consumer coordination. Timed waits and
cancellation remain unimplemented.

The synchronized queue now supports irreversible, idempotent graceful
shutdown. Shutdown rejects new enqueue operations, broadcasts both queue
conditions to release blocked producers and consumers, and preserves queued
Tasks for FIFO draining. Dequeue and peek return `SHUTDOWN` only when shutdown
is active and queue storage is empty. Storage queries retain their normal
meaning, and Task ownership remains with the caller.

## Phase 5 — Performance Engineering

Phase 5 added a reproducible Release benchmark, Windows high-resolution timing,
exact callback validation, compile-time-gated contention profiling, and a
controlled A/B optimization process. Minimal-work throughput scaled weakly or
negatively with additional workers around the shared bounded queue. Direct
profiling supported queue coordination as a material bottleneck for tiny Tasks,
while partitioned exact accounting did not explain the scaling result.

A waiter-aware transition-signaling experiment reduced condition signals
substantially in several configurations, but its medium-CPU median regressed
5.79%, exceeding the acceptance safeguard. The experiment was rejected:
profiling and transition-aware signaling both default to `OFF`, and no
experimental optimization is active in the production configuration.

Phase 5 reports:

- [Benchmark plan and architecture](docs/phase-5-benchmark-plan.md)
- [Baseline benchmark report](docs/phase-5-baseline-benchmark-report.md)
- [Contention profiling report](docs/phase-5-contention-profiling-report.md)
- [Queue-coordination experiment](docs/phase-5-queue-coordination-optimization-report.md)
- [Performance engineering closure](docs/phase-5-performance-engineering-closure.md)
- [Phase 5 artifact index](docs/phase-5-performance-index.md)

Portfolio summary: Built and profiled a bounded concurrent task scheduler in
C with a fixed worker pool, deterministic correctness validation,
high-resolution Windows timing, queue-contention instrumentation, and
controlled A/B acceptance gates. Profiling identified shared queue coordination
as a material tiny-Task bottleneck. A signal-reduction candidate was honestly
rejected after violating a medium-workload safeguard, preserving validated
default behavior.

Phase 6 reliability and observability architecture is in progress. Runtime
accounting and a test-only private hybrid snapshot are implemented; no public
monitoring API exists. Fault injection, recovery, and watchdog behavior remain
unimplemented.
Private lifecycle-invariant validation, derived health, and deterministic
fixed-buffer formatting are also implemented. Validation is explicit and does
not enforce or alter runtime behavior.

## Planned Architecture

```text
Task Producer
→ Thread-Safe Queue
→ Scheduler
→ Worker Pool
→ Task Execution
→ Metrics
```
