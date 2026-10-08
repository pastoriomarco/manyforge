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

#include <manyforge_xarm_control/lite6_brand_lifecycle_port.hpp>

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_xarm_control/lite6_brand_lifecycle_catalog.hpp>

namespace manyforge::xarm_control {
namespace {

using core::BrandLifecycleObservationDTO;
using core::BrandLifecyclePrimitiveIntentDTO;
using core::BrandLifecyclePrimitiveResultDTO;
using core::ComponentLifecycleStateDTO;
using core::ErrorDTO;
using core::LifecycleFactDTO;
using core::LifecycleFactId;
using core::LifecycleFactValueDTO;
using core::LifecycleFactValueKindDTO;
using core::LifecyclePrimitiveDispatchStateDTO;
using core::LifecyclePrimitiveEffectDTO;
using core::LifecyclePrimitiveKnownResultDTO;
constexpr int kJointServoMode = 1;
constexpr int kReadyFeedbackState = 2;
constexpr int kStoppedFeedbackState = 4;
constexpr int kModeChangedFeedbackState = 5;

[[nodiscard]] std::string bounded_reason(std::string value) {
  if (value.empty()) {
    return "unspecified";
  }
  constexpr std::size_t kMaxReasonBytes =
      core::kMaxLifecycleErrorDetailValueBytes;
  if (value.size() > kMaxReasonBytes) {
    value.resize(kMaxReasonBytes);
  }
  return value;
}

[[nodiscard]] ErrorDTO error(std::string code, std::string message,
                             std::string reason) {
  return {std::move(code),
          std::move(message),
          {{"reason", bounded_reason(std::move(reason))}}};
}

[[nodiscard]] LifecycleFactValueDTO unknown_value() {
  return {LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt,
          std::nullopt};
}

[[nodiscard]] LifecycleFactValueDTO boolean_value(bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
          std::nullopt};
}

[[nodiscard]] LifecycleFactValueDTO fact_value(FactState value) {
  switch (value) {
  case FactState::kFalse:
    return boolean_value(false);
  case FactState::kTrue:
    return boolean_value(true);
  case FactState::kUnknown:
    return unknown_value();
  }
  return unknown_value();
}

[[nodiscard]] LifecycleFactValueDTO token_value(std::string value) {
  return {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
          std::move(value)};
}

[[nodiscard]] LifecycleFactValueDTO integer_value(std::int64_t value) {
  return {LifecycleFactValueKindDTO::kInteger, std::nullopt, value,
          std::nullopt};
}

[[nodiscard]] FactState known_boolean(bool value) {
  return value ? FactState::kTrue : FactState::kFalse;
}

[[nodiscard]] FactState warning_present(const XArmObservedState &observed) {
  if (observed.warning_code < 0) {
    return FactState::kUnknown;
  }
  return known_boolean(observed.warning_code > 0);
}

[[nodiscard]] FactState
joint_servo_mode_selected(const XArmObservedState &observed) {
  if (observed.mode < 0) {
    return FactState::kUnknown;
  }
  return known_boolean(observed.mode == kJointServoMode);
}

[[nodiscard]] FactState vendor_stopped(const XArmObservedState &observed) {
  if (observed.state < 0) {
    return FactState::kUnknown;
  }
  return known_boolean(observed.state == kStoppedFeedbackState);
}

[[nodiscard]] FactState
ready_state_request_eligible(const XArmObservedState &observed) {
  if (observed.state < 0) {
    return FactState::kUnknown;
  }
  return known_boolean(observed.state == kStoppedFeedbackState ||
                       observed.state == kModeChangedFeedbackState);
}

[[nodiscard]] LifecycleFactValueDTO
feedback_class(const XArmObservedState &observed) {
  switch (observed.state) {
  case kStoppedFeedbackState:
    return token_value("stopped");
  case kModeChangedFeedbackState:
    return token_value("mode_changed");
  case kReadyFeedbackState:
    return token_value("ready");
  default:
    return observed.state < 0 ? unknown_value() : token_value("other");
  }
}

[[nodiscard]] bool is_true(FactState value) {
  return value == FactState::kTrue;
}

[[nodiscard]] bool is_false(FactState value) {
  return value == FactState::kFalse;
}

[[nodiscard]] ComponentLifecycleStateDTO
observed_state(const XArmObservedState &observed) {
  const auto stopped = vendor_stopped(observed);
  const auto joint_servo = joint_servo_mode_selected(observed);
  const auto warning = warning_present(observed);

  // A drive-disabled stopped arm keeps lifecycle state Disabled even with a
  // vendor fault (the Recover plan requires it); the fault takes precedence in
  // the `manyforge.blocking_cause_class` fact instead (spec 210).
  if (is_false(observed.motion_enabled) && is_true(stopped)) {
    return ComponentLifecycleStateDTO::kDisabled;
  }
  if (is_true(observed.fault_present) || is_true(observed.recovery_required)) {
    return ComponentLifecycleStateDTO::kFaulted;
  }
  if (is_true(observed.motion_enabled) && is_true(observed.position_valid) &&
      is_true(joint_servo) && is_true(observed.vendor_control_ready) &&
      observed.state == kReadyFeedbackState &&
      is_false(observed.fault_present) &&
      is_false(observed.recovery_required) && is_false(warning)) {
    return ComponentLifecycleStateDTO::kActive;
  }
  if (is_true(observed.motion_enabled) && is_true(observed.position_valid) &&
      is_true(joint_servo) && is_false(observed.vendor_control_ready) &&
      observed.state == kModeChangedFeedbackState &&
      is_false(observed.fault_present) &&
      is_false(observed.recovery_required) && is_false(warning)) {
    return ComponentLifecycleStateDTO::kPrepared;
  }
  return ComponentLifecycleStateDTO::kDegraded;
}

[[nodiscard]] bool report_unavailable(const XArmObservedState &observed) {
  return std::find(observed.blocking_reason_codes.begin(),
                   observed.blocking_reason_codes.end(),
                   "xarm_report_unavailable") !=
         observed.blocking_reason_codes.end();
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

// Conservative vendor-state classification of one fresh, connected report.
//
// A positive controller error code is classified by its documented meaning;
// every other positive code is `fault`. The protective stop has no documented
// code and is never inferred: anything not established from the report is
// `unknown`. An unrequested mode/state change (see `track_vendor_control`) is
// `external_control`.
[[nodiscard]] core::LifecycleBlockingCauseClassDTO
vendor_cause(const XArmObservedState &observed,
             const bool external_control_detected) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  if (observed.error_code > 0) {
    if (emergency_stop_error(observed.error_code)) {
      return Cause::kEmergencyStop;
    }
    if (observed.error_code == kCollisionErrorCode) {
      return Cause::kCollision;
    }
    return Cause::kFault;
  }
  if (is_true(observed.fault_present) || is_true(observed.recovery_required)) {
    return Cause::kFault;
  }
  if (report_unavailable(observed)) {
    return Cause::kAvailability;
  }
  if (external_control_detected) {
    return Cause::kExternalControl;
  }
  if (!is_false(observed.fault_present) ||
      !is_false(observed.recovery_required) ||
      observed.motion_enabled == FactState::kUnknown ||
      !is_true(observed.position_valid) ||
      !is_false(warning_present(observed)) || observed.mode < 0 ||
      observed.state < 0) {
    return Cause::kUnknown;
  }
  return Cause::kNone;
}

[[nodiscard]] std::vector<LifecycleFactDTO>
normalized_facts(const XArmObservedState &observed,
                 const core::LifecycleBlockingCauseClassDTO cause) {
  std::vector<LifecycleFactDTO> facts{
      {LifecycleFactId(std::string(kXArmMotionEnabledFactId)),
       fact_value(observed.motion_enabled)},
      {LifecycleFactId(std::string(kXArmPositionValidFactId)),
       fact_value(observed.position_valid)},
      {LifecycleFactId(std::string(kXArmJointServoModeSelectedFactId)),
       fact_value(joint_servo_mode_selected(observed))},
      {LifecycleFactId(std::string(kXArmVendorControlReadyFactId)),
       fact_value(observed.vendor_control_ready)},
      {LifecycleFactId(std::string(kXArmVendorStoppedFactId)),
       fact_value(vendor_stopped(observed))},
      {LifecycleFactId(std::string(kXArmFaultPresentFactId)),
       fact_value(observed.fault_present)},
      {LifecycleFactId(std::string(kXArmRecoveryRequiredFactId)),
       fact_value(observed.recovery_required)},
      {LifecycleFactId(std::string(kXArmWarningPresentFactId)),
       fact_value(warning_present(observed))},
      {LifecycleFactId(std::string(kXArmReadyStateRequestEligibleFactId)),
       fact_value(ready_state_request_eligible(observed))},
      {LifecycleFactId(std::string(kXArmVendorFeedbackClassFactId)),
       feedback_class(observed)},
      {LifecycleFactId(std::string(core::kLifecycleBlockingCauseFactId)),
       token_value(std::string(core::to_string(cause)))},
  };
  if (!report_unavailable(observed)) {
    if (observed.error_code > 0) {
      facts.push_back({LifecycleFactId(std::string(kVendorCodeErrorFactId)),
                       token_value("C" + std::to_string(observed.error_code))});
    }
    if (power_cycle_error(observed.error_code)) {
      facts.push_back(
          {LifecycleFactId(std::string(kVendorCodePowerCycleFactId)),
           token_value(std::string(kPowerCycleRequiredToken))});
    }
    if (observed.warning_code > 0) {
      facts.push_back({LifecycleFactId(std::string(kVendorCodeWarningFactId)),
                       integer_value(observed.warning_code)});
    }
    if (observed.mode >= 0) {
      facts.push_back({LifecycleFactId(std::string(kVendorCodeModeFactId)),
                       integer_value(observed.mode)});
    }
    if (observed.state >= 0) {
      facts.push_back({LifecycleFactId(std::string(kVendorCodeStateFactId)),
                       integer_value(observed.state)});
    }
  }
  std::sort(facts.begin(), facts.end(),
            [](const auto &left, const auto &right) {
              return left.lifecycle_fact_id < right.lifecycle_fact_id;
            });
  return facts;
}

[[nodiscard]] std::optional<XArmLifecycleCommand>
command_for(const core::LifecyclePrimitiveId &primitive_id) {
  const auto &value = primitive_id.value();
  if (value == kXArmEnableMotionPrimitiveId) {
    return XArmLifecycleCommand::kEnableMotion;
  }
  if (value == kXArmSelectJointServoModePrimitiveId) {
    return XArmLifecycleCommand::kSelectJointServoMode;
  }
  if (value == kXArmSetReadyStatePrimitiveId) {
    return XArmLifecycleCommand::kSetReadyState;
  }
  if (value == kXArmSetStoppedStatePrimitiveId) {
    return XArmLifecycleCommand::kSetStoppedState;
  }
  if (value == kXArmDisableMotionPrimitiveId) {
    return XArmLifecycleCommand::kDisableMotion;
  }
  if (value == kXArmClearErrorPrimitiveId) {
    return XArmLifecycleCommand::kClearError;
  }
  if (value == kXArmClearWarningPrimitiveId) {
    return XArmLifecycleCommand::kClearWarning;
  }
  return std::nullopt;
}

[[nodiscard]] BrandLifecyclePrimitiveResultDTO
result_base(const BrandLifecyclePrimitiveIntentDTO &intent) {
  return {intent.lifecycle_transaction_id,
          intent.lifecycle_transaction_generation,
          intent.controllable_component_id,
          intent.control_domain_id,
          intent.control_domain_revision,
          intent.provider_instance_id,
          intent.provider_manifest_id,
          intent.provider_manifest_revision,
          intent.provider_runtime_unit_id,
          intent.provider_runtime_epoch,
          intent.lifecycle_primitive_id,
          intent.lifecycle_primitive_attempt_id,
          intent.expected_observation_generation,
          LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
          LifecyclePrimitiveEffectDTO::kNone,
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] BrandLifecyclePrimitiveResultDTO
not_dispatched(const BrandLifecyclePrimitiveIntentDTO &intent, std::string code,
               std::string reason) {
  auto result = result_base(intent);
  result.error =
      error(std::move(code), "the Lite6 lifecycle primitive was not dispatched",
            std::move(reason));
  return result;
}

[[nodiscard]] BrandLifecyclePrimitiveResultDTO
dispatched_uncertain(const BrandLifecyclePrimitiveIntentDTO &intent,
                     std::string reason) {
  auto result = result_base(intent);
  result.dispatch_state =
      LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain;
  result.effect = LifecyclePrimitiveEffectDTO::kPossible;
  result.error = error(
      "xarm_lifecycle_primitive_outcome_unknown",
      "the Lite6 lifecycle primitive may have crossed the transport boundary",
      std::move(reason));
  return result;
}

[[nodiscard]] BrandLifecyclePrimitiveResultDTO
dispatched_succeeded(const BrandLifecyclePrimitiveIntentDTO &intent) {
  auto result = result_base(intent);
  result.dispatch_state = LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown;
  result.effect = LifecyclePrimitiveEffectDTO::kPossible;
  result.known_result = LifecyclePrimitiveKnownResultDTO::kSucceeded;
  return result;
}

// An own primitive explains vendor mode/state changes for this long after it
// returned, unless a report showed its final expected pair earlier.
constexpr std::int64_t kOwnPrimitiveSettleNs = 5'000'000'000LL;
constexpr int kPositionMode = 0;
constexpr int kMovingFeedbackState = 1;
constexpr int kDeceleratingFeedbackState = 6;

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
  case XArmLifecycleCommand::kEnableMotion:
    return mode == base_mode &&
           (state == base_state || state == kStoppedFeedbackState);
  case XArmLifecycleCommand::kSelectJointServoMode:
    return (mode == kJointServoMode &&
            (state == base_state || state == kStoppedFeedbackState ||
             state == kModeChangedFeedbackState)) ||
           (mode == base_mode && state == kModeChangedFeedbackState);
  case XArmLifecycleCommand::kSetReadyState:
    return mode == base_mode && state == kReadyFeedbackState;
  case XArmLifecycleCommand::kSetStoppedState:
    return base_or_position_mode && (state == kStoppedFeedbackState ||
                                      state == kDeceleratingFeedbackState);
  case XArmLifecycleCommand::kDisableMotion:
  case XArmLifecycleCommand::kClearError:
  case XArmLifecycleCommand::kClearWarning:
    return base_or_position_mode &&
           (state == base_state || state == kStoppedFeedbackState);
  default:
    return false;
  }
}

