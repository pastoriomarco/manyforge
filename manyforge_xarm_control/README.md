# ManyForge xArm control

`manyforge_xarm_control` is the xArm vendor adapter of the ManyForge real lane
(R7). It serves every xArm-family robot (xArm 5/6/7, Lite 6, UF850) through the
unmodified vendor driver. It does not own CCLC transactions, readiness,
execution admission, retries or Stop semantics.

## Current boundary (R7 packages 3 and 4: observation and lifecycle actions)

- `XArmRobotStatesTransport` subscribes to the driver's
  `<component namespace>/xarm/robot_states` (`xarm_msgs/msg/RobotMsg`) and
  keeps the status fields of the latest report, stamped at receipt. The pose
  is never read. The joint angles are never a measured state (under an
  emergency stop the controller reports all-zero angles; measured joints come
  only from the joint state broadcaster); they serve only the Enable gate's
  agreement check, with no vendor error.
- `XArmBrandLifecyclePort` classifies that status into the common cause
  classes (C1-C3 `emergency_stop`, C31 `collision`, other errors `fault`, an
  unrequested mode/state change `external_control`, a report older than
  0.5 s `availability`), the `device.*` facts and the
  `manyforge.vendor_code.*` incident codes. The hardware activation (arming,
  the plugin's `on_activate`) and deactivation (position mode) are own
  effects, never external control. `CheckActivationGate` is the vendor part of
  the Enable gate: the robot ready and the measured joints within the
  per-robot tolerance of the reported angles.
- m5.5 (activation and state handling): while ManyForge has not armed the
  robot, a vendor error (C2 left after an emergency stop) is only facts and
  vendor codes; Prepare and Recover clear it (`xarm.clear_error`, verified on
  the reports: an error still reported 5 s after the clear fails the step
  typed, `xarm_error_persists_after_clear` with the vendor code, and stays
  facts: no incident). C10-C17 are always a cause. Unrequested states 3/4/5/6 and modes other than
  0/1 carry a typed condition token (`xarm_paused`, `xarm_stopped`,
  `xarm_mode_changed`, `xarm_stop_requested`, `xarm_manual_mode`,
  `xarm_mode_external`, `xarm_moved_externally`). An error-free external
  change while armed is, under the policy `external_move_rearm: automatic`
  (default), the `device.moved_externally` fact (with `device.in_motion`):
  on an idle cell the CCLC target driver shows a non-blocking warning and
  motion commands are refused `component_rearm_required` (no effect) until
  Recover cell re-arms it (Recover, then the driver's Prepare and Enable);
  Composer does that and re-sends the command in one press; while ManyForge work may be in flight it is
  the `external_control` incident; under `operator` it is always the
  incident. The Real arm plans are
  bounded at 10 s.
- `XArmDriverServices` calls the driver's `clean_error` and `clean_warn`
  (`xarm.clear_error` / `xarm.clear_warning`) once each, bounded at 4 s; no
  answer or a non-zero return is an unknown outcome, never retried.
- The ros2_control motion domain merges this status and routes the vendor
  primitives and the gate
  (`RetainedRos2ControlRuntimeHandle::InstallDeviceStatusAdapter`); CCLC keeps
  incidents, retained causes and provider loss.
- `lifecycle_profile.hpp` holds the Real arm's catalog
  (`manyforge.provider.ros2_control.xarm`): Prepare arms through the stock
  hardware activation (never over a vendor error), absorbs the plugin's
  reactivate-all, switches the joint state broadcaster back on alone
  (`ros2_control.broadcaster.activate`) when that reactivation left it off,
  and marks the arm prepared only on a broadcaster sample received after the
  activation; Enable is the trajectory-controller activation; Disable
  switches off the controller, then the hardware; Recover disarms and clears
  the error, then the warning. It also holds the maintained Lite 6 gripper
  plans and the native `xarm_arm_provider` Recover plan.
- A Real component's gripper domain has its own provider
  (`manyforge.provider.ros2_control.xarm.gripper`, its own provider instance):
  the shared ros2_control plans, with Recover targeting `disabled` like the
  arm, so the component has one common Recover target. A healthy gripper has
  nothing to clear. The plans are in `manyforge_roboplan`
  (`retained_ros2_control_gripper_lifecycle_plans`).

The supervised mode, its restricted transport session, the fork-only
messages and the supervised Lite 6 lifecycle catalog were removed in R7
package 5 (2026-10-25). The package builds against the standard `xarm_msgs`
of the fork's `lyrical` branch.

## Qualification

`test_xarm_brand_lifecycle_port` covers the classification, the device facts,
freshness, unrequested-change tracking, the activation gate and the Real
catalog; `test_xarm_robot_states_transport` covers the subscription against a
real ROS publisher; `test_xarm_driver_services` covers the two recovery
services against in-process servers.
