#include <algorithm>
#include <manyforge_core/component_coordinator.hpp>

namespace manyforge::core {
namespace {

ErrorDTO action_error(std::string code, std::string message, const std::string& component_id,
                      const ComponentAction action) {
  return ErrorDTO{std::move(code),
                  std::move(message),
                  {{"component_id", component_id}, {"action", std::string(to_string(action))}}};
}

/// The four lifecycle actions are always offered; the others only when the
/// adapter declares the matching capability (CONTRACTS §2, §3).
bool action_supported(const ComponentAction action, const std::vector<Capability>& capabilities) {
  const auto declared = [&](const Capability capability) {
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
  };
  switch (action) {
    case ComponentAction::kPrepare:
    case ComponentAction::kEnable:
    case ComponentAction::kDisable:
    case ComponentAction::kRecover:
      return true;
    case ComponentAction::kResetPosition:
      return declared(Capability::kResetPosition);
    case ComponentAction::kPowerOff:
      return declared(Capability::kPowerOff);
    case ComponentAction::kReboot:
      return declared(Capability::kReboot);
    case ComponentAction::kSimReset:
      return declared(Capability::kSimReset);
  }
  return false;
}

/// CONTRACTS §6: whether `lifecycle` shows the result of a succeeded
/// `action`. Actions without a lifecycle target always do.
bool result_observed(const ComponentAction action, const ComponentLifecycle lifecycle) {
  switch (action) {
    case ComponentAction::kPrepare:
    case ComponentAction::kReboot:
      return lifecycle == ComponentLifecycle::kPrepared || lifecycle == ComponentLifecycle::kEnabled;
    case ComponentAction::kRecover:
      return lifecycle != ComponentLifecycle::kFault && lifecycle != ComponentLifecycle::kOffline;
    case ComponentAction::kEnable:
      return lifecycle == ComponentLifecycle::kEnabled;
    case ComponentAction::kDisable:
      return lifecycle == ComponentLifecycle::kPrepared;
    case ComponentAction::kPowerOff:
      return lifecycle == ComponentLifecycle::kOffline;
    case ComponentAction::kResetPosition:
    case ComponentAction::kSimReset:
      return true;
  }
  return true;
}

/// Publication compares states without their clock-driven fields.
ComponentState without_ages(ComponentState state) {
  state.observed_at_ns = 0;
  for (auto& stream : state.streams) stream.age_ns.reset();
  return state;
}

bool same_states(const std::vector<ComponentState>& left, const std::vector<ComponentState>& right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                    [](const auto& a, const auto& b) { return without_ages(a) == without_ages(b); });
}

}  // namespace

ComponentLifecycle MapComponentLifecycleState(const ComponentLifecycleStateDTO state,
                                              const bool reachable) noexcept {
  switch (state) {
    case ComponentLifecycleStateDTO::kUndeclared:
    case ComponentLifecycleStateDTO::kDeclared:
      return reachable ? ComponentLifecycle::kNotPrepared : ComponentLifecycle::kOffline;
    case ComponentLifecycleStateDTO::kPrepared:
    case ComponentLifecycleStateDTO::kDisabling:
    case ComponentLifecycleStateDTO::kDisabled:
      return ComponentLifecycle::kPrepared;
    case ComponentLifecycleStateDTO::kActive:
      return ComponentLifecycle::kEnabled;
    case ComponentLifecycleStateDTO::kDegraded:
    case ComponentLifecycleStateDTO::kFaulted:
      return ComponentLifecycle::kFault;
  }
  return ComponentLifecycle::kFault;
}

ComponentCoordinator::ComponentCoordinator(IClock& clock, ComponentStatesSink publish)
    : clock_(clock), publish_(std::move(publish)) {}

ComponentCoordinator::~ComponentCoordinator() {
  std::vector<std::jthread> workers;
  {
    std::lock_guard lock(mutex_);
    closing_ = true;
    for (auto& entry : entries_) workers.push_back(std::move(entry.worker));
  }
  // Each jthread requests stop and joins on destruction.
}

