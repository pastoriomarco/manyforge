#include "host_component_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

/// Opens the component's command routes inside its `enable`, after the adapter
/// succeeded, so `enabled` is never published with closed routes.
class HostComponentRuntime::RouteOpeningAdapter final : public core::IComponentAdapter {
 public:
  RouteOpeningAdapter(std::string component_id, std::shared_ptr<core::IComponentAdapter> adapter,
                      const Hooks &hooks, bool transport_consumer)
      : component_id_(std::move(component_id)), adapter_(std::move(adapter)), hooks_(hooks),
        transport_consumer_(transport_consumer) {}

  [[nodiscard]] core::ComponentState State(const std::int64_t now_ns) const override {
    if (hooks_.shared_transport_fault) {
      auto fault = hooks_.shared_transport_fault(component_id_);
      if (fault && fault->code == core::FaultCode::kOwnerStalled) {
        core::ComponentState state;
        state.lifecycle = core::ComponentLifecycle::kFault;
        state.activity = core::ComponentActivity::kUnknown;
        state.fault = std::move(fault);
        state.observed_at_ns = now_ns;
        return state;
      }
    }
    auto state = adapter_->State(now_ns);
    if (hooks_.shared_transport_fault) {
      if (auto fault = hooks_.shared_transport_fault(component_id_)) {
        state.lifecycle = core::ComponentLifecycle::kFault;
        state.fault = std::move(fault);
        state.reason.reset();
        state.activity = core::ComponentActivity::kUnknown;
      }
    }
    return state;
  }

  [[nodiscard]] bool CanRecoverSharedOwner() const override {
    if (hooks_.shared_transport_fault) {
      const auto fault = hooks_.shared_transport_fault(component_id_);
      // A stall is not an executor failure and does not enable shared-owner
      // recovery; its Recover route remains held pending OD51.
      if (fault && fault->code == core::FaultCode::kOwnerStalled) return false;
      if (fault) return true;
    }
    return adapter_->CanRecoverSharedOwner();
  }

  [[nodiscard]] PreparedRecovery PrepareSharedOwnerRecovery() const override {
    if (hooks_.prepare_shared_transport_recovery) {
      auto prepared = hooks_.prepare_shared_transport_recovery(component_id_);
      if (prepared) return prepared;
    }
    if (hooks_.prepare_adapter_transport_recovery) {
      auto prepared = hooks_.prepare_adapter_transport_recovery(component_id_, adapter_);
      if (prepared) return prepared;
    }
    return adapter_->PrepareSharedOwnerRecovery();
  }

  [[nodiscard]] core::ActionResult Run(const core::ComponentAction action,
                                       const std::stop_token stop) override {
    return RunWithConfirmation(action, stop, {});
  }

  [[nodiscard]] core::ActionResult RunConfirmed(
      const core::ComponentAction action, const std::stop_token stop,
      const std::function<bool()> &current) override {
    if (!current) {
      core::ActionResult refused;
      refused.action = action;
      refused.outcome = core::ActionOutcome::kFailed;
      refused.error = core::ActionError{"stale_generation", "Configuration changed; confirm again",
                                        std::nullopt, std::nullopt};
      return refused;
    }
    return RunWithConfirmation(action, stop, current);
  }