// True when the reported pair is the command's final expected postcondition.
[[nodiscard]] bool own_effect_completed(const XArmLifecycleCommand command,
                                        const int mode, const int state) {
  switch (command) {
  case XArmLifecycleCommand::kSelectJointServoMode:
    return mode == kJointServoMode && state == kModeChangedFeedbackState;
  case XArmLifecycleCommand::kSetReadyState:
    return state == kReadyFeedbackState;
  default:
    return state == kStoppedFeedbackState;
  }
}

// Stop-direction primitives CCLC may run while an intervention cause is open
// (a requested Disable). They do not authorize a fresh baseline.
[[nodiscard]] bool stop_direction(const XArmLifecycleCommand command) {
  return command == XArmLifecycleCommand::kSetStoppedState ||
         command == XArmLifecycleCommand::kDisableMotion;
}

} // namespace

Lite6RestrictedRosLifecycleTransport::Lite6RestrictedRosLifecycleTransport(
    XArmRosTransportSessionConfig config)
    : execute_timeout_ns_(config.execute_timeout_ns),
      session_(std::move(config)) {
  if (execute_timeout_ns_ <= 0 ||
      execute_timeout_ns_ > core::kMaxLifecycleTransactionTimeoutNs) {
    throw std::invalid_argument(
        "invalid Lite6 restricted ROS lifecycle call bound");
  }
}

