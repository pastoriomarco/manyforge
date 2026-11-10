#include <manyforge_roboplan/simulator_component_adapter.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend {

core::ComponentState SimulatorComponentState(const std::optional<SimulationState> &observed,
                                             const std::int64_t now_ns) {
  core::ComponentState state;
  state.activity = core::ComponentActivity::kIdle;
  state.observed_at_ns = now_ns;
  if (!observed) {
    state.lifecycle = core::ComponentLifecycle::kOffline;
    state.reason = core::ComponentStateReason{std::string(kSimulatorUnreachableReason),
                                              "The simulator doesn't answer."};
    return state;
  }
  switch (*observed) {
    case SimulationState::kPlaying:
      state.lifecycle = core::ComponentLifecycle::kEnabled;
      break;
    case SimulationState::kPaused:
      state.lifecycle = core::ComponentLifecycle::kPrepared;
      state.reason = core::ComponentStateReason{std::string(kSimulationPausedReason),
                                                "The simulation is paused."};
      break;
    case SimulationState::kStopped:
      state.lifecycle = core::ComponentLifecycle::kPrepared;
      state.reason = core::ComponentStateReason{std::string(kSimulationStoppedReason),
                                                "The simulation is stopped."};
      break;
  }
  return state;
}

SimulatorComponentAdapter::SimulatorComponentAdapter(std::shared_ptr<ISimulationControl> control)
    : control_(std::move(control)) {
  if (!control_) throw std::invalid_argument("the simulator adapter needs its simulator");
}

core::ComponentState SimulatorComponentAdapter::State(const std::int64_t now_ns) const {
  const auto observed = control_->Observed();
  return SimulatorComponentState(observed, now_ns);
}

SimulationCall SimulatorComponentAdapter::Set(const SimulationState state,
                                              const std::stop_token &stop) {
  auto call = control_->Set(state, stop);
  return call;
}

core::ActionResult SimulatorComponentAdapter::Run(const core::ComponentAction action,
                                                  const std::stop_token stop) {
  SimulationCall call;
  switch (action) {
    case core::ComponentAction::kPrepare:
    case core::ComponentAction::kRecover:
      call = control_->Read(stop);
      break;
    case core::ComponentAction::kEnable:
      call = Set(SimulationState::kPlaying, stop);
      break;
    case core::ComponentAction::kDisable:
      call = Set(SimulationState::kPaused, stop);
      break;
    case core::ComponentAction::kSimReset:
      call = control_->Reset(stop);
      break;
    default:
      call = SimulationCall{core::ActionOutcome::kFailed,
                            core::ActionError{"component_action_unsupported",
                                              "A simulator doesn't offer this action.",
                                              std::nullopt, std::nullopt}};
      break;
  }
  core::ActionResult result;
  result.action = action;
  result.outcome = call.outcome;
  result.error = std::move(call.error);
  return result;
}

std::vector<core::Capability> SimulatorComponentAdapter::capabilities() const {
  return {core::Capability::kSimReset};
}

}  // namespace manyforge::roboplan_backend
