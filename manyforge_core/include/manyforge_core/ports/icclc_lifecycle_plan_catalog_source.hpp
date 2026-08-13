#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Exact manifest-and-digest-keyed declarative lifecycle-plan source.
class ICCLCLifecyclePlanCatalogSource {
 public:
  virtual ~ICCLCLifecyclePlanCatalogSource() = default;

  [[nodiscard]] virtual Result<BrandLifecyclePlanCatalogDTO> LoadExactLifecyclePlanCatalog(
      const ProviderManifestId& provider_manifest_id,
      const ProviderManifestRevision& provider_manifest_revision,
      const ContentDigest& catalog_digest) const = 0;
};

}  // namespace manyforge::core
