# Changelog

## [1.0.0] - 2026-07-29

### Added

- C17 task model, bounded FIFO, synchronized queue, fixed worker pool, and
  explicit scheduler lifecycle.
- Blocking and non-blocking multi-producer submission with caller-owned tasks.
- Installable static library, public headers, and CMake package metadata.

### Reliability

- Exact callback, submission, queue, and worker accounting.
- Private runtime snapshots, lifecycle validation, derived health, and
  deterministic diagnostic formatting.
- Compile-time deterministic fault injection covering partial initialization
  and cleanup failures.

### Performance

- Reproducible Release benchmark with deterministic no-op and CPU workloads.
- Worker, producer, queue-capacity, sustained-load, and contention evidence.
- Profiling remains compile-time gated and OFF by default.

### Testing

- Seven normal CTests and an eighth fault-injection test when enabled.
- Normal, profiling, fault-enabled, and combined build configurations.
- GCC strict-warning and static-analysis audit.

### Documentation

- Architecture, performance, reliability, robustness, release, installation,
  and external-consumer documentation.
- MIT License.

### Known limitations

- Only Windows 11 with MinGW has been validated.
- No real-time guarantees, work stealing, dynamic scaling, persistence, or
  callback crash containment.
- ASan, UBSan, and TSan runtimes were unavailable in the validated toolchain.

The historical `v0.5-performance-engineering` tag records an engineering
milestone rather than this first complete public release.
