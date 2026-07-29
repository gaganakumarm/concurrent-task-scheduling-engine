# Phase 7.1 performance, scalability, and stability validation

## 1. Executive summary

Phase 7.1 measured the unchanged scheduler implementation at baseline commit
`08a745c` on one Windows laptop. All recorded runs passed exact post-join
runtime-accounting and lifecycle validation. No overflow, rejection, callback
failure, cleanup failure, hang, or crash was observed.

The results confirm the Phase 5 conclusion: the shared bounded queue is a
material bottleneck for tiny tasks. CPU workloads scale usefully through the
four physical cores and continue to benefit at eight logical processors.
No-op work peaks much earlier. No optimization was introduced.

## 2. Scope

This checkpoint covers worker, producer, capacity, task-volume, callback-cost,
contention, sustained-load, lifecycle-stress, and historical-regression
measurements. Results describe only the configurations and system below.

## 3. Non-goals

There is no scheduling redesign, work stealing, dynamic scaling, lock-free
queue, automatic tuning, public monitoring, production fault control, or
benchmark-specific production shortcut in this change.

## 4. Baseline commit

The audited Phase 7.1 baseline is `08a745c` (`complete scheduler reliability
validation`) on `main`, equal to `origin/main`. The tree was initially clean.
The Phase 5 tag `v0.5-performance-engineering` resolves to `0527b6d`.

## 5. Hardware and software environment

| Item | Value |
|---|---|
| OS | Windows 11 Home Single Language 10.0.26200, build 26200 |
| CPU | Intel Core i5-1155G7 |
| Cores | 4 physical, 8 logical |
| Memory | 8,215,060,480 physical bytes (about 7.65 GiB) |
| Compiler | GCC 16.1.0, MSYS2 UCRT64 |
| CMake | 4.4.0 |
| Python | 3.12.7 |
| Build | Release, MinGW Makefiles |
| Power plan | Balanced |
| External power | Not recorded |

The machine was not isolated from normal Windows services and OneDrive. Some
runs have visible outliers. These results are not cross-machine comparable.

## 6. Benchmark methodology

Ordinary matrices use profiling OFF, fault injection OFF, transition-aware
signaling OFF, one warm-up, and 15 recorded samples. Sustained cases use one
warm-up and nine samples. Median throughput is primary; ranges, population
standard deviation, coefficient of variation (CV), elapsed p50/p95, and maximum
queue high-water are calculated by `tools/analyze_benchmark.py`.

Every measured iteration initializes, starts, submits, shuts down, joins,
captures a snapshot, validates quiescence and STOPPED health, then destroys the
scheduler. Snapshot capture is after the measured interval.

The Phase 5 CSV is preserved. Phase 7 uses an append-only schema extension,
identified here as the Phase 7.1 schema, adding snapshot version/consistency,
all scheduler accounting counters, queue depth/high-water, validation outcome,
and derived health after `correctness_passed`.

## 7. Workload definitions

| Workload | Deterministic work per callback |
|---|---|
| `noop` | Callback accounting and checksum contribution only |
| `light_cpu` | 128 integer mixing operations |
| `medium_cpu` | 4,096 integer mixing operations |
| `heavy_cpu` | 65,536 integer mixing operations |

The first three retain their Phase 5 semantics. `heavy_cpu` is new and uses the
same deterministic, allocation-free, I/O-free mixing function. Exact aggregate
checksum validation prevents removal of callback work.

## 8. Worker scalability

Fixed controls were four producers, capacity 64, and 100,000 no-op, 20,000
medium, or 2,000 heavy tasks. Each value is 15 samples.

