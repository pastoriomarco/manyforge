#include <manyforge_deployment/route_selection_current_snapshot_source.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
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

template <typename Values, typename Identity, typename Projection>
[[nodiscard]] const typename Values::value_type *
unique_by(const Values &values, const Identity &identity,
          Projection projection) noexcept {
  const typename Values::value_type *result = nullptr;
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

[[nodiscard]] bool bounded_identity(const auto &value) noexcept {
  return !value.empty() &&
         value.value().size() <= core::kMaxRouteSelectionIdentityBytes;
}

} // namespace

bool ControllerLifecycleSourceOccurrenceDTO::locally_valid() const noexcept {
  return bounded_identity(control_domain_id) &&
         bounded_identity(source_runtime_unit_id) &&
         bounded_identity(source_runtime_epoch);
}

VerifiedRouteSelectionCurrentSnapshotSource::
    VerifiedRouteSelectionCurrentSnapshotSource(
        const RetainedBundle &retained_bundle,
        const core::IActiveBundlePublicationReader &active_publication_reader,
        core::ActiveBundlePublicationReceiptDTO expected_active_publication,
        core::ICCLCCurrentFactReader &current_fact_reader,
        const IVerifiedRetainedProductArtifactSource &artifact_source,
        std::vector<ControllerLifecycleSourceOccurrenceDTO> controller_sources,
        std::function<
            core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
                const core::ControlDomainId &)>
            kernel_current_configuration_source)
    : retained_bundle_(retained_bundle),
      active_publication_reader_(active_publication_reader),
      expected_active_publication_(std::move(expected_active_publication)),
      current_fact_reader_(current_fact_reader),
      artifact_source_(artifact_source),
      controller_sources_(std::move(controller_sources)),
      kernel_current_configuration_source_(
          std::move(kernel_current_configuration_source)) {
  if (!verify_retained_bundle_identity(retained_bundle_) ||
      !expected_active_publication_.valid() ||
      expected_active_publication_.request.candidate_retained_bundle_id !=
          retained_bundle_.retained_bundle_id() ||
      expected_active_publication_.request.candidate_graph_revision !=
          retained_bundle_.value().cell_graph.graph_revision) {
    throw RetainedBundleError(
        "route current-snapshot source has mismatched retained/publication "
        "anchors");
  }
  std::sort(controller_sources_.begin(), controller_sources_.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.control_domain_id < rhs.control_domain_id;
            });
  const auto &bundle = retained_bundle_.value();
  const auto &graph = bundle.cell_graph;
  const bool empty_physical_graph =
      graph.controllable_components.empty() && graph.control_domains.empty() &&
      graph.control_bindings.empty() &&
      bundle.desired_control_configurations.empty();
  if ((controller_sources_.empty() && !empty_physical_graph) ||
      controller_sources_.size() > core::kMaxRouteSelectionComponents ||
      std::any_of(controller_sources_.begin(), controller_sources_.end(),
                  [](const auto &row) { return !row.locally_valid(); }) ||
      std::adjacent_find(controller_sources_.begin(), controller_sources_.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.control_domain_id ==
                                  rhs.control_domain_id;
                         }) != controller_sources_.end()) {
    throw RetainedBundleError(
        "route current-snapshot controller-source inventory is invalid");
  }
}

