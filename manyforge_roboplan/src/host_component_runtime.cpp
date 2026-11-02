#include "host_component_runtime.hpp"

#include <algorithm>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

/// Opens the component's command routes inside its `enable`, after the adapter
/// succeeded, so `enabled` is never published with closed routes.
class HostComponentRuntime::RouteOpeningAdapter final : public core::IComponentAdapter {
 public:
  RouteOpeningAdapter(std::string component_id, std::shared_ptr<core::IComponentAdapter> adapter,
                      const Hooks &hooks)
      : component_id_(std::move(component_id)), adapter_(std::move(adapter)), hooks_(hooks) {}

  [[nodiscard]] core::ComponentState State(const std::int64_t now_ns) const override {
    return adapter_->State(now_ns);
  }

  [[nodiscard]] core::ActionResult Run(const core::ComponentAction action,
                                       const std::stop_token stop) override {
    // The component's routes open before its adapter enables it (as M7's route
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

 private:
  std::string component_id_;
  std::shared_ptr<core::IComponentAdapter> adapter_;
  const Hooks &hooks_;
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
      auto found = wrappers_.find(key);
      auto wrapper = found != wrappers_.end() && found->second->component_id() ==
                                                     component.component_id
                         ? found->second
                         : std::make_shared<RouteOpeningAdapter>(component.component_id,
                                                                 component.adapter, hooks_);
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

void HostComponentRuntime::InterruptActions() { coordinator_.InterruptActions(); }

void HostComponentRuntime::Refresh() { coordinator_.Refresh(); }

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
