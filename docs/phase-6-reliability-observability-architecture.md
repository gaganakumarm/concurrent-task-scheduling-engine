# Phase 6 reliability and observability architecture

## 1. Executive summary

Phase 6 will add internal-first, data-race-safe runtime accounting,
deterministic failure testing, and production-oriented reliability evidence
without changing scheduler semantics. Checkpoint 6.1 is architecture only:
runtime snapshots, fault injection, recovery, watchdogs, and new public APIs
are not implemented. The selected snapshot model is hybrid: each existing
synchronization domain is read consistently, locks are never nested, and the
combined snapshot is explicitly not one atomic instant.

## 2. Current scheduler architecture

`Scheduler` is a caller-allocated opaque wrapper around a private
implementation. That implementation owns a bounded synchronized FIFO queue,
fixed worker arrays, worker contexts, lifecycle and worker synchronization,
the callback pointer, and its caller-owned context pointer. Queue and scheduler
store non-owning `Task *` values. Windows synchronization is hidden behind
mutex, condition, and thread abstractions. Profiling is private, compile-time
gated, and disabled by default.

The private scheduler states are `INITIALIZED`, `STARTING`, `RUNNING`,
`SHUTTING_DOWN`, `STOPPED`, and `FAILED`. They are explicit internally but not
observable through the public API. Public `TaskState` is separate and is not
automatically changed by scheduler execution.

## 3. Current lifecycle

1. `scheduler_init` validates configuration, allocates the implementation,
   initializes the queue, lifecycle mutex, and lifecycle condition, then enters
   `INITIALIZED`. Each failed initialization step releases earlier resources.
2. `scheduler_start` enters `STARTING`, allocates worker storage, initializes
   worker synchronization, creates the fixed workers, and waits for every
   worker to report ready. Allocation failures return to `INITIALIZED`.
   Partial thread-creation or readiness failures shut down the queue, join
   created workers where possible, and enter `FAILED`.
3. `scheduler_submit` and `scheduler_try_submit` register under the lifecycle
   mutex only in `RUNNING`. Blocking submit may wait for capacity. The gate
   count prevents shutdown from returning while a registered submitter remains.
4. Workers dequeue FIFO pointers until queue shutdown and empty storage.
   Callback return zero is counted as success and nonzero as failure; either
   result leaves the worker running. Queue or worker synchronization failure
   requests queue shutdown and returns a worker infrastructure result.
5. `scheduler_shutdown` changes `RUNNING` to `SHUTTING_DOWN`, irreversibly
   closes the queue, broadcasts blocked queue users, and waits for registered
   submitters. Repeated shutdown is valid after start and after stop.
6. Accepted queued Tasks are drained, not cancelled or abandoned. New and
   blocked submissions return shutdown once the gate closes.
7. `scheduler_join` is valid from `SHUTTING_DOWN` or `FAILED`, joins every
   created worker it can, validates worker results, frees worker storage, and
   enters `STOPPED`. A join failure leaves resources for an explicit retry or
   diagnostic failure path rather than pretending all workers were joined.
8. `scheduler_destroy` accepts `INITIALIZED`, fully cleaned `FAILED`, or
   `STOPPED`; it never implicitly shuts down or joins. It releases scheduler
   synchronization and queue storage but never Tasks or callback context.

Lifecycle operations on one scheduler require external serialization.
Callbacks may run concurrently. The caller owns Task memory, callback context,
and synchronization for both. There is no scheduler cancellation API, timeout,
retry, restart, persistence, or automatic recovery.

## 4. Existing reliability protections

- Checked lifecycle preconditions and stable result codes.
- Stepwise cleanup for queue and synchronization initialization.
- Partial worker-start cleanup and a private `FAILED` state.
- Worker readiness rendezvous before `scheduler_start` succeeds.
- Submission registration closes the shutdown race.
- Predicate-loop condition waits and shutdown broadcasts.
- FIFO draining before workers exit.
- Idempotent shutdown and join after successful start.
- Explicit join requirement before ordinary destruction.
- Callback failure accounting without stopping the pool.
- Worker return-code and infrastructure-failure validation during join.
- Non-owning Task/context contracts documented publicly.
- Exact benchmark accounting and extensive queue, shutdown, and lifecycle tests.

