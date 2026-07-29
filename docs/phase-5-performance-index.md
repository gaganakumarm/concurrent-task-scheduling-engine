# Phase 5 performance engineering index

- [Benchmark plan and architecture](phase-5-benchmark-plan.md) — workloads,
  correctness gates, timing boundaries, CSV design, and comparison matrix.
- [Benchmark methodology](../benchmarks/README.md) — CLI, schemas, accounting,
  timing, and reproduction guidance.
- [Baseline benchmark report](phase-5-baseline-benchmark-report.md) — ordinary
  Release measurements and initial scaling observations.
- [Contention profiling report](phase-5-contention-profiling-report.md) —
  direct queue, lock, wait, occupancy, worker, and callback evidence.
- [Queue-coordination optimization report](phase-5-queue-coordination-optimization-report.md)
  — controlled signaling A/B experiment and rejection decision.
- [Phase 5 closure](phase-5-performance-engineering-closure.md) — accepted
  findings, rejected hypotheses, unresolved questions, and production status.
- [Baseline results](../results/baseline/) — environment, summary, and raw CSVs.
- [Profiling results](../results/profiling/) — environment, summary, raw and
  companion CSVs.
- [Optimization results](../results/optimization/) — paired evidence, summary,
  and charts.
- [Reproduction helper](../scripts/reproduce_phase5.ps1) — build and validation
  commands that do not modify historical results.

Run the helper from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\reproduce_phase5.ps1
```