core::Result<core::RouteSelectionCurrentSnapshotDTO>
VerifiedRouteSelectionCurrentSnapshotSource::ReadCurrentRouteClosure(
    const core::RetainedBundleId &expected_retained_bundle_id,
    const core::GraphRevision &expected_graph_revision,
    const core::DesiredControlConfigurationDTO &desired_configuration,
    const bool observation_only) const {
  if (!bounded_identity(expected_retained_bundle_id) ||
      !bounded_identity(expected_graph_revision) ||
      desired_configuration.control_domain_id.empty() ||
      desired_configuration.control_domain_revision.empty() ||
      desired_configuration.controller_topology_id.empty() ||
      desired_configuration.topology_revision.empty() ||
      desired_configuration.activation_set_id.empty() ||
      desired_configuration.binding_routes.empty() ||
      expected_retained_bundle_id != retained_bundle_.retained_bundle_id() ||
      expected_graph_revision !=
          retained_bundle_.value().cell_graph.graph_revision ||
      !verify_retained_bundle_identity(retained_bundle_)) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "query_or_retained_identity_mismatch",
        "route current-snapshot query does not name the retained source");
  }

  const auto read_active = [&]() {
    return active_publication_reader_.ReadActiveBundlePublication(
        expected_active_publication_.publication_generation);
  };
  const auto active_before = read_active();
  if (!active_before.ok()) {
    return core::Result<core::RouteSelectionCurrentSnapshotDTO>::Err(
        active_before.error());
  }
  if (active_before.value() != expected_active_publication_) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "active_publication_changed",
        "active publication differs from the construction anchor");
  }

  const auto &bundle = retained_bundle_.value();
  const auto *retained_desired =
      unique_by(bundle.desired_control_configurations,
                desired_configuration.control_domain_id,
                [](const auto &value) { return value.control_domain_id; });
  const auto *domain =
      unique_by(bundle.cell_graph.control_domains,
                desired_configuration.control_domain_id,
                [](const auto &value) { return value.control_domain_id; });
  if (retained_desired == nullptr || domain == nullptr ||
      *retained_desired != desired_configuration ||
      domain->control_domain_revision !=
          desired_configuration.control_domain_revision ||
      domain->controller_topology_id !=
          desired_configuration.controller_topology_id ||
      domain->topology_revision != desired_configuration.topology_revision) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "desired_domain_not_retained",
        "desired configuration is not the exact retained domain candidate");
  }

  const auto source = std::lower_bound(
      controller_sources_.begin(), controller_sources_.end(),
      domain->control_domain_id, [](const auto &row, const auto &identity) {
        return row.control_domain_id < identity;
      });
  if (source == controller_sources_.end() ||
      source->control_domain_id != domain->control_domain_id ||
      std::find(
          domain->runtime_unit_ids.begin(), domain->runtime_unit_ids.end(),
          source->source_runtime_unit_id) == domain->runtime_unit_ids.end()) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "controller_source_not_retained",
        "domain has no exact retained controller-lifecycle source");
  }

  std::set<core::ControllableComponentId> component_ids;
  std::set<core::ResourceId> resource_ids;
  std::vector<core::ConfigurationFenceBindingScopeDTO> binding_scopes;
  binding_scopes.reserve(desired_configuration.binding_routes.size());
  for (const auto &desired_route : desired_configuration.binding_routes) {
    const auto *binding = unique_by(
        bundle.cell_graph.control_bindings, desired_route.control_binding_id,
        [](const auto &value) { return value.control_binding_id; });
    if (binding == nullptr ||
        binding->control_binding_revision !=
            desired_route.control_binding_revision ||
        binding->control_domain_id != domain->control_domain_id ||
        std::find(binding->legal_control_route_ids.begin(),
                  binding->legal_control_route_ids.end(),
                  desired_route.control_route_id) ==
            binding->legal_control_route_ids.end()) {
      return fail<core::RouteSelectionCurrentSnapshotDTO>(
          "retained_binding_scope_incomplete",
          "desired route has no exact retained binding closure");
    }
    component_ids.insert(binding->controllable_component_id);
    for (const auto &resource : binding->resource_bindings) {
      resource_ids.insert(resource.resource_id);
    }
    binding_scopes.push_back({binding->control_binding_id,
                              binding->control_binding_revision,
                              binding->control_binding_revision});
  }
  if (component_ids.empty() || resource_ids.empty()) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "domain_scope_empty",
        "retained domain resolves no complete component/resource scope");
  }

  const std::vector<core::ControllableComponentId> components(
      component_ids.begin(), component_ids.end());
  const std::vector<core::ControlDomainId> domains{domain->control_domain_id};
  const auto facts =
      current_fact_reader_.LoadCurrentSnapshotFacts(components, domains);
  if (!facts.ok()) {
    return core::Result<core::RouteSelectionCurrentSnapshotDTO>::Err(
        facts.error());
  }
  if (!facts.value().locally_valid() ||
      facts.value().components.size() != components.size() ||
      facts.value().domains.size() != 1U ||
      facts.value().domains.front().control_domain_id !=
          domain->control_domain_id) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "current_fact_readback_mismatch",
        "atomic current-fact readback differs from the complete domain query");
  }

  std::vector<core::RouteSelectionComponentExpectationDTO>
      component_expectations;
  component_expectations.reserve(components.size());
  for (std::size_t index = 0U; index < components.size(); ++index) {
    const auto &fact = facts.value().components[index];
    if (fact.controllable_component_id != components[index]) {
      return fail<core::RouteSelectionCurrentSnapshotDTO>(
          "current_component_order_mismatch",
          "current lifecycle facts are not the requested canonical set");
    }
    // Observation needs no lifecycle expectation; only route selection
    // requires exact numbered lifecycle truth for every component.
    if (!observation_only &&
        fact.durable_generation.has_value() != fact.current_state.has_value()) {
      return fail<core::RouteSelectionCurrentSnapshotDTO>(
          "current_lifecycle_fact_unresolved",
          "numbered lifecycle truth has no exact current state");
    }
    component_expectations.push_back(
        {fact.controllable_component_id,
         fact.durable_generation.value_or(
             core::ComponentLifecycleGeneration(1U)),
         fact.current_state.value_or(
             core::ComponentLifecycleStateDTO::kDeclared)});
  }

  auto active_configuration =
      facts.value().domains.front().current_active_control_configuration;
  if (kernel_current_configuration_source_) {
    const auto kernel_current =
        kernel_current_configuration_source_(domain->control_domain_id);
    if (!kernel_current.ok()) {
      return core::Result<core::RouteSelectionCurrentSnapshotDTO>::Err(
          kernel_current.error());
    }
    if (active_configuration.has_value() &&
        kernel_current.value() != active_configuration) {
      return fail<core::RouteSelectionCurrentSnapshotDTO>(
          "current_configuration_owner_mismatch",
          "CCLC durable and Kernel current route facts disagree");
    }
    if (!active_configuration.has_value()) {
      active_configuration = kernel_current.value();
    }
  }
  if (active_configuration.has_value() &&
      (active_configuration->source_runtime_unit_id !=
           source->source_runtime_unit_id ||
       (active_configuration->source_runtime_epoch !=
            source->source_runtime_epoch &&
        active_configuration->state !=
            core::ActiveControlConfigurationStateDTO::kReconciled))) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "current_controller_source_mismatch",
        "durable active configuration belongs to another controller source");
  }

  std::optional<core::ControllerLifecycleExpectedDomain> expected_domain;
  try {
    const auto loaded = load_verified_controller_lifecycle_expected_domain(
        retained_bundle_, artifact_source_, desired_configuration,
        source->source_runtime_unit_id, source->source_runtime_epoch);
    expected_domain = loaded.sealed();
  } catch (const RetainedBundleError &error) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "controller_expected_domain_invalid", error.what());
  } catch (const std::exception &error) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "controller_expected_domain_load_failed", error.what());
  }

  const auto active_after = read_active();
  if (!active_after.ok()) {
    return core::Result<core::RouteSelectionCurrentSnapshotDTO>::Err(
        active_after.error());
  }
  if (active_after.value() != active_before.value()) {
    return fail<core::RouteSelectionCurrentSnapshotDTO>(
        "active_publication_changed_during_read",
        "active publication changed while joining current route facts");
  }

  core::ConfigurationFenceScopeDTO scope{
      components,
      {{domain->control_domain_id, domain->control_domain_revision,
        domain->control_domain_revision}},
      std::move(binding_scopes),
      std::vector<core::ResourceId>(resource_ids.begin(), resource_ids.end())};
  core::RouteSelectionCurrentSnapshotDTO snapshot{
      expected_retained_bundle_id,       expected_graph_revision,
      std::move(component_expectations), active_configuration,
      expected_domain->value(),          std::move(scope)};
  return core::Result<core::RouteSelectionCurrentSnapshotDTO>::Ok(
      std::move(snapshot));
}

