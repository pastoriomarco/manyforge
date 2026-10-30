#include <algorithm>
#include <manyforge_core/component_lifecycle_admission_query.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <utility>

namespace manyforge::core {
namespace {

// The evaluator's decision as the three admission answers. Its refusals keep
// their exact ErrorDTO (with `decisive_fact`); only an invalid input stays an
// error. The postcondition answer is the plan's own on the same input.
template <typename Health>
[[nodiscard]] Result<LifecycleAdmissionDTO> admission(
    const BrandLifecyclePlanCatalogDTO& catalog, ComponentLifecycleIntentKindDTO intent,
    ComponentLifecycleStateDTO requested_state, const Health& health,
    const Result<LifecycleDomainPlanDecision>& decision) {
  if (decision.ok()) {
    const bool done = decision.value().kind == LifecycleDomainPlanDecisionKind::kAlreadySatisfied;
    return Result<LifecycleAdmissionDTO>::Ok(
        {done ? LifecyclePostconditionResultDTO::kSatisfied
              : LifecyclePostconditionResultDTO::kNotSatisfied,
         std::nullopt, decision.value().selected_transition});
  }
  if (decision.error().code == "component_lifecycle_plan_evaluation_invalid")
    return Result<LifecycleAdmissionDTO>::Err(decision.error());
  const auto plan = std::find_if(catalog.plans.begin(), catalog.plans.end(), [&](const auto& p) {
    return p.intent == intent && p.requested_state == requested_state;
  });
  return Result<LifecycleAdmissionDTO>::Ok(
      {plan == catalog.plans.end()
           ? LifecyclePostconditionResultDTO::kNotSatisfied
           : EvaluateLifecyclePredicate(plan->requested_postcondition, health),
       decision.error()});
}

}  // namespace

CatalogLifecycleAdmission::CatalogLifecycleAdmission(BrandLifecyclePlanCatalogDTO catalog)
    : catalog_(std::move(catalog)) {}

Result<LifecycleAdmissionDTO> CatalogLifecycleAdmission::Evaluate(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecycleObservationDTO& observation) const {
  return admission(catalog_, intent, requested_state, observation,
                   EvaluateLifecycleDomainPlan(intent, requested_state, catalog_, observation));
}

Result<LifecycleAdmissionDTO> CatalogLifecycleAdmission::Evaluate(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const LifecycleDomainHealthDTO& health) const {
  return admission(catalog_, intent, requested_state, health,
                   EvaluateLifecycleDomainPlan(intent, requested_state, catalog_, health));
}

Result<std::array<LifecyclePostconditionResultDTO, 3>> CatalogLifecycleAdmission::RecoverTargets(
    const BrandLifecycleObservationDTO& observation) const {
  return EvaluateLifecycleRecoverTargets(catalog_, observation);
}

}  // namespace manyforge::core