| Workload | Workers | Median tasks/s | Min | Max | CV | vs 1 worker | Efficiency | High-water |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| No-op | 1 | 449,483 | 323,230 | 700,556 | 22.31% | 1.00x | 100% | 64 |
| No-op | 2 | 1,210,079 | 1,025,362 | 1,683,779 | 11.63% | 2.69x | 135% | 64 |
| No-op | 4 | 1,257,356 | 1,112,986 | 1,475,523 | 8.06% | 2.80x | 70% | 64 |
| No-op | 8 | 1,014,416 | 872,896 | 1,117,018 | 6.05% | 2.26x | 28% | 64 |
| No-op | 16 | 854,542 | 652,394 | 953,571 | 11.92% | 1.90x | 12% | 64 |
| Medium | 1 | 57,056 | 11,129 | 77,500 | 30.21% | 1.00x | 100% | 64 |
| Medium | 2 | 117,263 | 100,378 | 124,196 | 5.70% | 2.06x | 103% | 64 |
| Medium | 4 | 217,784 | 146,210 | 251,397 | 11.16% | 3.82x | 95% | 64 |
| Medium | 8 | 398,122 | 362,961 | 421,639 | 4.04% | 6.98x | 87% | 64 |
| Medium | 16 | 407,635 | 267,849 | 425,412 | 11.70% | 7.14x | 45% | 64 |
| Heavy | 1 | 4,494 | 3,865 | 4,652 | 5.34% | 1.00x | 100% | 64 |
| Heavy | 2 | 9,403 | 7,783 | 9,612 | 6.21% | 2.09x | 105% | 64 |
| Heavy | 4 | 17,669 | 13,654 | 18,263 | 8.90% | 3.93x | 98% | 64 |
| Heavy | 8 | 30,352 | 25,354 | 31,720 | 6.28% | 6.75x | 84% | 64 |
| Heavy | 16 | 31,628 | 27,554 | 32,989 | 4.19% | 7.04x | 44% | 64 |

Superlinear two-worker no-op ratios reflect the noisy one-worker baseline and
must not be read as an architectural property. No-op saturates around two to
four workers and degrades beyond four. CPU work scales through eight logical
processors; 16 workers add little throughput and halve efficiency.

## 9. Producer scalability

Controls were four workers, capacity 64, and 100,000 no-op or 20,000 medium
tasks.

| Workload | Producers | Median tasks/s | Min | Max | CV | High-water |
|---|---:|---:|---:|---:|---:|---:|
| No-op | 1 | 1,258,637 | 1,069,103 | 1,340,910 | 6.19% | 64 |
| No-op | 2 | 1,208,255 | 1,028,044 | 1,276,160 | 5.57% | 64 |
| No-op | 4 | 1,246,073 | 950,272 | 1,447,566 | 11.57% | 64 |
| No-op | 8 | 1,038,527 | 936,943 | 1,137,161 | 5.54% | 64 |
| Medium | 1 | 261,894 | 223,252 | 274,398 | 5.13% | 64 |
| Medium | 2 | 236,992 | 128,052 | 258,419 | 16.15% | 64 |
| Medium | 4 | 220,788 | 202,797 | 249,109 | 6.83% | 64 |
| Medium | 8 | 221,430 | 178,674 | 236,285 | 7.40% | 64 |

Additional producers do not improve these cases. Eight producers reduce no-op
median by 17.5% relative to one producer. Medium work is worker-limited while
additional producers add queue synchronization pressure. All submissions were
accepted and the queue reached capacity.

## 10. Queue-capacity analysis

Controls were four workers, four producers, and the same task counts above.

| Capacity | No-op median | No-op CV | Medium median | Medium CV | High-water |
|---:|---:|---:|---:|---:|---:|
| 1 | 452,184 | 32.54% | 105,823 | 19.01% | 1 |
| 8 | 1,189,837 | 5.98% | 214,017 | 7.67% | 8 |
| 64 | 1,255,993 | 3.79% | 232,546 | 8.11% | 64 |
| 256 | 1,268,760 | 4.91% | 225,472 | 6.09% | 256 |
| 1,024 | 1,259,106 | 12.46% | 218,491 | 7.25% | 1,024 |

Capacity 1 exposes producer blocking and poor burst absorption. Capacity 64 is
near the useful plateau. Larger queues consume more pointer storage and absorb
larger bursts but do not consistently increase throughput. Shutdown, draining,
and accounting remained correct at every capacity.

## 11. Task-count analysis