  [[nodiscard]] core::ActionResult RunWithConfirmation(
      const core::ComponentAction action, const std::stop_token stop,
      const std::function<bool()> &current) {
    std::shared_ptr<void> transport_admission;
    if (hooks_.enter_transport_effect) {
      auto admitted = hooks_.enter_transport_effect(component_id_, transport_consumer_.load());
      if (!admitted.ok()) {
        core::ActionResult refused;
        refused.action = action;
        refused.outcome = core::ActionOutcome::kFailed;
        refused.error = core::ActionError{admitted.error().code, admitted.error().message,
                                          std::nullopt, std::nullopt};
        return refused;
      }
      transport_admission = std::move(admitted.value());
    }
    if (current && !current()) {
      core::ActionResult refused;
      refused.action = action;
      refused.outcome = core::ActionOutcome::kFailed;
      refused.error = core::ActionError{"stale_generation", "Configuration changed; confirm again",
                                        std::nullopt, std::nullopt};
      return refused;
    }
    if (action != core::ComponentAction::kDisable && hooks_.transport_effect_fault) {
      if (const auto fault = hooks_.transport_effect_fault(component_id_, adapter_.get())) {
        core::ActionResult refused;
        refused.action = action;
        refused.outcome = core::ActionOutcome::kFailed;
        refused.error = core::ActionError{fault->code, fault->message, std::nullopt, std::nullopt};
        return refused;
      }
    }
    if (action == core::ComponentAction::kPrepare && hooks_.shared_transport_fault &&
        hooks_.shared_transport_fault(component_id_)) {
      core::ActionResult refused;
      refused.action = action;
      refused.outcome = core::ActionOutcome::kFailed;
      refused.error = core::ActionError{"component_action_in_progress",
                                       "The shared transport recovery is not settled",
                                       std::nullopt, std::nullopt};
      return refused;
    }
    // The component's routes open before its adapter enables it (as route
    // selection did): a route that can't open leaves the component prepared,
    // and commands stay refused until the component reports enabled anyway.
    if (action == core::ComponentAction::kEnable && hooks_.open_routes) {
      const auto opened = hooks_.open_routes(component_id_);
      if (!opened.ok()) {
        core::ActionResult refused;
        refused.action = action;
        refused.outcome = core::ActionOutcome::kFailed;
        refused.error = core::ActionError{opened.error().code, opened.error().message,
                                          std::nullopt, std::nullopt};
        return refused;
      }
    }
    return adapter_->Run(action, stop);
  }

  [[nodiscard]] std::vector<core::Capability> capabilities() const override {
    return adapter_->capabilities();
  }

  [[nodiscard]] const std::string &component_id() const noexcept { return component_id_; }
  [[nodiscard]] const std::shared_ptr<core::IComponentAdapter> &adapter() const noexcept {
    return adapter_;
  }
  [[nodiscard]] bool transport_consumer() const noexcept { return transport_consumer_.load(); }
  [[nodiscard]] bool adapter_transport_consumer() const noexcept { return adapter_transport_consumer_.load(); }
  void MarkAdapterTransportConsumer(bool value) noexcept {
    if (value) adapter_transport_consumer_.store(true);
  }
  void MarkTransportConsumer(bool value) noexcept {
    if (value) transport_consumer_.store(true);
  }

 private:
  std::string component_id_;
  std::shared_ptr<core::IComponentAdapter> adapter_;
  const Hooks &hooks_;
  std::atomic<bool> transport_consumer_;
  std::atomic<bool> adapter_transport_consumer_{false};
};

HostComponentRuntime::HostComponentRuntime(core::IClock &clock, Hooks hooks)
    : hooks_(std::move(hooks)),
      coordinator_(clock, [this](const std::vector<core::ComponentState> &states) {
        Publish(states);
      }) {}

HostComponentRuntime::~HostComponentRuntime() = default;

std::vector<std::jthread> HostComponentRuntime::SetComponents(
    std::vector<core::CoordinatedComponent> components) {
  std::vector<core::CoordinatedComponent> coordinated;
  coordinated.reserve(components.size());
  {
    std::lock_guard lock(mutex_);
    std::map<const core::IComponentAdapter *, std::shared_ptr<RouteOpeningAdapter>> next;
    for (auto &component : components) {
      const auto *key = component.adapter.get();
      const bool transport_consumer = hooks_.uses_shared_transport &&
                                      hooks_.uses_shared_transport(component.component_id);
      const bool adapter_transport_consumer = hooks_.uses_adapter_transport &&
                                              hooks_.uses_adapter_transport(component.component_id);
      auto found = wrappers_.find(key);
      auto wrapper = found != wrappers_.end() && found->second->component_id() ==
                                                     component.component_id
                         ? found->second
                         : std::make_shared<RouteOpeningAdapter>(component.component_id,
                                                                 component.adapter, hooks_,
                                                                 transport_consumer);
      wrapper->MarkTransportConsumer(transport_consumer);
      wrapper->MarkAdapterTransportConsumer(adapter_transport_consumer);
      next.emplace(key, wrapper);
      coordinated.push_back({std::move(component.component_id), std::move(wrapper),
                             std::move(component.owner_id), std::move(component.role),
                             component.owner});
    }
    wrappers_ = std::move(next);
  }
  return coordinator_.SetComponents(std::move(coordinated));
}

