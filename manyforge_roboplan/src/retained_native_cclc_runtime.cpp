#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/filesystem_resolved_stop_base_plan_repository.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/retained_native_controller_lifecycle_port.hpp>

namespace manyforge::roboplan_backend {
namespace {

class EmptyCellRoutePublicationOwner final
    : public deployment::IRouteConfigurationKernelPublicationOwner {
public:
  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &) const override {
    return core::Result<std::vector<core::RuntimeParticipantBinding>>::Err(
        {"empty_cell_physical_route_unavailable",
         "the verified empty cell has no physical control domain",
         {{"effect", "not_started"}}});
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &,
      std::vector<deployment::VerifiedRouteReadinessParticipant>) override {
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        {"empty_cell_physical_route_unavailable",
         "the verified empty cell has no physical control domain",
         {{"effect", "not_started"}}});
  }
};

template <typename Value>
[[nodiscard]] core::Result<Value> reject(std::string reason,
                                         std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

template <typename Range, typename Identity, typename Projection>
[[nodiscard]] const typename Range::value_type *
unique_by(const Range &values, const Identity &identity,
          Projection projection) noexcept {
  const typename Range::value_type *result = nullptr;
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

[[nodiscard]] core::ObservedStateSourceKind
publication_source(const core::StateAuthorityClass authority) {
  switch (authority) {
  case core::StateAuthorityClass::kMeasured:
    return core::ObservedStateSourceKind::kJointState;
  case core::StateAuthorityClass::kCommanded:
    return core::ObservedStateSourceKind::kCommandedState;
  case core::StateAuthorityClass::kPredicted:
    return core::ObservedStateSourceKind::kPredictedState;
  }
  throw std::invalid_argument("unknown retained state-authority class");
}

[[nodiscard]] bool
exact_graph_binding(const core::RuntimeParticipantBindingDTO &physical,
                    const core::ControlBindingDTO &binding,
                    const core::DesiredControlConfigurationDTO &desired,
                    const core::DesiredControlBindingRouteDTO &route) noexcept {
  if (!physical.control_path.has_value()) {
    return false;
  }
  const auto &path = *physical.control_path;
  return physical.controllable_component_id ==
             binding.controllable_component_id &&
         physical.command_target_id == binding.command_target_id &&
         physical.capability_id == binding.capability_id &&
         physical.capability_definition_revision ==
             binding.capability_definition_revision &&
         path.control_domain_id == desired.control_domain_id &&
         path.control_domain_revision == desired.control_domain_revision &&
         path.control_binding_id == binding.control_binding_id &&
         path.control_binding_revision == binding.control_binding_revision &&
         path.control_interface_contract_id ==
             binding.control_interface_contract_id &&
         path.control_interface_revision ==
             binding.control_interface_revision &&
         path.controller_topology_id == desired.controller_topology_id &&
         path.topology_revision == desired.topology_revision &&
         path.control_route_id == route.control_route_id &&
         path.activation_set_id == desired.activation_set_id &&
         path.provider_instance_id == binding.provider_instance_id &&
         path.provider_manifest_id == binding.provider_manifest_id &&
         path.provider_manifest_revision ==
             binding.provider_manifest_revision &&
         path.provider_runtime_unit_id == binding.provider_runtime_unit_id &&
         path.state_authority_id == binding.state_authority_id &&
         path.state_authority_class == binding.state_authority_class;
}

[[nodiscard]] bool active_configuration_matches_desired(
    const core::ActiveControlConfigurationDTO &active,
    const core::DesiredControlConfigurationDTO &desired) noexcept {
  if (active.control_domain_id != desired.control_domain_id ||
      active.control_domain_revision != desired.control_domain_revision ||
      active.controller_topology_id != desired.controller_topology_id ||
      active.topology_revision != desired.topology_revision ||
      active.activation_set_id != desired.activation_set_id ||
      active.binding_routes.size() != desired.binding_routes.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < active.binding_routes.size(); ++index) {
    const auto &current = active.binding_routes[index];
    const auto &candidate = desired.binding_routes[index];
    if (current.control_binding_id != candidate.control_binding_id ||
        current.control_binding_revision !=
            candidate.control_binding_revision ||
        current.control_route_id != candidate.control_route_id) {
      return false;
    }
  }
  return true;
}

struct RuntimeEpochRouteReconciliation final {
  core::ActiveControlConfigurationDTO predecessor;
  core::ControllableComponentId controllable_component_id;
  core::RuntimeEpoch source_runtime_epoch;
  std::optional<core::RouteSelectionTransactionId> transaction_id;
  bool settled{};
};

struct RuntimeEpochRouteReconciliationState final {
  std::mutex mutex;
  std::vector<RuntimeEpochRouteReconciliation> rows;
  /// Component readiness was rebuilt from committed facts after the last
  /// recovered route, while the Kernel's route-recovery exclusion was held.
  bool readiness_reconstructed{};
};

[[nodiscard]] core::Result<std::vector<RuntimeEpochRouteReconciliation>>
adopt_durable_route_configurations(
    const deployment::CCLCRuntimeCompositionConfig &config,
    const deployment::RetainedBundle &retained_bundle,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
    deployment::IRouteConfigurationKernelPublicationOwner &route_owner,
    core::ResourceAddressedExecutionKernel &kernel,
    const std::function<core::Result<core::RuntimeRouteConfigurationReadbackDTO>(
        const core::ActiveControlConfigurationDTO &)> &adopt_predecessor) {
  try {
    const auto &bundle = retained_bundle.value();
    std::set<core::ControllableComponentId> component_ids;
    std::set<core::ControlDomainId> domain_ids;
    for (const auto &binding : physical_bindings) {
      if (!binding.value().control_path.has_value()) {
        continue;
      }
      component_ids.insert(binding.value().controllable_component_id);
      domain_ids.insert(binding.value().control_path->control_domain_id);
    }
    if (component_ids.empty() || domain_ids.empty()) {
      return reject<std::vector<RuntimeEpochRouteReconciliation>>(
          "native_cclc_restart_scope_empty",
          "native CCLC restart adoption has no physical route scope");
    }

    core::SqliteCCLCJournal journal(
        config.state_root / std::string(core::kCCLCJournalFilename),
        config.cell_id, config.exact_result_history_capacity,
        config.terminal_result_bytes_capacity, config.on_incidents_pruned,
        config.preparation_failure_source);
    const auto facts = journal.LoadCurrentSnapshotFacts(
        std::vector<core::ControllableComponentId>(component_ids.begin(),
                                                   component_ids.end()),
        std::vector<core::ControlDomainId>(domain_ids.begin(),
                                           domain_ids.end()));
    if (!facts.ok()) {
      return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::Err(
          facts.error());
    }

    std::vector<RuntimeEpochRouteReconciliation> reconciliations;
    core::FilesystemResolvedStopBasePlanRepository repository(
        config.run_root, config.maximum_resolved_stop_plan_count,
        config.maximum_resolved_stop_plan_bytes,
        config.maximum_single_resolved_stop_plan_bytes);
    for (const auto &domain : facts.value().domains) {
      if (!domain.current_active_control_configuration.has_value()) {
        continue;
      }
      const auto &active = *domain.current_active_control_configuration;
      const auto desired = std::find_if(
          bundle.desired_control_configurations.begin(),
          bundle.desired_control_configurations.end(),
          [&](const auto &candidate) {
            return active_configuration_matches_desired(active, candidate);
          });
      if (desired == bundle.desired_control_configurations.end()) {
        return reject<std::vector<RuntimeEpochRouteReconciliation>>(
            "native_cclc_restart_configuration_not_retained",
            "durable current route is absent from the active retained bundle");
      }

      if (active.state == core::ActiveControlConfigurationStateDTO::
                              kExternalOrUnknownUnreconciled) {
        auto published = route_owner.PublishAndReadBackRouteConfiguration(active, {});
        if (!published.ok()) {
          return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::
              Err(published.error());
        }
        continue;
      }
      if (active.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled) {
        return reject<std::vector<RuntimeEpochRouteReconciliation>>(
            "native_cclc_restart_configuration_invalid",
            "durable current route has an unknown state");
      }

      auto physical = route_owner.ReadCurrentPhysicalParticipants(active);
      if (!physical.ok()) {
        return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::Err(
            physical.error());
      }
      std::optional<core::RuntimeUnitId> current_source_unit;
      std::optional<core::RuntimeEpoch> current_source_epoch;
      std::optional<core::ControllableComponentId> current_component_id;
      for (const auto &participant : physical.value()) {
        if (!participant.value().control_path.has_value()) {
          return reject<std::vector<RuntimeEpochRouteReconciliation>>(
              "native_cclc_restart_source_missing",
              "durable current route has a participant without a control "
              "source");
        }
        const auto &path = *participant.value().control_path;
        if (!current_source_unit.has_value()) {
          current_source_unit = path.provider_runtime_unit_id;
          current_source_epoch = path.provider_runtime_epoch;
          current_component_id = participant.value().controllable_component_id;
        } else if (*current_source_unit != path.provider_runtime_unit_id ||
                   *current_source_epoch != path.provider_runtime_epoch) {
          return reject<std::vector<RuntimeEpochRouteReconciliation>>(
              "native_cclc_restart_source_ambiguous",
              "durable current route spans multiple current source owners");
        }
      }
      if (!current_source_unit.has_value() ||
          !current_source_epoch.has_value() ||
          !current_component_id.has_value() ||
          *current_source_unit != active.source_runtime_unit_id) {
        return reject<std::vector<RuntimeEpochRouteReconciliation>>(
            "native_cclc_restart_source_mismatch",
            "durable current route belongs to another runtime unit");
      }
      const auto fault = kernel.get_stop_fault_latch();
      if (*current_source_epoch != active.source_runtime_epoch ||
          (fault && fault->state != core::StopFaultLatchStateDTO::kCleared)) {
        auto adopted = adopt_predecessor(active);
        if (!adopted.ok()) {
          return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::
              Err(adopted.error());
        }
        reconciliations.push_back({
            .predecessor = active,
            .controllable_component_id = *current_component_id,
            .source_runtime_epoch = *current_source_epoch,
            .transaction_id = std::nullopt,
            .settled = false,
        });
        continue;
      }

      {
        // A native Apply has just installed exactly this durable route in
        // this Kernel. Route readiness is already current; component command
        // eligibility is reconstructed separately after every route.
        const auto installed =
            kernel.current_runtime_route_configuration(active.control_domain_id);
        if (!installed.ok()) {
          return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::Err(
              installed.error());
        }
        if (installed.value().has_value() &&
            installed.value()->active_configuration == active &&
            installed.value()->operation_admission_open) {
          continue;
        }
      }
      const auto expected =
          deployment::load_verified_controller_lifecycle_expected_domain(
              retained_bundle, artifact_source, *desired,
              active.source_runtime_unit_id, active.source_runtime_epoch);
      std::set<core::ControllableComponentId> contributing_components;
      for (const auto &route : desired->binding_routes) {
        const auto *binding = unique_by(
            bundle.cell_graph.control_bindings, route.control_binding_id,
            [](const auto &row) { return row.control_binding_id; });
        if (binding == nullptr || binding->control_binding_revision !=
                                      route.control_binding_revision) {
          return reject<std::vector<RuntimeEpochRouteReconciliation>>(
              "native_cclc_restart_binding_not_retained",
              "durable current route has no exact retained binding");
        }
        contributing_components.insert(binding->controllable_component_id);
      }
      std::vector<core::RouteSelectionComponentExpectationDTO>
          component_expectations;
      component_expectations.reserve(contributing_components.size());
      for (const auto &component_id : contributing_components) {
        const auto fact = std::find_if(
            facts.value().components.begin(), facts.value().components.end(),
            [&](const auto &candidate) {
              return candidate.controllable_component_id == component_id;
            });
        if (fact == facts.value().components.end() ||
            !fact->durable_generation.has_value() ||
            !fact->current_state.has_value()) {
          return reject<std::vector<RuntimeEpochRouteReconciliation>>(
              "native_cclc_restart_lifecycle_fact_missing",
              "durable current route has no complete lifecycle generation");
        }
        component_expectations.push_back(
            {component_id, *fact->durable_generation, *fact->current_state});
      }

      auto participants =
          deployment::resolve_store_and_verify_route_readiness_batch(
              retained_bundle, artifact_source, expected, active,
              component_expectations, physical.value(), repository);
      if (!participants.ok()) {
        return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::Err(
            participants.error());
      }
      auto published = route_owner.PublishAndReadBackRouteConfiguration(
          active, std::move(participants.value()));
      if (!published.ok()) {
        return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::Err(
            published.error());
      }
      if (!published.value().operation_admission_open) {
        return reject<std::vector<RuntimeEpochRouteReconciliation>>(
            "native_cclc_restart_route_not_ready",
            "the restored route did not read back complete route readiness");
      }
    }
    return core::Result<std::vector<RuntimeEpochRouteReconciliation>>::Ok(
        std::move(reconciliations));
  } catch (const std::exception &error) {
    return reject<std::vector<RuntimeEpochRouteReconciliation>>(
        "native_cclc_restart_adoption_failed", error.what());
  } catch (...) {
    return reject<std::vector<RuntimeEpochRouteReconciliation>>(
        "native_cclc_restart_adoption_failed",
        "durable current routes could not be adopted by the new Kernel");
  }
}

/// Rebuild component command eligibility only from matching committed facts,
/// under the Kernel's route-recovery exclusion. A component stays
/// command-closed when its fact is absent (including after a settled partial
/// effect, which erases it) or not Active, when its latest lifecycle
/// transaction is not terminal, when a native Apply plan is unadopted, or
/// when one of its routes is not yet current. Nothing here dispatches a
/// primitive or implicitly Enables anything.
[[nodiscard]] core::Result<void> reconstruct_component_command_readiness(
    const deployment::CCLCRuntimeCompositionConfig &config,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
    core::ICCLCComponentCommandReadinessPort &readiness_owner) {
  try {
    std::set<core::ControllableComponentId> component_ids;
    std::set<core::ControlDomainId> domain_ids;
    for (const auto &binding : physical_bindings) {
      if (!binding.value().control_path.has_value()) continue;
      component_ids.insert(binding.value().controllable_component_id);
      domain_ids.insert(binding.value().control_path->control_domain_id);
    }
    if (component_ids.empty() || domain_ids.empty()) {
      return core::Result<void>::Ok();
    }
    core::SqliteCCLCJournal journal(
        config.state_root / std::string(core::kCCLCJournalFilename),
        config.cell_id, config.exact_result_history_capacity,
        config.terminal_result_bytes_capacity);
    const auto facts = journal.LoadCurrentSnapshotFacts(
        std::vector<core::ControllableComponentId>(component_ids.begin(),
                                                   component_ids.end()),
        std::vector<core::ControlDomainId>(domain_ids.begin(),
                                           domain_ids.end()));
    if (!facts.ok()) return core::Result<void>::Err(facts.error());
    const auto unsettled = journal.LoadComponentsWithUnsettledLifecycle();
    if (!unsettled.ok()) return core::Result<void>::Err(unsettled.error());
    const auto pending_apply = journal.ReadUnsettledNativeApplyPlan();
    if (!pending_apply.ok()) {
      return core::Result<void>::Err(pending_apply.error());
    }
    journal.Close();
    for (const auto &fact : facts.value().components) {
      if (!fact.durable_generation.has_value()) continue;
      const bool eligible =
          !pending_apply.value().has_value() &&
          fact.current_state == core::ComponentLifecycleStateDTO::kActive &&
          std::find(unsettled.value().begin(), unsettled.value().end(),
                    fact.controllable_component_id) == unsettled.value().end();
      core::ComponentCommandReadinessPublicationDTO publication{
          core::ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt,
          std::nullopt, fact.controllable_component_id, *fact.durable_generation,
          eligible};
      auto published =
          readiness_owner.PublishComponentCommandReadiness(publication);
      const auto reason =
          published.ok() ? std::string{}
                         : (published.error().details.contains("reason")
                                ? published.error().details.at("reason")
                                : std::string{});
      if (!published.ok() && eligible &&
          reason == "component_readiness_route_not_current") {
        // A route still awaits owner reconciliation; stay closed until the
        // route-recovery path rebuilds readiness from current facts.
        publication.command_eligible = false;
        published =
            readiness_owner.PublishComponentCommandReadiness(publication);
      }
      if (!published.ok()) return core::Result<void>::Err(published.error());
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &error) {
    return reject<void>("native_cclc_readiness_reconstruction_failed", error.what());
  }
}

[[nodiscard]] core::Result<void> reconcile_runtime_epoch_routes(
    deployment::CCLCRuntimeComposition &composition,
    core::IKernelCellAdmissionPort &cell_admission,
    std::vector<RuntimeEpochRouteReconciliation> &reconciliations) {
  auto &port = composition.open_request_port();
  const auto owner_epoch = port.ReadRequestOwnerEpoch();
  for (std::size_t index = 0U; index < reconciliations.size(); ++index) {
    auto &required = reconciliations[index];
    if (required.settled)
      continue;
    if (!required.transaction_id) {
      if (!composition.coordinator().accepting_new_requests()) {
        return reject<void>("native_cclc_restart_recovery_pending",
                            "retained CCLC work is still reconciling");
      }
      auto templates =
          port.ReadRouteSelectionTemplates(required.controllable_component_id);
      if (!templates.ok()) {
        return core::Result<void>::Err(templates.error());
      }
      const auto selected = std::find_if(
          templates.value().templates.begin(),
          templates.value().templates.end(), [&](const auto &candidate) {
            return active_configuration_matches_desired(
                required.predecessor, candidate.desired_configuration);
          });
      if (selected == templates.value().templates.end()) {
        return reject<void>(
            "native_cclc_restart_route_template_missing",
            "current retained route has no owner-reconciliation template");
      }
      core::RouteSelectionIntentDTO intent{
          {core::RequestId("native-owner-reconcile." +
                           std::to_string(index + 1U)),
           owner_epoch, "native_owner_restart", std::nullopt},
          templates.value().retained_bundle_id,
          templates.value().graph_revision,
          selected->desired_configuration,
          selected->expected_active_control_configuration,
          selected->component_expectations,
          {core::RequestTransportDTO::kInternal,
           core::RequestAssuranceDTO::kUnverified, std::nullopt},
          cell_admission.GetAuthoringAdmission().admission_generation};
      auto submitted = composition.coordinator().SubmitRouteRecovery(
          intent, required.predecessor);
      if (!submitted.ok()) {
        return core::Result<void>::Err(submitted.error());
      }
      if (!submitted.value().acceptance.has_value()) {
        return reject<void>(
            "native_cclc_restart_route_acceptance_missing",
            "runtime-owner reconciliation produced no route acceptance");
      }
      required.transaction_id =
          submitted.value().acceptance->route_selection_transaction_id;
    }
    auto status = port.LoadRouteSelectionStatus(*required.transaction_id);
    if (!status.ok()) {
      return core::Result<void>::Err(status.error());
    }
    if (status.value() && !status.value()->terminal_result) {
      if (status.value()->error)
        return core::Result<void>::Err(*status.value()->error);
      return reject<void>(
          "native_cclc_restart_recovery_pending",
          "controller route observation or settlement is pending");
    }
    if (!status.value().has_value() ||
        status.value()->terminal_result !=
            core::RouteSelectionTerminalResultDTO::kSucceeded ||
        !status.value()->resulting_active_control_configuration.has_value() ||
        status.value()
                ->resulting_active_control_configuration
                ->source_runtime_epoch != required.source_runtime_epoch) {
      return reject<void>(
          "native_cclc_restart_route_reconciliation_failed",
          "runtime-owner route reconciliation did not publish current truth");
    }
    required.settled = true;
  }
  return core::Result<void>::Ok();
}

[[nodiscard]] bool provider_matches_component(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    const std::set<core::ControllableComponentId> &components,
    const core::CellGraphDTO &graph) noexcept {
  return std::any_of(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &binding) {
        return components.contains(binding.controllable_component_id) &&
               binding.provider_instance_id == provider.provider_instance_id &&
               binding.provider_manifest_id == provider.provider_manifest_id &&
               binding.provider_manifest_revision ==
                   provider.provider_manifest_revision &&
               binding.provider_runtime_unit_id ==
                   provider.provider_runtime_unit_id;
      });
}

struct DerivedRuntimeClosure final {
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>
      lifecycle_provider_occurrences;
  std::vector<deployment::ControllerLifecycleSourceOccurrenceDTO>
      controller_sources;
  std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports;
  std::vector<core::ControllerLifecyclePortRegistrationDTO> controller_ports;
  std::vector<core::KinematicResourceSourceProjectionDTO> source_projections;
};

[[nodiscard]] core::Result<DerivedRuntimeClosure> derive_runtime_closure(
    const RetainedNativeCCLCRuntimeConfig &config,
    const deployment::RetainedBundle &bundle,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
    core::IClock &clock,
    const RetainedExternallyManagedDomainSnapshotSource
        &externally_managed_controller_snapshot_source) {
  const auto &value = bundle.value();
  const auto &graph = value.cell_graph;
  if (!config.locally_valid() ||
      !deployment::verify_retained_bundle_identity(bundle) ||
      physical_bindings.empty() ||
      physical_bindings.size() > core::kMaxRouteReadinessPublicationRows) {
    return reject<DerivedRuntimeClosure>(
        "native_cclc_input_invalid",
        "native CCLC derivation requires one bounded retained runtime");
  }

  std::map<core::ControlBindingId, const core::RuntimeParticipantBinding *>
      physical_by_binding;
  for (const auto &physical : physical_bindings) {
    const auto &participant = physical.value();
    if (participant.retained_bundle_id != bundle.retained_bundle_id() ||
        participant.graph_revision != graph.graph_revision ||
        !participant.control_path.has_value() ||
        !physical_by_binding
             .emplace(participant.control_path->control_binding_id, &physical)
             .second) {
      return reject<DerivedRuntimeClosure>(
          "native_cclc_physical_binding_invalid",
          "physical participant bindings are stale, duplicated, or not "
          "control addressed");
    }
  }

  DerivedRuntimeClosure result;
  result.controller_sources.reserve(
      value.desired_control_configurations.size());
  result.controller_ports.reserve(value.desired_control_configurations.size());
  std::set<core::ControlBindingId> consumed_bindings;
  for (const auto &desired : value.desired_control_configurations) {
    const auto *domain = unique_by(
        graph.control_domains, desired.control_domain_id,
        [](const auto &candidate) { return candidate.control_domain_id; });
    if (domain == nullptr ||
        domain->control_domain_revision != desired.control_domain_revision ||
        domain->controller_topology_id != desired.controller_topology_id ||
        domain->topology_revision != desired.topology_revision) {
      return reject<DerivedRuntimeClosure>(
          "native_cclc_desired_domain_invalid",
          "desired controller configuration has no exact retained domain");
    }

    std::vector<core::RuntimeParticipantBinding> domain_bindings;
    domain_bindings.reserve(desired.binding_routes.size());
    std::optional<core::RuntimeUnitId> source_runtime_unit;
    std::optional<core::RuntimeEpoch> source_runtime_epoch;
    for (const auto &route : desired.binding_routes) {
      const auto *binding = unique_by(
          graph.control_bindings, route.control_binding_id,
          [](const auto &candidate) { return candidate.control_binding_id; });
      const auto physical = physical_by_binding.find(route.control_binding_id);
      if (binding == nullptr || physical == physical_by_binding.end() ||
          binding->control_binding_revision != route.control_binding_revision ||
          binding->control_domain_id != desired.control_domain_id ||
          !exact_graph_binding(physical->second->value(), *binding, desired,
                               route) ||
          !consumed_bindings.insert(route.control_binding_id).second) {
        return reject<DerivedRuntimeClosure>(
            "native_cclc_binding_coverage_invalid",
            "desired controller configuration has no sole exact physical "
            "binding representative");
      }
      const auto &path = *physical->second->value().control_path;
      if (!source_runtime_unit.has_value()) {
        source_runtime_unit = path.provider_runtime_unit_id;
        source_runtime_epoch = path.provider_runtime_epoch;
      } else if (*source_runtime_unit != path.provider_runtime_unit_id ||
                 *source_runtime_epoch != path.provider_runtime_epoch) {
        return reject<DerivedRuntimeClosure>(
            "native_cclc_controller_source_ambiguous",
            "one native controller domain spans multiple source runtime "
            "incarnations");
      }
      domain_bindings.push_back(*physical->second);
    }
    if (!source_runtime_unit.has_value() || !source_runtime_epoch.has_value()) {
      return reject<DerivedRuntimeClosure>(
          "native_cclc_controller_source_missing",
          "native controller domain has no exact runtime source");
    }

    try {
      auto expected =
          deployment::load_verified_controller_lifecycle_expected_domain(
              bundle, artifact_source, desired, *source_runtime_unit,
              *source_runtime_epoch);
      core::Result<std::shared_ptr<core::IControllerLifecyclePort>> port =
          core::Result<std::shared_ptr<core::IControllerLifecyclePort>>::Err(
              {"controller_lifecycle_port_unselected",
               "no controller lifecycle product was selected",
               {}});
      if (externally_managed_controller_snapshot_source) {
        auto external =
            RetainedExternallyManagedControllerLifecyclePort::Create(
                expected.sealed(), std::move(domain_bindings),
                externally_managed_controller_snapshot_source, clock,
                config.controller_observation_freshness_ns);
        if (external.ok()) {
          port =
              core::Result<std::shared_ptr<core::IControllerLifecyclePort>>::Ok(
                  std::move(external.value()));
        } else {
          port = core::Result<std::shared_ptr<core::IControllerLifecyclePort>>::
              Err(external.error());
        }
      } else {
        auto native = RetainedNativeControllerLifecyclePort::Create(
            expected.sealed(), std::move(domain_bindings), clock,
            config.controller_observation_freshness_ns);
        if (native.ok()) {
          port =
              core::Result<std::shared_ptr<core::IControllerLifecyclePort>>::Ok(
                  std::move(native.value()));
        } else {
          port = core::Result<std::shared_ptr<core::IControllerLifecyclePort>>::
              Err(native.error());
        }
      }
      if (!port.ok()) {
        return core::Result<DerivedRuntimeClosure>::Err(port.error());
      }
      result.controller_sources.push_back({desired.control_domain_id,
                                           *source_runtime_unit,
                                           *source_runtime_epoch});
      result.controller_ports.push_back(
          {core::MakeControllerLifecycleObservationQuery(expected.value()),
           std::move(port.value())});
    } catch (const std::exception &exception) {
      return reject<DerivedRuntimeClosure>(
          "native_cclc_controller_product_invalid", exception.what());
    }
  }
  if (consumed_bindings.size() != physical_by_binding.size()) {
    return reject<DerivedRuntimeClosure>(
        "native_cclc_physical_binding_extra",
        "physical runtime contains a binding outside the retained desired "
        "controller configurations");
  }

  std::sort(brand_ports.begin(), brand_ports.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.provider_occurrence.control_domain_id <
                     rhs.provider_occurrence.control_domain_id;
            });
  if (brand_ports.size() != value.desired_control_configurations.size()) {
    return reject<DerivedRuntimeClosure>(
        "native_cclc_lifecycle_coverage_incomplete",
        "native CCLC requires one exact lifecycle registration per domain");
  }
  result.lifecycle_provider_occurrences.reserve(brand_ports.size());
  for (std::size_t index = 0U; index < brand_ports.size(); ++index) {
    const auto &registration = brand_ports[index];
    const auto &occurrence = registration.provider_occurrence;
    const auto *desired = unique_by(
        value.desired_control_configurations, occurrence.control_domain_id,
        [](const auto &candidate) { return candidate.control_domain_id; });
    const auto *domain = unique_by(
        graph.control_domains, occurrence.control_domain_id,
        [](const auto &candidate) { return candidate.control_domain_id; });
    std::set<core::ControllableComponentId> components;
    if (domain != nullptr) {
      for (const auto &binding : graph.control_bindings) {
        if (binding.control_domain_id == domain->control_domain_id) {
          components.insert(binding.controllable_component_id);
        }
      }
    }
    const auto controller_source = std::find_if(
        result.controller_sources.begin(), result.controller_sources.end(),
        [&](const auto &source) {
          return source.control_domain_id == occurrence.control_domain_id;
        });
    if (registration.port == nullptr || !occurrence.locally_valid() ||
        domain == nullptr || desired == nullptr ||
        occurrence.control_domain_revision != domain->control_domain_revision ||
        !std::binary_search(domain->runtime_unit_ids.begin(),
                            domain->runtime_unit_ids.end(),
                            occurrence.provider_runtime_unit_id) ||
        !provider_matches_component(occurrence, components, graph) ||
        controller_source == result.controller_sources.end() ||
        controller_source->source_runtime_unit_id !=
            occurrence.provider_runtime_unit_id ||
        controller_source->source_runtime_epoch !=
            occurrence.provider_runtime_epoch) {
      return reject<DerivedRuntimeClosure>(
          "native_cclc_lifecycle_registration_invalid",
          "lifecycle registration does not match its exact retained "
          "component/domain/runtime occurrence");
    }
    const auto catalog = catalog_source.LoadExactLifecyclePlanCatalog(
        occurrence.provider_manifest_id, occurrence.provider_manifest_revision,
        occurrence.lifecycle_plan_catalog_digest);
    if (!catalog.ok()) {
      return core::Result<DerivedRuntimeClosure>::Err(catalog.error());
    }
    const auto verified =
        core::VerifyBrandLifecyclePlanCatalog(catalog.value());
    if (!verified.ok() ||
        catalog.value().provider_manifest_id !=
            occurrence.provider_manifest_id ||
        catalog.value().provider_manifest_revision !=
            occurrence.provider_manifest_revision ||
        catalog.value().catalog_digest !=
            occurrence.lifecycle_plan_catalog_digest) {
      return reject<DerivedRuntimeClosure>(
          "native_cclc_lifecycle_catalog_mismatch",
          "lifecycle registration has no exact verified build-owned catalog");
    }
    result.lifecycle_provider_occurrences.push_back(occurrence);
  }
  result.brand_ports = std::move(brand_ports);

  auto projections =
      DeriveRetainedNativeStateSourceProjections(physical_bindings);
  if (!projections.ok()) {
    return core::Result<DerivedRuntimeClosure>::Err(projections.error());
  }
  result.source_projections = std::move(projections.value());
  return core::Result<DerivedRuntimeClosure>::Ok(std::move(result));
}

} // namespace

