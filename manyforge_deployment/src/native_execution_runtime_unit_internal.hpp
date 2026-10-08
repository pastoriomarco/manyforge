#pragma once

#include <cstddef>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

class CompilerQualifiedCellIr;

namespace detail {

[[nodiscard]] native_execution_runtime_contract::OrdinaryMotionJerkSettings
decode_ordinary_motion_jerk_settings(const core::JsonObjectDTO& source);

[[nodiscard]] std::vector<
    native_execution_runtime_contract::Configuration::MotionTargetLimit>
decode_execution_motion_target_limits(
    const core::JsonValueDTO::Array &motion_target_limits,
    bool include_ordinary_jerk);

// The artifact-role/media contract is unchanged. This payload version pins the
// infrastructure-only projection; candidate controller settings stay in the
// retained graph and are independently qualified before installation.
inline constexpr std::string_view kNativeExecutionRuntimeManifestApiVersion =
    "manyforge.native_execution_runtime_manifest.v2";
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
    "6b540dac39434e05a2cb5da209206e2105b824b9f56704a74f0d88f327944689";
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
/// The typed configuration of native runtime unit `runtime_unit_id`, checked
/// against the control bindings that unit hosts in `graph`: motion-only
/// fields exist exactly when it hosts motion execution, and every motion
/// limit or gripper configuration names a target it hosts. Callers establish
/// bundle identity and closed-schema qualification first.
[[nodiscard]] native_execution_runtime_contract::Configuration
decode_native_execution_runtime_configuration(
    const core::JsonObjectDTO &configuration, const core::CellGraphDTO &graph,
    const core::RuntimeUnitId &runtime_unit_id);
/// Shared controller selection for runtime construction, retention and fence
/// derivation. Inputs have already passed native configuration decoding and
/// graph verification. The native adapter supports at most one motion target
/// per component; additional non-motion capabilities remain independent.
[[nodiscard]] native_execution_runtime_contract::Configuration
select_native_component_configuration(
    native_execution_runtime_contract::Configuration configuration,
    const core::CellGraphDTO &graph, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);
void require_native_execution_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes, std::string_view launch_descriptor_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id);

} // namespace detail
} // namespace manyforge::deployment
