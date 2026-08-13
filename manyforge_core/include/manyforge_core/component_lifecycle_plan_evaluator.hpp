#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

enum class LifecycleDomainPlanDecisionKind : std::uint8_t {
  kAlreadySatisfied = 0,
  kDispatchPrimitive,
};

/// Closed in-process result of evaluating one exact accepted plan against one
/// fresh provider observation. It contains no adapter call or authority token.
struct LifecycleDomainPlanDecision final {
  LifecycleDomainPlanDecisionKind kind;
  LifecycleIntentPlanDTO selected_plan;
  std::optional<LifecyclePrimitiveTransitionDTO> selected_transition;
};

struct LifecycleRecoverSelectionInput final {
  ResolvedLifecycleProviderOccurrenceDTO provider_occurrence;
  BrandLifecyclePlanCatalogDTO lifecycle_plan_catalog;
  BrandLifecycleObservationDTO fresh_observation;
};

/// Select one common Recover target from complete, aligned, fresh domain
/// observations. Zero, several, or potentially-common inconclusive targets
/// fail closed without choosing a primitive.
[[nodiscard]] Result<ComponentLifecycleStateDTO> SelectCommonRecoverTarget(
    const std::vector<LifecycleRecoverSelectionInput>& inputs);

/// Evaluate one exact fixed or already-resolved Recover plan. An already
/// satisfied postcondition succeeds without a primitive; otherwise exactly
/// one transition must be satisfied and every other transition conclusively
/// not satisfied.
[[nodiscard]] Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog,
    const BrandLifecycleObservationDTO& fresh_observation);

}  // namespace manyforge::core
