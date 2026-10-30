#include <algorithm>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::core {
namespace {

template <typename T>
[[nodiscard]] Result<T> reject(std::string code, std::string message, std::string reason) {
  return Result<T>::Err(
      ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}});
}

// DX496: diagnostics come only from the same validated observation on failure.
// They never participate in eligibility or source/generation comparisons.
template <typename Health>
[[nodiscard]] const char* observed_boolean_fact(const Health& observation,
                                                std::string_view id) noexcept {
  const auto found = std::find_if(
      observation.facts.begin(), observation.facts.end(),
      [id](const LifecycleFactDTO& fact) { return fact.lifecycle_fact_id.value() == id; });
  if (found == observation.facts.end()) {
    return "missing";
  }
  switch (found->value.kind) {
    case LifecycleFactValueKindDTO::kUnknown:
      return "unknown";
    case LifecycleFactValueKindDTO::kBoolean:
      return *found->value.boolean_value ? "true" : "false";
    case LifecycleFactValueKindDTO::kInteger:
    case LifecycleFactValueKindDTO::kToken:
      return "nonboolean";
  }
  return "nonboolean";
}

template <typename Health>
[[nodiscard]] std::string observed_token_fact(const Health& observation, std::string_view id) {
  const auto found = std::find_if(
      observation.facts.begin(), observation.facts.end(),
      [id](const LifecycleFactDTO& fact) { return fact.lifecycle_fact_id.value() == id; });
  if (found == observation.facts.end()) return "missing";
  if (found->value.kind != LifecycleFactValueKindDTO::kToken || !found->value.token_value)
    return "nontoken";
  return found->value.token_value->substr(0, kMaxLifecycleErrorDetailValueBytes);
}

// The first clause of `predicate` the observation does not meet, as
// "<fact>=<observed>" or "state=<observed state>"; empty when it is met.
template <typename Health>
[[nodiscard]] std::string unmet_clause(const LifecyclePredicateDTO& predicate,
                                       const Health& observation) {
  if (!std::binary_search(predicate.allowed_states.begin(), predicate.allowed_states.end(),
                          observation.observed_state)) {
    return "state=" + std::string(to_string(observation.observed_state));
  }
  for (const auto& expected : predicate.fact_predicates) {
    const auto found = std::find_if(
        observation.facts.begin(), observation.facts.end(), [&](const LifecycleFactDTO& fact) {
          return fact.lifecycle_fact_id == expected.lifecycle_fact_id;
        });
    if (found == observation.facts.end() || found->value != expected.expected_value) {
      return expected.lifecycle_fact_id.value() + "=" +
             observed_boolean_fact(observation, expected.lifecycle_fact_id.value());
    }
  }
  return {};
}

// Decisive facts of a refusal: the distinct unmet clauses, bounded to one
// detail value at a clause boundary.
[[nodiscard]] std::string decisive_facts(const std::vector<std::string>& clauses) {
  std::string joined;
  for (auto clause = clauses.begin(); clause != clauses.end(); ++clause) {
    if (clause->empty() || std::find(clauses.begin(), clause, *clause) != clause) continue;
    const auto next = joined.empty() ? *clause : joined + ", " + *clause;
    if (next.size() > kMaxLifecycleErrorDetailValueBytes) break;
    joined = next;
  }
  return joined;
}