std::vector<std::jthread> ComponentCoordinator::SetComponents(
    std::vector<CoordinatedComponent> components) {
  std::vector<std::jthread> removed;
  {
    std::lock_guard lock(mutex_);
    std::vector<Entry> next;
    next.reserve(components.size());
    for (auto& component : components) {
      const auto current =
          std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return entry.component_id == component.component_id &&
                   entry.adapter == component.adapter;
          });
      if (current != entries_.end()) {
        current->owner_id = std::move(component.owner_id);
        current->role = std::move(component.role);
        current->owner = component.owner;
        next.push_back(std::move(*current));
        entries_.erase(current);
      } else {
        next.push_back(Entry{std::move(component.component_id), std::move(component.adapter),
                             std::move(component.owner_id), std::move(component.role),
                             component.owner, std::nullopt, std::nullopt, false, {}});
      }
    }
    for (auto& entry : entries_) {
      entry.worker.request_stop();
      removed.push_back(std::move(entry.worker));
    }
    entries_ = std::move(next);
  }
  Refresh();
  return removed;
}

Result<ComponentActionInProgress> ComponentCoordinator::Submit(const std::string& component_id,
                                                                const ComponentAction action) {
  std::jthread finished;
  ComponentActionInProgress accepted;
  {
    std::lock_guard lock(mutex_);
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return row.component_id == component_id;
    });
    if (entry == entries_.end()) {
      return Result<ComponentActionInProgress>::Err(action_error(
          "component_not_found", "The component is not in the cell", component_id, action));
    }
    if (entry->action) {
      return Result<ComponentActionInProgress>::Err(
          action_error("component_action_in_progress",
                       "Another action of this component is still running", component_id, action));
    }
    // CONTRACTS §2, §16: an owner offers no enable/disable.
    if (!action_supported(action, entry->adapter->capabilities()) ||
        (entry->owner &&
         (action == ComponentAction::kEnable || action == ComponentAction::kDisable))) {
      return Result<ComponentActionInProgress>::Err(
          action_error("component_action_unsupported", "The component doesn't offer this action",
                       component_id, action));
    }
    // CONTRACTS §14 (SIM-1, RP-1b): a position reset can't take effect while
    // the component's simulation isn't playing; refused at once, never waited.
    if (action == ComponentAction::kResetPosition) {
      const auto reason = entry->adapter->State(clock_.now()).reason;
      if (reason && reason->code == "simulation_not_playing")
        return Result<ComponentActionInProgress>::Err(action_error(
            "simulation_not_playing", "The simulation is not playing", component_id, action));
    }
    if (entry->owner_id) {
      const auto owner_entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
        return row.component_id == *entry->owner_id;
      });
      if (owner_entry == entries_.end() ||
          owner_entry->adapter->State(clock_.now()).lifecycle != ComponentLifecycle::kPrepared) {
        auto refused = action_error("owner_not_prepared",
                                    "The machine " + *entry->owner_id + " isn't prepared",
                                    component_id, action);
        refused.details.emplace("owner_id", *entry->owner_id);
        return Result<ComponentActionInProgress>::Err(std::move(refused));
      }
    }
    // CONTRACTS §2, §16: an owner's power action disables its members first.
    const bool group = entry->owner && (action == ComponentAction::kPowerOff ||
                                        action == ComponentAction::kReboot);
    if (group) {
      const auto busy = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
        return row.owner_id == component_id && row.action;
      });
      if (busy != entries_.end()) {
        auto refused = action_error("component_action_in_progress",
                                    "An action of the member " + busy->component_id +
                                        " is still running",
                                    component_id, action);
        refused.details.emplace("member_id", busy->component_id);
        return Result<ComponentActionInProgress>::Err(std::move(refused));
      }
    }
    accepted = ComponentActionInProgress{action, clock_.now()};
    std::vector<GroupMember> members;
    if (group) {
      for (auto& row : entries_) {
        if (row.owner_id != component_id) continue;
        row.action = ComponentActionInProgress{ComponentAction::kDisable, accepted.started_at_ns};
        row.interrupted = false;
        members.push_back({row.component_id, row.adapter});
      }
    }
    entry->action = accepted;
    entry->interrupted = false;
    finished = std::move(entry->worker);
    entry->worker = std::jthread(
        [this, component_id, adapter = entry->adapter, action,
         started_at_ns = accepted.started_at_ns,
         members = std::move(members)](const std::stop_token& stop) {
          RunAction(component_id, adapter, action, started_at_ns, members, stop);
        });
  }
  Refresh();
  return Result<ComponentActionInProgress>::Ok(accepted);
}

