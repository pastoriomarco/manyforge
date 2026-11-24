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

/// A passive tool's payload (OD-336) from its URDF: the `<inertial>` of
/// every link, summed, the centre of mass in `root_link` (its mount) through
/// the joint origins; `{center_of_mass_m, mass_kg}`, none without mass.
[[nodiscard]] std::optional<CanonicalValue> passive_tool_urdf_payload(
    const UrdfDocumentIr &urdf, std::string_view root_link);

/// OD-350: `semantics` with every `<collision>` link of `urdf` that no
/// collision-body binding covers bound, sorted, to `default_collision`
/// (merged with an authored one); none when every collision link is covered.
[[nodiscard]] std::optional<CanonicalValue> default_collision_semantics(
    const CanonicalValue &semantics, const UrdfDocumentIr &urdf);

/// OD-352 A1: `semantics` without the bindings of the family's optional
/// collision bodies whose links `urdf` lacks (all of them when `urdf` is
/// null). An optional body with any link in the model stays bound, so a link
/// without geometry is refused as for every body (OD-350).
[[nodiscard]] CanonicalValue without_absent_optional_bodies(
    const CanonicalValue &semantics,
    const std::vector<std::string> &optional_collision_body_role_ids,
    const UrdfDocumentIr *urdf);

/// The family's optional collision bodies (`roles.optional_collision_bodies`
/// of its component descriptor); empty without a descriptor.
[[nodiscard]] std::vector<std::string>
optional_collision_body_role_ids(const CanonicalValue &model_descriptor);

/// Qualifies the model asset, adding the `default_collision` binding when
/// collision links are uncovered and leaving an optional body the model has
/// no links for unbound; a fully covered model is qualified as given.
[[nodiscard]] ActivationPreparedModel qualify_activation_prepared_model(
    std::string_view model_asset_id, const CanonicalValue &semantics,
    std::string_view expected_package_uri_prefix,
    std::string_view canonical_preparation_receipt,
    std::string_view original_urdf_bytes,
    std::optional<std::string_view> original_srdf_bytes,
    std::vector<PreparedModelSourceArtifact> artifacts,
    const std::vector<std::string> &optional_collision_body_role_ids = {});

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
