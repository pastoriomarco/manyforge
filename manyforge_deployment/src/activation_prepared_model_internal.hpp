#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/structural_model_preparation.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_prepared_model_closure_internal.hpp"

namespace manyforge::deployment::detail {

struct PreparedModelSourceArtifact final {
  std::string logical_uri;
  std::string media_type;
  core::ContentDigest content_digest;
  std::string bytes;
};

/// One source-built model product used by both activation and offline checks.
///
/// All bytes are caller-owned captured/prepared values. This function performs
/// no filesystem, package-index, catalog-selection, or content-store I/O.
struct ActivationPreparedModel final {
  std::string catalog_bytes;
  core::ModelAssetCatalogRecordRefDTO catalog_reference;
  std::vector<PreparedModelSourceArtifact> source_artifacts;
  SourceVerifiedModelAsset verified_asset;
  BuildQualifiedStructuralModelPreparation preparation;
};

[[nodiscard]] ActivationPreparedModel qualify_activation_prepared_model(
    std::string_view model_asset_id, const CanonicalValue &semantics,
    std::string_view expected_package_uri_prefix,
    std::string_view canonical_preparation_receipt,
    std::string_view original_urdf_bytes,
    std::optional<std::string_view> original_srdf_bytes,
    std::vector<PreparedModelSourceArtifact> artifacts);

/// Join the three independently validated preparation inputs exactly once.
///
/// Profiles cover the exact authored component set. Only physical components
/// contribute prepared model rows, in canonical physical-component order; a
/// model-free provider cannot contribute a model. No family/catalog fallback.
/// Resource bytes are reused from the captured closure and renamed only by the
/// preparation receipt that the model qualifier verifies independently.
[[nodiscard]] std::vector<ActivationPreparedModel>
qualify_activation_prepared_models(
    const ActivationDeploymentIntent &intent,
    const std::vector<ActivationComponentProfile> &profiles,
    std::vector<PreparedComponentModelSource> prepared_sources);

} // namespace manyforge::deployment::detail
