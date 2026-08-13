#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/prepared_execution.hpp>

namespace manyforge::core {
namespace {

constexpr std::uint64_t kMaximumIJsonInteger = 9'007'199'254'740'991ULL;

template <typename Identity>
[[nodiscard]] bool identity(const Identity& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes &&
         value.value().find('\0') == std::string::npos;
}

template <typename Counter>
[[nodiscard]] bool counter(const Counter& value) noexcept {
  return value.nonzero() && value.value() <= kMaximumIJsonInteger;
}

[[nodiscard]] bool stable_error(const ErrorDTO& value) noexcept {
  if (value.code.empty() || value.code.size() > 128U ||
      value.code.find('\0') != std::string::npos || value.message.size() > 1024U ||
      value.message.find('\0') != std::string::npos || value.details.size() > 8U) {
    return false;
  }
  return std::all_of(value.details.begin(), value.details.end(), [](const auto& detail) {
    return !detail.first.empty() && detail.first.size() <= 64U &&
           detail.first.find('\0') == std::string::npos && detail.second.size() <= 256U &&
           detail.second.find('\0') == std::string::npos;
  });
}

[[nodiscard]] bool digest(const ContentDigest& value) noexcept {
  return content_identity::is_sha256_digest(value.value());
}

[[nodiscard]] bool capability_provider_pair_valid(
    const std::optional<CapabilityProviderBindingId>& id,
    const std::optional<CapabilityProviderBindingRevision>& revision) noexcept {
  return id.has_value() == revision.has_value() &&
         (!id.has_value() || (identity(*id) && identity(*revision)));
}

[[nodiscard]] bool canonical_resources(const std::vector<ResourceId>& resources) noexcept {
  if (resources.empty() || resources.size() > kMaximumStopSelectorTargets) {
    return false;
  }
  for (std::size_t index = 0U; index < resources.size(); ++index) {
    if (!identity(resources[index]) ||
        (index > 0U && !(resources[index - 1U] < resources[index]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool participant_identity_shape(
    const PreparedOperationParticipantDTO& participant) noexcept {
  return digest(participant.prepared_contract_digest) && identity(participant.operation_id) &&
         counter(participant.operation_revision) && identity(participant.participant_id) &&
         identity(participant.controllable_component_id) &&
         identity(participant.command_target_id) && identity(participant.capability_id) &&
         identity(participant.control_domain_id) && identity(participant.control_domain_revision) &&
         identity(participant.control_binding_id) &&
         identity(participant.control_binding_revision) &&
         identity(participant.active_control_configuration_id) &&
         counter(participant.active_control_configuration_generation) &&
         capability_provider_pair_valid(
             participant.capability_provider
                 ? std::optional(participant.capability_provider->capability_provider_binding_id)
                 : std::nullopt,
             participant.capability_provider
                 ? std::optional(
                       participant.capability_provider->capability_provider_binding_revision)
                 : std::nullopt) &&
         identity(participant.controller_topology_id) && identity(participant.topology_revision) &&
         identity(participant.control_route_id) &&
         identity(participant.control_provider.runtime_epoch) &&
         canonical_resources(participant.resources);
}

[[nodiscard]] std::vector<ResourceId> binding_resources(
    const RuntimeParticipantBindingDTO& binding) {
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  return resources;
}

[[nodiscard]] PreparedProviderRuntimeOccurrenceDTO provider(const RuntimeControlPathDTO& path) {
  return PreparedProviderRuntimeOccurrenceDTO{
      path.provider_instance_id, path.provider_manifest_id, path.provider_manifest_revision,
      path.provider_runtime_unit_id, path.provider_runtime_epoch};
}

[[nodiscard]] PreparedCapabilityProviderRuntimeOccurrenceDTO provider(
    const RuntimeCapabilityProviderPathDTO& path) {
  return PreparedCapabilityProviderRuntimeOccurrenceDTO{
      path.capability_provider_binding_id, path.capability_provider_binding_revision,
      PreparedProviderRuntimeOccurrenceDTO{
          path.provider_instance_id, path.provider_manifest_id, path.provider_manifest_revision,
          path.provider_runtime_unit_id, path.provider_runtime_epoch}};
}

[[nodiscard]] bool prepare_identity_matches(
    const PrepareParticipantResultDTO& result,
    const PreparedOperationParticipantDTO& participant) noexcept {
  const auto expected_provider_id =
      participant.capability_provider
          ? std::optional(participant.capability_provider->capability_provider_binding_id)
          : std::nullopt;
  const auto expected_provider_revision =
      participant.capability_provider
          ? std::optional(participant.capability_provider->capability_provider_binding_revision)
          : std::nullopt;
  return result.operation_id == participant.operation_id &&
         result.operation_revision == participant.operation_revision &&
         result.participant_id == participant.participant_id &&
         result.controllable_component_id == participant.controllable_component_id &&
         result.command_target_id == participant.command_target_id &&
         result.capability_id == participant.capability_id &&
         result.control_domain_id == participant.control_domain_id &&
         result.control_domain_revision == participant.control_domain_revision &&
         result.control_binding_id == participant.control_binding_id &&
         result.control_binding_revision == participant.control_binding_revision &&
         result.active_control_configuration_id == participant.active_control_configuration_id &&
         result.active_control_configuration_generation ==
             participant.active_control_configuration_generation &&
         result.capability_provider_binding_id == expected_provider_id &&
         result.capability_provider_binding_revision == expected_provider_revision &&
         result.controller_topology_id == participant.controller_topology_id &&
         result.topology_revision == participant.topology_revision &&
         result.control_route_id == participant.control_route_id &&
         result.provider_runtime_epoch == participant.control_provider.runtime_epoch &&
         result.payload_kind == prepared_participant_payload_kind(participant);
}

[[nodiscard]] bool backend_identity_matches(
    const ExecutionBackendStatusDTO& status, const PreparedOperationParticipantDTO& participant,
    const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation) noexcept {
  return status.operation_id == participant.operation_id &&
         status.operation_revision == participant.operation_revision &&
         status.participant_id == participant.participant_id &&
         status.controllable_component_id == participant.controllable_component_id &&
         status.command_target_id == participant.command_target_id &&
         status.capability_id == participant.capability_id &&
         status.control_domain_id == participant.control_domain_id &&
         status.control_domain_revision == participant.control_domain_revision &&
         status.control_binding_id == participant.control_binding_id &&
         status.control_binding_revision == participant.control_binding_revision &&
         status.controller_topology_id == participant.controller_topology_id &&
         status.topology_revision == participant.topology_revision &&
         status.control_route_id == participant.control_route_id &&
         status.active_control_configuration_id == participant.active_control_configuration_id &&
         status.active_control_configuration_generation ==
             participant.active_control_configuration_generation &&
         status.controller_goal_id == controller_goal_id &&
         status.controller_goal_generation == controller_goal_generation &&
         status.provider_runtime_epoch == participant.control_provider.runtime_epoch &&
         status.resources == participant.resources;
}

[[nodiscard]] ErrorDTO invalid_projection(std::string_view reason) {
  return ErrorDTO{std::string(kErrorInvalidRequest),
                  "prepared backend projection is not an exact closed value",
                  {{"reason", std::string(reason)}}};
}

}  // namespace

PreparedParticipantPayloadKindDTO prepared_participant_payload_kind(
    const PreparedOperationParticipantDTO& participant) noexcept {
  return std::holds_alternative<PreparedMotionExecutionDTO>(participant.payload)
             ? PreparedParticipantPayloadKindDTO::kMotionExecution
             : PreparedParticipantPayloadKindDTO::kCapabilityCommand;
}

std::optional<std::string_view> prepared_participant_backend_binding_field_error(
    const PreparedOperationParticipantDTO& participant,
    const RuntimeParticipantBinding& backend_binding,
    PreparedParticipantPayloadKindDTO expected_payload_kind) noexcept {
  const auto& binding = backend_binding.value();
  if (!participant_identity_shape(participant) || !binding.control_path.has_value() ||
      prepared_participant_payload_kind(participant) != expected_payload_kind) {
    return "prepared_participant_backend_shape_invalid";
  }
  const auto& path = *binding.control_path;
  const auto expected_capability_provider =
      binding.capability_provider_path ? std::optional(provider(*binding.capability_provider_path))
                                       : std::nullopt;
  if (participant.program_id != binding.program_id ||
      participant.program_revision != binding.program_revision ||
      participant.program_binding_id != binding.program_binding_id ||
      participant.retained_bundle_id != binding.retained_bundle_id ||
      participant.graph_revision != binding.graph_revision ||
      participant.controllable_component_id != binding.controllable_component_id ||
      participant.component_type_id != binding.component_type_id ||
      participant.component_type_revision != binding.component_type_revision ||
      participant.command_target_id != binding.command_target_id ||
      participant.capability_id != binding.capability_id ||
      participant.capability_definition_revision != binding.capability_definition_revision ||
      participant.model_bindings != binding.model_bindings ||
      participant.frame_bindings != binding.frame_bindings ||
      participant.control_domain_id != path.control_domain_id ||
      participant.control_domain_revision != path.control_domain_revision ||
      participant.control_binding_id != path.control_binding_id ||
      participant.control_binding_revision != path.control_binding_revision ||
      participant.control_interface_contract_id != path.control_interface_contract_id ||
      participant.control_interface_revision != path.control_interface_revision ||
      participant.controller_topology_id != path.controller_topology_id ||
      participant.topology_revision != path.topology_revision ||
      participant.control_route_id != path.control_route_id ||
      participant.activation_set_id != path.activation_set_id ||
      participant.control_provider != provider(path) ||
      participant.capability_provider != expected_capability_provider ||
      participant.resources != binding_resources(binding) ||
      participant.active_control_configuration_id != path.active_control_configuration_id ||
      participant.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      participant.runtime_unit_epochs != binding.runtime_unit_epochs ||
      participant.stop_evidence_policy_id != path.stop_evidence_policy_id ||
      participant.stop_evidence_policy_revision != path.stop_evidence_policy_revision) {
    return "prepared_participant_backend_binding_mismatch";
  }
  const auto& stop_plan = participant.ordinary_stop_plan;
  if (stop_plan.operation_id != participant.operation_id ||
      stop_plan.operation_revision != participant.operation_revision ||
      stop_plan.participant_id != participant.participant_id ||
      stop_plan.program_id != participant.program_id ||
      stop_plan.program_revision != participant.program_revision ||
      stop_plan.program_binding_id != participant.program_binding_id ||
      stop_plan.program_run_id != participant.program_run_id ||
      stop_plan.participant_stop_profile_purpose != ParticipantStopProfilePurposeDTO::kOrdinary ||
      stop_plan.participant_stop_profile.has_value() ||
      stop_plan.motion_interrupt_scope_id.has_value()) {
    return "prepared_participant_backend_stop_plan_mismatch";
  }
  return std::nullopt;
}

std::optional<std::string_view> prepare_participant_result_field_error(
    const PrepareParticipantResultDTO& result,
    const PreparedOperationParticipantDTO& participant) noexcept {
  if (!participant_identity_shape(participant) || !prepare_identity_matches(result, participant) ||
      !capability_provider_pair_valid(result.capability_provider_binding_id,
                                      result.capability_provider_binding_revision)) {
    return "prepare_participant_result_identity_invalid";
  }
  switch (result.state) {
    case PrepareParticipantResultStateDTO::kPrepared:
      if (!result.prepared_contract_digest.has_value() ||
          *result.prepared_contract_digest != participant.prepared_contract_digest ||
          !digest(*result.prepared_contract_digest) || result.error.has_value()) {
        return "prepare_participant_result_presence_invalid";
      }
      return std::nullopt;
    case PrepareParticipantResultStateDTO::kRejected:
      if (result.prepared_contract_digest.has_value() || !result.error.has_value() ||
          !stable_error(*result.error)) {
        return "prepare_participant_result_presence_invalid";
      }
      return std::nullopt;
  }
  return "prepare_participant_result_state_invalid";
}

Result<PrepareParticipantResultDTO> MakePrepareParticipantResult(
    const PreparedOperationParticipantDTO& participant, PrepareParticipantResultStateDTO state,
    std::optional<ErrorDTO> error) {
  const auto provider_id =
      participant.capability_provider
          ? std::optional(participant.capability_provider->capability_provider_binding_id)
          : std::nullopt;
  const auto provider_revision =
      participant.capability_provider
          ? std::optional(participant.capability_provider->capability_provider_binding_revision)
          : std::nullopt;
  PrepareParticipantResultDTO result{participant.operation_id,
                                     participant.operation_revision,
                                     participant.participant_id,
                                     participant.controllable_component_id,
                                     participant.command_target_id,
                                     participant.capability_id,
                                     participant.control_domain_id,
                                     participant.control_domain_revision,
                                     participant.control_binding_id,
                                     participant.control_binding_revision,
                                     participant.active_control_configuration_id,
                                     participant.active_control_configuration_generation,
                                     provider_id,
                                     provider_revision,
                                     participant.controller_topology_id,
                                     participant.topology_revision,
                                     participant.control_route_id,
                                     participant.control_provider.runtime_epoch,
                                     prepared_participant_payload_kind(participant),
                                     state,
                                     state == PrepareParticipantResultStateDTO::kPrepared
                                         ? std::optional(participant.prepared_contract_digest)
                                         : std::nullopt,
                                     std::move(error)};
  if (const auto field_error = prepare_participant_result_field_error(result, participant)) {
    return Result<PrepareParticipantResultDTO>::Err(invalid_projection(*field_error));
  }
  return Result<PrepareParticipantResultDTO>::Ok(std::move(result));
}

std::optional<std::string_view> execution_backend_status_field_error(
    const ExecutionBackendStatusDTO& status, const PreparedOperationParticipantDTO& participant,
    const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation) noexcept {
  if (!participant_identity_shape(participant) || !identity(controller_goal_id) ||
      !counter(controller_goal_generation) ||
      !backend_identity_matches(status, participant, controller_goal_id,
                                controller_goal_generation)) {
    return "execution_backend_status_identity_invalid";
  }
  if (!std::isfinite(status.accepted_trajectory_time_s) ||
      status.accepted_trajectory_time_s < 0.0 || !std::isfinite(status.progress) ||
      status.progress < 0.0 || status.progress > 1.0 || status.source_sequence == 0U ||
      status.source_sequence > kMaximumIJsonInteger ||
      status.receiver_local_receipt_timestamp_ns < 0 ||
      (status.error.has_value() && !stable_error(*status.error))) {
    return "execution_backend_status_shape_invalid";
  }
  switch (status.state) {
    case ExecutionBackendStateDTO::kAccepted:
    case ExecutionBackendStateDTO::kExecuting:
    case ExecutionBackendStateDTO::kPaused:
    case ExecutionBackendStateDTO::kCompleted:
      return status.error.has_value()
                 ? std::optional<std::string_view>("execution_backend_status_error_invalid")
                 : std::nullopt;
    case ExecutionBackendStateDTO::kFailed:
      return !status.error.has_value()
                 ? std::optional<std::string_view>("execution_backend_status_error_missing")
                 : std::nullopt;
    case ExecutionBackendStateDTO::kCancelled:
      return std::nullopt;
  }
  return "execution_backend_status_state_invalid";
}

Result<ExecutionBackendStatusDTO> MakeExecutionBackendStatus(
    const PreparedOperationParticipantDTO& participant, const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation, ExecutionBackendStateDTO state,
    double accepted_trajectory_time_s, double progress, std::uint64_t source_sequence,
    std::int64_t receiver_local_receipt_timestamp_ns, std::optional<ErrorDTO> error) {
  ExecutionBackendStatusDTO status{participant.operation_id,
                                   participant.operation_revision,
                                   participant.participant_id,
                                   participant.controllable_component_id,
                                   participant.command_target_id,
                                   participant.capability_id,
                                   participant.control_domain_id,
                                   participant.control_domain_revision,
                                   participant.control_binding_id,
                                   participant.control_binding_revision,
                                   participant.controller_topology_id,
                                   participant.topology_revision,
                                   participant.control_route_id,
                                   participant.active_control_configuration_id,
                                   participant.active_control_configuration_generation,
                                   controller_goal_id,
                                   controller_goal_generation,
                                   participant.control_provider.runtime_epoch,
                                   participant.resources,
                                   state,
                                   accepted_trajectory_time_s,
                                   progress,
                                   source_sequence,
                                   receiver_local_receipt_timestamp_ns,
                                   std::move(error)};
  if (const auto field_error = execution_backend_status_field_error(
          status, participant, controller_goal_id, controller_goal_generation)) {
    return Result<ExecutionBackendStatusDTO>::Err(invalid_projection(*field_error));
  }
  return Result<ExecutionBackendStatusDTO>::Ok(std::move(status));
}

}  // namespace manyforge::core