core::Result<
    std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>>
MakeVerifiedEmptyCellRoutePublicationOwner(
    const deployment::RetainedBundle &retained_bundle) {
  const auto &graph = retained_bundle.value().cell_graph;
  if (!deployment::verify_retained_bundle_identity(retained_bundle) ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !retained_bundle.value().desired_control_configurations.empty()) {
    return reject<
        std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>>(
        "empty_cell_route_product_invalid",
        "empty-cell route dependency requires a verified empty physical graph");
  }
  return core::Result<
      std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>>::Ok(
      std::make_unique<EmptyCellRoutePublicationOwner>());
}

core::Result<std::vector<core::KinematicResourceSourceProjectionDTO>>
DeriveRetainedNativeStateSourceProjections(
    const std::span<const core::RuntimeParticipantBinding> physical_bindings) {
  std::map<core::ResourceId, core::KinematicResourceSourceProjectionDTO>
      projections;
  for (const auto &physical : physical_bindings) {
    const auto &participant = physical.value();
    if (!participant.control_path.has_value()) {
      return reject<
          std::vector<core::KinematicResourceSourceProjectionDTO>>(
          "native_state_source_path_missing",
          "native state-source projection has no retained control path");
    }
    const auto &path = *participant.control_path;
    for (const auto &resource : participant.resources) {
      const core::KinematicControlBindingSourceDTO source{
          path.control_binding_id, path.control_binding_revision};
      auto [found, inserted] = projections.try_emplace(
          resource.resource_id,
          core::KinematicResourceSourceProjectionDTO{
              resource.resource_id,
              {},
              path.provider_instance_id,
              path.provider_manifest_id,
              path.provider_manifest_revision,
              path.state_authority_id,
              path.state_authority_class,
              publication_source(path.state_authority_class)});
      auto &projection = found->second;
      if ((!inserted &&
           (projection.provider_instance_id != path.provider_instance_id ||
            projection.provider_manifest_id != path.provider_manifest_id ||
            projection.provider_manifest_revision !=
                path.provider_manifest_revision ||
            projection.state_authority_id != path.state_authority_id ||
            projection.state_authority_class != path.state_authority_class ||
            projection.publication_source !=
                publication_source(path.state_authority_class))) ||
          std::find(projection.contributing_control_bindings.begin(),
                    projection.contributing_control_bindings.end(), source) !=
              projection.contributing_control_bindings.end()) {
        return reject<
            std::vector<core::KinematicResourceSourceProjectionDTO>>(
            "native_state_source_projection_ambiguous",
            "one native resource has conflicting or duplicate state-source "
            "authority");
      }
      projection.contributing_control_bindings.push_back(source);
    }
  }
  std::vector<core::KinematicResourceSourceProjectionDTO> result;
  result.reserve(projections.size());
  for (auto &[resource_id, projection] : projections) {
    (void)resource_id;
    std::sort(projection.contributing_control_bindings.begin(),
              projection.contributing_control_bindings.end(),
              [](const auto &lhs, const auto &rhs) {
                return std::tie(lhs.control_binding_id,
                                lhs.control_binding_revision) <
                       std::tie(rhs.control_binding_id,
                                rhs.control_binding_revision);
              });
    result.push_back(std::move(projection));
  }
  return core::Result<
      std::vector<core::KinematicResourceSourceProjectionDTO>>::Ok(
      std::move(result));
}

