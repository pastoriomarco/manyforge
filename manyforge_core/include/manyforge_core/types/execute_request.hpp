#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/kinematic_state_view.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/observed_state.hpp>
#include <manyforge_core/types/plan_request.hpp>
#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/runtime_object_binding.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/timeouts.hpp>
#include <manyforge_core/types/tolerances.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {

/// Exact binding evidence passed from the resource-addressed owner to its
/// private motion engine. The subordinate engine may use generated names only
/// through this owner-produced value; it cannot select a robot or group.
struct ResolvedExecutionBindingDTO final {
  OperationId operation_id;
  OperationParticipantId participant_id;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  ProgramRunId program_run_id;
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
  ResolvedFrameDTO base_frame;
  ResolvedFrameDTO tcp_frame;
  std::vector<ComponentAttachmentRevision> component_attachment_revisions;
  std::vector<CarriedObjectAttachmentRevision> carried_object_attachment_revisions;
  std::optional<RuntimeScaleRouteQualificationDTO> runtime_scale_qualification;

  /// Complete portable binding maps retained for Program reference
  /// resolution. Generated names remain adapter-private values inside these
  /// compiler-issued correspondences.
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  TargetRoleId target_role_id;
  std::vector<ProgramFrameRoleBindingDTO> frame_role_bindings;
  std::vector<ProgramCollisionBodyRoleBindingDTO> collision_body_role_bindings;

  friend bool operator==(const ResolvedExecutionBindingDTO&,
                         const ResolvedExecutionBindingDTO&) = default;
};

/// Owner-resolved contact-grasp template plus one public runtime-object
/// selection. Together with the enclosing `ResolvedExecutionBindingDTO` this
/// is the complete immutable effective binding; the execution engine validates
/// it against one retained scene projection before installing an ephemeral
/// collision-policy entry.
struct ResolvedContactGraspBindingDTO final {
  ContactPolicyId contact_policy_id;
  ContactPolicyRevision contact_policy_revision;
  RuntimeObjectId runtime_object_id;
  SceneResourceId selected_object_id;
  std::vector<CollisionBodyId> allowed_robot_body_ids;
  double max_penetration_m{};
  double terminal_window_start_fraction{};

  friend bool operator==(const ResolvedContactGraspBindingDTO&,
                         const ResolvedContactGraspBindingDTO&) = default;
};

/// Immutable input used by the focused reference resolver to capture all
/// kinematic views needed by one relative-motion request. Capture happens once
/// during admission; subsequent reference resolution performs no mutable
/// state or scene reads.
struct ProgramFrameResolutionCaptureDTO final {
  ResolvedExecutionBindingDTO binding;
  ControlledStateDTO start_state;
  ObservedStatePublicationDTO observed_start_publication;
  ProjectionLeaseBindingDTO projection_binding;
  std::shared_ptr<const LogicalSceneSnapshotDTO> logical_scene;
  std::vector<RuntimeObjectReferenceBindingDTO> runtime_object_bindings;
  std::vector<ReferenceDTO> requested_references;
  std::int64_t captured_at_ns{};
};

/// Immutable operation-start context for resolving a Program reference.
/// It binds the complete compiler role map, authority-selected start state,
/// and exact retained logical-scene projection into one value. Resolvers may
/// evaluate FK but may not reread mutable scene or controller truth.
struct ProgramFrameResolutionContextDTO final {
  ResolvedExecutionBindingDTO binding;
  ControlledStateDTO start_state;
  ObservedStatePublicationDTO observed_start_publication;
  ProjectionLeaseBindingDTO projection_binding;
  std::shared_ptr<const LogicalSceneSnapshotDTO> logical_scene;
  std::vector<RuntimeObjectReferenceBindingDTO> runtime_object_bindings;
  std::vector<KinematicFramePathDTO> kinematic_paths;
  std::vector<KinematicStatePathViewDTO> kinematic_state_path_views;
  std::int64_t captured_at_ns{};

