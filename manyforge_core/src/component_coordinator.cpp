#include <algorithm>
#include <manyforge_core/component_coordinator.hpp>
#include <pthread.h>

namespace manyforge::core {
namespace {

ErrorDTO action_error(std::string code, std::string message, const std::string& component_id,
                      const ComponentAction action) {
  return ErrorDTO{std::move(code),
                  std::move(message),
                  {{"component_id", component_id}, {"action", std::string(to_string(action))}}};
}

/// The four lifecycle actions are always offered; the others only when the
/// adapter declares the matching capability.
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

/// Whether `lifecycle` shows the result of a succeeded
/// `action`. Actions without a lifecycle target always do.
bool result_observed(const ComponentAction action, const ComponentState& state) {
  const auto lifecycle = state.lifecycle;
  switch (action) {
    case ComponentAction::kPrepare:
    case ComponentAction::kReboot:
      return lifecycle == ComponentLifecycle::kPrepared || lifecycle == ComponentLifecycle::kEnabled;
    case ComponentAction::kRecover:
      return !state.stop && lifecycle != ComponentLifecycle::kFault &&
             lifecycle != ComponentLifecycle::kOffline;
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

ComponentCoordinator::ActionCompletion::~ActionCompletion() {
  std::lock_guard lock(coordinator->mutex_);
  --coordinator->actions_in_flight_;
  // The waiter cannot return until this guard has unlocked; after unlocking
  // this worker does not access the coordinator again.
  coordinator->actions_drained_.notify_all();
}

bool ComponentCoordinator::CloseAndDrainActions(
    std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(mutex_);
  action_admission_closed_ = true;
  return actions_drained_.wait_until(lock, deadline,
                                     [this] { return actions_in_flight_ == 0; });
}

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
        // Apply may replace the adapter while its component reservation is
        // held; carry the existing reservation to the new entry by identity.
        const auto prior = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
          return entry.component_id == component.component_id;
        });
        const bool reserved = prior != entries_.end() && prior->composite_reserved;
        next.push_back(Entry{std::move(component.component_id), std::move(component.adapter),
                             std::move(component.owner_id), std::move(component.role),
                             component.owner, std::nullopt, std::nullopt, false, {}, reserved});
      }
    }
    for (auto& entry : entries_) removed.push_back(std::move(entry.worker));
    entries_ = std::move(next);
  }
  // The removed actions' stop callbacks run after the lock.
  for (auto& worker : removed) worker.request_stop();
  Refresh();
  return removed;
}

ComponentRecoveryFact RecoveryFact(const ComponentState& state) {
  ComponentRecoveryFact result{state.component_id, std::nullopt, std::nullopt, "ready"};
  if (state.fault) {
    result.cause_code = std::string(to_string(state.fault->code));
    result.vendor_code = state.fault->vendor_code;
  } else if (state.stop) {
    result.cause_code = std::string(to_string(state.stop->code));
    result.vendor_code = state.stop->vendor_code;
  }
  if (state.stop) {
    result.stop_code = std::string(to_string(state.stop->code));
    result.stop_vendor_code = state.stop->vendor_code;
  }
  if (std::find(state.capabilities.begin(), state.capabilities.end(), Capability::kSimReset) !=
      state.capabilities.end()) result.eligibility = "excluded";
  else if (state.action) result.eligibility = "blocked";
  else if (state.fault && state.stop)
    result.eligibility = state.fault->code != FaultCode::kOwnerStalled && state.fault->clearable &&
                                 state.fault->operator_action == FaultOperatorAction::kRecover
                             ? "recover" : "blocked";
  else if (state.fault) {
    result.eligibility = state.fault->code == FaultCode::kOwnerStalled || !state.fault->clearable ? "blocked" :
        state.fault->operator_action == FaultOperatorAction::kRecover ? "recover" :
        state.fault->operator_action == FaultOperatorAction::kEnable ? "ready" : "blocked";
  } else if (state.stop) result.eligibility = "recover";
  else if (state.lifecycle == ComponentLifecycle::kOffline ||
           state.lifecycle == ComponentLifecycle::kNotPrepared ||
           state.lifecycle == ComponentLifecycle::kFault) result.eligibility = "unknown";
  return result;
}

