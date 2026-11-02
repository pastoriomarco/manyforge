#include <manyforge_roboplan/passive_component_adapter.hpp>

#include <utility>

namespace manyforge::roboplan_backend {

PassiveComponentAdapter::PassiveComponentAdapter(std::function<bool()> action_running)
    : action_running_(std::move(action_running)) {}

core::ComponentState PassiveComponentAdapter::State(const std::int64_t now_ns) const {
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  {
    std::lock_guard lock(mutex_);
    state.lifecycle = lifecycle_;
  }
  state.activity = action_running_ && action_running_() ? core::ComponentActivity::kBusy
                                                        : core::ComponentActivity::kIdle;
  return state;
}

core::ActionResult PassiveComponentAdapter::Run(const core::ComponentAction action,
                                                std::stop_token) {
  core::ActionResult result;
  result.action = action;
  std::lock_guard lock(mutex_);
  switch (action) {
  case core::ComponentAction::kPrepare:
  case core::ComponentAction::kDisable:
  case core::ComponentAction::kRecover:
    lifecycle_ = core::ComponentLifecycle::kPrepared;
    result.outcome = core::ActionOutcome::kSucceeded;
    return result;
  case core::ComponentAction::kEnable:
    lifecycle_ = core::ComponentLifecycle::kEnabled;
    result.outcome = core::ActionOutcome::kSucceeded;
    return result;
  case core::ComponentAction::kResetPosition:
  case core::ComponentAction::kPowerOff:
  case core::ComponentAction::kReboot:
  case core::ComponentAction::kSimReset:
    break;
  }
  result.outcome = core::ActionOutcome::kFailed;
  result.error = core::ActionError{"component_action_unsupported",
                                   "This component does not offer this action", std::nullopt,
                                   std::nullopt};
  return result;
}

std::vector<core::Capability> PassiveComponentAdapter::capabilities() const { return {}; }

} // namespace manyforge::roboplan_backend
