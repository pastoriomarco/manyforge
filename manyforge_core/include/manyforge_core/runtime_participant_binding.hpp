#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/observed_state.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_core/types/lifecycle_provider_occurrence.hpp>
#include <manyforge_core/types/participant_binding_identity.hpp>
#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace manyforge::core {

struct RuntimeUnitEpochBindingDTO final {
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch runtime_epoch;

  friend bool operator==(const RuntimeUnitEpochBindingDTO&,
                         const RuntimeUnitEpochBindingDTO&) = default;
};

struct RuntimeModelGroupBindingDTO final {
  ModelGroupRoleId model_group_role_id;
  ModelInstanceId model_instance_id;
  std::string compiled_group_name;
  /// Exact expanded SRDF membership retained by the Deployment Compiler.
  /// This includes fixed joints and therefore is not a command-coordinate
  /// vector. The ordered command projection lives in `resources`.
  std::vector<std::string> expanded_compiled_joint_names;

  friend bool operator==(const RuntimeModelGroupBindingDTO&,
                         const RuntimeModelGroupBindingDTO&) = default;
};

struct RuntimeResourceBindingDTO final {
  ResourceRoleId resource_role_id;
  ResourceId resource_id;
  /// Exact source-local URDF joint for model-backed coordinates. Absent for a
  /// non-model resource. Never contains a generated/prefixed runtime name.
  std::optional<std::string> source_joint_name;
  std::string compiled_resource_name;
  CoordinateKind coordinate_kind{CoordinateKind::kAngular};

  friend bool operator==(const RuntimeResourceBindingDTO&,
                         const RuntimeResourceBindingDTO&) = default;
};

struct RuntimeControlPathDTO final {
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
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  StateAuthorityId state_authority_id;
  StateAuthorityClass state_authority_class{StateAuthorityClass::kMeasured};
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  ContainmentGranularity containment_granularity{ContainmentGranularity::kResource};
  /// Exact route-specific retained scaling evidence. Absence is an explicit
  /// unsupported result, never permission for adapter inference.
  std::optional<RuntimeScaleRouteQualificationDTO> runtime_scale_qualification;

  friend bool operator==(const RuntimeControlPathDTO&, const RuntimeControlPathDTO&) = default;
};

struct RuntimeCapabilityProviderPathDTO final {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  CapabilityProviderBindingId capability_provider_binding_id;
  CapabilityProviderBindingRevision capability_provider_binding_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;

  friend bool operator==(const RuntimeCapabilityProviderPathDTO&,
                         const RuntimeCapabilityProviderPathDTO&) = default;
};

/// Exact selected endpoint. Body IDs follow the existing semantic-to-physical
/// alias projection; this record does not grant motion ownership to the tool.
struct RuntimeMotionEndpointBindingDTO final {
  TargetRoleId target_role_id;
  CommandTargetId command_target_id;
  ProgramFrameRoleBindingDTO frame;
  std::vector<ProgramContactGraspBodyBindingDTO> contact_body_bindings;
  friend bool operator==(const RuntimeMotionEndpointBindingDTO&,
                         const RuntimeMotionEndpointBindingDTO&) = default;
};

/// Active TCP (M7): the TCP entry of a resolved endpoint. `owner` is the
/// component owning the endpoint's frame (B7: the registration's
/// `motion_endpoint_owner`, else the motion component itself).
[[nodiscard]] inline TcpEntryDTO tcp_entry_of(const RuntimeMotionEndpointBindingDTO& endpoint,
                                              const ControllableComponentId& owner) {
  return TcpEntryDTO{owner, endpoint.command_target_id, endpoint.frame.frame_role_id};
}

/// Active TCP (M7): whether a move's or a Set TCP's selector admits the
/// resolved endpoint. Current and form 2 are resolved by the owner's resolver
/// (from the robot's Active TCP and the Program binding). An entry must name
/// the endpoint's target and frame and, where the owner is known (the Kernel,
/// from its registration), its owner; the sealed binding alone does not carry
/// the owner (B7), so the registry passes none.
[[nodiscard]] inline bool tcp_selector_admits(const TcpSelectorDTO& selector,
                                              const RuntimeMotionEndpointBindingDTO& endpoint,
                                              const std::optional<ControllableComponentId>& owner) {
  const auto* entry = std::get_if<TcpEntryDTO>(&selector);
  return entry == nullptr ||
         (entry->target_id == endpoint.command_target_id &&
          entry->frame_role_id == endpoint.frame.frame_role_id &&
          (!owner || entry->component_id == *owner));
}

