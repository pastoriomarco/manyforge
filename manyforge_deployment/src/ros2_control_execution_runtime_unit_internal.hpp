#pragma once

#include <cstddef>
#include <optional>
#include <manyforge_core/types/gripper.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <span>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <string>
#include <string_view>

namespace manyforge::deployment {

class CompilerQualifiedCellIr;

namespace detail {

inline constexpr std::string_view
    kRos2ControlExecutionRuntimeManifestApiVersion =
        "manyforge.ros2_control_execution_runtime_manifest.v1";
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeProductContractId =
        ros2_control_execution_runtime_contract::kProductContractId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeManifestArtifactRole = "runtime_manifest";
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeManifestMediaType =
        "application/vnd.manyforge.runtime-manifest+json";
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole =
        "launch_descriptor";
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeLaunchDescriptorMediaType =
        "application/vnd.manyforge.installed-target-launch+json";
inline constexpr std::string_view kRos2ControlExecutionRuntimeProfileId =
    ros2_control_execution_runtime_contract::kRuntimeProfileId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeConfigurationSchemaId =
        "manyforge.value_schema.runtime.ros2_control_execution.v1";
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeConfigurationSchemaRevision =
        "sha256:"
        "b37e963e21f9c72c47f713d7bc6b5f24fb680e2126d0f29e4cc4ed86fc9155ba";
inline constexpr std::string_view kRos2ControlExecutionRuntimeRoleId =
    ros2_control_execution_runtime_contract::kRuntimeRoleId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeStatusEndpointRoleId =
        ros2_control_execution_runtime_contract::kStatusEndpointRoleId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeProjectionEndpointRoleId =
        ros2_control_execution_runtime_contract::kProjectionEndpointRoleId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId =
        ros2_control_execution_runtime_contract::kLogicalSceneEndpointRoleId;
inline constexpr std::string_view kRos2ControlExecutionRuntimeImplementationId =
    ros2_control_execution_runtime_contract::kImplementationId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeProcessAdapterContractId =
        ros2_control_execution_runtime_contract::kProcessAdapterContractId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeRetrievalContractId =
        ros2_control_execution_runtime_contract::kRetrievalContractId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeStatusEndpointContractId =
        ros2_control_execution_runtime_contract::kStatusEndpointContractId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeProjectionEndpointContractId =
        ros2_control_execution_runtime_contract::kProjectionEndpointContractId;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeLogicalSceneEndpointContractId =
        ros2_control_execution_runtime_contract::kLogicalSceneEndpointContractId;
inline constexpr std::string_view kRos2ControlExecutionRuntimeOperatingSystem =
    ros2_control_execution_runtime_contract::kOperatingSystem;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeLaunchEnvironmentContract =
        ros2_control_execution_runtime_contract::kLaunchEnvironmentContract;
inline constexpr std::string_view
    kRos2ControlExecutionRuntimeInstalledTargetId =
        ros2_control_execution_runtime_contract::kInstalledTargetId;
inline constexpr std::size_t kMaxRos2ControlExecutionRuntimeManifestBytes =
    64U * 1024U;
inline constexpr std::size_t
    kMaxRos2ControlExecutionLaunchDescriptorBytes = 4U * 1024U;

/// Reject collisions on the exclusive topics of the known Isaac profiles.
/// Both authored compilation and retained activation use this one check. Only
/// an arm (a component with a motion target limit) owns those topics.
void require_no_exclusive_ros_controller_topic_collision(
    const CanonicalValue::Array &controller_profiles,
    const CanonicalValue::Array &motion_target_limits);

[[nodiscard]] std::string ros2_control_execution_runtime_profile_document();
[[nodiscard]] std::string ros2_control_execution_runtime_profile_revision();
[[nodiscard]] bool ros_gripper_output_mode_is_valid(
    std::string_view actuation_kind, std::string_view output_mode) noexcept;

/// Strict active ROS thirteen-field row. The archival neutral twelve-field
/// decoder receives only its exact private data projection.
[[nodiscard]] std::vector<
    ros2_control_execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_ros2_control_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &rows);

/// The strict `component_streams` rows of one ROS unit configuration,
/// sorted by (component, stream). Exactly `measured_joints` for every
/// controller-profile component plus `gripper_feedback` for every gripper-row
/// component; `fault` + `auto` is refused. Shared by the pre-Draft row hook
/// and the retained decoder.
[[nodiscard]] std::vector<
    ros2_control_execution_runtime_contract::Configuration::ComponentStream>
decode_ros2_control_execution_component_streams(
    const core::JsonObjectDTO &configuration);

/// A controller profile row's optional `stop_lead_in_anchor` (`commanded` or
/// `measured`); absent stays absent (the lane default, commanded). Any other
/// value is refused.
[[nodiscard]] std::optional<core::StopLeadInAnchor>
decode_ros2_control_stop_lead_in_anchor(const std::optional<std::string> &value);

/// A controller profile row's optional `controller_tcp_offset_sync`
/// (absent: on); a value that is not a boolean is refused.
[[nodiscard]] bool decode_ros2_control_tcp_offset_sync(const core::JsonValueDTO *value);

/// One source classifier for the already-qualified selected graph.
/// Absence preserves the historical physical/default Stop contract. A returned
/// enum comes only from the exact strict ROS target row, never a policy token.
[[nodiscard]] std::optional<core::GripperActuationKind>
classify_ros_gripper_actuation_kind(
    const core::CellGraphDTO &graph, const core::ControlBindingDTO &binding,
    std::span<const CompositionControlBindingCoordinateMap> coordinate_maps);

/// Preserve required nonempty rows for actual hosted membership at graph and
/// retained-product qualification. Returns true for a host with no components.
[[nodiscard]] bool require_ros2_control_execution_runtime_required_rows(
    const core::CellGraphDTO &graph, const core::RuntimeUnitDTO &unit);

[[nodiscard]] bool
ros2_control_execution_runtime_configuration_is_qualified(
    const core::JsonObjectDTO &configuration) noexcept;
[[nodiscard]] std::string
ros2_control_execution_runtime_implementation_revision();
[[nodiscard]] std::string
ros2_control_execution_runtime_process_adapter_revision();
[[nodiscard]] std::string
ros2_control_execution_runtime_retrieval_contract_revision();
[[nodiscard]] std::string
ros2_control_execution_runtime_status_endpoint_revision();
[[nodiscard]] std::string
ros2_control_execution_runtime_projection_endpoint_revision();
[[nodiscard]] std::string
ros2_control_execution_runtime_logical_scene_endpoint_revision();

[[nodiscard]] std::string
ros2_control_execution_runtime_launch_descriptor();
[[nodiscard]] std::string ros2_control_execution_runtime_product_id(
    const core::RuntimeUnitId &runtime_unit_id);
[[nodiscard]] CanonicalValue
ros2_control_execution_runtime_manifest_projection(
    const core::RuntimeUnitDTO &runtime_unit);

[[nodiscard]] std::string emit_ros2_control_execution_runtime_manifest(
    const CompilerQualifiedCellIr &compiler_ir,
    const core::RuntimeUnitId &runtime_unit_id);
[[nodiscard]] CanonicalValue
parse_and_verify_ros2_control_execution_runtime_manifest(
    std::string_view canonical_bytes);
void require_ros2_control_execution_runtime_manifest_matches(
    std::string_view canonical_bytes,
    const CompilerQualifiedCellIr &compiler_ir,
    const core::RuntimeUnitId &runtime_unit_id);
void require_ros2_control_execution_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes,
    std::string_view launch_descriptor_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id);

} // namespace detail
} // namespace manyforge::deployment
