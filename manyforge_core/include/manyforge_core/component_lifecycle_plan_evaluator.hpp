#pragma once

#include <array>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

enum class LifecycleDomainPlanDecisionKind : std::uint8_t {
  kAlreadySatisfied = 0,
  kDispatchPrimitive,
};

/// Recover targets in the order the per-target answers use.
inline constexpr std::array<ComponentLifecycleStateDTO, 3> kLifecycleRecoverTargets{
    ComponentLifecycleStateDTO::kDeclared, ComponentLifecycleStateDTO::kPrepared,
    ComponentLifecycleStateDTO::kDisabled};

/// Closed in-process result of evaluating one exact accepted plan against one
/// fresh provider observation. It contains no adapter call or authority token.
struct LifecycleDomainPlanDecision final {
  LifecycleDomainPlanDecisionKind kind;
  LifecycleIntentPlanDTO selected_plan;
  std::optional<LifecyclePrimitiveTransitionDTO> selected_transition;
};

/// Catalogs passed here come from a catalog source that verified them once at
/// composition; evaluation does not re-verify their digest or provider
/// identity. A refusal names its decisive unmet clauses in
/// `details.decisive_fact`.
///
/// Select one common Recover target from every domain's per-target answers
/// (`ILifecycleAdmission::RecoverTargets`, in `kLifecycleRecoverTargets`
/// order). Zero, several, or potentially-common inconclusive targets fail
/// closed without choosing a target.
[[nodiscard]] Result<ComponentLifecycleStateDTO> SelectCommonRecoverTarget(
    const std::vector<std::array<LifecyclePostconditionResultDTO, 3>>& domain_targets);

/// Evaluate one exact fixed or already-resolved Recover plan. An already
/// satisfied postcondition succeeds without a primitive; otherwise exactly
/// one transition must be satisfied and every other transition conclusively
/// not satisfied.
[[nodiscard]] Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog,
    const BrandLifecycleObservationDTO& fresh_observation);

/// Evaluate the same plan/predicates over a pure exact-occurrence health
/// projection. This does not construct or validate an execution observation
/// and cannot grant admission or dispatch a primitive.
[[nodiscard]] Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog,
    const ResolvedLifecycleProviderOccurrenceDTO& provider_occurrence,
    const LifecycleDomainHealthDTO& health);

/// The same over a health projection whose occurrence the caller holds.
[[nodiscard]] Result<LifecycleDomainPlanDecision> EvaluateLifecycleDomainPlan(
    ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
    const BrandLifecyclePlanCatalogDTO& catalog, const LifecycleDomainHealthDTO& health);

/// Each Recover target's eligibility on one fresh observation, in
/// `kLifecycleRecoverTargets` order; a target without a plan is not satisfied.
[[nodiscard]] Result<std::array<LifecyclePostconditionResultDTO, 3>> EvaluateLifecycleRecoverTargets(
    const BrandLifecyclePlanCatalogDTO& catalog,
    const BrandLifecycleObservationDTO& fresh_observation);

}  // namespace manyforge::core
