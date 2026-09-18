#pragma once

#include <cstdint>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <manyforge_core/types/runtime_policy.hpp>
#include <manyforge_core/types/validation.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kPreparedOperationParticipantApiVersion =
    "manyforge.prepared_operation_participant.v1";
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
struct PreparedRuntimePolicyRefDTO final {
  /// Exact retained artifact accepted by Deployment for this operation.
  RuntimePolicyCatalogRecordRefDTO source_artifact;
  friend bool operator==(const PreparedRuntimePolicyRefDTO&,
                         const PreparedRuntimePolicyRefDTO&) = default;
};
struct PreparedMotionInterruptScopeDTO final {
  MotionInterruptScopeId scope_id;
  InterruptPolicyId interrupt_policy_id;
  MotionInterruptActivationMode activation_mode{
      MotionInterruptActivationMode::kRequireInactiveAtArm};
  std::optional<std::int64_t> arm_timeout_ns;
  std::optional<std::string> result_binding_key;
  bool required{true};
  MotionInterruptStopProfileDTO stop_profile;
  MotionInterruptNodeCategory node_category{MotionInterruptNodeCategory::kGeneric};
  InterruptStructuredValue metadata;
  PreparedParticipantStopPlanDTO stop_plan;
};
struct PreparedMotionExecutionDTO final {
  MoveId move_id;
  PlanRequestDTO plan_request;
  TrajectoryDTO trajectory;
  ControlledStateDTO start_state;
  PreparedMotionPlanningEvidenceDTO planning_evidence;
  PreparedMotionValidationEvidenceDTO validation_evidence;
  std::optional<PreparedContactPolicyRefDTO> contact_policy;
  PreparedRuntimePolicyRefDTO runtime_policy;
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
using PreparedParticipantPayloadDTO =
    std::variant<PreparedMotionExecutionDTO, PreparedCapabilityCommandDTO>;

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
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActivationSetId activation_set_id;
  PreparedProviderRuntimeOccurrenceDTO control_provider;
  std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO> capability_provider;
  std::vector<ResourceId> resources;
  PreparedParticipantEndpointContractDTO endpoint_contract;
  std::vector<ControllerInterfaceInstanceDTO> claimed_command_interfaces;
  std::vector<ControllerInterfaceInstanceDTO> claimed_state_interfaces;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  std::vector<RuntimeUnitEpochBindingDTO> runtime_unit_epochs;
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  PreparedParticipantStopPlanDTO ordinary_stop_plan;
  PreparedParticipantPayloadDTO payload;
  /// Exact deployment-owned runtime-fault plan for this admitted goal. It is
  /// absent for non-motion participants and deployments without a qualified
  /// controlled fault profile.
  std::optional<PreparedParticipantStopPlanDTO> fault_stop_plan;
};

}  // namespace manyforge::core