## 5. Reliability gaps

There is no supported runtime snapshot, health state, high-water mark, public
failure total, active-worker total, invariant report, or production failure
dump. Existing callback counters can overflow and disappear at destruction.
Infrastructure failures largely collapse to `SYSTEM_ERROR`. A callback can
stall forever because execution has no timeout or cancellation. A failed
native join has limited diagnostics. The scheduler does not translate callback
outcomes into `TaskState`, so operational completion/failure must not be
inferred from Task state. There is no deterministic fault-injection framework,
long-duration harness, or resource-leak observation protocol.

## 6. Risk register

| ID | Subsystem/scenario | Consequence | Current protection and tests | Observability gap | Sev. | Likelihood | Future mitigation | Target |
|---|---|---|---|---|---|---|---|---|
| R01 | Scheduler partial initialization | Leak or unusable wrapper | Ordered cleanup; init tests | Failed stage unavailable | High | Low | Injection plus stage diagnostic | 6.4–6.5 |
| R02 | Queue allocation/init failure | Start unavailable | Result mapping and cleanup | Cause collapsed | High | Low | Deterministic injection/category | 6.4–6.5 |
| R03 | Mutex/condition initialization | Partial resource leak | Stepwise destruction; backend tests | Exact primitive unavailable | High | Low | Injection point per primitive | 6.4–6.5 |
| R04 | Partial worker creation | Live orphan or bad state | Shutdown/join created workers; start tests | Created/joined counts hidden | Critical | Low | Snapshot plus injected create failure | 6.2, 6.5 |
| R05 | Submission during shutdown | Accepted work after closure | Lifecycle gate and shutdown tests | Rejection totals hidden | High | Low | Submission counters/invariant | 6.2–6.3 |
| R06 | Repeated lifecycle misuse | Invalid destruction or leak | State checks and idempotence tests | State not inspectable | High | Medium | Lifecycle snapshot/diagnostic | 6.2–6.3 |
| R07 | Callback reports failure | Silent workload failure | Private failure count; callback tests | Not operator-visible | High | Medium | Snapshot failure counter | 6.2 |
| R08 | Callback stalls indefinitely | Join/shutdown never completes | Documented blocking semantics only | No running duration | Critical | Medium | Soak diagnostic; no watchdog yet | 6.6–6.7 |
| R09 | Callback corrupts shared context | Application corruption | Caller owns synchronization | Cannot detect generically | Critical | Medium | Contract and application tests | 6.7 |
| R10 | Queue saturation | Producer blocking/latency | Bounded blocking and try-submit | No production wait/high-water data | Medium | High | Queue accounting | 6.2 |
| R11 | Producer starvation | Unbounded submission delay | Condition signaling and stress tests | No per-producer production data | High | Low | Reliability stress diagnostics | 6.6 |
| R12 | Worker starvation | Reduced progress | Phase 5 distribution evidence | No runtime per-worker health | High | Low | Active/per-worker optional data | 6.2, 6.6 |
| R13 | Missed wake-up | Deadlock | Predicate loops and concurrency tests | No wait-state snapshot | Critical | Low | Stress/fault timeout dump | 6.6–6.7 |
| R14 | Premature worker exit | Reduced or stopped drain | Worker result checked at join | Not visible until join | Critical | Low | Active-worker accounting/health | 6.2–6.3 |
| R15 | Incomplete draining | Lost accepted Task | Shutdown drain tests | No accepted balance snapshot | Critical | Low | Accounting invariant | 6.2–6.3 |
| R16 | Lost/double Task accounting | False completion | Exact benchmark/test oracles | No production invariant | Critical | Low | Checked counters and validator | 6.2–6.3 |
| R17 | Duplicate Task submission | Same pointer executes twice | Allowed by current API; caller responsibility | No identity registry | High | Medium | Document; test oracle only | 6.7 |
| R18 | Task ownership confusion | Use-after-free | Explicit non-ownership contract/tests | Cannot validate lifetime | Critical | Medium | Diagnostics documentation | 6.7 |
| R19 | Shutdown deadlock | Service cannot stop | Gate count, broadcasts, tests | No timeout/state dump | Critical | Low | Timed harness oracle | 6.6–6.7 |
| R20 | Native join failure | Live handle/resource uncertainty | Error returned; handle retained | Which worker failed hidden | Critical | Low | Per-worker join result | 6.2, 6.5 |
| R21 | Counter overflow | Invalid diagnostics | No current checked production counters | Undetected | Medium | Low | Saturation plus overflow flag | 6.2 |
| R22 | Snapshot data race | Undefined behavior | Snapshot does not yet exist | Entire surface absent | Critical | Medium if naïve | Existing-lock hybrid design | 6.2 |
| R23 | Instrumentation changes semantics | Regression/deadlock | Profiling default off; Phase 5 gates | Production accounting not proven | High | Medium | Lock-order review and rollback gate | 6.2 |
| R24 | Snapshot during transition | Contradictory totals | External lifecycle serialization only | No consistency contract | High | Medium | Domain-consistent snapshot metadata | 6.2–6.3 |
| R25 | Synchronization operation failure | Worker/queue failure | Returned `SYSTEM_ERROR`, queue shutdown | Primitive/location collapsed | Critical | Low | Failure category and injection | 6.4–6.5 |

