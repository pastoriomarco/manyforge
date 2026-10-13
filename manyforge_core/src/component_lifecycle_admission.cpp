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

[[nodiscard]] std::optional<std::int64_t> fixed_plan_timeout(
    const BrandLifecyclePlanCatalogDTO& catalog, ComponentLifecycleIntentKindDTO intent) noexcept {
  const auto match =
      std::find_if(catalog.plans.begin(), catalog.plans.end(),
                   [&](const LifecycleIntentPlanDTO& plan) { return plan.intent == intent; });
  if (match == catalog.plans.end()) {
    return std::nullopt;
  }
  return match->timeout_duration_ns;
}

[[nodiscard]] std::optional<std::int64_t> recover_preselection_timeout(
    const BrandLifecyclePlanCatalogDTO& catalog) noexcept {
  std::optional<std::int64_t> result;
  for (const auto& plan : catalog.plans) {
    if (plan.intent != ComponentLifecycleIntentKindDTO::kRecover) {
      continue;
    }
    result =
        result.has_value() ? std::min(*result, plan.timeout_duration_ns) : plan.timeout_duration_ns;
  }
  return result;
}

[[nodiscard]] std::optional<std::int64_t> admission_timeout(
    const ComponentLifecycleIntentDTO& intent,
    const std::vector<BrandLifecyclePlanCatalogDTO>& catalogs) noexcept {
  std::optional<std::int64_t> result;
  for (const auto& catalog : catalogs) {
    const auto selected = intent.intent == ComponentLifecycleIntentKindDTO::kRecover
                              ? recover_preselection_timeout(catalog)
                              : fixed_plan_timeout(catalog, intent.intent);
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
                      ComponentLifecycleDomainEffectDTO::kNone,
                      {},
                      std::nullopt,
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
    const auto selected_timeout = admission_timeout(intent, candidate.lifecycle_plan_catalogs);
    if (!selected_timeout.has_value()) {
      return reject("component_lifecycle_intent_unsupported",
                    "one or more accepted provider catalogs do not support the lifecycle intent",
                    "catalog_plan_missing");
    }
    timeout_duration_ns = *selected_timeout;
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

Result<PreparedComponentLifecycleAdmission> PrepareApplyComponentBringUpAdmission(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAdmissionCandidate& candidate,
    const ApplyComponentBringUpEvidenceDTO& evidence,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& qualified_candidate_domains,
    const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& candidate_provider_occurrences) {
  const auto rejected = [](std::string code, std::string message, std::string reason) {
    auto result = reject(std::move(code), std::move(message), std::move(reason));
    auto error = result.error();
    error.details["effect"] = "not_started";
    return Result<PreparedComponentLifecycleAdmission>::Err(std::move(error));
  };
  if (!intent.locally_valid() ||
      (intent.intent != ComponentLifecycleIntentKindDTO::kPrepare &&
       intent.intent != ComponentLifecycleIntentKindDTO::kEnable &&
       intent.intent != ComponentLifecycleIntentKindDTO::kDisable) ||
      intent.program_node_evidence || candidate.lifecycle_transaction_id.empty() ||
      candidate.lifecycle_transaction_id.value().size() > kMaxLifecycleIdentityBytes ||
      !candidate.lifecycle_transaction_generation.nonzero() || candidate.accepted_at_ns < 0 ||
      !intent.apply_generation_selection || !candidate.apply_generation_selection ||
      !candidate.apply_generation_selection->locally_valid() ||
      candidate.apply_generation_selection != intent.apply_generation_selection ||
      candidate.durable_component_generation_predecessor.value() !=
          (candidate.apply_generation_selection->predecessor_generation
               ? candidate.apply_generation_selection->predecessor_generation->value()
               : (candidate.apply_generation_selection->owner_qualified_predecessor
                      ? candidate.apply_generation_selection->owner_qualified_predecessor->generation.value()
                      : 0U)) ||
      intent.expected_component_lifecycle_generation !=
          candidate.durable_component_generation_predecessor ||
      intent.expected_admission_generation !=
          evidence.parent_authority.parent_claim.intent.expected_admission_generation) {
    return rejected("component_lifecycle_admission_invalid",
                    "the delegated lifecycle admission candidate is invalid", "candidate_invalid");
  }
  if (qualified_candidate_domains.size() != intent.expected_control_domains.size() ||
      candidate_provider_occurrences.size() != qualified_candidate_domains.size() ||
      candidate.lifecycle_plan_catalogs.size() != qualified_candidate_domains.size()) {
    return rejected("component_lifecycle_configuration_stale",
                    "the qualified candidate closure does not match the original request",
                    "candidate_domain_set_changed");
  }
  for (std::size_t index = 0U; index < qualified_candidate_domains.size(); ++index) {
    const auto& scope = qualified_candidate_domains[index];
    const auto& provider = candidate_provider_occurrences[index];
    if (!ExpectedDomainMatchesResolvedScope(intent.expected_control_domains[index], scope) ||
        !provider.locally_valid() || provider.control_domain_id != scope.control_domain_id ||
        provider.control_domain_revision != scope.control_domain_revision) {
      return rejected("component_lifecycle_configuration_stale",
                      "the candidate domain or provider differs from the qualified expectation",
                      "candidate_domain_or_provider_mismatch");
    }
    if (!catalog_matches_occurrence(candidate.lifecycle_plan_catalogs[index], provider)) {
      return rejected("component_lifecycle_catalog_mismatch",
                      "the candidate catalog does not match its exact provider occurrence",
                      "catalog_identity_or_shape_mismatch");
    }
  }
  const auto timeout = admission_timeout(intent, candidate.lifecycle_plan_catalogs);
  if (!timeout.has_value()) {
    return rejected("component_lifecycle_intent_unsupported",
                    "the candidate provider catalog does not support the original lifecycle intent",
                    "catalog_plan_missing");
  }
  if (candidate.accepted_at_ns > std::numeric_limits<std::int64_t>::max() - *timeout) {
    return rejected("component_lifecycle_admission_invalid",
                    "the delegated lifecycle deadline overflows", "deadline_overflow");
  }

  if (!candidate.current_snapshot &&
      candidate.apply_generation_selection->owner_qualified_predecessor) {
    return rejected("component_lifecycle_configuration_stale",
                    "the qualified predecessor requires its exact verified snapshot",
                    "qualified_predecessor_snapshot_missing");
  }
  const auto requested_state = requested_state_for(intent.intent);
  std::optional<ComponentLifecycleStateDTO> component_state;
  std::vector<ComponentLifecycleDomainStatusDTO> domains;
  if (candidate.current_snapshot) {
    const auto& snapshot = *candidate.current_snapshot;
    if (!candidate.apply_generation_selection->owner_qualified_predecessor ||
        candidate.apply_generation_selection->owner_qualified_predecessor->generation !=
            snapshot.component_lifecycle_generation ||
        candidate.apply_generation_selection->owner_qualified_predecessor->snapshot_digest !=
            snapshot.snapshot_digest ||
        (evidence.recovery &&
         evidence.recovery->predecessor_snapshot_digest != snapshot.snapshot_digest) ||
        !VerifyComponentLifecycleCurrentSnapshot(snapshot).ok() ||
        snapshot.controllable_component_id != intent.controllable_component_id ||
        snapshot.component_lifecycle_generation !=
            candidate.durable_component_generation_predecessor ||
        snapshot.active_publication_receipt.request.candidate_graph_revision !=
            intent.expected_graph_revision ||
        snapshot.active_publication_receipt.request.candidate_projection_revision.scene_revision !=
            evidence.candidate_projection_revision.scene_revision ||
        (evidence.recovery &&
         evidence.recovery->predecessor_snapshot_digest != snapshot.snapshot_digest)) {
      return rejected("component_lifecycle_current_snapshot_mismatch",
                      "the delegated predecessor does not match the exact original component facts",
                      "snapshot_identity_or_generation_mismatch");
    }
    component_state = snapshot.component_lifecycle_state;
    domains = resolved_status_domains(intent, snapshot, requested_state);
  } else {
    if (evidence.recovery) {
      return rejected("component_lifecycle_current_snapshot_mismatch",
                      "contained recovery lacks its genuine predecessor",
                      "current_snapshot_missing");
    }
    for (const auto& expected : intent.expected_control_domains) {
      domains.push_back({expected.control_domain_id,
                         expected,
                         std::nullopt,
                         std::nullopt,
                         std::nullopt,
                         std::nullopt,
                         requested_state,
                         ComponentLifecycleDomainActionStateDTO::kNotStarted,
                         ComponentLifecycleDomainEffectDTO::kNone,
                         {},
                         std::nullopt,
                         std::nullopt});
    }
  }

  ConfigurationFenceScopeDTO exact_scope{{intent.controllable_component_id}, {}, {}, {}};
  std::vector<ControlDomainId> reserved_domains;
  for (const auto& domain : domains) {
    exact_scope.domains.push_back(
        {domain.control_domain_id,
         domain.current_scope ? std::optional(domain.current_scope->control_domain_revision)
                              : std::nullopt,
         domain.expected_domain ? std::optional(domain.expected_domain->control_domain_revision)
                                : std::nullopt});
    reserved_domains.push_back(domain.control_domain_id);
  }
  std::map<ControlBindingId, ConfigurationFenceBindingScopeDTO> bindings;
  const auto collect = [&](const std::vector<ResolvedLifecycleControlDomainScopeDTO>& scopes,
                           bool successor) {
    for (const auto& scope : scopes) {
      exact_scope.resources.insert(exact_scope.resources.end(), scope.current_resources.begin(),
                                   scope.current_resources.end());
      for (const auto& binding : scope.current_control_bindings) {
        const auto entry =
            bindings
                .try_emplace(binding.control_binding_id,
                             ConfigurationFenceBindingScopeDTO{binding.control_binding_id,
                                                               std::nullopt, std::nullopt})
                .first;
        auto& revision = successor ? entry->second.candidate_control_binding_revision
                                   : entry->second.predecessor_control_binding_revision;
        if (revision && revision != binding.control_binding_revision) return false;
        revision = binding.control_binding_revision;
      }
    }
    return true;
  };
  if ((candidate.current_snapshot && !collect(candidate.current_snapshot->domains, false)) ||
      !collect(qualified_candidate_domains, true)) {
    return rejected("component_lifecycle_configuration_stale",
                    "the delegated binding union has conflicting revisions",
                    "binding_union_mismatch");
  }
  for (auto& [id, binding] : bindings) exact_scope.bindings.push_back(std::move(binding));
  std::sort(exact_scope.resources.begin(), exact_scope.resources.end());
  exact_scope.resources.erase(
      std::unique(exact_scope.resources.begin(), exact_scope.resources.end()),
      exact_scope.resources.end());
  if (!exact_scope.valid() || exact_scope != evidence.selected_scope) {
    return rejected("component_lifecycle_configuration_stale",
                    "the selected scope does not equal the full predecessor and candidate union",
                    "selected_union_mismatch");
  }
  ComponentLifecycleAcceptedDTO acceptance{
      intent.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew,
      candidate.lifecycle_transaction_id,
      candidate.lifecycle_transaction_generation,
      candidate.apply_generation_selection->candidate_generation,
      candidate.accepted_at_ns,
      *timeout};
  ComponentLifecycleStatusDTO status{acceptance,
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
                                     *timeout,
                                     std::nullopt,
                                     evidence};
  if (!acceptance.locally_valid() || !status.locally_valid()) {
    return rejected("component_lifecycle_admission_invariant_failed",
                    "the delegated lifecycle acceptance is not a closed valid value",
                    "derived_acceptance_invalid");
  }
  return Result<PreparedComponentLifecycleAdmission>::Ok({std::move(acceptance), std::move(status),
                                                          std::move(reserved_domains),
                                                          candidate.lifecycle_plan_catalogs});
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
  if (offer.candidate_participants.empty() ||
      offer.candidate_participants.size() > kMaxLifecycleResolvedBindings) {
    return invalid("candidate_participant_closure_empty_or_oversized");
  }
  std::map<ControlDomainId, ConfigurationFenceDomainScopeDTO> domains;
  std::map<ControlBindingId, ConfigurationFenceBindingScopeDTO> bindings;
  std::vector<ResourceId> resources;
  for (const auto& domain : offer.predecessor.domains) {
    domains.emplace(domain.control_domain_id,
                    ConfigurationFenceDomainScopeDTO{domain.control_domain_id,
                                                     domain.control_domain_revision, std::nullopt});
    for (const auto& binding : domain.current_control_bindings) {
      const auto [entry, inserted] = bindings.emplace(
          binding.control_binding_id,
          ConfigurationFenceBindingScopeDTO{binding.control_binding_id,
                                            binding.control_binding_revision, std::nullopt});
      if (!inserted &&
          entry->second.predecessor_control_binding_revision != binding.control_binding_revision) {
        return invalid("predecessor_binding_revision_conflict");
      }
    }
    resources.insert(resources.end(), domain.current_resources.begin(),
                     domain.current_resources.end());
  }
  for (const auto& participant : offer.candidate_participants) {
    const auto checked = verify_runtime_participant_binding(participant);
    if (!checked.ok()) return Result<ConfigurationFenceScopeDTO>::Err(checked.error());
    if (participant.controllable_component_id != offer.predecessor.controllable_component_id ||
        participant.retained_bundle_id != offer.candidate_projection_revision.retained_bundle_id ||
        participant.graph_revision != offer.candidate_projection_revision.graph_revision ||
        !participant.control_path) {
      return invalid("candidate_component_product_or_control_path_mismatch");
    }
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
    if (!domain.active_control_configuration) continue;
    const auto& current = *domain.active_control_configuration;
    result.configurations.push_back({domain.control_domain_id,
                                     current.active_control_configuration_id,
                                     current.active_control_configuration_generation});
  }
  const auto valid = ValidateCellAdmissionScope(result);
  if (!valid.ok()) return Result<CellAdmissionScopeDTO>::Err(valid.error());
  return Result<CellAdmissionScopeDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
