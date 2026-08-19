#pragma once

#include <cstddef>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <string>
#include <string_view>

namespace manyforge::deployment {

class CompilerQualifiedCellIr;

namespace detail {

inline constexpr std::string_view kReferenceRuntimeManifestApiVersion =
    "manyforge.reference_runtime_manifest.v1";
inline constexpr std::string_view kReferenceRuntimeProductContractId =
    "manyforge.reference_runtime_unit";
inline constexpr std::string_view kReferenceRuntimeManifestArtifactRole = "runtime_manifest";
inline constexpr std::string_view kReferenceRuntimeManifestMediaType =
    "application/vnd.manyforge.runtime-manifest+json";
inline constexpr std::string_view kReferenceRuntimeLaunchableArtifactRole = "launchable_script";
inline constexpr std::string_view kReferenceRuntimeLaunchableMediaType = "application/x-sh";
inline constexpr std::string_view kReferenceRuntimeProfileId =
    "manyforge.reference_manager_runtime";
inline constexpr std::string_view kReferenceRuntimeUnitId = "reference_runtime";
inline constexpr std::string_view kReferenceRuntimeRoleId = "manager_fixture";
inline constexpr std::string_view kReferenceRuntimeStatusEndpointRoleId = "runtime_status";
inline constexpr std::string_view kReferenceRuntimeImplementationId =
    "manyforge.reference_process_lifecycle";
inline constexpr std::string_view kReferenceRuntimeProcessAdapterContractId =
    "manyforge.posix_sh.retained_script";
inline constexpr std::string_view kReferenceRuntimeRetrievalContractId =
    "manyforge.retained_raw_artifact";
inline constexpr std::string_view kReferenceRuntimeStatusEndpointContractId =
    "manyforge.reference_runtime.status_stdout";
// These are immutable public API identities, not endpoint-contract revisions.
// Their semantic owner is the reference runtime process.
inline constexpr std::string_view kReferenceRuntimeRoleApiContractId =
    "manyforge.api.reference.execution";
inline constexpr std::string_view kReferenceRuntimeStatusApiContractId =
    "manyforge.api.reference.status";
inline constexpr std::string_view kReferenceRuntimeOperatingSystem = "linux";
inline constexpr std::string_view kReferenceRuntimeShellContract = "posix.1-2017";
inline constexpr std::size_t kMaxReferenceRuntimeManifestBytes = 64U * 1024U;
inline constexpr std::size_t kMaxReferenceRuntimeLaunchableBytes = 4U * 1024U;

/// Exact build-owned source profile used by the qualification runtime row.
[[nodiscard]] std::string reference_runtime_profile_document();
[[nodiscard]] std::string reference_runtime_profile_revision();

/// Fixed qualification process artifact consumed by the selected adapter.
///
/// This is a real bounded POSIX-shell process contract used to qualify I4
/// lifecycle mechanics. It is not a planner, robot owner, or supported lane.
[[nodiscard]] const std::string& reference_runtime_launchable_script();

[[nodiscard]] std::string reference_runtime_implementation_revision();
[[nodiscard]] std::string reference_runtime_process_adapter_revision();
[[nodiscard]] std::string reference_runtime_retrieval_contract_revision();
[[nodiscard]] std::string reference_runtime_status_endpoint_revision();

/// Stable bundle-local product identity for one exact runtime unit.
[[nodiscard]] std::string reference_runtime_product_id(const core::RuntimeUnitId& runtime_unit_id);

/// Emit the exact build-qualified runtime manifest for the reference row.
[[nodiscard]] std::string emit_reference_runtime_manifest(
    const CompilerQualifiedCellIr& compiler_ir, const core::RuntimeUnitId& runtime_unit_id);

/// Parse and independently verify one canonical closed runtime manifest.
[[nodiscard]] CanonicalValue parse_and_verify_reference_runtime_manifest(
    std::string_view canonical_bytes);

/// Require exact equality with the projection derived from final I2 IR.
void require_reference_runtime_manifest_matches(std::string_view canonical_bytes,
                                                const CompilerQualifiedCellIr& compiler_ir,
                                                const core::RuntimeUnitId& runtime_unit_id);

/// Require the manifest and launchable bytes to agree with one exact retained
/// bundle subject, profile source, artifact table, and provenance output.
void require_reference_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes, std::string_view launchable_bytes,
    const core::RetainedBundleDTO& retained_bundle, const core::RuntimeUnitId& runtime_unit_id);

}  // namespace detail
}  // namespace manyforge::deployment
