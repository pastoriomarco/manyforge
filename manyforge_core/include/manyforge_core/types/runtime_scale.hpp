#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

enum class RuntimeScaleApplicationState {
  kPending,
  kApplied,
  kRejected,
  kFailed,
};

struct RuntimeScaleControlTargetDTO final {
  ComponentRoleId component_role_id;
  TargetRoleId target_role_id;
  friend bool operator==(const RuntimeScaleControlTargetDTO&,
                         const RuntimeScaleControlTargetDTO&) = default;
};

/// Public selector for one operator runtime-velocity setting. The Kernel joins
/// these portable Program identities to the current exact control/scaling
/// domain; callers never supply controller or route identities.
struct RuntimeScaleControlSelectorDTO final {
  ProgramBindingId program_binding_id;
  ComponentRoleId component_role_id;
  TargetRoleId target_role_id;
  /// Alternative to the singular roles: the complete synchronized motion set.
  /// This selects control scopes, not Program membership or Run authority.
  std::vector<RuntimeScaleControlTargetDTO> synchronized_targets{};

  friend bool operator==(const RuntimeScaleControlSelectorDTO&,
                         const RuntimeScaleControlSelectorDTO&) = default;
};

/// Owner-scoped HMI setting. It persists across operations and Program Reset
/// for the current owner epoch; any live operation is selected by the Kernel.
struct RuntimeScaleControlRequestDTO final {
  PublicRequestContextDTO request_context;
  RuntimeScaleControlSelectorDTO selector;
  RuntimeScaleControlGeneration expected_control_generation;
  double scale{1.0};
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};

  friend bool operator==(const RuntimeScaleControlRequestDTO&,
                         const RuntimeScaleControlRequestDTO&) = default;
};

struct RuntimeScaleControlAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  RuntimeScaleControlSelectorDTO selector;
  double requested_scale{1.0};
  RuntimeScaleControlGeneration control_generation;
  std::optional<OperationId> active_operation_id;
  std::optional<OperationRevision> active_operation_revision;
  std::optional<RuntimeScaleGeneration> active_operation_scale_generation;
  std::int64_t accepted_at_ns{};
};

struct RuntimeScaleControlMemberDTO final {
  RuntimeScaleControlTargetDTO target;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ScalingDomainId scaling_domain_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
};

/// Owner projection consumed by a stable command-bar control. Exact resolved
/// identities prove scope and support; optional operation fields are
/// observation only and never become client-side routing authority.
struct RuntimeScaleControlStatusDTO final {
  RuntimeScaleControlSelectorDTO selector;
  RuntimeScaleControlGeneration control_generation;
  double requested_scale{1.0};
  bool supported{};
  bool frozen{};
  std::optional<ControlDomainId> control_domain_id;
  std::optional<ControlDomainRevision> control_domain_revision;
  std::optional<ControlBindingId> control_binding_id;
  std::optional<ControlBindingRevision> control_binding_revision;
  std::optional<ScalingDomainId> scaling_domain_id;
  std::optional<ActiveControlConfigurationId> active_control_configuration_id;
  std::optional<ActiveControlConfigurationGeneration>
      active_control_configuration_generation;
  std::optional<OperationId> active_operation_id;
  std::optional<OperationRevision> active_operation_revision;
  std::optional<RuntimeScaleGeneration> active_operation_scale_generation;
  std::optional<double> effective_scale;
  std::optional<RuntimeScaleApplicationState> application_state;
  std::optional<ErrorDTO> reason;
  std::vector<RuntimeScaleControlMemberDTO> synchronized_members{};
};

[[nodiscard]] constexpr const char* to_string(
    RuntimeScaleApplicationState state) noexcept {
  switch (state) {
    case RuntimeScaleApplicationState::kPending:
      return "pending";
    case RuntimeScaleApplicationState::kApplied:
      return "applied";
    case RuntimeScaleApplicationState::kRejected:
      return "rejected";
    case RuntimeScaleApplicationState::kFailed:
      return "failed";
  }
  return "failed";
}

/// Private subordinate-mechanism observation. Exact public identities and
/// generations are joined by the resource-addressed Kernel; a backend cannot
/// mint or select them.
/// Identity of the one command installed in an exact coordinated product's
/// shared time law. It is observation, not another participant acceptance.
struct CoordinatedRuntimeScaleCommandDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  RuntimeScaleGeneration generation;

  friend bool operator==(const CoordinatedRuntimeScaleCommandDTO&,
                         const CoordinatedRuntimeScaleCommandDTO&) = default;
};

