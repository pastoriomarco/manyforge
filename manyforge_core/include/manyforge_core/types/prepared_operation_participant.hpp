#pragma once

#include <cstdint>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/io_rule_stop_response.hpp>
#include <manyforge_core/types/lifecycle_provider_occurrence.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_core/types/participant_binding_identity.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <manyforge_core/types/validation.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kPreparedOperationParticipantApiVersion =
    "manyforge.prepared_operation_participant.v2";
inline constexpr std::string_view kPreparedOperationParticipantIdentityDomain =
    "prepared-operation-participant-contract-digest";

struct PreparedProviderRuntimeOccurrenceDTO final {
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch runtime_epoch;
  friend bool operator==(const PreparedProviderRuntimeOccurrenceDTO&,
                         const PreparedProviderRuntimeOccurrenceDTO&) = default;
};

struct PreparedCapabilityProviderRuntimeOccurrenceDTO final {
  CapabilityProviderBindingId capability_provider_binding_id;
  CapabilityProviderBindingRevision capability_provider_binding_revision;
  PreparedProviderRuntimeOccurrenceDTO provider;
  friend bool operator==(const PreparedCapabilityProviderRuntimeOccurrenceDTO&,
                         const PreparedCapabilityProviderRuntimeOccurrenceDTO&) = default;
};

struct PreparedParticipantFocusedPortDTO final {
  RuntimeUnitId runtime_unit_id;
  friend bool operator==(const PreparedParticipantFocusedPortDTO&,
                         const PreparedParticipantFocusedPortDTO&) = default;
};
struct PreparedParticipantSelectedRouteDTO final {
  std::vector<ControllerLifecycleExpectedEndpointDTO> endpoints;
  friend bool operator==(const PreparedParticipantSelectedRouteDTO&,
                         const PreparedParticipantSelectedRouteDTO&) = default;
};
using PreparedParticipantEndpointContractDTO =
    std::variant<PreparedParticipantFocusedPortDTO, PreparedParticipantSelectedRouteDTO>;

struct PreparedMotionPlanningEvidenceDTO final {
  PlanResultDTO planning_result;
  std::uint32_t planning_attempt_index{};
  PlannerCapabilityBindingDTO planner_capability_binding;
};
struct PreparedMotionValidationEvidenceDTO final {
  ValidationRevision validation_revision;
  ValidationResultDTO noncollision_result;
  CollisionWorldRevision collision_world_revision;
  CollisionPolicyRevision collision_policy_revision;
  CollisionDecision collision_decision;
};
struct PreparedContactPolicyRefDTO final {
  ContactPolicyId contact_policy_id;
  ContactPolicyRevision contact_policy_revision;
  friend bool operator==(const PreparedContactPolicyRefDTO&,
                         const PreparedContactPolicyRefDTO&) = default;
};
struct PreparedMotionInterruptScopeDTO final {
  MotionInterruptScopeId scope_id;
  InterruptPolicyId interrupt_policy_id;
  MotionInterruptActivationMode activation_mode{
      MotionInterruptActivationMode::kRequireInactiveAtArm};
  std::optional<std::int64_t> arm_timeout_ns;
  bool required{true};
  /// Desired completion requires its profile; fault actions use the enclosing
  /// ordinary Stop plan and never fabricate a desired completion profile.
  std::optional<MotionInterruptStopProfileDTO> stop_profile;
  MotionInterruptNodeCategory node_category{MotionInterruptNodeCategory::kGeneric};
  InterruptStructuredValue metadata;
  PreparedParticipantStopPlanDTO stop_plan;
  InterruptPreemptionAction preemption_action{InterruptPreemptionAction::kCompleteMoveOnSignal};
  std::optional<IoRulePreparedStopEntryDTO> stop_entry{};
  /// Exact last qualified containing stage accepted by this desired response.
  /// Absent only for legacy scopes without a sealed entry.
  std::optional<IoRulePreparedStopEntryDTO> success_through{};
};
struct PreparedMotionExecutionDTO final {
  MoveId move_id;
  PlanRequestDTO plan_request;
  TrajectoryDTO trajectory;
  ControlledStateDTO start_state;
  PreparedMotionPlanningEvidenceDTO planning_evidence;
  PreparedMotionValidationEvidenceDTO validation_evidence;
  std::optional<PreparedContactPolicyRefDTO> contact_policy;
  std::optional<double> requested_tcp_linear_speed_mps;
  double deployment_maximum_tcp_linear_speed_mps{};
  std::vector<PreparedMotionInterruptScopeDTO> interrupt_scopes;
  double execution_timeout_s{};
};

