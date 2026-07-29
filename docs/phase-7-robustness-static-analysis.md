# Phase 7.2 robustness, static-analysis, and undefined-behavior audit

## 1. Executive summary

Phase 7.2 audited the scheduler at commit `a896aa6`. GCC 16.1's static analyzer
completed a whole-project build without a finding. Normal, fault-enabled,
profiling, and combined-feature configurations all passed. A strict warning
build produced only reviewed diagnostics from deliberately grouped enum
fallbacks and one bounded test expression; there are no unexplained project
warnings.

AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer could not be
linked because this MSYS2 UCRT64 installation lacks their runtime libraries.
This report therefore makes no sanitizer or universal race-freedom claim. A
manual synchronization, ownership, arithmetic, lifecycle, and error-path audit
found no verified defect requiring a production change.

## 2. Scope

The review covered public and internal headers, task and queue implementations,
the Windows synchronization backend, scheduler workers and lifecycle,
accounting and validation, fault injection, profiling, benchmarks, tests,
CMake warning configuration, Phase 6 reliability documents, the Phase 7.1
report, and ADR-006.

## 3. Non-goals

This checkpoint adds no feature, scheduling redesign, recovery mechanism,
optimization, public diagnostic control, portability claim, or style-only
refactor. Tests and production semantics are unchanged.

## 4. Baseline commit

The initial tree was clean on `main`. `HEAD` and `origin/main` were
`a896aa61ac0c5f69f3b3c927a4676c71dd0c8c22`
(`validate scheduler scalability and stability`). Phase 7.1 was committed and
pushed. `v0.5-performance-engineering` was present.

## 5. Environment

The audit used Windows 11 build 26200, GCC/MSYS2 UCRT64 16.1.0, CMake 4.4.0,
MinGW Makefiles, and C17. No Clang compiler, clang-tidy, Clang Static Analyzer,
scan-build, or cppcheck executable was locally available.

## 6. Risk inventory

| Class | Principal risk | Existing control | Result |
|---|---|---|---|
| Ownership | Leaks or double frees | Explicit owner, staged cleanup, nulled wrappers | Reviewed |
| Lifetime | Worker access after destroy | Shutdown/join precondition before destroy | Reviewed |
| Synchronization | Queue/lifecycle/worker races | Separate mutex domains and atomic worker slots | Reviewed |
| Atomicity | Live counter tearing/lost updates | Lock-free 64-bit single-writer atomics | Reviewed |
| Arithmetic | Allocation/counter/time overflow | Prechecks, saturation, overflow flags | Reviewed |
| Conversions | `size_t`, fixed-width, Win32 handles | Strict conversion warnings and checked casts | Reviewed |
| API misuse | Nulls and invalid lifecycle order | Deterministic result validation | Reviewed |
| Failure cleanup | Partial start/allocation/join failure | Reverse-order cleanup and fault tests | Reviewed |
| Thread creation | Partial native resource ownership | Created-count tracking and rollback join | Reviewed |
| Thread joining | Handle leak or join under lock | Join outside worker/lifecycle mutexes | Reviewed |
| Queue operations | Missed wake-up or corrupt indices | Predicate loops under one queue mutex | Reviewed |
| Callback execution | Lock re-entry, invalid caller data | No internal lock held; caller owns data | Reviewed |
| Observability | Unsynchronized or inconsistent reads | Domain locks plus per-worker atomics | Reviewed |
| Test-only code | Fault/profiling conflict | Compile-time gates and combined build | Reviewed |
| Platform | Win32 semantic assumptions | Isolated backend and documented limitation | Reviewed |

## 7. Compiler warning audit

The audit build added:

```text
-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
-Wcast-align -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes
-Wmissing-declarations -Wformat=2 -Wformat-security -Wnull-dereference
-Wdouble-promotion -Wswitch-enum -Wswitch-default
-Wimplicit-fallthrough -Wundef -Wpointer-arith -Wwrite-strings -Wvla
-Walloca -Wstrict-overflow=5 -Wduplicated-cond -Wduplicated-branches
-Wlogical-op
```

All flags were accepted by GCC 16.1. They were audit-only and were not added to
ordinary project configuration.