Result<ComponentRecoveryPreflight> ComponentCoordinator::RecoveryPreflight(
    const std::string& component_id) const {
  using Reply = Result<ComponentRecoveryPreflight>;
  // The coordinator's part is copied under the lock; the adapters are read
  // after it (an observation without admission authority).
  struct Row {
    std::string component_id;
    std::optional<std::string> owner_id;
    std::optional<std::string> role;
    std::optional<ComponentActionInProgress> action;
    std::optional<ActionResult> last_action;
    std::shared_ptr<IComponentAdapter> adapter;
  };
  const auto copy = [](const Entry& row) {
    return Row{row.component_id, row.owner_id, row.role, row.action, row.last_action, row.adapter};
  };
  std::optional<Row> member;
  std::optional<Row> owner;
  {
    std::lock_guard lock(mutex_);
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return row.component_id == component_id;
    });
    if (entry == entries_.end() || closing_)
      return Reply::Err(action_error("component_not_found", "The component is not in the cell",
                                     component_id, ComponentAction::kEnable));
    member = copy(*entry);
    if (entry->owner_id) {
      const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
        return row.component_id == *entry->owner_id;
      });
      if (found == entries_.end())
        return Reply::Err(action_error("owner_not_prepared", "The component's machine is not in the cell",
                                       component_id, ComponentAction::kEnable));
      owner = copy(*found);
    }
  }
  const auto now = clock_.now();
  const auto read = [&](const Row& row) {
    auto state = row.adapter->State(now);
    state.component_id = row.component_id;
    state.owner_id = row.owner_id;
    state.role = row.role;
    state.action = row.action;
    state.last_action = row.last_action;
    if (!(state.fault && state.fault->code == FaultCode::kOwnerStalled))
      state.capabilities = row.adapter->capabilities();
    return state;
  };
  ComponentRecoveryPreflight result{read(*member), std::nullopt};
  if (owner) result.owner = read(*owner);
  return Reply::Ok(std::move(result));
}

