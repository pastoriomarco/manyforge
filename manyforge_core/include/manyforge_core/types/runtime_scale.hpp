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
struct RuntimeScaleMechanismStatusDTO final {
  bool supported{};
  bool active{};
  double commanded_scale{1.0};
  double applied_scale{1.0};
  double virtual_trajectory_time_s{};
  std::uint64_t acknowledgement_sequence{};
  std::int64_t acknowledgement_timestamp_ns{};
  std::optional<ErrorDTO> error;
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
