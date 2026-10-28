#include <manyforge_deployment/component_lifecycle_current_snapshot_source.hpp>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::deployment {
namespace {

template <typename Value>
[[nodiscard]] core::Result<Value> fail(std::string reason,
                                       std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

template <typename Value, typename Identity, typename Projection>
[[nodiscard]] const Value *unique_by(const std::vector<Value> &values,
                                     const Identity &identity,
                                     Projection projection) noexcept {
  const Value *result = nullptr;
  for (const auto &value : values) {
    if (projection(value) != identity) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &value;
  }
  return result;
}

[[nodiscard]] bool contains(const std::vector<core::RuntimeUnitId> &values,
                            const core::RuntimeUnitId &value) noexcept {
  return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] bool route_legal(const core::ControlBindingDTO &binding,
                               const core::ControlRouteId &route) noexcept {
  return std::find(binding.legal_control_route_ids.begin(),
                   binding.legal_control_route_ids.end(),
                   route) != binding.legal_control_route_ids.end();
}

[[nodiscard]] const core::ControlBindingDTO *
binding_by_id(const std::vector<const core::ControlBindingDTO *> &bindings,
              const core::ControlBindingId &identity) noexcept {
  const core::ControlBindingDTO *result = nullptr;
  for (const auto *binding : bindings) {
    if (binding->control_binding_id != identity) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = binding;
  }
  return result;
}

[[nodiscard]] core::Result<std::vector<core::ExpectedControlBindingRevisionDTO>>
resolved_bindings(
    const core::ControlDomainDTO &domain,
    const core::DesiredControlConfigurationDTO &desired,
    const std::vector<const core::ControlBindingDTO *> &graph_bindings,
    const std::optional<core::ActiveControlConfigurationDTO> &current) {
  if (graph_bindings.empty() ||
      desired.control_domain_id != domain.control_domain_id ||
      desired.control_domain_revision != domain.control_domain_revision ||
      desired.controller_topology_id !=
          domain.controller().controller_topology_id ||
      desired.topology_revision != domain.controller().topology_revision) {
    return fail<std::vector<core::ExpectedControlBindingRevisionDTO>>(
        "retained_domain_configuration_incomplete",
        "retained desired control configuration does not close its domain");
  }

  std::vector<core::ExpectedControlBindingRevisionDTO> result;
  result.reserve(graph_bindings.size());
  if (current.has_value()) {
    if (!current->locally_valid() ||
        current->control_domain_id != domain.control_domain_id ||
        current->control_domain_revision != domain.control_domain_revision ||
        current->controller_topology_id !=
            domain.controller().controller_topology_id ||
        current->topology_revision != domain.controller().topology_revision ||
        current->binding_routes.size() != graph_bindings.size()) {
      return fail<std::vector<core::ExpectedControlBindingRevisionDTO>>(
          "current_active_configuration_mismatch",
          "current active control configuration does not close the retained "
          "domain");
    }
    for (const auto &route : current->binding_routes) {
      const auto *binding =
          binding_by_id(graph_bindings, route.control_binding_id);
      if (binding == nullptr ||
          binding->control_binding_revision != route.control_binding_revision ||
          !route_legal(*binding, route.control_route_id)) {
        return fail<std::vector<core::ExpectedControlBindingRevisionDTO>>(
            "current_binding_route_mismatch",
            "current active configuration contains a non-retained binding "
            "route");
      }
      result.push_back({route.control_binding_id,
                        route.control_binding_revision,
                        route.control_route_id});
    }
  } else {
    if (desired.binding_routes.size() != graph_bindings.size()) {
      return fail<std::vector<core::ExpectedControlBindingRevisionDTO>>(
          "bootstrap_binding_route_incomplete",
          "bootstrap desired configuration does not contain the complete "
          "domain binding set");
    }
    for (const auto &route : desired.binding_routes) {
      const auto *binding =
          binding_by_id(graph_bindings, route.control_binding_id);
      if (binding == nullptr ||
          binding->control_binding_revision != route.control_binding_revision ||
          !route_legal(*binding, route.control_route_id)) {
        return fail<std::vector<core::ExpectedControlBindingRevisionDTO>>(
            "bootstrap_binding_route_mismatch",
            "bootstrap desired configuration contains a non-retained binding "
            "route");
      }
      result.push_back({route.control_binding_id,
                        route.control_binding_revision,
                        route.control_route_id});
    }
  }
  std::sort(result.begin(), result.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.control_binding_id < rhs.control_binding_id;
  });
  if (std::adjacent_find(
          result.begin(), result.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.control_binding_id == rhs.control_binding_id;
          }) != result.end()) {
    return fail<std::vector<core::ExpectedControlBindingRevisionDTO>>(
        "current_binding_identity_ambiguous",
        "current component scope contains duplicate binding identities");
  }
  return core::Result<std::vector<core::ExpectedControlBindingRevisionDTO>>::Ok(
      std::move(result));
}

