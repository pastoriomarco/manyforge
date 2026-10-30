#include <algorithm>
#include <limits>
#include <map>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<PreparedComponentLifecycleAdmission> reject(std::string code,
                                                                 std::string message,
                                                                 std::string reason) {
  return Result<PreparedComponentLifecycleAdmission>::Err(
      ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] ComponentLifecycleRequestedStateDTO requested_state_for(
    ComponentLifecycleIntentKindDTO intent) {
  switch (intent) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
      return {ComponentLifecycleRequestedStatePresenceDTO::kResolved,
              ComponentLifecycleStateDTO::kPrepared};
    case ComponentLifecycleIntentKindDTO::kEnable:
      return {ComponentLifecycleRequestedStatePresenceDTO::kResolved,
              ComponentLifecycleStateDTO::kActive};
    case ComponentLifecycleIntentKindDTO::kDisable:
      return {ComponentLifecycleRequestedStatePresenceDTO::kResolved,
              ComponentLifecycleStateDTO::kDisabled};
    case ComponentLifecycleIntentKindDTO::kRecover:
      return {ComponentLifecycleRequestedStatePresenceDTO::kUnresolved, std::nullopt};
  }
  return {ComponentLifecycleRequestedStatePresenceDTO::kUnresolved, std::nullopt};
}

[[nodiscard]] bool catalog_matches_occurrence(
    const BrandLifecyclePlanCatalogDTO& catalog,
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence) {
  return VerifyBrandLifecyclePlanCatalog(catalog).ok() &&
         catalog.provider_manifest_id == occurrence.provider_manifest_id &&
         catalog.provider_manifest_revision == occurrence.provider_manifest_revision &&
         catalog.catalog_digest == occurrence.lifecycle_plan_catalog_digest;
}

[[nodiscard]] ProviderConfigurationFenceOccurrenceDTO provider_fence_occurrence(
    const ResolvedProviderDomainScopeDTO& scope) {
  return {scope.provider.controllable_component_id, scope.provider.provider_instance_id,
          scope.provider.signal_configuration_digest, scope.output_resources};
}

void attach_provider_fence_occurrence(ConfigurationFenceDomainScopeDTO& fence,
                                      const ResolvedLifecycleControlDomainScopeDTO& scope,
                                      bool candidate) {
  const auto* provider = scope.provider();
  if (!provider) return;
  if (!std::holds_alternative<ProviderConfigurationFenceDomainDTO>(fence.payload))
    fence.payload = ProviderConfigurationFenceDomainDTO{};
  auto& payload = std::get<ProviderConfigurationFenceDomainDTO>(fence.payload);
  (candidate ? payload.candidate_provider : payload.predecessor_provider) =
      provider_fence_occurrence(*provider);
}

[[nodiscard]] std::optional<std::int64_t> resolved_plan_timeout(
    const LifecycleIntentPlanDTO& plan,
    const ResolvedLifecycleControlDomainScopeDTO& domain) noexcept {
  if (const auto* fixed = std::get_if<std::int64_t>(&plan.timeout_policy)) return *fixed;
  if (plan.timeout_policy.valueless_by_exception()) return std::nullopt;
  const auto* provider = domain.provider();
  if (!provider || !provider->provider.locally_valid()) return std::nullopt;
  return provider->provider.lifecycle_command_timeout_ns;
}

[[nodiscard]] std::optional<std::int64_t> selected_plan_timeout(
    const BrandLifecyclePlanCatalogDTO& catalog, ComponentLifecycleIntentKindDTO intent,
    const ResolvedLifecycleControlDomainScopeDTO& domain) noexcept {
  const auto match =
      std::find_if(catalog.plans.begin(), catalog.plans.end(),
                   [&](const LifecycleIntentPlanDTO& plan) { return plan.intent == intent; });
  if (match == catalog.plans.end()) {
    return std::nullopt;
  }
  return resolved_plan_timeout(*match, domain);
}

[[nodiscard]] std::optional<std::int64_t> recover_preselection_timeout(
    const BrandLifecyclePlanCatalogDTO& catalog,
    const ResolvedLifecycleControlDomainScopeDTO& domain) noexcept {
  std::optional<std::int64_t> result;
  for (const auto& plan : catalog.plans) {
    if (plan.intent != ComponentLifecycleIntentKindDTO::kRecover) {
      continue;
    }
    const auto selected = resolved_plan_timeout(plan, domain);
    if (!selected) return std::nullopt;
    result = result.has_value() ? std::min(*result, *selected) : *selected;
  }
  return result;
}