struct PreparedCapabilityCommandResourceBindingDTO final {
  ResourceRoleId resource_role_id;
  ResourceId resource_id;
  std::string interface_name;
  friend bool operator==(const PreparedCapabilityCommandResourceBindingDTO&,
                         const PreparedCapabilityCommandResourceBindingDTO&) = default;
};
struct PreparedCapabilityCommandChannelBindingDTO final {
  ChannelRoleId channel_role_id;
  ResourceId resource_id;
  std::string interface_name;
  friend bool operator==(const PreparedCapabilityCommandChannelBindingDTO&,
                         const PreparedCapabilityCommandChannelBindingDTO&) = default;
};
struct PreparedCapabilityCommandDTO final {
  CommandSchemaId command_schema_id;
  CommandSchemaRevision command_schema_revision;
  CapabilityCommandValueDTO command_value;
  std::vector<CommandDurationOptionDTO> validated_options;
  std::vector<PreparedCapabilityCommandResourceBindingDTO> resource_bindings;
  std::vector<PreparedCapabilityCommandChannelBindingDTO> channel_bindings;
  double completion_timeout_s{};
  CommandCompletionRule completion_rule{CommandCompletionRule::kGoalFencedTargetReached};
  CommandConfirmationRule confirmation_rule{CommandConfirmationRule::kBindingFeedbackRequired};
  std::vector<CommandStopEvidenceRequirement> stop_evidence_requirements;
};
struct PreparedIoOutputCommandDTO final {
  CommandSchemaId command_schema_id;
  CommandSchemaRevision command_schema_revision;
  IoSignalWriteCommandValueDTO command_value;
  IoSignalId signal_id;
  ResourceId output_resource;
  ContentDigest signal_descriptor_digest;
  IoOutputCompletionPolicyDTO completion_policy;
};
using PreparedParticipantPayloadDTO =
    std::variant<PreparedMotionExecutionDTO, PreparedCapabilityCommandDTO,
                 PreparedIoOutputCommandDTO>;

struct PreparedControllerExecutionAuthorityDTO final {
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActivationSetId activation_set_id;
  PreparedProviderRuntimeOccurrenceDTO control_provider;
  std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO> capability_provider;
  PreparedParticipantEndpointContractDTO endpoint_contract;
  std::vector<ControllerInterfaceInstanceDTO> claimed_command_interfaces;
  std::vector<ControllerInterfaceInstanceDTO> claimed_state_interfaces;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  PreparedParticipantStopPlanDTO ordinary_stop_plan;
  std::optional<PreparedParticipantStopPlanDTO> fault_stop_plan;
};

struct PreparedProviderExecutionAuthorityDTO final {
  ResolvedLifecycleProviderOccurrenceDTO lifecycle_provider;
  ContentDigest signal_configuration_digest;
  PreparedCapabilityProviderRuntimeOccurrenceDTO capability_provider;
};

struct PreparedOperationParticipantDTO final {
  ContentDigest prepared_contract_digest;
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  ProgramRunId program_run_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ComponentTypeId component_type_id;
  ComponentTypeRevision component_type_revision;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  std::vector<ProgramModelRoleBindingDTO> model_bindings;
  std::vector<ProgramFrameRoleBindingDTO> frame_bindings;
  ParticipantBindingIdentityDTO binding_identity;
  std::vector<ResourceId> resources;
  std::vector<RuntimeUnitEpochBindingDTO> runtime_unit_epochs;
  PreparedParticipantPayloadDTO payload;
  std::variant<PreparedControllerExecutionAuthorityDTO, PreparedProviderExecutionAuthorityDTO>
      execution_authority;
  std::optional<RuntimeMotionEndpointBindingDTO> motion_endpoint{};

