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
- `XArmDriverServices` calls the driver's `clean_error` and `clean_warn`
  (`xarm.clear_error` / `xarm.clear_warning`) once each, bounded at 4 s; no
  answer or a non-zero return is an unknown outcome, never retried.
- The ros2_control motion domain merges this status and routes the vendor
  primitives and the gate
  (`RetainedRos2ControlRuntimeHandle::InstallDeviceStatusAdapter`); CCLC keeps
  incidents, retained causes and provider loss.
- `lifecycle_profile.hpp` holds the Real arm's catalog
  (`manyforge.provider.ros2_control.xarm`): Prepare arms through the stock
  hardware activation (never over a vendor error) and absorbs the plugin's
  reactivate-all; Enable is the trajectory-controller activation; Disable
  switches off the controller, then the hardware; Recover disarms and clears
  the error, then the warning. It also holds the maintained Lite 6 gripper
  plans and the native `xarm_arm_provider` Recover plan.

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
