#include <manyforge_core/execution_stop_support.hpp>

namespace manyforge::core {

const char* stop_mode_string(MotionInterruptStopMode mode) {
  switch (mode) {
    using enum MotionInterruptStopMode;
    case kSpringBackHold: return "spring_back_hold";
    case kSoftOnPath: return "soft_on_path";
    case kHardDeceleration: return "hard_deceleration";
    case kSpringBackFast: return "spring_back_fast";
  }
  return "unknown";
}

const char* stop_command_source_string(
    MotionInterruptStopCommandSource source) {
  switch (source) {
    using enum MotionInterruptStopCommandSource;
    case kFjtPreemptHold: return "fjt_preempt_hold";
    case kFjtPreemptPathRamp: return "fjt_preempt_path_ramp";
    case kJtcSpeedScalingRamp: return "jtc_speed_scaling_ramp";
    case kJtcCancelDecelerateOnCancel: return "jtc_cancel_decelerate_on_cancel";
    case kJtcCancelUnprofiledHold: return "jtc_cancel_unprofiled_hold";
    case kNativeSimulatedStop: return "native_simulated_stop";
    case kNativeSimulatedPathRamp: return "native_simulated_path_ramp";
    case kRobotNativeStop: return "robot_native_stop";
    case kDiagnosticOther: return "diagnostic_other";
  }
  return "unknown";
}

bool is_classified_stop_intervention(
    const std::map<std::string, std::string>& details) {
  const auto reason_it = details.find("reason");
  const std::string reason =
      reason_it != details.end() ? reason_it->second : std::string{};
  const bool has_stop_mode =
      details.contains("requested_stop_mode") ||
      details.contains("executed_stop_mode") ||
      details.contains("hard_stop_transitioned");
  if (reason == "stop_not_settled_measured_velocity" ||
      reason == "operator_stop_total_failure" ||
      reason == "in_stop_completion_timeout" ||
      reason == "stop_path_collision" ||
      reason == "hard_deceleration_not_settled") {
    return true;
  }
  if ((reason == "fault_stop_total_failure" ||
       reason == "fault_preempt_stop_path_collision" ||
       reason == "fault_preempt_pending_timeout" ||
       reason == "fault_preempt_backend_failed") &&
      has_stop_mode) {
    return true;
  }
  const auto phase_it = details.find("phase");
  const bool stop_phase =
      phase_it != details.end() &&
      (phase_it->second == "operator_stop" ||
       phase_it->second == "motion_trip_stop" ||
       phase_it->second == "motion_trip_stop_path_collision" ||
       phase_it->second == "preempt_trajectory" ||
       phase_it->second == "fault_preempt" ||
       phase_it->second == "fault_preempt_pending_watchdog");
  return stop_phase && has_stop_mode;
}

bool drain_execution_handle_bounded(ExecutionHandle& handle) {
  if (!handle.wait_for_completion) {
    return true;
  }
  for (int poll = 0; poll < kExecutionDrainMaxPolls; ++poll) {
    if (handle.wait_for_completion(kExecutionPollNs)) {
      return true;
    }
  }
  return false;
}

ErrorDTO operator_intervention_error(ErrorCode code,
                                     std::string message,
                                     std::string phase,
                                     ExecutionState previous_state,
                                     std::uint64_t execution_id,
                                     const std::string& move_id) {
  return ErrorDTO{
      std::string(to_string(code)),
      std::move(message),
      {{"requires_operator_intervention", "true"},
       {"phase", std::move(phase)},
       {"previous_state", to_string(previous_state)},
       {"execution_id", std::to_string(execution_id)},
       {"move_id", move_id},
       {"stop_completion_timeout_s", std::to_string(kExecutionDrainTimeoutS)}}};
}

}  // namespace manyforge::core