Severity and likelihood are engineering prioritization, not claims of observed
production incidents.

## 7. Observability requirements

Required Phase 6 data is minimal and semantic: lifecycle, gate status, worker
counts, exact submission/callback balance, queue size/capacity/high-water mark,
and categorized failures. Optional timing must wait until a clock and overhead
contract exists. Benchmark profiling remains separate.

| Field | Classification | Type | Update/read location | Synchronization | Overflow | Visibility/reset | Production overhead |
|---|---|---|---|---|---|---|---|
| lifecycle state | Required | internal enum | existing lifecycle transitions | lifecycle mutex | N/A | Internal, lifetime reset | None |
| accepting submissions | Required, derived | `bool` | snapshot from `RUNNING` | lifecycle mutex | N/A | Internal, derived | None |
| stop requested | Required, derived | `bool` | `SHUTTING_DOWN/STOPPED` | lifecycle mutex | N/A | Internal, derived | None |
| configured/created/joined/active workers | Required | `size_t` | start, worker entry/exit, join | worker mutex | Checked against configured | Internal, lifetime | Small |
| submitted/accepted/rejected | Required | `uint64_t` | submit registration/result | lifecycle mutex | Saturate and set overflow | Internal, lifetime | Small |
| dequeued/started/running | Required | `uint64_t` | worker around callback | worker mutex | Saturate and flag | Internal, lifetime | Small |
| completed/failed | Required | `uint64_t` | callback return accounting | worker mutex | Saturate and flag | Internal, lifetime | Existing lock increment |
| cancelled | Rejected for now | — | No cancellation semantics | — | — | — | — |
| queue size/capacity | Required | `size_t` | queue snapshot | queue mutex | Capacity bound | Internal, current | Snapshot only |
| queue high-water mark | Required | `size_t` | successful enqueue | queue mutex | Capacity bound | Internal, lifetime | One compare |
| enqueue/dequeue waits | Optional | `uint64_t` | condition wait entry | queue mutex | Saturate and flag | Internal, lifetime | Increment per wait |
| callback/worker/sync/invariant failures | Required | `uint64_t` plus bit flags | detection sites | owning domain lock | Saturate and flag | Internal, lifetime | Small |
| uptime/callback/shutdown timing | Optional | `uint64_t` ns | future clock boundary | To be designed | Checked/saturating | Internal, lifetime | Potentially material |
| profiling lock/wait detail | Benchmark-only | existing profile types | profiling build | queue mutex/post-join | Existing checks | Private, per lifetime | Default zero |

