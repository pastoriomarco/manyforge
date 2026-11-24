#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {

// Cell latch. A latch condition is armed when a
// Stop is accepted, or when a component becomes unhealthy while anything
// executes. While armed, the cell is latched when a non-exempt component has
// a stop, is in `fault` or `offline`, or reports `activity: unknown` (a
// member shown offline through its owner is its owner's cause). The
// arm clears when every component in the cell is healthy and idle. A
// component offline by the operator's own power action, or a member offline
// through a not-prepared owner, is outside the latch (`latch_exempt`).
// Nothing is stored or journaled; every change recomputes it.
//
// A component whose last Stop ended uncontained (stop state model §3.6 F2)
// counts as `activity: unknown` with reason `stop_unconfirmed` until a state
// of it arriving after the failure reports idle; the existing latch then
// holds by itself (R1) and clears by itself (R2). No new latch exists.
//
// OD-379: a component that enters `fault` while an operation executes also
// stops every robot in motion, in the same locked step that arms the latch:
// one Kernel-internal `component_fault` Stop of the whole cell, each robot
// with its fault Stop profile (as the runtime monitor's containment). A
// component stop (e-stop) or `offline` only latches, and a simulation that
// is not playing is neither (`simulation_not_playing` is no fault).

void ResourceAddressedExecutionKernel::observe_component_states(
    std::vector<ComponentState> states) {
  std::vector<std::shared_ptr<IIoSignalService>> resumed;
  // Obtained before the Kernel lock (it may allocate on this thread's first
  // Stop); completed after the unlock.
  StopHandoff& handoff = thread_stop_handoff();
  {
    std::lock_guard state_lock(state_mutex_);
    const bool was_latched = cell_latched_locked();
    if (const auto fault = observe_component_states_locked(std::move(states)))
      request_component_fault_stop_locked(*fault, handoff);
    // Interrupt evaluation resumes when the latch ends. A rule whose
    // condition became true meanwhile trips now (the level, not a lost edge),
    // without waiting for its next sample.
    if (was_latched && !cell_latched_locked()) {
      if (io_signal_service_) resumed.push_back(io_signal_service_);
      if (global_io_rule_service_ && global_io_rule_service_ != io_signal_service_)
        resumed.push_back(global_io_rule_service_);
    }
  }
  complete_stop_handoff(handoff);
  for (const auto& service : resumed) (void)notify_io_rule_evidence_processing(service);
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::observe_component_states_locked(
    std::vector<ComponentState> states) {
  bool arm = false;
  // OD-379: the components that entered `fault` in this update while an
  // operation executes (once per fault episode of each component).
  std::vector<const ComponentState*> entered_fault;
  if (execution_in_progress_locked()) {
    for (const auto& state : states) {
      if (component_healthy(state) || latch_exempt(state, states)) continue;
      const auto previous =
          std::find_if(component_states_.begin(), component_states_.end(),
                       [&](const auto& row) { return row.component_id == state.component_id; });
      if (previous == component_states_.end() || component_healthy(*previous)) arm = true;
      if (state.lifecycle == ComponentLifecycle::kFault &&
          (previous == component_states_.end() ||
           previous->lifecycle != ComponentLifecycle::kFault))
        entered_fault.push_back(&state);
    }
  }
  std::optional<ErrorDTO> fault_stop;
  // Only while an operation executes: a run between nodes has nothing to
  // stop (the latch refuses its next node).
  if (!entered_fault.empty() &&
      std::any_of(active_operations_.begin(), active_operations_.end(),
                  [](const auto& row) { return !terminal(row.second->state.state); })) {
    // The typed cause each stopped participant keeps (built before the
    // states move below).
    const auto& first = *entered_fault.front();
    std::string components;
    for (const auto* state : entered_fault)
      components += (components.empty() ? "" : ", ") + state->component_id;
    fault_stop = ErrorDTO{std::string(error_codes::kComponentFault),
                          "Component " + components +
                              " entered a fault; motion stopped with its fault Stop profile",
                          {{"reason", std::string(error_codes::kComponentFault)},
                           {"component_id", first.component_id}}};
    if (first.fault) {
      fault_stop->details.emplace("fault_code", std::string(to_string(first.fault->code)));
      fault_stop->details.emplace("fault_message", first.fault->message);
    }
  }
  // A component that becomes `prepared` starts its outputs'
  // core-held `changed_outside` signals false.
  if (io_signal_service_) {
    for (const auto& state : states) {
      if (state.lifecycle != ComponentLifecycle::kPrepared) continue;
      // The stopped-owner presentation shows members as Prepared/Disabled;
      // it is not a Prepare action and must not reset output change flags.
      if (state.owner_id && state.reason &&
          state.reason->code == kStoppedByEstopReason &&
          std::any_of(states.begin(), states.end(), [&](const auto& owner) {
            return owner.component_id == *state.owner_id && owner.stop.has_value();
          })) continue;
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
  // F2 clear: only the controller's own idle report clears "stop
  // unconfirmed" (Reset and Recover do not). Ordered by arrival under this
  // lock: the first idle state in a component-state update that arrives after
  // the failure was recorded clears it. Component states are stamped by the
  // host's component runtime clock (`WallClock`, roboplan
  // `ros_managed_runtime_host.cpp:7280`), not necessarily the Kernel's clock
  // (`clock_`, a FakeClock in tests), so no times are compared across clocks.
  if (stop_unconfirmed_.table) {
    for (const auto& state : component_states_) {
      if (state.activity != ComponentActivity::kIdle) continue;
      const auto index = stop_component_index_locked(ControllableComponentId(state.component_id));
      if (index && stop_unconfirmed_.marked[*index]) stop_unconfirmed_.marked[*index] = 0U;
    }
  }
  refresh_cell_latch_locked(arm);
  return fault_stop;
}

void ResourceAddressedExecutionKernel::request_component_fault_stop_locked(
    const ErrorDTO& cause, StopHandoff& handoff) {
  // One Stop of the whole cell through the one Stop core: every executing
  // robot's participant starts its fault Stop profile; one already stopping
  // is covered, not restarted (its Stop keeps its own cause). Never refused.
  StopRequest request;
  request.request_key = mint_kernel_request_key();
  request.kind = StopKind::kComponentFault;
  request.selector = StopSelectorKind::kCell;
  request.source_label = "kernel.component_fault";
  request.reason = error_codes::kComponentFault;
  request.attribution_rank = StopAttribution{StopAttributionClass::kOperator, 0, 0U};
  request.mode = StopMode::kFault;
  const auto record = request_stop_locked(request, handoff);
  const StopEventId event_id = stop_events_.records[record].event_id;
  // The participants this event started keep the typed cause, as the
  // runtime monitor's (their Stop lanes run only after the unlock).
  for (auto& [id, operation] : active_operations_) {
    static_cast<void>(id);
    for (auto& participant : operation->participants) {
      if (participant.stop.started_by != event_id) continue;
      participant.runtime_collision_error = cause;
      participant.last_subordinate_error = cause;
    }
  }
}

void ResourceAddressedExecutionKernel::set_stop_listener(IStopListener* listener) noexcept {
  stop_listener_.store(listener, std::memory_order_release);
}

void ResourceAddressedExecutionKernel::rebuild_stop_component_table_locked() {
  auto table = std::make_shared<StopComponentTable>();
  table->reserve(stop_component_scope_.size());
  for (const auto& component : stop_component_scope_)
    table->push_back(component.controllable_component_id);
  std::sort(table->begin(), table->end());
  table->erase(std::unique(table->begin(), table->end()), table->end());
  // Keep the marks of components that stay in the cell.
  std::vector<std::uint8_t> marked(table->size(), 0U);
  if (stop_unconfirmed_.table) {
    for (std::size_t old_index = 0U; old_index < stop_unconfirmed_.table->size(); ++old_index) {
      if (!stop_unconfirmed_.marked[old_index]) continue;
      const auto& id = (*stop_unconfirmed_.table)[old_index];
      const auto found = std::lower_bound(table->begin(), table->end(), id);
      if (found == table->end() || *found != id) continue;
      marked[static_cast<std::size_t>(found - table->begin())] = 1U;
    }
  }
  std::shared_ptr<const StopComponentTable> shared = std::move(table);
  stop_unconfirmed_.table = shared;
  stop_unconfirmed_.marked = std::move(marked);
  stop_component_count_.store(shared->size(), std::memory_order_release);
  stop_component_table_ = std::move(shared);
}

std::optional<std::size_t> ResourceAddressedExecutionKernel::stop_component_index_locked(
    const ControllableComponentId& component) const noexcept {
  if (!stop_component_table_) return std::nullopt;
  const auto& table = *stop_component_table_;
  const auto found = std::lower_bound(table.begin(), table.end(), component);
  if (found == table.end() || *found != component) return std::nullopt;
  return static_cast<std::size_t>(found - table.begin());
}

bool ResourceAddressedExecutionKernel::stop_unconfirmed_locked(
    const std::string_view component_id) const noexcept {
  if (!stop_unconfirmed_.table) return false;
  const auto& table = *stop_unconfirmed_.table;
  const auto found = std::lower_bound(
      table.begin(), table.end(), component_id,
      [](const ControllableComponentId& row, std::string_view id) { return row.value() < id; });
  return found != table.end() && found->value() == component_id &&
         stop_unconfirmed_.marked[static_cast<std::size_t>(found - table.begin())] != 0U;
}

std::vector<ComponentState> ResourceAddressedExecutionKernel::latch_component_states_locked() const {
  auto states = component_states_;
  for (auto& state : states) {
    if (!stop_unconfirmed_locked(state.component_id)) continue;
    state.activity = ComponentActivity::kUnknown;
    state.reason = ComponentStateReason{
        std::string(kStopUnconfirmedReason),
        "Its last Stop did not confirm the robot at rest; activity is unknown until its "
        "controller reports idle"};
  }
  return states;
}

std::vector<ControllableComponentId> ResourceAddressedExecutionKernel::stop_unconfirmed_components()
    const {
  std::lock_guard state_lock(state_mutex_);
  std::vector<ControllableComponentId> components;
  if (!stop_unconfirmed_.table) return components;
  for (std::size_t index = 0U; index < stop_unconfirmed_.table->size(); ++index)
    if (stop_unconfirmed_.marked[index]) components.push_back((*stop_unconfirmed_.table)[index]);
  return components;
}

std::vector<ErrorDTO> ResourceAddressedExecutionKernel::ReadExecutionBlocks() const {
  std::lock_guard state_lock(state_mutex_);
  std::vector<ErrorDTO> blocks;
  if (internal_fault_)
    blocks.push_back({"execution_host_internal_fault",
                      "The execution host had an internal fault; restart ManyForge to run motion again",
                      {{"reason", internal_fault_->reason_code}}});
  return blocks;
}

CellLatch ResourceAddressedExecutionKernel::get_cell_latch() const {
  std::lock_guard state_lock(state_mutex_);
  if (!cell_latch_armed_) return {};
  auto causes = cell_latch_causes(latch_component_states_locked());
  const bool latched = !causes.empty();
  return CellLatch{latched, std::move(causes)};
}

bool ResourceAddressedExecutionKernel::cell_settled_locked() const {
  return std::all_of(component_states_.begin(), component_states_.end(), [&](const auto& state) {
    return latch_exempt(state, component_states_) ||
           (component_healthy(state) && state.activity == ComponentActivity::kIdle &&
            !stop_unconfirmed_locked(state.component_id));
  });
}

void ResourceAddressedExecutionKernel::refresh_cell_latch_locked(const bool arm) {
  if (arm) cell_latch_armed_ = true;
  if (cell_latch_armed_ && cell_settled_locked()) cell_latch_armed_ = false;
}

bool ResourceAddressedExecutionKernel::cell_latched_locked() const {
  // The same causes `get_cell_latch` lists: a member offline through its
  // owner counts as its owner. The stop-unconfirmed override needs a copy
  // only after a failed Stop marked a component; otherwise the stored states
  // are read as they are (a causes list is built only when latched).
  if (!cell_latch_armed_) return false;
  const bool marked = std::any_of(stop_unconfirmed_.marked.begin(), stop_unconfirmed_.marked.end(),
                                  [](const std::uint8_t mark) { return mark != 0U; });
  if (marked) return !cell_latch_causes(latch_component_states_locked()).empty();
  return !cell_latch_causes(component_states_).empty();
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
