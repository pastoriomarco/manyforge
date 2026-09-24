#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kBrandLifecyclePlanCatalogApiVersion =
    "manyforge.brand_lifecycle_plan_catalog.v1";
inline constexpr std::string_view kBrandLifecyclePlanCatalogIdentityDomain =
    "brand-lifecycle-plan-catalog";
inline constexpr std::string_view kBrandLifecyclePlanCatalogSemanticIdentityDomain =
    "brand-lifecycle-plan-catalog-semantic";

/// Compute the acyclic plan-semantics anchor carried by a provider manifest.
/// Its canonical projection contains only API version, provider-manifest ID,
/// and plan rows.
[[nodiscard]] Result<ContentDigest> ComputeBrandLifecyclePlanCatalogSemanticDigest(
    const ProviderManifestId& provider_manifest_id,
    const std::vector<LifecycleIntentPlanDTO>& plans);

/// Verify that one already-qualified provider manifest anchors the exact
/// final catalog selected by its manifest pair.
[[nodiscard]] Result<void> VerifyBrandLifecyclePlanCatalogManifestAnchor(
    const ProviderManifestDTO& provider_manifest, const BrandLifecyclePlanCatalogDTO& catalog);

/// Construct one immutable catalog and mint its canonical content digest.
[[nodiscard]] Result<BrandLifecyclePlanCatalogDTO> MakeBrandLifecyclePlanCatalog(
    ProviderManifestId provider_manifest_id, ProviderManifestRevision provider_manifest_revision,
    std::vector<LifecycleIntentPlanDTO> plans);

/// Verify the closed shape and exact canonical content digest.
[[nodiscard]] Result<void> VerifyBrandLifecyclePlanCatalog(
    const BrandLifecyclePlanCatalogDTO& catalog);

/// The generic in-process native control lifecycle, shared by every native
/// provider: Prepare verifies the constructed engine, Enable opens and Disable
/// closes its Kernel command gate. A provider selects it by anchoring these
/// plans (under its own manifest ID) in its manifest. No Recover plan exists.
[[nodiscard]] std::vector<LifecycleIntentPlanDTO> MakeNativeControlLifecyclePlans();

/// Emit the complete RFC-8785 canonical catalog artifact, including its
/// verified own digest.
[[nodiscard]] Result<std::string> EncodeBrandLifecyclePlanCatalog(
    const BrandLifecyclePlanCatalogDTO& catalog);

}  // namespace manyforge::core
