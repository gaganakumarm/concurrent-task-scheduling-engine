# Contention Profiling and Bottleneck Analysis

## 1. Executive conclusion

Direct, default-off instrumentation confirms substantial queue coordination in
the measured minimal-work scheduler. It does not prove a universal root cause.
The strongest evidence is capacity-one behavior, worker-count scaling, direct
wait entry, lock-acquisition duration, signaling, occupancy, and exact
standard/partitioned comparisons.

## 2. Scope

This verification step measures; it does not optimize. The public API, queue semantics,
lock order, shutdown behavior, ownership model, and six-test topology remain
unchanged.

## 3. Baseline

The baseline is `acbe5481264beee876de734e162567a57b6901b2` on `main`.

## 4. Build gating

`CONCURRENT_SCHEDULER_ENABLE_PROFILING` defaults to `OFF`. Profiling storage and
execution are compiled only when it is enabled.

## 5. Private observation boundary

The internal header is unavailable without the profiling macro. Snapshots are
read only after producers and workers are joined; nothing is publicly exposed.

## 6. Timer and failure policy

Durations use QueryPerformanceCounter with checked differences, conversion,
and accumulation. A frequency, timing, or overflow failure invalidates the run.

## 7. Inferred contention

A lock acquisition is classified as inferred contention only when duration is
greater than one microsecond. This avoids calling every timer delta contention.

## 8. Wait semantics

Wait counts are actual entries into condition-variable wait. A wake-up followed
by a still-false predicate is called a predicate-false wake-up, not necessarily
a spurious wake-up.

## 9. Occupancy semantics

Occupancy is sampled after successful enqueue and dequeue mutations. The mean
is event-sampled, not time-weighted.

## 10. Evidence set

Sixteen profiled configurations each contain five measured iterations plus
worker and producer companions. Two ordinary-build controls contain five
iterations. Every profiled iteration passed exact correctness.

## 11. No-op worker scaling

Median throughput for 1/2/4/8 workers was 1.080M, 1.038M, 1.026M, and 0.865M
Tasks/s. More workers did not improve this minimal callback.

## 12. Light worker scaling

Median throughput for 1/2/4/8 workers was 0.869M, 0.839M, 1.028M, and 0.839M
Tasks/s. The peak at four workers was not sustained at eight.

## 13. Full-queue waiting

For standard no-op at four workers, the median run entered enqueue waiting
10,929 times: 10.929% of 100,000 enqueue lock attempts. At capacity one it
entered 46,550 times, or 46.550%.

## 14. Empty-queue waiting

For standard no-op at four workers, the median run entered dequeue waiting
8,666 times, or 8.666% of roughly 100,004 dequeue attempts. At eight workers
the rate rose to 40.590%.

## 15. Producer lock acquisition

Median cumulative enqueue-lock duration for standard no-op at four workers was
about 221.0 ms (2.267 concurrent-thread-seconds per wall second). Across
one/two/four producers it increased from about 42.6 ms to 112.8 ms to 221.0 ms.

## 16. Worker lock acquisition

Median cumulative dequeue-lock duration for no-op 1/2/4/8-worker runs was
about 33.6, 119.4, 210.3, and 357.6 ms. It increased with worker count.

## 17. Capacity-one diagnosis

Capacity one delivered 0.201M Tasks/s versus 1.026M at capacity 64. Its
46.550% enqueue-wait and 43.472% dequeue-wait rates, plus occupancy fixed
between zero and one, directly support coordination/backpressure dominance.
The data do not isolate full-queue waiting as the sole cost.

## 18. Capacity and occupancy

Event-sampled mean occupancy for capacities 1/16/64/256 was 0.50, 7.24, 37.67,
and 126.85. Capacity 256 therefore increased observed event-sampled occupancy.

## 19. Worker distribution

No measured summary had a zero-Task worker. Median Task-distribution CV for
no-op 1/2/4/8 workers was 0, 0.049, 0.045, and 0.153: all workers participated,
but imbalance increased at eight workers.

## 20. Predicate-false wake-ups

Median enqueue/dequeue predicate-false wake-ups were 1,756/1,808 for the
four-worker no-op run, 7,063/4,846 at capacity one, and 467/9,957 with eight
workers. These are competitive or other predicate-false returns, not proof of
OS-spurious wake-ups.

## 21. Callback timing

In the four-worker standard no-op run, median cumulative compute/accounting
times were about 4.49/29.70 ms: approximately 13.1% compute and 86.9%
accounting within those two callback components. For light CPU they were about
51.8/33.6 ms: approximately 60.7% and 39.3%.

## 22. Medium callback

Medium CPU throughput was 0.177M Tasks/s. Cumulative computation was roughly
3.03 worker-seconds per wall-second, consistent with computation dominating
the measured callback components.

## 23. Exact accounting diagnostic

Partitioned accounting retains atomic per-Task execution counts, unknown and
duplicate detection, per-worker reduction, rejected-Task checks, and a
post-join deterministic checksum.

## 24. Standard versus partitioned

No-op median throughput was 1.026M standard versus 1.018M partitioned
(-0.8%). Light was 1.028M standard versus 1.026M partitioned (-0.2%).
Partitioning did not improve these runs.

## 25. Accounting hypothesis

Because exact partitioning did not restore scaling, shared benchmark accounting
does not explain most negative scaling on this matrix. Accounting is still a
measurable part of minimal callback time.

## 26. Scheduler-contention inference

Direct wait-entry, lock-duration, occupancy, signaling, and worker evidence
supports queue coordination as a bottleneck for minimal work, especially at
capacity one and eight workers. Concurrent timing totals overlap and cannot be
summed into a wall-time budget.

## 27. Profiling overhead

Ordinary/profile median durations were 78.398/97.491 ms for no-op and
85.917/97.283 ms for light, overheads of 24.35% and 13.23%. Because no-op
exceeds 15%, fine-grained shares are diagnostic and must be interpreted
cautiously. Environmental variation is also uncontrolled.

## 28. Confirmed measurements

Confirmed: actual condition waits and returns, lock-acquisition durations,
signals and broadcasts, predicate-false wake-ups, mutation-sampled occupancy,
stable per-worker execution, producer latency, callback component timing, and
exact accounting-mode outcomes.

## 29. Supported inferences

Supported: capacity-one slowdown is dominated by queue coordination broadly;
worker-side coordination grows with workers; producer lock duration grows with
producers; benchmark accounting is not the principal cause of negative scaling.

## 30. Rejected hypotheses

Rejected for this evidence set: that partitioned exact accounting improves
no-op scaling, that workers are starved to zero Tasks, or that a larger queue
necessarily improves throughput beyond capacity 64.

## 31. Unresolved questions

Unresolved: kernel versus user-space blocking cost, cache-line movement,
scheduler preemption, context-switch counts, topology effects, and behavior on
other CPUs or longer production callbacks.

## 32. Root-cause confidence

Confidence is moderate for queue coordination being a material bottleneck in
the measured minimal-work cases, high for the individual counters as measured,
and low for extrapolation beyond this machine and matrix.

## 33. Exact first Verification step 5.4 experiment

Test exactly one optimization: replace per-dequeue `not_full` signaling with a
semantics-preserving batched producer-notification experiment, while retaining
the same bounded queue behavior and rerunning capacity 1/16/64/256 plus all
correctness tests. This is a proposal only; it is not implemented here.

Raw evidence is under `results/profiling/raw`, the machine-readable aggregation
is `results/profiling/profiling-summary.csv`, and the twelve generated SVGs are
under `docs/images/contention-profiling`.
