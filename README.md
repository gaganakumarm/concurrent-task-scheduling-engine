# Concurrent Task Scheduling Engine in C

This project will explore concurrent task execution, thread-safe task queues,
configurable scheduling policies, worker threads, synchronization,
backpressure, and performance measurement.

Current status: Phase 3 synchronized queue integration is implemented locally
and remains uncommitted. Scheduler and worker execution features are planned.

The C17 core library foundation is available with a minimal project version
API and runnable initialization executable. Public task state and priority
definitions provide validation and readable name APIs. A basic public task
record stores an ID, priority, and state, and can be initialized without
dynamic allocation. It also stores total and remaining work as abstract units.
Its lifecycle supports explicit, validated state transitions without changing
work metadata. Running tasks can consume abstract work units, and exact work
exhaustion completes the task. This models deterministic work accounting, not
CPU execution or elapsed time. Scheduling and worker execution are not
implemented.

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
internal synchronization backend, and synchronized queue wrapper.

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
they support bounded producer-consumer coordination. Timed waits, cancellation,
shutdown, worker pools, and scheduler integration remain unimplemented.

## Planned Architecture

```text
Task Producer
→ Thread-Safe Queue
→ Scheduler
→ Worker Pool
→ Task Execution
→ Metrics
```
