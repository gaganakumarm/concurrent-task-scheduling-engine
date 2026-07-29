# Phase 6 runtime accounting and hybrid snapshot

## 1. Executive summary

Checkpoint 6.2 implements a private, versioned scheduler snapshot with exact
lifetime accounting, queue high-water tracking, checked saturation, and
deterministic tests. It adds no public API and changes no Task, callback,
queue, shutdown, or ownership semantics. Capture follows the approved hybrid
model and never nests lifecycle, worker, and queue mutexes.

## 2. Scope

The implementation records valid per-instance submission outcomes, worker
lifecycle, callback outcomes, queue dimensions, and genuinely detectable
worker/join failures. A seventh dedicated CTest validates live and quiescent
snapshots.

## 3. Non-goals

There is no public monitoring API, health threshold, invariant enforcement,
timing, wait accounting, fault injection, logging, watchdog, cancellation,
timeout, retry, recovery, dynamic worker behavior, or optimization.

## 4. Private interface

`src/internal/scheduler_observability.h` defines snapshot version 1,
`SCHEDULER_SNAPSHOT_CONSISTENCY_DOMAIN_EXACT`, private lifecycle names,
`SchedulerSnapshot`, queue runtime snapshot data, capture functions, basic and
quiescent validators, and checked-counter helpers. Nothing is installed from
this path.

## 5. Snapshot fields

| Field | Type | Initial | Update/read site | Protection/domain | Precision | Overflow |
|---|---|---:|---|---|---|---|
| version/consistency | `uint32_t`/enum | 1/domain | capture | metadata | Exact | N/A |
| overflow | `bool` | false | checked helpers/capture | all domains | Sticky | Signals saturation |
| lifecycle/open/shutdown | private enum/bools | initialized/false | existing transitions | lifecycle mutex | Domain-exact | N/A |
| configured workers | `size_t` | configured value | init/capture | immutable | Exact | Validated input |
| submitted/accepted/rejected | `uint64_t` | 0 | valid submit attempt/result | lifecycle mutex | Domain-exact | Saturate |
| created/ready/active/joined | `size_t` | 0 | create, entry, exit, join | worker mutex | Domain-exact | Bounded by configured |
| dequeued/started/succeeded/failed | `uint64_t` | 0 | worker callback path | per-worker single-writer atomics | Individually exact | Saturate |
| currently running | `uint64_t` | 0 | immediately around callback | per-worker single-writer atomic | Individually exact | Sticky overflow |
| queue capacity/size/high-water | `size_t` | capacity/0/0 | queue mutation/capture | queue mutex | Domain-exact | Capacity bound |
| startup/runtime/join failures | `uint64_t` | 0 | detected failure sites | worker mutex | Domain-exact | Saturate |

Excluded fields are cancellation (no scheduler cancellation), timing (no clock
contract), queue waits (profiling-only), callback status codes (only aggregate
success/failure is stable), speculative synchronization failures, Task
pointers, context pointers, and native handles.

## 6. Field semantics

A submitted attempt is a call with a valid scheduler instance and non-null
Task. Invalid pointers cannot safely be attributed to an instance and are not
counted. Accepted means queue insertion returned scheduler success. Every other
valid attempt is rejected exactly once. Active means a created worker entered
the routine and has not exited. Joined increments only after native join and an
`OK` worker result. Ready is the existing current readiness-barrier count and
returns to zero when worker resources are released.

Dequeued is recorded after successful removal. Started and running increment
immediately before callback invocation. Exactly one success/failure increments
after the existing integer callback result; running then decrements. Public
Task state is untouched.

## 7. Accounting update points

- Registration increments submitted under the lifecycle mutex.
- A closed/invalid lifecycle state increments rejected before returning.
- Deregistration classifies a registered operation accepted or rejected.
- Thread creation is counted only after native creation succeeds.
- Worker entry updates active and ready under the worker mutex.
- Worker exit decrements active and records detectable runtime failure.
- Callback-path slots are cache-line-separated per worker and single-writer.
- Join counts only a successfully joined worker with a validated result.
- Queue high-water updates after successful insertion under the queue mutex.
- Per-worker totals are archived under the worker mutex before contexts free.

