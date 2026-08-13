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

[[nodiscard]] std::vector<LifecycleFactDTO>
normalized_facts(const XArmObservedState &observed) {
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
  };
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
dispatched_failed(const BrandLifecyclePrimitiveIntentDTO &intent,
                  std::string reason) {
  auto result = result_base(intent);
  result.dispatch_state = LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown;
  result.effect = LifecyclePrimitiveEffectDTO::kPossible;
  result.known_result = LifecyclePrimitiveKnownResultDTO::kFailed;
  result.error = error("xarm_lifecycle_primitive_failed",
                       "the restricted xArm owner reported primitive failure",
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
      normalized_facts(observed)};
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

  XArmLifecycleCommandResult transport_result;
  try {
    transport_result = transport_->execute(
        *command, intent.lifecycle_primitive_attempt_id.value(),
        intent.expected_observation_generation, intent.remaining_duration_ns,
        cancellation);
  } catch (const std::exception &exception) {
    return dispatched_uncertain(intent, exception.what());
  } catch (...) {
    return dispatched_uncertain(intent, "unknown_transport_exception");
  }

  switch (transport_result.state) {
  case XArmLifecycleResultState::kNotDispatched:
    return not_dispatched(intent, "xarm_lifecycle_primitive_not_dispatched",
                          transport_result.reason);
  case XArmLifecycleResultState::kOutcomeUnknownAfterDispatch:
    return dispatched_uncertain(intent, transport_result.reason);
  case XArmLifecycleResultState::kResponseReceived:
    if (!transport_result.attempted) {
      return not_dispatched(intent, "xarm_lifecycle_primitive_not_permitted",
                            transport_result.reason);
    }
    if (transport_result.permitted && transport_result.return_code == 0) {
      return dispatched_succeeded(intent);
    }
    return dispatched_failed(intent, transport_result.reason);
  }
  return dispatched_uncertain(intent, "unknown_transport_result_state");
}

} // namespace manyforge::xarm_control
