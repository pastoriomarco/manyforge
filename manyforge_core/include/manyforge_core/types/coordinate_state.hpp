#pragma once

#include <cstdint>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

enum class CoordinateKind : std::uint8_t {
  kAngular = 0,
  kLinear,
};

[[nodiscard]] constexpr std::string_view to_string(
    CoordinateKind value) noexcept {
  switch (value) {
    case CoordinateKind::kAngular:
      return "angular";
    case CoordinateKind::kLinear:
      return "linear";
  }
  return {};
}

struct AngularPositionDTO final {
  double value_rad{};
  friend bool operator==(const AngularPositionDTO&,
                         const AngularPositionDTO&) = default;
};

struct LinearPositionDTO final {
  double value_m{};
  friend bool operator==(const LinearPositionDTO&,
                         const LinearPositionDTO&) = default;
};

using CoordinatePositionDTO =
    std::variant<AngularPositionDTO, LinearPositionDTO>;

struct AngularVelocityDTO final {
  double value_rad_s{};
  friend bool operator==(const AngularVelocityDTO&,
                         const AngularVelocityDTO&) = default;
};

struct LinearVelocityDTO final {
  double value_m_s{};
  friend bool operator==(const LinearVelocityDTO&,
                         const LinearVelocityDTO&) = default;
};

using CoordinateVelocityDTO =
    std::variant<AngularVelocityDTO, LinearVelocityDTO>;

struct AngularAccelerationDTO final {
  double value_rad_s2{};
  friend bool operator==(const AngularAccelerationDTO&,
                         const AngularAccelerationDTO&) = default;
};

struct LinearAccelerationDTO final {
  double value_m_s2{};
  friend bool operator==(const LinearAccelerationDTO&,
                         const LinearAccelerationDTO&) = default;
};

using CoordinateAccelerationDTO =
    std::variant<AngularAccelerationDTO, LinearAccelerationDTO>;

struct AngularEffortDTO final {
  double value_nm{};
  friend bool operator==(const AngularEffortDTO&,
                         const AngularEffortDTO&) = default;
};

struct LinearEffortDTO final {
  double value_n{};
  friend bool operator==(const LinearEffortDTO&,
                         const LinearEffortDTO&) = default;
};

using CoordinateEffortDTO = std::variant<AngularEffortDTO, LinearEffortDTO>;

[[nodiscard]] constexpr CoordinateKind coordinate_kind(
    const CoordinatePositionDTO& value) noexcept {
  return std::holds_alternative<AngularPositionDTO>(value)
             ? CoordinateKind::kAngular
             : CoordinateKind::kLinear;
}

[[nodiscard]] constexpr CoordinateKind coordinate_kind(
    const CoordinateVelocityDTO& value) noexcept {
  return std::holds_alternative<AngularVelocityDTO>(value)
             ? CoordinateKind::kAngular
             : CoordinateKind::kLinear;
}

[[nodiscard]] constexpr CoordinateKind coordinate_kind(
    const CoordinateAccelerationDTO& value) noexcept {
  return std::holds_alternative<AngularAccelerationDTO>(value)
             ? CoordinateKind::kAngular
             : CoordinateKind::kLinear;
}

[[nodiscard]] constexpr CoordinateKind coordinate_kind(
    const CoordinateEffortDTO& value) noexcept {
  return std::holds_alternative<AngularEffortDTO>(value)
             ? CoordinateKind::kAngular
             : CoordinateKind::kLinear;
}

struct ControlledStateDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  std::vector<ResourceId> resources;
  std::vector<std::string> joint_names;
  std::vector<CoordinatePositionDTO> positions;
  std::optional<std::vector<CoordinateVelocityDTO>> velocities;

  friend bool operator==(const ControlledStateDTO&,
                         const ControlledStateDTO&) = default;
};

enum class ObservedStateSourceKind : std::uint8_t {
  kActionFeedback = 0,
  kControllerState,
  kJointState,
  kCommandedState,
  kPredictedState,
};

[[nodiscard]] constexpr std::string_view to_string(
    ObservedStateSourceKind value) noexcept {
  switch (value) {
    case ObservedStateSourceKind::kActionFeedback:
      return "action_feedback";
    case ObservedStateSourceKind::kControllerState:
      return "controller_state";
    case ObservedStateSourceKind::kJointState:
      return "joint_state";
    case ObservedStateSourceKind::kCommandedState:
      return "commanded_state";
    case ObservedStateSourceKind::kPredictedState:
      return "predicted_state";
  }
  return {};
}

struct ObservedStateSnapshotDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  std::vector<ResourceId> resources;
  std::uint64_t sequence{};
  std::vector<std::string> joint_names;
  std::vector<CoordinatePositionDTO> positions;
  std::optional<std::vector<CoordinateVelocityDTO>> velocities;
  std::string source_id;
  RuntimeEpoch source_epoch;
  ObservedStateSourceKind source_kind{ObservedStateSourceKind::kJointState};
  std::int64_t source_timestamp_ns{};
  std::int64_t received_timestamp_ns{};
  StateAuthorityId state_authority_id;
  StateAuthorityClass state_authority_class{StateAuthorityClass::kMeasured};
  std::uint64_t authority_generation{};
  std::int64_t freshness_budget_ns{};
  bool complete{};

  friend bool operator==(const ObservedStateSnapshotDTO&,
                         const ObservedStateSnapshotDTO&) = default;
};

/// Validate the shape, finiteness, unit agreement, and immutable addressed
/// identity fields of one controlled-state value.
[[nodiscard]] Result<void> validate_controlled_state(
    const ControlledStateDTO& state);

/// Validate one observed-state publication, including source/authority and
/// measured-freshness rules. `now_ns` is the receiver's injected monotonic
/// time; negative values reject.
[[nodiscard]] Result<void> validate_observed_state_snapshot(
    const ObservedStateSnapshotDTO& state, std::int64_t now_ns);

/// Produce the legacy all-angular value projection required by the existing
/// TrajectoryDTO/planner subset. Mixed or linear state fails closed.
[[nodiscard]] Result<std::vector<double>> angular_position_projection(
    const ControlledStateDTO& state);

}  // namespace manyforge::core