`-Wswitch-enum` reported deliberately grouped fallback cases in
`concurrent_task_queue.c`. Each switch handles the result possible for that
operation explicitly and maps impossible/corrupt lower-level results through a
safe `default` system-error path. This is a justified deviation, not a
suppression or missing error path. Changing semantics merely to silence the
diagnostic was rejected.

`-Wstrict-overflow=5` reported the bounded test comparison
`gate.completed < index + 1`. Both operands are `int`, and the enclosing fixed
test loop cannot approach `INT_MAX`; this is a tool sensitivity at the most
aggressive strict-overflow level. No production expression was implicated.
Every other requested warning class was clean. Ordinary builds have zero
warnings.

## 8. Static-analysis tools

GCC 16.1 `-fanalyzer`, together with `-Wall -Wextra -Wpedantic`, compiled all
production sources, benchmarks, and normal tests. It reported no leaks,
double-free paths, use-after-free paths, null dereferences, uninitialized uses,
or other analyzer findings. No warning was suppressed.

Discovery of `clang`, `clang-tidy`, `scan-build`, and `cppcheck` was attempted
with `where.exe`; none was installed. No system-wide dependency was installed.
The strict GCC diagnostic pass served as an additional independent compiler
audit, but is not represented as a second path-sensitive analyzer.

## 9. AddressSanitizer

The Debug configure used `-fsanitize=address -fno-omit-frame-pointer -g` for
compilation and `-fsanitize=address` for linking. CMake's compiler probe failed:

```text
ld.exe: cannot find -lasan: No such file or directory
```

No ASan executable was produced, so no ASan tests ran and no ASan coverage is
claimed. Clang was not locally available. Normal, fault-injection, analyzer,
and lifecycle tests are supporting evidence but are not an ASan equivalent.

## 10. UndefinedBehaviorSanitizer

The Debug configure used `-fsanitize=undefined
-fno-sanitize-recover=all -fno-omit-frame-pointer -g`. The compiler probe
failed with:

```text
ld.exe: cannot find -lubsan: No such file or directory
```

No UBSan checks ran and no UBSan coverage is claimed. The strict conversion,
shift, format, overflow, null-dereference, and pointer warnings plus
`-fanalyzer` are the available static evidence.

## 11. ThreadSanitizer or manual race audit

The TSan configure failed during the compiler probe with
`ld.exe: cannot find -ltsan`. No TSan runtime result exists.

The manual audit found the following shared-state controls:

| Shared mutable state | Writers/readers | Synchronization |
|---|---|---|
| Queue storage, indices, shutdown, waiters, high-water, profiling | Producers, workers, snapshots | Queue mutex |
| Scheduler state, submitter count, submission counters, fault configuration | Lifecycle calls and submitters | Lifecycle mutex |
| Worker readiness/activity/join counts, aggregate worker failures/counters | Workers and lifecycle controller | Worker mutex |
| Per-worker callback/dequeue/running counters | One worker writes; snapshots/controller read | C11 atomics |
| Per-worker overflow flag | One worker writes; snapshots/controller read | C11 atomic |
| Execute function/context and configuration | Initialized before thread creation; read by workers | Immutable until after join |
| Fault plan | Test controller and fault sites | C11 atomics; configuration restricted to initialized state |
| Benchmark shared accounting | Concurrent callbacks/controller | Benchmark mutex or partitioned atomics |
| Caller Task and callback context | User callbacks and caller | Caller responsibility |

Initialization publishes fully populated worker contexts before each native
thread is created. Immutable scheduler configuration remains alive through
join. Worker arrays and handles are released only after successful native
joins. Snapshot lifecycle, worker, and queue domains are each synchronized;
the documented hybrid snapshot is domain-exact rather than a single global
instant. Validation operates on the caller-owned snapshot copy and needs no
live locks.

The audit found no unprotected shared access in these reviewed paths. This is
not a universal race-freedom claim.

## 12. Lock-order analysis

| Object | Predicate/protected state | Wait/signal behavior |
|---|---|---|
| Queue mutex | Queue contents, shutdown, waiters, profiling | Enqueue waits while full; dequeue waits while empty; shutdown broadcasts both |
| Lifecycle mutex | State, active submitters, submission counters | Shutdown/join wait while active submitters are nonzero; deregistration broadcasts |
| Worker mutex | Readiness, activity, joins, aggregate failures | Start waits while ready workers are below configured count; workers broadcast readiness |
| Benchmark mutex | Non-partitioned callback accounting/blocking gate | Controlled callback waits on explicit predicate |