[[nodiscard]] std::optional<std::int64_t> admission_timeout(
    const ComponentLifecycleIntentDTO& intent,
    const std::vector<BrandLifecyclePlanCatalogDTO>& catalogs,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& domains) noexcept {
  if (catalogs.size() != domains.size()) return std::nullopt;
  std::optional<std::int64_t> result;
  for (std::size_t index = 0U; index < catalogs.size(); ++index) {
    const auto& catalog = catalogs[index];
    const auto selected = intent.intent == ComponentLifecycleIntentKindDTO::kRecover
                              ? recover_preselection_timeout(catalog, domains[index])
                              : selected_plan_timeout(catalog, intent.intent, domains[index]);
    if (!selected.has_value()) {
      return std::nullopt;
    }
    result = result.has_value() ? std::min(*result, *selected) : *selected;
  }
  return result;
}

[[nodiscard]] std::vector<ComponentLifecycleDomainStatusDTO> resolved_status_domains(
    const ComponentLifecycleIntentDTO& intent, const ComponentLifecycleCurrentSnapshotDTO& snapshot,
    const ComponentLifecycleRequestedStateDTO& requested_state) {
  std::vector<ComponentLifecycleDomainStatusDTO> result;
  std::size_t expected_index = 0U;
  std::size_t current_index = 0U;
  while (expected_index < intent.expected_control_domains.size() ||
         current_index < snapshot.domains.size()) {
    const bool take_expected = current_index == snapshot.domains.size() ||
                               (expected_index < intent.expected_control_domains.size() &&
                                intent.expected_control_domains[expected_index].control_domain_id <
                                    snapshot.domains[current_index].control_domain_id);
    const bool take_current =
        expected_index == intent.expected_control_domains.size() ||
        (current_index < snapshot.domains.size() &&
         snapshot.domains[current_index].control_domain_id <
             intent.expected_control_domains[expected_index].control_domain_id);

    std::optional<ExpectedControlDomainRevisionDTO> expected;
    std::optional<ResolvedLifecycleControlDomainScopeDTO> current;
    std::optional<ResolvedLifecycleProviderOccurrenceDTO> provider;
    std::optional<ControlDomainId> domain_id;
    if (take_expected) {
      expected = intent.expected_control_domains[expected_index++];
      domain_id = expected->control_domain_id;
    } else if (take_current) {
      current = snapshot.domains[current_index];
      provider = snapshot.provider_occurrences[current_index++];
      domain_id = current->control_domain_id;
    } else {
      expected = intent.expected_control_domains[expected_index++];
      current = snapshot.domains[current_index];
      provider = snapshot.provider_occurrences[current_index++];
      domain_id = current->control_domain_id;
    }

    const std::optional<ComponentLifecycleStateDTO> observed =
        current.has_value() ? std::optional(snapshot.component_lifecycle_state) : std::nullopt;
    const auto action_state = current.has_value()
                                  ? ComponentLifecycleDomainActionStateDTO::kReserved
                                  : ComponentLifecycleDomainActionStateDTO::kNotStarted;
    result.push_back({std::move(*domain_id),
                      std::move(expected),
                      std::move(current),
                      std::move(provider),
                      observed,
                      observed,
                      requested_state,
                      action_state,
                      {},
                      std::nullopt});
  }
  return result;
}

}  // namespace

