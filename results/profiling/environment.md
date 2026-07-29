# Contention profiling measurement environment

- Baseline commit: `acbe5481264beee876de734e162567a57b6901b2`
- OS: Windows NT 10.0.26200.0, AMD64
- CPU: Intel Core i5-1155G7, 8 logical processors
- Compiler: GCC 16.1.0
- CMake: 4.4
- Build: MinGW Makefiles, Release
- Power plan observed: Balanced
- Matrix: 100,000 Tasks, two discarded warm-ups, five measured iterations
- Date: 2026-07-29 (Asia/Calcutta)

Physical-core count, memory topology, battery state, context-switch counts,
temperature, and effective clock were not captured. Background Windows work,
frequency scaling, antivirus activity, and thermal state remain uncontrolled
sources of variation.
