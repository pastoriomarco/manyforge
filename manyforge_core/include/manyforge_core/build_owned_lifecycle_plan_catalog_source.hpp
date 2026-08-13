#pragma once

#include <manyforge_core/ports/icclc_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <vector>

namespace manyforge::core {

struct BuildOwnedLifecyclePlanCatalogEntryDTO final {
  ProviderManifestDTO provider_manifest;
  BrandLifecyclePlanCatalogDTO catalog;

  friend bool operator==(const BuildOwnedLifecyclePlanCatalogEntryDTO&,
                         const BuildOwnedLifecyclePlanCatalogEntryDTO&) = default;
};

/// Immutable manifest-pair registry for build-owned lifecycle catalogs.
/// Construction verifies every catalog and rejects ambiguous manifest pairs;
/// reads perform no I/O and are safe to call concurrently.
class BuildOwnedLifecyclePlanCatalogSource final : public ICCLCLifecyclePlanCatalogSource {
 public:
  [[nodiscard]] static Result<BuildOwnedLifecyclePlanCatalogSource> Make(
      std::vector<BuildOwnedLifecyclePlanCatalogEntryDTO> entries);

  [[nodiscard]] Result<BrandLifecyclePlanCatalogDTO> LoadExactLifecyclePlanCatalog(
      const ProviderManifestId& provider_manifest_id,
      const ProviderManifestRevision& provider_manifest_revision,
      const ContentDigest& catalog_digest) const override;

 private:
  explicit BuildOwnedLifecyclePlanCatalogSource(std::vector<BrandLifecyclePlanCatalogDTO> catalogs);

  std::vector<BrandLifecyclePlanCatalogDTO> catalogs_;
};

}  // namespace manyforge::core
