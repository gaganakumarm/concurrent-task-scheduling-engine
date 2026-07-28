# Concurrent Task Scheduling Engine in C

This project will explore concurrent task execution, thread-safe task queues,
configurable scheduling policies, worker threads, synchronization,
backpressure, and performance measurement.

Current status: Phase 0 — Project Foundation

These systems are planned and are not yet implemented.

The C17 core library foundation is available with a minimal project version
API and runnable initialization executable. Scheduling and concurrency
functionality are not yet implemented.

A minimal CTest foundation verifies the public project version API.

## Planned Architecture

```text
Task Producer
→ Thread-Safe Queue
→ Scheduler
→ Worker Pool
→ Task Execution
→ Metrics
```
