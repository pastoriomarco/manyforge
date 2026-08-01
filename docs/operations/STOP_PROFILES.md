# Controlled Stop Profiles

> **Cutover status:** retained as a restoration and behavior reference. The
> command recipes below are not qualified on the current `dev` branch and
> must not be executed until I8/P5 and the selected execution lane gate pass.
> Present-tense Composer and runtime descriptions below refer to the captured
> baseline; retained stop math alone is not a live Stop owner.

This runbook records the former operator-facing controlled-stop behavior in Composer.
Implementation details live in
[`../reference/STOP_PROFILES_INTERNALS.md`](../reference/STOP_PROFILES_INTERNALS.md).

ManyForge controlled stops are operational software stops. They are designed to
be deterministic, auditable, and compatible with robot safety functions, but they
are not a certified E-stop or protective-stop channel.

## Stop Modes

| Mode                      | What It Does                                                                                                  | When To Use                                                                                                                     |
| ------------------------- | ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `soft_on_path`            | Sends a backend-authored replacement trajectory that decelerates on the already accepted path.                | Default operator stop for ordinary motion.                                                                                      |
| `spring_back_hold`        | Sends a two-phase on-path replacement: decelerate forward, then return on the same path to the trip cursor.   | Operator-selected visible return-to-trip behavior.                                                                              |
| `hard_deceleration`       | Uses JTC `decelerate_on_cancel` when the ros2_control deployment proves that capability.                      | Fast profiled stop floor on ros2_control lanes that support it.                                                                 |
| Immediate hard-stop floor | ros2_control plain cancel/hold or native immediate hold.                                                      | Mandatory final floor when a controlled replacement is refused, times out, or cannot be trusted. Not a normal operator profile. |
| Natural completion        | Keeps the accepted trajectory running when it will reach rest before a useful injected stop can be generated. | Short remaining path. Composer stays in `Stopping` until terminal evidence arrives.                                             |

`spring_back_hold` is not the default fallback for `soft_on_path`. It is often
harder to generate because the forward deceleration gets only part of the total
time and then the robot must return to the trip cursor. Infeasible, refused,
timed-out, or unconfirmed stops use the hard-stop floor instead.

## Editable Fields

The Deployment tab exposes the controlled-stop parameters while the runtime is
idle or stopped. They are frozen during an active stop.

| Field                   | Default                  | Rule                                                                                                                      |
| ----------------------- | ------------------------ | ------------------------------------------------------------------------------------------------------------------------- |
| Mode                    | Deployment-specific      | Must be one of the backend-supported modes. `Not configured` is only a disabled placeholder.                              |
| Deceleration Time       | `1.0 s`                  | Requested wall-time stop duration. Must be `> 0` and `<= 2.0 s`.                                                          |
| Max Stop Time           | Auto                     | Optional confirmation deadline. If unset, ManyForge derives it from the requested stop time plus lane tolerance.          |
| Settle Velocity         | `0.01 rad/s`             | Optional measured-rest threshold where measured velocity exists.                                                          |
| Max Deceleration        | `10 rad/s^2` in examples | Per-joint operational limit used by the planner.                                                                          |
| Lead-In                 | `0.1 s`                  | Command-frame lead-in budget for seam-preserving replacement trajectories. Must be less than the requested stop duration. |
| Sample Period           | `0.01 s` in examples     | Stop trajectory sampling period.                                                                                          |
| Max Jerk                | Derived unless set       | Per-joint operational jerk limit.                                                                                         |
| Path Ramp Enabled       | Enabled                  | If disabled, `soft_on_path` may not be advertised.                                                                        |
| Stop Completion Timeout | Lane-specific            | Extra ros2_control confirmation margin stored as `stop_completion_timeout_s`.                                             |

Executable deployments must configure both:

- `operator_stop_profile`;
- `default_fault_stop_profile`.

The backend must also report a valid hard-stop floor:

- `hard_deceleration` when JTC `decelerate_on_cancel` is proven;
- `jtc_cancel_unprofiled_hold` for ros2_control plain cancel/hold;
- `native_immediate_hold` for the native lane.

If either profile or the hard-stop floor is missing, Composer degrades readiness
and blocks Run.

## Deployment Configuration Checklist

A stop profile is a request. The backend advertises it only when the deployment
also declares the lane-specific capability evidence needed to execute it.

For ros2_control / Isaac lanes that use `operator_stop_profile.stop_mode: soft_on_path`, configure all of the following under `adapters.execution.config`:

```yaml
enable_soft_on_path_path_ramp: true
soft_on_path_sample_period_s: 0.01
soft_on_path_stop_traj_delta_s: 0.1
soft_on_path_max_jerk_rad_s3:
  shoulder_pan_joint: 50.0
  shoulder_lift_joint: 50.0
  elbow_joint: 50.0
  wrist_1_joint: 50.0
  wrist_2_joint: 50.0
  wrist_3_joint: 50.0
operator_stop_profile:
  profile_id: operator_soft_on_path
  stop_mode: soft_on_path
  deceleration_time_s: 1.0
  per_joint_max_deceleration_rad_s2:
    shoulder_pan_joint: 10.0
    shoulder_lift_joint: 10.0
    elbow_joint: 10.0
    wrist_1_joint: 10.0
    wrist_2_joint: 10.0
    wrist_3_joint: 10.0
```