All condition waits are `while` predicate loops and update predicates under the
matching mutex. Signals and broadcasts occur after predicate changes while the
mutex is held. The scheduler releases the lifecycle mutex before entering the
queue, releases queue locks before callbacks, and releases lifecycle locks
before joining. Snapshot acquisition is sequential—lifecycle, then worker,
then queue—with no nested mutexes. No reverse nested order exists.

Windows condition waits atomically release and reacquire their critical
section. Destroy is permitted only with no threads/contexts and no active
submitter. The audit found no unresolved join-under-lock, callback-under-lock,
missed-wakeup, or circular-wait hazard.

## 13. Ownership and lifetime analysis

| Allocation | Owner | Cleanup |
|---|---|---|
| `TaskQueue.items` | Task queue | `task_queue_destroy` |
| Concurrent queue implementation | Concurrent queue wrapper | `concurrent_task_queue_destroy` |
| Queue mutex/condition private storage | Concurrent queue implementation | Reverse initialization order |
| Scheduler implementation | Public `Scheduler` wrapper | `scheduler_destroy` |
| Worker thread array/context array | Scheduler implementation | `release_workers` after joins |
| Native thread handle/start record | `SchedThread` | `sched_thread_destroy` after join |
| Lifecycle/worker sync storage | Scheduler implementation | Destroy after valid terminal state |
| Benchmark arrays and sync state | Benchmark iteration | Unified cleanup path |

Tasks and callback contexts are never owned by the scheduler. All allocation
failure paths release previously acquired resources. `calloc` provides a
cleanup-safe initial state. Thread creation increments ownership counts only
after success; partial startup shuts the queue down, joins created threads, and
releases arrays. Native start records live until join and handle destruction.
No thread is detached.

Queue destroy and already-destroyed scheduler destroy are idempotent as
documented. Scheduler destroy rejects live states and nonempty worker
ownership. Reinitialization is supported only after the wrapper has been reset
by destroy.

## 14. Integer and counter analysis

Queue and worker allocation multiplications are rejected when counts exceed
`SIZE_MAX / sizeof(element)`. Circular queue indices wrap by comparison with
`capacity - 1`, avoiding overflowing increment at maximum index. Queue
invariants reject zero capacity, out-of-range indices, and size above capacity.

Runtime counters saturate at `UINT64_MAX` and set explicit overflow flags.
Per-worker atomics have one writer, so their load/store saturation algorithm
does not lose concurrent writer updates. Aggregation uses subtraction-before-
addition overflow checks. Validation avoids underflow with ordered bounds and
`add_exceeds`; overflow makes validation incomplete instead of silently
asserting correctness.

Fault occurrence counters use fixed-width atomics and flag saturation.
Benchmark allocation sizes, task-address calculations, additions, duration
subtraction, timer-frequency conversion, percentile storage, and nanosecond
conversion are checked. Deterministic checksum multiplication is unsigned
`uint64_t` modular arithmetic and therefore defined. Divisions validate
denominators. Strict conversion and format diagnostics found no unexplained
narrowing or signed/unsigned defect.

## 15. Enum and lifecycle analysis

Task priority/state validators reject unknown values. Task transitions enumerate
allowed edges and terminal states. Scheduler states cover initialized,
starting, running, shutting down, stopped, and failed. Invalid or repeated
operations return deterministic statuses; shutdown and successful join are
idempotent in their documented states. Result/name functions safely return
`UNKNOWN` for invalid enum values.

Queue-result translations use explicit expected cases and a conservative
system-error default. Snapshot conversion maps an unknown internal state to
FAILED. Validation rejects unsupported versions/consistency and records invalid
lifecycle combinations.

## 16. Nullability and API misuse

Public APIs validate required object, callback, task, and output pointers.
Initialization rejects zero queue capacity, zero workers, invalid priority,
zero work, and unsafe allocation counts. Queue operations reject malformed
storage. Scheduler operations reject calls before initialization and invalid
lifecycle ordering.

Documented supported cases include null-safe queue destruction, already-
destroyed scheduler destruction, repeated shutdown after start, and repeated
join after successful completion. Concurrent scheduler lifecycle calls require
external serialization. Task/callback mutation synchronization is explicitly
caller-owned. Internal helpers rely only on preconditions established by their
validated callers.

