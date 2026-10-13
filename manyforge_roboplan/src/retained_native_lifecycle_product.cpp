#include <manyforge_roboplan/retained_native_lifecycle_product.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
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
  // Any native provider that anchors exactly the generic native plans under
  // its own manifest ID selects this product; a vendor catalog (for example
  // the Lite6 real-robot lifecycle) anchors other plans and is refused.
  if (manifest.lifecycle_plan_catalog.has_value()) {
    auto catalog = core::MakeBrandLifecyclePlanCatalog(
        manifest.provider_manifest_id, manifest.provider_manifest_revision,
        core::MakeNativeControlLifecyclePlans());
    if (catalog.ok() &&
        core::VerifyBrandLifecyclePlanCatalogManifestAnchor(manifest, catalog.value()).ok()) {
      return catalog;
    }
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
    core::IClock &clock, const core::Duration observation_freshness_ns,
    std::function<core::Result<void>(const core::ControlDomainId &)>
        verify_native_engine,
    std::function<core::Result<bool>(const core::ControlDomainId &)>
        route_ready,
    std::function<core::Result<std::optional<core::ComponentLifecycleStateDTO>>(
        const core::ControllableComponentId &)>
        retained_state,
    bool allow_closed_active_during_route_recovery) {
  if (retained_bundle == nullptr ||
      !deployment::verify_retained_bundle_identity(*retained_bundle) ||
      physical_bindings.empty() || !verify_native_engine || !route_ready ||
      !retained_state ||
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
    std::optional<core::ControllableComponentId> selected_component;
    for (const auto &binding : physical_bindings) {
      const auto &value = binding.value();
      if (!value.control_path.has_value() ||
          value.control_path->control_domain_id != desired.control_domain_id) {
        continue;
      }
      const auto &path = *value.control_path;
      if (!selected_component) {
        selected_component = value.controllable_component_id;
      } else if (*selected_component != value.controllable_component_id) {
        return core::Result<
            std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(
            product_error("native_lifecycle_domain_component_ambiguous",
                          "one native control domain spans multiple components",
                          "component_domain_ambiguous"));
      }
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
    if (selected_path == nullptr || !selected_component) {
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
    auto initial = retained_state(*selected_component);
    if (!initial.ok()) {
      return core::Result<
          std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(initial.error());
    }
    auto port = RetainedNativeBrandLifecyclePort::Create(
        occurrence, entry->second.catalog, clock, observation_freshness_ns,
        [verify_native_engine,
         domain = desired.control_domain_id]() {
          return verify_native_engine(domain);
        },
        [route_ready, domain = desired.control_domain_id]() {
          return route_ready(domain);
        },
        initial.value(), allow_closed_active_during_route_recovery);
    if (!port.ok()) {
      return core::Result<
          std::unique_ptr<RetainedNativeLifecycleProduct>>::Err(port.error());
    }
    auto native_port = std::move(port.value());
    registrations.push_back(
        {std::move(occurrence), native_port,
         [native_port, domain = desired.control_domain_id](
             std::int64_t now_ns) {
           return native_port->ReadLifecycleHealth(domain,
                                                   core::TimePoint(now_ns));
         }});
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