Use the robot's actual joint names. If `soft_on_path` is selected but the path
ramp is not enabled, jerk limits are incomplete, or the backend cannot construct
the replacement trajectory, capability readiness reports `soft_on_path_not_enabled`
and Composer blocks Run instead of silently falling back.

The fault profile is intentionally separate. Runtime collision/fault paths should
normally use `default_fault_stop_profile` with `hard_deceleration` or the backend's
hard-stop floor. Do not use operator `soft_on_path` as the only fault-stop plan.

## Velocity Scaling

ManyForge freezes the owned runtime velocity scale during timed controlled stops.
Operator slider edits made during the stop are deferred and applied after the
stop window ends. The slider should be disabled only while the cycle is
`Stopping`, not after it has reached `Stopped`.

Hardware or pendant scaling that ManyForge cannot read/freeze is deferred from
the current release gate. It must be qualified on the real robot lane before
claiming timed behavior under external scaling.

## Stop History And Incidents

Successful controlled stops publish compact success summaries. Faults, warnings,
and fallback transitions publish incident snapshots containing:

- all recent events in order;
- events grouped by channel;
- requested and executed stop modes;
- hard-stop floor level;
- timing and settle evidence;
- fallback/escalation reasons.

In Composer, inspect the error panel and the execution history after any stop
that reports a warning or fault. Important fields include:

- `requested_stop_mode`;
- `executed_stop_mode`;
- `fallback_reason`;
- `hard_stop_transitioned`;
- `hard_stop_level`;
- `backend_stop_confirmed`;
- `stop_snapshot_at_rest`;
- `stop_snapshot_max_abs_velocity_rad_s`.

## Offline Qualification

Run the offline controlled-stop suite from the repo root:

```bash
./scripts/qualify-controlled-stop-offline.sh
```

This runs Docker-contained tests for:

- Composer stop-capability admission gates;
- frontend stop profile selectors and build;
- core soft-on-path, interrupt-manager, and execution-kernel tests;
- native backend and ros2_control capability tests;
- planner latency evidence.

Offline qualification does not prove live controller behavior. It proves the
deterministic math, policies, UI gates, and native/ros2 capability surfaces.

## Live Validation Checklist

When Isaac or a real robot is available, capture evidence for:

1. `soft_on_path` at the selected stop time.
1. `spring_back_hold` returning to the trip cursor.
1. `hard_deceleration` when supported by JTC.
1. Natural completion when remaining trajectory time is shorter than the stop
   window.
1. Stop while planning, validating, executing, and stopping.
1. Run-from-stopped replans from current state and does not reset.
1. Velocity slider is frozen only during `Stopping`.
1. Stop history shows requested/executed mode, hard-stop floor, timing, and
   settle evidence.
1. Stop/fault intervention recovery choices work as labeled:
   **Acknowledge fault** clears the operational latch without backend reset,
   leaves motion stopped, and exposes a separate Resume command that replans
   from the current pose; **Reset cycle** explicitly uses the reset path and may
   command the configured initial pose in
   commanded-sim deployments.
1. For real robots, hardware/pendant scaling behavior is observed and classified
   before claiming timed guarantees.

For Lite6 real-robot validation, first reproduce the lane in Isaac with the Lite6
scene and deployment, then repeat the same stop matrix on the physical robot.

## Intervention Recovery Choices

Stop/fault `INTERVENTION_REQUIRED` is a recovery boundary. Pressing Run while it
is latched does not silently acknowledge the fault. Composer presents:

- **Acknowledge fault** (`acknowledge_and_continue`): the kernel verifies the backend state is readable,
  the backend is not still stopping, the pose is finite, the hard-stop floor is
  reportable, and measured velocity is settled when a settle signal exists. It
  then clears the stop/fault latch without calling backend reset. The cycle
  remains stopped; the now-visible Resume command replans from the current
  robot pose only after a second operator press.
- **Reset cycle**: Composer explicitly uses the reset path. In
  commanded-sim lanes this can command the declared initial pose and must be
  treated as a reset action, not a resume.
- **Cancel**: leave the intervention latched.

The floating alarm stack exposes **Acknowledge alarm** for the exact current
activation. Acknowledgement records operator awareness; it does not clear the
condition, fault, recovery offer, or backend state. Up to three newest alarms
requiring acknowledgement are shown. Older attention alarms remain retained
and surface as newer ones are acknowledged. Repeated samples of one still-
active condition update its bounded evidence instead of creating popup spam;
after a real return and recurrence, a new activation is annunciated.

**Acknowledge all** appears only when at least two alarms require
acknowledgement and requires confirmation. Retry/Resume/Replan remains disabled
until every motion-attention alarm is acknowledged and alarm state is healthy/
reconciled, while Stop and explicitly safe Reset remain available. An
acknowledged active condition remains a standing alarm until its named source
reports return-to-normal. Operational fault acknowledgement exists only in the
execution controls. The Messages/History drawer retains source events and
alarm/recovery audit records after attention clears.
Unknown/unclassified interventions, unreadable backend state, active
direct-control leases, backend still stopping, or non-settled measured velocity
remain fail-closed until the operator fixes the cause or chooses Reset.
