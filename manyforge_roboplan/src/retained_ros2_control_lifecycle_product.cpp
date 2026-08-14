#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::ErrorDTO product_error(std::string code,
                                           std::string message,
                                           std::string reason) {
  return {std::move(code), std::move(message),
          {{"reason", std::move(reason)}}};
}

[[nodiscard]] core::LifecycleFactValueDTO boolean_value(const bool value) {
  return {core::LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
          std::nullopt};
}

[[nodiscard]] core::LifecyclePredicateDTO predicate(
    std::vector<core::ComponentLifecycleStateDTO> states,
    const bool controller_ready, const bool fault_present,
    const bool transport_ready) {
  return {
      std::move(states),
      {{core::LifecycleFactId("external.controller_configuration_ready"),
        boolean_value(controller_ready)},
       {core::LifecycleFactId("external.fault_present"),
        boolean_value(fault_present)},
       {core::LifecycleFactId("external.transport_ready"),
        boolean_value(transport_ready)}}};
}

} // namespace

std::vector<core::LifecycleIntentPlanDTO>
retained_ros2_control_lifecycle_plans() {
  const auto ready = predicate(
      {core::ComponentLifecycleStateDTO::kPrepared,
       core::ComponentLifecycleStateDTO::kActive},
      true, false, true);
  const auto active = predicate(
      {core::ComponentLifecycleStateDTO::kActive}, true, false, true);
  const auto disabled = predicate(
      {core::ComponentLifecycleStateDTO::kDisabled}, false, false, true);

  // These are observation plans. An unsatisfied request fails known-no-effect;
  // the external ros2_control owner must independently change lifecycle state.
  return {
      {core::ComponentLifecycleIntentKindDTO::kPrepare,
       core::ComponentLifecycleStateDTO::kPrepared, std::nullopt, ready,
       30'000'000'000LL, {}},
      {core::ComponentLifecycleIntentKindDTO::kEnable,
       core::ComponentLifecycleStateDTO::kActive, std::nullopt, active,
       30'000'000'000LL, {}},
      {core::ComponentLifecycleIntentKindDTO::kDisable,
       core::ComponentLifecycleStateDTO::kDisabled, std::nullopt, disabled,
       30'000'000'000LL, {}},
      {core::ComponentLifecycleIntentKindDTO::kRecover,
       core::ComponentLifecycleStateDTO::kDisabled, disabled, disabled,
       30'000'000'000LL, {}},
  };
}

core::Result<core::BrandLifecyclePlanCatalogDTO>
make_retained_ros2_control_lifecycle_catalog(
    const core::ProviderManifestDTO &manifest) {
  if (manifest.provider_manifest_id.value() !=
      kRos2ControlLifecycleProviderManifestId) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(product_error(
        "ros2_control_lifecycle_product_unsupported",
        "the provider manifest is not the retained ROS control product",
        "provider_manifest_id_unsupported"));
  }
  auto catalog = core::MakeBrandLifecyclePlanCatalog(
      manifest.provider_manifest_id, manifest.provider_manifest_revision,
      retained_ros2_control_lifecycle_plans());
  if (!catalog.ok()) {
    return catalog;
  }
  const auto anchored = core::VerifyBrandLifecyclePlanCatalogManifestAnchor(
      manifest, catalog.value());
  if (!anchored.ok()) {
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        anchored.error());
  }
  return catalog;
}

RetainedRos2ControlLifecycleProduct::RetainedRos2ControlLifecycleProduct(
    core::BuildOwnedLifecyclePlanCatalogSource catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports)
    : catalog_source_(std::move(catalog_source)),
      brand_ports_(std::move(brand_ports)) {}

core::Result<std::unique_ptr<RetainedRos2ControlLifecycleProduct>>
RetainedRos2ControlLifecycleProduct::Create(
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const deployment::IVerifiedRetainedProviderManifestSource
        &provider_manifest_source,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle,
    core::IClock &clock, const core::Duration observation_freshness_ns) {
  if (retained_bundle == nullptr ||
      !deployment::verify_retained_bundle_identity(*retained_bundle) ||
      physical_bindings.empty() || runtime_handle == nullptr ||
      observation_freshness_ns <= 0 ||
      observation_freshness_ns >
          core::kMaxLifecycleTransactionTimeoutNs) {
    return core::Result<
        std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
        product_error("ros2_control_lifecycle_product_input_invalid",
                      "the ROS lifecycle product requires one exact retained "
                      "runtime and observer handle",
                      "bundle_bindings_handle_or_freshness_invalid"));
  }

  using ManifestKey =
      std::pair<core::ProviderManifestId, core::ProviderManifestRevision>;
  std::map<ManifestKey, core::BuildOwnedLifecyclePlanCatalogEntryDTO> entries;
  std::vector<core::BrandLifecyclePortRegistrationDTO> registrations;
  registrations.reserve(
      retained_bundle->value().desired_control_configurations.size());
  const auto snapshot_source = runtime_handle->lifecycle_snapshot_source();

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
            std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
            product_error("ros2_control_lifecycle_product_domain_stale",
                          "the ROS provider path is stale for its desired "
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
            std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
            product_error("ros2_control_lifecycle_product_domain_ambiguous",
                          "one control domain spans multiple ROS lifecycle "
                          "provider occurrences",
                          "provider_occurrence_ambiguous"));
      }
    }
    if (selected_path == nullptr) {
      return core::Result<
          std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
          product_error("ros2_control_lifecycle_product_domain_missing",
                        "a desired control domain has no physical ROS "
                        "provider occurrence",
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
            std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
            product_error("ros2_control_lifecycle_provider_manifest_missing",
                          "the exact retained ROS provider manifest is absent",
                          "provider_manifest_not_found"));
      }
      auto catalog =
          make_retained_ros2_control_lifecycle_catalog(manifest->value());
      if (!catalog.ok()) {
        return core::Result<
            std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
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
    auto port = RetainedExternallyManagedBrandLifecyclePort::Create(
        occurrence, snapshot_source, clock, observation_freshness_ns);
    if (!port.ok()) {
      return core::Result<
          std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
          port.error());
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
        std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
        source.error());
  }
  std::sort(registrations.begin(), registrations.end(),
            [](const auto &left, const auto &right) {
              return left.provider_occurrence.control_domain_id <
                     right.provider_occurrence.control_domain_id;
            });
  return core::Result<
      std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Ok(
      std::unique_ptr<RetainedRos2ControlLifecycleProduct>(
          new RetainedRos2ControlLifecycleProduct(std::move(source.value()),
                                                  std::move(registrations))));
}

const core::BuildOwnedLifecyclePlanCatalogSource &
RetainedRos2ControlLifecycleProduct::catalog_source() const noexcept {
  return catalog_source_;
}

const std::vector<core::BrandLifecyclePortRegistrationDTO> &
RetainedRos2ControlLifecycleProduct::brand_ports() const noexcept {
  return brand_ports_;
}

} // namespace manyforge::roboplan_backend
