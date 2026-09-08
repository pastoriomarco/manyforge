#pragma once

#include <cstddef>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <string>
#include <string_view>

namespace manyforge::deployment {

class CompilerQualifiedCellIr;

namespace detail {

inline constexpr std::string_view kNativeExecutionRuntimeManifestApiVersion =
    "manyforge.native_execution_runtime_manifest.v1";
inline constexpr std::string_view kNativeExecutionRuntimeProductContractId =
    native_execution_runtime_contract::kProductContractId;
inline constexpr std::string_view kNativeExecutionRuntimeManifestArtifactRole =
    "runtime_manifest";
inline constexpr std::string_view kNativeExecutionRuntimeManifestMediaType =
    "application/vnd.manyforge.runtime-manifest+json";
inline constexpr std::string_view
    kNativeExecutionRuntimeLaunchDescriptorArtifactRole = "launch_descriptor";
inline constexpr std::string_view
    kNativeExecutionRuntimeLaunchDescriptorMediaType =
        "application/vnd.manyforge.installed-target-launch+json";
inline constexpr std::string_view kNativeExecutionRuntimeProfileId =
    native_execution_runtime_contract::kRuntimeProfileId;
inline constexpr std::string_view kNativeExecutionRuntimeConfigurationSchemaId =
    "manyforge.value_schema.runtime.native_execution.v1";
inline constexpr std::string_view kNativeExecutionRuntimeConfigurationSchemaRevision =
    "sha256:"
    "4405892649681799b9a813fafee4b5d23579d080b5457bf60f51ce2e296fcff8";
inline constexpr std::string_view kNativeExecutionRuntimeRoleId =
    native_execution_runtime_contract::kRuntimeRoleId;
inline constexpr std::string_view kNativeExecutionRuntimeStatusEndpointRoleId =
    native_execution_runtime_contract::kStatusEndpointRoleId;
inline constexpr std::string_view
    kNativeExecutionRuntimeProjectionEndpointRoleId =
        native_execution_runtime_contract::kProjectionEndpointRoleId;
inline constexpr std::string_view
    kNativeExecutionRuntimeLogicalSceneEndpointRoleId =
        native_execution_runtime_contract::kLogicalSceneEndpointRoleId;
inline constexpr std::string_view kNativeExecutionRuntimeImplementationId =
    native_execution_runtime_contract::kImplementationId;
inline constexpr std::string_view
    kNativeExecutionRuntimeProcessAdapterContractId =
        native_execution_runtime_contract::kProcessAdapterContractId;
inline constexpr std::string_view kNativeExecutionRuntimeRetrievalContractId =
    native_execution_runtime_contract::kRetrievalContractId;
inline constexpr std::string_view
    kNativeExecutionRuntimeStatusEndpointContractId =
        native_execution_runtime_contract::kStatusEndpointContractId;
inline constexpr std::string_view
    kNativeExecutionRuntimeProjectionEndpointContractId =
        native_execution_runtime_contract::kProjectionEndpointContractId;
inline constexpr std::string_view
    kNativeExecutionRuntimeLogicalSceneEndpointContractId =
        native_execution_runtime_contract::kLogicalSceneEndpointContractId;
inline constexpr std::string_view kNativeExecutionRuntimeOperatingSystem =
    native_execution_runtime_contract::kOperatingSystem;
inline constexpr std::string_view
    kNativeExecutionRuntimeLaunchEnvironmentContract =
        native_execution_runtime_contract::kLaunchEnvironmentContract;
inline constexpr std::string_view kNativeExecutionRuntimeInstalledTargetId =
    native_execution_runtime_contract::kInstalledTargetId;
inline constexpr std::size_t kMaxNativeExecutionRuntimeManifestBytes =
    64U * 1024U;
inline constexpr std::size_t kMaxNativeExecutionLaunchDescriptorBytes =
    4U * 1024U;

[[nodiscard]] std::string native_execution_runtime_profile_document();
[[nodiscard]] std::string native_execution_runtime_profile_revision();
[[nodiscard]] bool native_execution_runtime_configuration_is_qualified(
    const core::JsonObjectDTO &configuration) noexcept;
[[nodiscard]] std::string native_execution_runtime_implementation_revision();
[[nodiscard]] std::string native_execution_runtime_process_adapter_revision();
[[nodiscard]] std::string
native_execution_runtime_retrieval_contract_revision();
[[nodiscard]] std::string native_execution_runtime_status_endpoint_revision();
[[nodiscard]] std::string
native_execution_runtime_projection_endpoint_revision();
[[nodiscard]] std::string
native_execution_runtime_logical_scene_endpoint_revision();

[[nodiscard]] std::string native_execution_runtime_launch_descriptor();
[[nodiscard]] std::string
native_execution_runtime_product_id(const core::RuntimeUnitId &runtime_unit_id);
[[nodiscard]] CanonicalValue native_execution_runtime_manifest_projection(
    const core::RuntimeUnitDTO &runtime_unit);

[[nodiscard]] std::string emit_native_execution_runtime_manifest(
    const CompilerQualifiedCellIr &compiler_ir,
    const core::RuntimeUnitId &runtime_unit_id);
[[nodiscard]] CanonicalValue parse_and_verify_native_execution_runtime_manifest(
    std::string_view canonical_bytes);
void require_native_execution_runtime_manifest_matches(
    std::string_view canonical_bytes,
    const CompilerQualifiedCellIr &compiler_ir,
    const core::RuntimeUnitId &runtime_unit_id);
void require_native_execution_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes, std::string_view launch_descriptor_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id);

} // namespace detail
} // namespace manyforge::deployment
