# Scheduler Performance Evaluation

## 1. Executive summary

Performance Evaluation introduced reproducible benchmarking and compile-time-gated internal
profiling. Minimal-work throughput did not scale positively with more workers
around the current shared bounded queue, and direct measurements support queue
coordination as a material tiny-Task bottleneck. Exact correctness accounting
was investigated and was not the dominant explanation. Transition-aware
signaling reduced signal count but failed the complete acceptance gate, so no
optimization was promoted. Default scheduler behavior remains validated and
unchanged. The work improved evidence and engineering confidence even though
the experiment was rejected.

## 2. Objective

Establish repeatable performance measurement, characterize the scheduler,
measure internal contention, and evaluate one evidence-driven optimization
without weakening correctness.

## 3. Scope and non-goals

The work covers the Windows fixed-worker scheduler and shared bounded queue.
It does not claim cross-platform performance, statistical significance,
universal scalability, or completion of any alternative queue design.

## 4. Evidence progression

The evaluation established a deterministic benchmark harness, captured an
ordinary Release baseline, added separately gated contention profiling,
compared scheduler contention with benchmark-accounting contention, and ran a
controlled signaling experiment with an explicit rollback gate. Raw CSVs,
environment records, analysis tools, and SVG charts retain the evidence behind
this summary.

## 5. Benchmark architecture

The private benchmark provides deterministic no-op, light, medium, and
controlled-blocking callbacks; QPC timing; exact per-Task execution and
checksum validation; producer barriers; warm-ups; repeated measurements; and
stable CSV output. Profiling extends only diagnostic builds.

## 6. Baseline results

One worker was competitive or best for no-op work, minimal-work scaling was
weak or negative, extra producers raised submission cost, and capacity one
caused substantial throughput loss. Medium CPU shifted cost toward callback
computation. Shutdown and join were minor in tested runs. Variation limited
fine-grained claims.

The later scalability matrix used four producers, capacity 64, and 15 samples
per value. Median no-op throughput was approximately 449,000, 1.21 million,
1.26 million, 1.01 million, and 855,000 Tasks/s at 1, 2, 4, 8, and 16 workers.
No-op work therefore saturated around two to four workers. Medium work scaled
from about 57,000 Tasks/s at one worker to 398,000 at eight, while heavy work
scaled from about 4,500 to 30,000. Both CPU workloads flattened at 16 workers
as oversubscription reduced efficiency.

With four workers, increasing producers from one to eight reduced no-op median
throughput by about 17.5% and did not improve medium work. Capacity one reduced
no-op throughput to about 452,000 Tasks/s; capacity 64 reached the useful
plateau near 1.26 million, while 256 and 1,024 provided no consistent gain.
Larger queues absorb bursts but consume more pointer storage.

## 7. Contention profiling results

Four-worker no-op enqueue/dequeue wait rates were 10.929%/8.666%. Capacity-one
rates were 46.550%/43.472%, and eight-worker dequeue waiting reached 40.590%.
No persistent zero-Task worker appeared. Partitioned exact accounting did not
improve no-op or light scaling. Queue coordination remained the strongest
scheduler-side hypothesis with moderate confidence. Profiling overhead was
24.35% for no-op and 13.23% for light CPU, so concurrent timing shares are
diagnostic and non-additive.

## 8. Optimization experiment

Strict empty/full transition-only signaling showed progress risk in smoke
testing, requiring waiter-aware wake-up propagation. The safe candidate reduced
four-worker no-op signals from 2.0 to 0.508 per Task, or 74.59%. Performance and
wait-rate changes were mixed. Medium CPU regressed 5.79%, beyond the mandatory
5% safeguard, so the candidate was rejected and its option defaults to `OFF`.

## 9. Accepted findings

- Capacity one creates heavy producer and worker blocking.
- Eight-worker tiny-Task cases show increased coordination pressure.
- Medium CPU execution is callback dominated.
- Shutdown and join are minor contributors in measured configurations.
- No persistent worker starvation was observed.
- Reduced signaling alone does not reliably reduce wait rates.
- Partitioned correctness accounting did not materially improve scaling.
- Profiling has measurable observer overhead.
- No-op throughput saturates around two to four workers on the measured host.
- CPU-heavy callbacks scale through eight logical processors before flattening.
- Capacity 64 is near the measured useful plateau.
- Additional producers do not improve the tested four-worker workloads.

