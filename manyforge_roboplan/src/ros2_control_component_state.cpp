#include <manyforge_roboplan/ros2_control_component_state.hpp>

#include <algorithm>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::Fault fault(const core::FaultCode code, std::string message,
                                const core::FaultOperatorAction action,
                                const std::int64_t now_ns) {
  return core::Fault{code,  std::move(message), action != core::FaultOperatorAction::kNone,
                     action, std::nullopt,       std::nullopt,
                     {},     now_ns};
}

} // namespace

core::ComponentState ros2_control_component_state(const Ros2ControlObservation &observation,
                                                  const std::int64_t now_ns) {
  using Lifecycle = core::ComponentLifecycle;
  using Action = core::FaultOperatorAction;
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  state.streams = observation.streams;
  const bool reachable = observation.local_owner_ready.has_value() ||
                         (observation.controller_manager_present && observation.observed);
  const auto stale = std::find_if(
      observation.streams.begin(), observation.streams.end(),
      [](const auto &stream) { return stream.freshness == core::StreamFreshness::kStale; });

  if (observation.owner_stalled) {
    state.fault = fault(core::FaultCode::kOwnerStalled, *observation.owner_stalled,
                        Action::kRecover, now_ns);
    state.lifecycle = Lifecycle::kFault;
    state.activity = core::ComponentActivity::kUnknown;
    return state;
  }
  if (observation.process_fault) {
    state.fault = fault(core::FaultCode::kProcessExited, *observation.process_fault,
                        Action::kRecover, now_ns);
  } else if (observation.guard_fault) {
    state.fault = fault(core::FaultCode::kCommunication, *observation.guard_fault,
                        Action::kRecover, now_ns);
  } else if (observation.tool_context_fault) {
    state.fault = fault(core::FaultCode::kVendorFault, *observation.tool_context_fault,
                        Action::kRecover, now_ns);
  } else if (reachable && stale != observation.streams.end() && !observation.simulated) {
    state.fault = fault(core::FaultCode::kStreamStale,
                        "The " + stale->stream_id + " stream stopped arriving",
                        Action::kRecover, now_ns);
  }

  if (state.fault) {
    state.lifecycle = Lifecycle::kFault;
  } else if (!reachable) {
    state.lifecycle = Lifecycle::kOffline;
    state.reason = core::ComponentStateReason{
        "controller_manager_unavailable", "The component's controller manager is not reachable"};
  } else if (observation.local_owner_ready.has_value()) {
    state.lifecycle = !*observation.local_owner_ready ? Lifecycle::kNotPrepared
                      : observation.local_owner_enabled ? Lifecycle::kEnabled
                                                        : Lifecycle::kPrepared;
  } else if (observation.controller_loaded && observation.hardware_active) {
    state.lifecycle =
        observation.controller_active ? Lifecycle::kEnabled : Lifecycle::kPrepared;
  } else {
    state.lifecycle = Lifecycle::kNotPrepared;
  }
  // A simulator that is not playing stops the
  // streams; the component keeps its controller's lifecycle and shows why.
  if (!state.fault && reachable && observation.simulated && stale != observation.streams.end())
    state.reason = core::ComponentStateReason{
        "simulation_not_playing", "The simulation is not playing"};

  // An open goal whose result cannot come back (the manager or the launch
  // is gone), or hasn't come back by its expected end plus the lane's bound,
  // is unknown; otherwise the controller's own report decides.
  const bool result_overdue = observation.goal_past_end_ns &&
                              observation.goal_result_bound_ns &&
                              *observation.goal_past_end_ns > *observation.goal_result_bound_ns;
  if (observation.goal_outcome_unknown || (observation.goal_running &&
      (!reachable || observation.process_fault || result_overdue))) {
    state.activity = core::ComponentActivity::kUnknown;
  } else if (observation.goal_running || observation.decelerating ||
             observation.reset_in_progress) {
    state.activity = core::ComponentActivity::kBusy;
  } else {
    state.activity = core::ComponentActivity::kIdle;
  }
  return state;
}

} // namespace manyforge::roboplan_backend
