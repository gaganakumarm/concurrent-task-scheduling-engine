# Concurrent Task Scheduling Engine

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C17](https://img.shields.io/badge/C-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-%E2%89%A53.20-064F8C.svg)
![Platform: Windows](https://img.shields.io/badge/platform-Windows-0078D4.svg)
[![Release: v1.0.0](https://img.shields.io/badge/release-v1.0.0-blue.svg)](docs/releases/v1.0.0.md)

A C17 library for executing caller-owned Tasks on a fixed-size worker pool. It
provides a bounded FIFO, blocking and non-blocking submission, explicit
lifecycle control, and graceful draining during shutdown.

The project focuses on behavior that is difficult to get right in concurrent C
software: ownership, shutdown races, partial startup, worker cleanup, exact
runtime accounting, and evidence-based performance analysis. Its public API is
independent of native threading types; the supported backend uses Windows
critical sections, condition variables, and `_beginthreadex`.

## Why this project?

- Make concurrency behavior explicit and deterministic.
- Preserve accepted work across graceful shutdown.
- Separate Task ownership, queue storage, worker lifecycle, and platform code.
- Exercise synchronization and failure paths without timing-dependent tests.
- Validate performance claims with reproducible, correctness-gated benchmarks.
- Provide an installable C library with a small, documented public API.

## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Repository Structure](#repository-structure)
- [Build](#build)
- [Running Tests](#running-tests)
- [Running Benchmarks](#running-benchmarks)
- [Performance Analysis](#performance-analysis)
- [Documentation](#documentation)
- [Project Highlights](#project-highlights)
- [Design Principles](#design-principles)
- [License](#license)

## Features

- C17 implementation with compiler extensions disabled
- Caller-owned Tasks with validated states, priority metadata, and work
  accounting
- Fixed-capacity FIFO with synchronized multi-producer, multi-consumer access
- Fixed-size worker pool with readiness coordination and blocking or
  non-blocking submission
- Explicit initialize, start, shutdown, join, and destroy lifecycle
- Graceful shutdown that rejects new submissions and drains accepted Tasks
- Exact submission, queue, worker, and callback accounting with private
  snapshot validation and derived health
- Compile-time-gated deterministic fault injection and contention profiling
- Deterministic benchmark workloads with high-resolution Windows timing and
  CSV output
- Platform-neutral public headers with an isolated Windows synchronization
  backend
- Strict-warning CMake builds, seven normal CTests, installable libraries,
  public headers, and package metadata

## Architecture

```text
                         caller-owned Task objects
                                  │
Task producers                    │
      │                           │
      ▼                           │
Scheduler submission gate        │
      │                           │
      ▼                           │
ConcurrentTaskQueue ──────────────┘
  ├── mutex
  ├── not-empty condition
  ├── not-full condition
  └── graceful shutdown state
      │
      ▼
Bounded TaskQueue
  └── circular FIFO of non-owning Task pointers
      │
      ▼
Fixed worker pool
      │
      ▼
Caller callbacks
      │
      ▼
Task outcomes and private runtime accounting
      │
      ▼
Snapshot validation and derived health
```

The scheduler owns its queue, worker resources, synchronization objects, and
private accounting. It borrows every Task, callback, and callback context.
Callbacks execute without scheduler lifecycle or queue locks and may run
concurrently.

```text
initialize → start → submit → shutdown → join → destroy
```

Shutdown closes submission, wakes blocked operations, and preserves accepted
Tasks for FIFO draining. Successful join guarantees that no callback remains
active. Task priority is metadata and does not change FIFO ordering.

For more information, see [Architecture](docs/architecture.md) and
[Threading architecture](docs/threading-architecture.md).

## Repository Structure

| Path | Purpose |
|---|---|
| `include/concurrent_scheduler/` | Installed public C API |
| `src/` | Task, queue, scheduler, validation, and version implementation |
| `src/internal/` | Private observability, profiling, and fault interfaces |
| `src/platform/` | Internal synchronization abstraction |
| `src/platform/windows/` | Windows synchronization backend |
| `tests/` | Deterministic unit, concurrency, lifecycle, and failure tests |
| `benchmarks/` | Benchmark executable, Windows timer, and methodology |
| `results/` | Committed baseline, profiling, and experiment evidence |
| `scripts/` | Build validation and report-asset generation |
| `tools/` | General benchmark CSV analysis |
| `docs/` | Architecture, performance, reliability, and release guides |
| `cmake/` | Installed-package configuration template |

Only headers under `include/concurrent_scheduler/` are public. Files under
`src/internal/` and `src/platform/` are private and are not installed.

## Build

### Requirements

- Windows 11 or a compatible Windows environment
- CMake 3.20 or newer
- C17 compiler
- MSYS2 UCRT64/MinGW Makefiles for the documented build

Linux and macOS are not supported because the repository provides only a
Windows synchronization backend.

### Release

```powershell
cmake -S . -B build-release -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Run the demonstration executable:

```powershell
.\build-release\concurrent-task-scheduling-engine.exe
```

Expected output includes:

```text
Version: 1.0.0
Status: initialized
```

### Debug

```powershell
cmake -S . -B build-debug -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

### Installation

```powershell
cmake --install build-release --prefix build-install
```

The installation contains the static scheduler and synchronization libraries,
public headers, MIT License, exported targets, and CMake package files.

An installed consumer can use:

```cmake
find_package(ConcurrentScheduler 1.0 REQUIRED)

target_link_libraries(
    my_program
    PRIVATE
        ConcurrentScheduler::concurrent_scheduler
)
```

The umbrella header exposes the complete API:

```c
#include <concurrent_scheduler/concurrent_scheduler.h>
```

Granular Task, queue, and scheduler headers remain available for narrower
includes.

## Running Tests

Run the seven normal CTests:

```powershell
ctest --test-dir build-release --output-on-failure
```

| Test | Coverage |
|---|---|
| `concurrent_scheduler.foundation` | Runtime version and link integration |
| `concurrent_scheduler.task_domain` | Task validation, transitions, and work accounting |
| `concurrent_scheduler.task_queue` | FIFO behavior, ownership, failures, and wraparound |
| `concurrent_scheduler.synchronization_backend` | Mutex, condition, broadcast, and thread operations |
| `concurrent_scheduler.concurrent_task_queue` | Multi-producer/consumer coordination, blocking, and shutdown |
| `concurrent_scheduler.scheduler` | Worker lifecycle, submission, callbacks, draining, and cleanup |
| `concurrent_scheduler.observability` | Snapshots, overflow, invariants, diagnostics, and health |

Concurrency tests use mutexes, condition variables, protected predicates, and
thread joins rather than arbitrary sleeps as correctness oracles.

### Fault-injection build

```powershell
cmake -S . -B build-fault -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION=ON
cmake --build build-fault
ctest --test-dir build-fault --output-on-failure
```

This configuration adds `concurrent_scheduler.fault_injection`. It
deterministically exercises worker-array allocation failure, partial worker
creation, join-result rejection, cleanup, retry, counter overflow, and wrapper
reuse. Fault injection is private, instance-scoped, disabled by default, and
compiled out of normal builds.

## Running Benchmarks

Run the Release benchmark self-test:

```powershell
.\build-release\concurrent_scheduler_benchmarks.exe --self-test
```

Example validated throughput run:

```powershell
.\build-release\concurrent_scheduler_benchmarks.exe `
  --scenario throughput `
  --workers 4 `
  --producers 4 `
  --capacity 64 `
  --tasks 100000 `
  --warmup 2 `
  --iterations 10 `
  --callback-profile noop `
  --mode validated `
  --output build-release\benchmark.csv
```

The harness provides no-op, deterministic light and medium CPU, and controlled
blocking callback profiles. It writes one CSV row per measured iteration;
warm-ups are discarded.

CSV output includes configuration, Task accounting, throughput, submission and
end-to-end latency, lifecycle duration, and correctness. Applicable builds add
runtime health or private contention fields. Validated mode rejects duplicate
or unknown Tasks, accounting mismatches, lifecycle failures, and callbacks
observed after join.

For the complete CLI, schema, timing boundaries, and reproducibility guidance,
see [Benchmark methodology](benchmarks/README.md).

## Performance Analysis

```text
results/
├── baseline/      Ordinary Release measurements
├── profiling/     Queue, worker, producer, and accounting diagnostics
└── optimization/  Paired control and experimental measurements
```

The transition-aware signaling experiment reduced signal counts but failed the
complete workload safeguard and remains disabled. Published conclusions and
measurement limitations are summarized in
[Performance evaluation](docs/performance-evaluation-summary.md).

Analyze compatible benchmark CSVs with:

```powershell
python tools\analyze_benchmark.py `
  build-release\benchmark.csv `
  --output build-release\benchmark-summary.csv
```

The tool rejects failed correctness, overflow, incomplete validation, invariant
violations, and non-stopped terminal health. It reports median, range, mean,
population standard deviation, coefficient of variation, p50/p95 duration, and
queue high-water mark.

- `scripts/` validates fixed evidence sets, regenerates report assets, and
  rebuilds known configurations.
- `tools/` analyzes caller-selected compatible CSV files.

Measurements describe the documented Windows host and configurations; they are
not cross-machine performance guarantees.

## Documentation

Public API contracts are documented directly in the installed headers under
`include/concurrent_scheduler/`.

- [Architecture](docs/architecture.md)
- [Threading architecture](docs/threading-architecture.md)
- [Benchmark plan](docs/benchmark-plan.md)
- [Performance evaluation](docs/performance-evaluation-summary.md)
- [Scheduler reliability](docs/reliability-report.md)
- [Robustness and static analysis](docs/robustness-and-static-analysis.md)
- [v1.0.0 release notes](docs/releases/v1.0.0.md)

Performance and contention charts are stored under
`docs/images/performance/` and `docs/images/contention-profiling/`.

## Project Highlights

- Layered Task, FIFO, synchronized queue, scheduler, and platform-backend
  implementation
- Namespaced public headers with opaque scheduler and native synchronization
  boundaries
- Fixed worker pool with readiness coordination and graceful queue draining
- Submission gate that coordinates concurrent producers with shutdown
- Callbacks executed outside scheduler locks
- Private snapshots with exact domain accounting and quiescent invariants
- Saturating counters with explicit overflow reporting
- Deterministic synchronization and failure-path tests
- Correctness-gated benchmarks with committed raw evidence
- Strict-warning C17 build with installation and package-consumer support

## Design Principles

- **Explicit ownership:** callers retain Tasks and callback data; containers own
  only their internal resources.
- **Deterministic lifecycle:** startup, shutdown, joining, and destruction have
  distinct contracts.
- **Minimal public API:** platform and diagnostic details remain private.
- **Synchronization by predicate:** blocking operations recheck protected state
  rather than relying on wake-up order.
- **Reproducible evidence:** tests and benchmarks validate correctness before
  reporting results.

## License

This project is licensed under the [MIT License](LICENSE).