core::Result<core::NativeApplyPlanRequestDTO>
DeriveRetainedNativeApplyPlanRequest(
    const core::NamedApplyPublicationTransactionDTO &apply,
    const deployment::RetainedBundle &candidate,
    const deployment::RetainedBundle *predecessor,
    const deployment::ApplyComponentDeltaDTO &delta,
    const std::span<const core::RuntimeParticipantBinding> candidate_physical_bindings,
    const std::span<const core::RuntimeRouteConfigurationReadbackDTO> retained_route_readbacks,
    const core::ComponentLifecycleStateDTO changed_component_state) {
  using Plan = core::NativeApplyPlanRequestDTO;
  try {
    if (!apply.valid() || !deployment::verify_retained_bundle_identity(candidate) ||
        !delta.valid() ||
        (changed_component_state != core::ComponentLifecycleStateDTO::kActive &&
         changed_component_state != core::ComponentLifecycleStateDTO::kDeclared) ||
        candidate_physical_bindings.size() > core::kMaxRouteReadinessPublicationRows) {
      return reject<Plan>("native_apply_plan_input_invalid",
                          "native Apply planning requires exact retained inputs");
    }
    const auto &graph = candidate.value().cell_graph;
    if (graph.controllable_components.size() > core::kMaximumCCLCComponentWatermarks ||
        candidate.value().desired_control_configurations.size() >
            core::kMaximumCCLCActiveConfigurationWatermarks) {
      return reject<Plan>("native_apply_plan_input_invalid",
                          "native Apply scope exceeds the existing CCLC plan limits");
    }
    if (predecessor != nullptr &&
        (!deployment::verify_retained_bundle_identity(*predecessor) ||
         predecessor->value().cell_graph.cell_id != graph.cell_id ||
         deployment::derive_apply_component_delta(*predecessor, candidate) != delta)) {
      return reject<Plan>("native_apply_plan_delta_mismatch",
                          "native Apply delta differs from the verified bundles");
    }
    Plan plan{apply, candidate.retained_bundle_id(), graph.graph_revision,
              {}, {}, {}, {}, {}, {}, changed_component_state};
    std::map<core::ControllableComponentId, std::vector<core::ControlDomainId>> candidate_domains;
    for (const auto &component : graph.controllable_components) {
      plan.components.push_back(component.controllable_component_id);
      candidate_domains.emplace(component.controllable_component_id,
                                 std::vector<core::ControlDomainId>{});
    }
    std::sort(plan.components.begin(), plan.components.end());
    for (const auto &binding : graph.control_bindings)
      candidate_domains.at(binding.controllable_component_id).push_back(binding.control_domain_id);
    for (auto &[_, domains] : candidate_domains) {
      std::sort(domains.begin(), domains.end());
      domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
    }
    std::size_t candidate_rows = 0U;
    for (const auto &row : delta.components) {
      const auto domains = candidate_domains.find(row.controllable_component_id);
      const auto identity = deployment::derive_component_runtime_identity(
          candidate, row.controllable_component_id);
      if (identity != row.candidate_runtime_identity ||
          (domains == candidate_domains.end()
               ? !row.candidate_domains.empty()
               : domains->second != row.candidate_domains) ||
          (predecessor == nullptr && row.disposition != deployment::ApplyComponentDisposition::kAdd)) {
        return reject<Plan>("native_apply_plan_component_mismatch",
                            "native Apply component membership differs from the retained candidate");
      }
      if (identity) ++candidate_rows;
      if (row.disposition == deployment::ApplyComponentDisposition::kRetain)
        plan.retained_components.push_back(row.controllable_component_id);
      else if (row.disposition == deployment::ApplyComponentDisposition::kRemove)
        plan.removed_components.push_back(row.controllable_component_id);
    }
    if (candidate_rows != plan.components.size())
      return reject<Plan>("native_apply_plan_component_mismatch",
                          "native Apply delta does not cover every candidate component");

    std::map<core::ControlBindingId, const core::RuntimeParticipantBinding *> physical_by_binding;
    for (const auto &physical : candidate_physical_bindings) {
      const auto &value = physical.value();
      if (value.retained_bundle_id != candidate.retained_bundle_id() ||
          value.graph_revision != graph.graph_revision || !value.control_path ||
          !physical_by_binding.emplace(value.control_path->control_binding_id, &physical).second)
        return reject<Plan>("native_apply_plan_binding_mismatch",
                            "native Apply physical bindings are stale, duplicated or unaddressed");
    }
    std::set<core::ControlBindingId> consumed;
    std::set<core::ControlDomainId> retained_domains;
    for (const auto &desired : candidate.value().desired_control_configurations) {
      std::optional<core::ActiveControlConfigurationGeneration> floor;
      for (const auto &route : desired.binding_routes) {
        const auto *binding = unique_by(graph.control_bindings, route.control_binding_id,
            [](const auto &row) { return row.control_binding_id; });
        const auto physical = physical_by_binding.find(route.control_binding_id);
        if (binding == nullptr || physical == physical_by_binding.end() ||
            binding->control_binding_revision != route.control_binding_revision ||
            binding->control_domain_id != desired.control_domain_id ||
            !exact_graph_binding(physical->second->value(), *binding, desired, route) ||
            !consumed.insert(route.control_binding_id).second)
          return reject<Plan>("native_apply_plan_binding_mismatch",
                              "native Apply desired route has no exact compiled representative");
        const auto generation = physical->second->value().control_path->active_control_configuration_generation;
        if (!generation.nonzero() ||
            generation.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            (floor && *floor != generation))
          return reject<Plan>("native_apply_plan_generation_mismatch",
                              "native Apply domain has conflicting compiled generation floors");
        floor = generation;
        if (std::binary_search(plan.retained_components.begin(), plan.retained_components.end(),
                               binding->controllable_component_id))
          retained_domains.insert(desired.control_domain_id);
      }
      if (!floor)
        return reject<Plan>("native_apply_plan_binding_mismatch",
                            "native Apply desired domain has no compiled representative");
      plan.compiled_routes.push_back({desired.control_domain_id, *floor});
      if (!retained_domains.contains(desired.control_domain_id)) continue;
      const auto *readback = unique_by(retained_route_readbacks, desired.control_domain_id,
          [](const auto &row) { return row.active_configuration.control_domain_id; });
      if (readback == nullptr || !readback->operation_admission_open ||
          !readback->active_configuration.locally_valid() ||
          readback->active_configuration.state != core::ActiveControlConfigurationStateDTO::kReconciled ||
          !active_configuration_matches_desired(readback->active_configuration, desired))
        return reject<Plan>("native_apply_plan_retained_route_mismatch",
                            "native Apply retained domain has no exact current open Kernel route");
      plan.retained_routes.push_back(readback->active_configuration);
    }
    if (consumed.size() != physical_by_binding.size() ||
        retained_route_readbacks.size() != retained_domains.size())
      return reject<Plan>("native_apply_plan_route_coverage_mismatch",
                          "native Apply bindings or retained readbacks widen the planned domains");
    if (predecessor != nullptr) {
      for (const auto &desired : predecessor->value().desired_control_configurations)
        if (std::none_of(candidate.value().desired_control_configurations.begin(),
                         candidate.value().desired_control_configurations.end(),
                         [&](const auto &row) { return row.control_domain_id == desired.control_domain_id; }))
          plan.removed_domains.push_back(desired.control_domain_id);
    }
    if (plan.removed_components.size() > core::kMaximumCCLCComponentWatermarks ||
        plan.removed_domains.size() > core::kMaximumCCLCActiveConfigurationWatermarks)
      return reject<Plan>("native_apply_plan_input_invalid",
                          "native Apply removals exceed the existing CCLC plan limits");
    return core::Result<Plan>::Ok(std::move(plan));
  } catch (const std::exception &error) {
    return reject<Plan>("native_apply_plan_derivation_failed", error.what());
  }
}