XArmObservedState
Lite6RestrictedRosLifecycleTransport::observe(std::int64_t now_ns) const {
  return session_.observe_vendor(now_ns);
}

XArmLifecycleCommandResult Lite6RestrictedRosLifecycleTransport::execute(
    XArmLifecycleCommand command, std::string_view request_id,
    std::uint64_t expected_generation, std::int64_t remaining_duration_ns,
    std::stop_token cancellation) {
  if (cancellation.stop_requested()) {
    XArmLifecycleCommandResult result;
    result.reason = "cancelled_before_transport_call";
    result.observed_generation = expected_generation;
    return result;
  }
  if (remaining_duration_ns < execute_timeout_ns_) {
    XArmLifecycleCommandResult result;
    result.reason = "remaining_duration_shorter_than_transport_call_bound";
    result.observed_generation = expected_generation;
    return result;
  }
  return session_.execute_primitive(command, request_id, expected_generation);
}

Lite6BrandLifecyclePort::Lite6BrandLifecyclePort(
    Lite6BrandLifecyclePortConfig config, core::IClock &clock,
    std::unique_ptr<ILite6BrandLifecycleTransport> transport)
    : config_(std::move(config)), clock_(clock),
      transport_(std::move(transport)) {
  const core::BrandLifecycleObservationQueryDTO validation_query{
      core::LifecycleTransactionId("validation.transaction"),
      core::LifecycleTransactionGeneration(1U),
      config_.controllable_component_id,
      config_.control_domain_id,
      config_.control_domain_revision,
      config_.provider_instance_id,
      core::ProviderManifestId(std::string(kLite6LifecycleProviderManifestId)),
      config_.provider_manifest_revision,
      config_.provider_runtime_unit_id,
      config_.provider_runtime_epoch};
  if (!validation_query.locally_valid() ||
      config_.transport_owner_component_epoch.empty() ||
      config_.transport_owner_component_epoch.size() >
          core::kMaxLifecycleIdentityBytes) {
    throw std::invalid_argument(
        "invalid Lite6 brand lifecycle provider occurrence");
  }
  if (transport_ == nullptr) {
    throw std::invalid_argument("Lite6 brand lifecycle transport is missing");
  }
}