template <typename Health>
[[nodiscard]] Result<LifecycleDomainPlanDecision> reject_evaluation(
    std::string code, std::string message, std::string reason,
    const Health& observation, std::string decisive_fact = {}) {
  // Robot providers publish external.* facts; the generic I/O driver publishes
  // io.driver.* facts under the same detail keys (plus resources_ready, and
  // without the robot-only controller_loaded).
  const bool io_driver = std::any_of(
      observation.facts.begin(), observation.facts.end(), [](const LifecycleFactDTO& fact) {
        return fact.lifecycle_fact_id.value().starts_with("io.driver.");
      });
  const std::string prefix = io_driver ? "io.driver." : "external.";
  ErrorDTO error{
      std::move(code),
      std::move(message),
      {{"reason", std::move(reason)},
       {"observed_state", std::string(to_string(observation.observed_state))},
       {"observed_fault_present", observed_boolean_fact(observation, prefix + "fault_present")},
       {"observed_transport_ready",
        observed_boolean_fact(observation, prefix + "transport_ready")},
       {"observed_blocking_cause",
        observed_token_fact(observation, kLifecycleBlockingCauseFactId)}}};
  if (io_driver)
    error.details.emplace("observed_resources_ready",
                          observed_boolean_fact(observation, "io.driver.resources_ready"));
  else
    error.details.emplace("observed_controller_loaded",
                          observed_boolean_fact(observation, "external.controller_loaded"));
  if (!decisive_fact.empty()) error.details.emplace("decisive_fact", std::move(decisive_fact));
  if (observation.diagnostic && !observation.diagnostic->code.empty())
    error.details.emplace("observed_diagnostic_code",
                          observation.diagnostic->code.substr(0, kMaxLifecycleErrorDetailValueBytes));
  return Result<LifecycleDomainPlanDecision>::Err(std::move(error));
}

[[nodiscard]] const LifecycleIntentPlanDTO* find_plan(
    const BrandLifecyclePlanCatalogDTO& catalog, ComponentLifecycleIntentKindDTO intent,
    ComponentLifecycleStateDTO requested_state) noexcept {
  const auto match = std::lower_bound(
      catalog.plans.begin(), catalog.plans.end(), std::pair{intent, requested_state},
      [](const LifecycleIntentPlanDTO& candidate,
         const std::pair<ComponentLifecycleIntentKindDTO, ComponentLifecycleStateDTO>& key) {
        return candidate.intent < key.first ||
               (candidate.intent == key.first && candidate.requested_state < key.second);
      });
  if (match == catalog.plans.end() || match->intent != intent ||
      match->requested_state != requested_state) {
    return nullptr;
  }
  return &*match;
}

// Whether a Declared domain has nothing engaged, by the provider contract on
// `LifecycleIntentPlanDTO`: its Recover plan's postcondition admits only the
// resting states declared/disabled, includes declared, and holds on the
// fresh observation (e.g. a Real arm: hardware and controller inactive, no
// vendor error or warning, no fault, transport ready). This relies on the
// provider contract; a provider without such a plan gives no such assurance,
// and its Disable fails closed as before.
template <typename Health>
[[nodiscard]] bool unarmed_resting_proven(const BrandLifecyclePlanCatalogDTO& catalog,
                                          const Health& fresh_observation) {
  return std::any_of(catalog.plans.begin(), catalog.plans.end(), [&](const auto& plan) {
    const auto& states = plan.requested_postcondition.allowed_states;
    return plan.intent == ComponentLifecycleIntentKindDTO::kRecover &&
           std::find(states.begin(), states.end(), ComponentLifecycleStateDTO::kDeclared) !=
               states.end() &&
           std::all_of(states.begin(), states.end(), [](ComponentLifecycleStateDTO state) {
             return state == ComponentLifecycleStateDTO::kDeclared ||
                    state == ComponentLifecycleStateDTO::kDisabled;
           }) &&
           EvaluateLifecyclePredicate(plan.requested_postcondition, fresh_observation) ==
               LifecyclePostconditionResultDTO::kSatisfied;
  });
}

