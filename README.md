# Concurrent Task Scheduling Engine in C

This project will explore concurrent task execution, thread-safe task queues,
configurable scheduling policies, worker threads, synchronization,
backpressure, and performance measurement.

Current status: Phase 1 — Task Model (basic task record)

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

CTest verifies the public project version API and task domain definitions.

## Planned Architecture

```text
Task Producer
→ Thread-Safe Queue
→ Scheduler
→ Worker Pool
→ Task Execution
→ Metrics
```
