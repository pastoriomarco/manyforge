#include <manyforge_roboplan/ros2_control_component_adapter.hpp>

#include <utility>

namespace manyforge::roboplan_backend {

Ros2ControlComponentAdapter::Ros2ControlComponentAdapter(
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle,
    const std::chrono::nanoseconds lifecycle_command_timeout,
    MockResetPositionPhaseCallback reset_phase, StreamsFn streams, const bool simulated,
    const std::optional<std::chrono::nanoseconds> goal_result_bound)
    : handle_(std::move(handle)), lifecycle_command_timeout_(lifecycle_command_timeout),
      reset_phase_(std::move(reset_phase)), streams_(std::move(streams)),
      simulated_(simulated), goal_result_bound_(goal_result_bound) {}

core::ComponentState Ros2ControlComponentAdapter::State(const std::int64_t now_ns) const {
  auto observation = handle_->ReadObservation(now_ns);
  if (streams_) observation.streams = streams_(now_ns);
  observation.simulated = simulated_;
  if (goal_result_bound_) observation.goal_result_bound_ns = goal_result_bound_->count();
  return ros2_control_component_state(observation, now_ns);
}

core::ActionResult Ros2ControlComponentAdapter::Run(const core::ComponentAction action,
                                                    const std::stop_token stop) {
  using Action = core::ComponentAction;
  const bool arm = handle_->motion_domain().has_value();
  // M7: a just-launched component's controllers are spawned within its
  // launch's own bound before its lifecycle command time starts.
  if (action == Action::kPrepare)
    if (auto refused = handle_->WaitForControllerLaunch(stop))
      return ros2_control_action_result(action, *refused);
  // The component's lifecycle command time bounds the whole action.
  const auto deadline = std::chrono::steady_clock::now() + lifecycle_command_timeout_;
  const auto result = [&]() -> RetainedRos2ControlSwitchResult {
    switch (action) {
    case Action::kPrepare:
      return arm ? handle_->PrepareArmController(stop, deadline)
                 : handle_->PrepareGripperController(stop, deadline);
    case Action::kEnable:
      return arm ? handle_->ActivateArmController(stop, deadline)
                 : handle_->SwitchGripperController(true, stop, deadline);
    case Action::kDisable:
      return arm ? handle_->DeactivateArmController(stop, deadline)
                 : handle_->SwitchGripperController(false, stop, deadline);
    case Action::kRecover:
      // The guard's reset, then the controller left inactive and prepared;
      // the next Enable binds a new epoch.
      return arm ? handle_->RecoverArmController(stop, deadline)
                 : handle_->SwitchGripperController(false, stop, deadline);
    case Action::kResetPosition:
      if (arm && reset_phase_) return handle_->ResetMockPosition(stop, reset_phase_);
      break;
    case Action::kPowerOff:
    case Action::kReboot:
    case Action::kSimReset:
      break;
    }
    return {RetainedRos2ControlSwitchOutcome::kNotSent,
            core::ErrorDTO{"component_action_unsupported",
                           "This component does not offer this action", {}}};
  }();
  return ros2_control_action_result(action, result);
}

std::vector<core::Capability> Ros2ControlComponentAdapter::capabilities() const {
  if (reset_phase_ && handle_->mock_reset_position_target().has_value())
    return {core::Capability::kResetPosition};
  return {};
}

core::ActionResult ros2_control_action_result(const core::ComponentAction action,
                                              const RetainedRos2ControlSwitchResult &result) {
  core::ActionResult out;
  out.action = action;
  switch (result.outcome) {
  case RetainedRos2ControlSwitchOutcome::kSucceeded:
    out.outcome = core::ActionOutcome::kSucceeded;
    return out;
  case RetainedRos2ControlSwitchOutcome::kUncertain:
    out.outcome = core::ActionOutcome::kUnknown;
    break;
  case RetainedRos2ControlSwitchOutcome::kNotSent:
  case RetainedRos2ControlSwitchOutcome::kFailed:
  case RetainedRos2ControlSwitchOutcome::kFailedNoEffect:
  case RetainedRos2ControlSwitchOutcome::kFailedEffectObserved:
    out.outcome = core::ActionOutcome::kFailed;
    break;
  }
  // A lifecycle step the handle refused carries the handle's generic motion
  // code; for a component action it is the component's refusal.
  if (result.error)
    out.error = core::ActionError{result.error->code == "execution_rejected"
                                      ? std::string("component_action_refused")
                                      : result.error->code,
                                  result.error->message, std::nullopt, std::nullopt};
  return out;
}

} // namespace manyforge::roboplan_backend
