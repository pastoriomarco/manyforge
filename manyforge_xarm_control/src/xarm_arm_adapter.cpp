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

#include <manyforge_xarm_control/xarm_arm_adapter.hpp>
#include <manyforge_xarm_control/xarm_fault.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>


namespace manyforge::xarm_control {

using core::ActionOutcome;
using core::ActionResult;
using core::ComponentAction;
using core::ComponentLifecycle;

namespace {

/// After `set_tcp_offset` and `set_state(0)`, the robot reports joint-servo
/// mode and the ready state within this bound (5 Hz reports).
constexpr std::int64_t kXArmTcpOffsetReadyNs = 3'000'000'000LL;

/// The plugin's controller cycle after an arming: its deactivation and its
/// reactivation each block its write loop for up to its 1 s service timeout.
constexpr std::int64_t kXArmControllerCycleNs = 3'000'000'000LL;

[[nodiscard]] ActionResult result(const ComponentAction action, const ActionOutcome outcome,
                                  std::optional<core::ActionError> error = std::nullopt) {
  ActionResult made;
  made.action = action;
  made.outcome = outcome;
  made.error = std::move(error);
  return made;
}

[[nodiscard]] ActionResult failed(const ComponentAction action, std::string code,
                                  std::string message,
                                  std::optional<std::string> vendor_code = std::nullopt) {
  return result(action, ActionOutcome::kFailed,
                core::ActionError{std::move(code), std::move(message), std::move(vendor_code),
                                  std::nullopt});
}

[[nodiscard]] core::ErrorDTO offset_error(std::string reason, std::string message,
                                          const bool effect_possible) {
  return {std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
          std::move(message),
          {{"reason", std::move(reason)},
           {"effect", effect_possible ? "possible" : "not_started"}}};
}

/// One call's allowed time within a tool-context step: min(the call cap,
/// the step's remaining time); none left: not sent.
[[nodiscard]] std::optional<std::chrono::nanoseconds>
allowed_call_time(const std::optional<std::chrono::steady_clock::time_point> &step_deadline) {
  if (!step_deadline) return kXArmServiceResponseTimeout;
  const auto now = std::chrono::steady_clock::now();
  if (now >= *step_deadline) return std::nullopt;
  return std::min<std::chrono::nanoseconds>(
      kXArmServiceResponseTimeout,
      std::chrono::duration_cast<std::chrono::nanoseconds>(*step_deadline - now));
}

} // namespace

/// Marks an own operation (an action or a tool-context send) for its whole
/// duration: its mode and state changes are never external control.
class XArmArmAdapter::OwnOperation final {
public:
  explicit OwnOperation(XArmArmAdapter &adapter) : adapter_(adapter) {
    std::lock_guard lock(adapter_.mutex_);
    ++adapter_.own_operations_;
  }
  ~OwnOperation() {
    const auto now = adapter_.Now();
    std::lock_guard lock(adapter_.mutex_);
    --adapter_.own_operations_;
    adapter_.own_ended_at_ns_ = now;
    adapter_.own_settled_ = false;
  }
  OwnOperation(const OwnOperation &) = delete;
  OwnOperation &operator=(const OwnOperation &) = delete;

private:
  XArmArmAdapter &adapter_;
};

XArmArmAdapter::XArmArmAdapter(XArmArmAdapterConfig config, XArmArmAdapterPorts ports)
    : config_(config), ports_(std::move(ports)),
      tool_context_(
          [this](const core::Pose3DTO &tcp, const XArmToolContextBound &bound) {
            return SendTcpOffset(tcp, bound);
          },
          [this](const double mass_kg, const std::array<double, 3> &center_of_mass_m,
                 const XArmToolContextBound &bound) {
            const auto allowed = allowed_call_time(bound.step_deadline);
            if (!allowed)
              return XArmServiceCallResult{XArmServiceCallOutcome::kNotSent,
                                           "component_lifecycle_deadline_expired",
                                           std::nullopt};
            return ports_.driver->SetTcpLoad(mass_kg, center_of_mass_m, *allowed,
                                             bound.cancellation);
          },
          [this]() -> std::optional<std::array<double, 6>> {
            const auto observed = ports_.status->observe();
            if (!Fresh(observed, Now()) || observed.tcp_offset.size() != 6U)
              return std::nullopt;
            std::array<double, 6> offset{};
            std::copy(observed.tcp_offset.begin(), observed.tcp_offset.end(), offset.begin());
            return offset;
          }) {
  if (!ports_.status || !ports_.driver || !ports_.lane || !ports_.hardware ||
      !ports_.hardware_active || !ports_.pause || !ports_.measured_joints ||
      ports_.clock == nullptr)
    throw std::invalid_argument("the xArm arm adapter needs every port");
  if (config_.axis_count < 1 || config_.axis_count > 7)
    throw std::invalid_argument("an xArm-family robot has 1 to 7 axes");
}

void XArmArmAdapter::ObserveOwnerEstop() const {
  std::lock_guard lock(mutex_);
  if (armed_ && own_operations_ == 0)
    stopped_by_estop_.store(true, std::memory_order_release);
}

void XArmArmAdapter::ForgetOwnerEstop() noexcept {
  std::lock_guard lock(mutex_);
  stopped_by_estop_.store(false, std::memory_order_release);
}

std::int64_t XArmArmAdapter::Now() const { return ports_.clock->now(); }

bool XArmArmAdapter::Fresh(const XArmObservedState &observed, const std::int64_t now_ns) const {
  return observed.received_timestamp_ns &&
         now_ns - *observed.received_timestamp_ns <= kXArmStatusFreshnessNs;
}

bool XArmArmAdapter::ExternalControl(const XArmObservedState &observed,
                                     const std::int64_t now_ns) const {
  std::lock_guard lock(mutex_);
  // Only while ManyForge has the arm armed, outside its own operations and
  // once the last one settled.
  if (!armed_ || own_operations_ > 0 || !Fresh(observed, now_ns)) return false;
  if (observed.error_code != 0) {
    // A controller error explains the pair it reports (the owner's error
    // fault) and drops the arm out of its armed state (an e-stop
    // leaves it stopped with the motors off); its Enable re-arms it.
    armed_ = false;
    return false;
  }
  const bool changed =
      observed.mode != kXArmJointServoMode ||
      (observed.state != kXArmMovingState && observed.state != kXArmReadyState);
  if (!own_settled_) {
    // Settled by a report after it showing the armed pair, or by time.
    if (*observed.received_timestamp_ns > own_ended_at_ns_ && !changed)
      own_settled_ = true;
    else if (now_ns - own_ended_at_ns_ <= kXArmOwnOperationSettleNs)
      return false;
    else
      own_settled_ = true;
  }
  return changed;
}

void XArmArmAdapter::ReleaseExternalControl(const std::int64_t now_ns) {
  if (!ExternalControl(ports_.status->observe(), now_ns)) return;
  std::lock_guard lock(mutex_);
  armed_ = false;
}

core::ComponentState XArmArmAdapter::State(const std::int64_t now_ns) const {
  auto state = ports_.lane->State(now_ns);
  state.observed_at_ns = now_ns;
  // The lane's own process state (offline, process exited) comes first.
  if (state.lifecycle == ComponentLifecycle::kOffline ||
      (state.fault && state.fault->code == core::FaultCode::kProcessExited)) {
    // Its Prepare doesn't survive the process: the next one starts not
    // prepared.
    std::lock_guard lock(mutex_);
    prepared_ = false;
    return state;
  }
  {
    // The hardware off after its own Prepare, Recover or Disable (motors
    // off): prepared, Enable arms it.
    std::lock_guard lock(mutex_);
    if (prepared_ && state.lifecycle == ComponentLifecycle::kNotPrepared)
      state.lifecycle = ComponentLifecycle::kPrepared;
  }
  const auto observed = ports_.status->observe();
  if (state.fault && state.fault->code == core::FaultCode::kStreamStale) {
    // Read controller activity without the arm mutex. Serialize the final
    // flag/report check with lifecycle and owner-replacement invalidation.
    const bool inactive = ports_.controllers_inactive && ports_.controllers_inactive();
    std::lock_guard lock(mutex_);
    const auto current = ports_.status->observe();
    if (stopped_by_estop_.load(std::memory_order_acquire) && Fresh(current, now_ns)) {
      if (current.error_code >= 1 && current.error_code <= 3) {
        state.fault.reset();
        state.lifecycle = ComponentLifecycle::kPrepared;
        state.reason = core::ComponentStateReason{
            "stopped_by_estop", "Stopped by e-stop"};
      } else if (current.error_code == 0 && inactive) {
        state.fault.reset();
        state.lifecycle = ComponentLifecycle::kPrepared;
        state.reason = core::ComponentStateReason{
            "controllers_stopped_after_estop",
            "Controllers stopped after the e-stop. Enable the arm to resume"};
      }
    }
  }
  // All motors off: nothing can be executing.
  if (Fresh(observed, now_ns) && observed.motion_enable_mask >= 0 &&
      (observed.motion_enable_mask & ((1 << config_.axis_count) - 1)) == 0)
    state.activity = core::ComponentActivity::kIdle;
  return state;
}

std::vector<core::Capability> XArmArmAdapter::capabilities() const { return {}; }

ActionResult XArmArmAdapter::Run(const ComponentAction action, const std::stop_token stop) {
  OwnOperation own(*this);
  if (action == ComponentAction::kEnable || action == ComponentAction::kDisable ||
      action == ComponentAction::kRecover)
    ForgetOwnerEstop();
  switch (action) {
  case ComponentAction::kPrepare:
  case ComponentAction::kRecover: return Prepare(action, stop);
  case ComponentAction::kEnable: return Enable(stop);
  case ComponentAction::kDisable: return Disable(stop);
  case ComponentAction::kPowerOff:
  case ComponentAction::kReboot:
  case ComponentAction::kResetPosition:
  case ComponentAction::kSimReset: break;
  }
  return failed(action, "component_action_unsupported",
                "the xArm arm doesn't support this action; power actions are its machine's");
}

ActionResult XArmArmAdapter::Lane(const ComponentAction lane_action,
                                  const ComponentAction action, const std::stop_token stop) {
  auto made = ports_.lane->Run(lane_action, stop);
  made.action = action;
  std::lock_guard lock(mutex_);
  if (lane_action == ComponentAction::kEnable)
    armed_ = made.outcome == ActionOutcome::kSucceeded;
  else if (made.outcome != ActionOutcome::kFailed)
    armed_ = false;
  return made;
}

ActionResult XArmArmAdapter::Hardware(const bool active, const ComponentAction action,
                                      const std::stop_token stop) {
  auto made = ports_.hardware(active, stop);
  made.action = action;
  return made;
}

ActionResult XArmArmAdapter::Disable(const std::stop_token stop) {
  // The arm controller off, then the hardware off (the plugin
  // selects position mode).
  auto disabled = Lane(ComponentAction::kDisable, ComponentAction::kDisable, stop);
  if (disabled.outcome != ActionOutcome::kSucceeded) return disabled;
  auto off = Hardware(false, ComponentAction::kDisable, stop);
  if (off.outcome == ActionOutcome::kSucceeded) {
    std::lock_guard lock(mutex_);
    prepared_ = true;
  }
  return off;
}

ActionResult XArmArmAdapter::Prepare(const ComponentAction action, const std::stop_token stop) {
  // Motors off. The owner, prepared first, connected the driver (its
  // Recover, not its Prepare, also clears a latched error; never
  // `motion_enable`). The lane's prepare or recover leaves the
  // hardware as it is; it is switched off (at once when it already is, as on
  // a fresh process: the launch starts it inactive).
  const auto deadline = Now() + config_.action_timeout.count();
  auto made = Lane(action, action, stop);
  if (made.outcome != ActionOutcome::kSucceeded) return made;
  made = Hardware(false, action, stop);
  if (made.outcome != ActionOutcome::kSucceeded) return made;
  // The driver's status report (state, mode, errors), within the action's
  // time.
  while (!Fresh(ports_.status->observe(), Now())) {
    if (stop.stop_requested())
      return failed(action, "component_action_interrupted_by_stop",
                    "a Stop interrupted the wait for the robot controller's status report");
    if (Now() >= deadline)
      return failed(action, "xarm_status_unavailable",
                    "the robot controller's status report is missing or out of date");
    ports_.pause();
  }
  std::lock_guard lock(mutex_);
  prepared_ = true;
  return made;
}

bool XArmArmAdapter::NeedsArming() const {
  const auto observed = ports_.status->observe();
  const int axes = (1 << config_.axis_count) - 1;
  return Fresh(observed, Now()) &&
         (observed.mode != kXArmJointServoMode ||
          (observed.state != kXArmMovingState && observed.state != kXArmReadyState) ||
          observed.motion_enable_mask < 0 || (observed.motion_enable_mask & axes) != axes);
}

ActionResult XArmArmAdapter::Arm(const std::stop_token stop) {
  // As a Disable then Prepare: the arm controller off, the hardware off
  // (when on, so that its activation runs the arming again), then on: the
  // plugin's `clean_error`, `clean_warn`, `motion_enable`, `set_mode(1)`,
  // `set_state(0)`, with the joint state broadcaster.
  constexpr auto action = ComponentAction::kEnable;
  const auto deadline = Now() + config_.action_timeout.count();
  auto made = Lane(ComponentAction::kDisable, action, stop);
  if (made.outcome != ActionOutcome::kSucceeded) return made;
  made = Hardware(false, action, stop);
  if (made.outcome != ActionOutcome::kSucceeded) return made;
  made = Hardware(true, action, stop);
  if (made.outcome != ActionOutcome::kSucceeded) return made;
  const auto mark_armed = [this] {
    std::lock_guard lock(mutex_);
    armed_ = true;
    prepared_ = false;
  };
  mark_armed();
  const auto armed_at = Now();
  const auto interrupted = [&](const char *what) {
    return failed(action, "component_action_interrupted_by_stop",
                  std::string("a Stop interrupted the wait for ") + what + " after the arming");
  };
  // The plugin (`UFRobotSystemHardware::write`): until it sees the robot
  // ready it switches every active controller off from its write loop, and
  // switches them all back on once ready (the trajectory controller too).
  // No ManyForge switch may overlap that cycle: its reactivation's
  // `list_controllers` then times out and nothing is switched back on. So
  // first a report received after the arming that shows the robot ready.
  const int axes = (1 << config_.axis_count) - 1;
  std::int64_t ready_at = 0;
  for (;;) {
    const auto observed = ports_.status->observe();
    if (observed.received_timestamp_ns && *observed.received_timestamp_ns > armed_at &&
        observed.error_code == 0 && observed.mode == kXArmJointServoMode &&
        (observed.state == kXArmMovingState || observed.state == kXArmReadyState) &&
        observed.motion_enable_mask >= 0 && (observed.motion_enable_mask & axes) == axes) {
      ready_at = *observed.received_timestamp_ns;
      break;
    }
    if (stop.stop_requested()) return interrupted("the robot to report ready");
    if (Now() >= deadline)
      return failed(action, "xarm_not_ready",
                    "the robot did not report ready after its arming within the component's "
                    "lifecycle timeout",
                    "C" + std::to_string(observed.error_code) + " mode=" +
                        std::to_string(observed.mode) + " state=" +
                        std::to_string(observed.state));
    ports_.pause();
  }
  // Then joint states stamped after that report (the broadcaster on again
  // after the plugin's cycle, if it ran). Not back within the plugin's cycle
  // (its reactivation failed): the broadcaster is requested once.
  const auto flowing = [&] { return FlowingSince(ready_at); };
  bool requested = false;
  while (!flowing()) {
    if (stop.stop_requested()) return interrupted("joint states");
    if (Now() >= deadline)
      return failed(action, "xarm_joint_angles_unavailable",
                    "no joint states arrived after the arming within the component's "
                    "lifecycle timeout");
    if (!requested && Now() - ready_at >= kXArmControllerCycleNs) {
      requested = true;
      made = Hardware(true, action, stop);
      if (made.outcome != ActionOutcome::kSucceeded) return made;
    }
    ports_.pause();
  }
  // The plugin's reactivation switched the trajectory controller on too,
  // without an Enable: off again; the Enable switches it on.
  if (ports_.lane->State(Now()).lifecycle == ComponentLifecycle::kEnabled) {
    made = Lane(ComponentAction::kDisable, action, stop);
    if (made.outcome != ActionOutcome::kSucceeded) return made;
    mark_armed();
  }
  return made;
}

bool XArmArmAdapter::FlowingSince(const std::int64_t ready_at_ns) const {
  // Joint states stamped after `ready_at_ns`, and the controller manager's
  // activity showing the broadcaster active: the plugin's reactivation
  // (broadcaster and trajectory controller in one switch) has then been
  // reported, so the cached trajectory controller state is not the one from
  // before its cycle.
  const auto now = Now();
  const auto streams = ports_.lane->State(now).streams;
  const auto joints = std::find_if(streams.begin(), streams.end(), [](const auto &stream) {
    return stream.stream_id == "measured_joints";
  });
  return joints != streams.end() && joints->freshness == core::StreamFreshness::kFresh &&
         joints->age_ns && *joints->age_ns <= now - ready_at_ns &&
         ports_.measured_joints().has_value() &&
         (!ports_.broadcaster_active || ports_.broadcaster_active());
}

std::optional<ActionResult> XArmArmAdapter::EstopStillActive() const {
  // Enable's activation cleared the controller error; an e-stop error
  // (C1-C3) that stays means the e-stop or the control box's emergency
  // input is still active.
  const auto observed = ports_.status->observe();
  if (!Fresh(observed, Now()) || observed.error_code < 1 || observed.error_code > 3)
    return std::nullopt;
  return failed(ComponentAction::kEnable, "robot_estop_active",
                "The e-stop is still active (" + std::string(xarm_error_text(observed.error_code)) +
                    "): release it, then Enable.",
                xarm_vendor_code(observed.error_code));
}

ActionResult XArmArmAdapter::Enable(const std::stop_token stop) {
  std::optional<std::int64_t> rearmed_at;
  // The motors are switched on here, never at Prepare.
  if (!ports_.hardware_active() || NeedsArming()) {
    auto armed = Arm(stop);
    if (armed.outcome != ActionOutcome::kSucceeded) {
      if (auto estop = EstopStillActive()) return *estop;
      return armed;
    }
    rearmed_at = Now();
  }
  if (auto refused = CheckEnableGate(rearmed_at, stop)) return *refused;
  return Lane(ComponentAction::kEnable, ComponentAction::kEnable, stop);
}

std::optional<ActionResult>
XArmArmAdapter::CheckEnableGate(const std::optional<std::int64_t> since_ns,
                                const std::stop_token stop) {
  const auto refuse = [](std::string code, std::string message,
                         std::optional<std::string> vendor = std::nullopt) {
    return std::optional<ActionResult>(
        failed(ComponentAction::kEnable, std::move(code), std::move(message), std::move(vendor)));
  };
  auto observed = ports_.status->observe();
  if (since_ns) {
    // After a re-arm, only a report received after it shows its result.
    const auto started = Now();
    while (!(observed.received_timestamp_ns && *observed.received_timestamp_ns > *since_ns) &&
           !stop.stop_requested() && Now() - started < kXArmStatusFreshnessNs) {
      ports_.pause();
      observed = ports_.status->observe();
    }
    if (!(observed.received_timestamp_ns && *observed.received_timestamp_ns > *since_ns))
      return refuse("xarm_status_unavailable",
                    "the robot controller sent no status report after its re-arm");
  }
  if (!Fresh(observed, Now()))
    return refuse("xarm_status_unavailable",
                  "the robot controller's status report is missing or out of date");
  if (auto estop = EstopStillActive()) return *estop;
  const int axes = (1 << config_.axis_count) - 1;
  if (observed.error_code != 0 || observed.mode != kXArmJointServoMode ||
      (observed.state != kXArmMovingState && observed.state != kXArmReadyState) ||
      observed.motion_enable_mask < 0 || (observed.motion_enable_mask & axes) != axes)
    return refuse("xarm_not_ready",
                  "the robot is not ready for commands (error, mode, state or motors)",
                  "C" + std::to_string(observed.error_code) + " mode=" +
                      std::to_string(observed.mode) + " state=" +
                      std::to_string(observed.state));
  const auto measured = ports_.measured_joints();
  const auto count = static_cast<std::size_t>(config_.axis_count);
  if (!measured || measured->size() != count || observed.joint_angles_rad.size() < count)
    return refuse("xarm_joint_angles_unavailable",
                  "the measured or reported joint angles are missing");
  for (std::size_t axis = 0U; axis < count; ++axis) {
    const auto reported = observed.joint_angles_rad[axis];
    const auto actual = (*measured)[axis];
    if (!std::isfinite(reported) || !std::isfinite(actual))
      return refuse("xarm_joint_angles_unavailable",
                    "a measured or reported joint angle is not finite");
    if (std::abs(actual - reported) > config_.joint_agreement_tolerance_rad)
      return refuse("xarm_measured_joints_disagree",
                    "measured joint " + std::to_string(axis + 1U) +
                        " differs from the angle the robot controller reports");
  }
  return std::nullopt;
}

XArmJointSampleUse XArmArmAdapter::JointSampleUse(const std::int64_t now_ns,
                                                  const std::vector<double> &positions) const {
  if (positions.empty() ||
      std::all_of(positions.begin(), positions.end(),
                  [](const double value) { return value == 0.0; }) ||
      !std::all_of(positions.begin(), positions.end(),
                   [](const double value) { return std::isfinite(value); }))
    return XArmJointSampleUse::kIgnore;
  // With the hardware off (prepared, motors off) the driver's own
  // topic carries the true pose, e-stop included; the broadcaster isn't relied on.
  if (!ports_.hardware_active()) return XArmJointSampleUse::kLeads;
  const auto observed = ports_.status->observe();
  std::lock_guard lock(mutex_);
  return rearms_ > 0 && Fresh(observed, now_ns) && observed.error_code == 0
             ? XArmJointSampleUse::kLeads
             : XArmJointSampleUse::kDisplay;
}

core::Result<bool>
XArmArmAdapter::ApplyToolContext(const std::optional<core::Pose3DTO> &offset,
                                 const std::optional<core::ToolContextPointMassDTO> &payload,
                                 const bool at_enable, const XArmToolContextBound &bound) {
  OwnOperation own(*this);
  {
    std::lock_guard lock(mutex_);
    ++rearms_;
  }
  auto applied = tool_context_.Apply(offset, payload, at_enable, bound);
  std::optional<std::int64_t> ready_at;
  {
    std::lock_guard lock(mutex_);
    --rearms_;
    ready_at = offset_ready_at_ns_;
  }
  if (!at_enable || !applied.ok() || !applied.value() || !ready_at) return applied;
  // At Enable the host switches the trajectory controller on right after
  // this hook. The offset's state 5 and the ready after `set_state(0)` made
  // the plugin cycle every controller off and on from its write loop (as
  // after Arm's arming): no switch may overlap that cycle (its
  // reactivation's `list_controllers` then times out and nothing comes back
  // on). So the hook returns only once joint states stamped after the ready
  // report flow and the broadcaster is reported active, within the plugin's
  // cycle and the step's time.
  for (;;) {
    if (FlowingSince(*ready_at)) return applied;
    const auto not_back = [&](std::string code, std::string message) {
      auto error = offset_error("xarm_reengage_not_ready", std::move(message), true);
      error.code = std::move(code);
      error.details.insert_or_assign("effect", "observed");
      return core::Result<bool>::Err(std::move(error));
    };
    if (bound.cancellation.stop_requested())
      return not_back("component_action_interrupted_by_stop",
                      "a Stop interrupted the wait for joint states after the TCP offset");
    if (bound.step_deadline && std::chrono::steady_clock::now() >= *bound.step_deadline)
      return not_back("component_lifecycle_deadline_expired",
                      "no time was left to see joint states after the TCP offset");
    if (Now() - *ready_at >= kXArmControllerCycleNs)
      return not_back(std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
                      "the driver did not bring the joint state broadcaster back after the "
                      "TCP offset; Recover the robot");
    ports_.pause();
  }
}

XArmTcpOffsetSent XArmArmAdapter::SendTcpOffset(const core::Pose3DTO &tcp,
                                                const XArmToolContextBound &bound) {
  // From ready, `set_tcp_offset` applies but the controller drops to state 5
  // and stays there; `set_state(0)` restores ready (live 2026-10-28). Each
  // call once; then a fresh ready report, which also shows the offset.
  const auto send = [&](const auto &call) -> XArmServiceCallResult {
    const auto allowed = allowed_call_time(bound.step_deadline);
    if (!allowed)
      return {XArmServiceCallOutcome::kNotSent, "component_lifecycle_deadline_expired",
              std::nullopt};
    return call(*allowed);
  };
  const auto unconfirmed = [](const XArmServiceCallResult &call, std::string reason,
                              std::string message, const bool effect_possible) {
    auto error = offset_error(std::move(reason), std::move(message), effect_possible);
    if (call.outcome != XArmServiceCallOutcome::kNotSent && XArmCallUnanswered(call.reason))
      error.code = std::string(kRobotControllerUnresponsiveCode);
    if (call.reason == "component_lifecycle_deadline_expired")
      error.code = "component_lifecycle_deadline_expired";
    error.details.emplace("vendor_reason", call.reason);
    if (call.vendor_return)
      error.details.emplace("vendor_return", std::to_string(*call.vendor_return));
    return XArmTcpOffsetSent{false, std::move(error)};
  };
  const auto offset = send([&](const std::chrono::nanoseconds allowed) {
    return ports_.driver->SetTcpOffset(tcp, allowed, bound.cancellation);
  });
  if (offset.outcome != XArmServiceCallOutcome::kSucceeded) {
    auto sent = unconfirmed(offset, "tool_context_not_applied",
                            "the robot did not confirm its tcp_offset (" + offset.reason + ")",
                            offset.outcome != XArmServiceCallOutcome::kNotSent);
    sent.error->details.emplace("tool_context_field", "tcp_offset");
    return sent;
  }
  const auto ready = send([&](const std::chrono::nanoseconds allowed) {
    return ports_.driver->SetState(0, allowed, bound.cancellation);
  });
  if (ready.outcome != XArmServiceCallOutcome::kSucceeded) {
    auto sent = unconfirmed(ready, "xarm_reengage_not_confirmed",
                            "the robot's set_state after its tcp_offset was not confirmed", true);
    sent.error->details.emplace("step", "set_state");
    return sent;
  }
  const auto since = Now();
  {
    std::lock_guard lock(mutex_);
    offset_ready_at_ns_.reset();
  }
  const int axes = (1 << config_.axis_count) - 1;
  for (;;) {
    const auto observed = ports_.status->observe();
    const auto now = Now();
    if (Fresh(observed, now) && *observed.received_timestamp_ns > since &&
        observed.error_code == 0 && observed.mode == kXArmJointServoMode &&
        observed.state == kXArmReadyState && observed.motion_enable_mask >= 0 &&
        (observed.motion_enable_mask & axes) == axes) {
      std::optional<std::array<double, 6>> reported;
      if (observed.tcp_offset.size() == 6U) {
        reported.emplace();
        std::copy(observed.tcp_offset.begin(), observed.tcp_offset.end(), reported->begin());
      }
      {
        std::lock_guard lock(mutex_);
        offset_ready_at_ns_ = *observed.received_timestamp_ns;
      }
      return {true, std::nullopt, reported};
    }
    if (bound.step_deadline && std::chrono::steady_clock::now() >= *bound.step_deadline) {
      auto error = offset_error("xarm_reengage_not_ready",
                                "no time was left to see the robot ready after its tcp_offset",
                                true);
      error.code = "component_lifecycle_deadline_expired";
      return {false, std::move(error)};
    }
    if (bound.cancellation.stop_requested() || now - since >= kXArmTcpOffsetReadyNs) {
      auto error = offset_error("xarm_reengage_not_ready",
                                "the robot did not report joint-servo mode and the ready state "
                                "in time after its tcp_offset",
                                true);
      if (!bound.cancellation.stop_requested() &&
          !(observed.received_timestamp_ns && *observed.received_timestamp_ns > since)) {
        error.code = std::string(kRobotControllerUnresponsiveCode);
        error.message = "the robot controller sent no status report in time";
      }
      return {false, std::move(error)};
    }
    ports_.pause();
  }
}

} // namespace manyforge::xarm_control
