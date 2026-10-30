#pragma once

#include <array>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>

namespace manyforge::core {

/// The adapter's admission answer for one transition on current status
/// (V2-1). Already done = `postcondition` satisfied; ready = not satisfied
/// and `not_ready` empty; not ready = `not_ready` holds the typed reason.
struct LifecycleAdmissionDTO final {
  LifecyclePostconditionResultDTO postcondition;
  std::optional<ErrorDTO> not_ready;
  /// Adapter-internal: the substep a ready answer selects, which the
  /// transition runner dispatches next. CCLC never reads it.
  std::optional<LifecyclePrimitiveTransitionDTO> selected_substep{};

  friend bool operator==(const LifecycleAdmissionDTO&, const LifecycleAdmissionDTO&) = default;
};

/// Pure admission query of one adapter: no I/O, no observation bookkeeping.
/// An error is an invalid input; every refusal of a valid input is
/// `not_ready`. Each caller keeps its own wait/refuse policy.
class ILifecycleAdmission {
 public:
  virtual ~ILifecycleAdmission() = default;

  [[nodiscard]] virtual Result<LifecycleAdmissionDTO> Evaluate(
      ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
      const BrandLifecycleObservationDTO& observation) const = 0;

  [[nodiscard]] virtual Result<LifecycleAdmissionDTO> Evaluate(
      ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
      const LifecycleDomainHealthDTO& health) const = 0;

  /// Each Recover target's eligibility, in `kLifecycleRecoverTargets` order;
  /// a target without a plan is not satisfied.
  [[nodiscard]] virtual Result<std::array<LifecyclePostconditionResultDTO, 3>> RecoverTargets(
      const BrandLifecycleObservationDTO& observation) const = 0;
};

/// The default helper every current adapter uses: the shared plan evaluator
/// (`EvaluateLifecycleDomainPlan`, including the unarmed-resting Disable rule)
/// and the Recover-eligibility predicates over the adapter's own catalog.
class CatalogLifecycleAdmission final : public ILifecycleAdmission {
 public:
  explicit CatalogLifecycleAdmission(BrandLifecyclePlanCatalogDTO catalog);

  [[nodiscard]] Result<LifecycleAdmissionDTO> Evaluate(
      ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
      const BrandLifecycleObservationDTO& observation) const override;
  [[nodiscard]] Result<LifecycleAdmissionDTO> Evaluate(
      ComponentLifecycleIntentKindDTO intent, ComponentLifecycleStateDTO requested_state,
      const LifecycleDomainHealthDTO& health) const override;
  [[nodiscard]] Result<std::array<LifecyclePostconditionResultDTO, 3>> RecoverTargets(
      const BrandLifecycleObservationDTO& observation) const override;

  [[nodiscard]] const BrandLifecyclePlanCatalogDTO& catalog() const noexcept { return catalog_; }

 private:
  BrandLifecyclePlanCatalogDTO catalog_;
};

}  // namespace manyforge::core
