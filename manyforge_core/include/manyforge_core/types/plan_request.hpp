#pragma once

#include <cstdint>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/resolved_frame.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

struct PlannerModelBindingDTO final {
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;

  friend bool operator==(const PlannerModelBindingDTO&, const PlannerModelBindingDTO&) = default;
};

struct PlannerModelGroupBindingDTO final {
  ModelGroupRoleId model_group_role_id;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::string compiled_group_name;
  std::vector<std::string> compiled_joint_names;

  friend bool operator==(const PlannerModelGroupBindingDTO&,
                         const PlannerModelGroupBindingDTO&) = default;
};

struct PlannerRuntimeUnitBindingDTO final {
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch runtime_epoch;

  friend bool operator==(const PlannerRuntimeUnitBindingDTO&,
                         const PlannerRuntimeUnitBindingDTO&) = default;
};

/// Exact capability-provider generation selected for candidate computation.
/// The participant's public capability remains the execution capability;
/// this separate owner-produced value proves which planner implementation is
/// allowed to realize the requested motion kind.
struct PlannerCapabilityBindingDTO final {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  CapabilityProviderBindingId capability_provider_binding_id;
  CapabilityProviderBindingRevision capability_provider_binding_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;

  friend bool operator==(const PlannerCapabilityBindingDTO&,
                         const PlannerCapabilityBindingDTO&) = default;
};

/// Immutable Kernel-selected source for one speculative moving replacement.
/// This carries provenance, not Replace permission or an execution owner.
struct MovingReplacementInputDTO final {
  ExecutionGoalKey source_goal;
  ExecutionBindingEpoch source_binding_epoch;
  double source_cursor_s{};
  TrajectoryDTO source_prefix_trajectory;
  double divergence_cursor_s{};
  ControlledStateDTO suffix_start_state;
  ResolvedFrameDTO source_base_frame;
  ResolvedFrameDTO source_tcp_frame;
  SpeedProfileDTO source_speed_profile;
  PerResourceDynamicsLimitsDTO source_per_resource_dynamics_limits;
  double remaining_preparation_budget_s{};
};

/// Kernel-produced, fully resolved planning request (spec 210 section 3.6).
/// Public callers never construct this type and no field permits selection by
/// a singleton robot/group spelling. Generated frame/joint names appear only
/// inside their exact compiler-owned model/resource binding.
struct PlanRequestDTO final {
  OperationId operation_id;
  OperationParticipantId participant_id;
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  ProgramRunId program_run_id;
  MoveId move_id;

  ControllableComponentId controllable_component_id;
  ComponentTypeId component_type_id;
  ComponentTypeRevision component_type_revision;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  PlannerCapabilityBindingDTO planner_capability_binding;
  std::vector<PlannerModelBindingDTO> model_bindings;
  PlannerModelGroupBindingDTO model_group_binding;

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
  RuntimeEpoch provider_runtime_epoch;
  std::vector<PlannerRuntimeUnitBindingDTO> runtime_unit_bindings;

  std::vector<ResourceId> resources;
  std::optional<ActionSpanDTO> action_span;
  ResolvedFrameDTO base_frame;
  ResolvedFrameDTO tcp_frame;
  ControlledStateDTO start_state;
  ResolvedMotionSpecDTO motion;
  TolerancesDTO tolerances;
  SpeedProfileDTO speed_profile;
  PlanningProfileDTO planning_profile{PlanningProfileDTO::kFirstFeasible};
  std::optional<std::string> contact_policy_id;
  PerResourceDynamicsLimitsDTO per_resource_dynamics_limits;
  double planning_timeout_s{5.0};
  /// Kernel-owned zero-based attempt ordinal for this logical motion plan.
  /// Candidate producers may combine it with a deployment seed to make retry
  /// diversity reproducible. It is execution metadata, not program intent.
  std::uint32_t planning_attempt_index{0U};
  /// Exact retained-bundle, graph, and logical-scene projection retained for
  /// this request. Production planners must reject an invalid or unavailable
  /// binding; silently planning against any newer member is not permitted.
  ProjectionLeaseBindingDTO projection_binding;

  /// Empty means the exact retained graph/scene has no attachment of that
  /// class. These ordered revisions plus the projection content hash prevent
  /// a candidate computed for a different attachment set from being admitted.
  std::vector<ComponentAttachmentRevision> component_attachment_revisions;
  std::vector<CarriedObjectAttachmentRevision> carried_object_attachment_revisions;

  /// Immutable world/policy handles retained by `projection_binding` for this
  /// exact planning call. These are private in-process owner projections, not
  /// public wire fields. A scene-aware production planner consumes them when
  /// constructing its native candidate scene; authoritative collision
  /// validation remains Kernel-owned.
  CollisionWorldSnapshotPtr retained_collision_world{};
  std::shared_ptr<const CollisionPolicySnapshot> retained_collision_policy{};
  std::optional<MovingReplacementInputDTO> moving_replacement{};
};

/// Planning result (spec §3.6)
struct PlanResultDTO {
  std::optional<TrajectoryDTO> trajectory;  // present on success
  std::optional<ErrorDTO> error;            // present on failure; absent on success
  /// Exact composite projection consumed by the planner. Required whenever
  /// `trajectory` is present and must equal the request projection revision.
  ProjectionRevisionDTO projection_revision;
};

}  // namespace manyforge::core
