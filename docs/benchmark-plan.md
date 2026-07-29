# Scheduler Benchmark Plan

## Goals and evaluation sequence

Performance Evaluation measures the completed scheduler without changing its semantics.
The benchmark foundation establishes the timer, deterministic harness,
correctness gate, CLI, statistics, and raw CSV export. Baseline evaluation
captures controlled machine-specific observations. An optimization hypothesis
may be tested only after the optimization gate is met.

The measurement questions are:

- What complete-workload and submission throughput is observed?
- How are submission and end-to-end latency distributed?
- How much measured time is spent in shutdown and join?
- How do workers, producers, capacity, and deterministic callback cost affect
  results when varied independently?
- Does accepted, executed, and rejected accounting remain exact?

## Variables and controls

Independent variables are worker count, producer count, queue capacity,
callback profile, Task count, and accounting mode. Controlled variables include
the executable and build type, machine, callback operation count, warm-up and
iteration counts, submission API, Task initialization, producer coordination,
and lifecycle sequence.

Capacity studies vary only capacity. Worker studies vary only workers.
Producer studies vary only producers. Callback-cost studies vary only callback
profile. The primary tiers are 1,000, 100,000, and 1,000,000 Tasks; the full
Cartesian product is not automatic.

## Methodology

Each iteration owns stable preallocated Task and measurement arrays, a fresh
scheduler, non-overlapping producer ranges, and callback context. Producers
wait on a condition-variable start barrier. The controller records the
monotonic start counter immediately before broadcast. Submission duration ends
after all producers join; shutdown and join use distinct boundaries; total
duration ends after scheduler join. Warm-ups repeat the same lifecycle and are
discarded.

Validated mode requires exact once-only execution of every accepted pointer,
zero execution of rejected or unknown pointers, successful producer joins and
scheduler lifecycle, stable context lifetime, and safe counters. Invalid runs
are failures, not statistical samples. Low-overhead mode is secondary and must
be preceded by validated mode for the same scenario.

Raw iteration rows are the measurement record. Summaries include all valid
iterations and never remove outliers automatically. Percentiles use the
nearest-rank definition documented with the harness.

## Baseline-first optimization gate

No production optimization may be introduced until:

1. the benchmark harness is validated;
2. baseline results are captured;
3. a bottleneck is measured;
4. an optimization hypothesis is written;
5. unchanged correctness tests pass; and
6. the post-change benchmark uses the same methodology.

The benchmark harness itself makes no performance claim and changes no
scheduler, queue, callback, lifecycle, shutdown, join, ownership, or public API
behavior.

## Threats to validity

Windows scheduler noise, background processes, frequency scaling, thermal
throttling, antivirus activity, timer limitations, first-run page faults,
allocator state, console interference, callback-accounting overhead,
debug-versus-Release differences, and small samples remain threats.

Mitigation is to close unnecessary programs, use a recorded Release build,
avoid measured-loop output, use identical warm-ups, take multiple measurements,
report median and p95 with the full range, repeat suspicious runs, and record
machine details. These steps do not eliminate environmental noise.

Results are specific to the current machine, toolchain, executable, and
configuration. They must not be presented as universal scalability, precise
worker utilization, or evidence that an unmeasured production change is
beneficial.