  PreparedOperationParticipantDTO(
      ContentDigest prepared_contract_digest, OperationId operation_id,
      OperationRevision operation_revision, OperationParticipantId participant_id,
      ProgramId program_id, ProgramRevision program_revision, ProgramBindingId program_binding_id,
      ProgramRunId program_run_id, RetainedBundleId retained_bundle_id,
      GraphRevision graph_revision, ControllableComponentId controllable_component_id,
      ComponentLifecycleGeneration component_lifecycle_generation,
      ComponentTypeId component_type_id, ComponentTypeRevision component_type_revision,
      CommandTargetId command_target_id, CapabilityId capability_id,
      CapabilityDefinitionRevision capability_definition_revision,
      std::vector<ProgramModelRoleBindingDTO> model_bindings,
      std::vector<ProgramFrameRoleBindingDTO> frame_bindings,
      ParticipantBindingIdentityDTO binding_identity, std::vector<ResourceId> resources,
      std::vector<RuntimeUnitEpochBindingDTO> runtime_unit_epochs,
      PreparedParticipantPayloadDTO payload,
      std::variant<PreparedControllerExecutionAuthorityDTO, PreparedProviderExecutionAuthorityDTO>
          execution_authority)
      : prepared_contract_digest(std::move(prepared_contract_digest)),
        operation_id(std::move(operation_id)),
        operation_revision(std::move(operation_revision)),
        participant_id(std::move(participant_id)),
        program_id(std::move(program_id)),
        program_revision(std::move(program_revision)),
        program_binding_id(std::move(program_binding_id)),
        program_run_id(std::move(program_run_id)),
        retained_bundle_id(std::move(retained_bundle_id)),
        graph_revision(std::move(graph_revision)),
        controllable_component_id(std::move(controllable_component_id)),
        component_lifecycle_generation(std::move(component_lifecycle_generation)),
        component_type_id(std::move(component_type_id)),
        component_type_revision(std::move(component_type_revision)),
        command_target_id(std::move(command_target_id)),
        capability_id(std::move(capability_id)),
        capability_definition_revision(std::move(capability_definition_revision)),
        model_bindings(std::move(model_bindings)),
        frame_bindings(std::move(frame_bindings)),
        binding_identity(std::move(binding_identity)),
        resources(std::move(resources)),
        runtime_unit_epochs(std::move(runtime_unit_epochs)),
        payload(std::move(payload)),
        execution_authority(std::move(execution_authority)) {}