core::Result<core::RouteSelectionCurrentSnapshot>
VerifiedRouteSelectionCurrentSnapshotSource::ReadCurrentRouteSelectionSnapshot(
    const core::RetainedBundleId &expected_retained_bundle_id,
    const core::GraphRevision &expected_graph_revision,
    const core::DesiredControlConfigurationDTO &desired_configuration) const {
  auto closure =
      ReadCurrentRouteClosure(expected_retained_bundle_id,
                              expected_graph_revision, desired_configuration,
                              false);
  if (!closure.ok()) {
    return core::Result<core::RouteSelectionCurrentSnapshot>::Err(
        closure.error());
  }
  const auto verified =
      core::verify_route_selection_current_snapshot(std::move(closure.value()));
  if (!verified.ok()) {
    return fail<core::RouteSelectionCurrentSnapshot>(
        "route_snapshot_verification_failed", verified.error().message);
  }
  return verified;
}

core::Result<core::RouteConfigurationObservationSnapshot>
VerifiedRouteSelectionCurrentSnapshotSource::
    ReadCurrentRouteConfigurationObservation(
        const core::RetainedBundleId &expected_retained_bundle_id,
        const core::GraphRevision &expected_graph_revision,
        const core::DesiredControlConfigurationDTO &desired_configuration)
        const {
  auto closure =
      ReadCurrentRouteClosure(expected_retained_bundle_id,
                              expected_graph_revision, desired_configuration,
                              true);
  if (!closure.ok()) {
    return core::Result<core::RouteConfigurationObservationSnapshot>::Err(
        closure.error());
  }
  // Only identity, controller source, agreed current configuration and the
  // expected domain carry over; component lifecycle expectations do not.
  auto &value = closure.value();
  auto verified = core::verify_route_configuration_observation_snapshot(
      {std::move(value.retained_bundle_id), std::move(value.graph_revision),
       std::move(value.active_control_configuration),
       std::move(value.expected_controller_domain)});
  if (!verified.ok()) {
    return fail<core::RouteConfigurationObservationSnapshot>(
        "route_observation_verification_failed", verified.error().message);
  }
  return verified;
}