Lite6BrandLifecyclePort::Lite6BrandLifecyclePort(
    Lite6BrandLifecyclePortConfig config, core::IClock &clock,
    XArmRosTransportSessionConfig transport_config)
    : Lite6BrandLifecyclePort(
          std::move(config), clock,
          std::make_unique<Lite6RestrictedRosLifecycleTransport>(
              std::move(transport_config))) {}

Lite6BrandLifecyclePort::~Lite6BrandLifecyclePort() = default;

bool Lite6BrandLifecyclePort::matches_occurrence(
    const core::BrandLifecycleObservationQueryDTO &query) const noexcept {
  return query.controllable_component_id == config_.controllable_component_id &&
         query.control_domain_id == config_.control_domain_id &&
         query.control_domain_revision == config_.control_domain_revision &&
         query.provider_instance_id == config_.provider_instance_id &&
         query.provider_manifest_id ==
             core::ProviderManifestId(
                 std::string(kLite6LifecycleProviderManifestId)) &&
         query.provider_manifest_revision ==
             config_.provider_manifest_revision &&
         query.provider_runtime_unit_id == config_.provider_runtime_unit_id &&
         query.provider_runtime_epoch == config_.provider_runtime_epoch;
}

bool Lite6BrandLifecyclePort::matches_occurrence(
    const core::BrandLifecyclePrimitiveIntentDTO &intent) const noexcept {
  return intent.controllable_component_id ==
             config_.controllable_component_id &&
         intent.control_domain_id == config_.control_domain_id &&
         intent.control_domain_revision == config_.control_domain_revision &&
         intent.provider_instance_id == config_.provider_instance_id &&
         intent.provider_manifest_id ==
             core::ProviderManifestId(
                 std::string(kLite6LifecycleProviderManifestId)) &&
         intent.provider_manifest_revision ==
             config_.provider_manifest_revision &&
         intent.provider_runtime_unit_id == config_.provider_runtime_unit_id &&
         intent.provider_runtime_epoch == config_.provider_runtime_epoch;
}