Result<ComponentActionInProgress> ComponentCoordinator::SubmitRecoveryEnable(
    const std::string& component_id, std::vector<ComponentRecoveryFact> confirmed,
    std::function<bool()> current, std::function<bool()> owner_recovered) {
  using Reply = Result<ComponentActionInProgress>;
  using Row = std::vector<Entry>::iterator;
  ComponentActionInProgress accepted{ComponentAction::kEnable, clock_.now()};
  // Every check on the coordinator's state, in today's order (all of them
  // rank before the facts); on none, `entry` and `owner` (end() for none) are
  // the rows. The caller holds `mutex_`.
  const auto state_refusal = [&](Row& entry, Row& owner) -> std::optional<ErrorDTO> {
    if (action_admission_closed_ || closing_) {
      auto refused = action_error("component_not_ready", "Component actions are closing",
                                  component_id, ComponentAction::kEnable);
      refused.details.emplace("reason", "coordinator_closing");
      return refused;
    }
    entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return row.component_id == component_id;
    });
    if (entry == entries_.end() || closing_)
      return action_error("component_not_found", "The component is not in the cell",
                          component_id, ComponentAction::kEnable);
    if (entry->owner)
      return action_error("component_action_unsupported", "A machine has no Enable action",
                          component_id, ComponentAction::kEnable);
    if (entry->action || entry->composite_reserved)
      return action_error("component_action_in_progress", "Another component action is running",
                          component_id, ComponentAction::kEnable);
    owner = entries_.end();
    if (entry->owner_id) {
      owner = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
        return row.component_id == *entry->owner_id;
      });
      if (owner == entries_.end())
        return action_error("owner_not_prepared", "The machine is not in the cell",
                            component_id, ComponentAction::kEnable);
      if (owner->action || owner->composite_reserved)
        return action_error("component_action_in_progress", "Another machine action is running",
                            component_id, ComponentAction::kEnable);
      const auto busy_member = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
        return row.owner_id == entry->owner_id && (row.action || row.composite_reserved);
      });
      if (busy_member != entries_.end())
        return action_error("component_action_in_progress", "Another machine member action is running",
                            component_id, ComponentAction::kEnable);
    }
    return std::nullopt;
  };
  const auto read_fact = [&](const std::string& id, const std::shared_ptr<IComponentAdapter>& source) {
    auto state = source->State(clock_.now());
    state.component_id = id;
    if (!(state.fault && state.fault->code == FaultCode::kOwnerStalled))
      state.capabilities = source->capabilities();
    return RecoveryFact(state);
  };
  // The adapters are read without the lock, between two memory-only passes
  // under it. Runs again only when the component list replaced the component
  // or its owner in between: the order "replaced, then Submit".
  for (;;) {
    std::shared_ptr<IComponentAdapter> adapter;
    std::optional<std::string> owner_id;
    std::shared_ptr<IComponentAdapter> owner_adapter;
    {
      std::lock_guard lock(mutex_);
      Row entry;
      Row owner;
      if (auto refused = state_refusal(entry, owner)) return Reply::Err(std::move(*refused));
      adapter = entry->adapter;
      owner_id = entry->owner_id;
      if (owner != entries_.end()) owner_adapter = owner->adapter;
    }
    std::vector<ComponentRecoveryFact> actual;
    if (owner_adapter) actual.push_back(read_fact(*owner_id, owner_adapter));
    actual.push_back(read_fact(component_id, adapter));
    std::optional<ErrorDTO> refused_by_facts;
    if (!current || !current()) {
      refused_by_facts = action_error("stale_generation", "Configuration changed; confirm again",
                                      component_id, ComponentAction::kEnable);
    } else if (actual != confirmed) {
      // OD-312: the causes changed, not the configuration: the GUI re-reads them.
      refused_by_facts = action_error("component_recovery_confirmation_changed",
                                      "Recovery causes changed; confirm again",
                                      component_id, ComponentAction::kEnable);
    } else {
      const auto& member_fact = actual.back();
      const bool owner_recover = owner_adapter && actual.front().eligibility == "recover";
      if ((owner_adapter && actual.front().eligibility != "ready" && !owner_recover) ||
          member_fact.eligibility == "blocked" || member_fact.eligibility == "excluded" ||
          (member_fact.eligibility == "unknown" && !owner_recover))
        refused_by_facts = action_error("component_action_refused",
                                        "The component cannot be recovered and enabled",
                                        component_id, ComponentAction::kEnable);
    }
    {
      std::lock_guard lock(mutex_);
      Row entry;
      Row owner;
      if (auto refused = state_refusal(entry, owner)) return Reply::Err(std::move(*refused));
      const auto current_owner =
          owner == entries_.end() ? std::shared_ptr<IComponentAdapter>{} : owner->adapter;
      if (entry->adapter != adapter || entry->owner_id != owner_id || current_owner != owner_adapter)
        continue;  // replaced since the read: read the current one
      if (refused_by_facts) return Reply::Err(std::move(*refused_by_facts));
      if (owner != entries_.end()) owner->composite_reserved = true;
      entry->action = accepted;
      entry->interrupted = false;
      ++actions_in_flight_;
    }
    // The thread is created without the lock and stored after it.
    std::jthread worker;
    try {
      auto action_body =
          [this, component_id, adapter, owner_id, owner_adapter,
           confirmed = std::move(confirmed), current = std::move(current),
           owner_recovered = std::move(owner_recovered),
           started = accepted.started_at_ns](std::stop_token stop) mutable {
            RunRecoveryEnable(component_id, adapter, owner_id, owner_adapter,
                              std::move(confirmed), std::move(current), std::move(owner_recovered), started, stop);
          };
      worker = std::jthread(
          [this, action_body = std::optional{std::move(action_body)}](std::stop_token stop) mutable {
            static_cast<void>(::pthread_setname_np(::pthread_self(), "mf_comp_recover"));
            ActionCompletion completion{this};
            {
              auto action = std::move(*action_body);
              action_body.reset();
              action(stop);
            } // Both callable wrappers and captures retire before completion.
          });
    } catch (...) {
      // As today: the action is taken back and refused. No other request can
      // reserve the owner meanwhile, so its mark is this one's.
      {
        std::lock_guard lock(mutex_);
        for (auto& row : entries_) {
          if (row.component_id == component_id && row.adapter == adapter && row.action == accepted) {
            row.action.reset();
            row.interrupted = false;
          }
          if (owner_adapter && row.component_id == *owner_id) row.composite_reserved = false;
        }
        --actions_in_flight_;
        actions_drained_.notify_all();
      }
      return Reply::Err(action_error("component_action_refused", "The component action could not start",
                                     component_id, ComponentAction::kEnable));
    }
    // Joined after Refresh, without the lock.
    auto finished = StoreWorker(component_id, adapter, accepted, std::move(worker));
    Refresh();
    return Reply::Ok(accepted);
  }
}