void ComponentCoordinator::RunAction(const std::string& component_id,
                                     const std::shared_ptr<IComponentAdapter>& adapter,
                                     const ComponentAction action, const std::int64_t started_at_ns,
                                     const std::vector<GroupMember>& members,
                                     const std::stop_token& stop) {
  // CONTRACTS §6: a succeeded action whose adapter state doesn't show its
  // result is recorded failed.
  const auto stamped = [&](ActionResult result, const ComponentAction run,
                           const std::shared_ptr<IComponentAdapter>& runner) {
    if (result.outcome == ActionOutcome::kSucceeded &&
        !result_observed(run, runner->State(clock_.now()).lifecycle)) {
      result.outcome = ActionOutcome::kFailed;
      result.error = ActionError{"component_action_result_not_observed",
                                 "The component's state doesn't show the action's result",
                                 std::nullopt, std::nullopt};
    }
    result.action = run;
    result.started_at_ns = started_at_ns;
    result.finished_at_ns = clock_.now();
    return result;
  };
  std::optional<ActionResult> refused;
  for (const auto& member : members) {
    if (refused) {
      Finish(member.component_id, member.adapter, std::nullopt);
      continue;
    }
    auto disabled = stamped(member.adapter->Run(ComponentAction::kDisable, stop),
                            ComponentAction::kDisable, member.adapter);
    if (disabled.outcome != ActionOutcome::kSucceeded) refused = disabled;
    Finish(member.component_id, member.adapter, std::move(disabled));
  }
  Finish(component_id, adapter,
         stamped(refused ? std::move(*refused) : adapter->Run(action, stop), action, adapter));
}

void ComponentCoordinator::Finish(const std::string& component_id,
                                  const std::shared_ptr<IComponentAdapter>& adapter,
                                  std::optional<ActionResult> result) {
  {
    std::lock_guard lock(mutex_);
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return row.component_id == component_id && row.adapter == adapter;
    });
    if (entry == entries_.end() || closing_) return;  // removed from the cell
    if (result && entry->interrupted && result->outcome == ActionOutcome::kFailed) {
      auto vendor = result->error.value_or(ActionError{});
      result->error = ActionError{"component_action_interrupted_by_stop",
                                  "A Stop interrupted the action", std::move(vendor.vendor_code),
                                  std::move(vendor.vendor_text)};
    }
    entry->action.reset();
    entry->interrupted = false;
    if (result) entry->last_action = std::move(result);
  }
  Refresh();
}

void ComponentCoordinator::InterruptActions() {
  std::lock_guard lock(mutex_);
  for (auto& entry : entries_) {
    if (!entry.action) continue;
    entry.interrupted = true;
    entry.worker.request_stop();
  }
}

void ComponentCoordinator::Refresh() {
  std::lock_guard refresh_lock(refresh_mutex_);
  struct Snapshot {
    std::string component_id;
    std::optional<std::string> owner_id;
    std::optional<std::string> role;
    std::optional<ComponentActionInProgress> action;
    std::optional<ActionResult> last_action;
    std::shared_ptr<IComponentAdapter> adapter;
  };
  std::vector<Snapshot> snapshots;
  {
    std::lock_guard lock(mutex_);
    snapshots.reserve(entries_.size());
    for (const auto& entry : entries_)
      snapshots.push_back({entry.component_id, entry.owner_id, entry.role, entry.action,
                           entry.last_action, entry.adapter});
  }
  const auto now = clock_.now();
  std::vector<ComponentState> states;
  states.reserve(snapshots.size());
  for (auto& snapshot : snapshots) {
    auto observed = snapshot.adapter->State(now);
    observed.component_id = std::move(snapshot.component_id);
    observed.owner_id = std::move(snapshot.owner_id);
    observed.role = std::move(snapshot.role);
    observed.capabilities = snapshot.adapter->capabilities();
    observed.action = std::move(snapshot.action);
    observed.last_action = std::move(snapshot.last_action);
    states.push_back(std::move(observed));
  }
  // CONTRACTS §1, §16: a member is usable only while its owner is. Owners
  // have no owner, so their states here are their adapters' own.
  const auto adapter_states = states;
  for (auto& state : states) {
    if (!state.owner_id) continue;
    const auto owner = std::find_if(adapter_states.begin(), adapter_states.end(),
                                    [&](const auto& row) { return row.component_id == *state.owner_id; });
    if (owner != adapter_states.end()) state = presented_member_state(std::move(state), *owner);
  }
  {
    std::lock_guard lock(mutex_);
    if (closing_ || same_states(states, published_)) return;
    published_ = states;
  }
  publish_(states);
}

std::vector<ComponentState> ComponentCoordinator::States() const {
  std::lock_guard lock(mutex_);
  return published_;
}

}  // namespace manyforge::core