Controls were four workers, four producers, and capacity 64.

| Workload | Tasks | Median tasks/s | Min | Max | CV |
|---|---:|---:|---:|---:|---:|
| No-op | 10,000 | 1,196,945 | 1,125,239 | 1,546,575 | 9.45% |
| No-op | 100,000 | 1,260,138 | 1,160,723 | 1,448,729 | 5.62% |
| No-op | 1,000,000 | 1,201,102 | 436,674 | 1,401,781 | 30.25% |
| Medium | 10,000 | 209,901 | 129,459 | 249,066 | 14.90% |
| Medium | 50,000 | 232,568 | 205,654 | 240,422 | 4.43% |
| Medium | 100,000 | 236,914 | 210,120 | 248,351 | 4.08% |

Medium used 50,000 instead of 1,000,000 as an intermediate practical volume;
100,000 is already about ten times the callback work of the million-task no-op
case. Fixed setup is more visible at 10,000 tasks. Medium throughput stabilizes
as volume increases. The million-task no-op median is stable relative to
100,000, but background-load outliers raise CV to 30.25%.

## 12. Sustained stability

Nine long-lived, one-million-task no-op schedulers plus nine 100,000-task
medium schedulers completed in 30.66 seconds of wall time. This is a
high-volume equivalent (9.9 million validated callbacks), not a ten-minute
soak. Every iteration completed a full terminal snapshot validation.

There were zero failures, count mismatches, overflows, hangs, or unexplained
resource trends. The process was observed only through lifecycle completion
and OS execution success; detailed resident-memory sampling was not collected.

## 13. Repeated lifecycle stress

The test completed 2,100 cycles: 1,000 one-worker/capacity-1 no-op cycles with
32 tasks, 1,000 four-worker/capacity-64 no-op cycles with 64 tasks, and 100
four-worker/capacity-64 medium cycles with 64 tasks. Wall times were 0.332,
0.769, and 0.204 seconds respectively. Failures and first failing cycle: none.
Every cycle ended with zero active workers and queue depth, balanced
created/joined workers, exact counters, STOPPED health, and successful destroy.

## 14. Contention profiling

Profiling was compile-time enabled only in a separate Release build. Values are
medians of nine samples; wait counts are events, not wait durations.

| Case | Throughput | Enq contended | Deq contended | Enq waits | Deq waits | Mean occupancy | Full observations |
|---|---:|---:|---:|---:|---:|---:|---:|
| No-op, 4w/1p | 1,085,349 | 15,358 | 32,293 | 42 | 76,165 | 1.56 | 24 |
| No-op, 4w/4p | 1,138,427 | 55,269 | 55,302 | 4,499 | 5,782 | 35.44 | 16,769 |
| No-op, 4w/8p | 903,805 | 59,391 | 53,161 | 29,020 | 6,393 | 45.87 | 32,423 |
| Medium, 4w/4p | 221,166 | 2,138 | 5,779 | 17,328 | 19 | 62.37 | 10,583 |
| Medium, 8w/4p | 372,419 | 331 | 2,495 | 6,860 | 443 | 55.06 | 8,253 |

Attempts are exactly one enqueue and approximately one dequeue per task.
Standard benchmark accounting was kept separate: median accounting time was
18.4--22.1 ms for profiled no-op cases versus only 2.3--4.8 ms of callback
compute, while queue lock/wait counters are scheduler coordination evidence.
For medium cases, callback compute was 279.8--315.4 ms versus 4.5--5.3 ms
accounting. Thus profiling overhead and benchmark accounting must not be
mistaken for production wait duration.

Every case emitted one not-empty and one not-full signal per task because
transition signaling remained OFF. The Phase 5 conclusion remains valid:
shared queue coordination dominates tiny work and producer pressure increases
contention. CPU work amortizes coordination. The previously rejected signaling
experiment was not re-enabled.

## 15. Historical regression comparison