## 8. Consistency contract

Capture order is lifecycle, worker (including atomic per-worker slots), then
queue. Each mutex is released before the next is acquired. Output is assigned
only after all domains succeed. Each locked domain is exact at its copy point;
each atomic slot is individually exact. The combined snapshot spans an
observation window and is not a globally atomic instant. Live cross-domain
relations are advisory; quiescent validation is exact after join.

## 9. Lock-ordering model

Snapshot:

1. lifecycle mutex, release;
2. worker mutex, aggregate per-worker atomic slots, release;
3. queue mutex, release.

Submission retains lifecycle registration, then releases it before queue
access, then reacquires lifecycle only for classification. Worker callbacks
acquire no new mutex. Queue operations retain their original single queue
mutex. Shutdown and join gain no new nested order. The worker mutex now lives
from successful start setup through destruction so stopped snapshots remain
synchronized. Before it exists, initialized-state worker fields are known
zeros; this preserves the original `scheduler_init` failure boundary.

## 10. Overflow policy

`uint64_t` counters saturate at `UINT64_MAX`, permanently set their owning
overflow flag, never wrap, emit no output, and do not reject work. Worker slots
use lock-free 64-bit C17 atomics verified by a compile-time assertion. Each
slot has one writer; snapshots are readers, so load/store operations avoid a
contended read-modify-write. Tests exercise `MAX-1`, `MAX`, repeated increments,
and sticky overflow for both lock-protected and atomic helpers.

## 11. Failure-accounting boundaries

Counted: native worker creation failure, non-OK worker exit, failed native join,
callback failure, and accounting overflow. Not counted: hypothetical
corruption, callback stalls without timeout, external context misuse, or
backend failures that cannot be identified more specifically. Existing public
results remain authoritative.

## 12. Public API compatibility

No file under `include/` changed. `Task`, callback signatures, Scheduler
wrapper, result values, ownership rules, and installed headers are identical to
baseline `09bc87e`. The snapshot is private and test-oriented.

## 13. Performance implications

Submission classification uses the lifecycle lock already acquired for gate
accounting. High-water tracking extends the existing queue critical section by
one size comparison. Callback accounting uses cache-line-aligned per-worker,
single-writer atomic load/store slots and no new callback mutex acquisition.
Snapshots are on demand.

An adjacent Release sanity comparison used two warm-ups and nine measured
iterations per variant. No-op medians were 1,287,371 baseline versus 1,326,781
candidate Tasks/s (+3.06%; CV 13.19%/6.04%). Medium medians were 213,853 versus
202,383 Tasks/s (-5.36%; CV 5.94%/6.08%). The medium difference is 0.36 points
beyond the nominal safeguard but smaller than either run’s variability, so it
is treated as noisy and not a demonstrated meaningful regression. No
statistical significance is claimed. Phase 6.3 should retain an overhead gate.

## 14. Test strategy

`concurrent_scheduler_observability_tests` uses test-controlled mutexes and
conditions, never arbitrary sleeps. Its twenty named checks cover metadata,
initialized/running/shutting/stopped states, valid and rejected submission,
capacity/size/high-water, live running count, callback success/failure, active
and joined workers, live/basic and quiescent validation, repeated join,
destroyed/invalid objects, saturation, sticky overflow, no wrap, and a fresh
lifecycle reset.

## 15. Validation results

The normal build is warning-free. Seven CTests pass. All seven executables pass
directly. Main reports version 0.1.0 and initialized status. Normal and
profiling benchmark self-tests pass. Profiling and transition signaling remain
independently gated and default off.

## 16. Known limitations

Live snapshots can contain cross-domain skew. Aggregate callback counters do
not identify Tasks or preserve callback status codes. There are no timings,
wait totals, cancellation outcomes, public health model, reset operation, or
fault injection. Counter saturation is detectable but makes later accounting
incomplete.

## 17. Checkpoint decision

The minimal private runtime-accounting and hybrid snapshot foundation is
approved for continued internal validation. Public exposure and invariant
enforcement remain deferred.
