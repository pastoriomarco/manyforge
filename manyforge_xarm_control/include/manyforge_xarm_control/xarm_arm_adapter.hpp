// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/tool_context.hpp>
#include <manyforge_xarm_control/contract.hpp>
#include <manyforge_xarm_control/xarm_tool_context.hpp>

namespace manyforge::xarm_control {

struct XArmArmAdapterConfig final {
  /// The arm's joint count: `mt_able` bits of its motors.
  int axis_count{6};
  /// The Enable gate's measured-versus-reported joint tolerance
  /// (`lanes.real.joint_agreement_tolerance_rad`).
  double joint_agreement_tolerance_rad{kXArmJointAgreementToleranceRad};
  /// The component's lifecycle command timeout (`lifecycle_command_timeout_ns`):
  /// bounds the wait for a status report at Prepare and Recover, and for
  /// joint states after Enable's arming.
  std::chrono::nanoseconds action_timeout{};
};

/// What the arm adapter runs on: its owner's report and driver services,
/// and the host's ros2_control connection.
struct XArmArmAdapterPorts final {
  /// The owner's `robot_states`.
  std::shared_ptr<const IXArmStatusSource> status;
  /// The owner's driver services (tool context only).
  std::shared_ptr<const IXArmDriver> driver;
  /// The ros2_control lane adapter of the arm's connection: its prepare
  /// waits for the loaded arm controller, its recover switches it off (both
  /// leave the hardware as it is); its enable and disable switch the arm
  /// controller.
  std::shared_ptr<core::IComponentAdapter> lane;
  /// Switches the arm's ros2_control hardware component on or off, with the
  /// arm controller off; succeeds at once when it already is. On activation
  /// the vendor plugin arms the robot (`clean_error`, `clean_warn`,
  /// `motion_enable`, `set_mode(1)`, `set_state(0)`), and the joint state
  /// broadcaster is switched on with it; on deactivation it selects position
  /// mode. `step_deadline`: Enable's lifecycle deadline, which bounds every
  /// controller-manager call of the switch (none: each call's own cap).
  std::function<core::ActionResult(
      bool active, std::optional<std::chrono::steady_clock::time_point> step_deadline,
      std::stop_token)>
      hardware;
  /// The controller manager reports the arm's hardware component active.
  std::function<bool()> hardware_active;
  /// Between reads of a wait (production: 20 ms).
  std::function<void()> pause;
  /// The joint state broadcaster's latest fresh positions in axis order
  /// (the lane's measured joints); none without a fresh sample.
  std::function<std::optional<std::vector<double>>()> measured_joints;
  core::IClock *clock{};
  /// The controller manager's latest activity shows the joint state
  /// broadcaster active (none: not checked).
  std::function<bool()> broadcaster_active{};
  /// The observed controller-manager activity explicitly shows both the
  /// trajectory controller and broadcaster loaded and inactive.
  std::function<bool()> controllers_inactive{};
  /// The controller manager's latest activity shows the trajectory
  /// controller or the joint state broadcaster active (none: the arm never
  /// switches them off on lost readiness).
  std::function<bool()> controllers_active{};
  /// Sends one best-effort `switch_controller` deactivating the trajectory
  /// controller and the joint state broadcaster, without waiting; `answered`
  /// runs once the controller manager answers. False when not sent.
  std::function<bool(std::function<void()> answered)> deactivate_controllers{};
};

/// The arm member of an xArm machine (xArm 5/6/7, Lite 6,
/// UF850). It runs around the ros2_control lane adapter and reads the
/// controller's report through its owner; the controller's error register,
/// `clean_error` and the power actions are the owner's.
///
/// The motors are switched on only by Enable.
/// - `prepare` / `recover`: the lane's prepare / recover (the arm controller
///   loaded and off), then the hardware off (already off on a fresh
///   process), then a fresh status report within the lifecycle timeout. The
///   owner, prepared first, connected the driver and cleared
///   the controller's error and warning; nothing calls `motion_enable`.
///   While prepared the driver's own joint topic is the measured stream
///   (`JointSampleUse`).
/// - `enable`: with the hardware off or the robot not ready for commands,
///   the arming: the arm controller off, the hardware off, the hardware on
///   (the plugin's arming sequence, with the joint state broadcaster), then
///   joint states stamped after it; then the Enable gate, then the lane's
///   enable.
/// - `disable`: the lane's disable, then the hardware off.
/// - Lost readiness (`OnReport`): the vendor plugin's own controller
///   switching is answered without effect (`XArmPluginControllers`), so the
///   arm switches the trajectory controller and the joint state broadcaster
///   off itself when a report shows the robot not ready (an error, an
///   e-stop, a mode or state change) with the hardware on, outside its own
///   arming. Only Enable switches them on again.
///
/// State: the lane's; with the hardware off after its own Prepare, Recover
/// or Disable it is `prepared` (Enable arms); during its own arming the
/// lane's `fault_stream_stale` is `prepared` with reason `xarm_arming` (the
/// streams still show stale); and all motors off
/// (`mt_able == 0`) is `idle`. A mode or state change from outside while ManyForge has the arm
/// armed (`ExternalControl`) is the owner's fault. Trust the controller: no
/// retries and no verification loops; each vendor call is sent once.
/// What a sample of the driver's own `<driver ns>/joint_states` is good for.
enum class XArmJointSampleUse : std::uint8_t {
  /// Not a pose: all zero (the controller's e-stop signature, and the
  /// plugin's zero fallback) or not finite.
  kIgnore,
  /// The display only.
  kDisplay,
  /// Also the measured stream: while the hardware is off (Prepared, motors
  /// off: the driver publishes the true pose, e-stop included), and during
  /// ManyForge's own re-arm of a TCP offset send on an error-free report.
  kLeads,
};

class XArmArmAdapter final : public core::IComponentAdapter {
public:
  XArmArmAdapter(XArmArmAdapterConfig config, XArmArmAdapterPorts ports);