  PreparedOperationParticipantDTO(
      ContentDigest prepared_contract_digest, OperationId operation_id,
      OperationRevision operation_revision, OperationParticipantId participant_id,
      ProgramId program_id, ProgramRevision program_revision, ProgramBindingId program_binding_id,
      ProgramRunId program_run_id, RetainedBundleId retained_bundle_id,
      GraphRevision graph_revision, ControllableComponentId controllable_component_id,
      ComponentLifecycleGeneration component_lifecycle_generation,
      ComponentTypeId component_type_id, ComponentTypeRevision component_type_revision,
      CommandTargetId command_target_id, CapabilityId capability_id,
      CapabilityDefinitionRevision capability_definition_revision,
      std::vector<ProgramModelRoleBindingDTO> model_bindings,
      std::vector<ProgramFrameRoleBindingDTO> frame_bindings, ControlDomainId control_domain_id,
      ControlDomainRevision control_domain_revision, ControlBindingId control_binding_id,
      ControlBindingRevision control_binding_revision,
      ControlInterfaceContractId control_interface_contract_id,
      ControlInterfaceRevision control_interface_revision,
      ControllerTopologyId controller_topology_id, TopologyRevision topology_revision,
      ControlRouteId control_route_id, ActivationSetId activation_set_id,
      PreparedProviderRuntimeOccurrenceDTO control_provider,
      std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO> capability_provider,
      std::vector<ResourceId> resources, PreparedParticipantEndpointContractDTO endpoint_contract,
      std::vector<ControllerInterfaceInstanceDTO> claimed_command_interfaces,
      std::vector<ControllerInterfaceInstanceDTO> claimed_state_interfaces,
      ActiveControlConfigurationId active_control_configuration_id,
      ActiveControlConfigurationGeneration active_control_configuration_generation,
      std::vector<RuntimeUnitEpochBindingDTO> runtime_unit_epochs,
      StopEvidencePolicyId stop_evidence_policy_id,
      StopEvidencePolicyRevision stop_evidence_policy_revision,
      PreparedParticipantStopPlanDTO ordinary_stop_plan, PreparedParticipantPayloadDTO payload,
      std::optional<PreparedParticipantStopPlanDTO> fault_stop_plan = {})
      : prepared_contract_digest(std::move(prepared_contract_digest)),
        operation_id(std::move(operation_id)),
        operation_revision(std::move(operation_revision)),
        participant_id(std::move(participant_id)),
        program_id(std::move(program_id)),
        program_revision(std::move(program_revision)),
        program_binding_id(std::move(program_binding_id)),
        program_run_id(std::move(program_run_id)),
        retained_bundle_id(std::move(retained_bundle_id)),
        graph_revision(std::move(graph_revision)),
        controllable_component_id(std::move(controllable_component_id)),
        component_lifecycle_generation(std::move(component_lifecycle_generation)),
        component_type_id(std::move(component_type_id)),
        component_type_revision(std::move(component_type_revision)),
        command_target_id(std::move(command_target_id)),
        capability_id(std::move(capability_id)),
        capability_definition_revision(std::move(capability_definition_revision)),
        model_bindings(std::move(model_bindings)),
        frame_bindings(std::move(frame_bindings)),
        binding_identity{std::move(control_domain_id), std::move(control_domain_revision),
                         ControllerParticipantBindingIdentityDTO{
                             std::move(control_binding_id), std::move(control_binding_revision),
                             capability_provider
                                 ? std::optional(CapabilityProviderBindingIdentityDTO{
                                       capability_provider->capability_provider_binding_id,
                                       capability_provider->capability_provider_binding_revision})
                                 : std::nullopt}},
        resources(std::move(resources)),
        runtime_unit_epochs(std::move(runtime_unit_epochs)),
        payload(std::move(payload)),
        execution_authority(PreparedControllerExecutionAuthorityDTO{
            std::move(control_interface_contract_id), std::move(control_interface_revision),
            std::move(controller_topology_id), std::move(topology_revision),
            std::move(control_route_id), std::move(activation_set_id), std::move(control_provider),
            std::move(capability_provider), std::move(endpoint_contract),
            std::move(claimed_command_interfaces), std::move(claimed_state_interfaces),
            std::move(active_control_configuration_id),
            std::move(active_control_configuration_generation), std::move(stop_evidence_policy_id),
            std::move(stop_evidence_policy_revision), std::move(ordinary_stop_plan),
            std::move(fault_stop_plan)}) {}

  [[nodiscard]] PreparedControllerExecutionAuthorityDTO& controller() {
    return std::get<PreparedControllerExecutionAuthorityDTO>(execution_authority);
  }
  [[nodiscard]] const PreparedControllerExecutionAuthorityDTO& controller() const {
    return std::get<PreparedControllerExecutionAuthorityDTO>(execution_authority);
  }
  [[nodiscard]] const PreparedProviderExecutionAuthorityDTO* provider() const noexcept {
    return std::get_if<PreparedProviderExecutionAuthorityDTO>(&execution_authority);
  }
};

}  // namespace manyforge::core