Accepted counters are not user-resettable in Phase 6; a new initialized
scheduler creates a new lifetime. Reset during execution would break invariants.

## 8. Runtime snapshot model

The future internal `SchedulerSnapshot` should contain a schema version,
capture sequence, lifecycle/health, gate flags, configured/created/active/joined
workers, submitted/accepted/rejected, dequeued/started/running/completed/failed,
queue capacity/size/high-water, failure totals/flags, counter-overflow status,
and consistency metadata. It should not contain Task pointers, callback context,
native handles, wall-clock timestamps, cancellation counts, or profiling-only
timings.

Phase 6.2 should first expose the snapshot only to internal tests through a
private header guarded against ordinary public inclusion. Snapshot formatting
belongs in the reliability test/application layer.

## 9. Snapshot consistency model

**Decision: hybrid domain-consistent snapshot.** The reader briefly locks and
copies lifecycle fields, unlocks, then locks and copies worker fields, unlocks,
then obtains a queue-mutex snapshot. No locks are nested. Each domain is exact
at its copy point, but the combined result spans a bounded observation window
and is not one exact instant.

This model is data-race safe, preserves current lock ordering, avoids atomics,
and prevents a diagnostic reader from holding the lifecycle lock while blocked
on the queue. It can briefly block submissions, accounting, or queue mutations
one domain at a time. Derived cross-domain invariants are classified as:

- stable invariants, valid regardless of capture skew, such as bounds;
- quiescent invariants, evaluated only before start or after successful join;
- advisory live invariants, reported as indeterminate if skew can explain them.

A sequence value before/after each domain may later identify concurrent
mutation, but Phase 6.2 must not spin until a globally atomic view appears.

## 10. Health model

Health is derived, not stored independently:

| Health | Exact conditions | Submissions | Queue/workers | Action/recovery |
|---|---|---|---|---|
| `HEALTHY` | `INITIALIZED` or `RUNNING`, no failure/overflow/invariant flags, full configured worker set when running | Only if running | Continue normally | None; recoverable by normal lifecycle |
| `DEGRADED` | Running with callback failures or diagnostic counter overflow, but worker infrastructure and invariants intact | Allowed while lifecycle is running | Continue drain/execution | Inspect application failures; no automatic recovery |
| `STOPPING` | `SHUTTING_DOWN` | Rejected | Queued Tasks drain; workers continue | Join required |
| `STOPPED` | `STOPPED`, zero active workers, all created workers joined | Rejected | No callback continues | Destroy or inspect |
| `FAILED` | Private `FAILED`, worker/synchronization/invariant failure, or created-worker deficit after start | Rejected | Best-effort shutdown/join only | Operator cleanup/diagnosis required |

Queue saturation alone does not imply degraded health without an evidence-based
duration threshold. No threshold is invented here; future thresholds require
soak evidence and an explicit configuration/clock contract.

## 11. Lifecycle invariants

| Invariant | Status |
|---|---|
| `submitted = accepted + rejected` after each submit call is classified | Requires Phase 6 counters |
| `accepted = queued + running + completed + failed` | Quiescent/exact after consistent accounting; advisory live |
| `running <= configured workers` | Required and live-testable |
| `queue size <= capacity` | Already structurally enforced and tested |
| callback completion is counted exactly once per dequeue | Existing control flow; needs invariant tests |
| failed callback is not also successful | Existing exclusive branch; testable now |
| cancelled work is not later executed | Not applicable: scheduler cancellation absent |
| `joined <= created <= configured` | Requires new counters; testable |
| `active <= created` | Requires active accounting; testable |
| stopped implies zero active workers | Existing join contract; future snapshot test |
| destroyed implies no owned synchronization resources | Existing cleanup contract/tests; wrapper becomes null |
| no submission accepted after gate closure | Existing gate and tests |
| successful shutdown/join implies all created workers joined, or failure is explicit | Existing join result; diagnostics need worker detail |