  /// Record an owner-reported emergency stop only while this arm is armed.
  void ObserveOwnerEstop() const;
  /// The installed transport owner was replaced; prior presentation is stale.
  void ForgetOwnerEstop() noexcept;
  /// Its own arming runs (Enable's `Arm`, a TCP offset's re-arm): its
  /// hardware switch pauses the machine's streams meanwhile, which is not a
  /// fault (the action verifies its streams before it succeeds).
  [[nodiscard]] bool Arming() const;

  /// Each received report (the `robot_states` callback; never blocks): one
  /// not ready for commands as the plugin reads it (an error, a state above
  /// ready, out of joint-servo mode), with the hardware on, the trajectory
  /// controller or the joint state broadcaster active and no own arming
  /// running, sends their deactivation (`deactivate_controllers`), unless
  /// an earlier one is unanswered.
  void OnReport(const XArmObservedState &report);

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

  /// The vendor joint-sample gate.
  [[nodiscard]] XArmJointSampleUse JointSampleUse(std::int64_t now_ns,
                                                  const std::vector<double> &positions) const;

  /// The rule: while ManyForge has the arm armed, outside its own
  /// operations and once the last one settled (`kXArmOwnOperationSettleNs`),
  /// a fresh error-free report out of joint-servo mode or out of the moving
  /// and ready states (paused 3, stopped 4, not ready 5, stop requested 6) is
  /// external control (UFactory Studio, the pendant). The owner reports it.
  /// A report with a controller error explains its pair and disarms the arm
  /// (its Enable re-arms it).
  [[nodiscard]] bool ExternalControl(const XArmObservedState &observed,
                                     std::int64_t now_ns) const;
  /// The owner's Recover or Prepare: an arm under external control is no
  /// longer counted armed; its next Enable re-arms it.
  void ReleaseExternalControl(std::int64_t now_ns);

  /// Active TCP (event driven): the TCP offset (with the re-arm the
  /// controller needs after it and the post-send check) and the total
  /// payload; see `XArmToolContextSender`. True when an offset was sent and
  /// the robot re-armed. The offset's own state 5 is never external control
  /// and doesn't switch the controllers off (an own arming).
  [[nodiscard]] core::Result<bool>
  ApplyToolContext(const std::optional<core::Pose3DTO> &offset,
                   const std::optional<core::ToolContextPointMassDTO> &payload, bool at_enable,
                   const XArmToolContextBound &bound = {});

private:
  class OwnOperation;
  class ArmingScope;

