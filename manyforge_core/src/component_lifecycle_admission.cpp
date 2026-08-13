#include <algorithm>
#include <limits>
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

[[nodiscard]] Result<DisableHandoffComponentScopeExpectationDTO> reject_expectation(
    std::string reason) {
  return Result<DisableHandoffComponentScopeExpectationDTO>::Err(
      ErrorDTO{"component_lifecycle_scope_invalid",
               "the current component snapshot cannot form one exact Disable scope",
               {{"reason", std::move(reason)}}});
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

[[nodiscard]] std::vector<ComponentLifecycleDomainStatusDTO> unresolved_disable_domains(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleRequestedStateDTO& requested_state) {
  std::vector<ComponentLifecycleDomainStatusDTO> result;
  result.reserve(intent.expected_control_domains.size());
  for (const auto& expected : intent.expected_control_domains) {
    result.push_back({expected.control_domain_id,
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
  return result;
}

}  // namespace

Result<DisableHandoffComponentScopeExpectationDTO> BuildDisableHandoffComponentScopeExpectation(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  if (!snapshot.locally_valid()) {
    return reject_expectation("current_snapshot_invalid");
  }

  ConfigurationFenceScopeDTO scope;
  scope.controllable_component_ids = {snapshot.controllable_component_id};
  std::vector<CurrentDomainActiveControlConfigurationDTO> configurations;
  configurations.reserve(snapshot.domains.size());
  for (const auto& domain : snapshot.domains) {
    scope.domains.push_back(
        {domain.control_domain_id, domain.control_domain_revision, std::nullopt});
    for (const auto& binding : domain.current_control_bindings) {
      scope.bindings.push_back(
          {binding.control_binding_id, binding.control_binding_revision, std::nullopt});
    }
    scope.resources.insert(scope.resources.end(), domain.current_resources.begin(),
                           domain.current_resources.end());
    if (domain.active_control_configuration.has_value()) {
      configurations.push_back(
          {domain.control_domain_id,
           {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
            domain.active_control_configuration->active_control_configuration_id,
            domain.active_control_configuration->active_control_configuration_generation}});
    } else {
      configurations.push_back(
          {domain.control_domain_id,
           {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt}});
    }
  }
  const auto binding_less = [](const ConfigurationFenceBindingScopeDTO& left,
                               const ConfigurationFenceBindingScopeDTO& right) {
    return left.control_binding_id < right.control_binding_id;
  };
  std::sort(scope.bindings.begin(), scope.bindings.end(), binding_less);
  if (std::adjacent_find(scope.bindings.begin(), scope.bindings.end(),
                         [](const auto& left, const auto& right) {
                           return left.control_binding_id == right.control_binding_id;
                         }) != scope.bindings.end()) {
    return reject_expectation("control_binding_occurs_in_several_domains");
  }
  std::sort(scope.resources.begin(), scope.resources.end());
  scope.resources.erase(std::unique(scope.resources.begin(), scope.resources.end()),
                        scope.resources.end());
  if (!scope.valid()) {
    return reject_expectation("derived_component_scope_invalid");
  }

  DisableHandoffComponentScopeExpectationDTO result{
      snapshot.snapshot_digest,
      snapshot.active_publication_receipt.publication_generation,
      snapshot.active_publication_receipt.durable_public_pointer_content_digest,
      snapshot.active_publication_receipt.request.candidate_retained_bundle_id,
      snapshot.active_publication_receipt.request.candidate_graph_revision,
      snapshot.component_lifecycle_generation,
      std::move(scope),
      std::move(configurations)};
  if (!result.locally_valid()) {
    return reject_expectation("derived_component_expectation_invalid");
  }
  return Result<DisableHandoffComponentScopeExpectationDTO>::Ok(std::move(result));
}

Result<PreparedComponentLifecycleAdmission> PrepareComponentLifecycleAdmission(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAdmissionCandidate& candidate) {
  if (!intent.locally_valid() || candidate.lifecycle_transaction_id.empty() ||
      candidate.lifecycle_transaction_id.value().size() > kMaxLifecycleIdentityBytes ||
      !candidate.lifecycle_transaction_generation.nonzero() || candidate.accepted_at_ns < 0 ||
      !candidate.durable_component_generation_predecessor.nonzero() ||
      candidate.durable_component_generation_predecessor.value() ==
          std::numeric_limits<std::uint64_t>::max() ||
      candidate.containment_only_disable_timeout_duration_ns <= 0 ||
      candidate.containment_only_disable_timeout_duration_ns > kMaxLifecycleTransactionTimeoutNs ||
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
  bool lifecycle_mutation_eligible = false;

  if (!candidate.current_snapshot.has_value()) {
    if (intent.intent != ComponentLifecycleIntentKindDTO::kDisable ||
        !candidate.lifecycle_plan_catalogs.empty() ||
        !candidate.disable_handoff_admission.has_value() ||
        !candidate.disable_handoff_admission->locally_valid() ||
        candidate.disable_handoff_admission->query.controllable_component_id !=
            intent.controllable_component_id ||
        candidate.disable_handoff_admission->query.request_kind !=
            DisableHandoffScopeRequestKindDTO::kFullStopRequired ||
        candidate.disable_handoff_admission->query.component_expectation.has_value() ||
        candidate.disable_handoff_admission->scope_resolution !=
            DisableHandoffScopeResolutionDTO::kPromotedToFull) {
      return reject("component_lifecycle_scope_unresolved",
                    "only a recognized promoted Full-Stop Disable may omit current scope",
                    "unresolved_scope_not_promoted_disable");
    }
    if (intent.expected_component_lifecycle_generation.value() >
        candidate.durable_component_generation_predecessor.value()) {
      return reject("component_lifecycle_current_snapshot_mismatch",
                    "an unresolved-scope Disable expectation cannot advance the durable "
                    "predecessor",
                    "unresolved_disable_expected_generation_ahead");
    }
    timeout_duration_ns = candidate.containment_only_disable_timeout_duration_ns;
    domains = unresolved_disable_domains(intent, requested_state);
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
    if (intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
      if (snapshot.active_publication_receipt.request.candidate_graph_revision !=
              intent.expected_graph_revision ||
          candidate.lifecycle_plan_catalogs.size() != snapshot.provider_occurrences.size()) {
        return reject("component_lifecycle_current_snapshot_mismatch",
                      "the coherent current graph or catalog closure does not match admission",
                      "snapshot_graph_or_catalog_count_mismatch");
      }
      if (intent.expected_component_lifecycle_generation !=
          candidate.durable_component_generation_predecessor) {
        return reject("component_lifecycle_current_snapshot_mismatch",
                      "the lifecycle expectation does not match the durable predecessor",
                      "expected_generation_or_watermark_mismatch");
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
    } else if (intent.expected_component_lifecycle_generation.value() >
               candidate.durable_component_generation_predecessor.value()) {
      return reject("component_lifecycle_current_snapshot_mismatch",
                    "a Disable expectation cannot advance the durable predecessor",
                    "disable_expected_generation_ahead");
    }
    const bool complete_catalog_closure =
        candidate.lifecycle_plan_catalogs.size() == snapshot.provider_occurrences.size();
    bool catalogs_qualified = complete_catalog_closure;
    if (complete_catalog_closure) {
      for (std::size_t index = 0U; index < snapshot.provider_occurrences.size(); ++index) {
        catalogs_qualified = catalogs_qualified &&
                             catalog_matches_occurrence(candidate.lifecycle_plan_catalogs[index],
                                                        snapshot.provider_occurrences[index]);
      }
    }
    if (intent.intent != ComponentLifecycleIntentKindDTO::kDisable && !catalogs_qualified) {
      return reject("component_lifecycle_catalog_mismatch",
                    "one or more lifecycle catalogs do not match the accepted provider closure",
                    "catalog_identity_or_shape_mismatch");
    }
    const auto selected_timeout = catalogs_qualified
                                      ? admission_timeout(intent, candidate.lifecycle_plan_catalogs)
                                      : std::nullopt;
    if (!selected_timeout.has_value() &&
        intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
      return reject("component_lifecycle_intent_unsupported",
                    "one or more accepted provider catalogs do not support the lifecycle intent",
                    "catalog_plan_missing");
    }
    timeout_duration_ns =
        selected_timeout.value_or(candidate.containment_only_disable_timeout_duration_ns);
    if (selected_timeout.has_value()) {
      qualified_catalogs = candidate.lifecycle_plan_catalogs;
    }
    component_state = snapshot.component_lifecycle_state;
    domains = resolved_status_domains(intent, snapshot, requested_state);
    reserved_domains.reserve(snapshot.domains.size());
    for (const auto& domain : snapshot.domains) {
      reserved_domains.push_back(domain.control_domain_id);
    }

    if (intent.intent == ComponentLifecycleIntentKindDTO::kDisable) {
      const auto expectation = BuildDisableHandoffComponentScopeExpectation(snapshot);
      if (!expectation.ok() || !candidate.disable_handoff_admission.has_value() ||
          !candidate.disable_handoff_admission->locally_valid() ||
          candidate.disable_handoff_admission->query.controllable_component_id !=
              intent.controllable_component_id ||
          candidate.disable_handoff_admission->query.request_kind !=
              DisableHandoffScopeRequestKindDTO::kResolvedComponentScope ||
          candidate.disable_handoff_admission->query.component_expectation !=
              std::optional(expectation.value())) {
        return reject("component_lifecycle_disable_admission_mismatch",
                      "the Kernel Disable admission snapshot does not match the component snapshot",
                      "disable_admission_expectation_mismatch");
      }
      lifecycle_mutation_eligible =
          candidate.disable_handoff_admission->scope_resolution ==
              DisableHandoffScopeResolutionDTO::kExact &&
          snapshot.active_publication_receipt.request.candidate_graph_revision ==
              intent.expected_graph_revision &&
          intent.expected_component_lifecycle_generation ==
              candidate.durable_component_generation_predecessor &&
          intent.expected_control_domains.size() == snapshot.domains.size() &&
          std::equal(intent.expected_control_domains.begin(), intent.expected_control_domains.end(),
                     snapshot.domains.begin(), [](const auto& expected, const auto& current) {
                       return ExpectedDomainMatchesResolvedScope(expected, current);
                     });
    } else if (candidate.disable_handoff_admission.has_value()) {
      return reject("component_lifecycle_admission_invalid",
                    "a non-Disable lifecycle admission cannot carry a Kernel handoff snapshot",
                    "unexpected_disable_admission");
    } else {
      lifecycle_mutation_eligible = true;
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
                                             std::nullopt,
                                             std::nullopt,
                                             std::nullopt,
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
       std::move(qualified_catalogs), lifecycle_mutation_eligible,
       candidate.disable_handoff_admission});
}

}  // namespace manyforge::core
