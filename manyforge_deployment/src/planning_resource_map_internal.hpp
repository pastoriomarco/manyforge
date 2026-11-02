#pragma once

#include <cstdint>
#include <manyforge_core/types/model_asset.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment::detail {

inline constexpr std::string_view kPlanningResourceMapApiVersion =
    "manyforge.planning_resource_map.v1";
inline constexpr std::string_view kPlanningResourceMapArtifactRole =
    "planning_resource_map";
inline constexpr std::string_view kPlanningResourceMapMediaType =
    "application/vnd.manyforge.planning-resource-map+json";

/// One compiler-owned projection from an immutable ModelAsset source resource
/// into the revision-scoped namespace used by the flattened planning product.
struct PlanningResourceMapRow final {
  core::ModelAssetId model_asset_id;
  core::ModelRevision model_revision;
  std::string source_logical_uri;
  std::string compiled_logical_uri;
  std::string media_type;
  std::uint64_t byte_size;
  core::ContentDigest content_digest;
  core::ContentAddressedStoreKey content_addressed_store_key;

  friend bool operator==(const PlanningResourceMapRow&,
                         const PlanningResourceMapRow&) = default;
};

/// Deterministically derives the bounded revision-scoped planning URI for one
/// already-qualified source URI. This never changes the source ModelAsset URI.
[[nodiscard]] std::string compiled_planning_resource_uri(
    const core::ModelAssetId& model_asset_id,
    const core::ModelRevision& model_revision,
    std::string_view source_logical_uri);

/// Emits the exact canonical retained mapping artifact.
[[nodiscard]] std::string emit_planning_resource_map(
    std::vector<PlanningResourceMapRow> rows);

/// Strictly decodes and independently verifies the exact mapping artifact,
/// including canonical bytes, row ordering, generated names, and CAS identity.
[[nodiscard]] std::vector<PlanningResourceMapRow>
parse_and_verify_planning_resource_map(std::string_view canonical_bytes);

}  // namespace manyforge::deployment::detail