// Unrequested mode/state change detection (addendum 2 §7). ManyForge knows
// the changes it causes: each own primitive carries an expected vendor
// effect. With a fresh error-free report, a mode/state change that neither
// the last explained pair nor the in-flight or just-completed own primitive
// explains is `external_control` (for example a UFactory Studio jog leaving
// mode 0 / state 5, teach mode 2, or a stop that ManyForge did not request).
// It stays reported, also across report loss, until CCLC dispatches the next
// non-stop primitive, which it does only after the operator's acknowledgement;
// that dispatch re-baselines from the latest fresh report. Detection of real
// Studio jogs and of these vendor sequences is verified on hardware in R7.
void Lite6BrandLifecyclePort::track_vendor_control(
    const XArmObservedState &observed, const std::int64_t now_ns) {
  if (report_unavailable(observed)) {
    return;
  }
  last_reported_error_code_ = observed.error_code;
  if (observed.mode < 0 || observed.state < 0) {
    return;
  }
  const VendorControlPair reported{observed.mode, observed.state};
  last_reported_pair_ = reported;
  if (own_expectation_.has_value()) {
    if (own_expectation_->completed_at_ns.has_value() &&
        now_ns - *own_expectation_->completed_at_ns > kOwnPrimitiveSettleNs) {
      own_expectation_.reset();
    } else if (own_expectation_->base.mode < 0 ||
               own_expectation_->base.state < 0) {
      own_expectation_->base = explained_pair_.value_or(reported);
    }
  }
  // A vendor error explains the pair it reports; its code classifies it.
  if (observed.error_code != 0 || !is_false(observed.fault_present) ||
      !is_false(observed.recovery_required)) {
    explained_pair_ = reported;
    return;
  }
  // ManyForge only ever selects position or joint-servo mode.
  if (reported.mode != kPositionMode && reported.mode != kJointServoMode) {
    external_control_detected_ = true;
    return;
  }
  if (!explained_pair_.has_value()) {
    explained_pair_ = reported;
    return;
  }
  if (!equivalent_pair(explained_pair_->mode, explained_pair_->state,
                       reported.mode, reported.state)) {
    if (!own_expectation_.has_value() ||
        !own_effect_explains(own_expectation_->command,
                             own_expectation_->base.mode,
                             own_expectation_->base.state, reported.mode,
                             reported.state)) {
      // The explained pair stays frozen at the last explained status.
      external_control_detected_ = true;
      return;
    }
    explained_pair_ = reported;
  }
  if (own_expectation_.has_value() &&
      own_expectation_->completed_at_ns.has_value() &&
      own_effect_completed(own_expectation_->command, reported.mode,
                           reported.state)) {
    own_expectation_.reset();
  }
}

