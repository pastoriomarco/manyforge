# Controlled Stop — Internals Reference

Reference companion to [`operations/STOP_PROFILES.md`](../operations/STOP_PROFILES.md).
This document describes the implemented stop architecture and the current
qualification surface. It is intentionally code-facing: future maintainers should
be able to verify the stop behavior from the files and symbols named here without
reconstructing the design history.

Historical rationale and work planning live in `manyforge_specs/docs/plans`.
This file is the source of truth for the current `manyforge` implementation.

## 1. Architecture

ManyForge uses the modified Option A architecture:

1. The backend owns the stop profile.
2. The backend computes a concrete replacement trajectory in non-real-time code.
3. Stock `joint_trajectory_controller` executes that trajectory through
   `FollowJointTrajectory` preemption on ros2_control lanes.
4. The native lane replays the same core trajectory directly, because it has no
   ROS controller.
5. Stop success is claimed only after a terminal hold and measured/post-stop
   evidence, not merely because a command was sent.

The current production stop math lives in
`manyforge_core/src/soft_on_path_stop_planner.cpp`; ros2_control dispatch lives in
`manyforge_roboplan/src/ros2_control_execution_backend.cpp`; native replay lives
in `manyforge_roboplan/src/roboplan_execution_backend.cpp`.

The retired controller-native JTC plugin is not the target architecture. It is
preserved historically on the `jtc_plugin` branch, but `main` uses stock JTC plus
backend-authored replacement goals.

## 2. Stop Catalog

| Stop role | Mechanism | Intended use |
| --- | --- | --- |
| `soft_on_path` | Backend computes an on-path replacement trajectory from the trip state and active accepted path. Stock JTC executes it as a preempting FJT goal; native replays it directly. | Default operator stop. |
| `spring_back_hold` | Backend sends a seam-preserving anchored FJT goal to the trip/observed joint position with zero terminal velocity. Native stops the worker and holds the observed position. | Non-critical fallback / visible hold behavior. It must not be implemented as cancel-first. |
| `hard_deceleration` | JTC cancel path when `decelerate_on_cancel` is configured and proven. | Preferred hard-stop floor where available. |
| `immediate` hard-stop floor | Plain cancel/hold. | Last-resort physical stop when controlled replacement cannot be trusted. |
| Natural completion | Let the already accepted trajectory finish when it will reach rest sooner than the requested stop window and policy allows it. | Short-path case only, bounded by the effective stop deadline. Enabled by default; precision-contact lanes must opt out explicitly. |

`soft_on_path` and `spring_back_hold` are replacement/preemption profiles. They
must be sent before JTC enters cancel hold or `decelerate_on_cancel`, otherwise
the controller can introduce exactly the bump these profiles are meant to avoid.
`hard_deceleration` and `immediate` are native JTC cancel functions and are a
different category.

Hard-stop floors are only issued while ManyForge has reason to believe a
controller goal may still be active or untrusted: refused controlled stop,
unconfirmed stop, deadline watchdog, stop-path collision, or explicit hard-stop
disposition. If a controlled stop was accepted and backend-confirmed but
measured velocity still fails to settle within the effective stop deadline, ManyForge does
not issue another JTC cancel. At that point there is no known active trajectory
to cancel; the condition means the robot/controller is not following the
confirmed command, so ManyForge records `stop_not_settled_measured_velocity` and
holds `INTERVENTION_REQUIRED` for operator/robot-safety recovery.

## 3. Defaults And Profile Validation

Shared constants are in
`manyforge_core/include/manyforge_core/types/motion_interrupt.hpp`.

- `kDefaultDecelerationTimeS = 2.0`.
- `kMaxControlledStopDecelerationTimeS = 2.0`.
- `motion_interrupt_deceleration_time_or_default(profile)` is the only supported
  way for backends to resolve an unset stop deceleration time.
- `allow_natural_completion_on_short_path` defaults to `true` on all lanes. The
  short-path case is the least disruptive stop: the already accepted trajectory is
  allowed to finish while snapshots report `Stopping`, then the backend reports
  stopped/completed after fresh terminal evidence.
- `max_stop_time_s` is optional. When set, it is the hard stop-confirmation
  deadline. When unset, each backend derives the deadline from the requested stop
  time plus its stop-confirmation tolerance: ros2_control uses
  `deceleration_time_s + stop_completion_timeout_s`; the native backend uses
  `deceleration_time_s + soft_on_path_sample_period_s`.

Profile shape is validated in three layers:

- C++ kernel/interrupt layer: `manyforge_core/src/interrupt_manager.cpp`.
- Python behavior client DTOs: `manyforge_behavior/manyforge_behavior/kernel_client.py`.
- Native/ROS Python bindings: `manyforge_roboplan/python/bindings.cpp`.