std::jthread ComponentCoordinator::StoreWorker(const std::string& component_id,
                                               const std::shared_ptr<IComponentAdapter>& adapter,
                                               const ComponentActionInProgress& accepted,
                                               std::jthread worker) {
  std::stop_source stop(std::nostopstate);
  std::jthread finished;
  {
    std::lock_guard lock(mutex_);
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return row.component_id == component_id && row.adapter == adapter;
    });
    if (entry != entries_.end() && !closing_ && entry->action == accepted) {
      // A Stop between the acceptance and here found no thread to stop.
      if (entry->interrupted) stop = worker.get_stop_source();
      finished = std::move(entry->worker);
      entry->worker = std::move(worker);
    } else {
      // Replaced or removed (or closing) while the thread was being created:
      // its result is dropped, as for an action removed by SetComponents, and
      // it is joined by Submit. Or the action already ended (a short join).
      stop = worker.get_stop_source();
      finished = std::move(worker);
    }
  }
  stop.request_stop();
  return finished;
}

Result<ComponentActionInProgress> ComponentCoordinator::Submit(const std::string& component_id,
                                                                const ComponentAction action) {
  using Reply = Result<ComponentActionInProgress>;
  using Row = std::vector<Entry>::iterator;
  // The first checks on the coordinator's state, in today's order; on none,
  // `entry` is the component's row. The caller holds `mutex_`.
  const auto state_refusal = [&](Row& entry) -> std::optional<ErrorDTO> {
    if (action_admission_closed_ || closing_) {
      auto refused = action_error("component_not_ready", "Component actions are closing",
                                  component_id, action);
      refused.details.emplace("reason", "coordinator_closing");
      return refused;
    }
    entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return row.component_id == component_id;
    });
    if (entry == entries_.end())
      return action_error("component_not_found", "The component is not in the cell", component_id,
                          action);
    if (entry->action || entry->composite_reserved)
      return action_error("component_action_in_progress",
                          "Another action of this component is still running", component_id,
                          action);
    return std::nullopt;
  };
  const auto owner_row = [&](const std::optional<std::string>& owner_id) {
    return !owner_id ? entries_.end()
                     : std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
                         return row.component_id == *owner_id;
                       });
  };
  // An owner's power action disables its members first.
  const bool power_action = action == ComponentAction::kPowerOff || action == ComponentAction::kReboot;
  const bool motion_action =
      action == ComponentAction::kEnable || action == ComponentAction::kResetPosition;
  // The adapters are read without the lock, between two memory-only passes
  // under it. Runs again only when the component list replaced the component
  // or its owner in between: the order "replaced, then Submit".
  for (;;) {
    std::shared_ptr<IComponentAdapter> adapter;
    bool owner{};
    std::optional<std::string> owner_id;
    // The member's owner, when it is in the cell.
    std::shared_ptr<IComponentAdapter> owner_adapter;
    {
      std::lock_guard lock(mutex_);
      Row entry;
      if (auto refused = state_refusal(entry)) return Reply::Err(std::move(*refused));
      adapter = entry->adapter;
      owner = entry->owner;
      owner_id = entry->owner_id;
      if (const auto found = owner_row(owner_id); found != entries_.end())
        owner_adapter = found->adapter;
    }
    // The adapters' facts and the refusals they decide; returned under the
    // lock at today's positions.
    std::optional<ErrorDTO> refused_by_facts;
    // An owner offers no enable/disable.
    if (!action_supported(action, adapter->capabilities()) ||
        (owner && (action == ComponentAction::kEnable || action == ComponentAction::kDisable)))
      refused_by_facts = action_error("component_action_unsupported",
                                      "The component doesn't offer this action", component_id, action);
    if (!refused_by_facts && action == ComponentAction::kEnable) {
      auto state = adapter->State(clock_.now());
      state.capabilities = adapter->capabilities();
      if (RecoveryFact(state).eligibility != "excluded" && (state.fault || state.stop))
        refused_by_facts = action_error("component_action_refused",
                                        "Recovery confirmation is required before Enable",
                                        component_id, action);
    }
    // A position reset can't take effect while
    // the component's simulation isn't playing; refused at once, never waited.
    if (!refused_by_facts && action == ComponentAction::kResetPosition) {
      const auto reason = adapter->State(clock_.now()).reason;
      if (reason && reason->code == "simulation_not_playing")
        refused_by_facts = action_error("simulation_not_playing", "The simulation is not playing",
                                        component_id, action);
    }
    auto prepared_recovery = !refused_by_facts && action == ComponentAction::kRecover
                                 ? adapter->PrepareSharedOwnerRecovery()
                                 : IComponentAdapter::PreparedRecovery{};
    std::optional<ComponentState> owner_state;
    if (!refused_by_facts && owner_adapter) owner_state = owner_adapter->State(clock_.now());

    ComponentActionInProgress accepted;
    std::vector<GroupMember> members;
    {
      std::lock_guard lock(mutex_);
      Row entry;
      if (auto refused = state_refusal(entry)) return Reply::Err(std::move(*refused));
      const auto owner_entry = owner_row(owner_id);
      const auto current_owner =
          owner_entry == entries_.end() ? std::shared_ptr<IComponentAdapter>{} : owner_entry->adapter;
      if (entry->adapter != adapter || entry->owner != owner || entry->owner_id != owner_id ||
          current_owner != owner_adapter)
        continue;  // replaced since the read: read the current one
      if (refused_by_facts) return Reply::Err(std::move(*refused_by_facts));
      if (owner_id) {
        if (owner_entry != entries_.end() && owner_entry->composite_reserved)
          return Reply::Err(action_error("component_action_in_progress",
                                         "The machine has a recovery action in progress",
                                         component_id, action));
        // An owner's stop (an e-stop and its latched error) is normal: it gates
        // motion (Enable, Reset position), never its members' Prepare, Disable
        // or Recover. Its explicit clear is the machine's Recover (Recover and
        // enable, or Enable all).
        if (owner_state && owner_state->lifecycle == ComponentLifecycle::kPrepared &&
            !owner_state->fault && owner_state->stop && motion_action && !prepared_recovery) {
          auto refused = action_error("owner_estop_active",
                                      "The machine " + *owner_id +
                                          " reports an e-stop; Recover it once the e-stop is released",
                                      component_id, action);
          refused.details.emplace("owner_id", *owner_id);
          return Reply::Err(std::move(refused));
        }
        const bool owner_ready = owner_state &&
                                 owner_state->lifecycle == ComponentLifecycle::kPrepared &&
                                 !owner_state->fault;
        if (owner_entry == entries_.end() || (!owner_ready && !prepared_recovery)) {
          auto refused = action_error("owner_not_prepared",
                                      "The machine " + *owner_id + " isn't prepared",
                                      component_id, action);
          refused.details.emplace("owner_id", *owner_id);
          return Reply::Err(std::move(refused));
        }
      }
      const bool group = owner && power_action;
      if (owner && action == ComponentAction::kRecover) {
        const auto busy = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
          return row.owner_id == component_id && row.composite_reserved;
        });
        if (busy != entries_.end()) {
          auto refused = action_error("component_action_in_progress",
                                      "An action of the member " + busy->component_id +
                                          " is still running", component_id, action);
          refused.details.emplace("member_id", busy->component_id);
          return Reply::Err(std::move(refused));
        }
      }
      if (group) {
        const auto busy = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
          return row.owner_id == component_id && (row.action || row.composite_reserved);
        });
        if (busy != entries_.end()) {
          auto refused = action_error("component_action_in_progress",
                                      "An action of the member " + busy->component_id +
                                          " is still running",
                                      component_id, action);
          refused.details.emplace("member_id", busy->component_id);
          return Reply::Err(std::move(refused));
        }
      }
      accepted = ComponentActionInProgress{action, clock_.now()};
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
      ++actions_in_flight_;
    }
    // The thread is created without the lock and stored after it.
    std::jthread worker;
    try {
      auto action_body =
        [this, component_id, adapter, action,
         started_at_ns = accepted.started_at_ns,
         members = std::move(members),
         prepared_recovery = std::move(prepared_recovery)](const std::stop_token& stop) {
          RunAction(component_id, adapter, action, started_at_ns, members, stop, prepared_recovery);
        };
      worker = std::jthread(
        [this, action_body = std::optional{std::move(action_body)}](const std::stop_token& stop) mutable {
          static_cast<void>(::pthread_setname_np(::pthread_self(), "mf_comp_action"));
          ActionCompletion completion{this};
          {
            auto run = std::move(*action_body);
            action_body.reset();
            run(stop);
          } // Both callable wrappers and captures retire before completion.
        });
    } catch (...) {
      {
        std::lock_guard lock(mutex_);
        --actions_in_flight_;
        actions_drained_.notify_all();
      }
      throw;
    }
    // Joined after Refresh, without the lock.
    auto finished = StoreWorker(component_id, adapter, accepted, std::move(worker));
    Refresh();
    return Reply::Ok(accepted);
  }
}