`accepted = rejected` is not an invariant. Task states `COMPLETED`, `FAILED`,
and `CANCELLED` are distinguishable in the Task domain, but scheduler callbacks
do not transition them; operational callback outcomes must use scheduler
accounting unless the application explicitly manages Task state.

## 12. Failure taxonomy

| Category | Detection/propagation/state | Accounting and cleanup | Diagnostic/test |
|---|---|---|---|
| Configuration | Public argument validation; `INVALID_ARGUMENT`; no state | No ownership acquired | Invalid-input unit tests |
| Allocation | Allocation site; `ALLOCATION`; initialized or failed-safe state | Free prior allocations | Deterministic allocation injection |
| Sync initialization | Backend init; `SYSTEM_ERROR`; no running state | Destroy earlier primitives | Per-stage injection |
| Worker creation | Start loop; `SYSTEM_ERROR`; `FAILED` | Shutdown and join created workers | Nth-create injection |
| Lifecycle misuse | State gate; `INVALID_STATE`/`SHUTDOWN`; unchanged | Caller retains everything | State-matrix tests |
| Submission rejection | Gate/queue; queue-full/shutdown/system result | Rejected Task remains caller-owned | Counters plus saturation tests |
| Queue operation | Worker/submit mapping; `SYSTEM_ERROR`; worker requests shutdown | Accepted pointer is never freed | Queue injection and drain oracle |
| Callback-reported | Nonzero callback return; worker continues; health degraded | Count failed outcome; caller owns Task/context | Deterministic callback fixture |
| Worker internal | Worker result/infrastructure flag; join returns system failure | Best-effort join and retained failure detail | Startup/runtime injection |
| Shutdown/join | Queue shutdown, wait, or native join result | Do not claim resources joined on failure | Repeated shutdown/join injection |
| Invariant violation | Future validator; health failed | Freeze diagnostic flag; no automatic repair | Deliberate internal test corruption only |

All failures use C return codes, stored diagnostic categories, and explicit
cleanup. The library remains silent unless a future optional diagnostic surface
is invoked.

## 13. Callback failure contract

The existing callback already returns `int`: zero means success and nonzero
means failure. The scheduler counts both privately and continues processing.
It does not expose the callback code, change `TaskState`, retry, cancel, or
retain a per-Task result.

Phase 6 should first expose aggregate success/failure in an internal snapshot.
Future compatible options include a separate extended callback API, a
caller-managed result in application context, or an optional completion
observer. Changing the existing callback signature or silently changing Task
state would break compatibility and is rejected for this phase.

## 14. Fault-injection architecture

Future injection is guarded by
`CONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION=OFF`. Normal builds compile no
active trigger. A private per-instance plan uses deterministic operation-kind
and Nth-call triggers, never global mutable state.

| Injection point | Trigger | Expected response/cleanup | Test oracle | Production risk |
|---|---|---|---|---|
| Allocation | Nth private allocation | Correct error; free prior resources | Wrapper/resource state | None when compiled out |
| Queue initialization | Specific init call | Init failure and no wrapper publication | Destructibility/leak observation | Same |
| Mutex/condition init | Primitive type and ordinal | Reverse-order cleanup | Backend handle accounting | Same |
| Worker create/startup | Worker index | Shutdown/join created subset; failed-safe state | Created=joined | Same |
| Enqueue/dequeue | Operation ordinal/result | Propagate; request shutdown on worker error | No fabricated acceptance | Same |
| Callback failure | Task/index fixture, preferably test-side | Count failure and continue | Exact aggregate | No library hook required initially |
| Shutdown/join | Specific backend call | Explicit failure; no false stopped claim | Resource/worker detail retained | Same |