  [[nodiscard]] bool valid() const noexcept {
    const bool envelope_valid =
        !binding.program_id.empty() && !binding.program_revision.empty() &&
        !binding.program_binding_id.empty() && !binding.program_run_id.empty() &&
        !binding.retained_bundle_id.empty() && !binding.graph_revision.empty() &&
        !binding.target_role_id.empty() && !binding.controllable_component_id.empty() &&
        !binding.command_target_id.empty() && !binding.capability_id.empty() &&
        !binding.model_bindings.empty() && !binding.frame_role_bindings.empty() &&
        projection_binding.valid() && logical_scene && logical_scene->locally_valid() &&
        captured_at_ns >= 0 &&
        projection_binding.projection_revision.retained_bundle_id == binding.retained_bundle_id &&
        projection_binding.projection_revision.graph_revision == binding.graph_revision &&
        logical_scene->revision == projection_binding.projection_revision.scene_revision &&
        start_state.controllable_component_id == binding.controllable_component_id &&
        start_state.command_target_id == binding.command_target_id &&
        start_state.control_binding_id == binding.control_binding_id &&
        start_state.control_binding_revision == binding.control_binding_revision &&
        start_state.active_control_configuration_id == binding.active_control_configuration_id &&
        start_state.active_control_configuration_generation ==
            binding.active_control_configuration_generation &&
        start_state.resources == binding.resources &&
        observed_start_publication.controllable_component_id == binding.controllable_component_id &&
        observed_start_publication.command_target_id == binding.command_target_id &&
        observed_start_publication.control_binding_id == binding.control_binding_id &&
        observed_start_publication.control_binding_revision == binding.control_binding_revision &&
        observed_start_publication.active_control_configuration_id ==
            binding.active_control_configuration_id &&
        observed_start_publication.active_control_configuration_generation ==
            binding.active_control_configuration_generation &&
        observed_start_publication.runtime_epoch == binding.provider_runtime_epoch &&
        observed_start_publication.snapshot.resources == binding.resources &&
        observed_start_publication.snapshot.joint_names == start_state.joint_names &&
        observed_start_publication.snapshot.positions == start_state.positions &&
        observed_start_publication.snapshot.velocities == start_state.velocities &&
        kinematic_paths.size() == kinematic_state_path_views.size();
    if (!envelope_valid) {
      return false;
    }
    for (std::size_t index = 0U; index < runtime_object_bindings.size(); ++index) {
      const auto& binding_row = runtime_object_bindings[index];
      if (binding_row.runtime_object_id.empty() || binding_row.scene_resource_id.empty() ||
          (index != 0U && !(runtime_object_bindings[index - 1U].runtime_object_id <
                            binding_row.runtime_object_id))) {
        return false;
      }
    }
    for (std::size_t index = 0U; index < kinematic_paths.size(); ++index) {
      const auto& path = kinematic_paths[index];
      const auto& view = kinematic_state_path_views[index];
      if (compiled_frame_name(path.terminal_frame).empty() || path != view.query.frame_path ||
          view.query.retained_bundle_id != binding.retained_bundle_id ||
          view.query.graph_revision != binding.graph_revision ||
          !IsExactLowercaseSha256Digest(view.view_digest.value()) ||
          (index != 0U && !(compiled_frame_name(kinematic_paths[index - 1U].terminal_frame) <
                            compiled_frame_name(path.terminal_frame)))) {
        return false;
      }
    }
    return true;
  }
};

/// Execute request - kernel input (spec §3.10)
struct ExecuteRequestDTO {
  explicit ExecuteRequestDTO(ResolvedExecutionBindingDTO exact_binding)
      : binding(std::move(exact_binding)) {}

  ResolvedExecutionBindingDTO binding;
  std::string move_id;
  TolerancesDTO tolerances;
  SpeedProfileDTO speed_profile;
  TimeoutsDTO timeouts;
  CacheMode cache_mode;
  ResolvedMotionSpecDTO motion;
  PlanningProfileDTO planning_profile = PlanningProfileDTO::kFirstFeasible;
  MotionTargetModeDTO target_mode = MotionTargetModeDTO::kAbsolute;
  /// Required for relative pose/Cartesian targets; forbidden for absolute
  /// targets and relative joint deltas. Resolution is kernel-owned and
  /// revision-bound.
  std::optional<ReferenceDTO> target_reference;
  /// Optional coordinate basis for relative pose/Cartesian offsets. Defaults
  /// to target_reference when omitted.
  std::optional<ReferenceDTO> offset_reference;
  /// Admission-resolved typed blackboard-object bindings used by
  /// `ReferenceDTO::kRuntimeObject`. Empty unless this request names a runtime
  /// object; callers cannot rely on string equality with SceneResourceId.
  std::vector<RuntimeObjectReferenceBindingDTO> runtime_object_bindings;
  /// Present only when `contact_policy_id` selected a Program-Binding-owned
  /// geometry-only contact-grasp template. Public callers cannot populate the
  /// compiled body ids carried by this value.
  std::optional<ResolvedContactGraspBindingDTO> resolved_contact_grasp;
  /// Per-move bindings that arm `scope_kind == kMove` interrupt
  /// policies for exactly this execute request (spec/260 §2.6 +
  /// spec/210 §3.10). Atomic with dispatch: the kernel arms scopes
  /// after planning and validation pass and immediately before
  /// backend dispatch, then auto-reaps on any terminal execution
  /// state. Empty preserves free-move semantics.
  std::vector<MotionInterruptScopeDTO> motion_interrupt_scopes;
  /// Per-joint absolute dynamics caps (Phase 4-P5.1K). Empty preserves prior
  /// behavior. Resolved against model/backend/controller limits and enforced in
  /// time-parameterization; copied into the PlanRequestDTO by the kernel.
  PerResourceDynamicsLimitsDTO per_resource_dynamics_limits;
  /// Causal action-span envelope for this motion request (spec/210 §3.1.1).
  /// Empty means the caller has no span context; the kernel does not mint
  /// behavior-node attempts.
  std::optional<ActionSpanDTO> action_span;
  /// Effective concrete contact-policy binding for this move. Empty means
  /// free-space validation/execution.
  std::string contact_policy_id;
  /// Owner-resolved scale captured atomically with execution admission. This
  /// prevents a prior operation's participant-local scale from leaking into a
  /// newly dispatched operation before the first focused-port update.
  std::optional<ResolvedRuntimeScaleCommandDTO> initial_runtime_scale;
  /// Kernel-owned count of actual carried-versus-detached contacts discovered
  /// from waypoint zero before candidate planning. Public operation intent has
  /// no corresponding field; the value is evidence for the exact retained
  /// admission policy carried through planning and validation.
  std::size_t dynamically_discovered_start_contact_count{};
};

}  // namespace manyforge::core