[[nodiscard]] bool provider_matches_component_binding(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    const core::ControllableComponentId &component,
    const std::vector<const core::ControlBindingDTO *> &bindings) noexcept {
  return std::any_of(
      bindings.begin(), bindings.end(), [&](const auto *binding) {
        return binding->controllable_component_id == component &&
               binding->provider_instance_id == provider.provider_instance_id &&
               binding->provider_manifest_id == provider.provider_manifest_id &&
               binding->provider_manifest_revision ==
                   provider.provider_manifest_revision &&
               binding->provider_runtime_unit_id ==
                   provider.provider_runtime_unit_id;
      });
}

} // namespace

VerifiedComponentLifecycleCurrentSnapshotSource::
    VerifiedComponentLifecycleCurrentSnapshotSource(
        const RetainedBundle &retained_bundle,
        const core::IActiveBundlePublicationReader &active_publication_reader,
        core::ActiveBundlePublicationReceiptDTO expected_active_publication,
        core::ICCLCCurrentFactReader &current_fact_reader,
        const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
        std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>
            provider_occurrences,
        std::function<core::Result<core::LifecycleDomainHealthDTO>(
            const core::ResolvedLifecycleProviderOccurrenceDTO &)>
            domain_health)
    : retained_bundle_(retained_bundle),
      active_publication_reader_(active_publication_reader),
      expected_active_publication_(std::move(expected_active_publication)),
      current_fact_reader_(current_fact_reader),
      lifecycle_catalog_source_(lifecycle_catalog_source),
      provider_occurrences_(std::move(provider_occurrences)),
      domain_health_(std::move(domain_health)) {
  if (!verify_retained_bundle_identity(retained_bundle_) ||
      !expected_active_publication_.valid() ||
      expected_active_publication_.request.candidate_retained_bundle_id !=
          retained_bundle_.retained_bundle_id() ||
      expected_active_publication_.request.candidate_graph_revision !=
          retained_bundle_.value().cell_graph.graph_revision) {
    throw RetainedBundleError(
        "component lifecycle current snapshot source: retained bundle and "
        "active publication do not match");
  }
  std::sort(provider_occurrences_.begin(), provider_occurrences_.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.control_domain_id < rhs.control_domain_id;
            });
  if (provider_occurrences_.size() > core::kMaxLifecycleProviderOccurrences ||
      std::any_of(
          provider_occurrences_.begin(), provider_occurrences_.end(),
          [](const auto &provider) { return !provider.locally_valid(); }) ||
      std::adjacent_find(
          provider_occurrences_.begin(), provider_occurrences_.end(),
          [](const auto &lhs, const auto &rhs) {
            return lhs.control_domain_id == rhs.control_domain_id;
          }) != provider_occurrences_.end()) {
    throw RetainedBundleError(
        "component lifecycle current snapshot source: provider occurrence "
        "inventory is invalid or ambiguous");
  }
}

