# Scheduler benchmark methodology

## Purpose and non-goals

`concurrent_scheduler_benchmarks` measures the completed fixed-worker
scheduler with deterministic workloads. It is an explicit benchmark program,
not a correctness CTest. This checkpoint establishes measurement machinery; it
does not claim that one configuration is faster, identify a bottleneck, or
justify production optimization.

The harness never changes scheduler ownership: Tasks and callback state are
preallocated by the benchmark and remain valid through `scheduler_join`.
Every iteration creates a fresh lifecycle:

`init -> start -> submit -> shutdown -> join -> destroy`

Restart is not attempted.

## Build and invocation

Use a separate release build:

```powershell
cmake -S . -B build-bench -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench
.\build-bench\concurrent_scheduler_benchmarks.exe --help
.\build-bench\concurrent_scheduler_benchmarks.exe --self-test
```

MinGW Makefiles is a single-configuration generator. Consequently
`CMAKE_BUILD_TYPE=Release` selects the Release flags while configuring this
build tree; inspect `CMakeCache.txt` and verbose build output when comparing
toolchains.

Example:

```powershell
.\build-bench\concurrent_scheduler_benchmarks.exe `
  --scenario throughput --workers 4 --producers 4 --capacity 16 `
  --tasks 1000 --warmup 2 --iterations 5 `
  --callback-profile noop --mode validated `
  --output benchmark-results.csv
```

Options are `--help`, `--self-test`, `--scenario`, `--workers`,
`--producers`, `--capacity`, `--tasks`, `--warmup`, `--iterations`,
`--callback-profile`, `--mode`, and `--output`. Numeric values must be positive,
fully formed, and representable as `size_t`. The Task count must be at least
the producer count.

## Scenarios and controlled dimensions

Named scenarios provide starting configurations:

| Name | Producers | Workers | Profile | Special control |
|---|---:|---:|---|---|
| `s1` | 1 | 1 | noop | scheduler/queue overhead |
| `s2` | 1 | 4 | noop | worker comparison |
| `s3` | 4 | 4 | noop | producer contention |
| `s4` | 4 | 8 | noop | higher worker count |
| `s5` | 4 | 4 | light CPU | small deterministic work |
| `s6` | 4 | 4 | medium CPU | callback-dominated work |
| `s7` | 1 | 1 | controlled blocking | capacity one |
| `s8` | 4 | 4 | noop | run separately at capacities 1/16/64/256 |
| `throughput` | configurable | configurable | configurable | general run |

Supported comparison matrices are workers 1/2/4/8, producers 1/2/4, and
capacities 1/16/64/256. Task tiers are quick (1,000), standard (100,000), and
extended (1,000,000). The executable does not automatically run their Cartesian
product.

For a queue-capacity comparison, hold workers, producers, callback profile,
Task count, mode, warm-ups, and measured iterations fixed, changing only
capacity. For worker scaling, vary only workers and report
`speedup(N) = throughput(N) / throughput(1)` and
`efficiency(N) = speedup(N) / N`. For producer scaling, vary only producers.
Callback-cost comparisons keep every other setting fixed.

## Callback profiles

- `noop` performs a minimal checksum contribution under benchmark accounting.
- `light_cpu` performs exactly 128 fixed unsigned operations per Task.
- `medium_cpu` performs exactly 4,096 fixed unsigned operations per Task.
- `controlled_blocking` uses a benchmark mutex and condition variable. The
  first callback announces entry and waits for an explicit controller
  broadcast, creating deterministic capacity pressure without sleep or polling.

All computations use defined unsigned arithmetic, allocate no memory, perform
no I/O, and contribute to an observable checksum.

## Timing and metrics

The private Windows timer uses `QueryPerformanceCounter` and retrieves a
validated `QueryPerformanceFrequency` for each iteration. Counter values are
monotonic for benchmark purposes. Tick differences are checked for underflow
and converted to integer nanoseconds with overflow checks. Conversion truncates
fractions smaller than one nanosecond; the nanosecond unit does not imply
nanosecond hardware precision. UTC is used only to label CSV rows, never to
measure duration.

