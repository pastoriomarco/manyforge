# Controlled Stop — Internals Reference

> **Current `dev` status:** I8/P5 is implemented in the maintained native
> product. The addressed `ResourceAddressedExecutionKernel` is its sole Stop
> owner. The ROS-free contracts and ros2_control adapter build surface are
> current, but Isaac, ros2_control product launch, and real-robot operation
> still require their I9/I10 gates. Historical adapter details below are kept
> only where they remain algorithm or transport qualification input.

Reference companion to [`operations/STOP_PROFILES.md`](../operations/STOP_PROFILES.md).
This document describes the current I8 owner and the retained transport
qualification surface. It is intentionally code-facing: future maintainers should
be able to verify the stop behavior from the files and symbols named here without
reconstructing the design history.

Current source and generated contracts win if this reference drifts.

## 1. Architecture

ManyForge uses one addressed owner and prepared participant-local realization:

1. Deployment/CCLC verifies the base Stop plan and exact runtime readiness.
1. Kernel Prepare maps the typed source-joint profile into one immutable
   participant Stop invocation plan before physical dispatch.
1. Public Stop/Full Stop is resolved and captured by
   `ResourceAddressedExecutionKernel`; the emergency reserve and resource-slot
   fence commit recognition without waiting for storage or backend I/O.
1. A fixed `BoundedStopControlExecutor` progresses the transaction. Independent
   `BoundedStopStageAttemptExecutor` lanes invoke the exact
   `IControlDomainExecutionPort::TranslateStopStage` contract with the remaining
   phase budget, so one bounded provider lane cannot block another participant
   or the watchdog.
1. The pure terminal reducer consumes immutable stage/evidence history and
   emits one certificate plus any fault latch. Journal projection and
   post-terminal surveillance occur after containment and cannot rewrite that
   certificate.
1. Stock `joint_trajectory_controller` may execute a prepared replacement
   trajectory in a later ros2_control product; the maintained native backend
   realizes the same selected contract directly.

The stop math remains in `manyforge_core/src/soft_on_path_stop_planner.cpp`.
Native realization is in `manyforge_roboplan/src/roboplan_execution_backend.cpp`;
the generic ros2_control implementation is in
`manyforge_ros2_control_execution/src/ros2_control_execution_backend.cpp` but is
not yet a qualified product lane.

The retired controller-native JTC plugin is not the target architecture. It is
preserved historically on the `jtc_plugin` branch, but `main` uses stock JTC plus
backend-authored replacement goals.

## 2. Stop Catalog

| Stop role                   | Mechanism                                                                                                                                                                                                                                                                       | Intended use                                                                                                                          |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `soft_on_path`              | Backend computes an on-path replacement trajectory from the trip state and active accepted path. Stock JTC executes it as a preempting FJT goal; native replays it directly.                                                                                                    | Default operator stop.                                                                                                                |
| `spring_back_hold`          | Backend computes a two-phase on-path replacement: forward decel over half the requested duration, then rest-to-rest return along the same accepted path to the trip cursor over the remaining half. Stock JTC executes it as a preempting FJT goal; native replays it directly. | Operator-selected visible spring-back behavior. It must not be implemented as cancel-first.                                           |
| `hard_deceleration`         | JTC cancel path when `decelerate_on_cancel` is configured and proven.                                                                                                                                                                                                           | Preferred hard-stop floor where available.                                                                                            |
| `immediate` hard-stop floor | Unprofiled cancel/hold floor: ros2_control uses plain JTC cancel/hold; native freezes the authored replay immediately.                                                                                                                                                          | Last-resort physical stop when controlled replacement cannot be trusted. Not operator-selectable as a normal controlled-stop profile. |
| Natural completion          | Let the already accepted trajectory finish when it will reach rest sooner than the requested stop window and policy allows it.                                                                                                                                                  | Short-path case only, bounded by the effective stop deadline. Enabled by default; precision-contact lanes must opt out explicitly.    |

`soft_on_path` and `spring_back_hold` are replacement/preemption profiles. They
must be sent before JTC enters cancel hold or `decelerate_on_cancel`, otherwise
the controller can introduce exactly the bump these profiles are meant to avoid.
`hard_deceleration` and the immediate floor are a different category from
replacement profiles. `hard_deceleration` is a profiled JTC
`decelerate_on_cancel` capability; the immediate floor is unprofiled and
lane-specific (`jtc_cancel_unprofiled_hold` on ros2_control,
`native_immediate_hold` on native).

