// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <manyforge_xarm_control/lite6_brand_lifecycle_catalog.hpp>

#include <manyforge_xarm_control/generated/lite6_brand_lifecycle_product_identity.hpp>

#include <string>
#include <utility>

namespace manyforge::xarm_control {

core::Result<core::ContentDigest>
compute_lite6_brand_lifecycle_catalog_semantic_digest() {
  return lite6_product::compute_lite6_lifecycle_catalog_semantic_digest();
}

core::Result<core::ProviderManifestDTO>
make_build_owned_lite6_lifecycle_provider_manifest() {
  const auto semantic = compute_lite6_brand_lifecycle_catalog_semantic_digest();
  if (!semantic.ok()) {
    return core::Result<core::ProviderManifestDTO>::Err(semantic.error());
  }
  if (semantic.value().value() !=
      generated::kLite6LifecycleCatalogSemanticDigest) {
    return core::Result<core::ProviderManifestDTO>::Err(
        {"lite6_lifecycle_product_identity_stale",
         "the generated Lite6 lifecycle semantic identity is stale",
         {{"reason", "semantic_digest_mismatch"}}});
  }
  auto capabilities =
      lite6_product::make_lite6_lifecycle_capability_offers();
  if (!capabilities.ok()) {
    return core::Result<core::ProviderManifestDTO>::Err(capabilities.error());
  }
  return core::Result<core::ProviderManifestDTO>::Ok(
      {core::ProviderManifestId(
           std::string(kLite6LifecycleProviderManifestId)),
       core::ProviderManifestRevision(
           std::string(generated::kLite6LifecycleProviderManifestRevision)),
       std::move(capabilities.value()),
       {},
       core::ProviderLifecyclePlanCatalogDeclarationDTO{
           core::SchemaId(
               std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
           semantic.value()}});
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_lite6_brand_lifecycle_plan_catalog(
    const core::ProviderManifestDTO &provider_manifest) {
  return lite6_product::make_lite6_lifecycle_catalog(provider_manifest);
}

Lite6BrandLifecyclePlanCatalogSource::Lite6BrandLifecyclePlanCatalogSource(
    core::BrandLifecyclePlanCatalogDTO catalog,
    core::BuildOwnedLifecyclePlanCatalogSource source)
    : catalog_(std::move(catalog)), source_(std::move(source)) {}

core::Result<Lite6BrandLifecyclePlanCatalogSource>
Lite6BrandLifecyclePlanCatalogSource::make_build_owned() {
  auto manifest = make_build_owned_lite6_lifecycle_provider_manifest();
  if (!manifest.ok()) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        manifest.error());
  }
  auto source = make(std::move(manifest.value()));
  if (!source.ok()) {
    return source;
  }
  if (source.value().catalog().catalog_digest.value() !=
      generated::kLite6LifecycleCatalogDigest) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        {"lite6_lifecycle_product_identity_stale",
         "the generated Lite6 lifecycle catalog identity is stale",
         {{"reason", "catalog_digest_mismatch"}}});
  }
  return source;
}

core::Result<Lite6BrandLifecyclePlanCatalogSource>
Lite6BrandLifecyclePlanCatalogSource::make(
    core::ProviderManifestDTO provider_manifest) {
  auto catalog = make_lite6_brand_lifecycle_plan_catalog(provider_manifest);
  if (!catalog.ok()) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        catalog.error());
  }
  auto source = core::BuildOwnedLifecyclePlanCatalogSource::Make(
      {{std::move(provider_manifest), catalog.value()}});
  if (!source.ok()) {
    return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Err(
        source.error());
  }
  return core::Result<Lite6BrandLifecyclePlanCatalogSource>::Ok(
      Lite6BrandLifecyclePlanCatalogSource(std::move(catalog.value()),
                                           std::move(source.value())));
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
Lite6BrandLifecyclePlanCatalogSource::LoadExactLifecyclePlanCatalog(
    const core::ProviderManifestId &provider_manifest_id,
    const core::ProviderManifestRevision &provider_manifest_revision,
    const core::ContentDigest &catalog_digest) const {
  return source_.LoadExactLifecyclePlanCatalog(
      provider_manifest_id, provider_manifest_revision, catalog_digest);
}

const core::BrandLifecyclePlanCatalogDTO &
Lite6BrandLifecyclePlanCatalogSource::catalog() const noexcept {
  return catalog_;
}

} // namespace manyforge::xarm_control
