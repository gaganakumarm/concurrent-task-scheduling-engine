# Checkpoint 5.4 environment

- Baseline: `7e7e962f` (`main`, equal to `origin/main`)
- OS: Windows NT 10.0.26200.0, AMD64
- CPU: Intel Core i5-1155G7
- Logical processors: 8
- Compiler: GCC 16.1.0
- CMake: 4.4
- Generator/build type: MinGW Makefiles, Release
- Power plan: Balanced
- Primary run: 100,000 Tasks, two warm-ups, seven measured iterations
- Stress run: 1,000,000 Tasks, two warm-ups, one measured iteration, repeated three times per variant
- Accounting: exact validated standard accounting

Command format:

`concurrent_scheduler_benchmarks --scenario throughput --workers N
--producers N --capacity N --tasks N --warmup 2 --iterations 7
--callback-profile PROFILE --mode validated --accounting standard --output CSV`

Background load, effective clock, temperature, antivirus activity, and Windows
scheduling were not controlled. Control/candidate order was alternated.
