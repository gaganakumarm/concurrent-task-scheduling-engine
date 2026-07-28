# Concurrent Task Scheduling Engine in C

This project will explore concurrent task execution, thread-safe task queues,
configurable scheduling policies, worker threads, synchronization,
backpressure, and performance measurement.

Current status: Phase 2 — Fixed-Capacity FIFO Task Queue

These systems are planned and are not yet implemented.

The C17 core library foundation is available with a minimal project version
API and runnable initialization executable. Public task state and priority
definitions provide validation and readable name APIs. A basic public task
record stores an ID, priority, and state, and can be initialized without
dynamic allocation. It also stores total and remaining work as abstract units.
Its lifecycle supports explicit, validated state transitions without changing
work metadata. Running tasks can consume abstract work units, and exact work
exhaustion completes the task. This models deterministic work accounting, not
CPU execution or elapsed time. Scheduling and concurrency functionality are
not implemented.

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

CTest verifies the public project version API, task domain, and FIFO queue.

## Planned Architecture

```text
Task Producer
→ Thread-Safe Queue
→ Scheduler
→ Worker Pool
→ Task Execution
→ Metrics
```
