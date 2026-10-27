// Copyright 2026 Flexin Group SRL
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

#include <manyforge_xarm_control/xarm_brand_lifecycle_port.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::xarm_control {
namespace {

using core::LifecycleBlockingCauseClassDTO;
using core::LifecycleFactDTO;
using core::LifecycleFactId;
using core::LifecycleFactValueDTO;
using core::LifecycleFactValueKindDTO;

// RobotMsg mode and state values (xarm_msgs/msg/RobotMsg.msg).
constexpr int kPositionMode = 0;
constexpr int kJointServoMode = 1;
constexpr int kMovingFeedbackState = 1;
constexpr int kReadyFeedbackState = 2;
constexpr int kStoppedFeedbackState = 4;

// An own primitive explains vendor mode/state changes for this long after it
// returned, unless a report showed its final expected pair earlier.
constexpr std::int64_t kOwnPrimitiveSettleNs = 5'000'000'000LL;

[[nodiscard]] LifecycleFactValueDTO unknown_value() {
  return {LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt,
          std::nullopt};
}

[[nodiscard]] LifecycleFactValueDTO boolean_value(const bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
          std::nullopt};
}

[[nodiscard]] LifecycleFactValueDTO token_value(std::string value) {
  return {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
          std::move(value)};
}

[[nodiscard]] LifecycleFactValueDTO integer_value(const std::int64_t value) {
  return {LifecycleFactValueKindDTO::kInteger, std::nullopt, value,
          std::nullopt};
}

[[nodiscard]] LifecycleFactDTO fact(const std::string_view id,
                                    LifecycleFactValueDTO value) {
  return {LifecycleFactId(std::string(id)), std::move(value)};
}

// UFactory controller error codes (addendum 2 §9; R7 hardware verification
// pending): C1 emergency-stop button, C2 control-box emergency IO, C3 enabling
// switch; C10-C17 servo faults that need a power cycle; C31 collision.
[[nodiscard]] bool emergency_stop_error(const int error_code) {
  return error_code >= 1 && error_code <= 3;
}

[[nodiscard]] bool power_cycle_error(const int error_code) {
  return error_code >= 10 && error_code <= 17;
}

constexpr int kCollisionErrorCode = 31;

[[nodiscard]] bool fresh_report(const XArmObservedState &observed,
                                const std::int64_t now_ns,
                                const std::int64_t freshness_ns) {
  return observed.received_timestamp_ns.has_value() &&
         *observed.received_timestamp_ns <= now_ns &&
         now_ns - *observed.received_timestamp_ns <= freshness_ns;
}

// Conservative vendor-state classification of one fresh report.
//
// A non-zero controller error code is classified by its documented meaning;
// every other code is `fault`. The protective stop has no documented code and
// is never inferred. An unrequested mode/state change (see `TrackVendorControl`)
// is `external_control`; a report whose status fields are not established is
// `unknown`. Warnings are reported as facts and codes, not as a cause: they do
// not stop the robot, and the vendor activation clears them (R7 §3.4).
[[nodiscard]] LifecycleBlockingCauseClassDTO
vendor_cause(const XArmObservedState &observed,
             const bool external_control_detected) {
  using Cause = LifecycleBlockingCauseClassDTO;
  if (observed.error_code != 0) {
    if (emergency_stop_error(observed.error_code)) {
      return Cause::kEmergencyStop;
    }
    if (observed.error_code == kCollisionErrorCode) {
      return Cause::kCollision;
    }
    return Cause::kFault;
  }
  if (external_control_detected) {
    return Cause::kExternalControl;
  }
  if (observed.mode < 0 || observed.state < 0 || observed.warning_code < 0 ||
      observed.motion_enable_mask < 0) {
    return Cause::kUnknown;
  }
  return Cause::kNone;
}

// Mode/state pairs equivalent for unrequested-change detection: in joint-servo
// mode the vendor alternates between moving and ready while ManyForge's own
// ros2_control stream executes.
[[nodiscard]] bool equivalent_pair(const int left_mode, const int left_state,
                                   const int right_mode,
                                   const int right_state) {
  if (left_mode != right_mode) {
    return false;
  }
  if (left_state == right_state) {
    return true;
  }
  const auto servo_running = [](const int state) {
    return state == kMovingFeedbackState || state == kReadyFeedbackState;
  };
  return left_mode == kJointServoMode && servo_running(left_state) &&
         servo_running(right_state);
}

