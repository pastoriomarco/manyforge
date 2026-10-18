#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/io_signal.hpp>

namespace manyforge::deployment {

struct IoSignalProviderConfiguration final {
  std::string profile;
  std::optional<std::string> topic;
  std::optional<std::size_t> input_bit;
  std::optional<bool> initial_value;
  std::size_t ingress_depth{};
  /// Fully qualified channel on the existing host's shared ROS context.
  /// Never a portable command operand or a caller-selected reservation key.
  std::optional<std::string> service{};
  /// One actual synthetic output channel, shared by every local alias.
  std::optional<std::string> channel{};
  std::optional<std::size_t> pin{};
  std::optional<std::string> group{};
  std::optional<std::string> interface_name{};
  friend bool operator==(const IoSignalProviderConfiguration &,
                         const IoSignalProviderConfiguration &) = default;
};

/// Resolved from the owning retained runtime unit's typed configuration, never
/// from a Program request. Addresses stay in this sealed configuration value.
struct RetainedIoSignalConfiguration final {
  core::RuntimeUnitId runtime_unit_id;
  core::CommandTargetId command_target_id;
  core::ResourceRoleId resource_role_id;
  core::IoSignalDescriptorDTO descriptor;
  IoSignalProviderConfiguration provider;
};

struct RetainedIoProviderDeviceConfiguration final {
  core::RuntimeUnitId runtime_unit_id;
  core::ControllableComponentId controllable_component_id;
  core::Duration lifecycle_command_timeout_ns;
  /// Required after normalization for actual ROS SetBool or UR SetIO outputs.
  /// Mock outputs have immediate synthetic acceptance and no deadline budget.
  std::optional<core::Duration> output_source_ack_timeout_ns{};
};

/// Applies the approved 30 s per-device default before schema normalization
/// and graph identity. Ordinary configurations gain no synthetic device rows;
/// the deferred xArm profile is not admitted in this R5 catalog.
[[nodiscard]] core::Result<void> MaterializeRetainedIoProviderDevices(
    core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// Reads explicit normalized per-device policy. Runtime admission consumes
/// the retained duration and never supplies its own fallback.
[[nodiscard]] core::Result<std::vector<RetainedIoProviderDeviceConfiguration>>
ResolveRetainedIoProviderDevices(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// A unit filter is only for the per-unit schema-normalization hook; graph
/// qualification, binding and activation always validate the complete catalog.
[[nodiscard]] core::Result<std::vector<RetainedIoSignalConfiguration>>
ResolveRetainedIoSignalCatalog(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// Rechecks direct provider scope against the retained component and actual
/// configured signal owners. Component-type role classification is proved by
/// the ordinary applicability qualifier before sealing.
[[nodiscard]] core::Result<void> ValidateNonCoordinateProviderTarget(
    const core::CellGraphDTO &graph,
    const core::CapabilityProviderBindingDTO &provider);

} // namespace manyforge::deployment