bool RetainedNativeCCLCRuntimeConfig::locally_valid() const noexcept {
  return composition.locally_valid() && controller_observation_freshness_ns > 0;
}

core::Result<void> InitializeRetainedNativeCandidateControlRoutes(
    const deployment::RetainedBundle &bundle,
    core::ResourceAddressedExecutionKernel &kernel,
    deployment::IRouteConfigurationKernelPublicationOwner &route_owner,
    core::ICCLCComponentCommandReadinessPort &readiness_owner,
    const core::ActiveBundlePublicationGeneration candidate_publication_generation,
    const core::NativeApplyPlanDTO &plan,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const std::span<const core::RuntimeParticipantBinding> physical_bindings,
    const std::filesystem::path &execution_run_root,
    const core::ConfigurationFenceId &parent_apply_fence_id) {
  try {
    if (!deployment::verify_retained_bundle_identity(bundle) ||
        !candidate_publication_generation.nonzero() || !plan.apply.valid() ||
        plan.candidate_retained_bundle_id != bundle.retained_bundle_id() ||
        plan.candidate_graph_revision != bundle.value().cell_graph.graph_revision ||
        plan.routes.size() != bundle.value().desired_control_configurations.size()) {
      return reject<void>("native_candidate_input_invalid",
                          "native CCLC candidate route inputs do not match the recorded plan");
    }
    core::FilesystemResolvedStopBasePlanRepository plan_repository(
        execution_run_root);
    for (const auto &desired : bundle.value().desired_control_configurations) {
      std::vector<core::RuntimeParticipantBinding> representatives;
      representatives.reserve(desired.binding_routes.size());
      const core::RuntimeControlPathDTO *source_path = nullptr;
      for (const auto &route : desired.binding_routes) {
        const auto representative = std::find_if(
            physical_bindings.begin(),
            physical_bindings.end(),
            [&](const auto &candidate) {
              return candidate.value().control_path.has_value() &&
                     candidate.value().control_path->control_domain_id ==
                         desired.control_domain_id &&
                     candidate.value().control_path->control_binding_id ==
                         route.control_binding_id &&
                     candidate.value().control_path->control_binding_revision ==
                         route.control_binding_revision;
            });
        if (representative == physical_bindings.end()) {
          return reject<void>("native_candidate_binding_missing",
                              "candidate route has no exact physical binding");
        }
        const auto &path = *representative->value().control_path;
        if (source_path == nullptr) {
          source_path = &path;
        } else if (source_path->provider_runtime_unit_id !=
                       path.provider_runtime_unit_id ||
                   source_path->provider_runtime_epoch !=
                       path.provider_runtime_epoch ||
                   source_path->active_control_configuration_id !=
                       path.active_control_configuration_id ||
                   source_path->active_control_configuration_generation !=
                       path.active_control_configuration_generation) {
          return reject<void>("native_candidate_source_ambiguous",
                              "candidate domain spans different runtime sources");
        }
        representatives.push_back(*representative);
      }
      if (source_path == nullptr) {
        return reject<void>("native_candidate_domain_empty",
                            "candidate control domain has no binding routes");
      }
      const auto planned_route = std::find_if(
          plan.routes.begin(), plan.routes.end(), [&](const auto &row) {
            return row.control_domain_id == desired.control_domain_id;
          });
      const auto domain_component =
          representatives.front().value().controllable_component_id;
      const auto planned_component = std::find_if(
          plan.components.begin(), plan.components.end(), [&](const auto &row) {
            return row.component_id == domain_component;
          });
      if (planned_route == plan.routes.end() ||
          planned_component == plan.components.end() ||
          std::any_of(representatives.begin(), representatives.end(),
                      [&](const auto &row) {
                        return row.value().controllable_component_id !=
                               domain_component;
                      })) {
        return reject<void>("native_candidate_plan_scope_mismatch",
                            "candidate domain has no single planned component and route");
      }
      std::vector<core::ActiveControlConfigurationBindingRouteDTO> active_routes;
      active_routes.reserve(desired.binding_routes.size());
      for (const auto &route : desired.binding_routes) {
        active_routes.push_back({route.control_binding_id,
                                 route.control_binding_revision,
                                 route.control_route_id});
      }
      std::sort(active_routes.begin(), active_routes.end(),
                core::CanonicalActiveControlConfigurationBindingRouteLess);
      core::ActiveControlConfigurationDTO configuration{
          source_path->active_control_configuration_id,
          desired.control_domain_id,
          desired.control_domain_revision,
          std::move(active_routes),
          desired.activation_set_id,
          desired.controller_topology_id,
          desired.topology_revision,
          source_path->provider_runtime_unit_id,
          source_path->provider_runtime_epoch,
          candidate_publication_generation.value(),
          {},
          planned_route->generation,
          core::ActiveControlConfigurationStateDTO::kReconciled};
      auto projections =
          DeriveRetainedNativeStateSourceProjections(representatives);
      if (!projections.ok()) {
        return core::Result<void>::Err(projections.error());
      }
      configuration.kinematic_resource_source_projections =
          std::move(projections.value());
      if (!configuration.locally_valid()) {
        return reject<void>("native_candidate_configuration_invalid",
                            "candidate route configuration is not valid");
      }
      const auto current = kernel.current_runtime_route_configuration(
          desired.control_domain_id);
      if (!current.ok()) {
        return core::Result<void>::Err(current.error());
      }
      if (std::binary_search(plan.retained_domains.begin(),
                             plan.retained_domains.end(),
                             desired.control_domain_id)) {
        // A retained route keeps its predecessor source sequence and
        // generation. The candidate publication is associated by the
        // product-bound Kernel registration, not by publishing a new route.
        if (!current.value().has_value() ||
            !current.value()->operation_admission_open) {
          return reject<void>("native_retained_route_not_current",
                              "retained native route has no current Kernel readback");
        }
        const auto &held = current.value()->active_configuration;
        if (held.control_domain_id != desired.control_domain_id ||
            held.control_domain_revision != desired.control_domain_revision ||
            held.binding_routes != configuration.binding_routes ||
            held.activation_set_id != desired.activation_set_id ||
            held.controller_topology_id != desired.controller_topology_id ||
            held.topology_revision != desired.topology_revision ||
            held.source_runtime_unit_id !=
                source_path->provider_runtime_unit_id ||
            held.source_runtime_epoch != source_path->provider_runtime_epoch ||
            held.kinematic_resource_source_projections !=
                configuration.kinematic_resource_source_projections ||
            held.active_control_configuration_id !=
                source_path->active_control_configuration_id ||
            held.active_control_configuration_generation !=
                planned_route->generation ||
            held.state !=
                core::ActiveControlConfigurationStateDTO::kReconciled) {
          return reject<void>("native_retained_route_mismatch",
                              "retained native route differs from its recorded Apply plan");
        }
        continue;
      }
      if (current.value().has_value() &&
          current.value()->active_configuration == configuration &&
          current.value()->operation_admission_open) {
        continue;
      }
      const auto expected =
          deployment::load_verified_controller_lifecycle_expected_domain(
              bundle, artifact_source, desired,
              source_path->provider_runtime_unit_id,
              source_path->provider_runtime_epoch);
      // The same generation and state CCLC recorded and will adopt.
      std::vector<core::RouteSelectionComponentExpectationDTO> components{
          {domain_component, planned_component->generation,
           planned_component->state}};
      auto participants =
          deployment::resolve_store_and_verify_route_readiness_batch(
              bundle, artifact_source, expected, configuration, components,
              representatives, plan_repository);
      if (!participants.ok()) {
        return core::Result<void>::Err(participants.error());
      }
      const auto published = route_owner.PublishAndReadBackRouteConfiguration(
          configuration, std::move(participants.value()));
      if (!published.ok()) {
        return core::Result<void>::Err(published.error());
      }
      if (!published.value().operation_admission_open) {
        return reject<void>("native_candidate_route_not_ready",
                            "candidate route readback has no complete route readiness");
      }
    }
    // Publish each planned component's complete result once all of its
    // routes are current. Only a component without a retained explicit
    // Disable is enabled; the plan's generation is the one CCLC will adopt.
    for (const auto &component : plan.components) {
      if (std::binary_search(plan.retained_components.begin(),
                             plan.retained_components.end(),
                             component.component_id)) {
        const auto current = readiness_owner.ReadComponentCommandReadiness(
            component.component_id);
        if (!current.ok()) {
          return core::Result<void>::Err(current.error());
        }
        if (current.value().component_lifecycle_generation !=
                component.generation ||
            current.value().command_eligible !=
                (component.state ==
                 core::ComponentLifecycleStateDTO::kActive)) {
          return reject<void>("native_retained_component_readiness_mismatch",
                              "retained component command eligibility changed during Apply");
        }
        continue;
      }
      const core::ComponentCommandReadinessPublicationDTO publication{
          core::ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
          parent_apply_fence_id, component.component_id, component.generation,
          component.state == core::ComponentLifecycleStateDTO::kActive};
      const auto published = readiness_owner.PublishComponentCommandReadiness(
          publication);
      if (!published.ok()) {
        return core::Result<void>::Err(published.error());
      }
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &error) {
    return reject<void>("native_candidate_initialization_failed", error.what());
  }
}

core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>
CreateRetainedNativeCCLCRuntime(
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    deployment::IRouteConfigurationKernelPublicationOwner
        &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler,
    std::function<
        core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
            const core::ControlDomainId &)>
        kernel_current_configuration_source,
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source,
    core::ICCLCComponentCommandReadinessPort *component_command_readiness_port) {
  if (!config.composition.trusted_roots_valid()) {
    return reject<std::unique_ptr<deployment::CCLCRuntimeComposition>>(
        "native_cclc_storage_capabilities_invalid",
        "native CCLC requires disjoint, pre-existing durable state and "
        "disposable run roots");
  }
  if (retained_bundle == nullptr) {
    return reject<std::unique_ptr<deployment::CCLCRuntimeComposition>>(
        "native_cclc_retained_bundle_missing",
        "native CCLC composition has no retained bundle");
  }
  auto closure = derive_runtime_closure(
      config, *retained_bundle, artifact_source, lifecycle_catalog_source,
      std::move(brand_ports), physical_bindings, clock,
      externally_managed_controller_snapshot_source);
  if (!closure.ok()) {
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(closure.error());
  }
  auto &value = closure.value();
  auto composition = deployment::CCLCRuntimeComposition::Create(
      std::move(config.composition), std::move(retained_bundle),
      active_publication_reader, std::move(expected_active_publication),
      artifact_source, lifecycle_catalog_source,
      std::move(value.lifecycle_provider_occurrences),
      std::move(value.controller_sources), std::move(value.brand_ports),
      std::move(value.controller_ports), std::move(value.source_projections),
      kernel_cell_admission_port,
      kernel_route_fence_port, kernel_publication_owner, clock, scheduler,
      std::move(kernel_current_configuration_source),
      component_command_readiness_port);
  return composition;
}

