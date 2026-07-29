# Architecture

## Purpose

The project is a C17 bounded concurrent task scheduler with a fixed worker pool
and explicit lifecycle. It provides deterministic queue coordination and
cleanup while keeping Task objects and callback data caller-owned.

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

Graceful shutdown closes submission and wakes blocked queue operations.
Accepted tasks drain before join releases worker handles and arrays. Destroy
then releases scheduler and queue resources. Private snapshots and validation
observe these domains without adding a public monitoring API.

Detailed lifecycle, threading, performance, and reliability decisions are
linked from the main README.
