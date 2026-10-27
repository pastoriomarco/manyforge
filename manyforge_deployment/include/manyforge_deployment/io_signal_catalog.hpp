#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

struct IoSignalProviderConfiguration final {
  struct GeometricContactEndpoint final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    core::FrameRoleId frame_role_id;
    friend bool operator==(const GeometricContactEndpoint &,
                           const GeometricContactEndpoint &) = default;
  };
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
  /// Existing retained motion endpoint observed by the geometric-contact
  /// provider. It is independent of the I/O signal's owning component/target;
  /// contact bodies are derived from this exact endpoint definition.
  std::optional<GeometricContactEndpoint> endpoint{};
  std::optional<core::SceneResourceId> target_scene_resource_id{};
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

/// CP9: one model-free pose-source row (`pose_sources`) of a runtime unit.
struct RetainedPoseSourceConfiguration final {
  core::RuntimeUnitId runtime_unit_id;
  core::ControllableComponentId controllable_component_id;
  /// Sorted, unique `poses/<object_id>/<policy_id>` of every binding.
  std::vector<std::string> stream_ids;
  /// The normalized unit-configuration row.
  core::JsonValueDTO row;
};

/// `poses/<object_id>/<policy_id>`: one stream (and provider-domain member)
/// per pose-source binding.
[[nodiscard]] std::string PoseSourceStreamId(std::string_view object_id,
                                             std::string_view policy_id);

/// Reads and checks the normalized `pose_sources` rows: hosted owner, at least
/// one binding, ids of `[a-z0-9_.-]+` with a stream id of at most 64 bytes,
/// unique (object_id, policy_id), unique required joint names. Typed reasons:
/// `pose_source_owner_mismatch`, `pose_source_bindings_empty`,
/// `pose_source_binding_id_invalid`, `pose_source_binding_duplicate`,
/// `pose_source_joint_names_duplicate`, `pose_source_configuration_invalid`.
[[nodiscard]] core::Result<std::vector<RetainedPoseSourceConfiguration>>
ResolveRetainedPoseSources(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// Rechecks direct provider scope against the retained component and actual
/// configured signal owners. Component-type role classification is proved by
/// the ordinary applicability qualifier before sealing. `scene.pose.observe`
/// on the `poses` target of a configured pose source has no endpoint.
[[nodiscard]] core::Result<void> ValidateNonCoordinateProviderTarget(
    const core::CellGraphDTO &graph,
    const core::CapabilityProviderBindingDTO &provider);

} // namespace manyforge::deployment