core::Result<std::shared_ptr<
    const deployment::CCLCRuntimeComposition::PreparedProductContext>>
PrepareRetainedNativeCCLCProductContext(
    deployment::CCLCRuntimeComposition &composition,
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    core::IClock &clock,
    deployment::IRouteConfigurationKernelPublicationOwner
        &kernel_publication_owner,
    std::shared_ptr<const void> installed_product_lifetime,
    std::function<
        core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
            const core::ControlDomainId &)>
        kernel_current_configuration_source,
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source,
    core::ICCLCComponentCommandReadinessPort *component_command_readiness_port) {
  using PreparedContext =
      deployment::CCLCRuntimeComposition::PreparedProductContext;
  if (retained_bundle == nullptr) {
    return reject<std::shared_ptr<const PreparedContext>>(
        "native_cclc_retained_bundle_missing",
        "native CCLC composition has no retained bundle");
  }
  auto closure = derive_runtime_closure(
      config, *retained_bundle, artifact_source, lifecycle_catalog_source,
      std::move(brand_ports), physical_bindings, clock,
      externally_managed_controller_snapshot_source);
  if (!closure.ok()) {
    return core::Result<std::shared_ptr<const PreparedContext>>::Err(
        closure.error());
  }
  auto &value = closure.value();
  return composition.PrepareProductContext({
      std::move(retained_bundle), active_publication_reader,
      std::move(expected_active_publication), artifact_source,
      lifecycle_catalog_source, std::move(value.lifecycle_provider_occurrences),
      std::move(value.controller_sources), std::move(value.brand_ports),
      std::move(value.controller_ports), std::move(value.source_projections),
      std::move(kernel_current_configuration_source),
      component_command_readiness_port, kernel_publication_owner,
      std::move(installed_product_lifetime)});
}

