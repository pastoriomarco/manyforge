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
  vendor codes; Prepare and Recover clear it (`device.clear_error`, verified by
  on-demand `get_err_warn_code` reads, M6 XADAPT D12: when an error still
  remains after scheduled reads and the final state read answers, the step
  fails typed as `xarm_error_persists_after_clear` with the vendor code, and
  stays facts: no incident). C10-C17 are always a cause. Unrequested states 3/4/5/6 and modes other than
  0/1 carry a typed condition token (`xarm_paused`, `xarm_stopped`,
  `xarm_mode_changed`, `xarm_stop_requested`, `xarm_manual_mode`,
  `xarm_mode_external`, `xarm_moved_externally`). An error-free external
  change while armed is, under the policy `external_move_rearm: automatic`
  (default), the `device.moved_externally` fact (with `device.in_motion`):
  on an idle cell the CCLC target driver shows a non-blocking warning; the
  next admitted motion re-engages the robot before it is planned
  (`MotionReengageDecision`: at rest, no error, every axis enabled; then
  `set_mode(1)` and `set_state(0)` as ManyForge's own arming, and a ready
  report ends the latch); Recover cell re-arms it explicitly (Recover, then
  the driver's Prepare and Enable); while ManyForge work may be in flight it is
  the `external_control` incident; under `operator` it is always the
  incident. The Real arm plans are
  bounded at 10 s.
- `XArmDriverServices` calls the driver's `clean_error` and `clean_warn`
  (`device.clear_error` / `xarm.clear_warning`) once each, bounded by the
  shared component call cap (10 s until measured live). A `clean_error`
  return of 0 is answered success; API returns 1 and 2 are also answered
  outcomes (an error or warning remains) and are verified rather than treated
  as lost calls. Before sending `clean_error`, the adapter reads
  `get_err_warn_code` and `get_state` on demand, each under the remaining step
  budget and the 10 s call cap. If either read fails with an answered service
  error, the clear is refused without sending it; if either does not answer,
  the clear is not sent and the controller is reported unresponsive. After
  `clean_error` answers,
  verification reads `get_err_warn_code` at 0, 150 and 300 ms, each under the
  remaining step budget. Error code 0 proves the clear immediately, without
  another state read. When an error remains after polling, the adapter reads
  `get_state` once and compares the final on-demand error, warning and state
  values before and
  after. If the step expires before that state read can complete, verification
  remains uncertain with `component_lifecycle_deadline_expired`. A completed
  persistence result uses `xarm_error_persists_after_clear` and includes the
  latest error code. Effect classification compares only those three values
  before and after: equal values support no effect and a changed value shows
  an observed effect. Mode and motion-enable facts do not participate. No
  answer within a call's bound reports the controller unresponsive; the clear
  request is never repeated.
  Before an admitted motion, a re-engage calls `set_mode` and `set_state` once
  each under the fixed per-call cap described below; no request is retried.
- **Tool context is event-driven.** ManyForge computes the Active TCP offset
  (pose in the flange frame) and total payload (mounted-tool payload plus
  attached-object payload, with centre of mass in the flange frame).
  `XArmToolContextSender` uses `set_tcp_offset` (mm; roll, pitch and yaw in
  radians, fixed X-Y-Z axes) and `set_tcp_load` (kg; centre of mass in mm).
  An ordinary move, or a move whose TCP is already Active, sends no TCP offset
  and reads no offset report. The former per-move offset comparison is not
  used.
  - When **Send TCP offset to controller** is on, changing the Active TCP
    sends its offset, then re-engages and rebinds the controllers, and checks
    the reported offset in the same ready wait (0.01 mm, 1e-4 rad tolerance).
    A mismatch returns `tcp_offset_not_applied` ("The robot controller did
    not apply the TCP offset"). The move is sent only
    after the step succeeds. A Stop during the vendor call lets the call
    finish; the pending move is then not sent. A later motion failure does not
    undo a confirmed Active TCP change.
  - With offset sync on, a reset sends the End-effector TCP offset, or zero
    when there is no End-effector TCP, when the controller can take it. If the
    controller is disabled, not prepared, in error or unavailable, Reset
    completes and the next Enable re-syncs it.
    With offset sync off, Reset changes ManyForge's selection only; Enable
    sends offset 0 if the controller reports another offset.
  - Enable reads the fresh `robot_states.offset`; if it differs, the adapter
    sends the Active TCP offset (zero when sync is off), re-engages and checks
    it. It then sends the total payload before the controller switch. This is
    inside the Enable transition budget. A robot without an End-effector TCP
    can still Enable; the controller is synchronized to zero (the flange).
  - The host follows its current Scene projection for payload changes,
    including Program, direct and automatic grasp-loss detach, even while the
    arm moves. Successful projection transactions and rebuilds prompt updates.
    When the total changes on an enabled robot it uses `set_tcp_load`, without
    sending an offset or re-engaging. An object with no declared payload or
    payload zero changes nothing. There is no load readback, so external
    payload edits are not detected. A composition failure or unconfirmed update
    can fault the robot. Enable and each new motion goal also check the host's
    current projection. See
    [TCPs](../docs/authoring/TCPS.md#attach-to-the-active-tcp).
- **Send TCP offset to controller** (`controller_tcp_offset_sync`, per Real
  robot, default on) controls offset events. With it off, a TCP change updates
  ManyForge's selection only; the controller offset remains zero (the
  flange). Enable sends zero if the fresh report differs. Payload updates
  still run. Controller-side TCP features then refer to the flange. Mock and
  Isaac have no vendor adapter and ignore this setting.
- A sent offset that is not confirmed after the send (unanswered call, failed
  re-engage/rebind or offset mismatch) faults the component; the Active TCP
  remains unchanged. Recover clears the persistent component fault; the
  subsequent Enable re-synchronizes that Active TCP if the report differs.
  A controller offset changed in UFACTORY Studio is corrected at the next
  Enable. The accepted late-request residual is described in
  [TCPs](../docs/authoring/TCPS.md#the-controllers-tcp-offset). The `enable_only` mode
  does not send offsets.
- The ros2_control motion domain merges this status and routes the vendor
  primitives and the gate
  (`RetainedRos2ControlRuntimeHandle::InstallDeviceStatusAdapter`); CCLC keeps
  incidents, retained causes and provider loss.
- `xarm_device_adapter.hpp` (library `manyforge_xarm_control_device_adapter`)
  is the xArm implementation of ManyForge's generic vendor device adapter
  interface (`manyforge_roboplan/vendor_device_adapter.hpp`, header-only):
  `make_xarm_device_adapter` builds it for one Real component, and
  `xarm_vendor_adapter_family()` is the xArm family's row (`xarm`: that
  factory, the arm provider and its catalog, the gripper provider) in
  RoboPlan's vendor registry (`retained_vendor_device_status.hpp`). RoboPlan
  selects the adapter and the Real lifecycle catalogs through that row only.
- `lifecycle_profile.hpp` holds the Real arm's catalog
  (`manyforge.provider.ros2_control.xarm`): Prepare arms through the stock
  hardware activation (never over a vendor error), absorbs the plugin's
  reactivate-all, switches the joint state broadcaster back on alone
  (`ros2_control.broadcaster.activate`) when that reactivation left it off,
  and marks the arm prepared only on a broadcaster sample received after the
  activation; Enable is the trajectory-controller activation; Disable
  switches off the controller, then the hardware; Recover clears the error
  first, armed or not, then disarms and clears the warning. A Recover with
  the emergency stop still engaged thus fails with no effect
  (`xarm_error_persists_after_clear`, the vendor code).
- Every Real arm step (hardware, controller and vendor) runs between status
  queries: the controller manager's `list_controllers` and
  `list_hardware_components` and a `robot_states` report received after the
  query started (0.5 s). The list services, once discovered, are allowed
  the call time below; not discovered yet, the step is refused before effect
  (`ros2_control_state_query_service_unavailable`, no fault), like an
  undiscovered hardware-state or switch service. A refused step, or a clear
  that left the error, ends definite: the same vendor facts and the same
  hardware, controller and broadcaster states before and after are no
  effect, any difference an observed effect; a status query cut short by
  cancellation leaves it uncertain. Every failed step carries the queried
  status: `vendor_status` (the vendor codes, `C2 mode=0 state=4`) and
  `ros2_control_status` (hardware, trajectory controller and broadcaster).
- Every call a Real arm sends to its controller manager or robot (a switch,
  a hardware-state request, a status query, a vendor service) is allowed
  min(the shared component call cap, 10 s until measured live, the step's
  remaining time); a clear's verification min(the settle window plus 1 s,
  the remaining time). No answer within the time a call was allowed has a
  known result, the lost connection: `controller_manager_unresponsive` or
  `robot_controller_unresponsive`, the step ends at once as a known failure
  whose observed effect is the component fault it raises (Faulted, `fault`
  cause, that code as the diagnostic: an incident that blocks motion); the
  code is the transaction's result. No time left before sending refuses the
  step before dispatch (`component_lifecycle_deadline_expired`, nothing sent,
  no fault); a cancellation stays uncertain without a fault. The report wait
  is freshness (0.5 s), not a call. The check is made at each send (each list
  query, the hardware-state request, the switch, the vendor service), and the
  proof query after a step follows the same rule: with no time left it is not
  sent, and a refusal's effect stays unknown (uncertain, reconciled by CCLC). Each sent call logs one INFO line `manyforge_component_call`
  with its measured duration. Recover's
  `ros2_control.controller.recover` re-queries both; answered, the fault ends.
  Before a motion, tool-context and re-engage service callbacks use a fixed
  `kXArmServiceResponseTimeout` (10 s until measured live) for `set_tcp_load`,
  `set_tcp_offset`, `set_mode` and `set_state`, with an empty service
  cancellation token (Enable's zero-offset send follows its step instead,
  above).
  The pre-motion action is not a lifecycle step and has no lifecycle-step
  budget. A Stop during
  the callback is checked before motion dispatch; an in-flight service call
  finishes within its cap, and the callback may make a needed follow-up
  tool-context call before that check. An unanswered sent call, or a ready wait with no status report,
  returns `robot_controller_unresponsive` and raises the same fault. Answered
  failures keep their typed refusals
  (`tool_context_not_applied`, `xarm_reengage_not_confirmed`,
  `xarm_reengage_not_ready`).
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
services and the two tool context services against in-process servers;
`test_xarm_tool_context` covers when the tool context is sent.