The hook boundary must be immediately adjacent to the real operation, have one
documented evaluation per call, and be excluded from production compilation.
No fault injection is implemented in Checkpoint 6.1.

## 15. Stress and soak strategy

A future `concurrent_scheduler_reliability_tests` executable remains separate
from performance benchmarking and from the six default CTests until bounded CI
cases are selected.

| Class/scenario | Configuration/purpose | Pass condition/timeout/diagnostic | Tier |
|---|---|---|---|
| Integration: high-volume accounting | 4 workers/producers, 1M Tasks | Exact balance; 60 s; snapshot dump | CI candidate |
| Stress: lifecycle repetition | 10k init/start/shutdown/join/destroy cycles | Every cleanup succeeds; 5 min | Extended |
| Stress: capacity one | 4×4, 250k Tasks | Exact drain/no timeout; 60 s | CI candidate |
| Stress: many producers | 4 workers, 16 producers | No lost/reordered ownership; 60 s | Extended |
| Integration: workers > Tasks | 64 workers, 8 Tasks | All accepted once; 30 s | CI candidate |
| Integration: active-producer shutdown | Seeded producer ranges | Accepted drain, later rejection; 30 s | CI candidate |
| Integration: queued shutdown | Block callbacks then close gate | FIFO drain; 30 s | CI candidate |
| Unit/integration: repeated shutdown | Fixed deterministic sequence | Idempotent results/invariants; 10 s | CI |
| Integration: post-stop rejection | Submit after gate close | All rejected as shutdown; 10 s | CI |
| Fault: callback failure | Known failing Task IDs | Exact failed/success totals; 10 s | CI with gate |
| Fault: partial worker create | Fail worker N | Created subset joined; 10 s | CI with gate |
| Stress: long callback | Explicit test release gate, not sleep | Snapshot shows running; release and drain; 30 s | Extended |
| Stress: seeded operations | Fixed PRNG algorithm/seed recorded | Replayable exact oracle; 2 min | Extended |
| Soak: one hour | Repeated mixed bounded workloads | No invariant/resource growth; hard 75 min | Manual/nightly |
| Soak: resource observation | OS process handles/memory externally sampled | No monotonic leak trend | Manual |

Timeouts invalidate a run and trigger lifecycle, queue, worker, counter, seed,
and last-operation diagnostics. Default CI never runs the one-hour soak.

## 16. Operational diagnostics

The library stays quiet during normal and failed operations. Existing and new
status-to-string helpers remain deterministic. Reliability tests/application
code may format a `SchedulerSnapshot` and invariant report to a caller-provided
stream or buffer. A dump includes schema version, health/lifecycle names,
counters, failure flags, consistency metadata, test seed, and first failed
invariant—never Task pointers or arbitrary callback memory.

A logging framework, mandatory file, implicit `stderr`, and OS-monitoring
integration are rejected. A future optional diagnostic callback requires
reentrancy and ownership analysis and is not selected here.

## 17. Public API compatibility

Runtime observation is internal/test-only through Phase 6.3. This permits
schema iteration and overhead validation without ABI commitment. After
reliability evidence, a future optional diagnostic API may copy a
versioned, fixed-width public snapshot into caller storage. It must not expose
private enums, locks, pointers, native handles, or reset live counters.

Checkpoint 6.1 changes no public header or behavior.

## 18. Security and misuse considerations

Snapshots must avoid pointer/address disclosure, callback-context contents,
native handles, and unbounded strings. Counter reads validate output pointers.
Callers must not use diagnostics to bypass external lifecycle serialization.
Fault-injection controls must be private, compile-time gated, and impossible to
activate from untrusted runtime input. Diagnostic formatting must use bounded
buffers and stable enum-name fallbacks.

## 19. Performance overhead considerations