The tagged Phase 5 build (`0527b6d`) and current baseline production code were
built Release on the same machine. Runs alternated baseline then candidate in
five pairs of three recorded samples, with a warm-up in each invocation: 15
samples per build/workload, four workers/producers, capacity 64, 100,000 no-op
or 20,000 medium tasks.

| Workload | Build | Median | Min | Max | CV | Candidate difference |
|---|---|---:|---:|---:|---:|---:|
| No-op | Phase 5 | 1,291,322 | 1,149,252 | 1,433,143 | 5.53% | — |
| No-op | Current | 1,210,131 | 1,177,472 | 1,509,427 | 8.40% | -6.29% |
| Medium | Phase 5 | 218,187 | 172,015 | 255,200 | 10.48% | — |
| Medium | Current | 226,787 | 199,995 | 247,922 | 5.27% | +3.94% |

Because the no-op gap crossed 5% but ranges overlapped, a fresh alternating
15-sample reproduction was required. It measured Phase 5 at 1,281,159 median
(1,093,842--1,473,312, CV 7.04%) and current at 1,254,765
(1,122,725--1,476,115, CV 8.73%), a -2.06% gap. Therefore the greater-than-5%
gap was not repeatable. The latest pre-Phase-7 commit is the current baseline
itself; Phase 7 changes only benchmark code and captures snapshots after the
timed interval. No repeatable regression above 5% is established.

## 16. Correctness validation

All published samples required benchmark checksum correctness, submitted =
accepted = dequeued = callback-started = callback-succeeded = expected tasks,
and rejected = callback-failed = currently-running = zero. Invalid samples are
rejected by the analysis script and are not eligible for throughput reporting.

## 17. Variability and measurement limitations

CV ranges from about 3.8% to 32.5%. Single-worker medium, capacity-1, and the
million-task no-op case are especially noisy. Min/max ranges are shown so
medians are not overinterpreted. Windows scheduling, background services,
Balanced power policy, thermal state, and unrecorded external-power state were
not controlled. No confidence intervals, energy use, cache counters, precise
wait duration attribution, or memory time series were collected.

## 18. Bottlenecks and saturation

No-op saturates at two to four workers and degrades with eight and sixteen.
Profiling ties this to queue mutex/condition coordination and per-task
signaling. Medium and heavy work scale to eight logical processors, then
flatten under oversubscription. Producer scaling shows that submission
parallelism is not the limiting resource in the tested four-worker cases.
Capacity below eight is restrictive; capacity above 64 gives little benefit.

## 19. Optimization decisions

No production optimization was made. Evidence continues to support queue
coordination as a possible future investigation, but Phase 5 already showed
that reducing signals can harm medium work. Any new candidate requires its own
controlled checkpoint and workload safeguards.

## 20. Known limitations

The test covers one x86-64 Windows host, fixed worker pools, one bounded FIFO,
successful callbacks, and synthetic CPU work. It is not a latency service-level
test, ten-minute soak, allocation benchmark, memory profiler, portability
comparison, or production workload model.

## 21. Reproduction instructions

Configure normal and profiling Release builds as documented in `README.md`.
Run the benchmark with explicit `--workers`, `--producers`, `--capacity`,
`--tasks`, `--warmup 1`, `--iterations 15`, `--callback-profile`, and
`--output` arguments. Keep output below an ignored build directory. Summarize:

```powershell
python tools\analyze_benchmark.py <csv-files> --output build\summary.csv
```

The script validates correctness, overflow, lifecycle validation, and STOPPED
health before calculating statistics. Profiling reproduction additionally
requires `-DCONCURRENT_SCHEDULER_ENABLE_PROFILING=ON`.

## 22. Acceptance result

All 32 checkpoint acceptance criteria passed. Normal, fault-enabled, and
profiling builds/tests pass; the matrices and stress cases completed; exact
accounting remained valid; no repeatable regression above 5% was found; public
headers and production scheduler code are unchanged; raw data is ignored.

## 23. Phase 7.2 readiness

Phase 7.1 is ready for review. Phase 7.2 has not started. The recommended next
step is to preserve this evidence as the performance baseline and scope any
future queue-coordination experiment separately.