`spring_back_hold` is intentionally stricter than a plain `soft_on_path` stop:
the forward deceleration gets only half the requested duration, then the robot
returns on the same path. It is a visible return-to-trip-point behavior, not a
guaranteed feasibility fallback for acceleration/jerk-limited soft stops. If
either phase violates the configured limits, the planner fails closed and the
kernel/backend hard-stop floor handles the event.

No current profile contains `allowed_fallback_modes`. `spring_back_hold` is
selected explicitly, or is the sole profile-authorized recoverable
`soft_on_path` substitution when the retained qualification permits it. That
substitution is part of one nominal provider contract, not Kernel escalation.
The 1-8-stage cascade, software-hard floor, and optional commissioned
safety-system request come only from the verified base plan.

Hard-stop floors are only issued while ManyForge has reason to believe a
controller goal may still be active or untrusted: refused controlled stop,
unconfirmed stop, deadline watchdog, stop-path collision, or explicit hard-stop
disposition. If a controlled stop was accepted and backend-confirmed but
measured velocity still fails to settle within the effective stop deadline, ManyForge does
not issue another JTC cancel. At that point there is no known active trajectory
to cancel; the condition means the robot/controller is not following the
confirmed command, so ManyForge records `stop_not_settled_measured_velocity` and
holds `INTERVENTION_REQUIRED` for operator/robot-safety recovery.

Stop-fault recovery is revision-fenced against the exact owner-issued
`StopFaultRecoveryOfferDTO`. Fresh settlement, configuration, external
reconciliation, reset, and orphan facts are evaluated privately by the Kernel.
The cleared latch and resulting `StopStatusDTO` are persisted before admission
can reopen. Recovery never resumes work and alarm acknowledgement never clears
the latch. Reset is a separate asynchronous operation control; it joins an
addressed containment certificate when needed and uses only the no-Stop local
retirement seam afterward.

## 2.1 Lane Matrix

| Concern                   | Shared kernel/planner                                                                                                                                                    | ros2_control lane                                                                                  | Native lane                                                                               |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| Stop policy               | Validates profiles, gates unsupported configured modes, decides fallback/escalation, records incidents.                                                                  | Same kernel policy.                                                                                | Same kernel policy.                                                                       |
| Trajectory math           | `manyforge_core` generates `soft_on_path` and two-phase `spring_back_hold`.                                                                                              | Dispatches the authored trajectory through stock JTC `FollowJointTrajectory` preemption.           | Replays the same authored waypoints directly.                                             |
| `soft_on_path`            | Exact-time command-frame lead-in + retimer/ALI, limit checked at trip time.                                                                                              | JTC executes the replacement; backend monitors action result, path deviation, and measured settle. | Native backend publishes the authored stop trajectory to the visualizer/replay clock.     |
| `spring_back_hold`        | Forward stop for half the duration, then rest-to-rest return on the accepted path to the trip cursor.                                                                    | Same authored replacement through JTC preemption. Must never be cancel-first.                      | Same authored replacement through native replay.                                          |
| `hard_deceleration`       | Treated as a hard-stop floor only when capability proves it.                                                                                                             | Available only with `decelerate_on_cancel`, velocity state, and per-joint cancel decel limits.     | Not available; no JTC cancel profile exists.                                              |
| Immediate hard-stop floor | Final floor after refusal/unconfirmed/deadline/collision/hard disposition. Diagnostics carry `hard_stop_floor`, `hard_stop_level`, and `hard_stop_floor_command_source`. | Plain JTC cancel/hold, reported as `hard_stop_level=jtc_cancel_unprofiled_hold`.                   | Deterministic immediate freeze/hold, reported as `hard_stop_level=native_immediate_hold`. |
| Visualization authority   | Stop trajectory is the command authority for commanded/native views. Feedback is evidence/monitoring.                                                                    | Composer commanded-sim view follows the authored reference; real robot lanes remain measured-only. | Native view follows the authored replay directly.                                         |

## 3. Defaults And Profile Validation

Shared constants are in
`manyforge_core/include/manyforge_core/types/motion_interrupt.hpp`.

- `kDefaultDecelerationTimeS = 1.0`.
- `kMaxControlledStopDecelerationTimeS = 2.0`.
- `motion_interrupt_deceleration_time_or_default(profile)` is the only supported
  way for backends to resolve an unset stop deceleration time.