Phase 6.2 must measure default production accounting overhead against the Phase
5 ordinary benchmark. Counters should reuse existing locks, add no I/O, avoid
per-Task allocation and timestamps, and use checked constant-time increments.
Snapshot reads are on demand. Acceptance requires unchanged lock order and no
material regression under a predeclared threshold; otherwise accounting is
reduced or compiled behind a default-off diagnostic option.

## 20. Phase 6 checkpoint roadmap

| Checkpoint | Objective and code scope | Tests/docs | Non-goals | Acceptance / rollback |
|---|---|---|---|---|
| 6.1 | Architecture only | Audit, ADR, risk/roadmap | Runtime features | Docs complete, six tests; rollback docs if unsupported |
| 6.2 | Private counters and hybrid snapshot | Unit/live/quiescent snapshot tests; schema doc | Public API/timing | Race-free, lock order and overhead pass; remove counters if semantics regress |
| 6.3 | Invariant and derived health validation | State/invariant matrix, failure dumps | Recovery | Stable/quiescent oracles pass; disable live assertions if skew produces false failures |
| 6.4 | Default-off per-instance fault injection | Trigger determinism tests and injection catalog | Production faults | Compiled-out normal path; remove any nonzero default behavior |
| 6.5 | Partial-init/failure-path testing | Allocation/sync/thread/join cases; cleanup report | Random chaos | Exact cleanup/ownership; rollback unsafe injection point |
| 6.6 | Reliability stress/soak executable | CI subset, seeded stress, manual soak guide | Performance optimization | Exact balances/no timeout/leak trend; quarantine flaky scenario with evidence |
| 6.7 | Diagnostic formatter and reliability report | Golden deterministic dumps, misuse tests | Logging framework/telemetry service | Bounded quiet library surface; rollback noisy or unsafe output |
| 6.8 | Phase closure | Full reproduction, evidence index, closure report | New features | All reliability gates and defaults pass; do not close with unresolved correctness defect |

Each checkpoint preserves public compatibility unless separately approved,
documents its code and test scope, and introduces only one dependency layer.

## 21. Acceptance criteria

Architecture approval requires an implementation-traced lifecycle, evidence-led
risk register, classified snapshot fields, explicit hybrid consistency,
precise derived health states, lifecycle invariants, failure/ownership taxonomy,
inactive fault boundaries, bounded stress/soak plan, internal-first API choice,
unchanged production behavior, six passing CTests, valid links, and clean
repository hygiene.

Future implementation checkpoints additionally require data-race analysis,
failure cleanup oracles, overhead measurement, and rollback thresholds before
their code is retained.

## 22. Non-goals

No fault injection, watchdog, recovery, retry, persistence, networking, HTTP,
distributed scheduling, work stealing, per-worker queue, dynamic resizing,
logging framework, OS monitoring, timeout semantics, cancellation, or
performance optimization is implemented.

## 23. Architecture decision

Proceed with internal-first, lifetime-scoped observability using existing
synchronization domains and a hybrid domain-consistent snapshot. Keep the
library quiet, preserve the public API, compile future deterministic
per-instance fault injection only when explicitly enabled, and separate
reliability validation from performance benchmarks. Checkpoint 6.2 may
implement only the minimal required counters and private snapshot after a
lock-order and overhead review.

Checkpoint 6.2 implementation note: snapshot version 1 now implements the
hybrid lifecycle → worker/per-worker-atomic → queue capture order. Callback
outcomes use cache-line-aligned, lock-free, single-writer C17 atomic slots to
avoid a new callback mutex acquisition. The interface remains private; health
thresholds, invariant enforcement, and fault injection remain unimplemented.

Checkpoint 6.3 implementation note: private validation separates live-safe
structural bounds from quiescent accounting identities. Overflow makes
accounting validation incomplete while structural checks continue. Health is
derived, and fixed-buffer diagnostics are explicit, deterministic,
allocation-free, and silent. No validator call is present in a scheduler
execution path.
