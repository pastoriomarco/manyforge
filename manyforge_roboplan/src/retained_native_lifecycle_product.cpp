#include <manyforge_roboplan/retained_native_lifecycle_product.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <manyforge_lite6_product/lifecycle_profile.hpp>
#include <manyforge_roboplan/retained_native_brand_lifecycle_port.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::ErrorDTO product_error(std::string code,
                                           std::string message,
                                           std::string reason) {
  return {std::move(code), std::move(message),
          {{"reason", std::move(reason)}}};
}

[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_catalog(const core::ProviderManifestDTO &manifest) {
  if (manifest.provider_manifest_id ==
      core::ProviderManifestId(
          std::string(lite6_product::kLifecycleProviderManifestId))) {
    return lite6_product::make_lite6_lifecycle_catalog(manifest);
  }
  if (manifest.provider_manifest_id ==
      core::ProviderManifestId(
          std::string(lite6_product::kGripperLifecycleProviderManifestId))) {
    return lite6_product::make_lite6_gripper_lifecycle_catalog(manifest);
  }
  return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(product_error(
      "native_lifecycle_product_unsupported",
      "the retained provider has no linked native lifecycle product",
      "provider_manifest_id_unsupported"));
}

} // namespace

RetainedNativeLifecycleProduct::RetainedNativeLifecycleProduct(
    core::BuildOwnedLifecyclePlanCatalogSource catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports)
    : catalog_source_(std::move(catalog_source)),
      brand_ports_(std::move(brand_ports)) {}

core::Result<std::unique_ptr<RetainedNativeLifecycleProduct>>
RetainedNativeLifecycleProduct::Create(
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const deployment::IVerifiedRetainedProviderManifestSource
        &provider_manifest_source,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
    core::IClock &clock, const core::Duration observation_freshness_ns) {
  if (retained_bundle == nullptr ||
      !deployment::verify_retained_bundle_identity(*retained_bundle) ||
      physical_bindings.empty() ||
      observation_freshness_ns <= 0 ||
      observation_freshness_ns >
          core::kMaxLifecycleTransactionTimeoutNs) {
    return core::Result<
        std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(product_error(
        "native_lifecycle_product_input_invalid",
        "the native lifecycle product requires one exact retained runtime",
        "bundle_bindings_or_freshness_invalid"));
  }

  using ManifestKey =
      std::pair<core::ProviderManifestId, core::ProviderManifestRevision>;
  std::map<ManifestKey, core::BuildOwnedLifecyclePlanCatalogEntryDTO> entries;
  std::vector<core::BrandLifecyclePortRegistrationDTO> registrations;
  registrations.reserve(
      retained_bundle->value().desired_control_configurations.size());

  for (const auto &desired :
       retained_bundle->value().desired_control_configurations) {
    const core::RuntimeControlPathDTO *selected_path = nullptr;
    for (const auto &binding : physical_bindings) {
      const auto &value = binding.value();
      if (!value.control_path.has_value() ||
          value.control_path->control_domain_id != desired.control_domain_id) {
        continue;
      }
      const auto &path = *value.control_path;
      if (path.control_domain_revision != desired.control_domain_revision) {
        return core::Result<
            std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(
            product_error("native_lifecycle_product_domain_stale",
                          "the native provider path is stale for its desired "
                          "control domain",
                          "control_domain_revision_mismatch"));
      }
      if (selected_path == nullptr) {
        selected_path = &path;
      } else if (std::tie(selected_path->provider_instance_id,
                          selected_path->provider_manifest_id,
                          selected_path->provider_manifest_revision,
                          selected_path->provider_runtime_unit_id,
                          selected_path->provider_runtime_epoch) !=
                 std::tie(path.provider_instance_id, path.provider_manifest_id,
                          path.provider_manifest_revision,
                          path.provider_runtime_unit_id,
                          path.provider_runtime_epoch)) {
        return core::Result<
            std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(
            product_error("native_lifecycle_product_domain_ambiguous",
                          "one control domain spans multiple lifecycle "
                          "provider occurrences",
                          "provider_occurrence_ambiguous"));
      }
    }
    if (selected_path == nullptr) {
      return core::Result<
          std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(product_error(
          "native_lifecycle_product_domain_missing",
          "a desired control domain has no physical provider occurrence",
          "physical_provider_occurrence_missing"));
    }

    const ManifestKey manifest_key{selected_path->provider_manifest_id,
                                   selected_path->provider_manifest_revision};
    auto entry = entries.find(manifest_key);
    if (entry == entries.end()) {
      const auto manifest = provider_manifest_source.get_verified(
          {retained_bundle->retained_bundle_id(),
           selected_path->provider_manifest_id,
           selected_path->provider_manifest_revision});
      if (!manifest.has_value()) {
        return core::Result<
            std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(
            product_error("native_lifecycle_provider_manifest_missing",
                          "the exact retained provider manifest is absent",
                          "provider_manifest_not_found"));
      }
      auto catalog = make_catalog(manifest->value());
      if (!catalog.ok()) {
        return core::Result<
            std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(
            catalog.error());
      }
      entry = entries
                  .emplace(manifest_key,
                           core::BuildOwnedLifecyclePlanCatalogEntryDTO{
                               manifest->value(), std::move(catalog.value())})
                  .first;
    }

    core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
        desired.control_domain_id,
        desired.control_domain_revision,
        selected_path->provider_instance_id,
        selected_path->provider_manifest_id,
        selected_path->provider_manifest_revision,
        selected_path->provider_runtime_unit_id,
        selected_path->provider_runtime_epoch,
        entry->second.catalog.catalog_digest};
    auto port = RetainedNativeBrandLifecyclePort::Create(
        occurrence, entry->second.catalog, clock, observation_freshness_ns);
    if (!port.ok()) {
      return core::Result<
          std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(port.error());
    }
    registrations.push_back({std::move(occurrence), std::move(port.value())});
  }

  std::vector<core::BuildOwnedLifecyclePlanCatalogEntryDTO> catalog_entries;
  catalog_entries.reserve(entries.size());
  for (auto &[unused, entry] : entries) {
    static_cast<void>(unused);
    catalog_entries.push_back(std::move(entry));
  }
  auto source = core::BuildOwnedLifecyclePlanCatalogSource::Make(
      std::move(catalog_entries));
  if (!source.ok()) {
    return core::Result<
        std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(source.error());
  }
  std::sort(registrations.begin(), registrations.end(),
            [](const auto &left, const auto &right) {
              return left.provider_occurrence.control_domain_id <
                     right.provider_occurrence.control_domain_id;
            });
  return core::Result<std::unique_ptr<RetainedNativeLifecycleProduct>>::Ok(
      std::unique_ptr<RetainedNativeLifecycleProduct>(
          new RetainedNativeLifecycleProduct(std::move(source.value()),
                                             std::move(registrations))));
}

const core::BuildOwnedLifecyclePlanCatalogSource &
RetainedNativeLifecycleProduct::catalog_source() const noexcept {
  return catalog_source_;
}

const std::vector<core::BrandLifecyclePortRegistrationDTO> &
RetainedNativeLifecycleProduct::brand_ports() const noexcept {
  return brand_ports_;
}

} // namespace manyforge::roboplan_backend