core::Result<BrandLifecycleObservationDTO> Lite6BrandLifecyclePort::Observe(
    const core::BrandLifecycleObservationQueryDTO &query) {
  if (!query.locally_valid() || !matches_occurrence(query)) {
    return core::Result<BrandLifecycleObservationDTO>::Err(error(
        "xarm_lifecycle_observation_identity_mismatch",
        "the observation query does not address this Lite6 provider occurrence",
        "provider_occurrence_mismatch"));
  }

  const auto now_ns = clock_.now();
  if (now_ns < 0) {
    return core::Result<BrandLifecycleObservationDTO>::Err(
        error("xarm_lifecycle_observation_clock_invalid",
              "the lifecycle observation clock returned a negative time",
              "negative_owner_clock"));
  }
  XArmObservedState observed;
  try {
    observed = transport_->observe(now_ns);
  } catch (const std::exception &exception) {
    return core::Result<BrandLifecycleObservationDTO>::Err(error(
        "xarm_lifecycle_observation_unavailable",
        "the restricted xArm observation boundary failed", exception.what()));
  } catch (...) {
    return core::Result<BrandLifecycleObservationDTO>::Err(
        error("xarm_lifecycle_observation_unavailable",
              "the restricted xArm observation boundary failed",
              "unknown_transport_exception"));
  }

  if (!observed.transport_owner_progress_healthy ||
      observed.transport_connected != FactState::kTrue ||
      observed.identity_verified != FactState::kTrue ||
      observed.generation == 0U ||
      observed.transport_owner_progress_sequence == 0U ||
      observed.transport_owner_component_epoch !=
          config_.transport_owner_component_epoch ||
      observed.source_timestamp_ns < 0 ||
      observed.transport_owner_received_timestamp_ns <
          observed.source_timestamp_ns ||
      observed.transport_owner_received_timestamp_ns > now_ns ||
      observed.fresh_until_ns <= now_ns ||
      observed.transport_owner_received_timestamp_ns >=
          observed.fresh_until_ns) {
    return core::Result<BrandLifecycleObservationDTO>::Err(error(
        "xarm_lifecycle_observation_unavailable",
        "the restricted xArm observation is stale, invalid, or cross-epoch",
        observed.transport_owner_progress_reason.empty()
            ? "observation_invariant_failed"
            : observed.transport_owner_progress_reason));
  }

  // A connected observation without a vendor report is an availability loss
  // only while no intervention-requiring cause was previously reported; any
  // classification from a report replaces the retained cause.
  auto cause = core::LifecycleBlockingCauseClassDTO::kUnknown;
  {
    std::lock_guard lock(cause_mutex_);
    track_vendor_control(observed, now_ns);
    cause = vendor_cause(observed, external_control_detected_);
    if (cause == core::LifecycleBlockingCauseClassDTO::kAvailability) {
      if (last_intervention_cause_ !=
          core::LifecycleBlockingCauseClassDTO::kNone) {
        cause = last_intervention_cause_;
      }
    } else {
      last_intervention_cause_ =
          core::lifecycle_cause_requires_intervention(cause)
              ? cause
              : core::LifecycleBlockingCauseClassDTO::kNone;
    }
  }

  BrandLifecycleObservationDTO result{
      query.lifecycle_transaction_id,
      query.lifecycle_transaction_generation,
      query.controllable_component_id,
      query.control_domain_id,
      query.control_domain_revision,
      query.provider_instance_id,
      query.provider_manifest_id,
      query.provider_manifest_revision,
      query.provider_runtime_unit_id,
      query.provider_runtime_epoch,
      observed.transport_owner_progress_sequence,
      observed.generation,
      observed.source_timestamp_ns,
      observed.transport_owner_received_timestamp_ns,
      observed.fresh_until_ns,
      observed_state(observed),
      normalized_facts(observed, cause)};
  if (!result.locally_valid() || !result.matches(query)) {
    return core::Result<BrandLifecycleObservationDTO>::Err(
        error("xarm_lifecycle_observation_invalid",
              "the normalized Lite6 observation violates the Core contract",
              "normalized_observation_invalid"));
  }
  return core::Result<BrandLifecycleObservationDTO>::Ok(std::move(result));
}

