# ADR-006: Runtime observability and reliability

## Status

Accepted for Phase 6 architecture; implementation deferred to later
checkpoints.

## Context

The scheduler has validated lifecycle, shutdown, callback-result, and ownership
behavior, but its private state and failure counters cannot be safely observed
during operation. Adding a public snapshot immediately would freeze an
untested ABI and risk new lock ordering. Reliability failure paths also need
deterministic testing without affecting normal production builds.

## Decision

- Develop observability internal-first behind a private interface.
- Use a hybrid domain-consistent snapshot: copy each existing lock domain
  separately and never nest scheduler, worker, and queue locks.
- Treat the composite as a bounded observation window, not an atomic instant.
- Derive health from lifecycle, worker, failure, and invariant data.
- Keep counters lifetime-scoped and non-resettable while running.
- Keep future deterministic per-instance fault injection behind
  `CONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION`, default `OFF`.
- Put stress/fault diagnostics in a separate reliability test executable.
- Keep library operation quiet and preserve the existing public API.
- Make no production semantic change in Checkpoint 6.1.

## Considered alternatives

1. **One fully consistent snapshot under nested locks.** Rejected because it
   increases deadlock and task-blocking risk and requires a new lock order.
2. **Lock-free atomic counters.** Deferred because compiler/platform support,
   memory ordering, composite consistency, and overhead have not been proven.
3. **Post-join-only profiling snapshot.** Insufficient for runtime health and
   shutdown diagnosis.
4. **Public snapshot immediately.** Rejected until the schema and semantics
   survive internal tests.
5. **Mandatory logging.** Rejected because libraries should not emit
   unsolicited output or require files/frameworks.

## Consequences

Snapshots will be data-race safe and low-risk but may combine values copied at
slightly different instants. Live cross-domain invariants must acknowledge that
skew. On-demand reads can briefly block one domain. Internal schema evolution
remains possible. Reliability tests gain deterministic, structured failure
evidence without exposing native resources.

Implementation clarification from Checkpoint 6.2: lifecycle and structural
worker fields use existing mutexes. Hot callback outcomes use cache-line-aligned
per-worker C17 atomic slots with one writer and snapshot readers. The build
requires lock-free 64-bit atomics and verifies this at compile time. This avoids
both a new lock order and shared callback-accounting cache lines.

## Compatibility

No public header, callback signature, Task representation, lifecycle rule,
queue semantic, shutdown behavior, ownership contract, or default Phase 5
option changes. A future public diagnostic API requires a separate decision.

## Performance implications

Implementation should reuse existing locks, avoid timestamps, allocation, and
I/O on Task paths, and measure overhead against Phase 5. If mandatory
accounting materially regresses ordinary performance, reduce the field set or
gate diagnostics rather than accepting an unmeasured cost.

## Testing implications

Tests must cover live and quiescent snapshots, bounds, overflow behavior,
health derivation, lock-order safety, deterministic injection triggers,
partial cleanup, exact Task balances, timeouts, and reproducible stress seeds.
The normal build must prove fault injection inactive.

## Rollback strategy

Checkpoint 6.1 is documentation-only and can be reverted without runtime
effect. Later counters, snapshots, or injection hooks must be removable as one
private layer. Any semantic regression, deadlock, data race, false invariant,
or unacceptable overhead requires rollback to the previously validated
internal state.