## 10. Stability evidence

Nine one-million-Task no-op runs and nine 100,000-Task medium runs completed
9.9 million validated callbacks without count mismatches, overflow, hangs, or
unexplained lifecycle failures. A separate repeated-lifecycle test completed
2,100 initialize/start/submit/shutdown/join/destroy cycles. Every cycle ended
with zero active workers and queue depth, balanced created/joined workers,
exact accounting, stopped health, and successful destruction.

These runs are high-volume and repeated-lifecycle evidence, not a long-duration
soak or memory-profiler result. Detailed resident-memory sampling was not
collected.

## 11. Rejected hypotheses

1. **Correctness accounting causes negative scaling — not supported.**
   Partitioned exact accounting did not materially improve no-op or light CPU.
2. **Fewer signals necessarily improve throughput — rejected generally.**
   Signal count fell substantially while throughput and waits remained mixed.
3. **Strict transition-only signaling is always sufficient — rejected.**
   The current architecture exhibited progress risk with sleeping waiters.
4. **The candidate should become the default — rejected.**
   Medium CPU violated the mandatory performance safeguard.

## 12. Unresolved questions

Unresolved work includes alternative queues, batching, per-worker queues, work
stealing, Windows scheduler influence, Linux reproduction, thread affinity,
higher Task-count stability, alternative synchronization primitives, and
worker-count selection by callback granularity. None is implemented here.

## 13. Threats to validity

Windows scheduling, background work, frequency and thermal changes, limited
sample sizes, a single CPU, profiling overhead, and very short callbacks limit
causal and external claims. Reported percentages use medians and are rounded to
three decimals in prose unless greater precision is needed for a gate.

## 14. Production behavior status

The scheduler uses the previously validated unconditional signaling behavior.
Profiling defaults to `OFF`; transition-aware signaling defaults to `OFF`.
Neither experiment is public. Public APIs, FIFO semantics, bounded blocking,
lifecycle, shutdown, ownership, and Task execution are
unchanged.

## 15. Correctness validation

Normal, profiling, and retained-candidate builds compile warning-free. Exactly
seven normal CTests pass, direct test executables pass, benchmark self-tests
pass, and the main executable reports version 1.0.0 with initialized status.
Fault-enabled builds register and pass an eighth dedicated test.

## 16. Reproducibility

The [benchmark methodology](../benchmarks/README.md), committed environment
records, raw CSVs, summaries, and charts preserve the evidence chain. Run
[`scripts/reproduce_performance_evaluation.ps1`](../scripts/reproduce_performance_evaluation.ps1) for build and
self-test validation without regenerating historical matrices.

## 17. Portfolio interpretation

The project demonstrates a bounded concurrent queue, fixed worker pool,
lifecycle and shutdown correctness, deterministic testing, high-resolution
benchmarking, contention profiling, controlled A/B evaluation, explicit
acceptance and rollback criteria, and honest rejection of an underperforming
optimization. The value is disciplined performance engineering, not an
unsupported speedup claim.

Portfolio paragraph: Built and profiled a bounded C task scheduler with a fixed
worker pool, deterministic correctness tests, Windows high-resolution timing,
queue-contention instrumentation, and controlled A/B experiments. Profiling
identified shared queue coordination as material for tiny Tasks. A
signal-reduction strategy was rejected after violating a medium-workload
safeguard, preserving validated production behavior.

## 18. Recommended future work

Reproduce the evidence on a quieter Windows environment and Linux before
selecting another single-variable experiment. Any future queue, batching,
affinity, or worker-selection study needs its own correctness and rollback gate.

## 19. Conclusion

Benchmarking, profiling, controlled experimentation, rollback, evidence
retention, and production-behavior checks support the conclusions above. No
unvalidated optimization is enabled in the default scheduler.