/// Complete owner-to-owner value before sealing. Public operation callers
/// never construct or submit this record.
struct RuntimeParticipantBindingDTO final {
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ComponentRoleId component_role_id;
  TargetRoleId target_role_id;
  ControllableComponentId controllable_component_id;
  ComponentTypeId component_type_id;
  ComponentTypeRevision component_type_revision;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  std::optional<ProgramCommandSchemaRefDTO> command_schema;
  std::vector<ProgramModelRoleBindingDTO> model_bindings;
  std::vector<RuntimeModelGroupBindingDTO> model_groups;
  std::vector<ProgramFrameRoleBindingDTO> frame_bindings;
  std::vector<RuntimeResourceBindingDTO> resources;
  std::vector<ProgramCollisionBodyRoleBindingDTO> collision_body_bindings;
  std::optional<RuntimeControlPathDTO> control_path;
  std::optional<RuntimeCapabilityProviderPathDTO> capability_provider_path;
  /// Additional exact capability providers required to realize the selected
  /// participant payload. For motion, the selected execution capability has
  /// a control path while the motion-kind-specific planner is a separately
  /// bound provider. Public callers cannot add or select these paths.
  std::vector<RuntimeCapabilityProviderPathDTO> supporting_capability_provider_paths;
  std::vector<RuntimeUnitEpochBindingDTO> runtime_unit_epochs;
  /// Exact Program-owned templates copied from the sealed Program Binding.
  /// Runtime object selection remains unresolved until operation admission.
  std::vector<ProgramContactGraspBindingDTO> contact_grasp_bindings{};
  std::optional<RuntimeMotionEndpointBindingDTO> motion_endpoint{};

  friend bool operator==(const RuntimeParticipantBindingDTO&,
                         const RuntimeParticipantBindingDTO&) = default;
};

/// Shared observation/preparation identity projected only from sealed owner facts.
[[nodiscard]] Result<ParticipantBindingIdentityDTO> runtime_participant_binding_identity(
    const RuntimeParticipantBindingDTO& binding);

/// Exact semantic-body to projected-physical-body alias produced from one
/// verified retained model map. Distinct portable collision-body roles may
/// intentionally select the same source link; runtime collision work must
/// preserve the role while resolving both selectors to one physical body.
struct RuntimeCollisionBodyAliasDTO final {
  std::string semantic_body_id;
  std::string projected_body_id;

  friend bool operator==(const RuntimeCollisionBodyAliasDTO&,
                         const RuntimeCollisionBodyAliasDTO&) = default;
};

/// Sealed participant binding consumed by the resource-addressed Kernel.
class RuntimeParticipantBinding final {
 public:
  RuntimeParticipantBinding(const RuntimeParticipantBinding&) = default;
  RuntimeParticipantBinding& operator=(const RuntimeParticipantBinding&) = default;
  RuntimeParticipantBinding(RuntimeParticipantBinding&&) noexcept = default;
  RuntimeParticipantBinding& operator=(RuntimeParticipantBinding&&) noexcept = default;

  [[nodiscard]] const RuntimeParticipantBindingDTO& value() const noexcept { return value_; }

 private:
  explicit RuntimeParticipantBinding(RuntimeParticipantBindingDTO value)
      : value_(std::move(value)) {}

  RuntimeParticipantBindingDTO value_;

  friend Result<RuntimeParticipantBinding> verify_runtime_participant_binding(
      RuntimeParticipantBindingDTO);
  friend Result<RuntimeParticipantBinding> bind_runtime_participant_collision_body_aliases(
      RuntimeParticipantBinding, std::span<const RuntimeCollisionBodyAliasDTO>);
};

/// Validate ordering, uniqueness, scope agreement, and complete active facts,
/// then seal one owner-produced runtime participant.
[[nodiscard]] Result<RuntimeParticipantBinding> verify_runtime_participant_binding(
    RuntimeParticipantBindingDTO value);

/// Resolve already-verified Program role/index body identities to the exact
/// deduplicated physical bodies in the active retained collision projection.
/// This is an owner-to-owner adaptation after semantic binding verification;
/// public operation callers cannot supply the alias table.
[[nodiscard]] Result<RuntimeParticipantBinding> bind_runtime_participant_collision_body_aliases(
    RuntimeParticipantBinding binding, std::span<const RuntimeCollisionBodyAliasDTO> aliases);

/// Derive the exact observed-state query coordinates from one sealed runtime
/// participant. Freshness is an active runtime policy and is supplied
/// separately; every identity and ordered unit comes only from the binding.
[[nodiscard]] Result<ObservedStateQueryContextDTO> make_observed_state_query_context(
    const RuntimeParticipantBinding& binding, std::int64_t freshness_budget_ns);

/// Return true when `selected` is one Program-scoped view of the same exact
/// physical execution owner as `physical`. Program/role identities may differ;
/// model, resource, route, provider, and runtime-epoch facts may only narrow
/// by an unambiguous subset and can never cross the physical owner.
[[nodiscard]] bool runtime_participant_binding_physically_contains(
    const RuntimeParticipantBinding& physical,
    const RuntimeParticipantBinding& selected) noexcept;

}  // namespace manyforge::core