// True when `command`, dispatched from (base_mode, base_state), may by
// itself have produced the reported (mode, state), including the vendor's
// intermediate states. Deliberately permissive: an own effect is never an
// incident, while modes ManyForge never selects are never explained.
[[nodiscard]] bool own_effect_explains(const XArmLifecycleCommand command,
                                       const int base_mode,
                                       const int base_state, const int mode,
                                       const int state) {
  const bool base_or_position_mode =
      mode == base_mode || mode == kPositionMode;
  switch (command) {
  case XArmLifecycleCommand::kClearError:
  case XArmLifecycleCommand::kClearWarning:
    return base_or_position_mode &&
           (state == base_state || state == kStoppedFeedbackState);
  case XArmLifecycleCommand::kArm:
    // The plugin's `on_activate`: clear, motion enable, servo mode, ready;
    // every intermediate state of that sequence (R7 §3.2).
    return base_or_position_mode || mode == kJointServoMode;
  case XArmLifecycleCommand::kDisarm:
    // The plugin's `on_deactivate` selects position mode in any state.
    return base_or_position_mode;
  }
  return false;
}

// True when the reported pair is the command's final expected postcondition.
[[nodiscard]] bool own_effect_completed(const XArmLifecycleCommand command,
                                        const int mode, const int state) {
  switch (command) {
  case XArmLifecycleCommand::kArm:
    return mode == kJointServoMode && (state == kMovingFeedbackState ||
                                       state == kReadyFeedbackState);
  case XArmLifecycleCommand::kDisarm:
    return mode == kPositionMode;
  case XArmLifecycleCommand::kClearError:
  case XArmLifecycleCommand::kClearWarning:
    break;
  }
  return state == kStoppedFeedbackState;
}

// The D9 device facts of one fresh report, and its vendor codes.
[[nodiscard]] std::vector<LifecycleFactDTO>
fresh_facts(const XArmObservedState &observed, const int axis_mask) {
  const bool error_present = observed.error_code != 0;
  const bool motion_enabled = observed.motion_enable_mask >= 0 &&
                              (observed.motion_enable_mask & axis_mask) ==
                                  axis_mask;
  // The vendor plugin's own readiness (err 0, state <= 2, joint-servo mode)
  // with every axis enabled; state 0 never occurs in a report.
  const bool accepts_commands =
      !error_present && motion_enabled && observed.mode == kJointServoMode &&
      (observed.state == kMovingFeedbackState ||
       observed.state == kReadyFeedbackState);
  std::vector<LifecycleFactDTO> facts{
      fact(core::kDeviceAcceptsCommandsFactId, boolean_value(accepts_commands)),
      fact(core::kDeviceErrorPresentFactId, boolean_value(error_present)),
      fact(core::kDeviceMotionEnabledFactId,
           observed.motion_enable_mask < 0 ? unknown_value()
                                           : boolean_value(motion_enabled)),
      fact(core::kDeviceWarningPresentFactId,
           observed.warning_code < 0
               ? unknown_value()
               : boolean_value(observed.warning_code != 0)),
  };
  if (error_present) {
    facts.push_back(fact(kVendorCodeErrorFactId,
                         token_value("C" + std::to_string(observed.error_code))));
  }
  if (power_cycle_error(observed.error_code)) {
    facts.push_back(fact(kVendorCodePowerCycleFactId,
                         token_value(std::string(kPowerCycleRequiredToken))));
  }
  if (observed.warning_code > 0) {
    facts.push_back(
        fact(kVendorCodeWarningFactId, integer_value(observed.warning_code)));
  }
  if (observed.mode >= 0) {
    facts.push_back(fact(kVendorCodeModeFactId, integer_value(observed.mode)));
  }
  if (observed.state >= 0) {
    facts.push_back(
        fact(kVendorCodeStateFactId, integer_value(observed.state)));
  }
  std::sort(facts.begin(), facts.end(), [](const auto &left, const auto &right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  });
  return facts;
}

