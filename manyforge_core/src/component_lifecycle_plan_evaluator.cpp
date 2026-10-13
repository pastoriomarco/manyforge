#include <algorithm>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
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
[[nodiscard]] Result<LifecycleDomainPlanDecision> reject_evaluation(
    std::string code, std::string message, std::string reason,
    const Health& observation) {
  return Result<LifecycleDomainPlanDecision>::Err(ErrorDTO{
      std::move(code),
      std::move(message),
      {{"reason", std::move(reason)},
       {"observed_state", std::string(to_string(observation.observed_state))},
       {"observed_controller_loaded",
        observed_boolean_fact(observation, "external.controller_loaded")},
       {"observed_fault_present", observed_boolean_fact(observation, "external.fault_present")},
       {"observed_transport_ready",
        observed_boolean_fact(observation, "external.transport_ready")}}});
}

[[nodiscard]] bool occurrence_matches_observation(
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence,
    const BrandLifecycleObservationDTO& observation) noexcept {
  return occurrence.control_domain_id == observation.control_domain_id &&
         occurrence.control_domain_revision == observation.control_domain_revision &&
         occurrence.provider_instance_id == observation.provider_instance_id &&
         occurrence.provider_manifest_id == observation.provider_manifest_id &&
         occurrence.provider_manifest_revision == observation.provider_manifest_revision &&
         occurrence.provider_runtime_unit_id == observation.provider_runtime_unit_id &&
         occurrence.provider_runtime_epoch == observation.provider_runtime_epoch;
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
    return reject_evaluation("component_lifecycle_postcondition_inconclusive",
                             "the requested lifecycle postcondition is inconclusive",
                             "requested_postcondition_inconclusive", fresh_observation);
  }

  const LifecyclePrimitiveTransitionDTO* selected_transition = nullptr;
  bool any_inconclusive = false;
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
      any_inconclusive = true;
    }
  }
  if (any_inconclusive) {
    return reject_evaluation(
        "component_lifecycle_transition_inconclusive",
        "every non-selected lifecycle transition must be conclusively ineligible",
        "transition_eligibility_inconclusive", fresh_observation);
  }
  if (selected_transition == nullptr) {
    return reject_evaluation("component_lifecycle_transition_missing",
                             "no lifecycle primitive is eligible for the fresh observation",
                             "no_transition_satisfied", fresh_observation);
  }
  return Result<LifecycleDomainPlanDecision>::Ok(
      {LifecycleDomainPlanDecisionKind::kDispatchPrimitive, *selected_plan, *selected_transition});
}

}  // namespace

Result<ComponentLifecycleStateDTO> SelectCommonRecoverTarget(
    const std::vector<LifecycleRecoverSelectionInput>& inputs) {
  if (inputs.empty() || inputs.size() > kMaxLifecycleProviderOccurrences) {
    return reject<ComponentLifecycleStateDTO>(
        "component_lifecycle_recover_selection_invalid",
        "Recover target selection requires a bounded complete provider set",
        "provider_set_empty_or_over_bound");
  }
  for (const auto& input : inputs) {
    if (!input.provider_occurrence.locally_valid() ||
        !VerifyBrandLifecyclePlanCatalog(input.lifecycle_plan_catalog).ok() ||
        !input.fresh_observation.locally_valid() ||
        input.lifecycle_plan_catalog.provider_manifest_id !=
            input.provider_occurrence.provider_manifest_id ||
        input.lifecycle_plan_catalog.provider_manifest_revision !=
            input.provider_occurrence.provider_manifest_revision ||
        input.lifecycle_plan_catalog.catalog_digest !=
            input.provider_occurrence.lifecycle_plan_catalog_digest ||
        !occurrence_matches_observation(input.provider_occurrence, input.fresh_observation)) {
      return reject<ComponentLifecycleStateDTO>(
          "component_lifecycle_recover_selection_invalid",
          "Recover input does not match its accepted provider occurrence",
          "provider_catalog_or_observation_mismatch");
    }
  }

  constexpr ComponentLifecycleStateDTO kRecoverTargets[] = {ComponentLifecycleStateDTO::kDeclared,
                                                            ComponentLifecycleStateDTO::kPrepared,
                                                            ComponentLifecycleStateDTO::kDisabled};
  std::vector<ComponentLifecycleStateDTO> common_targets;
  bool potentially_common_inconclusive = false;
  for (const auto target : kRecoverTargets) {
    bool all_satisfied = true;
    bool conclusively_excluded = false;
    bool any_inconclusive = false;
    for (const auto& input : inputs) {
      const auto* plan = find_plan(input.lifecycle_plan_catalog,
                                   ComponentLifecycleIntentKindDTO::kRecover, target);
      if (plan == nullptr) {
        conclusively_excluded = true;
        all_satisfied = false;
        break;
      }
      const auto evaluation =
          EvaluateLifecyclePredicate(*plan->recover_target_eligibility, input.fresh_observation);
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
      common_targets.push_back(target);
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

Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog,
    const BrandLifecycleObservationDTO& fresh_observation) {
  if (!VerifyBrandLifecyclePlanCatalog(catalog).ok() || !fresh_observation.locally_valid() ||
      catalog.provider_manifest_id != fresh_observation.provider_manifest_id ||
      catalog.provider_manifest_revision != fresh_observation.provider_manifest_revision) {
    return reject<LifecycleDomainPlanDecision>(
        "component_lifecycle_plan_evaluation_invalid",
        "the plan catalog and observation are not one exact provider contract",
        "catalog_or_observation_invalid");
  }
  return evaluate_normalized_plan(intent, requested_state, catalog, fresh_observation);
}

Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog,
    const ResolvedLifecycleProviderOccurrenceDTO& provider_occurrence,
    const LifecycleDomainHealthDTO& health) {
  if (!VerifyBrandLifecyclePlanCatalog(catalog).ok() ||
      !provider_occurrence.locally_valid() || !health.locally_valid() ||
      catalog.provider_manifest_id != provider_occurrence.provider_manifest_id ||
      catalog.provider_manifest_revision != provider_occurrence.provider_manifest_revision ||
      catalog.catalog_digest != provider_occurrence.lifecycle_plan_catalog_digest) {
    return reject<LifecycleDomainPlanDecision>(
        "component_lifecycle_plan_evaluation_invalid",
        "the plan catalog, occurrence and health are not one exact provider contract",
        "catalog_occurrence_or_health_invalid");
  }
  return evaluate_normalized_plan(intent, requested_state, catalog, health);
}

}  // namespace manyforge::core
