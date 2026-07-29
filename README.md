# Concurrent Task Scheduling Engine

A C17 bounded task scheduler for Windows with a fixed worker pool,
multi-producer submission, deterministic shutdown and join, private runtime
accounting, lifecycle validation, fault-injection testing, and reproducible
performance evidence. Version: **1.0.0 release candidate**.

## Key capabilities

- Caller-owned tasks with validated states, priorities, and work accounting
- Fixed-capacity FIFO and synchronized blocking queue
- Fixed-size worker pool with blocking and non-blocking submission
- Explicit initialize, start, shutdown, join, and destroy lifecycle
- Exact callback success/failure and worker accounting
- Private snapshots, derived health, and invariant validation
- Compile-time-gated deterministic fault injection and contention profiling
- Release benchmark with deterministic callback workloads and CSV output

## Architecture

```text
Producer(s) -> Scheduler -> Bounded synchronized FIFO -> Worker pool
                                                        -> User callback
```

Tasks and callback contexts remain caller-owned. Workers execute callbacks
without holding scheduler queue or lifecycle locks. The supported backend uses
Windows critical sections, condition variables, and `_beginthreadex`.

See [architecture.md](docs/architecture.md) and
[threading-architecture.md](docs/threading-architecture.md).

## Reliability and observability

Private runtime snapshots expose exact lifecycle-domain accounting to internal
tests without expanding the public API. Validation detects structural and
quiescent invariant violations and derives deterministic health states.
Fault-injection tests cover allocation failure, partial worker startup, join
failure, lifecycle misuse, and repeated cleanup. Fault injection is OFF by
default.

See the [Phase 6 reliability completion report](docs/phase-6-reliability-completion.md).

## Performance summary

On the documented Windows 11 system with an Intel i5-1155G7, four producers,
capacity 64, and 100,000 no-op tasks, median throughput peaked near four
workers at 1.26 million tasks/s. Medium and heavy deterministic CPU workloads
continued scaling through eight logical workers. These configuration-specific
measurements are not cross-machine guarantees. The shared bounded queue remains
the measured tiny-task bottleneck.

See the
[Phase 7.1 performance report](docs/phase-7-performance-scalability-validation.md).

## Build requirements

- Windows 11 or a compatible Windows environment
- CMake 3.20 or newer
- C17 compiler
- MinGW Makefiles and GCC were used for release validation

Linux and macOS have not been validated. C++ linkage compatibility is not
currently promised.

## Quick start

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\concurrent-task-scheduling-engine.exe
```

Expected output includes:

```text
Version: 1.0.0
Status: initialized
```

## Running tests

```powershell
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
```

The normal build registers exactly seven tests.

## Running benchmarks

```powershell
.\build\concurrent_scheduler_benchmarks.exe --self-test
.\build\concurrent_scheduler_benchmarks.exe `
  --scenario throughput --workers 4 --producers 4 --capacity 64 `
  --tasks 100000 --warmup 1 --iterations 15 `
  --callback-profile noop --output build\benchmark.csv
```

Keep generated measurements in ignored build directories. See
[benchmarks/README.md](benchmarks/README.md) for the complete CLI and schema.

## Optional build features

Profiling Release:

```powershell
cmake -S . -B build-profile -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCONCURRENT_SCHEDULER_ENABLE_PROFILING=ON
```

Fault-injection Debug:

```powershell
cmake -S . -B build-fault -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION=ON
```

Both options and the rejected transition-aware signaling experiment default to
OFF.

## Installation

```powershell
cmake --install build --prefix build-install
```

This installs the static library, public headers, and CMake package metadata.
Internal headers, tests, benchmarks, profiling controls, and fault controls are
not installed.

An installed CMake consumer can use:

```cmake
find_package(ConcurrentScheduler 1.0 REQUIRED)
target_link_libraries(my_program PRIVATE
    ConcurrentScheduler::concurrent_scheduler)
```

## Minimal usage

```c
#include <concurrent_scheduler/concurrent_scheduler.h>
#include <stdio.h>

int main(void)
{
    printf("%s\n", concurrent_scheduler_version());
    return 0;
}
```

See the public headers for scheduler ownership and lifecycle contracts.

## Project structure

```text
include/concurrent_scheduler/  Public C API
src/                           Core and Windows backend
tests/                         Deterministic test executables
benchmarks/                    Benchmark harness
tools/ and scripts/            Analysis utilities
docs/                          Architecture and validation evidence
```

## Documentation

- [Worker-pool architecture](docs/phase-4-worker-pool-architecture.md)
- [Phase 5 performance index](docs/phase-5-performance-index.md)
- [Phase 6 reliability architecture](docs/phase-6-reliability-observability-architecture.md)
- [Phase 7.1 performance validation](docs/phase-7-performance-scalability-validation.md)
- [Phase 7.2 robustness audit](docs/phase-7-robustness-static-analysis.md)
- [v1.0.0 release notes](docs/releases/v1.0.0.md)

## Known limitations

- Windows is the only validated platform.
- The scheduler uses a shared bounded FIFO; there is no work stealing,
  priority scheduling, dynamic scaling, persistence, or process isolation.
- There are no real-time guarantees.
- Callbacks own their memory safety, synchronization, and crash behavior.
- ASan, UBSan, and TSan runtimes were unavailable in the validated MinGW
  installation; no dynamic-sanitizer coverage is claimed.
- Fault injection and profiling are private compile-time diagnostics.

## Version

The release candidate version is **1.0.0**. CMake `project(VERSION ...)` is the
canonical source used by runtime version reporting, tests, and benchmark
metadata. The recommended final tag is `v1.0.0`; it has not been created.

## License

This project is licensed under the [MIT License](LICENSE).
