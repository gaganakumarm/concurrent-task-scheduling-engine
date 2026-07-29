# Phase 6 lifecycle invariant validation

## 1. Executive summary

Checkpoint 6.3 adds a private, pure validator for version 1 scheduler
snapshots. It reports stable issues, derives health, and formats deterministic
diagnostics without changing scheduler execution.

## 2. Scope

Validation operates on an already-captured snapshot in live or quiescent mode.
The implementation is internal and test-oriented.

## 3. Non-goals

There is no public monitoring API, runtime enforcement, automatic recovery,
logging, watchdog, timing threshold, callback timeout, or fault injection.

## 4. Validation modes

`SCHEDULER_VALIDATION_LIVE` evaluates bounds safe across a hybrid capture
window. `SCHEDULER_VALIDATION_QUIESCENT` additionally evaluates exact balances
for initialized, stopped, or fully cleaned failed states. Using quiescent mode
on a live snapshot records incomplete validation, not corruption.

## 5. Issue model

Stable private issue codes use informational, advisory, violation, or
incomplete severity. A result stores 16 issue records and total category
counts. Excess issues set `issues_truncated` without overwriting stored issues.
Each record contains a code and fixed-width observed and expected values.

## 6. Live-safe invariants

Live checks cover schema and consistency identifiers, queue capacity and
high-water bounds, configured/created/ready/active/joined worker bounds,
active/joined mutual exclusion, running callback bounds, outcome/start bounds,
submission bounds, dequeue/acceptance bounds, callback/dequeue bounds, and
lifecycle/gate/shutdown compatibility.

Live mode deliberately omits submitted/accepted/rejected, accepted/dequeued,
dequeued/started, and started/outcome equalities because separate domains can
advance between capture points.

## 7. Quiescent invariants

Without overflow, applicable quiescent validation requires submitted to equal
accepted plus rejected, accepted to equal dequeued, dequeued to equal callback
starts, and starts to equal successes plus failures. Queue size, running
callbacks, and active workers must be zero. A stopped scheduler must have
joined every created worker.

## 8. Advisory observations

Advisory severity is reserved for explainable observations. Callback and
infrastructure failure counters degrade derived health without being labeled
structural corruption. Queue saturation alone has no health threshold.

## 9. Hybrid snapshot limitations

Each lifecycle, worker, and queue domain is exact at its copy point, but the
combined snapshot is not globally atomic. Pure validation acquires no locks.
Cross-domain identities are therefore quiescent-only.

## 10. Overflow behavior

`overflow_detected` records an incomplete issue. Structural checks continue,
but saturated accounting bounds and equalities do not produce corruption
claims. Overflow derives degraded rather than failed health.

## 11. Health derivation

Health is computed, never stored. Failed lifecycle or invariant violation is
`FAILED`; shutdown in progress is `STOPPING`; a valid stopped snapshot is
`STOPPED`; failures, advisory results, or incomplete accounting are
`DEGRADED`; otherwise initialized or running state is `HEALTHY`.

## 12. Diagnostic formatting

The formatter writes stable names and values to caller storage, reports the
required length, safely truncates, and null-terminates nonempty buffers. It
allocates and emits nothing and includes no address, timestamp, or external
data.

## 13. Test strategy

The existing observability CTest covers real initialized, running, blocked
callback, and stopped snapshots plus synthetic invalid snapshots. It checks
mode misuse, bounds and balances, overflow, issue and format truncation, invalid
arguments, five health states, callback degradation, and deterministic repeated
validation. Synchronization is condition-based; no sleep is used.

## 14. Performance validation

The validator is a separate module with no call site in submission, queue,
worker, callback, shutdown, or join paths. Release build `09bc87e` is compared
with the working tree in alternating four-worker no-op and medium-CPU runs,
using 15 valid samples per build and workload. Raw CSV is retained outside the
repository.

The comparison used four workers, four producers, capacity 64, 100,000 Tasks,
one warm-up per invocation, one measured iteration, and alternating baseline
then candidate runs:

| Workload | Build | Median Tasks/s | Minimum | Maximum | CV |
|---|---|---:|---:|---:|---:|
| no-op | `09bc87e` | 1,297,444.294 | 1,138,530.726 | 1,501,424.852 | 6.286% |
| no-op | candidate | 1,246,197.540 | 1,032,190.939 | 1,443,343.013 | 7.672% |
| medium CPU | `09bc87e` | 229,993.710 | 142,286.715 | 248,054.693 | 11.074% |
| medium CPU | candidate | 234,270.274 | 68,645.322 | 247,554.287 | 19.938% |

Candidate median differences are -3.950% for no-op and +1.859% for medium
CPU. Both are within the 5% safeguard. Wide overlapping ranges, including a
low candidate medium outlier, prevent a statistical claim, but the result does
not corroborate the earlier medium regression. Code inspection confirms
Checkpoint 6.3 adds no hot-path call, write, or lock. Raw CSV and the baseline
build are retained under ignored `build/checkpoint-6.3-performance-raw/`.

## 15. Known limitations

The interface is private, issue capacity is fixed, health has no timing signal,
and live hybrid snapshots cannot prove cross-domain equalities. Validation is
explicit rather than automatic.

Deterministic failure snapshots used by Checkpoint 6.4 are described in
[Phase 6 deterministic fault injection](phase-6-deterministic-fault-injection.md).

## 16. Acceptance results

Normal and profiling builds are warning-free. Seven of seven CTests, all
direct test executables, the main regression, and both benchmark self-tests
pass. Public headers and Phase 5 evidence are unchanged. Final link and
repository hygiene results are recorded in the checkpoint report.

## 17. Checkpoint decision

The functional and performance acceptance gates support approval, subject to
the final repository hygiene audit.