core::Result<core::CCLCRouteSelectionTemplateSetDTO>
VerifiedRouteSelectionCurrentSnapshotSource::ReadCurrentRouteSelectionTemplates(
    const core::ControllableComponentId &component_id) const {
  const auto &bundle = retained_bundle_.value();
  const auto component = std::find_if(
      bundle.cell_graph.controllable_components.begin(),
      bundle.cell_graph.controllable_components.end(), [&](const auto &row) {
        return row.controllable_component_id == component_id;
      });
  if (!bounded_identity(component_id) ||
      component == bundle.cell_graph.controllable_components.end() ||
      !verify_retained_bundle_identity(retained_bundle_)) {
    return fail<core::CCLCRouteSelectionTemplateSetDTO>(
        "component_not_retained",
        "route template request does not name one retained component");
  }

  std::vector<core::CCLCRouteSelectionTemplateDTO> templates;
  templates.reserve(bundle.desired_control_configurations.size());
  for (const auto &desired : bundle.desired_control_configurations) {
    bool component_contributes = false;
    for (const auto &desired_route : desired.binding_routes) {
      const auto *binding = unique_by(
          bundle.cell_graph.control_bindings, desired_route.control_binding_id,
          [](const auto &row) { return row.control_binding_id; });
      if (binding != nullptr &&
          binding->controllable_component_id == component_id) {
        component_contributes = true;
        break;
      }
    }
    if (!component_contributes) {
      continue;
    }

    const auto current = ReadCurrentRouteSelectionSnapshot(
        retained_bundle_.retained_bundle_id(), bundle.cell_graph.graph_revision,
        desired);
    if (!current.ok()) {
      return core::Result<core::CCLCRouteSelectionTemplateSetDTO>::Err(
          current.error());
    }
    core::ExpectedActiveControlConfigurationDTO expected_active{
        core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
        std::nullopt, std::nullopt};
    if (current.value().value().active_control_configuration.has_value()) {
      const auto &active =
          *current.value().value().active_control_configuration;
      expected_active = {
          core::ExpectedActiveControlConfigurationPresenceDTO::kPresent,
          active.active_control_configuration_id,
          active.active_control_configuration_generation};
    }
    templates.push_back({desired, std::move(expected_active),
                         current.value().value().component_expectations});
  }
  std::sort(templates.begin(), templates.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.desired_configuration.control_domain_id <
                     rhs.desired_configuration.control_domain_id;
            });
  core::CCLCRouteSelectionTemplateSetDTO result{
      component_id, retained_bundle_.retained_bundle_id(),
      bundle.cell_graph.graph_revision, std::move(templates)};
  if (!result.locally_valid()) {
    return fail<core::CCLCRouteSelectionTemplateSetDTO>(
        "route_template_projection_invalid",
        "retained/current route template projection is invalid");
  }
  return core::Result<core::CCLCRouteSelectionTemplateSetDTO>::Ok(
      std::move(result));
}

} // namespace manyforge::deployment