core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>
CreateRecoveredRetainedNativeCellCCLCRuntime(
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    deployment::IRouteConfigurationKernelPublicationOwner &route_owner,
    core::ICCLCComponentCommandReadinessPort &readiness_owner,
    core::ResourceAddressedExecutionKernel &kernel,
    core::IClock &clock, core::IScheduler &scheduler,
    std::function<core::Result<core::RuntimeRouteConfigurationReadbackDTO>(
        const core::ActiveControlConfigurationDTO &)> adopt_predecessor,
    std::function<core::Result<void>()> reconcile_command_free_orphans,
    std::function<core::Result<void>()> &reconcile_after_start) {
  reconcile_after_start = {};
  if (retained_bundle == nullptr || !adopt_predecessor ||
      !reconcile_command_free_orphans) {
    return reject<std::unique_ptr<deployment::CCLCRuntimeComposition>>(
        "native_cell_restart_owner_missing",
        "native cell route recovery requires exact cell and predecessor owners");
  }
  // Recovery may publish predecessor routes. Reject malformed products and
  // physical scope before the first publication rather than discovering them
  // only when CCLC is constructed below.
  if (!config.locally_valid() ||
      !config.composition.trusted_roots_valid() ||
      !deployment::verify_retained_bundle_identity(*retained_bundle) ||
      physical_bindings.empty() ||
      physical_bindings.size() > core::kMaxRouteReadinessPublicationRows) {
    return reject<std::unique_ptr<deployment::CCLCRuntimeComposition>>(
        "native_cell_restart_input_invalid",
        "native cell restart requires one verified bounded retained product");
  }
  std::set<core::ControlBindingId> binding_ids;
  for (const auto &binding : physical_bindings) {
    if (binding.value().retained_bundle_id !=
            retained_bundle->retained_bundle_id() ||
        binding.value().graph_revision !=
            retained_bundle->value().cell_graph.graph_revision ||
        !binding.value().control_path.has_value() ||
        !binding_ids.insert(
            binding.value().control_path->control_binding_id).second) {
      return reject<std::unique_ptr<deployment::CCLCRuntimeComposition>>(
          "native_cell_restart_binding_invalid",
          "native cell restart physical bindings are stale or ambiguous");
    }
  }
  auto adopted = adopt_durable_route_configurations(
      config.composition, *retained_bundle, artifact_source,
      physical_bindings, route_owner, kernel, adopt_predecessor);
  if (!adopted.ok())
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(adopted.error());
  auto orphans = reconcile_command_free_orphans();
  if (!orphans.ok())
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(orphans.error());
  auto manual = kernel.reconcile_manual_operation_admission();
  if (!manual.ok())
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(manual.error());
  const auto readiness_config = config.composition;
  const auto readiness_bindings = physical_bindings;
  auto reconstructed = reconstruct_component_command_readiness(
      readiness_config, readiness_bindings, readiness_owner);
  if (!reconstructed.ok())
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(reconstructed.error());
  auto current_configuration = [&kernel](const core::ControlDomainId &domain)
      -> core::Result<std::optional<core::ActiveControlConfigurationDTO>> {
    const auto current = kernel.current_runtime_route_configuration(domain);
    if (!current.ok())
      return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::
          Err(current.error());
    if (!current.value().has_value())
      return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::
          Ok(std::nullopt);
    return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
        current.value()->active_configuration);
  };
  auto composition = CreateRetainedNativeCCLCRuntime(
      std::move(config), std::move(retained_bundle), active_publication_reader,
      std::move(expected_active_publication), artifact_source,
      lifecycle_catalog_source, std::move(brand_ports),
      std::move(physical_bindings),
      kernel_cell_admission_port, kernel_route_fence_port, route_owner, clock,
      scheduler, std::move(current_configuration), {}, &readiness_owner);
  if (!composition.ok())
    return composition;
  auto started = composition.value()->Start();
  if (!started.ok())
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(started.error());
  auto state = std::make_shared<RuntimeEpochRouteReconciliationState>();
  state->rows = std::move(adopted.value());
  state->readiness_reconstructed = state->rows.empty();
  if (state->rows.empty()) {
    auto ready = kernel.SetRouteRecoveryPending(false);
    if (!ready.ok())
      return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
          Err(ready.error());
  }
  reconcile_after_start = [owner = composition.value().get(),
                           &kernel_cell_admission_port, &kernel,
                           &readiness_owner, state, readiness_config,
                           readiness_bindings]() mutable {
    std::lock_guard lock(state->mutex);
    auto result = reconcile_runtime_epoch_routes(
        *owner, kernel_cell_admission_port, state->rows);
    if (!result.ok()) return result;
    if (!state->readiness_reconstructed) {
      result = reconstruct_component_command_readiness(
          readiness_config, readiness_bindings, readiness_owner);
      if (!result.ok()) return result;
      state->readiness_reconstructed = true;
    }
    return kernel.SetRouteRecoveryPending(false);
  };
  return composition;
}