[[nodiscard]] std::vector<LifecycleFactDTO> unavailable_facts() {
  return {fact(core::kDeviceAcceptsCommandsFactId, unknown_value()),
          fact(core::kDeviceErrorPresentFactId, unknown_value()),
          fact(core::kDeviceMotionEnabledFactId, unknown_value()),
          fact(core::kDeviceWarningPresentFactId, unknown_value())};
}

} // namespace

XArmBrandLifecyclePort::XArmBrandLifecyclePort(
    std::shared_ptr<const IXArmBrandLifecycleTransport> transport,
    const XArmBrandLifecyclePortConfig config)
    : transport_(std::move(transport)), config_(config) {
  if (transport_ == nullptr) {
    throw std::invalid_argument("xArm vendor status transport is missing");
  }
  if (config_.status_freshness_ns <= 0 || config_.axis_count < 1 ||
      config_.axis_count > 7 ||
      !std::isfinite(config_.joint_agreement_tolerance_rad) ||
      config_.joint_agreement_tolerance_rad <= 0.0) {
    throw std::invalid_argument("invalid xArm vendor adapter data");
  }
}

// Unrequested mode/state change detection (addendum 2 §7). ManyForge knows
// the changes it causes: each own primitive carries an expected vendor
// effect. With a fresh error-free report, a mode/state change that neither
// the last explained pair nor the in-flight or just-completed own primitive
// explains is `external_control` (for example a UFactory Studio jog leaving
// mode 0 / state 5, teach mode 2, or a stop that ManyForge did not request).
// It stays reported, also across report loss, until ManyForge dispatches its
// next own primitive, which it does only after the operator's acknowledgement;
// that dispatch re-baselines from the latest fresh report. Detection of real
// Studio jogs and of these vendor sequences is verified on hardware in R7.
void XArmBrandLifecyclePort::TrackVendorControl(
    const XArmObservedState &observed, const std::int64_t now_ns,
    VendorTrackingState &tracking) {
  tracking.last_reported_error_code = observed.error_code;
  if (observed.mode < 0 || observed.state < 0) {
    return;
  }
  const VendorControlPair reported{observed.mode, observed.state};
  tracking.last_reported_pair = reported;
  if (tracking.own_expectation.has_value()) {
    if (tracking.own_expectation->completed_at_ns.has_value() &&
        now_ns - *tracking.own_expectation->completed_at_ns >
            kOwnPrimitiveSettleNs) {
      tracking.own_expectation.reset();
    } else if (tracking.own_expectation->base.mode < 0 ||
               tracking.own_expectation->base.state < 0) {
      tracking.own_expectation->base =
          tracking.explained_pair.value_or(reported);
    }
  }
  // A vendor error explains the pair it reports; its code classifies it.
  if (observed.error_code != 0) {
    tracking.explained_pair = reported;
    return;
  }
  // ManyForge only ever selects position or joint-servo mode.
  if (reported.mode != kPositionMode && reported.mode != kJointServoMode) {
    tracking.external_control_detected = true;
    return;
  }
  if (!tracking.explained_pair.has_value()) {
    tracking.explained_pair = reported;
    return;
  }
  if (!equivalent_pair(tracking.explained_pair->mode,
                       tracking.explained_pair->state, reported.mode,
                       reported.state)) {
    if (!tracking.own_expectation.has_value() ||
        !own_effect_explains(tracking.own_expectation->command,
                             tracking.own_expectation->base.mode,
                             tracking.own_expectation->base.state,
                             reported.mode, reported.state)) {
      // The explained pair stays frozen at the last explained status.
      tracking.external_control_detected = true;
      return;
    }
    tracking.explained_pair = reported;
  }
  if (tracking.own_expectation.has_value() &&
      tracking.own_expectation->completed_at_ns.has_value() &&
      own_effect_completed(tracking.own_expectation->command, reported.mode,
                           reported.state)) {
    tracking.own_expectation.reset();
  }
}

XArmDeviceStatus
XArmBrandLifecyclePort::Classify(const std::int64_t now_ns,
                                 VendorTrackingState &tracking) const {
  const auto observed = transport_->observe();
  if (!fresh_report(observed, now_ns, config_.status_freshness_ns)) {
    return {LifecycleBlockingCauseClassDTO::kAvailability, unavailable_facts()};
  }
  TrackVendorControl(observed, now_ns, tracking);
  return {vendor_cause(observed, tracking.external_control_detected),
          fresh_facts(observed, (1 << config_.axis_count) - 1)};
}

