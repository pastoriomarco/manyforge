# Controlled Stop — Internals Reference

`ResourceAddressedExecutionKernel` is the sole Stop owner. In every lane (Mock,
Isaac, Real) the robot runs stock `joint_trajectory_controller` (JTC) under
ros2_control, and the Kernel's participant Stop stage sends the Stop
trajectories to it as replacement goals.

Reference companion to [`operations/STOP_PROFILES.md`](../operations/STOP_PROFILES.md).
This document describes the Stop owner and its realization. It is
intentionally code-facing: future maintainers should be able to verify the
stop behavior from the files and symbols named here without reconstructing
the design history.

ManyForge Stops are operational commands, not safety-rated stops. The cell's
emergency and protective stops belong to the robot's own safety controller and
the integrator's safety system.

Current source and generated contracts win if this reference drifts.

## 1. Architecture

ManyForge uses one addressed owner and prepared participant-local realization:

1. Apply compiles each robot's Deployment Stop settings (section 3) into the
   verified base Stop plan: one to eight escalation stages, the last a hard
   stop.
1. Kernel Prepare maps the typed source-joint profile into one immutable
   participant Stop invocation plan before physical dispatch
   (`manyforge_core/src/stop_profile_mapper.cpp`).
1. Public Stop/Full Stop is resolved and captured by
   `ResourceAddressedExecutionKernel`; the emergency reserve and resource-slot
   fence commit recognition without waiting for storage or backend I/O.
1. A fixed `BoundedStopControlExecutor` progresses the transaction. Independent
   `BoundedStopStageAttemptExecutor` lanes invoke the exact
   `IControlDomainExecutionPort::TranslateStopStage` contract with the remaining
   phase budget, so one bounded provider lane cannot block another participant
   or the watchdog.
1. For a robot, `ParticipantStopStageHelper`
   (`manyforge_core/src/participant_stop_stage_helper.cpp`) runs the stage:
   it plans the Stop trajectory from the trip state, sends it to JTC as a
   replacement goal and waits for the controller's result (section 6).
1. The pure terminal reducer (`ReduceStopTerminal`) consumes immutable
   stage/evidence history and emits one Stop certificate. Journal projection
   occurs after containment and cannot rewrite that certificate.

The stop math is in `manyforge_core/src/soft_on_path_stop_planner.cpp` and
`manyforge_core/include/manyforge_core/motion_interrupt/stop_replacement.hpp`.
ManyForge uses stock JTC plus Kernel-authored replacement goals; no
controller-native Stop plugin is used.

## 2. Stop Catalog

