#pragma once

#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

/// Safe idle state of the execution surface affected by a planned
/// reconfiguration.
///
/// This is deliberately a closed enumeration rather than a boolean: an
/// operator-facing refusal must name the exact reason, and `paused`, `stopping`
/// and `resetting` are each distinct from ordinary active execution.
enum class ManagementIdleState {
  kIdle,
  kStarting,
  kActive,
  kPaused,
  kStopping,
  kTerminalAwaitingReset,
  kInterventionOrRecovery,
  kResetting,
  kRecoveryRequired,
  kUnavailable,
};

[[nodiscard]] const char *ToString(ManagementIdleState state) noexcept;

/// The two effects a planned transition needs from its admission owner.
///
/// Implementations must read truth and must not start, stop, quiesce or resume
/// any process: steady observation never mutates lifecycle.
class IPlannedMaintenanceGate {
public:
  virtual ~IPlannedMaintenanceGate() = default;

  /// Read the exact idle state of the affected execution surface.
  [[nodiscard]] virtual ManagementIdleState ReadIdleState() const = 0;

  /// Close admission for new work and prove it is closed. Returns false when
  /// closure could not be established, in which case the caller must not
  /// destroy anything.
  [[nodiscard]] virtual bool CloseAdmissionAndProveClosed() = 0;
};

/// How one planned transition attempt ended.
enum class ManagerPlannedMaintenanceOutcome {
  /// Admission is closed and every affected Manager-owned runtime content was
  /// stopped with its exact absence proven. Replacement construction is the
  /// caller's next step.
  kEnteredIdleMaintenance,
  /// Refused before any process effect. Every effect of this attempt is
  /// therefore known: nothing happened.
  kFailedBeforeEffect,
  /// A process effect began and at least one effect could not be established.
  kUnknownEffect,
};

/// Truthful, bounded result of one planned transition attempt.
///
/// No mutable phase is carried here: the durable milestone set in the existing
/// Apply journal remains the only phase record, and the caller derives it.
struct ManagerPlannedMaintenanceResult final {
  ManagerPlannedMaintenanceOutcome outcome{
      ManagerPlannedMaintenanceOutcome::kFailedBeforeEffect};
  /// Non-empty exactly for a failed or unknown-effect attempt.
  std::string failure_code;
  std::string failure_message;
  /// Meaningful exactly for a failed or unknown-effect attempt.
  ManagerApplyEffectDisposition effect_disposition{
      ManagerApplyEffectDisposition::kKnown};
  /// Exact absence evidence for every affected content, in reverse dependency
  /// order. Populated only on a successful entry.
  std::vector<ProcessPresenceObservation> final_process_observations;

  [[nodiscard]] bool entered_idle_maintenance() const noexcept {
    return outcome == ManagerPlannedMaintenanceOutcome::kEnteredIdleMaintenance;
  }

  /// A terminal failure that must be retained exactly once.
  [[nodiscard]] bool terminal_failure() const noexcept {
    return outcome != ManagerPlannedMaintenanceOutcome::kEnteredIdleMaintenance;
  }

  [[nodiscard]] bool valid() const noexcept;
};

/// The one planned idle-reconfiguration transition of the Manager.
///
/// This coordinator adds no state machine and no second journal. It makes the
/// ordering that a planned reconfiguration requires explicit, and it decides
/// the one thing a caller must not guess: whether a failure left effects that
/// are established or unknown.
///
/// Ordering, each step complete before the next begins:
///   1. the caller validated the exact Deployment candidate and its compatible
///      prepared Program binding, holds the configuration fence, and has the
///      durable intent;
///   2. this method refuses unless the affected surface is exactly idle, with a
///      distinct typed refusal for active, paused, stopping and resetting;
///   3. the active plan is rechecked against the exact expected predecessor, so
///      a stale revision or identity is never destroyed;
///   4. admission is closed and proven closed;
///   5. affected Manager-owned runtime contents are stopped through the
///      existing reverse-dependency, prove-absence owner.
///
/// Replacement construction deliberately happens after this method returns, and
/// only through `ManagerActiveRuntimeCoordinator::Initialize()`. Unexpected
/// runtime loss remains the separate catastrophic-fault path: this class never
/// requests containment, reconciliation, retry, rollback or replacement.
class ManagerPlannedMaintenanceCoordinator final {
public:
  ManagerPlannedMaintenanceCoordinator(
      const IManagerActiveRuntimePlanSource &plan_source,
      ManagerActiveRuntimeCoordinator &active_runtime,
      IPlannedMaintenanceGate &gate);

  /// Observation-only refusal check used before the Apply acquires its
  /// configuration fence or changes a process. A parent authoring claim may
  /// already be held to serialize submission, but a successful result still
  /// authorizes no effect; EnterIdleMaintenance repeats these checks after
  /// admission closes.
  [[nodiscard]] std::optional<ManagerPlannedMaintenanceResult> Preflight(
      const ManagerApplyIntentDTO &intent,
      const manyforge::core::ProjectionRevisionDTO &expected_predecessor) const;

  [[nodiscard]] ManagerPlannedMaintenanceResult EnterIdleMaintenance(
      const ManagerApplyIntentDTO &intent,
      const manyforge::core::ProjectionRevisionDTO &expected_predecessor);

  ManagerPlannedMaintenanceCoordinator(
      const ManagerPlannedMaintenanceCoordinator &) = delete;
  ManagerPlannedMaintenanceCoordinator &
  operator=(const ManagerPlannedMaintenanceCoordinator &) = delete;

private:
  const IManagerActiveRuntimePlanSource &plan_source_;
  ManagerActiveRuntimeCoordinator &active_runtime_;
  IPlannedMaintenanceGate &gate_;
};

} // namespace manyforge::component_manager
