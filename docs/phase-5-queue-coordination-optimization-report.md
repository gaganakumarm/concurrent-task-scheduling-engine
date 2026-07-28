# Phase 5 queue-coordination optimization report

## 1. Executive decision

**OPTIMIZATION REJECTED.** Signaling fell substantially in most configurations
and selected no-op medians improved, but medium CPU regressed 5.79%, exceeding
the mandatory 5% safeguard. The internal switch therefore defaults to `OFF`.

## 2. Baseline evidence from Checkpoint 5.3

Checkpoint 5.3 directly measured queue waits, lock acquisition, signaling,
occupancy, and worker distribution. It supported shared queue coordination as
the strongest scheduler-side bottleneck hypothesis for minimal callbacks.

## 3. Optimization hypothesis

Avoiding condition signals when no boundary transition and no corresponding
waiter exists may reduce unnecessary kernel/user synchronization overhead.

## 4. Correctness and lost-wakeup risk analysis

Strict empty-to-non-empty signaling can leave multiple consumers asleep while
one consumer drains multiple queued Tasks. Strict full-to-not-full signaling
can similarly leave producers asleep while slots remain. One active thread can
often propagate progress, but the pre-matrix strict-policy smoke test stalled,
demonstrating that the current lifecycle cannot rely on boundary transitions
alone. The safe experiment tracks waiters under the existing mutex and signals
when a boundary changes or a corresponding waiter remains. This allows an
awakened thread to propagate wake-ups. Predicate loops, mutex ownership, and
lock order are unchanged. Shutdown broadcasts remain mandatory because all
blocked producers and consumers must observe shutdown.

## 5. Implementation scope

Production behavior changed only inside the private concurrent queue
implementation. A private CMake switch and profiling counters were added; no
public header, Task representation, scheduler API, or ownership rule changed.

## 6. Control and candidate methodology

Control and candidate were Release profiling builds from identical sources.
Control used unconditional signaling; candidate used the safe transition/waiter
policy. Runs alternated order, used two warm-ups and seven retained iterations.

## 7. Environment

Windows NT 10.0.26200.0, Intel i5-1155G7, eight logical processors, GCC 16.1,
CMake 4.4, MinGW Makefiles Release, Balanced power plan. See the environment
evidence file for limitations.

## 8. Correctness results

All 196 primary measured rows passed exact accepted/executed, duplicate,
unknown-pointer, rejected-Task, and checksum validation. All six million-Task
stress rows passed. Existing tests and lifecycle checks passed.

## 9. Signal-count results

Control emitted two signals per Task. Candidate signals per Task ranged from
0.448 at capacity 256 to 2.0 at capacity one. The candidate reduced signals
74.59% in the four-worker/capacity-64 no-op case and 74.38% at capacity 16;
capacity one could avoid none because every mutation crossed a boundary.

## 10. No-op worker scaling

Candidate median deltas at 1/2/4/8 workers were +0.07%, +3.46%, +5.91%, and
-3.60%. The four-worker result clears the 5% improvement threshold, but the
direction is not uniform and the two-worker candidate CV was high.

## 11. Light CPU worker scaling

Candidate deltas at 1/2/4/8 workers were +4.73%, -1.01%, +0.26%, and -2.84%.
No configuration regressed by more than 5%, but no stable improvement emerged.

## 12. Producer scaling

At one/two/four producers, no-op deltas were -0.10%, -4.64%, and +5.91%.
The result is mixed and does not establish producer-scaling improvement.

## 13. Capacity comparison

Capacity 1/16/64/256 deltas were +29.09%, -0.01%, +5.91%, and -4.45%.
Capacity one retained two signals per Task, so its gain cannot be attributed to
signal elimination and illustrates environmental/interaction uncertainty.

## 14. Medium CPU safeguard

Medium CPU throughput fell from 299,459 to 282,115 Tasks/s, an observed median
regression of 5.79%; median duration increased 6.15%. This fails the safeguard.

## 15. Stress and progress validation

Three one-million-Task executions per variant completed exactly. Median
throughput was 1.192M control and 1.280M candidate Tasks/s, but the candidate
runs included a 0.913M outlier. No matrix run deadlocked after waiter
propagation was introduced.

## 16. Worker utilization

No primary measured median reported a zero-Task worker. Worker-distribution
changes were mixed; the candidate did not show persistent worker starvation.

## 17. Environmental variability

Per-configuration throughput CVs ranged widely, including 22.97% for the
two-worker no-op candidate. Results are observed medians, not statistical
significance claims.

## 18. Threats to validity

Profiling overhead, Windows scheduling, frequency scaling, thermal state,
background work, short callbacks, and only seven iterations limit causal and
cross-machine conclusions. The strict-policy stalled smoke run was invalid and
is not included as performance evidence.

## 19. Acceptance criteria evaluation

| Criterion | Result |
|---|---|
| Exact tests and benchmark correctness | Pass |
| Public API/lifecycle/shutdown preserved | Pass |
| Signals materially decrease | Pass in most cases |
| At least one relevant no-op gain >=5% | Pass |
| Majority no-op regressions within 5% | Pass |
| Majority light regressions within 5% | Pass |
| Medium CPU regression within 5% | **Fail: -5.79%** |
| Million-Task stress correctness | Pass |
| No deadlock/starvation in final policy | Pass |

## 20. Final decision

**OPTIMIZATION REJECTED.** Acceptance requires every criterion, and the medium
CPU safeguard failed.

## 21. Rollback status

The internal experimental implementation remains reproducible, but
`CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING` defaults to `OFF`, preserving
the previous unconditional production policy.

## 22. Reproduction commands

Configure profiling control with transition signaling `OFF` and candidate with
it `ON`, build both, run their self-tests, then invoke the benchmark command
recorded in `results/optimization/environment.md`. Raw paired and companion
CSVs are retained under `results/optimization/raw`.

## 23. Exact next recommendation

Do not adopt or tune this signaling policy. The next checkpoint should first
repeat the medium safeguard and four-worker no-op A/B comparison on a quieter,
controlled environment to determine whether the conflicting medians reproduce;
it should not begin another optimization until that evidence is resolved.