| Stop role | Mechanism | Intended use |
| --- | --- | --- |
| `soft_on_path` | On-path replacement trajectory from the trip state along the active accepted path; JTC executes it as a preempting `FollowJointTrajectory` goal. | Default operator stop. |
| `spring_back_hold` | Two-phase on-path replacement: forward deceleration over half the requested duration, then a rest-to-rest return along the same path to the stop-trigger position over the other half. | Visible return to the trigger position. Never cancel-first. |
| `spring_back_fast` | The active trajectory's own state plus one zero-velocity point at the stop-trigger position, sent at once with no planning or checks. The robot can overshoot before returning. | Searches and probes stopped by an interrupt. Speed, acceleration and collision of this move are not checked. |
| Hard stop | Controlled hard stop (a closed-form brake within each joint's maximum deceleration), with the JTC cancel as its floor. | Last escalation stage; never selected in the Deployment editor. |
| Natural completion | The accepted trajectory finishes at its endpoint when its remaining path is too short for the requested Stop. | Short-path case only, first mode only, bounded by the Stop deadline. |

`soft_on_path` and both spring-back modes are replacement/preemption
profiles. They must be sent before JTC enters cancel hold or
`decelerate_on_cancel`, otherwise the controller can introduce exactly the
bump these profiles are meant to avoid.

`spring_back_hold` is intentionally stricter than a plain `soft_on_path` stop:
the forward deceleration gets only half the requested duration, then the robot
returns on the same path. Its return target is frozen once, at the command
cursor when the Kernel received the Stop (diagnostic
`spring_return_target_source`). It is a visible return-to-trip-point behavior,
not a feasibility fallback for acceleration/jerk-limited soft stops. If either
phase violates the configured limits, the planner refuses and the next mode
or the hard stop handles the event.

**Hard stop** (`RunHardStage`). The lane's hard stop is level 3, the
controlled hard stop (`plan_controlled_hard_stop_brake`): from the
controller's reference state, every joint decelerates linearly to rest at one
common time set by the slowest-braking joint, within that joint's maximum
deceleration (section 3), at the frozen speed factor. It is sent as a
replacement goal and finishes like a normal motion, on the controller's
result. If it cannot be built or sent, or ends unsuccessfully, level 4
follows: the JTC cancel with `decelerate_on_cancel`
(`hard_stop_level=jtc_cancel_decelerate_on_cancel`), bounded by the lane
row's `hard_stop_completion_timeout_s`. A goal that already ended is observed
as a hold (`hard_stop_level=goal_already_terminal`). Any hard-stop trigger
freezes the speed factor first.

## 3. Defaults And Profile Validation

Each robot target has one Stop profile in its Deployment, edited in the
Deployment editor's **Stop settings** card and authored as
`components[].target_settings.<target>.controlled_stop`:

- `stop_mode`: `soft_on_path`, `spring_back_hold` or `spring_back_fast`.
- `fallback_stop_modes`: up to two lower controlled modes, strictly in the
  order `soft_on_path` → `spring_back_hold` → `spring_back_fast`
  (`motion_interrupt_stop_fallbacks_valid`). The hard stop is implicit.
- `deceleration_time_s` (**Deceleration time**, or **Brake + return time** for
  spring-back): including the bounded lead-in (`stop_traj_delta_s`, 0.1 s on
  the ros2_control lane, `retained_ros2_control_lane_config.cpp`). Default
  `kDefaultDecelerationTimeS = 1.0`; at most
  `kMaxControlledStopDecelerationTimeS = 2.0`.
  `motion_interrupt_deceleration_time_or_default(profile)` is the only
  supported way to resolve an unset value.
- `max_stop_time_s` (optional, **Explicit maximum Stop time**): the time from
  the Stop request within which the controlled Stop must start braking; never
  below the deceleration time. Absent: the deceleration time plus
  `complete_move_on_signal_timeout` (500 ms).
- `allow_natural_completion_on_short_path`: omission normalizes to `true` for
  `soft_on_path` and `false` for the spring-back modes (which keep their
  precision return unless the profile opts in).
- `jerk_limits_enabled` (**Controlled Stop jerk**): an explicit choice;
  a profile without it cannot be prepared for a controlled attempt.
- `per_joint_limits[]`: per source joint `source_joint_name`,
  `max_deceleration_rad_s2`, `settle_velocity_threshold_rad_s` and optional
  `max_jerk_rad_s3`. Prepare resolves them to exact resource kinds and rejects
  incomplete, unknown, wrong-unit, or looser threshold rows.

The Deployment compiler parses `per_joint_limits` once
(`controlled_stop_limits`) and emits two outputs from it: the arm's Stop stage
rows and its controller profile's `max_deceleration_on_cancel_rad_s2`
(`{source_joint_name, max_deceleration_rad_s2}` per arm joint, sorted), so the
two cannot differ. The execution host writes that row into every lane's JTC
configuration as `max_deceleration_on_cancel` for each arm joint
(`manyforge_roboplan/src/controller_launch_artifacts.cpp`) and gives it to the
lane backend as the controlled hard stop's braking limit. A Stop profile that
misses an arm joint is refused at Deployment compile.

These Stop limits are separate from the speed limits of ordinary motion.
Profile shape is validated at retained policy decode
(`manyforge_deployment/src/deployment_applicability_codec.cpp`), at Deployment
compile (`manyforge_deployment/src/activation_deployment_compiler_internal.cpp`)
and at Prepare-time mapping (`manyforge_core/src/stop_profile_mapper.cpp`).

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
  -> ParticipantStopStageHelper (robot) / gripper command backend (tool)
  -> ReduceStopTerminal()
  -> terminal/checkpoint journal projection
```

Runtime collision and global interrupt rules with `on_trip: stop`:

```text
collision observation or rule trip
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

## 4.1 Stops And The Cell Latch

A Stop never latches the cell by itself, and nothing acknowledges its
settlement. After a Stop, the cell latch (CONTRACTS §5, `get_cell_latch`) is
derived from component states: it holds while any component in the cell is
in `fault` or `offline`, or reports `activity: unknown` (for a robot: a goal
whose result cannot come back or is overdue,
`manyforge_roboplan/src/ros2_control_component_state.cpp`), and it clears by
itself once every component is healthy and idle. There is no recovery
endpoint. A Program node whose Stop is unconfirmed waits for its component's
activity to leave `unknown` and then gets its normal result; only nodes with
`allow_intervention` hold for the operator. A rule trip is an operational
stop: never a fault, never a latch.

## 5. soft_on_path Planner

`plan_soft_on_path_stop(...)` is side-effect-free and returns a
`SoftOnPathStopPlanDTO` containing the replacement trajectory plus structured
diagnostics. `ParticipantStopStageHelper` calls it (and
`plan_spring_back_on_path_stop(...)`) at trip time.

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
stretch the operator's requested soft-stop time. The stage then tries the
profile's next fallback mode, if any (section 9).

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
disposition with ALI diagnostics; the stage records it
(`fallback_attempt_<n>`) and moves on as section 9 describes.

## 6. Robot Stop Stage (ros2_control)

`ParticipantStopStageHelper::RunControlledStage` handles a controlled profile
on every lane.

For `soft_on_path` and `spring_back_hold`:

1. Read a fresh controller sample before preemption and recover the active
   accepted trajectory and its cursor.
1. Choose the seam: the active command cursor at the frozen speed factor. On
   a Real component with measured trip evidence, the Deployment's
   `stop_lead_in_anchor` (**Stop lead-in start**) selects `commanded` (the
   default) or `measured` (the measured projection and its path rate). The
   measured state always remains the path and velocity drift guard.
1. Call `plan_soft_on_path_stop(...)` or `plan_spring_back_on_path_stop(...)`.
   The spring-back planner runs the soft planner with half the duration for
   the forward phase, then appends a reverse on-path, rest-to-rest quintic
   return to the frozen return target over the other half.
1. Publish unit runtime scale when JTC speed scaling is enabled, so the stop is
   executed as authored, and send the replacement goal.
1. Wait for the controller's result like a normal motion's, bounded by a
   normal motion's no-report budget for the Stop trajectory (at least 10 s).
   The Stop deadline (`max_stop_time_s` or its default) bounds everything up
   to the dispatch. Measured path deviation is recorded while waiting
   (`path_deviation_max_rad`).
1. Success ends the stage (`backend_stop_confirmed=true`). A goal-tolerance
   abort ends it with `stop_goal_tolerance_violated`; no result within the
   budget ends it with `backend_stop_not_confirmed`; the Kernel then runs the
   next escalation stage.

For `spring_back_fast` (`RunFastSpringBack`): one replacement goal with the
active trajectory's state and the return target at zero velocity, timed to
the deceleration time at the frozen factor; no planner, no limit or collision
check.

Natural completion is considered only for the first mode: when the planner
reports the remaining path too short, or the original goal already succeeded
before the replacement could be sent, the stage lets the accepted goal finish
(`completed_by_natural_completion`, `natural_completion_basis`).

## 7. Gripper Stop

A Stop cancels a position gripper's `GripperActionController` goal unless the
command is already done
(`manyforge_ros2_control_execution/src/ros2_gripper_command_backend.cpp`).
Then ManyForge does not send the cancel and lets the goal finish on the
controller's own result, like a robot move stopped in its last moment
(`completed_by_natural_completion` in the gripper status). This avoids a stock
controller race: a cancel that arrives after the goal finished, but before
`realtime_tools` sent its success result, is accepted and never answered, and
the success result is then never sent either. The command counts as done when
any of these holds:

- the action's feedback reported `reached_goal` (the command already
  completed, so nothing is left to cancel);
