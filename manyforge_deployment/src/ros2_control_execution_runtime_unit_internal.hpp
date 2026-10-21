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
        "775f7221ffc5433d18a7b794d3b26ab9993fa805ad5b83b6d1a3a0cb58bd1368";
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

/// Q85: reject collisions on the exclusive topics of the known Isaac profiles.
/// Both authored compilation and retained activation use this one check.
void require_no_exclusive_ros_controller_topic_collision(
    const CanonicalValue::Array &controller_profiles);

[[nodiscard]] std::string ros2_control_execution_runtime_profile_document();
[[nodiscard]] std::string ros2_control_execution_runtime_profile_revision();
[[nodiscard]] bool ros_gripper_output_mode_is_valid(
    std::string_view actuation_kind, std::string_view output_mode) noexcept;

/// Present ROS-only timeout validation shared by early authored activation
/// selection and strict Draft/retained decoding. Absence keeps its default.
[[nodiscard]] bool ros_gripper_delivery_result_timeout_is_valid(
    std::string_view actuation_kind, double timeout_s) noexcept;

/// Strict active ROS thirteen-field row, with an optional pneumatic-only
/// delivery_result_timeout_s field (fourteen fields when present). The archival
/// neutral twelve-field decoder receives only its exact private data projection.
[[nodiscard]] std::vector<
    ros2_control_execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_ros2_control_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &rows);

/// DX817: one source classifier for the already-qualified selected graph.
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
