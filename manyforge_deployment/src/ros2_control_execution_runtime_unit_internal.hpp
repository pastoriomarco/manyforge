#pragma once

#include <cstddef>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
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
        "9d37332a6f3c943e2f1811b5009c329d24248665f4ec14924a5ca437510c6905";
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

[[nodiscard]] std::string ros2_control_execution_runtime_profile_document();
[[nodiscard]] std::string ros2_control_execution_runtime_profile_revision();
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
