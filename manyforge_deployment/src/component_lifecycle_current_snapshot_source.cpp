#include <manyforge_deployment/component_lifecycle_current_snapshot_source.hpp>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>

#include <algorithm>
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
      desired.controller_topology_id != domain.controller_topology_id ||
      desired.topology_revision != domain.topology_revision) {
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
        current->controller_topology_id != domain.controller_topology_id ||
        current->topology_revision != domain.topology_revision ||
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
            provider_occurrences)
    : retained_bundle_(retained_bundle),
      active_publication_reader_(active_publication_reader),
      expected_active_publication_(std::move(expected_active_publication)),
      current_fact_reader_(current_fact_reader),
      lifecycle_catalog_source_(lifecycle_catalog_source),
      provider_occurrences_(std::move(provider_occurrences)) {
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
  if (component_fact.durable_generation.has_value()) {
    if (!component_fact.current_state.has_value()) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "current_lifecycle_state_unresolved",
          "durably numbered component has no coherent current lifecycle state");
    }
    lifecycle_generation = *component_fact.durable_generation;
    lifecycle_state = *component_fact.current_state;
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
    if (graph_domain == nullptr || desired == nullptr) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "retained_domain_not_found_or_ambiguous",
          "component domain has no unique retained domain and desired "
          "configuration");
    }

    std::vector<const core::ControlBindingDTO *> graph_bindings;
    std::set<core::ResourceId> resources;
    for (const auto &binding : graph.control_bindings) {
      if (binding.control_domain_id != domain_id) {
        continue;
      }
      if (binding.controller_topology_id !=
              graph_domain->controller_topology_id ||
          binding.topology_revision != graph_domain->topology_revision) {
        return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
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
        return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
            bindings.error());
      }
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "retained_domain_resources_missing",
          "component domain has no complete retained resource scope");
    }

    core::ResolvedLifecycleControlDomainScopeDTO scope{
        graph_domain->control_domain_id,
        graph_domain->control_domain_revision,
        graph_domain->controller_topology_id,
        graph_domain->topology_revision,
        domain_fact.current_active_control_configuration,
        bindings.value(),
        std::vector<core::ResourceId>(resources.begin(), resources.end())};
    if (!scope.locally_valid()) {
      return fail<core::ComponentLifecycleCurrentSnapshotDTO>(
          "resolved_domain_scope_invalid",
          "resolved current domain scope is not one closed canonical value");
    }

    const auto occurrence = std::lower_bound(
        provider_occurrences_.begin(), provider_occurrences_.end(), domain_id,
        [](const auto &candidate, const auto &requested) {
          return candidate.control_domain_id < requested;
        });
    if (occurrence == provider_occurrences_.end() ||
        occurrence->control_domain_id != domain_id ||
        occurrence->control_domain_revision !=
            graph_domain->control_domain_revision ||
        !contains(graph_domain->runtime_unit_ids,
                  occurrence->provider_runtime_unit_id) ||
        !contains(component->runtime_unit_ids,
                  occurrence->provider_runtime_unit_id) ||
        !provider_matches_component_binding(
            *occurrence, controllable_component_id, component_bindings)) {
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
    domains.push_back(std::move(scope));
    providers.push_back(*occurrence);
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