The cap is uniform across stop modes when a profile carries
`deceleration_time_s`. Longer operational stops require an explicit future policy
change; they should not slip through as ordinary soft stops.

Composer exposes the loaded operator stop mode and `deceleration_time_s` in the
transport controls. Edits are allowed only while idle/stopped, update the loaded
deployment session through `/api/deployment`, and are frozen while a stop is in
flight.

## 4. Operator And Fault Call Chains

Operator Stop:

```text
UI Stop
  -> POST /api/cycle/stop
  -> manyforge_behavior/cycle_manager.py: CycleManager.stop()
  -> kernel.stop()
  -> ExecutionKernel::stop()
  -> IExecutionBackend::execute_controlled_stop(operator_stop_profile)
```

Fault preemption:

```text
latched interrupt / fault
  -> ExecutionKernel::apply_latched_interrupt_preemption(...)
  -> IExecutionBackend::execute_controlled_stop(scope/default fault profile)
```

The important behavioral invariant is that Stop is a physical stop but a
software-resumable hold. Run from STOPPED must clear the hold and replan/re-dispatch
from the fresh measured state; it must not require Reset and must not resume stale
controller trajectory state.

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

Current implementation:

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
disposition; see the current plan status in `manyforge_specs`.

## 6. ros2_control Execution

`Ros2ControlExecutionBackend::execute_controlled_stop(...)` handles the profile.

For `soft_on_path`:

1. Capture a fresh observed state before preemption.
2. Recover the active accepted trajectory and trajectory time.
3. Choose measured or model-derived seam velocity according to lane config.
4. Call `plan_soft_on_path_stop(...)`.
5. Publish unit runtime scale when JTC speed scaling is enabled, so the stop is
   executed as authored.
6. Dispatch the replacement trajectory with
   `send_fjt_replacement_trajectory(...)`.
7. Wait for the managed FJT result within the effective stop deadline.
8. Wait, bounded by the same stop budget, for measured velocity to settle when
   the backend exposes velocity feedback. Normal controller-feedback lag is not
   classified as failure from a single instantaneous sample.
9. Read a fresh post-stop sample and publish a stopped snapshot.
10. Set `backend_stop_confirmed=true` only after command completion and fresh
    post-stop state are available.

For `spring_back_hold`:

1. Capture observed joint positions before stopping.
2. Use `send_fjt_anchored_goal(...)` to send a negative-time anchor plus a
   zero-velocity terminal point at the observed/trip position.
3. Wait for terminal result and post-stop state.

For hard-stop categories:

- `hard_deceleration` uses JTC cancel/decelerate-on-cancel only when configured
  and observable.
- the final floor is plain cancel/hold.

## 7. Native Execution

`RoboPlanExecutionBackend::execute_controlled_stop(...)` uses the same core
planner for `soft_on_path`. It stops the worker, replays the returned
replacement waypoints as authored snapshots, updates the scene positions, and
publishes a stopped snapshot at the final waypoint.

Native `spring_back_hold` currently stops the worker and holds the observed
position. The remaining visualization-quality gap is an animated native
spring-back replay matching the ros2_control anchored trajectory. That gap is
documented in the plan; it is not a physical stop failure.

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

| Planner/backend condition | Disposition |
| --- | --- |
| Planner succeeds | execute `soft_on_path`. |
| Original trajectory naturally reaches rest within policy window | natural completion, enabled by default unless the profile opts out. |
| Recoverable soft planner failure and `spring_back_hold` is allowed | execute `spring_back_hold`. |
| Infeasible stop time, ALI cannot prove a valid trajectory, dispatch timeout, scaling-source change, or stop monitor timeout | hard-stop category. |

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
- `SoftOnPathMinStopTimeTest.*`;
- `JerkLimitedStopProfileTest.*`;
- `InterruptManagerMotionScopedTest.StopProfileDecelerationTimeDefaultsToOneSecond`.

Performance evidence on Thor after ALI wiring:

- normal trip-time planner: about `p50 96 us`, `p999 314 us`;
- ALI-selected curved-path fallback: about `p50 4.4 ms`, `p99 4.4 ms`, rare
  scheduler-tail max about `12 ms`;
- arm-time feasibility envelope remains much heavier and is not the trip-time
  stop path.

Release qualification still requires live lane evidence:

- Isaac stock JTC seam-preserving preempt across scale values;
- visible `soft_on_path` and `spring_back_hold` behavior;
- Run-after-Stop replan semantics;
- hardware scaling-source-change escalation;
- Lite6 simulated scene before physical Lite6 testing.