Timing boundaries are:

- Submission latency: immediately before through immediately after
  `scheduler_submit`; capacity waiting is included and callback completion is
  excluded.
- End-to-end latency: immediately before submission through the completion
  timestamp recorded by the callback.
- Total throughput: immediately before the controller releases all ready
  producers through return from successful `scheduler_join`.
- Shutdown latency: immediately before through return from
  `scheduler_shutdown`; callback drain and worker joining are excluded.
- Join latency: immediately before through return from `scheduler_join`;
  remaining drain and worker termination are included.

The default is three discarded warm-ups followed by ten measured iterations.
Warm-ups use the exact resolved configuration. Results report count, minimum,
maximum, arithmetic mean, p50, p95, and population standard deviation.
Percentiles use nearest-rank indexing: for percentile `p` and sample count `n`,
the zero-based sorted index is `ceil(p*n/100)-1`. Equal values do not make
ordering significant.

No performance outlier is removed. Failed correctness iterations are reported
and excluded by terminating the run with a nonzero status.

## Correctness and accounting

Validated mode is the default. It records a completion timestamp and execution
count for every Task and rejects duplicate or unknown pointers. The gate also
requires successful lifecycle calls, joined producers, exact
accepted/executed equality, zero execution of rejected Tasks, defined counter
arithmetic, and no callback after successful join.

Low-overhead mode retains total execution count and checksum but omits
per-Task completion/distribution accounting. Its unavailable end-to-end CSV
fields are empty, not zero. Do not publish low-overhead results unless the same
scenario first passes validated mode.

Producer ranges are non-overlapping partitions of one stable Task array.
Producers rendezvous on a mutex/condition barrier: each reports ready, the
controller records the start counter, then broadcasts release. Every created
producer is joined.

## CSV schema

CSV has one header and one row per measured iteration; warm-ups are omitted.
Column order is:

`timestamp_utc, scenario, callback_profile, mode, iteration, workers,
producers, capacity, tasks_attempted, tasks_accepted, tasks_executed,
tasks_rejected, submit_duration_ns, shutdown_duration_ns, join_duration_ns,
total_duration_ns, throughput_tasks_per_second, mean_submit_latency_ns,
p50_submit_latency_ns, p95_submit_latency_ns, min_submit_latency_ns,
max_submit_latency_ns, mean_end_to_end_latency_ns,
p50_end_to_end_latency_ns, p95_end_to_end_latency_ns, correctness_passed`

The process uses the C locale-independent integer syntax and fixed `.` decimal
format emitted by the C runtime's initial locale. Generated result files are
validation artifacts and should be removed rather than committed.

## Reproducibility checklist

1. Record OS, architecture, compiler, build type, CPU, and logical processors.
2. Use a clean Release build and the same executable.
3. Close unnecessary applications and avoid console activity while measuring.
4. Fix scenario, mode, profile, workers, producers, capacity, Task count,
   warm-ups, and measured iterations.
5. Run self-test first and retain raw CSV outside the source tree.
6. Repeat suspicious scenarios and compare accounting before timing.
7. Report all iterations, including minimum, maximum, median, p95, and
   standard deviation.

## Threats, limitations, and interpretation

Windows scheduling noise, background processes, CPU frequency scaling, thermal
throttling, antivirus activity, timer resolution, first-run page faults,
allocator state, console output, accounting overhead, debug builds, and small
samples can all affect results. Warm-ups and repeated Release measurements
reduce but do not eliminate these effects.

Results describe only the measured machine and configuration. They do not
establish universal scalability, worker utilization, causality when multiple
variables change, or a production optimization opportunity. Memory usage and
CPU time are not reported because this checkpoint has no validated portable
measurement mechanism for them.
