#include "manyforge_component_manager/manager_planned_maintenance_coordinator.hpp"

#include <utility>

namespace manyforge::component_manager {
namespace {

[[nodiscard]] ManagerPlannedMaintenanceResult RefusedBeforeEffect(
    std::string code, std::string message) {
  ManagerPlannedMaintenanceResult result;
  result.outcome = ManagerPlannedMaintenanceOutcome::kFailedBeforeEffect;
  result.failure_code = std::move(code);
  result.failure_message = std::move(message);
  // No runtime lifecycle effect was issued, so this attempt is known.
  result.effect_disposition = ManagerApplyEffectDisposition::kKnown;
  return result;
}

} // namespace

const char *ToString(const ManagementIdleState state) noexcept {
  switch (state) {
  case ManagementIdleState::kIdle:
    return "idle";
  case ManagementIdleState::kStarting:
    return "starting";
  case ManagementIdleState::kActive:
    return "active";
  case ManagementIdleState::kPaused:
    return "paused";
  case ManagementIdleState::kStopping:
    return "stopping";
  case ManagementIdleState::kTerminalAwaitingReset:
    return "terminal_awaiting_reset";
  case ManagementIdleState::kInterventionOrRecovery:
    return "intervention_or_recovery";
  case ManagementIdleState::kResetting:
    return "resetting";
  case ManagementIdleState::kRecoveryRequired:
    return "recovery_required";
  case ManagementIdleState::kUnavailable:
    return "unavailable";
  }
  return "unknown";
}

bool ManagerPlannedMaintenanceResult::valid() const noexcept {
  if (entered_idle_maintenance()) {
    return failure_code.empty() && failure_message.empty() &&
           effect_disposition == ManagerApplyEffectDisposition::kKnown;
  }
  return !failure_code.empty() && !failure_message.empty() &&
         effect_disposition ==
             (outcome == ManagerPlannedMaintenanceOutcome::kFailedBeforeEffect
                  ? ManagerApplyEffectDisposition::kKnown
                  : ManagerApplyEffectDisposition::kUnknown);
}

ManagerPlannedMaintenanceCoordinator::ManagerPlannedMaintenanceCoordinator(
    const IManagerActiveRuntimePlanSource &plan_source,
    IPlannedMaintenanceGate &gate)
    : plan_source_(plan_source), gate_(gate) {}

std::optional<ManagerPlannedMaintenanceResult>
ManagerPlannedMaintenanceCoordinator::Preflight(
    const ManagerApplyIntentDTO &intent,
    const manyforge::core::ProjectionRevisionDTO &expected_predecessor) const {
  if (!intent.valid()) {
    return RefusedBeforeEffect(
        "planned_maintenance_intent_invalid",
        "planned reconfiguration has no valid durable operation intent");
  }
  if (!(intent.expected_active_projection_revision == expected_predecessor)) {
    return RefusedBeforeEffect(
        "planned_maintenance_predecessor_mismatch",
        "planned reconfiguration predecessor is not the accepted intent");
  }

  // Step 2: refuse before any Apply effect unless the affected surface is
  // exactly idle. A planned configuration transition must not overlap motion,
  // a paused run, an in-progress stop or a reset.
  switch (gate_.ReadIdleState()) {
  case ManagementIdleState::kIdle:
    break;
  case ManagementIdleState::kStarting:
    return RefusedBeforeEffect(
        "apply_refused_starting_execution",
        "planned reconfiguration refuses an execution that is starting");
  case ManagementIdleState::kActive:
    return RefusedBeforeEffect(
        "apply_refused_active_execution",
        "planned reconfiguration refuses active execution");
  case ManagementIdleState::kPaused:
    return RefusedBeforeEffect(
        "apply_refused_paused_execution",
        "planned reconfiguration refuses a paused execution");
  case ManagementIdleState::kStopping:
    return RefusedBeforeEffect(
        "apply_refused_stopping_execution",
        "planned reconfiguration refuses an execution that is stopping");
  case ManagementIdleState::kTerminalAwaitingReset:
    return RefusedBeforeEffect(
        "apply_refused_terminal_awaiting_reset",
        "planned reconfiguration requires the completed execution to be reset");
  case ManagementIdleState::kInterventionOrRecovery:
    return RefusedBeforeEffect(
        "apply_refused_intervention_or_recovery",
        "planned reconfiguration refuses an intervention or recovery state");
  case ManagementIdleState::kResetting:
    return RefusedBeforeEffect(
        "apply_refused_resetting_execution",
        "planned reconfiguration refuses an execution that is resetting");
  case ManagementIdleState::kRecoveryRequired:
    return RefusedBeforeEffect(
        "apply_refused_recovery_required",
        "planned reconfiguration refuses while execution recovery is required");
  case ManagementIdleState::kUnavailable:
    return RefusedBeforeEffect(
        "apply_refused_cycle_status_unavailable",
        "planned reconfiguration could not establish current execution state");
  }

  // Step 3: recheck that the original inputs and revisions are still current.
  // A stale predecessor must not be used to select the transition.
  try {
    const auto plan = plan_source_.ReadCurrentPlan();
    if (!plan.valid() ||
        !(plan.active_publication.request.candidate_projection_revision ==
          expected_predecessor)) {
      return RefusedBeforeEffect(
          "planned_maintenance_predecessor_stale",
          "active configuration changed before planned reconfiguration");
    }
  } catch (const std::exception &error) {
    return RefusedBeforeEffect(
        "planned_maintenance_plan_unavailable",
        std::string("active configuration could not be rechecked: ") +
            error.what());
  } catch (...) {
    return RefusedBeforeEffect("planned_maintenance_plan_unavailable",
                               "active configuration could not be rechecked");
  }
  return std::nullopt;
}

ManagerPlannedMaintenanceResult
ManagerPlannedMaintenanceCoordinator::EnterIdleMaintenance(
    const ManagerApplyIntentDTO &intent,
    const manyforge::core::ProjectionRevisionDTO &expected_predecessor) {
  // Repeat the observation-only check after the outer owner acquired its
  // exact admission. This closes the race between early refusal and teardown.
  if (auto refusal = Preflight(intent, expected_predecessor);
      refusal.has_value()) {
    return *refusal;
  }

  // Step 4: close admission and prove it is closed before runtime convergence.
  if (!gate_.CloseAdmissionAndProveClosed()) {
    return RefusedBeforeEffect(
        "planned_maintenance_admission_not_closed",
        "planned reconfiguration could not prove admission closed");
  }

  // Keep the Manager-owned host process and all unaffected component owners
  // alive. The accepted component delta is applied by the runtime convergence
  // owner under this still-held Manager Apply admission.
  ManagerPlannedMaintenanceResult result;
  result.outcome =
      ManagerPlannedMaintenanceOutcome::kEnteredIdleMaintenance;
  return result;
}

} // namespace manyforge::component_manager
