#pragma once

#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/types/lifecycle_target.hpp>
#include <manyforge_core/ports/icclc_lifecycle_target_store.hpp>
#include <manyforge_core/configured_robot_measured_state.hpp>
#include <functional>

namespace manyforge::core {

/// A transient projection from one current product pin. It grants neither
/// lifecycle admission nor dispatch permission and retains no sample history.
struct ComponentLifecycleDomainReadinessDTO final {
  ResolvedLifecycleProviderOccurrenceDTO provider_occurrence;
  std::optional<LifecycleDomainHealthDTO> health;
  std::optional<LifecycleDomainPlanDecisionKind> decision_kind;
  std::optional<ErrorDTO> error;
  /// Exact producer presentation metadata; it grants no lifecycle admission.
  std::optional<bool> has_activation_step{};
};

struct ComponentMeasuredStateReadinessDTO final {
  std::optional<bool> required;
  std::optional<bool> available;
  std::optional<ErrorDTO> error;
};

struct ComponentLifecycleReadinessDTO final {
  RequestOwnerEpoch request_owner_epoch;
  std::int64_t evaluated_at_ns;
  std::optional<ComponentLifecycleIntentKindDTO> next_intent;
  std::optional<ComponentLifecycleStateDTO> requested_state;
  bool ready;
  std::optional<ErrorDTO> error;
  std::vector<ComponentLifecycleDomainReadinessDTO> domains;
  ComponentMeasuredStateReadinessDTO measured_state;
};

/// Existing driver/journal facts plus any still-unrecorded owner blocker.
/// A read does not retire a target, clear a cause or publish progress.
struct ComponentLifecycleReadinessTargetFactsDTO final {
  ComponentLifecycleTargetFactsDTO facts;
  std::optional<ErrorDTO> blocker;
  friend bool operator==(const ComponentLifecycleReadinessTargetFactsDTO&,
                         const ComponentLifecycleReadinessTargetFactsDTO&) = default;
};

/// Operation-local leases from the existing route publisher. The validator
/// rechecks that exact captured association under its original route fence.
/// This is neither another inventory nor a source-selection authority.
struct ComponentLifecycleReadinessSourceView final {
  std::optional<RobotMeasuredStateSourceView> measured_sources;
  std::function<Result<void>()> validate_current;
};

struct CCLCComponentReadDTO final {
  ComponentLifecycleCurrentSnapshotDTO snapshot;
  ComponentLifecycleTargetViewDTO target_view;
  ComponentLifecycleReadinessDTO readiness;
};

}  // namespace manyforge::core