- `allow_natural_completion_on_short_path` normalizes to `true` only for
  `soft_on_path`; it is false and forbidden as an authored field for other
  modes. The short-path case still remains under the immutable outer Stop
  deadline and terminal-evidence requirement.
- The profile has no `max_stop_time_s`. The verified base plan owns one outer
  deadline and fixed phase allocation; Stop acceptance publishes only the
  shortened remaining budget and never extends it.
- `per_joint_max_deceleration` and optional
  `per_joint_settle_velocity_threshold` are typed source-joint maps. Prepare
  resolves them to exact resource kinds and rejects incomplete, unknown,
  wrong-unit, or looser threshold rows.

Profile shape is validated at retained policy decode, Core profile validation,
and Prepare-time mapping:

- C++ kernel/interrupt layer: `manyforge_core/src/interrupt_manager.cpp`.
- retained policy decode: `manyforge_deployment/src/deployment_applicability_codec.cpp`;
- exact resource mapping: `manyforge_core/src/stop_profile_mapper.cpp`.

The cap is uniform across stop modes when a profile carries
`deceleration_time_s`. Longer operational stops require an explicit future policy
change; they should not slip through as ordinary soft stops.

Clients may display retained profile and certificate facts, but they cannot
edit a prepared participant or select a fallback while a Stop is in flight.

## 4. Operator And Fault Call Chains

Public addressed Stop:

```text
Headless / Program / Robot Control client
  -> POST /api/execution/stops or /api/execution/full-stop
  -> ResourceAddressedExecutionKernel::stop()/full_stop()
  -> StopScopeResolver + EmergencyStopAcceptanceReserve
  -> ResourceSlotTable::stop_fence_batch()
  -> BoundedStopControlExecutor
  -> BoundedStopStageAttemptExecutor
  -> IControlDomainExecutionPort::TranslateStopStage()
  -> ReduceStopTerminal()
  -> terminal/checkpoint journal projection + surveillance/recovery
```

Runtime collision:

```text
subordinate ExecutionKernel collision observation
  -> addressed Kernel event sink
  -> deterministic internal operation-scoped Stop
  -> declared coordination-policy expansion
  -> same sole Stop transaction path above
```

The important behavioral invariant is that product Stop has one authority.
Subordinate engines may publish collision/interrupt facts but cannot issue a
competing backend Stop. Resume always creates new prepared participant/goal
identity from fresh state; it never resumes stale controller trajectory state.
`ExecutionKernel` has no Stop entry point of its own.

## 5. soft_on_path Planner

`plan_soft_on_path_stop(...)` is side-effect-free and returns a
`SoftOnPathStopPlanDTO` containing the replacement trajectory plus structured
diagnostics. It is used by both the ros2_control and native lanes.

Inputs:

- the accepted active trajectory;
- the trip state `(q, qdot)` with joint names;
- active trajectory time, when available;
- active runtime velocity scale;
- stop profile timing and per-joint deceleration limits;
- planner config: handoff delta, sample period, optional path/velocity drift
  limits, optional jerk limits.

The planner projects the trip state onto the active path, preserves a
seam-compatible initial state, and builds a derivative-complete trajectory:

```text
qdot = q'(u) * udot
qddot = q''(u) * udot^2 + q'(u) * uddot
```

Every dispatchable planner output includes positions, velocities, and
accelerations. Position-only waypoints are not sufficient for the soft stop,
because JTC would infer a different in-between motion.

### 5.1 Fast Quintic Candidate

The normal path-rate deceleration uses:

```text
v(r) = 1 - 10r^3 + 15r^4 - 6r^5
P(r) = r - 2.5r^4 + 3r^5 - r^6
```

with `r = t / T`. The boundary conditions are:

- `v(0)=1`, `v(1)=0`;
- `v'(0)=v'(1)=0`;
- `v''(0)=v''(1)=0`.

So the path-rate acceleration and path-rate jerk are zero at the seam and at the
terminal hold. `P(1)=0.5`, which is correct: a smooth deceleration from full rate
to rest covers half the distance of constant full-rate travel over the same time.

### 5.2 Minimum-Time Gate