template <typename Health>
[[nodiscard]] Result<LifecycleDomainPlanDecision> evaluate_normalized_plan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog, const Health& fresh_observation) {
  const auto* selected_plan = find_plan(catalog, intent, requested_state);
  if (selected_plan == nullptr) {
    return reject_evaluation("component_lifecycle_intent_unsupported",
                             "the accepted provider catalog has no requested lifecycle plan",
                             "selected_plan_missing", fresh_observation);
  }

  const auto requested =
      EvaluateLifecyclePredicate(selected_plan->requested_postcondition, fresh_observation);
  if (requested == LifecyclePostconditionResultDTO::kSatisfied) {
    return Result<LifecycleDomainPlanDecision>::Ok(
        {LifecycleDomainPlanDecisionKind::kAlreadySatisfied, *selected_plan, std::nullopt});
  }
  if (requested == LifecyclePostconditionResultDTO::kInconclusive) {
    return reject_evaluation(
        "component_lifecycle_postcondition_inconclusive",
        "the requested lifecycle postcondition is inconclusive",
        "requested_postcondition_inconclusive", fresh_observation,
        unmet_clause(selected_plan->requested_postcondition, fresh_observation));
  }

  const LifecyclePrimitiveTransitionDTO* selected_transition = nullptr;
  // Decisive facts: the unmet clause of each inconclusive transition, and of
  // each not-satisfied transition that allows the observed state.
  std::vector<std::string> inconclusive_clauses;
  std::vector<std::string> unmet_clauses;
  for (const auto& transition : selected_plan->transitions) {
    const auto eligibility =
        EvaluateLifecyclePredicate(transition.eligibility_predicate, fresh_observation);
    if (eligibility == LifecyclePostconditionResultDTO::kSatisfied) {
      if (selected_transition != nullptr) {
        return reject_evaluation("component_lifecycle_transition_ambiguous",
                                 "several lifecycle primitives are eligible for one observation",
                                 "several_transitions_satisfied", fresh_observation);
      }
      selected_transition = &transition;
    } else if (eligibility == LifecyclePostconditionResultDTO::kInconclusive) {
      inconclusive_clauses.push_back(unmet_clause(transition.eligibility_predicate, fresh_observation));
    } else if (std::binary_search(transition.eligibility_predicate.allowed_states.begin(),
                                  transition.eligibility_predicate.allowed_states.end(),
                                  fresh_observation.observed_state)) {
      unmet_clauses.push_back(unmet_clause(transition.eligibility_predicate, fresh_observation));
    }
  }
  if (!inconclusive_clauses.empty()) {
    return reject_evaluation(
        "component_lifecycle_transition_inconclusive",
        "every non-selected lifecycle transition must be conclusively ineligible",
        "transition_eligibility_inconclusive", fresh_observation,
        decisive_facts(inconclusive_clauses));
  }
  if (selected_transition == nullptr && intent == ComponentLifecycleIntentKindDTO::kDisable &&
      fresh_observation.observed_state == ComponentLifecycleStateDTO::kDeclared &&
      unarmed_resting_proven(catalog, fresh_observation)) {
    // No Disable step applies and, relying on the provider contract, its
    // unarmed resting predicate holds: nothing is engaged, so the domain is
    // already at its Disabled target (CCLC records it Disabled).
    return Result<LifecycleDomainPlanDecision>::Ok(
        {LifecycleDomainPlanDecisionKind::kAlreadySatisfied, *selected_plan, std::nullopt});
  }
  if (selected_transition == nullptr) {
    // Without a transition the unmet postcondition decides; when no
    // transition allows the observed state, the state itself decides.
    auto decisive = decisive_facts(unmet_clauses);
    if (decisive.empty())
      decisive = selected_plan->transitions.empty()
                     ? unmet_clause(selected_plan->requested_postcondition, fresh_observation)
                     : "state=" + std::string(to_string(fresh_observation.observed_state));
    return reject_evaluation("component_lifecycle_transition_missing",
                             "no lifecycle primitive is eligible for the fresh observation",
                             "no_transition_satisfied", fresh_observation, std::move(decisive));
  }
  return Result<LifecycleDomainPlanDecision>::Ok(
      {LifecycleDomainPlanDecisionKind::kDispatchPrimitive, *selected_plan, *selected_transition});
}

}  // namespace

