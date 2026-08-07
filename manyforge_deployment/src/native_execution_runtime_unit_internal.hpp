#pragma once

#include <cstddef>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <string>
#include <string_view>

namespace manyforge::deployment {

class CompilerQualifiedCellIr;

namespace detail {

inline constexpr std::string_view kNativeExecutionRuntimeManifestApiVersion =
    "manyforge.native_execution_runtime_manifest.v1";
inline constexpr std::string_view kNativeExecutionRuntimeProductContractId =
    "manyforge.native_execution_runtime_unit";
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
    "manyforge.native_execution_runtime";
inline constexpr std::string_view kNativeExecutionRuntimeRoleId =
    "native_execution";
inline constexpr std::string_view kNativeExecutionRuntimeStatusEndpointRoleId =
    "native_execution_status";
inline constexpr std::string_view
    kNativeExecutionRuntimeProjectionEndpointRoleId =
        "planner_scene_projection";
inline constexpr std::string_view kNativeExecutionRuntimeImplementationId =
    "manyforge.native_execution_host";
inline constexpr std::string_view
    kNativeExecutionRuntimeProcessAdapterContractId =
        "manyforge.installed_native_target";
inline constexpr std::string_view kNativeExecutionRuntimeRetrievalContractId =
    "manyforge.retained_raw_artifact";
inline constexpr std::string_view
    kNativeExecutionRuntimeStatusEndpointContractId =
        "manyforge.native_execution.status_direct";
inline constexpr std::string_view
    kNativeExecutionRuntimeProjectionEndpointContractId =
        "manyforge.planner_scene_projection.rpc";
inline constexpr std::string_view kNativeExecutionRuntimeOperatingSystem =
    "linux";
inline constexpr std::string_view
    kNativeExecutionRuntimeLaunchEnvironmentContract =
        "manyforge.native_execution_host.abi.v1";
inline constexpr std::string_view kNativeExecutionRuntimeInstalledTargetId =
    "manyforge_native_execution_host";
inline constexpr std::size_t kMaxNativeExecutionRuntimeManifestBytes =
    64U * 1024U;
inline constexpr std::size_t kMaxNativeExecutionLaunchDescriptorBytes =
    4U * 1024U;

[[nodiscard]] std::string native_execution_runtime_profile_document();
[[nodiscard]] std::string native_execution_runtime_profile_revision();
[[nodiscard]] std::string native_execution_runtime_implementation_revision();
[[nodiscard]] std::string native_execution_runtime_process_adapter_revision();
[[nodiscard]] std::string
native_execution_runtime_retrieval_contract_revision();
[[nodiscard]] std::string native_execution_runtime_status_endpoint_revision();
[[nodiscard]] std::string
native_execution_runtime_projection_endpoint_revision();

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