Before emitting the timed stop, the planner computes a minimum on-path stop time
with `compute_soft_on_path_min_stop_time(...)`. If the requested
`deceleration_time_s` is shorter than the declared per-joint limits can support,
the planner rejects with `recommended_disposition=hard_stop`; it does not silently
stretch the operator's requested soft-stop time.

### 5.3 Jerk-Limited Retimer

If the fast quintic candidate exceeds a declared acceleration or jerk limit, the
planner tries `compute_jerk_limited_stop_profile(...)`.

This retimer forward-integrates the path-rate phase plane `(u, udot, uddot)` and
uses a conservative backward velocity envelope to anticipate future corners. It
binary-searches the deceleration target to land at the requested stop time when it
can prove a derivative-complete, limit-consistent trajectory.

It is dispatchable only if all of these are true:

- the terminal sample is zero velocity and zero acceleration;
- achieved time matches the requested time within sample tolerance;
- oversampling the emitted trajectory through the same interpolation path stays
  within per-joint acceleration and finite-difference jerk limits.

### 5.4 ALI / Backtracking Candidate

If the quintic candidate fails and the jerk-limited retimer still cannot prove a
dispatchable exact-time profile, the planner tries the private ALI/backtracking
candidate set in `compute_ali_backtracking_stop_profile(...)`.

Retained planner implementation:

- generates bounded triangular S-curve path-rate brake candidates;
- tries the full requested horizon first, then progressively earlier
  stop-and-hold candidates;
- allows the moving part of the stop to finish before the requested horizon when
  that avoids a future bottleneck/corner;
- appends zero-velocity, zero-acceleration hold samples until
  `deceleration_time_s`;
- validates the whole emitted trajectory by oversampling acceleration and
  finite-difference jerk before it can dispatch.

Important semantic distinction: ALI is still a valid `soft_on_path` stop because
it stays on the accepted path and reaches rest within the requested time, but it
may stop before the horizon and hold. Diagnostics expose this explicitly:

- `soft_on_path_retimer=ali_backtracking`;
- `prior_retimer=jerk_limited`;
- `ali_motion_stop_time_s`;
- `ali_hold_time_s`;
- `ali_stopped_before_requested_time`;
- `ali_backtracking_iterations`.

If ALI cannot prove a valid trajectory, the planner returns a hard-stop
disposition with ALI diagnostics. Backend escalation wiring must honor that
disposition and surface the complete fallback chain through the controlled-stop
history.

## 6. ros2_control Execution

`Ros2ControlExecutionBackend::execute_controlled_stop(...)` handles the profile.

For `soft_on_path`:

1. Capture a fresh observed state before preemption.
1. Recover the active accepted trajectory and trajectory time.
1. Choose measured or model-derived seam velocity according to lane config.
1. Call `plan_soft_on_path_stop(...)`.
1. Publish unit runtime scale when JTC speed scaling is enabled, so the stop is
   executed as authored.
1. Dispatch the replacement trajectory with
   `send_fjt_replacement_trajectory(...)`.
1. Wait for the managed FJT result within the effective stop deadline.
1. Wait, bounded by the same stop budget, for measured velocity to settle when
   the backend exposes velocity feedback. Normal controller-feedback lag is not
   classified as failure from a single instantaneous sample.
1. Read a fresh post-stop sample and publish a stopped snapshot.
1. Set `backend_stop_confirmed=true` only after command completion and fresh
   post-stop state are available.

For `spring_back_hold`:

1. Capture a fresh observed state before preemption.
1. Recover the active accepted trajectory and trajectory time.
1. Call `plan_spring_back_on_path_stop(...)`.
1. The planner calls `plan_soft_on_path_stop(...)` internally with half of the
   requested duration for the forward decel phase. This reuses the same
   command-frame lead-in, ALI/retimer checks, and dispatch-latency compatibility
   as `soft_on_path`.
1. Append a reverse on-path, rest-to-rest quintic return from the forward stop
   point to the trip cursor over the other half of the requested duration.
1. Dispatch the combined replacement trajectory with
   `send_fjt_replacement_trajectory(...)`.
1. Wait for the managed FJT result and post-stop state. The final command-frame
   pose is the trip cursor, not the observed off-path feedback sample.

For hard-stop categories:

- `hard_deceleration` uses JTC cancel/decelerate-on-cancel only when configured
  and observable.
- the final floor is plain cancel/hold.

## 7. Native Execution