Result<PreparedComponentLifecycleAdmission> PrepareComponentLifecycleAdmission(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAdmissionCandidate& candidate) {
  if (!intent.locally_valid() || intent.apply_generation_selection ||
      candidate.apply_generation_selection || candidate.lifecycle_transaction_id.empty() ||
      candidate.lifecycle_transaction_id.value().size() > kMaxLifecycleIdentityBytes ||
      !candidate.lifecycle_transaction_generation.nonzero() || candidate.accepted_at_ns < 0 ||
      !candidate.durable_component_generation_predecessor.nonzero() ||
      candidate.durable_component_generation_predecessor.value() ==
          std::numeric_limits<std::uint64_t>::max() ||
      intent.expected_component_lifecycle_generation.value() ==
          std::numeric_limits<std::uint64_t>::max()) {
    return reject("component_lifecycle_admission_invalid",
                  "the lifecycle admission candidate is invalid", "candidate_invalid");
  }

  const auto requested_state = requested_state_for(intent.intent);
  std::int64_t timeout_duration_ns = 0;
  std::vector<ComponentLifecycleDomainStatusDTO> domains;
  std::vector<ControlDomainId> reserved_domains;
  std::optional<ComponentLifecycleStateDTO> component_state;
  std::optional<std::vector<BrandLifecyclePlanCatalogDTO>> qualified_catalogs;

  if (!candidate.current_snapshot.has_value()) {
    return reject("component_lifecycle_scope_unresolved",
                  "ordinary lifecycle work requires an exact current component snapshot",
                  "current_snapshot_missing");
  } else {
    const auto& snapshot = *candidate.current_snapshot;
    if (!snapshot.locally_valid() ||
        snapshot.controllable_component_id != intent.controllable_component_id ||
        snapshot.component_lifecycle_generation !=
            candidate.durable_component_generation_predecessor) {
      return reject("component_lifecycle_current_snapshot_mismatch",
                    "the coherent current component snapshot does not match admission",
                    "snapshot_identity_or_generation_mismatch");
    }
    if (snapshot.active_publication_receipt.request.candidate_graph_revision !=
            intent.expected_graph_revision ||
        candidate.lifecycle_plan_catalogs.size() != snapshot.provider_occurrences.size() ||
        intent.expected_component_lifecycle_generation !=
            candidate.durable_component_generation_predecessor) {
      return reject("component_lifecycle_current_snapshot_mismatch",
                    "the current graph, catalogs or lifecycle generation do not match admission",
                    "snapshot_identity_or_generation_mismatch");
    }
    if (intent.expected_control_domains.size() != snapshot.domains.size()) {
      return reject("component_lifecycle_configuration_stale",
                    "the current domain set differs from the caller expectation",
                    "domain_set_changed");
    }
    for (std::size_t index = 0U; index < snapshot.domains.size(); ++index) {
      if (!ExpectedDomainMatchesResolvedScope(intent.expected_control_domains[index],
                                              snapshot.domains[index])) {
        return reject("component_lifecycle_configuration_stale",
                      "the current domain configuration differs from the caller expectation",
                      "domain_revision_or_route_changed");
      }
    }
    bool catalogs_qualified = true;
    for (std::size_t index = 0U; index < snapshot.provider_occurrences.size(); ++index) {
      catalogs_qualified = catalogs_qualified &&
                           catalog_matches_occurrence(candidate.lifecycle_plan_catalogs[index],
                                                      snapshot.provider_occurrences[index]);
    }
    if (!catalogs_qualified) {
      return reject("component_lifecycle_catalog_mismatch",
                    "one or more lifecycle catalogs do not match the accepted provider closure",
                    "catalog_identity_or_shape_mismatch");
    }
    const auto selected_timeout =
        admission_timeout(intent, candidate.lifecycle_plan_catalogs, snapshot.domains);
    if (!selected_timeout.has_value()) {
      return reject("component_lifecycle_intent_unsupported",
                    "one or more accepted provider catalogs do not support the lifecycle intent",
                    "catalog_plan_missing");
    }
    // The component's own budget, when authored, replaces the plan's bound;
    // an intent the catalogs do not support stays refused above.
    timeout_duration_ns = candidate.lifecycle_budget_ns.value_or(*selected_timeout);
    qualified_catalogs = candidate.lifecycle_plan_catalogs;
    component_state = snapshot.component_lifecycle_state;
    domains = resolved_status_domains(intent, snapshot, requested_state);
    reserved_domains.reserve(snapshot.domains.size());
    for (const auto& domain : snapshot.domains) {
      reserved_domains.push_back(domain.control_domain_id);
    }
  }

  ComponentLifecycleAcceptedDTO acceptance{
      intent.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew,
      candidate.lifecycle_transaction_id,
      candidate.lifecycle_transaction_generation,
      ComponentLifecycleGeneration(candidate.durable_component_generation_predecessor.value() + 1U),
      candidate.accepted_at_ns,
      timeout_duration_ns};
  ComponentLifecycleStatusDTO initial_status{acceptance,
                                             intent,
                                             1U,
                                             intent.request_context.request_owner_epoch,
                                             component_state,
                                             component_state,
                                             requested_state,
                                             ComponentLifecycleTransactionPhaseDTO::kAccepted,
                                             std::move(domains),
                                             false,
                                             std::nullopt,
                                             std::nullopt,
                                             candidate.accepted_at_ns,
                                             candidate.accepted_at_ns,
                                             timeout_duration_ns};
  if (!acceptance.locally_valid() || !initial_status.locally_valid() ||
      (!reserved_domains.empty() &&
       (!std::is_sorted(reserved_domains.begin(), reserved_domains.end()) ||
        std::adjacent_find(reserved_domains.begin(), reserved_domains.end()) !=
            reserved_domains.end()))) {
    return reject("component_lifecycle_admission_invariant_failed",
                  "the derived lifecycle acceptance is not a closed valid value",
                  "derived_acceptance_invalid");
  }

  return Result<PreparedComponentLifecycleAdmission>::Ok(
      {std::move(acceptance), std::move(initial_status), std::move(reserved_domains),
       std::move(qualified_catalogs)});
}