void ComponentCoordinator::RunRecoveryEnable(
    const std::string& component_id, const std::shared_ptr<IComponentAdapter>& adapter,
    const std::optional<std::string>& owner_id, const std::shared_ptr<IComponentAdapter>& owner,
    std::vector<ComponentRecoveryFact> confirmed, std::function<bool()> current,
    std::function<bool()> owner_recovered,
    const std::int64_t started_at_ns, const std::stop_token stop) {
  ActionResult result;
  result.action = ComponentAction::kEnable;
  result.started_at_ns = started_at_ns;
  result.outcome = ActionOutcome::kFailed;
  const auto refused = [&](std::string code, std::string message) {
    result.error = ActionError{std::move(code), std::move(message), std::nullopt, std::nullopt};
  };
  auto member_fact = confirmed.back();
  std::optional<ComponentRecoveryFact> owner_fact;
  if (owner) owner_fact = confirmed.front();
  const auto read = [&](const std::string& id, const std::shared_ptr<IComponentAdapter>& source) {
    auto state = source->State(clock_.now());
    state.component_id = id;
    if (!(state.fault && state.fault->code == FaultCode::kOwnerStalled))
      state.capabilities = source->capabilities();
    return RecoveryFact(state);
  };
  // The adapters' facts are read before the lock; under it only the identity
  // of the member and its owner and their reservations.
  const auto revalidate = [&] {
    if (stop.stop_requested() || !current()) return false;
    if (owner && read(*owner_id, owner) != *owner_fact) return false;
    if (read(component_id, adapter) != member_fact) return false;
    std::lock_guard lock(mutex_);
    const auto member = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return row.component_id == component_id && row.adapter == adapter && row.owner_id == owner_id;
    });
    if (member == entries_.end() || !member->action) return false;
    if (!owner) return true;
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& row) {
      return owner_id && row.component_id == *owner_id && row.adapter == owner && row.composite_reserved;
    });
  };
  const auto phase = [&](const std::string& id, const std::shared_ptr<IComponentAdapter>& target,
                         ComponentAction action) {
    ActionResult step;
    step.action = action;
    step.outcome = ActionOutcome::kFailed;
    try {
      auto prepared = !stop.stop_requested() && action == ComponentAction::kRecover
                          ? target->PrepareSharedOwnerRecovery() : IComponentAdapter::PreparedRecovery{};
      if (stop.stop_requested()) {
        step.error = ActionError{"component_action_interrupted_by_stop", "The component action was interrupted",
                                 std::nullopt, std::nullopt};
      } else if (prepared && !revalidate()) {
        step.error = ActionError{"stale_generation", "Configuration changed; confirm again", std::nullopt, std::nullopt};
      } else {
        step = prepared ? prepared(stop) : target->RunConfirmed(action, stop, revalidate);
        if (step.outcome == ActionOutcome::kSucceeded &&
            !result_observed(action, target->State(clock_.now()))) {
          step.outcome = ActionOutcome::kFailed;
          step.error = ActionError{"component_action_result_not_observed",
                                   "The component's state doesn't show the action's result",
                                   std::nullopt, std::nullopt};
        }
      }
    } catch (...) {
      step.outcome = ActionOutcome::kUnknown;
      step.error = ActionError{"component_action_outcome_unknown", "The component action outcome is unknown",
                               std::nullopt, std::nullopt};
    }
    result.phases.push_back({id, action, step.outcome, step.error});
    if (step.outcome != ActionOutcome::kSucceeded) {
      result.outcome = step.outcome;
      result.error = step.error;
      return false;
    }
    return true;
  };
  try {
    bool proceed = true;
    if (owner && owner_fact->eligibility == "recover") {
      proceed = phase(*owner_id, owner, ComponentAction::kRecover);
      if (proceed && owner_recovered && !owner_recovered()) {
        refused("stale_generation", "The applied configuration changed; confirm again");
        proceed = false;
      }
      if (proceed) {
        owner_fact = ComponentRecoveryFact{*owner_id, std::nullopt, std::nullopt, "ready"};
        const auto observed = read(component_id, adapter);
        if (observed.eligibility == "ready" && !observed.cause_code && !observed.stop_code) {
          member_fact = observed;
        } else if (observed != member_fact || member_fact.eligibility == "unknown") {
          refused("component_recovery_confirmation_changed",
                  "The member has an undisclosed cause; confirm again");
          proceed = false;
        }
      }
    }
    if (proceed && member_fact.eligibility == "recover") {
      proceed = phase(component_id, adapter, ComponentAction::kRecover);
      if (proceed) member_fact = ComponentRecoveryFact{component_id, std::nullopt, std::nullopt, "ready"};
    }
    if (proceed && phase(component_id, adapter, ComponentAction::kEnable)) {
      result.outcome = ActionOutcome::kSucceeded;
      result.error.reset();
    }
  } catch (...) {
    result.outcome = ActionOutcome::kUnknown;
    refused("component_action_outcome_unknown", "The component action outcome is unknown");
  }
  {
    std::lock_guard lock(mutex_);
    if (owner_id) {
      const auto parent = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
        return row.component_id == *owner_id && row.adapter == owner;
      });
      if (parent != entries_.end()) parent->composite_reserved = false;
    }
  }
  result.finished_at_ns = clock_.now();
  Finish(component_id, adapter, std::move(result));
}

