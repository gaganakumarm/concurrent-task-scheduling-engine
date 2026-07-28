# Threading Architecture

## Purpose

This document records the C11 threading capability probe, internal Windows
backend, and active synchronization boundary.

## Current Task Queue

`TaskQueue` remains the fixed-capacity, non-thread-safe circular FIFO primitive
implemented in Phase 2. Its public structure and operations are unchanged.
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

Shutdown and timed waits remain unimplemented. Enqueue does not signal
not-full, and dequeue does not signal not-empty.

## Blocking Enqueue

Indefinite blocking enqueue is implemented. While the queue is full, it waits
on not-full inside a mutex-protected `while` predicate loop. The condition wait
atomically releases and reacquires the same wrapper mutex. After capacity is
available, insertion occurs under that mutex and successful insertion signals
not-empty.

The caller retains Task ownership and must keep both the Task and wrapper alive
while blocked. There is no timeout, cancellation, or shutdown path. Destroying
a queue with blocked or active operations is invalid. Fairness among blocked
producers is not guaranteed. Timed operations remain unimplemented.

## Blocking Dequeue

Indefinite blocking dequeue is implemented. While the queue is empty, it waits
on not-empty inside a mutex-protected `while` predicate loop. The condition
wait atomically releases and reacquires the wrapper mutex. Successful removal
signals not-full, and the removed pointer is published only after the
underlying mutation succeeds.

Failure before removal preserves caller output. A signaling or unlocking
failure after removal returns a system error while still publishing the removed
caller-owned pointer. There is no timeout, cancellation, or shutdown path, so
an empty queue may leave a consumer blocked indefinitely. Destruction with
blocked or active operations is invalid, and fairness among blocked consumers
is not guaranteed.

Shutdown state and wake-up semantics are planned but are not implemented.

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

This checkpoint does not implement blocking peek, timeouts, shutdown, worker
threads, task execution, or scheduler logic. `TaskQueue` itself remains
non-thread-safe.