BrandLifecyclePrimitiveResultDTO Lite6BrandLifecyclePort::Dispatch(
    const BrandLifecyclePrimitiveIntentDTO &intent,
    std::stop_token cancellation) {
  if (!intent.locally_valid() || !matches_occurrence(intent)) {
    return not_dispatched(intent, "xarm_lifecycle_primitive_identity_mismatch",
                          "provider_occurrence_mismatch");
  }
  const auto command = command_for(intent.lifecycle_primitive_id);
  if (!command.has_value()) {
    return not_dispatched(intent, "xarm_lifecycle_primitive_unsupported",
                          "primitive_not_in_lite6_brand_catalog");
  }
  if (cancellation.stop_requested()) {
    return not_dispatched(intent, "xarm_lifecycle_primitive_cancelled",
                          "cancelled_before_transport_call");
  }
  {
    std::lock_guard lock(cause_mutex_);
    // Servo faults C10-C17 need a power cycle; a clearing command must not
    // precede it (R7 verification list).
    if (*command == XArmLifecycleCommand::kClearError &&
        power_cycle_error(last_reported_error_code_)) {
      return not_dispatched(intent, "xarm_lifecycle_primitive_not_permitted",
                            "power_cycle_required");
    }
    // CCLC dispatches a non-stop primitive only after the operator
    // acknowledged an intervention; recovery starts from the fresh report.
    if (!stop_direction(*command) && external_control_detected_) {
      external_control_detected_ = false;
      explained_pair_ = last_reported_pair_;
    }
    own_expectation_ = OwnPrimitiveExpectation{
        *command, last_reported_pair_.value_or(VendorControlPair{}),
        std::nullopt};
  }
  const auto settle_own_expectation = [&](const bool effect_possible) {
    std::lock_guard lock(cause_mutex_);
    if (!own_expectation_.has_value() ||
        own_expectation_->command != *command ||
        own_expectation_->completed_at_ns.has_value()) {
      return;
    }
    if (effect_possible) {
      own_expectation_->completed_at_ns = clock_.now();
    } else {
      own_expectation_.reset();
    }
  };

  XArmLifecycleCommandResult transport_result;
  try {
    transport_result = transport_->execute(
        *command, intent.lifecycle_primitive_attempt_id.value(),
        intent.expected_observation_generation, intent.remaining_duration_ns,
        cancellation);
  } catch (const std::exception &exception) {
    settle_own_expectation(true);
    return dispatched_uncertain(intent, exception.what());
  } catch (...) {
    settle_own_expectation(true);
    return dispatched_uncertain(intent, "unknown_transport_exception");
  }

  switch (transport_result.state) {
  case XArmLifecycleResultState::kNotDispatched:
    settle_own_expectation(false);
    return not_dispatched(intent, "xarm_lifecycle_primitive_not_dispatched",
                          transport_result.reason);
  case XArmLifecycleResultState::kOutcomeUnknownAfterDispatch:
    settle_own_expectation(true);
    return dispatched_uncertain(intent, transport_result.reason);
  case XArmLifecycleResultState::kResponseReceived:
    if (!transport_result.attempted) {
      settle_own_expectation(false);
      return not_dispatched(intent, "xarm_lifecycle_primitive_not_permitted",
                            transport_result.reason);
    }
    settle_own_expectation(true);
    if (transport_result.permitted && transport_result.return_code == 0) {
      return dispatched_succeeded(intent);
    }
    // A non-zero vendor return is an unknown outcome (old-main semantics):
    // CCLC re-observes and reconciles; it never retries blindly.
    return dispatched_uncertain(
        intent, "vendor_return_code_" +
                    std::to_string(transport_result.return_code) +
                    (transport_result.reason.empty()
                         ? std::string()
                         : ":" + transport_result.reason));
  }
  settle_own_expectation(true);
  return dispatched_uncertain(intent, "unknown_transport_result_state");
}

} // namespace manyforge::xarm_control