void ComponentCoordinator::RunAction(const std::string& component_id,
                                     const std::shared_ptr<IComponentAdapter>& adapter,
                                     const ComponentAction action, const std::int64_t started_at_ns,
                                     const std::vector<GroupMember>& members,
                                     const std::stop_token& stop,
                                     const IComponentAdapter::PreparedRecovery& prepared_recovery) {
  // A succeeded action whose adapter state doesn't show its
  // result is recorded failed.
  const auto stamped = [&](ActionResult result, const ComponentAction run,
                           const std::shared_ptr<IComponentAdapter>& runner) {
    if (result.outcome == ActionOutcome::kSucceeded &&
        !result_observed(run, runner->State(clock_.now()))) {
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
         stamped(refused ? std::move(*refused)
                         : prepared_recovery ? prepared_recovery(stop) : adapter->Run(action, stop),
                 action, adapter));
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
  InterruptActionsOf([](std::string_view) { return true; });
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
    std::vector<Capability> last_published_capabilities;
  };
  std::vector<Snapshot> snapshots;
  {
    std::lock_guard lock(mutex_);
    snapshots.reserve(entries_.size());
    for (const auto& entry : entries_) {
      const auto previous = std::find_if(published_.begin(), published_.end(),
          [&](const auto& state) { return state.component_id == entry.component_id; });
      snapshots.push_back({entry.component_id, entry.owner_id, entry.role, entry.action,
                           entry.last_action, entry.adapter,
                           previous == published_.end() ? std::vector<Capability>{}
                                                        : previous->capabilities});
    }
  }
  const auto now = clock_.now();
  std::vector<ComponentState> states;
  states.reserve(snapshots.size());
  for (auto& snapshot : snapshots) {
    auto observed = snapshot.adapter->State(now);
    observed.component_id = std::move(snapshot.component_id);
    observed.owner_id = std::move(snapshot.owner_id);
    observed.role = std::move(snapshot.role);
    // A stall can be published between this State check and capabilities;
    // that Refresh may wait, and the next one will observe the latch.
    observed.capabilities = observed.fault && observed.fault->code == FaultCode::kOwnerStalled
                                ? std::move(snapshot.last_published_capabilities)
                                : snapshot.adapter->capabilities();
    observed.action = std::move(snapshot.action);
    observed.last_action = std::move(snapshot.last_action);
    states.push_back(std::move(observed));
  }
  // A member is usable only while its owner is. Owners
  // have no owner, so their states here are their adapters' own.
  const auto adapter_states = states;
  for (std::size_t index = 0; index < states.size(); ++index) {
    auto& state = states[index];
    if (!state.owner_id) continue;
    // A member's own stalled transport stays visible independently of the
    // (separately held) shared-owner Recover eligibility.
    if (state.lifecycle == ComponentLifecycle::kFault && state.fault &&
        state.fault->code == FaultCode::kOwnerStalled) continue;
    // Keep the actionable shared-owner fault visible while its Recover can
    // repair that owner; unrelated member states retain normal presentation.
    if (state.lifecycle == ComponentLifecycle::kFault && state.fault &&
        snapshots[index].adapter->CanRecoverSharedOwner()) continue;
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

std::optional<std::string> ComponentCoordinator::ReserveForApply(
    const std::vector<std::string>& component_ids, std::vector<std::string>& reserved) {
  reserved.clear();
  reserved.reserve(component_ids.size());
  std::lock_guard lock(mutex_);
  for (const auto& id : component_ids) {
    const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& row) { return row.component_id == id; });
    if (entry != entries_.end()) {
      if (entry->action || entry->composite_reserved) return id;
      if (entry->owner_id) {
        const auto owner = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& row) {
          return row.component_id == *entry->owner_id;
        });
        if (owner != entries_.end() && (owner->action || owner->composite_reserved)) return id;
      }
    }
  }
  for (const auto& entry : entries_)
    if (std::find(component_ids.begin(), component_ids.end(), entry.component_id) != component_ids.end())
      reserved.push_back(entry.component_id);
  for (auto& entry : entries_)
    if (std::find(reserved.begin(), reserved.end(), entry.component_id) != reserved.end())
      entry.composite_reserved = true;
  return std::nullopt;
}

void ComponentCoordinator::ReleaseApplyReservation(
    const std::vector<std::string>& component_ids) noexcept {
  std::lock_guard lock(mutex_);
  for (auto& entry : entries_)
    if (std::find(component_ids.begin(), component_ids.end(), entry.component_id) != component_ids.end())
      entry.composite_reserved = false;
}

std::vector<ComponentState> ComponentCoordinator::States() const {
  std::lock_guard lock(mutex_);
  return published_;
}

}  // namespace manyforge::core
