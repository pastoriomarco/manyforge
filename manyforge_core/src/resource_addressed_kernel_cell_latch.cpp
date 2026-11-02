#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {

// Cell latch (CONTRACTS §5, plan R1-R3). A latch condition is armed when a
// Stop is accepted, or when a component turns `fault`/`offline` while anything
// executes. While armed, the cell is latched exactly when some component still
// in the cell is in `fault` or `offline`, or reports `activity: unknown` (a
// member shown offline through its owner is its owner's cause). The
// arm clears when every component in the cell is healthy and idle. A
// component offline by the operator's own power action, or a member offline
// through a not-prepared owner, is outside the latch (`latch_exempt`).
// Nothing is stored or journaled; every change recomputes it.

void ResourceAddressedExecutionKernel::observe_component_states(
    std::vector<ComponentState> states) {
  std::vector<std::shared_ptr<IIoSignalService>> resumed;
  {
    std::lock_guard state_lock(state_mutex_);
    const bool was_latched = cell_latched_locked();
    observe_component_states_locked(std::move(states));
    // R1, §8: interrupt evaluation resumes when the latch ends. A rule whose
    // condition became true meanwhile trips now (the level, not a lost edge),
    // without waiting for its next sample.
    if (was_latched && !cell_latched_locked()) {
      if (io_signal_service_) resumed.push_back(io_signal_service_);
      if (global_io_rule_service_ && global_io_rule_service_ != io_signal_service_)
        resumed.push_back(global_io_rule_service_);
    }
  }
  for (const auto& service : resumed) (void)notify_io_rule_evidence_processing(service);
}

void ResourceAddressedExecutionKernel::observe_component_states_locked(
    std::vector<ComponentState> states) {
  bool arm = false;
  if (execution_in_progress_locked()) {
    for (const auto& state : states) {
      if (component_healthy(state) || latch_exempt(state, states)) continue;
      const auto previous =
          std::find_if(component_states_.begin(), component_states_.end(),
                       [&](const auto& row) { return row.component_id == state.component_id; });
      if (previous == component_states_.end() || component_healthy(*previous)) arm = true;
    }
  }
  // CONTRACTS §7: a component that becomes `prepared` starts its outputs'
  // core-held `changed_outside` signals false.
  if (io_signal_service_) {
    for (const auto& state : states) {
      if (state.lifecycle != ComponentLifecycle::kPrepared) continue;
      const auto previous =
          std::find_if(component_states_.begin(), component_states_.end(),
                       [&](const auto& row) { return row.component_id == state.component_id; });
      if (previous != component_states_.end() &&
          previous->lifecycle == ComponentLifecycle::kPrepared)
        continue;
      (void)io_signal_service_->ClearComponentOutsideChanges(
          ControllableComponentId(state.component_id));
    }
  }
  component_states_ = std::move(states);
  refresh_cell_latch_locked(arm);
  // A restart's unclosed predecessor ends here: every component reports
  // healthy and idle, so its orphan resources are free again (R2).
  if (motion_admission_unreconciled_ && cell_settled_locked()) {
    const auto snapshot = resource_slots_->coherent_snapshot();
    std::vector<ResourceId> orphans;
    for (const auto& slot : snapshot.slots)
      if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal && !slot.owner &&
          std::binary_search(recovered_orphan_resources_.begin(),
                             recovered_orphan_resources_.end(), slot.resource_id))
        orphans.push_back(slot.resource_id);
    std::sort(orphans.begin(), orphans.end());
    if (!orphans.empty())
      (void)resource_slots_->reconcile_unreconciled_external_or_orphan_goal(
          snapshot.operation_resource_generation, orphans);
    recovered_orphan_resources_.clear();
    motion_admission_unreconciled_ = false;
  }
}

void ResourceAddressedExecutionKernel::set_stop_listener(std::function<void()> on_stop_accepted) {
  std::lock_guard state_lock(state_mutex_);
  stop_listener_ = std::move(on_stop_accepted);
}

std::vector<ErrorDTO> ResourceAddressedExecutionKernel::ReadExecutionBlocks() const {
  std::lock_guard state_lock(state_mutex_);
  std::vector<ErrorDTO> blocks;
  if (internal_fault_)
    blocks.push_back({"execution_host_internal_fault",
                      "The execution host had an internal fault; restart it to run motion again",
                      {{"reason", internal_fault_->reason_code}}});
  if (manager_liveness_motion_inhibited_)
    blocks.push_back({"manager_unavailable",
                      "The cell Manager isn't answering; motion waits until it is back",
                      {}});
  // A restart's unclosed predecessor with no latch cause: before the first
  // component report one row for the restart, then one row per component
  // still busy, never blocking invisibly (R4).
  if (motion_admission_unreconciled_ && component_states_.empty())
    blocks.push_back({"execution_host_restart_awaiting_components",
                      "After the execution host restart, motion waits for the first "
                      "component report",
                      {}});
  if (motion_admission_unreconciled_ && !cell_latched_locked())
    for (const auto& state : component_states_)
      if (!latch_exempt(state, component_states_) && state.activity != ComponentActivity::kIdle)
        blocks.push_back({"execution_host_restart_settling",
                          "After the execution host restart, motion waits until " +
                              state.component_id + " is idle",
                          {{"component_id", state.component_id}}});
  return blocks;
}

CellLatch ResourceAddressedExecutionKernel::get_cell_latch() const {
  std::lock_guard state_lock(state_mutex_);
  if (!cell_latch_armed_ && !motion_admission_unreconciled_) return {};
  auto causes = cell_latch_causes(component_states_);
  const bool latched = !causes.empty();
  return CellLatch{latched, std::move(causes)};
}

bool ResourceAddressedExecutionKernel::cell_settled_locked() const {
  return std::all_of(component_states_.begin(), component_states_.end(), [&](const auto& state) {
    return latch_exempt(state, component_states_) ||
           (component_healthy(state) && state.activity == ComponentActivity::kIdle);
  });
}

void ResourceAddressedExecutionKernel::refresh_cell_latch_locked(const bool arm) {
  if (arm) cell_latch_armed_ = true;
  if (cell_latch_armed_ && cell_settled_locked()) cell_latch_armed_ = false;
}

bool ResourceAddressedExecutionKernel::cell_latched_locked() const {
  // The same causes `get_cell_latch` lists: a member offline through its
  // owner counts as its owner (CONTRACTS §5, §16).
  // A restart's unclosed predecessor counts as a Stop until it ends.
  return (cell_latch_armed_ || motion_admission_unreconciled_) &&
         !cell_latch_causes(component_states_).empty();
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::cell_latch_refusal_locked() const {
  if (!cell_latched_locked()) return std::nullopt;
  return error("cell_latched",
               "A component needs attention; execution waits until it is healthy and idle",
               "cell_latched");
}

bool ResourceAddressedExecutionKernel::execution_in_progress_locked() const {
  return program_cycle_claim_run_.has_value() || !immediate_step_engines_.empty() ||
         std::any_of(active_operations_.begin(), active_operations_.end(), [](const auto& row) {
           return !terminal(row.second->state.state);
         });
}

}  // namespace manyforge::core
