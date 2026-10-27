#pragma once

#include <manyforge_core/component_stream_health.hpp>
#include <cstdint>
#include <limits>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::core {

/// Source label of every lifecycle request CCLC submits while following a
/// requested target (ADR-0026 §7 lifecycle-target amendment, 2026-10-08).
inline constexpr std::string_view kLifecycleTargetSourceLabel = "manyforge_lifecycle_target";

/// Requested lifecycle target of one component. It is distinct from the
/// achieved CCLC state, which only a verified transaction establishes.
enum class LifecycleTargetStateDTO : std::uint8_t {
  kActive = 0,
  kPrepared,
  kDisabled,
};

enum class LifecycleTargetOriginDTO : std::uint8_t {
  kStartup = 0,
  kOperator,
  kProgram,
};

enum class LifecycleProgressPhaseDTO : std::uint8_t {
  kSatisfied = 0,
  kWaitingForPrerequisites,
  kTransitionInProgress,
  kOutcomeUncertain,
  kBlocked,
  kCancelled,
};

[[nodiscard]] constexpr std::string_view to_string(const LifecycleTargetStateDTO value) noexcept {
  switch (value) {
    case LifecycleTargetStateDTO::kActive:
      return "active";
    case LifecycleTargetStateDTO::kPrepared:
      return "prepared";
    case LifecycleTargetStateDTO::kDisabled:
      return "disabled";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(const LifecycleTargetOriginDTO value) noexcept {
  switch (value) {
    case LifecycleTargetOriginDTO::kStartup:
      return "startup";
    case LifecycleTargetOriginDTO::kOperator:
      return "operator";
    case LifecycleTargetOriginDTO::kProgram:
      return "program";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(const LifecycleProgressPhaseDTO value) noexcept {
  switch (value) {
    case LifecycleProgressPhaseDTO::kSatisfied:
      return "satisfied";
    case LifecycleProgressPhaseDTO::kWaitingForPrerequisites:
      return "waiting_for_prerequisites";
    case LifecycleProgressPhaseDTO::kTransitionInProgress:
      return "transition_in_progress";
    case LifecycleProgressPhaseDTO::kOutcomeUncertain:
      return "outcome_uncertain";
    case LifecycleProgressPhaseDTO::kBlocked:
      return "blocked";
    case LifecycleProgressPhaseDTO::kCancelled:
      return "cancelled";
  }
  return {};
}

/// The lifecycle state an ordinary Prepare/Enable/Disable intent requests as a
/// target. Recover names no target: it only re-establishes a known state.
[[nodiscard]] constexpr std::optional<LifecycleTargetStateDTO> lifecycle_target_of(
    const ComponentLifecycleIntentKindDTO intent) noexcept {
  switch (intent) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
      return LifecycleTargetStateDTO::kPrepared;
    case ComponentLifecycleIntentKindDTO::kEnable:
      return LifecycleTargetStateDTO::kActive;
    case ComponentLifecycleIntentKindDTO::kDisable:
      return LifecycleTargetStateDTO::kDisabled;
    case ComponentLifecycleIntentKindDTO::kRecover:
      return std::nullopt;
  }
  return std::nullopt;
}

/// Owner-private actual target read. It is neither caller attribution nor a
/// public request/wire field; fresh follower acceptance and reached writes
/// compare it to the same durable target row inside their transaction.
struct ComponentLifecycleTargetExpectationDTO final {
  ControllableComponentId controllable_component_id;
  std::uint64_t target_generation{};

  [[nodiscard]] bool locally_valid() const noexcept {
    return !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
           target_generation != 0U &&
           target_generation <= static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max());
  }
  friend bool operator==(const ComponentLifecycleTargetExpectationDTO&,
                         const ComponentLifecycleTargetExpectationDTO&) = default;
};

/// Durable session-scoped requested target. `target_generation` increases
/// monotonically per component for the session, including across removal.
struct ComponentLifecycleTargetDTO final {
  ControllableComponentId controllable_component_id;
  LifecycleTargetStateDTO requested_state{LifecycleTargetStateDTO::kActive};
  LifecycleTargetOriginDTO origin{LifecycleTargetOriginDTO::kStartup};
  std::uint64_t target_generation{};
  /// The component was at its target on fresh status since startup (or a
  /// changed-component Apply) or since its last acknowledged incident. Only
  /// then is a loss or unexpected change an incident; before it, losing
  /// readiness is ordinary waiting. CCLC-internal; never serialized.
  bool reached{};
  /// Cause of the incident the operator's Recover closed, until the target is
  /// reached again. An adapter that keeps reporting that same cause (a sticky
  /// vendor state cleared only by the next recovery step) does not block the
  /// policy-permitted steps and opens no new incident; plan eligibility on
  /// fresh status still decides every step. CCLC-internal; never serialized.
  std::optional<LifecycleBlockingCauseClassDTO> recovered_cause{};

  [[nodiscard]] bool locally_valid() const noexcept {
    return !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
           !to_string(requested_state).empty() && !to_string(origin).empty() &&
           target_generation != 0U;
  }

  friend bool operator==(const ComponentLifecycleTargetDTO&,
                         const ComponentLifecycleTargetDTO&) = default;
};

/// Provenance of retirement of this exact retained target generation. This
/// owner-private row fact is not a new command/readiness authority.
enum class LifecycleTargetRetirementReasonDTO : std::uint8_t {
  kNone = 0,
  kOrdinaryCancellation,
  kClose,
};

struct ComponentLifecycleTargetRetentionDTO final {
  ComponentLifecycleTargetDTO target;
  bool present;
  LifecycleTargetRetirementReasonDTO retirement_reason;

  [[nodiscard]] bool locally_valid() const noexcept {
    if (!target.locally_valid()) return false;
    return present
        ? retirement_reason == LifecycleTargetRetirementReasonDTO::kNone
        : retirement_reason == LifecycleTargetRetirementReasonDTO::kOrdinaryCancellation ||
          retirement_reason == LifecycleTargetRetirementReasonDTO::kClose;
  }
  friend bool operator==(const ComponentLifecycleTargetRetentionDTO&,
                         const ComponentLifecycleTargetRetentionDTO&) = default;
};

/// Last intervention-required cause observed for one component domain. It
/// survives loss of communication and is cleared only by fresh status.
struct ComponentLifecycleInterventionCauseDTO final {
  ControllableComponentId controllable_component_id;
  ControlDomainId control_domain_id;
  LifecycleBlockingCauseClassDTO cause_class{LifecycleBlockingCauseClassDTO::kUnknown};
  std::int64_t observed_at_ns{};

  [[nodiscard]] bool locally_valid() const noexcept {
    return !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
           !control_domain_id.empty() &&
           control_domain_id.value().size() <= kMaxLifecycleIdentityBytes &&
           lifecycle_cause_requires_intervention(cause_class) && observed_at_ns >= 0;
  }

  friend bool operator==(const ComponentLifecycleInterventionCauseDTO&,
                         const ComponentLifecycleInterventionCauseDTO&) = default;
};

/// Owner-local presentation of progress toward the requested target. It is
/// never persisted and never authorizes an effect.
struct ComponentLifecycleProgressDTO final {
  LifecycleProgressPhaseDTO phase{LifecycleProgressPhaseDTO::kWaitingForPrerequisites};
  std::optional<ComponentLifecycleIntentKindDTO> next_intent;
  LifecycleBlockingCauseClassDTO blocking_cause_class{LifecycleBlockingCauseClassDTO::kNone};
  std::optional<std::string> blocking_reason;
  std::optional<std::int64_t> waiting_since_ns;
  bool delayed{};
  /// The open incident, from detection until the operator's acknowledgement
  /// is accepted; `phase` is then `blocked` with the incident's cause.
  std::optional<ComponentLifecycleIncidentDTO> incident{};

  friend bool operator==(const ComponentLifecycleProgressDTO&,
                         const ComponentLifecycleProgressDTO&) = default;
};

/// Requested target plus progress for one component; both absent when no
/// target is recorded or the driver is not composed.
struct ComponentLifecycleTargetViewDTO final {
  std::optional<ComponentLifecycleTargetDTO> target;
  std::optional<ComponentLifecycleProgressDTO> progress;
  /// Informational cumulative drops since this host started; host replacement
  /// resets them. Both absent when unavailable, with no lifecycle/health effect.
  std::optional<std::uint64_t> dropped_output_lines{};
  std::optional<std::uint64_t> dropped_output_bytes{};
  /// CP9 health of the component's declared streams; empty when none.
  std::vector<StreamHealthDTO> stream_health{};

  friend bool operator==(const ComponentLifecycleTargetViewDTO&,
                         const ComponentLifecycleTargetViewDTO&) = default;
};

}  // namespace manyforge::core