Result<ConfigurationFenceScopeDTO> SelectedComponentBringUpFenceScope(
    const SelectedComponentBringUpOfferDTO& offer) {
  const auto invalid = [](std::string reason) {
    return Result<ConfigurationFenceScopeDTO>::Err(
        ErrorDTO{"selected_component_bring_up_scope_invalid",
                 "selected bring-up does not have an exact component closure",
                 {{"reason", std::move(reason)}}});
  };
  const auto verified = VerifyComponentLifecycleCurrentSnapshot(offer.predecessor);
  if (!verified.ok()) return Result<ConfigurationFenceScopeDTO>::Err(verified.error());
  if ((offer.candidate_participants.empty() && offer.candidate_provider_domains.empty()) ||
      offer.candidate_participants.size() > kMaxLifecycleResolvedBindings ||
      offer.candidate_provider_domains.size() > kMaxLifecycleResolvedDomains) {
    return invalid("candidate_participant_closure_empty_or_oversized");
  }
  std::map<ControlDomainId, ConfigurationFenceDomainScopeDTO> domains;
  std::map<ControlBindingId, ConfigurationFenceBindingScopeDTO> bindings;
  std::vector<ResourceId> resources;
  for (const auto& domain : offer.predecessor.domains) {
    const auto entry = domains.emplace(
        domain.control_domain_id,
        ConfigurationFenceDomainScopeDTO{domain.control_domain_id, domain.control_domain_revision,
                                         std::nullopt});
    attach_provider_fence_occurrence(entry.first->second, domain, false);
    for (const auto& binding : domain.control_bindings()) {
      const auto [entry, inserted] = bindings.emplace(
          binding.control_binding_id,
          ConfigurationFenceBindingScopeDTO{binding.control_binding_id,
                                            binding.control_binding_revision, std::nullopt});
      if (!inserted &&
          entry->second.predecessor_control_binding_revision != binding.control_binding_revision) {
        return invalid("predecessor_binding_revision_conflict");
      }
    }
    resources.insert(resources.end(), domain.resource_ids().begin(), domain.resource_ids().end());
  }
  std::optional<ControlDomainId> previous_provider_domain;
  for (const auto& scope : offer.candidate_provider_domains) {
    const auto* provider = scope.provider();
    if (!scope.locally_valid() || !provider ||
        provider->provider.controllable_component_id !=
            offer.predecessor.controllable_component_id ||
        (previous_provider_domain && !(*previous_provider_domain < scope.control_domain_id)))
      return invalid("candidate_provider_domain_closure_invalid");
    previous_provider_domain = scope.control_domain_id;
    auto [entry, inserted] =
        domains.emplace(scope.control_domain_id,
                        ConfigurationFenceDomainScopeDTO{scope.control_domain_id, std::nullopt,
                                                         scope.control_domain_revision});
    if (!inserted && entry->second.candidate_control_domain_revision &&
        entry->second.candidate_control_domain_revision != scope.control_domain_revision)
      return invalid("candidate_domain_revision_conflict");
    entry->second.candidate_control_domain_revision = scope.control_domain_revision;
    attach_provider_fence_occurrence(entry->second, scope, true);
    resources.insert(resources.end(), scope.resource_ids().begin(), scope.resource_ids().end());
  }
  for (const auto& participant : offer.candidate_participants) {
    const auto checked = verify_runtime_participant_binding(participant);
    if (!checked.ok()) return Result<ConfigurationFenceScopeDTO>::Err(checked.error());
    if (participant.controllable_component_id != offer.predecessor.controllable_component_id ||
        participant.retained_bundle_id != offer.candidate_projection_revision.retained_bundle_id ||
        participant.graph_revision != offer.candidate_projection_revision.graph_revision) {
      return invalid("candidate_component_product_or_control_path_mismatch");
    }
    if (participant.io_output_binding) {
      const auto& output = *participant.io_output_binding;
      const auto scope =
          std::find_if(offer.candidate_provider_domains.begin(),
                       offer.candidate_provider_domains.end(), [&](const auto& candidate) {
                         return candidate.control_domain_id == output.provider.control_domain_id;
                       });
      if (scope == offer.candidate_provider_domains.end() ||
          !ProviderDomainMatchesOccurrence(
              scope->control_domain_id, scope->control_domain_revision, scope->provider()->provider,
              scope->provider()->provider_runtime_epoch, output.provider) ||
          output.signal_configuration_digest !=
              scope->provider()->provider.signal_configuration_digest ||
          !output.signal.output_resource ||
          !std::binary_search(scope->resource_ids().begin(), scope->resource_ids().end(),
                              *output.signal.output_resource))
        return invalid("candidate_output_provider_domain_mismatch");
      continue;
    }
    if (!participant.control_path)
      return invalid("candidate_component_product_or_control_path_mismatch");
    const auto& path = *participant.control_path;
    auto [domain, domain_inserted] =
        domains.emplace(path.control_domain_id,
                        ConfigurationFenceDomainScopeDTO{path.control_domain_id, std::nullopt,
                                                         path.control_domain_revision});
    if (!domain_inserted) {
      if (domain->second.candidate_control_domain_revision &&
          domain->second.candidate_control_domain_revision != path.control_domain_revision) {
        return invalid("candidate_domain_revision_conflict");
      }
      domain->second.candidate_control_domain_revision = path.control_domain_revision;
    }
    auto [binding, binding_inserted] =
        bindings.emplace(path.control_binding_id,
                         ConfigurationFenceBindingScopeDTO{path.control_binding_id, std::nullopt,
                                                           path.control_binding_revision});
    if (!binding_inserted) {
      if (binding->second.candidate_control_binding_revision &&
          binding->second.candidate_control_binding_revision != path.control_binding_revision) {
        return invalid("candidate_binding_revision_conflict");
      }
      binding->second.candidate_control_binding_revision = path.control_binding_revision;
    }
    for (const auto& resource : participant.resources) resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
  ConfigurationFenceScopeDTO result{
      {offer.predecessor.controllable_component_id}, {}, {}, std::move(resources)};
  for (auto& [id, domain] : domains) result.domains.push_back(std::move(domain));
  for (auto& [id, binding] : bindings) result.bindings.push_back(std::move(binding));
  if (!result.valid()) return invalid("derived_scope_invalid");
  return Result<ConfigurationFenceScopeDTO>::Ok(std::move(result));
}

Result<CellAdmissionScopeDTO> SelectedComponentBringUpAdmissionScope(
    const SelectedComponentBringUpOfferDTO& offer) {
  auto fence = SelectedComponentBringUpFenceScope(offer);
  if (!fence.ok()) return Result<CellAdmissionScopeDTO>::Err(fence.error());
  CellAdmissionScopeDTO result{std::move(fence.value().resources), {}};
  for (const auto& domain : offer.predecessor.domains) {
    const auto active = domain.configuration_identity();
    if (!active) continue;
    const auto& current = *active;
    result.configurations.push_back({domain.control_domain_id,
                                     current.active_control_configuration_id,
                                     current.active_control_configuration_generation});
  }
  const auto valid = ValidateCellAdmissionScope(result);
  if (!valid.ok()) return Result<CellAdmissionScopeDTO>::Err(valid.error());
  return Result<CellAdmissionScopeDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