core::Result<core::ComponentActionInProgress> HostComponentRuntime::Submit(
    const std::string &component_id, const core::ComponentAction action) {
  return coordinator_.Submit(component_id, action);
}

core::Result<core::ComponentRecoveryPreflight> HostComponentRuntime::RecoveryPreflight(
    const std::string &component_id) const {
  return coordinator_.RecoveryPreflight(component_id);
}

core::Result<core::ComponentActionInProgress> HostComponentRuntime::SubmitRecoveryEnable(
    const std::string &component_id, std::vector<core::ComponentRecoveryFact> confirmed,
    std::function<bool()> current, std::function<bool()> owner_recovered) {
  return coordinator_.SubmitRecoveryEnable(component_id, std::move(confirmed), std::move(current),
                                           std::move(owner_recovered));
}

void HostComponentRuntime::InterruptActions() { coordinator_.InterruptActions(); }

void HostComponentRuntime::Refresh() {
  if (hooks_.refresh_shared_transport) hooks_.refresh_shared_transport();
  coordinator_.Refresh();
}

std::optional<std::string> HostComponentRuntime::ReserveForApply(
    const std::vector<std::string> &component_ids, std::vector<std::string> &reserved) {
  return coordinator_.ReserveForApply(component_ids, reserved);
}

void HostComponentRuntime::ReleaseApplyReservation(
    const std::vector<std::string> &component_ids) noexcept {
  coordinator_.ReleaseApplyReservation(component_ids);
}

HostComponentRuntime::Publication HostComponentRuntime::Read() const {
  std::lock_guard lock(mutex_);
  return publication_;
}

std::shared_ptr<core::IComponentAdapter> HostComponentRuntime::Adapter(
    const std::string &component_id) const {
  std::lock_guard lock(mutex_);
  for (const auto &[key, wrapper] : wrappers_)
    if (wrapper->component_id() == component_id) return wrapper->adapter();
  return nullptr;
}

HostComponentRuntime::EffectAdapter HostComponentRuntime::AdapterForEffect(
    const std::string &component_id) const {
  std::lock_guard lock(mutex_);
  for (const auto &[key, wrapper] : wrappers_)
    if (wrapper->component_id() == component_id)
      return {wrapper->adapter(), wrapper->transport_consumer(), wrapper->adapter_transport_consumer()};
  return {};
}

void HostComponentRuntime::Publish(const std::vector<core::ComponentState> &states) {
  {
    std::lock_guard lock(mutex_);
    ++publication_.revision;
    publication_.states = states;
  }
  if (hooks_.observe_states) hooks_.observe_states(states);
  if (hooks_.published) hooks_.published();
}

bool HasCompiledComponentRoutes(
    const std::vector<core::DesiredControlConfigurationDTO> &compiled_routes,
    const std::vector<core::ControlBindingDTO> &bindings,
    const core::ControllableComponentId &component) {
  return std::any_of(compiled_routes.begin(), compiled_routes.end(), [&](const auto &desired) {
    return std::any_of(desired.binding_routes.begin(), desired.binding_routes.end(),
                       [&](const auto &route) {
                         return std::any_of(bindings.begin(), bindings.end(), [&](const auto &row) {
                           return row.control_binding_id == route.control_binding_id &&
                                  row.controllable_component_id == component;
                         });
                       });
  });
}

}  // namespace manyforge::roboplan_backend::host_detail