## 17. Callback safety boundary

The worker dequeues and releases the queue mutex before invoking the callback.
No lifecycle or worker mutex is held during callback execution. A zero return
increments success; nonzero increments failure; both preserve dequeue/start/
outcome accounting.

The scheduler passes the exact non-owning `Task *` and shared context provided
by the caller. They must remain valid until callbacks finish, and the caller
must synchronize their mutable data. Calling join from a callback, retaining
invalid pointers, data races inside callback-owned state, process crashes, and
arbitrary memory corruption are outside scheduler guarantees.

## 18. Error-path analysis

Queue and scheduler initialization unwind in reverse acquisition order.
Worker-array overflow and allocation failures leave the initialized scheduler
destructible. Worker mutex/condition failures free arrays. Partial thread
creation shuts down the queue, joins all successfully created workers,
destroys handles, aggregates truthful counters, and enters a documented failed
state.

Shutdown first closes the lifecycle submission gate, then broadcasts queue
conditions, then waits for registered submitters to deregister. Join never
holds a worker-required lock while blocking. Failed native joins preserve the
thread arrays, preventing unsafe destruction and allowing the failure to remain
observable. Fault-injection tests cover allocation, partial start, join,
repeated cleanup, misuse, and terminal observability paths.

Validation diagnostics use fixed storage with deterministic truncation.
Cleanup failures are surfaced as system errors rather than overwritten with
success.

## 19. Assertion analysis

Production code contains no runtime `assert` calls. Its sole assertion is a
compile-time `_Static_assert` requiring lock-free 64-bit atomics. Tests also use
compile-time layout/API assertions. Public validation, benchmark correctness,
and error handling remain active with `NDEBUG`; no assertion has a side effect.

## 20. Platform assumptions

The tested backend is Windows/MinGW only. It uses `_beginthreadex`,
`CRITICAL_SECTION`, `CONDITION_VARIABLE`, `SleepConditionVariableCS`,
`WaitForSingleObject`, and `QueryPerformanceCounter`. Native handles are closed
after join. Thread entry and handle conversions use Windows-compatible
`uintptr_t` and calling conventions.

The code requires C17, fixed-width integers, C11 atomics, and lock-free 64-bit
atomics. `_Alignas(64)` is a false-sharing mitigation, not a cache-line
guarantee. Fixed-width values use `<inttypes.h>` format macros where formatted.
Clock conversion uses the queried frequency with overflow checks. No Linux or
macOS support is claimed because neither was built or tested.

## 21. Defects found

No verified production defect was found. Strict warnings were individually
classified above. GCC `-fanalyzer` reported no finding. Sanitizer unavailability
is a toolchain limitation, not a clean sanitizer result.

## 22. Fixes applied

No production, public-header, CMake, or test change was applied. Consequently
no regression test or performance remeasurement was required.

## 23. Remaining limitations

Dynamic ASan, UBSan, and TSan coverage remains outstanding on a compatible
toolchain. Manual review cannot prove absence of all races or undefined
behavior. Windows is the only validated platform. External callback behavior,
OS-level process faults, resource exhaustion beyond deterministic injection,
and lifecycle misuse explicitly outside the public contract remain caller or
environment responsibilities.

## 24. Reproduction instructions

Run the normal, fault, profiling, and combined CMake commands from this
repository's root as listed in the checkpoint. GCC analysis can be reproduced
without changing project files:

```powershell
cmake -S . -B build-phase-7-2-analyzer -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic -fanalyzer"
cmake --build build-phase-7-2-analyzer
```

The strict warning flags are listed in section 7. Sanitizer attempts use the
flags in sections 9–11; a compatible installation must provide the matching
runtime libraries. All output belongs in ignored build directories.

## 25. Acceptance result

All applicable acceptance criteria passed. Normal 7/7, fault-enabled 8/8,
profiling 7/7, and combined 8/8 tests passed with zero ordinary-build warnings.
The GCC analyzer was clean; unavailable tools are reported accurately; the
manual race, locking, ownership, arithmetic, lifecycle, error, assertion, and
platform audits are complete. No public or hot-path change occurred.

## 26. Phase 7.3 readiness

Phase 7.2 is ready for architectural review. Phase 7.3 has not begun. Future
dynamic-sanitizer work should use a compatible pre-existing environment or a
separately approved toolchain checkpoint.
