# Concurrent Task Scheduling Engine
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C17](https://img.shields.io/badge/C-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-%E2%89%A53.20-064F8C.svg)
![Platform: Windows](https://img.shields.io/badge/platform-Windows-0078D4.svg)
[![Release: v1.0.0](https://img.shields.io/badge/release-v1.0.0-blue.svg)](docs/releases/v1.0.0.md)

A C17 library for executing caller-owned tasks on a fixed-size worker pool. It
provides a bounded FIFO queue, blocking and non-blocking submission, explicit
lifecycle control, graceful shutdown, and deterministic runtime accounting.

The public API is independent of native threading types. The current
synchronization backend targets Windows and uses critical sections, condition
variables, and `_beginthreadex`.

## Why this project?

- Make concurrent ownership and lifecycle rules explicit.
- Preserve accepted work during graceful shutdown.
- Separate task modeling, queue storage, scheduling, and platform code.
- Exercise synchronization and failure paths deterministically.
- Support reproducible, correctness-gated performance measurements.
- Provide an installable C library with a compact public API.

## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Repository Structure](#repository-structure)
- [Build](#build)
- [Running Tests](#running-tests)
- [Running Benchmarks](#running-benchmarks)
- [Performance Analysis](#performance-analysis)
- [Documentation](#documentation)
- [Design Principles](#design-principles)
- [License](#license)

## Features

- C17 implementation with warning-enabled GCC, Clang, and MSVC builds
- Caller-owned tasks with validated priority, lifecycle, and work accounting
- Fixed-capacity FIFO queue with non-owning task pointers
- Thread-safe bounded queue with blocking and non-blocking operations
- Fixed-size worker pool with concurrent producer support
- Explicit initialization, startup, shutdown, join, and destruction contracts
- Graceful queue draining and deterministic worker cleanup
- Private runtime snapshots, health derivation, and invariant validation
- Compile-time deterministic fault injection and contention profiling
- Correctness-gated benchmark harness with CSV output
- Modern CMake build, installation, and package export support
- Deterministic unit, synchronization, scheduler, and fault-injection tests

Task priorities are metadata; scheduling remains FIFO. Scheduler-level
cancellation is not part of the public API.

## Architecture

```text
Caller-owned Task objects
          |
          v
  Producer thread(s)
          |
          v
       Scheduler
          |
          v
Bounded synchronized FIFO
          |
          v
   Fixed worker pool
          |
          v
     User callback
          |
          v
Private accounting and validation
```

The scheduler does not own submitted tasks or callback context. Workers invoke
callbacks without holding queue or scheduler lifecycle locks. Platform
synchronization is isolated behind an internal interface.

For details, see [Architecture](docs/architecture.md) and
[Threading Architecture](docs/threading-architecture.md).

## Repository Structure

| Path | Purpose |
|---|---|
| `include/concurrent_scheduler/` | Installed public API |
| `src/` | Core task, queue, scheduler, and validation implementation |
| `src/internal/` | Private observability, profiling, and fault-injection interfaces |
| `src/platform/windows/` | Windows synchronization backend |
| `tests/` | Deterministic test executables |
| `benchmarks/` | Benchmark harness and Windows timer |
| `results/` | Recorded baseline, profiling, optimization, and stability evidence |
| `scripts/` | Reproduction and report-asset generation scripts |
| `tools/` | Standalone benchmark analysis utility |
| `docs/` | Architecture, reliability, performance, and release documentation |
| `cmake/` | Installed-package configuration template |

## Build

### Requirements

- Windows or a compatible Windows environment
- CMake 3.20 or newer
- A C17 compiler
- MinGW Makefiles and GCC, or another supported Windows CMake toolchain

Linux and macOS are not supported because no synchronization backend is
available for those platforms.

### Release build with MinGW

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\concurrent-task-scheduling-engine.exe
```

### Debug build with MinGW

```powershell
cmake -S . -B build-debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
.\build-debug\concurrent-task-scheduling-engine.exe
```

### Install the library

```powershell
cmake --install build --prefix build-install
```

Include the public API through the umbrella header:

```c
#include <concurrent_scheduler/concurrent_scheduler.h>
```

The install includes the static library, public headers, MIT license, and CMake
package metadata. A CMake consumer can use:

```cmake
find_package(ConcurrentScheduler 1.0 REQUIRED)
target_link_libraries(
    my_program
    PRIVATE
        ConcurrentScheduler::concurrent_scheduler
)
```

## Running Tests

Configure and build, then run the complete normal test suite:

```powershell
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
```

The normal Windows build registers seven CTests covering:

- Version and library initialization
- Task lifecycle and work accounting
- Non-thread-safe FIFO queue behavior
- Windows synchronization primitives
- Concurrent queue behavior and shutdown
- Scheduler lifecycle, submission, execution, and cleanup
- Runtime observability and invariant validation

Fault injection is compile-time gated and disabled by default. Run its
additional test executable from a separate Debug build:

```powershell
cmake -S . -B build-fault -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION=ON
cmake --build build-fault
ctest --test-dir build-fault --output-on-failure
```

## Running Benchmarks

Use a dedicated Release build:

```powershell
cmake -S . -B build-bench -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench
.\build-bench\concurrent_scheduler_benchmarks.exe --self-test
```

Example validated throughput run:

```powershell
.\build-bench\concurrent_scheduler_benchmarks.exe `
  --scenario throughput `
  --workers 4 `
  --producers 4 `
  --capacity 64 `
  --tasks 100000 `
  --warmup 3 `
  --iterations 10 `
  --callback-profile noop `
  --mode validated `
  --output build-bench\benchmark-results.csv
```

The harness supports deterministic no-op, light CPU, medium CPU, and controlled
blocking callbacks. Its CSV output records configuration, submission and
end-to-end latency, shutdown and join duration, throughput, and correctness
status. See [Benchmark Methodology](benchmarks/README.md) for the complete CLI,
measurement boundaries, schema, and interpretation constraints.

## Performance Analysis

Recorded measurements and environment descriptions are retained under
`results/`:

- `results/baseline/` contains the baseline matrix and raw runs.
- `results/profiling/` separates scheduler contention from benchmark-accounting
  contention.
- `results/optimization/` contains control and candidate measurements.

Generate statistical summaries from benchmark CSV files with:

```powershell
python tools\analyze_benchmark.py build-bench\benchmark-results.csv
```

Use `scripts/reproduce_performance_evaluation.ps1` to reproduce the documented
evaluation matrix. The scripts under `scripts/` automate experiments and
report assets; `tools/analyze_benchmark.py` analyzes an existing CSV without
running the scheduler.

See [Performance Evaluation Summary](docs/performance-evaluation-summary.md)
for measured conclusions and their scope.

## Documentation

- [Architecture](docs/architecture.md) — component boundaries, ownership, worker
  lifecycle, and observability design
- [Threading Architecture](docs/threading-architecture.md) — synchronization,
  concurrent operations, and shutdown behavior
- [Benchmark Plan](docs/benchmark-plan.md) — controlled variables and
  measurement protocol
- [Performance Evaluation Summary](docs/performance-evaluation-summary.md) —
  baseline, contention, optimization, scalability, and stability evidence
- [Reliability Report](docs/reliability-report.md) — runtime snapshots,
  invariants, deterministic faults, and recovery
- [Robustness and Static Analysis](docs/robustness-and-static-analysis.md) —
  compiler, static-analysis, and undefined-behavior audit
- [v1.0.0 Release Notes](docs/releases/v1.0.0.md) — stable release summary

Public API contracts are documented directly in the installed headers under
`include/concurrent_scheduler/`.

## Design Principles

- Caller ownership is explicit and never transferred implicitly.
- Lifecycle transitions and failure guarantees are deterministic.
- Public interfaces avoid exposing native synchronization types.
- Platform-specific code remains behind a private synchronization boundary.
- Profiling and fault injection are private, optional, and disabled by default.
- Benchmarks gate reported measurements on correctness checks.

## License

This project is licensed under the [MIT License](LICENSE).
