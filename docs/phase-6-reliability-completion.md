# Phase 6 reliability completion

## Overview

Checkpoint 6.5 completes the planned Phase 6 validation layer by exercising
remaining deterministic lifecycle and cleanup paths. It changes no scheduler,
queue, worker, public API, fault model, or production execution behavior.

## Remaining validated failures

The fault-enabled suite now validates:

- worker-creation failure followed by explicit join cleanup;
- worker-array allocation failure at either allocation;
- retrying start after allocation failure and fault-plan reset;
- duplicate initialization rejection;
- shutdown and join before start;
- double start rejection;
- destroy while running rejection;
- idempotent shutdown, join, and destroy;
- join after destroy;
- wrapper reuse after a completed or failed lifetime;
- safe cleanup after injected join-result rejection.

These tests supplement the existing scheduler lifecycle matrix rather than
replacing it.

## Cleanup guarantees

Partial worker creation closes the queue and joins every created worker before
returning. A subsequent join is safe and moves the cleaned failed scheduler to
stopped state. Worker-array allocation failure frees a successful partial
allocation, retains initialized state, and permits either destroy or a
deterministic retry. Native join ownership is resolved before injected result
rejection. Every tested path finishes with zero active workers and a
destroyable wrapper.

Repeated cleanup follows the public contract: shutdown and join are idempotent
after successful completion, and destroy is idempotent after the wrapper is
cleared. No cleanup operation restarts workers or reopens submissions.

## Lifecycle validation

Invalid calls return existing status codes without mutation. In particular,
start cannot run twice, shutdown and join reject initialized state, destroy
rejects running state, and join rejects a destroyed wrapper. A reset
allocation plan permits a second start attempt only because the first
allocation failure restores the approved initialized state.

## Observability and health

After creation failure, the failed snapshot has exact created/joined counts,
zero active workers, one startup failure, and valid quiescent accounting.
Health is `FAILED` until explicit join cleanup completes; the resulting
structurally valid stopped snapshot derives `STOPPED`.

Allocation failure leaves an initialized, structurally valid, healthy
snapshot with no workers. Successful retry and cleanup produce a valid stopped
snapshot with joined equal to created. Join-result rejection continues to
produce the deterministic `STOPPED_WORKERS_NOT_JOINED` diagnostic and failed
health while retaining no live native thread.

## Fault-framework extensions

None. Existing allocation, worker-creation, and post-native-join seams are
sufficient for this checkpoint. Avoiding new seams preserves deterministic
behavior and keeps production builds unchanged.

## Unsupported scenarios

Worker readiness injection remains deferred because readiness arrival order is
scheduler-dependent. Pre-instance scheduler allocation, queue initialization,
and synchronization primitive injection would require an early global or
backend-level seam. Random faults, retries, recovery, thread restart,
watchdogs, callback timeout, cancellation, and resource corruption remain out
of scope.

## Testing

The production build retains seven CTests. Fault-enabled builds retain eight,
with the dedicated fault suite expanded from 30 to 50 checks. All new cases use
small bounded worker sets and existing lifecycle synchronization; no arbitrary
sleep or timing oracle is used.

## Production compatibility

Fault injection remains default OFF and compiled out of normal, benchmark, and
profiling builds. This checkpoint changes only fault-enabled tests and
documentation, so it adds no production data, branch, lock, atomic operation,
allocation, or runtime call.

## Future extension points

A later phase may design backend-owned deterministic initialization seams or a
stable readiness ordinal, but only with explicit ownership, cleanup, and
compile-out contracts. Phase 7 should begin with a separate architecture
decision rather than extending Phase 6 implicitly.

## Known limitations

The tests prove deterministic control-flow cleanup and accounting but do not
replace external handle, heap, or sanitizer leak tooling. Platform failure
paths that cannot be selected deterministically remain unverified.

## Checkpoint decision

Final approval depends on successful production, fault, benchmark, profiling,
documentation, and repository-hygiene validation.