Result<ComponentLifecycleStateDTO> SelectCommonRecoverTarget(
    const std::vector<std::array<LifecyclePostconditionResultDTO, 3>>& domain_targets) {
  if (domain_targets.empty() || domain_targets.size() > kMaxLifecycleProviderOccurrences) {
    return reject<ComponentLifecycleStateDTO>(
        "component_lifecycle_recover_selection_invalid",
        "Recover target selection requires a bounded complete provider set",
        "provider_set_empty_or_over_bound");
  }

  std::vector<ComponentLifecycleStateDTO> common_targets;
  bool potentially_common_inconclusive = false;
  for (std::size_t target = 0U; target < kLifecycleRecoverTargets.size(); ++target) {
    bool all_satisfied = true;
    bool conclusively_excluded = false;
    bool any_inconclusive = false;
    for (const auto& answers : domain_targets) {
      const auto evaluation = answers[target];
      if (evaluation == LifecyclePostconditionResultDTO::kNotSatisfied) {
        conclusively_excluded = true;
        all_satisfied = false;
        break;
      }
      if (evaluation == LifecyclePostconditionResultDTO::kInconclusive) {
        any_inconclusive = true;
        all_satisfied = false;
      }
    }
    if (all_satisfied) {
      common_targets.push_back(kLifecycleRecoverTargets[target]);
    } else if (!conclusively_excluded && any_inconclusive) {
      potentially_common_inconclusive = true;
    }
  }

  if (potentially_common_inconclusive) {
    return reject<ComponentLifecycleStateDTO>(
        "component_lifecycle_recover_target_inconclusive",
        "a potentially common Recover target has inconclusive eligibility",
        "recover_target_inconclusive");
  }
  if (common_targets.empty()) {
    return reject<ComponentLifecycleStateDTO>(
        "component_lifecycle_recover_target_missing",
        "no Recover target is common to every accepted provider occurrence",
        "no_common_recover_target");
  }
  if (common_targets.size() != 1U) {
    return reject<ComponentLifecycleStateDTO>(
        "component_lifecycle_recover_target_ambiguous",
        "several Recover targets are common to every accepted provider occurrence",
        "several_common_recover_targets");
  }
  return Result<ComponentLifecycleStateDTO>::Ok(common_targets.front());
}

Result<std::array<LifecyclePostconditionResultDTO, 3>> EvaluateLifecycleRecoverTargets(
    const BrandLifecyclePlanCatalogDTO& catalog,
    const BrandLifecycleObservationDTO& fresh_observation) {
  if (!fresh_observation.locally_valid()) {
    return reject<std::array<LifecyclePostconditionResultDTO, 3>>(
        "component_lifecycle_recover_selection_invalid",
        "Recover input observation is invalid or names another control domain",
        "observation_invalid_or_domain_mismatch");
  }
  std::array<LifecyclePostconditionResultDTO, 3> answers{};
  for (std::size_t target = 0U; target < kLifecycleRecoverTargets.size(); ++target) {
    const auto* plan = find_plan(catalog, ComponentLifecycleIntentKindDTO::kRecover,
                                 kLifecycleRecoverTargets[target]);
    answers[target] =
        plan == nullptr
            ? LifecyclePostconditionResultDTO::kNotSatisfied
            : EvaluateLifecyclePredicate(*plan->recover_target_eligibility, fresh_observation);
  }
  return Result<std::array<LifecyclePostconditionResultDTO, 3>>::Ok(answers);
}

Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog,
    const BrandLifecycleObservationDTO& fresh_observation) {
  // The catalog was verified once when composed (BuildOwnedLifecyclePlanCatalogSource).
  if (!fresh_observation.locally_valid()) {
    return reject<LifecycleDomainPlanDecision>(
        "component_lifecycle_plan_evaluation_invalid",
        "the lifecycle observation is not a valid fresh observation",
        "catalog_or_observation_invalid");
  }
  return evaluate_normalized_plan(intent, requested_state, catalog, fresh_observation);
}

Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog,
    const ResolvedLifecycleProviderOccurrenceDTO& provider_occurrence,
    const LifecycleDomainHealthDTO& health) {
  if (!provider_occurrence.locally_valid()) {
    return reject<LifecycleDomainPlanDecision>(
        "component_lifecycle_plan_evaluation_invalid",
        "the provider occurrence or health projection is invalid",
        "catalog_occurrence_or_health_invalid");
  }
  return EvaluateLifecycleDomainPlan(intent, requested_state, catalog, health);
}

Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog, const LifecycleDomainHealthDTO& health) {
  if (!health.locally_valid()) {
    return reject<LifecycleDomainPlanDecision>(
        "component_lifecycle_plan_evaluation_invalid",
        "the provider occurrence or health projection is invalid",
        "catalog_occurrence_or_health_invalid");
  }
  return evaluate_normalized_plan(intent, requested_state, catalog, health);
}

}  // namespace manyforge::core