XArmDeviceStatus XArmBrandLifecyclePort::Observe(const std::int64_t now_ns) {
  std::lock_guard lock(mutex_);
  return Classify(now_ns, tracking_);
}

XArmDeviceStatus
XArmBrandLifecyclePort::ReadHealth(const std::int64_t now_ns) const {
  std::lock_guard lock(mutex_);
  auto copy = tracking_;
  return Classify(now_ns, copy);
}

void XArmBrandLifecyclePort::BeginOwnPrimitive(
    const XArmLifecycleCommand command) {
  std::lock_guard lock(mutex_);
  // Dispatched only after the operator's acknowledgement: recovery starts
  // from the fresh report.
  if (tracking_.external_control_detected) {
    tracking_.external_control_detected = false;
    tracking_.explained_pair = tracking_.last_reported_pair;
  }
  tracking_.own_expectation = OwnPrimitiveExpectation{
      command, tracking_.last_reported_pair.value_or(VendorControlPair{}),
      std::nullopt};
}

void XArmBrandLifecyclePort::SettleOwnPrimitive(
    const XArmLifecycleCommand command, const bool effect_possible,
    const std::int64_t now_ns) {
  std::lock_guard lock(mutex_);
  if (!tracking_.own_expectation.has_value() ||
      tracking_.own_expectation->command != command ||
      tracking_.own_expectation->completed_at_ns.has_value()) {
    return;
  }
  if (effect_possible) {
    tracking_.own_expectation->completed_at_ns = now_ns;
  } else {
    tracking_.own_expectation.reset();
  }
}

bool XArmBrandLifecyclePort::ClearErrorPermitted() const {
  std::lock_guard lock(mutex_);
  return !power_cycle_error(tracking_.last_reported_error_code);
}

// R7 §3.6: a trajectory controller that becomes active while the hardware
// holds a zero target would drive there. The reported angles are compared
// only with no vendor error: under an emergency stop they are all zero.
core::Result<void> XArmBrandLifecyclePort::CheckActivationGate(
    const std::int64_t now_ns,
    const std::vector<double> &measured_positions_rad) const {
  const auto refuse = [](std::string reason, std::string message) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        std::move(message),
        {{"reason", std::move(reason)}}});
  };
  const auto observed = transport_->observe();
  if (!fresh_report(observed, now_ns, config_.status_freshness_ns))
    return refuse("xarm_status_unavailable",
                  "the robot's status report is missing or stale");
  const auto axis_mask = (1 << config_.axis_count) - 1;
  const auto facts = fresh_facts(observed, axis_mask);
  const auto accepts = std::find_if(
      facts.begin(), facts.end(), [](const LifecycleFactDTO &candidate) {
        return candidate.lifecycle_fact_id.value() ==
               core::kDeviceAcceptsCommandsFactId;
      });
  if (accepts == facts.end() || accepts->value.boolean_value != true)
    return refuse("xarm_not_ready",
                  "the robot is not ready for commands (error, mode, state "
                  "or motor enable)");
  const auto axes = static_cast<std::size_t>(config_.axis_count);
  if (observed.joint_angles_rad.size() < axes ||
      measured_positions_rad.size() != axes)
    return refuse("xarm_joint_angles_unavailable",
                  "the reported or measured joint count does not match the "
                  "robot's axes");
  for (std::size_t axis = 0U; axis < axes; ++axis) {
    const auto reported = observed.joint_angles_rad[axis];
    const auto measured = measured_positions_rad[axis];
    if (!std::isfinite(reported) || !std::isfinite(measured))
      return refuse("xarm_joint_angles_unavailable",
                    "a reported or measured joint angle is not finite");
    if (std::abs(measured - reported) > config_.joint_agreement_tolerance_rad)
      return refuse("xarm_measured_joints_disagree",
                    "the measured joint " + std::to_string(axis + 1U) +
                        " differs from the robot's reported angle");
  }
  return core::Result<void>::Ok();
}

} // namespace manyforge::xarm_control
