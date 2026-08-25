#include <manyforge_core/execution_stop_support.hpp>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>

namespace manyforge::core {

namespace {

std::string backend_hard_stop_level(IExecutionBackend& backend) {
  try {
    const auto cap = backend.get_motion_interrupt_stop_capability();
    return cap.hard_stop_level.value_or("unknown");
  } catch (const std::exception&) {
    return "unknown";
  }
}

std::string hard_stop_floor_role_from_level(const std::string& level) {
  if (level == "hard_deceleration") {
    return "hard_deceleration";
  }
  if (level == "native_immediate_hold" ||
      level == "jtc_cancel_unprofiled_hold") {
    return "immediate_hard_stop";
  }
  return "immediate_hard_stop";
}

std::string hard_stop_floor_command_source_from_level(
    const std::string& level) {
  if (level == "hard_deceleration") {
    return "jtc_cancel_decelerate_on_cancel";
  }
  if (level == "native_immediate_hold") {
    return "native_simulated_stop";
  }
  if (level == "jtc_cancel_unprofiled_hold") {
    return "jtc_cancel_unprofiled_hold";
  }
  return "backend_stop";
}

void add_optional_double_detail(std::map<std::string, std::string>& details,
                                const char* key,
                                const std::optional<double>& value) {
  if (value.has_value()) {
    details[key] = std::to_string(*value);
  }
}

void merge_controlled_stop_diagnostics(
    std::map<std::string, std::string>& details,
    const InterruptStructuredValue& diagnostics) {
  for (const auto& [key, value] : diagnostics) {
    const std::string projected = interrupt_structured_scalar_text(value);
    if (details.contains(key)) {
      details["controlled_stop_detail_" + key] = projected;
    } else {
      details[key] = projected;
    }
  }
}

constexpr std::array<StopFallbackQualificationRow, 1>
    kStopFallbackQualificationMatrix{{
        {MotionInterruptStopMode::kSoftOnPath,
         MotionInterruptStopMode::kSpringBackHold,
         MotionInterruptNodeCategory::kGeneric,
         {MotionInterruptStopCommandSource::kDiagnosticOther,
          MotionInterruptStopCommandSource::kFjtPreemptHold,
          MotionInterruptStopCommandSource::kNativeSimulatedStop,
          MotionInterruptStopCommandSource::kFjtPreemptPathRamp,
          MotionInterruptStopCommandSource::kNativeSimulatedPathRamp},
         5,
         "4.5G-soft_on_path-to-spring_back_hold-generic-v1",
         "path-preserving soft stop unavailable; generic node accepts the "
         "qualified spring-back stop",
         "fallback_reason, requested/executed modes, command source, stop "
         "snapshot, stop timing",
         "in_tree_qualified; live_adapter_evidence_required_for_release"},
    }};

}  // namespace

const char* stop_mode_string(MotionInterruptStopMode mode) {
  switch (mode) {
    using enum MotionInterruptStopMode;
    case kSpringBackHold: return "spring_back_hold";
    case kSoftOnPath: return "soft_on_path";
    case kHardDeceleration: return "hard_deceleration";
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

StopResultDTO request_hard_stop_floor_once(
    IExecutionBackend& backend,
    const std::shared_ptr<HardStopFloorAttempt>& attempt) {
  std::lock_guard<std::mutex> lock(attempt->mutex);
  if (!attempt->attempted) {
    attempt->result = backend.stop();
    attempt->attempted = true;
  }
  return attempt->result;
}

std::optional<StopResultDTO> hard_stop_floor_result_if_attempted(
    const std::shared_ptr<HardStopFloorAttempt>& attempt) {
  std::lock_guard<std::mutex> lock(attempt->mutex);
  if (!attempt->attempted) {
    return std::nullopt;
  }
  return attempt->result;
}

void add_hard_stop_floor_details(
    std::map<std::string, std::string>& details,
    IExecutionBackend& backend) {
  const auto level = backend_hard_stop_level(backend);
  details["hard_stop_level"] = level;
  details["hard_stop_floor"] = hard_stop_floor_role_from_level(level);
  details["hard_stop_floor_command_source"] =
      hard_stop_floor_command_source_from_level(level);
}

std::map<std::string, std::string> controlled_stop_success_event_details(
    const MotionInterruptStopProfileDTO& profile,
    const ControlledStopResultDTO& result,
    IExecutionBackend& backend,
    const std::string& phase) {
  std::map<std::string, std::string> details{
      {"phase", phase},
      {"controlled_stop_success", "true"},
      {"requested_stop_profile_id", profile.profile_id},
      {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
      {"executed_stop_mode", stop_mode_string(result.executed_stop_mode)},
      {"stop_command_source",
       stop_command_source_string(result.stop_command_source)},
      {"backend_stop_confirmed",
       result.backend_stop_confirmed ? "true" : "false"},
      {"completed_by_natural_completion",
       result.completed_by_natural_completion ? "true" : "false"},
      {"confirmation_latency_ns",
       std::to_string(result.confirmation_latency_ns)},
      {"deceleration_time_s",
       std::to_string(motion_interrupt_deceleration_time_or_default(profile))},
      {"emergency_fallback", "none"},
      {"hard_stop_transitioned", "false"}};
  if (result.fallback_reason.has_value()) {
    details["fallback_reason"] = *result.fallback_reason;
  }
  add_optional_double_detail(details, "stop_distance_rad",
                             result.stop_distance_rad);
  add_optional_double_detail(details, "path_deviation_max_rad",
                             result.path_deviation_max_rad);
  add_optional_double_detail(details, "initial_path_deviation_rad",
                             result.initial_path_deviation_rad);
  merge_controlled_stop_diagnostics(details, result.diagnostics);
  add_hard_stop_floor_details(details, backend);
  return details;
}

std::int64_t effective_controlled_stop_deadline_ns(
    const MotionInterruptStopProfileDTO& profile,
    Duration stop_completion_tolerance_ns) {
  const auto deceleration_ns = static_cast<std::int64_t>(
      motion_interrupt_deceleration_time_or_default(profile) * 1e9);
  return deceleration_ns + std::max<Duration>(0, stop_completion_tolerance_ns);
}

std::chrono::steady_clock::duration effective_controlled_stop_deadline_duration(
    const MotionInterruptStopProfileDTO& profile,
    Duration stop_completion_tolerance_ns) {
  const auto deadline_ns =
      std::max<std::int64_t>(
          0, effective_controlled_stop_deadline_ns(
                 profile, stop_completion_tolerance_ns));
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::nanoseconds(deadline_ns));
}

std::optional<double> measured_max_abs_velocity_from_snapshot(
    const ExecutionSnapshot& snapshot) {
  if (snapshot.joint_velocities_rad_s.empty() ||
      snapshot.joint_velocities_rad_s.size() !=
          snapshot.joint_positions_rad.size()) {
    return std::nullopt;
  }
  double max_abs = 0.0;
  for (double v : snapshot.joint_velocities_rad_s) {
    max_abs = std::max(max_abs, std::abs(v));
  }
  return max_abs;
}

bool all_joint_positions_finite(const ExecutionSnapshot& snapshot) {
  if (snapshot.joint_positions_rad.empty()) {
    return false;
  }
  for (double q : snapshot.joint_positions_rad) {
    if (!std::isfinite(q)) {
      return false;
    }
  }
  return true;
}

bool all_joint_velocities_finite_when_present(
    const ExecutionSnapshot& snapshot) {
  for (double v : snapshot.joint_velocities_rad_s) {
    if (!std::isfinite(v)) {
      return false;
    }
  }
  return true;
}

std::optional<double> parse_positive_double_detail(
    const std::map<std::string, std::string>& details,
    const std::string& key) {
  const auto it = details.find(key);
  if (it == details.end()) {
    return std::nullopt;
  }
  try {
    std::size_t parsed = 0;
    const double value = std::stod(it->second, &parsed);
    if (parsed != it->second.size() || !std::isfinite(value) ||
        value <= 0.0) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
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

bool is_command_authoritative_stop_snapshot(const ExecutionSnapshot& snapshot) {
  return snapshot.state_source.rfind("commanded_sim", 0) == 0 ||
         snapshot.state_source == "controlled_stop_command_reference";
}

MeasuredRestWaitResult wait_for_measured_rest_after_confirmed_stop(
    IExecutionBackend& backend,
    double settle_velocity_threshold_rad_s,
    TimePoint deadline,
    IClock& clock,
    IScheduler& scheduler,
    std::stop_token stop_token,
    std::function<bool()> abort_requested) {
  struct PollWaitState final {
    std::mutex mutex;
    std::condition_variable ready;
    bool timer_fired = false;
    bool cancellation_requested = false;
  };

  MeasuredRestWaitResult result;
  while (true) {
    if (abort_requested && abort_requested()) {
      result.at_rest = false;
      result.timed_out = true;
      result.aborted = true;
      return result;
    }
    const auto snapshot = backend.get_execution_snapshot();
    if (is_command_authoritative_stop_snapshot(snapshot)) {
      result.at_rest = true;
      return result;
    }
    const auto max_abs_velocity =
        measured_max_abs_velocity_from_snapshot(snapshot);
    if (!max_abs_velocity.has_value()) {
      return result;
    }
    result.settle_signal_available = true;
    result.max_abs_velocity_rad_s = *max_abs_velocity;
    result.at_rest = *max_abs_velocity <= settle_velocity_threshold_rad_s;
    if (result.at_rest) {
      return result;
    }
    if (stop_token.stop_requested()) {
      result.timed_out = true;
      return result;
    }
    const auto now = clock.now();
    if (now >= deadline) {
      result.timed_out = true;
      return result;
    }

    const auto wake_deadline =
        deadline - now > kExecutionPollNs ? now + kExecutionPollNs : deadline;
    auto wait_state = std::make_shared<PollWaitState>();
    std::stop_callback cancellation(stop_token, [wait_state]() {
      {
        std::lock_guard lock(wait_state->mutex);
        wait_state->cancellation_requested = true;
      }
      wait_state->ready.notify_all();
    });
    const auto wake_handle = scheduler.schedule(
        wake_deadline,
        [wait_state]() {
          {
            std::lock_guard lock(wait_state->mutex);
            wait_state->timer_fired = true;
          }
          wait_state->ready.notify_all();
        },
        SchedulerPriority::StopControl);

    bool timer_fired = false;
    {
      std::unique_lock lock(wait_state->mutex);
      wait_state->ready.wait(lock, [&wait_state]() {
        return wait_state->timer_fired || wait_state->cancellation_requested;
      });
      timer_fired = wait_state->timer_fired;
    }
    if (!timer_fired) {
      scheduler.cancel(wake_handle);
    }
  }
}

bool operator_controlled_stop_allowed(ExecutionState state) {
  return state == ExecutionState::kExecuting ||
         state == ExecutionState::kPaused ||
         state == ExecutionState::kStopping;
}

const StopFallbackQualificationRow* qualified_stop_fallback_pair(
    MotionInterruptStopMode requested,
    MotionInterruptStopMode executed,
    MotionInterruptNodeCategory node_category) {
  for (const auto& row : kStopFallbackQualificationMatrix) {
    if (row.requested == requested && row.executed == executed &&
        row.node_category == node_category) {
      return &row;
    }
  }
  return nullptr;
}

bool fallback_row_allows_command_source(
    const StopFallbackQualificationRow& row,
    MotionInterruptStopCommandSource source) {
  for (std::size_t i = 0; i < row.allowed_command_source_count; ++i) {
    if (row.allowed_command_sources[i] == source) {
      return true;
    }
  }
  return false;
}

bool robot_state_size_ordering_ok(const RobotStateDTO& state) {
  if (state.joint_names.empty()) {
    return false;
  }
  if (state.joint_names.size() != state.positions_rad.size()) {
    return false;
  }
  for (const auto& name : state.joint_names) {
    if (name.empty()) {
      return false;
    }
  }
  return true;
}

std::optional<ExecutionStopSnapshotDTO> capture_stop_snapshot(
    const ExecutionSnapshot& post_stop_backend_snapshot,
    const std::optional<RobotStateDTO>& observed_state_provider_state,
    bool backend_stop_confirmed,
    MotionInterruptNodeCategory node_category,
    std::int64_t captured_at_ns,
    double settle_velocity_threshold_rad_s,
    std::string& failure_reason) {
  ExecutionStopSnapshotDTO snap;
  snap.captured_at_ns = captured_at_ns;
  snap.backend_stop_confirmed = backend_stop_confirmed;
  const bool command_authoritative =
      is_command_authoritative_stop_snapshot(post_stop_backend_snapshot);

  const auto apply_settle_signal = [&](ExecutionStopSnapshotDTO& s) {
    if (s.robot_state.velocities_rad_s.empty() ||
        s.robot_state.velocities_rad_s.size() !=
            s.robot_state.positions_rad.size()) {
      return;
    }
    double max_abs = 0.0;
    for (double v : s.robot_state.velocities_rad_s) {
      max_abs = std::max(max_abs, std::abs(v));
    }
    s.max_abs_velocity_rad_s = max_abs;
    s.at_rest = command_authoritative ||
                max_abs <= settle_velocity_threshold_rad_s;
  };

  if (!post_stop_backend_snapshot.joint_positions_rad.empty() &&
      observed_state_provider_state &&
      observed_state_provider_state->joint_names.size() ==
          post_stop_backend_snapshot.joint_positions_rad.size()) {
    snap.source = ExecutionStopSnapshotSource::kExecutionBackendFeedback;
    snap.robot_state.joint_names = observed_state_provider_state->joint_names;
    snap.robot_state.positions_rad =
        post_stop_backend_snapshot.joint_positions_rad;
    if (post_stop_backend_snapshot.joint_velocities_rad_s.size() ==
        snap.robot_state.positions_rad.size()) {
      snap.robot_state.velocities_rad_s =
          post_stop_backend_snapshot.joint_velocities_rad_s;
    }
    if (post_stop_backend_snapshot.progress > 0.0 &&
        post_stop_backend_snapshot.progress <= 1.0) {
      snap.trajectory_progress = post_stop_backend_snapshot.progress;
    }
    if (!robot_state_size_ordering_ok(snap.robot_state)) {
      failure_reason = "stop_snapshot_robot_state_size_ordering_invalid";
      return std::nullopt;
    }
    apply_settle_signal(snap);
    return snap;
  }

  if (node_category == MotionInterruptNodeCategory::kPrecisionContact) {
    failure_reason =
        "stop_snapshot_observed_state_rejected_for_precision_contact";
    return std::nullopt;
  }
  if (observed_state_provider_state) {
    snap.source = ExecutionStopSnapshotSource::kObservedStateProvider;
    snap.robot_state = *observed_state_provider_state;
    if (!robot_state_size_ordering_ok(snap.robot_state)) {
      failure_reason = "stop_snapshot_robot_state_size_ordering_invalid";
      return std::nullopt;
    }
    apply_settle_signal(snap);
    return snap;
  }
  failure_reason = "stop_snapshot_no_usable_state_available";
  return std::nullopt;
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

ErrorDTO ensure_operator_intervention_details(ErrorDTO error,
                                              std::string phase,
                                              ExecutionState previous_state,
                                              std::uint64_t execution_id,
                                              const std::string& move_id) {
  error.details.try_emplace("requires_operator_intervention", "true");
  error.details.try_emplace("phase", std::move(phase));
  error.details.try_emplace("previous_state", to_string(previous_state));
  error.details.try_emplace("execution_id", std::to_string(execution_id));
  error.details.try_emplace("move_id", move_id);
  error.details.try_emplace("stop_completion_timeout_s",
                            std::to_string(kExecutionDrainTimeoutS));
  return error;
}

}  // namespace manyforge::core
