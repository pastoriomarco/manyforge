#include <algorithm>
#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] auto manifest_key(const BrandLifecyclePlanCatalogDTO& catalog) {
  return std::tie(catalog.provider_manifest_id, catalog.provider_manifest_revision);
}

[[nodiscard]] ErrorDTO source_error(std::string code, std::string message, std::string reason) {
  return ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

}  // namespace

BuildOwnedLifecyclePlanCatalogSource::BuildOwnedLifecyclePlanCatalogSource(
    std::vector<BrandLifecyclePlanCatalogDTO> catalogs)
    : catalogs_(std::move(catalogs)) {}

Result<BuildOwnedLifecyclePlanCatalogSource> BuildOwnedLifecyclePlanCatalogSource::Make(
    std::vector<BuildOwnedLifecyclePlanCatalogEntryDTO> entries) {
  if (entries.size() > kMaxLifecycleProviderOccurrences) {
    return Result<BuildOwnedLifecyclePlanCatalogSource>::Err(
        source_error("brand_lifecycle_plan_catalog_registry_invalid",
                     "the build-owned lifecycle catalog registry is over its bound",
                     "registry_cardinality_invalid"));
  }
  std::vector<BrandLifecyclePlanCatalogDTO> catalogs;
  catalogs.reserve(entries.size());
  for (auto& entry : entries) {
    const auto verified =
        VerifyBrandLifecyclePlanCatalogManifestAnchor(entry.provider_manifest, entry.catalog);
    if (!verified.ok()) {
      return Result<BuildOwnedLifecyclePlanCatalogSource>::Err(verified.error());
    }
    catalogs.push_back(std::move(entry.catalog));
  }
  std::sort(catalogs.begin(), catalogs.end(), [](const auto& left, const auto& right) {
    return manifest_key(left) < manifest_key(right);
  });
  if (std::adjacent_find(catalogs.begin(), catalogs.end(), [](const auto& left, const auto& right) {
        return manifest_key(left) == manifest_key(right);
      }) != catalogs.end()) {
    return Result<BuildOwnedLifecyclePlanCatalogSource>::Err(
        source_error("brand_lifecycle_plan_catalog_registry_ambiguous",
                     "one provider manifest pair selects more than one lifecycle catalog",
                     "duplicate_provider_manifest_pair"));
  }
  return Result<BuildOwnedLifecyclePlanCatalogSource>::Ok(
      BuildOwnedLifecyclePlanCatalogSource(std::move(catalogs)));
}

Result<BrandLifecyclePlanCatalogDTO>
BuildOwnedLifecyclePlanCatalogSource::LoadExactLifecyclePlanCatalog(
    const ProviderManifestId& provider_manifest_id,
    const ProviderManifestRevision& provider_manifest_revision,
    const ContentDigest& catalog_digest) const {
  const auto key = std::tie(provider_manifest_id, provider_manifest_revision);
  const auto found =
      std::lower_bound(catalogs_.begin(), catalogs_.end(), key,
                       [](const BrandLifecyclePlanCatalogDTO& candidate, const auto& requested) {
                         return manifest_key(candidate) < requested;
                       });
  if (found == catalogs_.end() || manifest_key(*found) != key) {
    return Result<BrandLifecyclePlanCatalogDTO>::Err(
        source_error("brand_lifecycle_plan_catalog_not_found",
                     "the exact provider manifest pair has no build-owned lifecycle catalog",
                     "provider_manifest_pair_not_found"));
  }
  if (found->catalog_digest != catalog_digest) {
    return Result<BrandLifecyclePlanCatalogDTO>::Err(
        source_error("brand_lifecycle_plan_catalog_digest_mismatch",
                     "the requested lifecycle catalog digest is not the build-owned digest",
                     "catalog_digest_mismatch"));
  }
  return Result<BrandLifecyclePlanCatalogDTO>::Ok(*found);
}

}  // namespace manyforge::core