- the last finger position is within the gripper's **finish tolerance** of the
  commanded target. The tolerance comes from the gripper component's lane row,
  `gripper_stop_finish_tolerance` (finger position units; e.g. 0.0004 m for
  the Lite gripper, 0.04 rad for the xArm gripper and Robotiq 2F-85, about 5%
  of the stroke). A Deployment gripper target may override it with
  `stop_finish_tolerance` (**Stop finish tolerance**; 0 turns this case off);
- the node set `command_duration_s` and at least that long has passed since
  the command started (the author's declared duration);
- the command is waiting for its duration (`wait_for_duration`) after the
  controller reported done.

Otherwise the cancel is sent. Every gripper action server ManyForge uses holds
the fingers before it answers ACCEPT: the stock controller's cancel callback
holds the current position, and the Isaac gripper bridge
(`manyforge_ros_bringup/isaac_gripper_action_bridge.py`) publishes the
current finger position as Isaac's target and stops republishing the goal's
target. So the cancel's ACCEPT ends the stopped command, like the goal's own
terminal result; whichever comes first settles it and a later result for that
goal is ignored (N10b). The Kernel waits for that settlement
(`cancellation_pending` in the gripper status) before it retires the stopped
operation and Reset reopens the gripper (Q70). With no cancel sent it settles
on the goal's own result (then also `completed_by_natural_completion` on
success). No timer is involved.