struct RuntimeScaleMechanismStatusDTO final {
  bool supported{};
  bool active{};
  double commanded_scale{1.0};
  double applied_scale{1.0};
  double virtual_trajectory_time_s{};
  std::uint64_t acknowledgement_sequence{};
  std::int64_t acknowledgement_timestamp_ns{};
  std::optional<ErrorDTO> error;
  std::optional<CoordinatedRuntimeScaleCommandDTO> coordinated_command{};
};

struct RuntimeScaleRequestDTO final {
  PublicRequestContextDTO request_context;
  OperationId operation_id;
  OperationRevision expected_operation_revision;
  RuntimeScaleGeneration expected_runtime_scale_generation;
  double scale{1.0};
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};

  friend bool operator==(const RuntimeScaleRequestDTO&,
                         const RuntimeScaleRequestDTO&) = default;
};

struct RuntimeScaleAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  OperationId operation_id;
  OperationRevision operation_revision;
  double requested_scale{1.0};
  RuntimeScaleGeneration runtime_scale_generation;
  std::int64_t accepted_at_ns{};
};

struct RuntimeScaleParticipantStatusDTO final {
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ScalingDomainId scaling_domain_id;
  std::optional<CoordinatedScalingBarrierId> coordinated_barrier_id;
  std::vector<ResourceId> resources;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  RuntimeScaleGeneration accepted_generation;
  double commanded_scale{1.0};
  double applied_scale{1.0};
  double virtual_trajectory_time_s{};
  RuntimeEpoch acknowledgement_source_epoch;
  std::uint64_t acknowledgement_sequence{};
  std::int64_t acknowledgement_timestamp_ns{};
  bool supported{};
  RuntimeScaleApplicationState state{RuntimeScaleApplicationState::kPending};
  std::optional<ErrorDTO> error;
};

struct RuntimeScaleStatusDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  RuntimeScaleGeneration runtime_scale_generation;
  double requested_scale{1.0};
  double effective_scale{1.0};
  std::vector<RuntimeScaleParticipantStatusDTO> participants;
  RuntimeScaleApplicationState state{RuntimeScaleApplicationState::kPending};
  std::int64_t updated_at_ns{};
};

/// Exact Kernel-produced input to one resolved participant engine. Public
/// callers never construct this value: the resource-addressed owner joins the
/// active operation, retained binding, controller goal, and generation fence.
struct ResolvedRuntimeScaleCommandDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ScalingDomainId scaling_domain_id;
  std::optional<CoordinatedScalingBarrierId> coordinated_barrier_id;
  std::vector<ResourceId> resources;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  RuntimeScaleGeneration expected_runtime_scale_generation;
  RuntimeScaleGeneration requested_runtime_scale_generation;
  double requested_scale{1.0};

  friend bool operator==(const ResolvedRuntimeScaleCommandDTO&,
                         const ResolvedRuntimeScaleCommandDTO&) = default;
};

struct ResolvedRuntimeScaleAcceptedDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ScalingDomainId scaling_domain_id;
  std::optional<CoordinatedScalingBarrierId> coordinated_barrier_id;
  std::vector<ResourceId> resources;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  RuntimeScaleGeneration accepted_generation;
  double commanded_scale{1.0};
  std::uint64_t acceptance_sequence{};
  std::int64_t accepted_at_ns{};

  friend bool operator==(const ResolvedRuntimeScaleAcceptedDTO&,
                         const ResolvedRuntimeScaleAcceptedDTO&) = default;
};

struct ResolvedRuntimeScaleStatusDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ScalingDomainId scaling_domain_id;
  std::optional<CoordinatedScalingBarrierId> coordinated_barrier_id;
  std::vector<ResourceId> resources;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  RuntimeScaleGeneration accepted_generation;
  double commanded_scale{1.0};
  double applied_scale{1.0};
  double virtual_trajectory_time_s{};
  RuntimeEpoch acknowledgement_source_epoch;
  std::uint64_t acknowledgement_sequence{};
  std::int64_t acknowledgement_timestamp_ns{};
  RuntimeScaleApplicationState state{RuntimeScaleApplicationState::kPending};
  std::optional<ErrorDTO> error;

  friend bool operator==(const ResolvedRuntimeScaleStatusDTO&,
                         const ResolvedRuntimeScaleStatusDTO&) = default;
};

}  // namespace manyforge::core
