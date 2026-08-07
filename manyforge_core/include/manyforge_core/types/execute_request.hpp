#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/timeouts.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/plan_request.hpp>
#include <manyforge_core/types/runtime_scale.hpp>

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
  std::vector<CarriedObjectAttachmentRevision>
      carried_object_attachment_revisions;
  std::optional<RuntimeScaleRouteQualificationDTO> runtime_scale_qualification;

  friend bool operator==(const ResolvedExecutionBindingDTO&,
                         const ResolvedExecutionBindingDTO&) = default;
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
  /// Required for relative pose/Cartesian targets; forbidden for joint and
  /// absolute targets. Resolution is kernel-owned and revision-bound.
  std::optional<ReferenceDTO> target_reference;
  /// Optional coordinate basis for relative pose/Cartesian offsets. Defaults
  /// to target_reference when omitted.
  std::optional<ReferenceDTO> offset_reference;
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
};

}  // namespace manyforge::core