## 8. Velocity Scaling

The stop planner starts from the effective trip state. If the robot is moving at a
reduced runtime scale, the measured/model-derived trip velocity already includes
that scale, so the planned stop sheds the velocity that is actually present.

ManyForge publishes unit execution scale immediately before dispatching the
replacement trajectory. This does not speed up the robot; the replacement
trajectory already encodes the effective current velocity. The publish/action
pair is not a DDS-atomic transaction, so speed-scaling-enabled lanes require
seam validation on the target controller.

Owned scaling is frozen during a Stop. Hardware/vendor scaling that ManyForge
cannot freeze must be monitored; a material change invalidates a timed
soft-stop claim and escalates to the hard stop.

## 9. Fallback Semantics

The stage tries the primary mode, then the profile's `fallback_stop_modes` in
order. A motion-takeover fallback starts at the first spring-back mode in that
list (`fallback_reason=takeover_spring_return`).

| Condition | Disposition |
| --- | --- |
| Planner succeeds | Execute that mode. |
| Remaining path too short and natural completion allowed (first mode only) | Natural completion. |
| Planner feasibility refusal (e.g. infeasible stop time, limit exceeded, ALI cannot prove a trajectory) with a lower fallback mode left and nothing sent yet | Try the next mode; the refusal is recorded as `fallback_attempt_<n>`. |
| No mode left, any other refusal (drift, invalid cursor, configuration), deadline exceeded, or a failure after a replacement may have reached the controller | Stage fails; the Kernel's next escalation stage (the hard stop) follows. |

Once any replacement may have reached the controller, its execution faults go
directly hard; the stage never retries another controlled mode.

## 10. Observability And Diagnostics

Key diagnostics:

- `soft_on_path_retimer`: `quintic`, `jerk_limited`, or `ali_backtracking`;
- `retimer_trigger_reason`;
- `retimer_used_backward_envelope`;
- `ali_motion_stop_time_s`, `ali_hold_time_s`, `ali_stopped_before_requested_time`;
- `requested_stop_mode`, `recoverable_substitution_pair`,
  `fallback_attempt_<n>`;
- `spring_return_target_source`, `spring_return_vs_trip_max_rad`;
- `path_deviation_max_rad`, `path_deviation_samples`;
- `hard_stop_level`, `hard_stop_fallback_reason`,
  `goal_recognition_delay_ns`;
- `stop_command_source`;
- `backend_stop_confirmed`.

The visualizer should use the Kernel-authored stop trajectory on simulated
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
- `JerkLimitedStopProfileTest.*`.

Performance evidence on Thor after ALI wiring:

- normal trip-time planner: about `p50 96 us`, `p999 314 us`;
- ALI-selected curved-path fallback: about `p50 4.4 ms`, `p99 4.4 ms`, rare
  scheduler-tail max about `12 ms`;
- arm-time feasibility envelope remains much heavier and is not the trip-time
  stop path.