`RoboPlanExecutionBackend::execute_controlled_stop(...)` uses the same core
planner for `soft_on_path` and `spring_back_hold`. It stops the normal worker,
replays the returned replacement waypoints as authored snapshots, updates the
scene positions, and publishes a stopped snapshot at the final waypoint.

Native `spring_back_hold` is no longer a stop-and-hold visualization shortcut:
it replays the same two-phase on-path spring-back trajectory used by the
ros2_control lane.

## 8. Velocity Scaling

The stop planner starts from the effective trip state. If the robot is moving at a
reduced runtime scale, the measured/model-derived trip velocity already includes
that scale, so the planned stop sheds the velocity that is actually present.

On the ros2_control lane, ManyForge publishes unit execution scale immediately
before dispatching the replacement trajectory. This does not speed up the robot;
the replacement trajectory already encodes the effective current velocity. The
publish/action pair is not a DDS-atomic transaction, so speed-scaling-enabled
lanes require seam validation on the target controller.

Owned scaling must be frozen during stop. Hardware/vendor scaling that ManyForge
cannot freeze must be monitored; a material change invalidates a timed soft-stop
claim and must escalate to a hard-stop category.

## 9. Fallback Semantics

Fallback decisions are deterministic from the trip-time planner result:

| Planner/backend condition                                                                                                   | Disposition                                                         |
| --------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| Planner succeeds                                                                                                            | execute `soft_on_path`.                                             |
| Original trajectory naturally reaches rest within policy window                                                             | natural completion, enabled by default unless the profile opts out. |
| Recoverable short-path/deviation failure and `spring_back_hold` is explicitly allowed                                       | execute `spring_back_hold`.                                         |
| Infeasible stop time, ALI cannot prove a valid trajectory, dispatch timeout, scaling-source change, or stop monitor timeout | hard-stop category.                                                 |

`spring_back_hold` is not the critical fallback after a runtime scaling-source
change. That case must use hard-stop behavior because the timed replacement
contract is no longer trustworthy.

## 10. Observability And Diagnostics

Key diagnostics:

- `soft_on_path_retimer`: `quintic`, `jerk_limited`, or `ali_backtracking`;
- `retimer_trigger_reason`;
- `retimer_used_backward_envelope`;
- `ali_motion_stop_time_s`, `ali_hold_time_s`, `ali_stopped_before_requested_time`;
- `path_deviation_max_rad`;
- `backend_call_to_goal_dispatch_ns`, `goal_dispatch_to_accept_ns`,
  `backend_call_to_goal_accept_ns`;
- `stop_command_source`;
- `backend_stop_confirmed`.

The visualizer should use the backend-authored stop trajectory on sim/native
commanded views. Measured feedback remains available as a delayed ghost/source.

## 11. Qualification Surface

Core tests:

- `SoftOnPathStopPlannerTest.UsesJerkLimitedRetimerWhenQuinticJerkWouldExceedLimit`;
- `SoftOnPathStopPlannerTest.UsesAliBacktrackingToStopBeforeFutureCornerAndHold`;
- `SoftOnPathStopPlannerTest.EscalatesAccelBindWhenRetimerCannotProveDispatch`;
- `SoftOnPathStopPlannerTest.RampIsAccelerationAndJerkContinuousAtSeamAndTerminus`;
- `SpringBackOnPathStopPlannerTest.BuildsTwoPhaseOnPathReturnToTripCursor`;
- `SpringBackOnPathStopPlannerTest.RejectsWhenForwardHalfCannotContainLeadIn`;
- `SoftOnPathMinStopTimeTest.*`;
- `JerkLimitedStopProfileTest.*`;
- `InterruptManagerMotionScopedTest.StopProfileDecelerationTimeDefaultsToTwoSeconds`.

Performance evidence on Thor after ALI wiring:

- normal trip-time planner: about `p50 96 us`, `p999 314 us`;
- ALI-selected curved-path fallback: about `p50 4.4 ms`, `p99 4.4 ms`, rare
  scheduler-tail max about `12 ms`;
- arm-time feasibility envelope remains much heavier and is not the trip-time
  stop path.

Release qualification still requires live lane evidence:

- Isaac stock JTC seam-preserving preempt across scale values;
- visible `soft_on_path` and two-phase `spring_back_hold` behavior;
- Run-after-Stop replan semantics;
- hardware scaling-source-change escalation;
- Lite6 simulated scene before physical Lite6 testing.
