# Deterministic Scheduler Fault Injection

## 1. Executive summary

Verification step 6.4 implements a private deterministic fault-injection framework
for scheduler failure-path tests. It is compiled only when
`CONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION=ON`, which is not the default.

## 2. Scope

The framework provides one-shot Nth-occurrence plans and selected scheduler
startup and teardown seams. It validates existing error propagation and
cleanup rather than introducing new recovery behavior.

## 3. Non-goals

There is no randomness, chaos mode, production control, environment-variable
configuration, retry, recovery, timeout, cancellation, queue corruption,
forced deadlock, or broad allocator interception.

## 4. Build gating

CMake option `CONCURRENT_SCHEDULER_ENABLE_FAULT_INJECTION` defaults to `OFF`.
Only enabled builds compile `scheduler_fault_injection.c`, define the feature
macro for the core, add private plan storage, compile fault branches, and
register the eighth test. Profiling and transition signaling are independent.

## 5. Private fault model

The private header defines allocation, worker-creation, worker-startup, and
worker-join identifiers. The initial framework accepts allocation,
worker-creation, and worker-join plans. Startup has a stable reserved name but
is rejected as unsupported.

## 6. Plan ownership

Each enabled scheduler implementation owns one plan. Tests may also construct
standalone plans to test triggering. There is no process-global mutable plan,
so configuration cannot leak between scheduler instances or tests.

## 7. Deterministic trigger semantics

Occurrence values are one-based. Only matching fault points advance the
counter. The selected occurrence fires once; later matching operations proceed
normally. Reconfiguration replaces the plan and resets its observation state.
Reset returns it to disabled state.

## 8. Thread-safety model

Plan state uses C17 atomics and is data-race safe. Configuration is performed
while the scheduler is initialized under the lifecycle mutex and, like other
lifecycle control, requires external lifecycle serialization. Creation,
allocation, and join observations occur in deterministic scheduler-controlled
loops rather than callback or queue hot paths.

## 9. Implemented injection points

| Point | Exact seam | Injected behavior |
|---|---|---|
| allocation | each of two worker-array allocations in `scheduler_start` | acts as a null allocation result |
| worker creation | immediately before each native create call | uses the existing create-failure branch |
| worker join | after successful native join, before result acceptance | rejects the worker result while still destroying the joined handle |

## 10. Deferred injection points

Worker startup/readiness is deferred because workers reach the readiness
barrier in scheduler-dependent order, conflicting with deterministic Nth
semantics. Scheduler-private allocation before an instance exists and queue
initialization would require a synchronized early global seam. Mutex,
condition, callback, and queue-operation corruption are intentionally absent.

## 11. Failure propagation

Allocation returns `SCHEDULER_ERROR_ALLOCATION` and restores initialized state.
Creation returns `SCHEDULER_ERROR_SYSTEM` and leaves a safely cleaned failed
state. Injected join rejection returns `SCHEDULER_ERROR_SYSTEM`; native thread
ownership has already been resolved.

## 12. Cleanup obligations

Creation failure closes the queue, joins and destroys every previously created
worker, frees worker arrays, and creates no later workers. Allocation failure
frees whichever array succeeded. Join rejection still destroys every native
thread handle and releases worker arrays. All tested objects remain safely
destroyable.

## 13. Observability integration

Fault branches enter existing failure handling. Creation increments the
existing startup-failure counter once. Join rejection increments the existing
join-failure counter once. Allocation has no corresponding snapshot counter.
No accounting value is incremented merely because a plan triggered.

## 14. Test architecture

Normal builds retain seven CTests. Enabled builds add only
`concurrent_scheduler_fault_injection_tests`, producing eight total. Tests use
small deterministic worker sets and no sleeps.

## 15. Production-disabled behavior

When the option is OFF, the private implementation source and test target are
absent, the scheduler structure has no fault plan, and all fault branches are
preprocessed away. Normal scheduler operations therefore execute no fault-plan
checks.

## 16. Performance implications

Disabled builds add no data field, branch, atomic operation, lock, or function
call. Enabled checks occur only during start allocations, worker creation, and
join. No submit, queue, callback, or worker execution hot path is changed.

## 17. Security and misuse considerations

The framework is not installed or reachable through public APIs, CLI,
environment variables, or runtime input. It cannot terminate threads, corrupt
memory, or activate in an ordinary build.

## 18. Known limitations

One plan is active per scheduler at a time. Reconfiguration requires lifecycle
serialization. The framework covers three seams, not every failure path.
Occurrence saturation disables further triggering and records overflow.

## 19. Validation results

The fault build compiles without warnings, registers eight tests, and passes
8/8. Creation faults at the first, middle, and last worker; both allocation
occurrences; safe join rejection; counter saturation; reset; repeatability;
snapshot validation; and health derivation are covered. Full normal,
benchmark, and profiling builds pass with fault injection OFF. The optional
combined profiling and fault build also passes 8/8. Symbol and generated-build
audits find no fault implementation or reference in the normal core. Final
hygiene results are recorded in the verification step report.

## 20. Decision

All functional, compatibility, compile-out, and documentation gates support
approval, subject to the final repository hygiene report.
