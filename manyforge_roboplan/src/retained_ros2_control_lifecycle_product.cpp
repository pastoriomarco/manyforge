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

[[nodiscard]] core::LifecyclePredicateDTO
predicate(std::vector<core::ComponentLifecycleStateDTO> states,
          const std::vector<std::pair<const char *, bool>> &facts) {
  std::sort(states.begin(), states.end());
  std::vector<core::LifecycleFactPredicateDTO> values;
  for (const auto &[id, value] : facts) {
    values.push_back({core::LifecycleFactId(id), boolean_value(value)});
  }
  std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  });
  return {std::move(states), std::move(values)};
}

} // namespace

std::vector<core::LifecycleIntentPlanDTO>
retained_ros2_control_lifecycle_plans() {
  using State = core::ComponentLifecycleStateDTO;
  using Intent = core::ComponentLifecycleIntentKindDTO;
  constexpr const char *kReady = "external.controller_configuration_ready";
  constexpr const char *kLoaded = "external.controller_loaded";
  constexpr const char *kFault = "external.fault_present";
  constexpr const char *kTransport = "external.transport_ready";
  // Prepared (or already active): the trajectory controller is loaded on
  // active hardware and the controller manager is reachable.
  const auto prepared = predicate({State::kPrepared, State::kActive},
                                  {{kLoaded, true}, {kFault, false}, {kTransport, true}});
  const auto active = predicate(
      {State::kActive},
      {{kReady, true}, {kLoaded, true}, {kFault, false}, {kTransport, true}});
  const auto disabled = predicate(
      {State::kDisabled}, {{kReady, false}, {kFault, false}, {kTransport, true}});
  constexpr std::int64_t timeout = 30'000'000'000LL;
  // Design R6 / §7.5: the spawner loads the controller inactive; CCLC Enable
  // activates it and Disable deactivates it (`switch_controller`). Recover
  // (the operator's Reset of an incident, Q41) re-prepares the component;
  // the target driver's next Enable activates the controller again.
  return {
      {Intent::kPrepare, State::kPrepared, std::nullopt, prepared, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.prepare"),
         predicate({State::kDeclared, State::kDisabled},
                   {{kLoaded, true}, {kFault, false}, {kTransport, true}}),
         prepared}}},
      {Intent::kEnable, State::kActive, std::nullopt, active, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.activate"),
         predicate({State::kPrepared}, {{kReady, false},
                                        {kLoaded, true},
                                        {kFault, false},
                                        {kTransport, true}}),
         active}}},
      {Intent::kDisable, State::kDisabled, std::nullopt, disabled, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.deactivate"),
         predicate({State::kPrepared, State::kActive, State::kDegraded},
                   {{kTransport, true}}),
         disabled}}},
      {Intent::kRecover, State::kPrepared,
       predicate({State::kPrepared, State::kActive, State::kDegraded,
                  State::kFaulted},
                 {{kTransport, true}}),
       prepared, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.recover"),
         predicate({State::kDegraded, State::kFaulted}, {{kTransport, true}}),
         prepared}}},
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
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    RetainedExternallyManagedDomainSnapshotSource snapshot_source)
    : snapshot_source_(std::move(snapshot_source)),
      catalog_source_(std::move(catalog_source)),
      brand_ports_(std::move(brand_ports)) {}

core::Result<std::unique_ptr<RetainedRos2ControlLifecycleProduct>>
RetainedRos2ControlLifecycleProduct::Create(
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const deployment::IVerifiedRetainedProviderManifestSource
        &provider_manifest_source,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
    const std::vector<std::shared_ptr<RetainedRos2ControlRuntimeHandle>>
        &runtime_handles,
    core::IClock &clock, const core::Duration observation_freshness_ns) {
  if (retained_bundle == nullptr ||
      !deployment::verify_retained_bundle_identity(*retained_bundle) ||
      physical_bindings.empty() || runtime_handles.empty() ||
      std::any_of(runtime_handles.begin(), runtime_handles.end(),
                  [](const auto &handle) { return handle == nullptr; }) ||
      observation_freshness_ns <= 0 ||
      observation_freshness_ns > core::kMaxLifecycleTransactionTimeoutNs) {
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
  std::map<core::ControlDomainId,
           std::shared_ptr<RetainedRos2ControlRuntimeHandle>>
      domain_handles;
  for (const auto &binding : physical_bindings) {
    if (!binding.value().control_path.has_value())
      continue;
    const auto owner = std::find_if(
        runtime_handles.begin(), runtime_handles.end(),
        [&](const auto &handle) { return handle->OwnsBinding(binding); });
    if (owner == runtime_handles.end() ||
        std::count_if(runtime_handles.begin(), runtime_handles.end(),
                      [&](const auto &handle) {
                        return handle->OwnsBinding(binding);
                      }) != 1) {
      return core::
          Result<std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
              product_error(
                  "ros2_control_lifecycle_owner_missing",
                  "a physical binding has no unique live ROS runtime handle",
                  "physical_handle_not_unique"));
    }
    const auto [existing, inserted] = domain_handles.emplace(
        binding.value().control_path->control_domain_id, *owner);
    if (!inserted && existing->second != *owner) {
      return core::
          Result<std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
              product_error("ros2_control_lifecycle_owner_ambiguous",
                            "a control domain crosses live ROS runtime handles",
                            "domain_handle_ambiguous"));
    }
  }
  RetainedExternallyManagedDomainSnapshotSource snapshot_source =
      [domain_handles](const core::ControlDomainId &domain,
                       core::TimePoint now) {
        const auto owner = domain_handles.find(domain);
        if (owner == domain_handles.end())
          return core::Result<RetainedExternallyManagedDomainSnapshot>::Err(
              product_error("ros2_control_lifecycle_domain_missing",
                            "the requested domain has no owned ROS handle",
                            "domain_handle_missing"));
        return owner->second->ReadLifecycleSnapshot(domain, now);
      };

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
    const auto handle = domain_handles.find(desired.control_domain_id);
    if (handle == domain_handles.end())
      return core::
          Result<std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Err(
              product_error("ros2_control_lifecycle_domain_missing",
                            "a desired domain has no live ROS runtime handle",
                            "domain_handle_missing"));
    auto port = RetainedExternallyManagedBrandLifecyclePort::Create(
        occurrence, snapshot_source, clock, observation_freshness_ns,
        handle->second->lifecycle_primitive_dispatcher());
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
  return core::Result<std::unique_ptr<RetainedRos2ControlLifecycleProduct>>::Ok(
      std::unique_ptr<RetainedRos2ControlLifecycleProduct>(
          new RetainedRos2ControlLifecycleProduct(std::move(source.value()),
                                                  std::move(registrations),
                                                  std::move(snapshot_source))));
}

RetainedExternallyManagedDomainSnapshotSource
RetainedRos2ControlLifecycleProduct::lifecycle_snapshot_source() const {
  return snapshot_source_;
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