  /// Prepare and Recover: motors off (see the class comment).
  [[nodiscard]] core::ActionResult Prepare(core::ComponentAction action, std::stop_token stop);
  [[nodiscard]] core::ActionResult Enable(std::stop_token stop);
  /// A fresh report shows the robot not ready for commands (mode, state or
  /// motors).
  [[nodiscard]] bool NeedsArming() const;
  /// Enable's arming: the arm controller off, the hardware off (when on),
  /// the hardware on (the plugin's arming sequence and the joint state
  /// broadcaster). Then a report after the arming showing the robot ready
  /// (else `xarm_not_ready`), then a broadcaster sample stamped after that
  /// report (the lane's `measured_joints` stream) with the measured joints
  /// there (else `xarm_joint_angles_unavailable`). All within
  /// `action_timeout` from its start.
  [[nodiscard]] core::ActionResult Arm(std::stop_token stop);
  /// Joint states stamped after a ready report at `ready_at_ns` (the plugin
  /// freezes its joint states while an error is present, so older ones may
  /// be the pose from before the arming), and the broadcaster reported
  /// active.
  [[nodiscard]] bool FlowingSince(std::int64_t ready_at_ns) const;
  /// The Enable gate: a fresh report (received after `since_ns` when given,
  /// waiting at most one freshness window) that accepts commands, and every
  /// measured joint within the tolerance of the reported angle. A trajectory
  /// controller activated on stale or zero joints would drive there.
  /// Enable's typed refusal when an e-stop error (C1-C3) stays after its
  /// activation cleared: the e-stop is still pressed.
  [[nodiscard]] std::optional<core::ActionResult> EstopStillActive() const;
  [[nodiscard]] std::optional<core::ActionResult> CheckEnableGate(
      std::optional<std::int64_t> since_ns, std::stop_token stop);
  /// The lane's action, reported as `action`; a successful enable arms the
  /// arm, any other success disarms it.
  [[nodiscard]] core::ActionResult Lane(core::ComponentAction lane_action,
                                        core::ComponentAction action, std::stop_token stop);
  [[nodiscard]] XArmTcpOffsetSent SendTcpOffset(const core::Pose3DTO &tcp,
                                                const XArmToolContextBound &bound);

  /// Disable: the lane's disable, then the hardware off.
  [[nodiscard]] core::ActionResult Disable(std::stop_token stop);
  /// The hardware on or off, reported as `action`, within `step_deadline`
  /// (none: each controller-manager call's own cap).
  [[nodiscard]] core::ActionResult
  Hardware(bool active, core::ComponentAction action,
           std::optional<std::chrono::steady_clock::time_point> step_deadline,
           std::stop_token stop);

  [[nodiscard]] bool Fresh(const XArmObservedState &observed, std::int64_t now_ns) const;
  [[nodiscard]] std::int64_t Now() const;

  const XArmArmAdapterConfig config_;
  const XArmArmAdapterPorts ports_;
  XArmToolContextSender tool_context_;
  mutable std::mutex mutex_;
  /// ManyForge armed the arm (Enable's hardware activation or a successful
  /// enable; cleared by prepare, recover, disable, a controller error or the
  /// owner's release).
  mutable bool armed_{false};
  mutable std::atomic<bool> stopped_by_estop_{false};
  /// Its own Prepare, Recover or Disable left the hardware off (cleared by
  /// Enable's arming, and when the lane reports its process offline or
  /// exited: a fresh process starts not prepared).
  mutable bool prepared_{false};
  /// Own operations in progress, and when the last one ended: their own
  /// mode and state changes are never external control.
  int own_operations_{0};
  std::int64_t own_ended_at_ns_{0};
  mutable bool own_settled_{true};
  /// A TCP offset send (with its re-arm) in progress.
  int rearms_{0};
  /// Its own armings in progress (`Arm`, `ApplyToolContext`): a lane
  /// `fault_stream_stale` meanwhile is reported as `prepared`, reason
  /// `xarm_arming`, and a not-ready report doesn't switch the controllers
  /// off. Atomic: the report callback reads it without the mutex.
  std::atomic<int> arming_{0};
  /// A deactivation was sent and is not answered yet (shared with its
  /// response callback, which may outlive the adapter).
  const std::shared_ptr<std::atomic<bool>> deactivation_pending_ =
      std::make_shared<std::atomic<bool>>(false);
};

} // namespace manyforge::xarm_control