core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>
CreateRetainedNativeCCLCRuntime(
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    RetainedNativeRuntime &runtime,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source) {
  if (!config.composition.trusted_roots_valid()) {
    return reject<std::unique_ptr<deployment::CCLCRuntimeComposition>>(
        "native_cclc_storage_capabilities_invalid",
        "native CCLC requires disjoint, pre-existing durable state and "
        "disposable run roots");
  }
  if (retained_bundle == nullptr || runtime.clock == nullptr ||
      runtime.scheduler == nullptr || runtime.active_addressed_kernel() == nullptr) {
    return reject<std::unique_ptr<deployment::CCLCRuntimeComposition>>(
        "native_cclc_runtime_incomplete",
        "native CCLC requires the stable addressed owner, clock, and "
        "scheduler");
  }
  auto adopted = adopt_durable_route_configurations(
      config.composition, *retained_bundle, artifact_source,
      physical_bindings, runtime, *runtime.active_addressed_kernel(),
      [&runtime](const core::ActiveControlConfigurationDTO &predecessor) {
        return runtime.AdoptAndReadBackRouteConfigurationPredecessor(
            predecessor);
      });
  if (!adopted.ok()) {
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(adopted.error());
  }
  // Route adoption/publication establishes complete command-free physical
  // evidence for the cell. Reconcile predecessor-owner markers once, after
  // every domain is current, rather than during partial domain replacement.
  auto orphans_reconciled = runtime.ReconcileCommandFreeOrphans();
  if (!orphans_reconciled.ok()) {
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(orphans_reconciled.error());
  }
  // Retire the predecessor's manual claim before requesting route authority;
  // the regular host reconciliation lane starts only after this composition.
  auto manual_reconciled =
      runtime.active_addressed_kernel()->reconcile_manual_operation_admission();
  if (!manual_reconciled.ok()) {
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(manual_reconciled.error());
  }
  const auto readiness_config = config.composition;
  const auto readiness_bindings = physical_bindings;
  auto reconstructed =
      reconstruct_component_command_readiness(readiness_config,
                                               readiness_bindings, runtime);
  if (!reconstructed.ok()) {
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(reconstructed.error());
  }
  auto observation_bundle = retained_bundle;
  std::function<
      core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
          const core::ControlDomainId &)>
      current_configuration;
  current_configuration = [&runtime](const core::ControlDomainId &domain_id) {
    return runtime.ReadCurrentPublishedRouteConfiguration(domain_id);
  };
  auto composition = CreateRetainedNativeCCLCRuntime(
      std::move(config), std::move(retained_bundle), active_publication_reader,
      std::move(expected_active_publication), artifact_source,
      lifecycle_catalog_source, std::move(brand_ports),
      std::move(physical_bindings),
      kernel_cell_admission_port, kernel_route_fence_port, runtime,
      *runtime.clock, *runtime.scheduler, std::move(current_configuration),
      std::move(externally_managed_controller_snapshot_source), &runtime);
  if (!composition.ok()) {
    return composition;
  }
  auto started = composition.value()->Start();
  if (!started.ok()) {
    return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::
        Err(started.error());
  }
  auto reconciliation_state =
      std::make_shared<RuntimeEpochRouteReconciliationState>();
  reconciliation_state->rows = std::move(adopted.value());
  // With nothing to recover, readiness was already rebuilt above.
  reconciliation_state->readiness_reconstructed =
      reconciliation_state->rows.empty();
  if (reconciliation_state->rows.empty()) {
    auto ready = runtime.active_addressed_kernel()->SetRouteRecoveryPending(false);
    if (!ready.ok()) {
      return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::Err(
          ready.error());
    }
  }
  runtime.reconcile_controller_routes = [owner = composition.value().get(),
                                         &kernel_cell_admission_port, &runtime,
                                         reconciliation_state,
                                         readiness_config,
                                         readiness_bindings]() mutable {
    std::lock_guard lock(reconciliation_state->mutex);
    auto result = reconcile_runtime_epoch_routes(
        *owner, kernel_cell_admission_port, reconciliation_state->rows);
    if (!result.ok())
      return result;
    // Recovered routes are current again: rebuild eligibility once from the
    // same committed facts before recovery stops holding admission closed.
    if (!reconciliation_state->readiness_reconstructed) {
      result = reconstruct_component_command_readiness(
          readiness_config, readiness_bindings, runtime);
      if (!result.ok())
        return result;
      reconciliation_state->readiness_reconstructed = true;
    }
    return runtime.active_addressed_kernel()->SetRouteRecoveryPending(false);
  };
  runtime.stop_recovery_configuration_source =
      [owner = composition.value().get(),
       bundle = std::move(observation_bundle), &runtime, reconciliation_state](
          const core::StopFaultRecoveryConfigurationExpectationDTO &expectation)
      -> core::Result<core::ContentDigest> {
    const auto &desired = bundle->value().desired_control_configurations;
    const auto selected = std::find_if(
        desired.begin(), desired.end(), [&](const auto &candidate) {
          return candidate.control_domain_id == expectation.control_domain_id;
        });
    if (selected == desired.end()) {
      return reject<core::ContentDigest>(
          "stop_recovery_domain_not_retained",
          "Stop recovery domain is not in the retained product");
    }

    std::optional<core::ActiveControlConfigurationDTO> recovered_successor;
    {
      std::lock_guard lock(reconciliation_state->mutex);
      const auto recovered = std::find_if(
          reconciliation_state->rows.begin(), reconciliation_state->rows.end(),
          [&](const auto &row) {
            return row.settled && row.transaction_id.has_value() &&
                   row.predecessor.control_domain_id ==
                       expectation.control_domain_id &&
                   row.predecessor.active_control_configuration_id ==
                       expectation.active_control_configuration_id &&
                   row.predecessor.active_control_configuration_generation ==
                       expectation.active_control_configuration_generation;
          });
      if (recovered != reconciliation_state->rows.end()) {
        auto current = runtime.ReadCurrentPublishedRouteConfiguration(
            expectation.control_domain_id);
        if (!current.ok()) {
          return core::Result<core::ContentDigest>::Err(current.error());
        }
        if (!current.value().has_value() ||
            current.value()->state !=
                core::ActiveControlConfigurationStateDTO::kReconciled ||
            current.value()->source_runtime_unit_id !=
                recovered->predecessor.source_runtime_unit_id ||
            !active_configuration_matches_desired(*current.value(),
                                                  *selected)) {
          return reject<core::ContentDigest>(
              "stop_recovery_route_successor_mismatch",
              "Recovered controller route is not the exact retained successor");
        }
        recovered_successor = *current.value();
      }
    }
    const auto observed_configuration_id =
        recovered_successor
            ? recovered_successor->active_control_configuration_id
            : expectation.active_control_configuration_id;
    const auto observed_configuration_generation =
        recovered_successor
            ? recovered_successor->active_control_configuration_generation
            : expectation.active_control_configuration_generation;
    auto observed = owner->coordinator().ObserveRouteConfiguration(
        bundle->retained_bundle_id(), bundle->value().cell_graph.graph_revision,
        *selected,
        core::ExpectedActiveControlConfigurationDTO{
            core::ExpectedActiveControlConfigurationPresenceDTO::kPresent,
            observed_configuration_id, observed_configuration_generation});
    if (!observed.ok())
      return core::Result<core::ContentDigest>::Err(observed.error());
    if (observed.value().second.disposition !=
        core::ControllerLifecycleReconciliationDispositionDTO::kReconciled) {
      return reject<core::ContentDigest>(
          "stop_recovery_controller_unreconciled",
          "Observed controller state does not match the retained route");
    }
    using content_identity::CanonicalValue;
    const auto &observation = observed.value().first;
    return core::Result<core::ContentDigest>::Ok(core::ContentDigest(
        content_identity::sha256_digest(content_identity::canonical_json(
            CanonicalValue(CanonicalValue::Array{
                CanonicalValue(std::string(
                    "manyforge.stop-recovery-controller-observation.v2")),
                CanonicalValue(bundle->retained_bundle_id().value()),
                CanonicalValue(
                    bundle->value().cell_graph.graph_revision.value()),
                CanonicalValue(expectation.control_domain_id.value()),
                CanonicalValue(
                    expectation.active_control_configuration_id.value()),
                CanonicalValue(std::to_string(
                    expectation.active_control_configuration_generation
                        .value())),
                CanonicalValue(observed_configuration_id.value()),
                CanonicalValue(
                    std::to_string(observed_configuration_generation.value())),
                CanonicalValue(observation.query.source_runtime_epoch.value()),
                CanonicalValue(
                    observation.query.source_runtime_unit_id.value()),
                CanonicalValue(std::to_string(observation.observed_at_ns)),
                CanonicalValue(
                    std::to_string(observation.source_sequence))})))));
  };
  return composition;
}

} // namespace manyforge::roboplan_backend
