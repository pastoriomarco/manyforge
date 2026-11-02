#pragma once

#include <manyforge_core/types/cell_admission.hpp>

#include <cstdint>

namespace manyforge::headless {

struct ManagerCellAdmissionReadinessFacts final {
  bool program_cycle_status_current{};
  manyforge::core::ProgramCycleStatusDTO program_cycle_status;
  bool required_component_status_current{};
  std::uint32_t required_component_total{};
  std::uint32_t required_component_ready{};
  bool deployment_status_current{};
  bool deployment_converged{};
  std::uint32_t blocking_fault_count{};
  std::uint32_t unresolved_owner_count{};
  std::uint32_t unsettled_effect_count{};
};

[[nodiscard]] inline bool IsCurrentProgramRunnerOwner(
    const manyforge::core::ProgramCycleStatusDTO &status,
    const manyforge::core::RuntimeEpoch &runtime_epoch) {
  return status.program_runner_owner_epoch.value() == runtime_epoch.value();
}

/// Attribute one failed contact to a previously qualified active cycle.
/// Does not classify readiness, liveness expiry, or physical settlement.
[[nodiscard]] inline std::optional<manyforge::core::ProgramRunnerContactLossDTO>
MakeProgramRunnerContactLoss(
    const std::optional<manyforge::core::ProgramCycleStatusDTO>
        &established_cycle,
    const manyforge::core::RequestOwnerEpoch &manager_owner_epoch,
    const manyforge::core::RuntimeEpoch &manager_runtime_epoch,
    const std::string_view predicate, const std::string_view message) {
  if (!established_cycle)
    return std::nullopt;
  manyforge::core::ProgramRunnerContactLossDTO loss{
      manager_owner_epoch, manager_runtime_epoch, *established_cycle,
      std::string(predicate), std::string(message.substr(0U, 1024U))};
  if (!manyforge::core::ValidateProgramRunnerContactLoss(loss).ok()) {
    loss.message = "Program Runner contact failed";
    if (!manyforge::core::ValidateProgramRunnerContactLoss(loss).ok())
      return std::nullopt;
  }
  return loss;
}

[[nodiscard]] inline manyforge::core::CellAdmissionReadinessSnapshotDTO
ReduceManagerCellAdmissionReadiness(
    const ManagerCellAdmissionReadinessFacts &facts) {
  return {
      facts.program_cycle_status_current,
      facts.program_cycle_status,
      facts.required_component_status_current,
      facts.required_component_total,
      facts.required_component_ready,
      facts.deployment_status_current,
      facts.deployment_converged,
      facts.blocking_fault_count,
      facts.unresolved_owner_count,
      facts.unsettled_effect_count,
  };
}

// Idle, Starting and Running may be observed at different publication points.
// Forward progress does not revoke an already-held Program parent. Keep the
// previous current facts during this renewal; Kernel still checks the exact
// parent/capture and current node Stop fence before admitting each child.
[[nodiscard]] inline bool IsSameProgramAdmissionProgress(
    const manyforge::core::CellAdmissionReadinessSnapshotDTO &before,
    const manyforge::core::CellAdmissionReadinessSnapshotDTO &after) {
  using manyforge::core::ProgramCycleStateDTO;
  if (!before.program_cycle_status_current ||
      !after.program_cycle_status_current || !before.program_cycle_status ||
      !after.program_cycle_status) {
    return false;
  }
  const auto &old_cycle = *before.program_cycle_status;
  const auto &new_cycle = *after.program_cycle_status;
  if ((new_cycle.state != ProgramCycleStateDTO::kStarting &&
       new_cycle.state != ProgramCycleStateDTO::kRunning) ||
      old_cycle.program_runner_owner_epoch !=
          new_cycle.program_runner_owner_epoch ||
      old_cycle.cycle_generation != new_cycle.cycle_generation ||
      !new_cycle.program_run_id || old_cycle.terminal_or_settlement_reference ||
      new_cycle.terminal_or_settlement_reference) {
    return false;
  }
  if (old_cycle.state == ProgramCycleStateDTO::kIdleAfterReset) {
    if (old_cycle.program_run_id || old_cycle.cycle_claim_generation ||
        (new_cycle.state == ProgramCycleStateDTO::kRunning &&
         !new_cycle.cycle_claim_generation)) {
      return false;
    }
  } else if (old_cycle.state != ProgramCycleStateDTO::kStarting ||
             old_cycle.program_run_id != new_cycle.program_run_id ||
             !new_cycle.cycle_claim_generation ||
             (new_cycle.state == ProgramCycleStateDTO::kStarting &&
              old_cycle.cycle_claim_generation) ||
             (old_cycle.cycle_claim_generation &&
              old_cycle.cycle_claim_generation !=
                  new_cycle.cycle_claim_generation)) {
    return false;
  }
  auto comparable = before;
  comparable.program_cycle_status = after.program_cycle_status;
  return comparable == after;
}

} // namespace manyforge::headless
