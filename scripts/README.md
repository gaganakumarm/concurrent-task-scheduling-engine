# Scripts

These documentation and performance utilities use only the Python standard
library or PowerShell plus the repository's existing build tools.

| Script | Responsibility |
|---|---|
| `generate_baseline_report_assets.py` | Validate committed baseline CSVs, regenerate the baseline summary, and write performance SVGs |
| `generate_profiling_report_assets.py` | Validate profiling and companion CSVs, regenerate the profiling summary, and write contention SVGs |
| `generate_optimization_report_assets.py` | Validate control/candidate evidence, regenerate the optimization summary, and write experiment SVGs |
| `reproduce_performance_evaluation.ps1` | Build and verify normal, benchmark, profiling, and retained-candidate configurations without rerunning historical matrices |
| `validate_architecture_svgs.py` | Validate the standalone monochrome software-architecture SVG, including XML metadata, colors, and embedded-resource restrictions |

Historical environment records and raw CSVs remain the measurement source of
truth. Regenerated summaries or charts should be reviewed together with those
inputs rather than interpreted as new benchmark runs.