core::Result<core::ComponentLifecycleCurrentSnapshotDTO>
VerifiedComponentLifecycleCurrentSnapshotSource::ReadCurrentComponentSnapshot(
    const core::ControllableComponentId &controllable_component_id) const {
  if (controllable_component_id.empty() ||
      controllable_component_id.value().size() >
          core::kMaxLifecycleIdentityBytes ||
      !verify_retained_bundle_identity(retained_bundle_)) {
    return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
        "component_or_bundle_invalid",
        "current component snapshot query or retained bundle is invalid");
  }

  const auto active = active_publication_reader_.ReadActiveBundlePublication(
      expected_active_publication_.publication_generation);
  if (!active.ok()) {
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
        active.error());
  }
  if (active.value() != expected_active_publication_) {
    return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
        "active_publication_changed",
        "active publication no longer matches the source construction anchor");
  }

  const auto &graph = retained_bundle_.value().cell_graph;
  const auto *component = unique_by(
      graph.controllable_components, controllable_component_id,
      [](const auto &value) { return value.controllable_component_id; });
  if (component == nullptr) {
    return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
        "controllable_component_not_found_or_ambiguous",
        "the active retained graph does not contain exactly one requested "
        "component");
  }

  std::vector<core::ControlDomainId> domain_ids;
  std::vector<const core::ControlBindingDTO *> component_bindings;
  for (const auto &binding : graph.control_bindings) {
    if (binding.controllable_component_id == controllable_component_id) {
      domain_ids.push_back(binding.control_domain_id);
      component_bindings.push_back(&binding);
    }
  }
  for (const auto &domain : graph.control_domains) {
    if (const auto *provider = domain.provider();
        provider &&
        provider->controllable_component_id == controllable_component_id)
      domain_ids.push_back(domain.control_domain_id);
  }
  std::sort(domain_ids.begin(), domain_ids.end());
  domain_ids.erase(std::unique(domain_ids.begin(), domain_ids.end()),
                   domain_ids.end());
  if (domain_ids.empty() ||
      domain_ids.size() > core::kMaxLifecycleResolvedDomains) {
    return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
        "component_domain_scope_missing_or_oversized",
        "requested component has no bounded retained control-domain scope");
  }

  const auto current_facts = current_fact_reader_.LoadCurrentSnapshotFacts(
      {controllable_component_id}, domain_ids);
  if (!current_facts.ok()) {
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
        current_facts.error());
  }
  if (!current_facts.value().locally_valid() ||
      current_facts.value().components.size() != 1U ||
      current_facts.value().components.front().controllable_component_id !=
          controllable_component_id ||
      current_facts.value().domains.size() != domain_ids.size()) {
    return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
        "current_fact_readback_mismatch",
        "journal current-fact readback does not match the exact component "
        "query");
  }

  core::ComponentLifecycleGeneration lifecycle_generation(1U);
  auto lifecycle_state = core::ComponentLifecycleStateDTO::kDeclared;
  const auto &component_fact = current_facts.value().components.front();
  // Settled below from fresh domain health when the numbered fact is missing.
  const bool settle_from_health = component_fact.durable_generation.has_value() &&
                                  !component_fact.current_state.has_value();
  if (component_fact.durable_generation.has_value()) {
    lifecycle_generation = *component_fact.durable_generation;
    lifecycle_state = component_fact.current_state.value_or(
        core::ComponentLifecycleStateDTO::kDeclared);
  } else if (component_fact.current_state.has_value()) {
    return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
        "current_lifecycle_generation_missing",
        "current lifecycle state has no durable generation");
  }

  std::vector<core::ResolvedLifecycleControlDomainScopeDTO> domains;
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers;
  domains.reserve(domain_ids.size());
  providers.reserve(domain_ids.size());
  for (std::size_t index = 0U; index < domain_ids.size(); ++index) {
    const auto &domain_id = domain_ids[index];
    const auto &domain_fact = current_facts.value().domains[index];
    if (domain_fact.control_domain_id != domain_id) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "current_domain_fact_order_mismatch",
          "journal current domain facts are not the requested canonical set");
    }
    const auto *graph_domain =
        unique_by(graph.control_domains, domain_id,
                  [](const auto &value) { return value.control_domain_id; });
    const auto *desired = unique_by(
        retained_bundle_.value().desired_control_configurations, domain_id,
        [](const auto &value) { return value.control_domain_id; });
    if (graph_domain == nullptr ||
        (!graph_domain->provider() && desired == nullptr)) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "retained_domain_not_found_or_ambiguous",
          "component domain has no unique retained domain and desired "
          "configuration");
    }

    const auto occurrence = std::lower_bound(
        provider_occurrences_.begin(), provider_occurrences_.end(), domain_id,
        [](const auto &candidate, const auto &requested) {
          return candidate.control_domain_id < requested;
        });
    if (occurrence == provider_occurrences_.end() ||
        occurrence->control_domain_id != domain_id)
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "lifecycle_provider_occurrence_mismatch",
          "domain has no actual provider occurrence");

    const auto build_scope =
        [&]() -> core::Result<core::ResolvedLifecycleControlDomainScopeDTO> {
      if (const auto *driver = graph_domain->provider()) {
        if (driver->controllable_component_id != controllable_component_id ||
            domain_fact.current_active_control_configuration ||
            !core::ProviderDomainMatchesOccurrence(
                domain_id, graph_domain->control_domain_revision, *driver,
                occurrence->provider_runtime_epoch, *occurrence))
          return fail<core::ResolvedLifecycleControlDomainScopeDTO>(
              "provider_domain_fact_mismatch",
              "provider domain does not match its actual occurrence");
        const auto catalog = ResolveRetainedIoSignalCatalog(graph);
        if (!catalog.ok())
          return core::Result<core::ResolvedLifecycleControlDomainScopeDTO>::
              Err(catalog.error());
        std::vector<core::ResourceId> outputs;
        for (const auto &row : catalog.value()) {
          if (row.runtime_unit_id == driver->provider_runtime_unit_id &&
              row.descriptor.signal_id.controllable_component_id ==
                  driver->controllable_component_id &&
              std::binary_search(driver->signal_names.begin(),
                                 driver->signal_names.end(),
                                 row.descriptor.signal_id.name) &&
              row.descriptor.output_resource)
            outputs.push_back(*row.descriptor.output_resource);
        }
        std::sort(outputs.begin(), outputs.end());
        outputs.erase(std::unique(outputs.begin(), outputs.end()),
                      outputs.end());
        return core::Result<core::ResolvedLifecycleControlDomainScopeDTO>::Ok(
            {domain_id, graph_domain->control_domain_revision,
             core::ResolvedProviderDomainScopeDTO{
                 *driver, occurrence->provider_runtime_epoch, std::nullopt,
                 std::move(outputs)}});
      }

      std::vector<const core::ControlBindingDTO *> graph_bindings;
      std::set<core::ResourceId> resources;
      for (const auto &binding : graph.control_bindings) {
        if (binding.control_domain_id != domain_id) {
          continue;
        }
        if (binding.controller_topology_id !=
                graph_domain->controller().controller_topology_id ||
            binding.topology_revision !=
                graph_domain->controller().topology_revision) {
          return fail<core::ResolvedLifecycleControlDomainScopeDTO>(
              "retained_binding_topology_mismatch",
              "control binding does not match its retained domain topology");
        }
        graph_bindings.push_back(&binding);
        for (const auto &resource : binding.resource_bindings) {
          resources.insert(resource.resource_id);
        }
      }
      const auto bindings =
          resolved_bindings(*graph_domain, *desired, graph_bindings,
                            domain_fact.current_active_control_configuration);
      if (!bindings.ok() || resources.empty()) {
        if (!bindings.ok()) {
          return core::Result<core::ResolvedLifecycleControlDomainScopeDTO>::
              Err(bindings.error());
        }
        return fail<core::ResolvedLifecycleControlDomainScopeDTO>(
            "retained_domain_resources_missing",
            "component domain has no complete retained resource scope");
      }

      core::ResolvedLifecycleControlDomainScopeDTO scope{
          graph_domain->control_domain_id,
          graph_domain->control_domain_revision,
          graph_domain->controller().controller_topology_id,
          graph_domain->controller().topology_revision,
          domain_fact.current_active_control_configuration,
          bindings.value(),
          std::vector<core::ResourceId>(resources.begin(), resources.end())};
      if (!scope.locally_valid()) {
        return fail<core::ResolvedLifecycleControlDomainScopeDTO>(
            "resolved_domain_scope_invalid",
            "resolved current domain scope is not one closed canonical value");
      }
      return core::Result<core::ResolvedLifecycleControlDomainScopeDTO>::Ok(
          std::move(scope));
    };
    auto built = build_scope();
    if (!built.ok())
      return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
          built.error());
    if (occurrence == provider_occurrences_.end() ||
        occurrence->control_domain_id != domain_id ||
        occurrence->control_domain_revision !=
            graph_domain->control_domain_revision ||
        !contains(graph_domain->runtime_unit_ids,
                  occurrence->provider_runtime_unit_id) ||
        !contains(component->runtime_unit_ids,
                  occurrence->provider_runtime_unit_id) ||
        (!graph_domain->provider() &&
         !provider_matches_component_binding(
             *occurrence, controllable_component_id, component_bindings))) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "lifecycle_provider_occurrence_mismatch",
          "component domain has no exact current retained lifecycle provider "
          "occurrence");
    }
    const auto catalog =
        lifecycle_catalog_source_.LoadExactLifecyclePlanCatalog(
            occurrence->provider_manifest_id,
            occurrence->provider_manifest_revision,
            occurrence->lifecycle_plan_catalog_digest);
    if (!catalog.ok()) {
      return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
          catalog.error());
    }
    const auto catalog_verified =
        core::VerifyBrandLifecyclePlanCatalog(catalog.value());
    if (!catalog_verified.ok() ||
        catalog.value().provider_manifest_id !=
            occurrence->provider_manifest_id ||
        catalog.value().provider_manifest_revision !=
            occurrence->provider_manifest_revision ||
        catalog.value().catalog_digest !=
            occurrence->lifecycle_plan_catalog_digest) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "lifecycle_catalog_readback_mismatch",
          "lifecycle catalog source returned a non-exact provider occurrence");
    }
    domains.push_back(std::move(built.value()));
    providers.push_back(*occurrence);
  }

  if (settle_from_health) {
    std::vector<std::optional<core::ComponentLifecycleStateDTO>> observed;
    observed.reserve(providers.size());
    for (const auto &provider : providers) {
      const auto health = domain_health_
                              ? domain_health_(provider)
                              : core::Result<core::LifecycleDomainHealthDTO>::Err(
                                    core::ErrorDTO{"lifecycle_health_unavailable", "", {}});
      observed.push_back(health.ok() ? std::optional(health.value().observed_state)
                                     : std::nullopt);
    }
    // The lifecycle facts must not change during the health join: a
    // transaction accepted meanwhile would otherwise pair its generation
    // with a state settled before it.
    const auto facts_after = current_fact_reader_.LoadCurrentSnapshotFacts(
        {controllable_component_id}, domain_ids);
    if (!facts_after.ok()) {
      return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
          facts_after.error());
    }
    if (facts_after.value().components != current_facts.value().components ||
        facts_after.value().domains != current_facts.value().domains) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "current_fact_changed_during_read",
          "lifecycle facts changed while domain health was joined");
    }
    const auto settled = core::SettleObservedComponentLifecycleState(observed);
    lifecycle_state = settled.state;
    if (settled.reconstructed) {
      std::lock_guard lock(reconstructed_log_mutex_);
      auto &logged = reconstructed_logged_[controllable_component_id];
      if (logged != lifecycle_generation.value()) {
        logged = lifecycle_generation.value();
        (void)std::fprintf(stderr,
                           "manyforge_cclc_lifecycle_state_reconstructed "
                           "component=%.128s generation=%llu state=declared\n",
                           controllable_component_id.value().c_str(),
                           static_cast<unsigned long long>(logged));
      }
    }
  }

  const auto active_reread =
      active_publication_reader_.ReadActiveBundlePublication(
          expected_active_publication_.publication_generation);
  if (!active_reread.ok()) {
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
        active_reread.error());
  }
  if (active_reread.value() != expected_active_publication_) {
    return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
        "active_publication_changed_during_read",
        "active publication changed while current component facts were joined");
  }

  core::ComponentLifecycleCurrentSnapshotDTO result{
      core::ContentDigest("pending"),
      controllable_component_id,
      lifecycle_generation,
      lifecycle_state,
      expected_active_publication_,
      std::move(domains),
      std::move(providers)};
  const auto digest =
      core::ComputeComponentLifecycleCurrentSnapshotDigest(result);
  if (!digest.ok()) {
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
        digest.error());
  }
  result.snapshot_digest = digest.value();
  const auto verified = core::VerifyComponentLifecycleCurrentSnapshot(result);
  if (!verified.ok()) {
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
        verified.error());
  }
  return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Ok(
      std::move(result));
}

} // namespace manyforge::deployment
