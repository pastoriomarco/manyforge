#include <manyforge_roboplan/roboplan_execution_backend.hpp>

#include <Eigen/Dense>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

constexpr auto kExecutionTick = std::chrono::milliseconds(10);
constexpr double kRuntimeScaleSlewPerSecond = 4.0;  // 0→1 in 250 ms.

double clamp_progress(double progress) {
  return std::max(0.0, std::min(1.0, progress));
}

template <typename Rep, typename Period>
std::string seconds_string(std::chrono::duration<Rep, Period> duration) {
  return std::to_string(std::chrono::duration<double>(duration).count());
}

}  // namespace

RoboPlanExecutionBackend::RoboPlanExecutionBackend(
    std::shared_ptr<roboplan::Scene> scene, std::string group_name,
    core::IClock& clock,
    std::shared_ptr<std::mutex> scene_mutex,
    RoboPlanExecutionBackendConfig config)
    : scene_(std::move(scene)),
      group_name_(std::move(group_name)),
      clock_(clock),
      scene_mutex_(std::move(scene_mutex)),
      config_(config) {
  if (config_.reset_completion_timeout_s <= 0.0 ||
      !std::isfinite(config_.reset_completion_timeout_s)) {
    throw std::invalid_argument(
        "reset_completion_timeout_s must be finite and > 0");
  }
  if (config_.streaming_stop_completion_timeout_s <= 0.0 ||
      !std::isfinite(config_.streaming_stop_completion_timeout_s)) {
    throw std::invalid_argument(
        "streaming_stop_completion_timeout_s must be finite and > 0");
  }
  if (config_.soft_on_path_stop_traj_delta_s < 0.0 ||
      !std::isfinite(config_.soft_on_path_stop_traj_delta_s)) {
    throw std::invalid_argument(
        "soft_on_path_stop_traj_delta_s must be finite and >= 0");
  }
  if (config_.soft_on_path_sample_period_s <= 0.0 ||
      !std::isfinite(config_.soft_on_path_sample_period_s)) {
    throw std::invalid_argument(
        "soft_on_path_sample_period_s must be finite and > 0");
  }
  if (config_.soft_on_path_max_initial_velocity_deviation_rad_s.has_value() &&
      (!std::isfinite(*config_.soft_on_path_max_initial_velocity_deviation_rad_s) ||
       *config_.soft_on_path_max_initial_velocity_deviation_rad_s < 0.0)) {
    throw std::invalid_argument(
        "soft_on_path_max_initial_velocity_deviation_rad_s must be finite and >= 0");
  }
  if (config_.soft_on_path_max_initial_path_deviation_rad.has_value() &&
      (!std::isfinite(*config_.soft_on_path_max_initial_path_deviation_rad) ||
       *config_.soft_on_path_max_initial_path_deviation_rad < 0.0)) {
    throw std::invalid_argument(
        "soft_on_path_max_initial_path_deviation_rad must be finite and >= 0");
  }
  for (const auto& [joint, jerk] : config_.soft_on_path_max_jerk_rad_s3) {
    if (joint.empty() || !std::isfinite(jerk) || jerk <= 0.0) {
      throw std::invalid_argument(
          "soft_on_path_max_jerk_rad_s3 entries must have non-empty joint names and finite positive values");
    }
  }
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
  state_.snapshot.timestamp_ns = clock_.now();
}

RoboPlanExecutionBackend::~RoboPlanExecutionBackend() {
  (void)reset();
  // jthread auto-joins
}

void RoboPlanExecutionBackend::publish_snapshot(
    core::ExecutionState state, double progress,
    const std::vector<double>& joint_positions,
    std::optional<core::ErrorDTO> error) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_.snapshot.timestamp_ns = clock_.now();
  state_.snapshot.sequence_number++;
  state_.snapshot.state = state;
  state_.snapshot.progress = clamp_progress(progress);
  state_.snapshot.joint_positions_rad = joint_positions;
  state_.snapshot.error = std::move(error);
}

void RoboPlanExecutionBackend::update_scene_positions(
    const std::vector<double>& joint_positions) {
  if (joint_positions.empty()) {
    return;
  }

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q(joint_positions.size());
  for (size_t i = 0; i < joint_positions.size(); ++i) {
    q[static_cast<Eigen::Index>(i)] = joint_positions[i];
  }

  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }

  scene_->setJointPositions(q);
}

std::vector<double> RoboPlanExecutionBackend::interpolate_joint_positions(
    const core::TrajectoryDTO& trajectory, double time_s) const {
  if (trajectory.waypoints.empty()) {
    return {};
  }

  if (trajectory.waypoints.size() == 1 ||
      time_s <= trajectory.waypoints.front().time_from_start_s) {
    return trajectory.waypoints.front().positions_rad;
  }

  if (time_s >= trajectory.waypoints.back().time_from_start_s) {
    return trajectory.waypoints.back().positions_rad;
  }

  for (size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    const auto& prev = trajectory.waypoints[i - 1];
    const auto& next = trajectory.waypoints[i];
    if (time_s > next.time_from_start_s) {
      continue;
    }

    const double dt = next.time_from_start_s - prev.time_from_start_s;
    if (dt <= 1e-9) {
      return next.positions_rad;
    }

    const double alpha = clamp_progress((time_s - prev.time_from_start_s) / dt);
    std::vector<double> interpolated(prev.positions_rad.size(), 0.0);
    for (size_t joint = 0; joint < prev.positions_rad.size(); ++joint) {
      interpolated[joint] =
          prev.positions_rad[joint] +
          alpha * (next.positions_rad[joint] - prev.positions_rad[joint]);
    }
    return interpolated;
  }

  return trajectory.waypoints.back().positions_rad;
}

void RoboPlanExecutionBackend::execution_worker(std::stop_token st, core::TrajectoryDTO trajectory) {
  const double total_duration_s = trajectory.waypoints.empty()
                                      ? 0.0
                                      : trajectory.waypoints.back().time_from_start_s;
  std::vector<double> latest_positions =
      trajectory.waypoints.empty() ? std::vector<double>{}
                                   : trajectory.waypoints.front().positions_rad;

  if (st.stop_requested()) {
    state_.running.store(false);
    execution_cv_.notify_all();
    return;
  }

  update_scene_positions(latest_positions);
  publish_snapshot(core::ExecutionState::kExecuting, 0.0, latest_positions);

  int64_t last_tick_ns = clock_.now();
  double trajectory_time_s = 0.0;
  double applied_runtime_scale = std::max(
      0.0, std::min(1.0, runtime_velocity_scale_.load(std::memory_order_acquire)));
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.active_trajectory = trajectory;
    state_.active_trajectory_time_s = trajectory_time_s;
    state_.active_runtime_velocity_scale = applied_runtime_scale;
  }

  while (true) {
    const bool pause_requested = state_.pause_requested.load();

    if (st.stop_requested()) {
      publish_snapshot(core::ExecutionState::kStopped,
                       get_execution_snapshot().progress, latest_positions);
      break;
    }

    if (pause_requested) {
      publish_snapshot(core::ExecutionState::kPaused,
                       get_execution_snapshot().progress, latest_positions);
      std::unique_lock<std::mutex> lock(execution_mutex_);
      execution_cv_.wait(lock, [this, &st]() {
        return st.stop_requested() || !state_.pause_requested.load();
      });
      last_tick_ns = clock_.now();
      if (st.stop_requested()) {
        continue;
      }
      publish_snapshot(core::ExecutionState::kExecuting,
                       get_execution_snapshot().progress, latest_positions);
      continue;
    }

    const int64_t now_ns = clock_.now();
    const double dt_s =
        std::max(0.0, static_cast<double>(now_ns - last_tick_ns) / 1e9);
    last_tick_ns = now_ns;
    const double target_scale = std::max(
        0.0, std::min(1.0, runtime_velocity_scale_.load(std::memory_order_acquire)));
    const double max_delta = kRuntimeScaleSlewPerSecond * dt_s;
    if (target_scale > applied_runtime_scale + max_delta) {
      applied_runtime_scale += max_delta;
    } else if (target_scale < applied_runtime_scale - max_delta) {
      applied_runtime_scale -= max_delta;
    } else {
      applied_runtime_scale = target_scale;
    }
    trajectory_time_s += dt_s * applied_runtime_scale;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_.active_trajectory_time_s = trajectory_time_s;
      state_.active_runtime_velocity_scale = applied_runtime_scale;
    }

    const double progress =
        total_duration_s <= 1e-9 ? 1.0 : clamp_progress(trajectory_time_s / total_duration_s);
    latest_positions = interpolate_joint_positions(trajectory, trajectory_time_s);
    update_scene_positions(latest_positions);
    publish_snapshot(core::ExecutionState::kExecuting, progress, latest_positions);

    if (progress >= 1.0) {
      publish_snapshot(core::ExecutionState::kCompleted, 1.0,
                       trajectory.waypoints.empty()
                           ? latest_positions
                           : trajectory.waypoints.back().positions_rad);
      if (!trajectory.waypoints.empty()) {
        update_scene_positions(trajectory.waypoints.back().positions_rad);
      }
      break;
    }

    std::unique_lock<std::mutex> lock(execution_mutex_);
    execution_cv_.wait_for(lock, kExecutionTick, [this, &st]() {
      return st.stop_requested() || state_.pause_requested.load();
    });
  }

  state_.running.store(false);
  state_.pause_requested.store(false);
  execution_cv_.notify_all();
}

core::ExecutionHandle RoboPlanExecutionBackend::execute_trajectory(
    const core::TrajectoryDTO& trajectory) {
  // jthread auto-joins, no need to call join_worker_if_needed()

  const std::vector<double> initial_positions =
      trajectory.waypoints.empty() ? std::vector<double>{}
                                   : trajectory.waypoints.front().positions_rad;

  state_.running.store(true);
  state_.pause_requested.store(false);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.active_trajectory = trajectory;
    state_.active_trajectory_time_s = 0.0;
    state_.active_runtime_velocity_scale = std::max(
        0.0, std::min(1.0, runtime_velocity_scale_.load(std::memory_order_acquire)));
  }
  publish_snapshot(core::ExecutionState::kExecuting, 0.0, initial_positions);
  update_scene_positions(initial_positions);

  worker_thread_ = std::jthread(
      [this, trajectory_copy = core::TrajectoryDTO(trajectory)](
          std::stop_token st) mutable {
        execution_worker(st, std::move(trajectory_copy));
      });

  return core::ExecutionHandle{
      [this](int64_t timeout_ns) {
        std::unique_lock<std::mutex> lock(execution_mutex_);
        if (timeout_ns > 0) {
          return execution_cv_.wait_for(
              lock, std::chrono::nanoseconds(timeout_ns),
              [this]() { return !state_.running.load(); });
        }
        execution_cv_.wait(lock, [this]() { return !state_.running.load(); });
        return true;
      },
      [this]() { return get_execution_snapshot(); }};
}

core::Result<void> RoboPlanExecutionBackend::sync_observed_state_before_planning() {
  return core::Result<void>::Ok();
}

core::StopResultDTO RoboPlanExecutionBackend::stop() {
  if (!state_.running.load()) {
    return core::StopResultDTO{
        false, core::ErrorDTO{core::kErrorInvalidRequest, "No execution in flight", {}}};
  }
  state_.pause_requested.store(false);
  worker_thread_.request_stop();
  execution_cv_.notify_all();
  return core::StopResultDTO{true, std::nullopt};
}

// Phase 4.5D-3: controlled-stop capability + result surface.
//
// The native simulator supports `kSpringBackHold` and Phase 4.5G
// `kSoftOnPath`. `kHardDeceleration` is a ros2_control/JTC-only mode in
// the current scope. Declaring only supported modes keeps the kernel's
// arm-time gate honest: scopes requesting unsupported modes fail closed
// before dispatch.

core::MotionInterruptStopCapabilityDTO
RoboPlanExecutionBackend::get_motion_interrupt_stop_capability() const {
  core::MotionInterruptStopCapabilityDTO cap;
  cap.supported_stop_modes = {core::MotionInterruptStopMode::kSpringBackHold,
                              core::MotionInterruptStopMode::kSoftOnPath};
  cap.capability_profile_id = "roboplan_native_v2_soft_on_path";
  core::MotionInterruptSoftOnPathPlannerConfigDTO soft_config;
  soft_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
  soft_config.sample_period_s = config_.soft_on_path_sample_period_s;
  soft_config.max_initial_path_deviation_rad =
      config_.soft_on_path_max_initial_path_deviation_rad;
  soft_config.max_initial_velocity_deviation_rad_s =
      config_.soft_on_path_max_initial_velocity_deviation_rad_s;
  soft_config.per_joint_max_jerk_rad_s3 =
      config_.soft_on_path_max_jerk_rad_s3;
  soft_config.allow_model_velocity_fallback = true;
  cap.soft_on_path_planner_config = std::move(soft_config);
  return cap;
}

core::ControlledStopResultDTO
RoboPlanExecutionBackend::execute_controlled_stop(
    const core::MotionInterruptStopProfileDTO& profile) {
  // Defense in depth: the kernel's arm-time gate already rejects
  // unsupported modes, but a backend that has been mis-wired into
  // running the wrong mode at trip time MUST fail closed rather than
  // silently producing a hold for a path-preserving request.
  core::ControlledStopResultDTO result;
  result.executed_stop_mode = profile.stop_mode;

  if (profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath) {
    core::TrajectoryDTO active_trajectory;
    core::ExecutionSnapshot active_snapshot;
    double active_time_s = 0.0;
    double active_runtime_scale = 1.0;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      active_snapshot = state_.snapshot;
      if (state_.active_trajectory.has_value()) {
        active_trajectory = *state_.active_trajectory;
      }
      active_time_s = state_.active_trajectory_time_s;
      active_runtime_scale = state_.active_runtime_velocity_scale;
    }

    if (active_trajectory.waypoints.empty() ||
        active_snapshot.joint_positions_rad.empty()) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "native soft_on_path requires an active trajectory and observed state",
          {{"reason", "soft_on_path_missing_active_trajectory_or_state"},
           {"backend_capability_profile_id", "roboplan_native_v2_soft_on_path"}}};
      return result;
    }

    core::SoftOnPathInitialStateDTO initial;
    initial.state.joint_names = active_trajectory.joint_names;
    initial.state.positions_rad = active_snapshot.joint_positions_rad;
    initial.trajectory_time_from_start_s = active_time_s;
    initial.position_source = core::SoftOnPathStateSource::kMeasuredFeedback;
    initial.velocity_source = core::SoftOnPathStateSource::kModelDerived;
    initial.runtime_velocity_scale_at_trip = active_runtime_scale;
    initial.sample_age_ns = std::max<std::int64_t>(
        0, clock_.now() - active_snapshot.timestamp_ns);

    core::SoftOnPathPlannerConfig planner_config;
    planner_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
    planner_config.sample_period_s = config_.soft_on_path_sample_period_s;
    planner_config.allow_model_velocity_fallback = true;
    planner_config.max_initial_path_deviation_rad =
        config_.soft_on_path_max_initial_path_deviation_rad;
    planner_config.max_initial_velocity_deviation_rad_s =
        config_.soft_on_path_max_initial_velocity_deviation_rad_s;
    planner_config.per_joint_max_jerk_rad_s3 =
        config_.soft_on_path_max_jerk_rad_s3;

    auto planned = core::plan_soft_on_path_stop(
        active_trajectory, profile, initial, planner_config);
    if (!planned.ok()) {
      if (profile.allow_natural_completion_on_short_path &&
          core::soft_on_path_error_recommends_natural_completion(
              planned.error())) {
        const auto call_start_ns = clock_.now();
        const auto wait_deadline_s = profile.max_stop_time_s.value_or(1.0);
        std::unique_lock<std::mutex> lock(execution_mutex_);
        const bool completed = execution_cv_.wait_for(
            lock,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(wait_deadline_s)),
            [this]() { return !state_.running.load(); });
        lock.unlock();
        const auto call_end_ns = clock_.now();
        result.confirmation_latency_ns =
            std::max<std::int64_t>(0, call_end_ns - call_start_ns);
        result.diagnostics = planned.error().details;
        result.diagnostics["command_source"] =
            "native_simulated_natural_completion";
        result.diagnostics["soft_on_path_phase"] =
            "4.5G_natural_completion";
        result.diagnostics["requested_stop_mode"] = "soft_on_path";
        result.stop_command_source =
            core::MotionInterruptStopCommandSource::kDiagnosticOther;
        if (!completed ||
            get_execution_snapshot().state !=
                core::ExecutionState::kCompleted) {
          result.accepted = true;
          result.backend_stop_confirmed = false;
          result.error = core::ErrorDTO{
              std::string(core::kErrorMotionInterruptBackendStopFailed),
              "native soft_on_path natural completion did not reach the "
              "trajectory endpoint within max_stop_time_s",
              {{"reason", "soft_on_path_natural_completion_timeout"},
               {"deadline_s", std::to_string(wait_deadline_s)}}};
          return result;
        }
        result.accepted = true;
        result.backend_stop_confirmed = true;
        result.completed_by_natural_completion = true;
        return result;
      }
      const bool may_fallback = std::find(
          profile.allowed_fallback_modes.begin(),
          profile.allowed_fallback_modes.end(),
          core::MotionInterruptStopMode::kSpringBackHold) !=
          profile.allowed_fallback_modes.end();
      if (may_fallback) {
        core::MotionInterruptStopProfileDTO fallback_profile = profile;
        fallback_profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
        auto fallback = execute_controlled_stop(fallback_profile);
        fallback.executed_stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
        fallback.fallback_reason = planned.error().details.count("reason") > 0
                                       ? planned.error().details.at("reason")
                                       : "soft_on_path_planner_rejected";
        fallback.diagnostics["requested_stop_mode"] = "soft_on_path";
        fallback.diagnostics["fallback_reason"] = *fallback.fallback_reason;
        fallback.diagnostics["soft_on_path_planner_error"] =
            planned.error().message;
        return fallback;
      }
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = planned.error();
      return result;
    }

    const auto call_start_ns = clock_.now();
    state_.pause_requested.store(false);
    worker_thread_.request_stop();
    execution_cv_.notify_all();
    {
      std::unique_lock<std::mutex> lock(execution_mutex_);
      const bool stopped = execution_cv_.wait_for(
          lock,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>(profile.max_stop_time_s.value_or(1.0))),
          [this]() { return !state_.running.load(); });
      if (!stopped) {
        result.accepted = true;
        result.backend_stop_confirmed = false;
        result.error = core::ErrorDTO{
            std::string(core::kErrorMotionInterruptBackendStopFailed),
            "worker did not yield to native soft_on_path before deadline",
            {{"reason", "soft_on_path_worker_stop_timeout"}}};
        return result;
      }
    }

    const auto& replacement = planned.value().replacement_trajectory;
    state_.running.store(true);
    const auto steady_start = std::chrono::steady_clock::now();
    const double total_time_s =
        replacement.waypoints.empty()
            ? 0.0
            : replacement.waypoints.back().time_from_start_s;
    for (const auto& waypoint : replacement.waypoints) {
      const auto wake_time =
          steady_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(waypoint.time_from_start_s));
      std::this_thread::sleep_until(wake_time);
      const double progress =
          total_time_s <= 1e-9
              ? 1.0
              : clamp_progress(waypoint.time_from_start_s / total_time_s);
      update_scene_positions(waypoint.positions_rad);
      publish_snapshot(core::ExecutionState::kStopping,
                       progress,
                       waypoint.positions_rad);
    }

    const auto call_end_ns = clock_.now();
    const auto& final_wp = replacement.waypoints.back();
    publish_snapshot(core::ExecutionState::kStopped, 1.0, final_wp.positions_rad);
    update_scene_positions(final_wp.positions_rad);
    state_.running.store(false);
    execution_cv_.notify_all();

    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.executed_stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kNativeSimulatedPathRamp;
    result.confirmation_latency_ns =
        std::max<std::int64_t>(0, call_end_ns - call_start_ns);
    result.path_deviation_max_rad = planned.value().path_deviation_max_rad;
    result.initial_path_deviation_rad =
        planned.value().initial_path_deviation_rad;
    result.stop_distance_rad = 0.0;
    if (!active_snapshot.joint_positions_rad.empty() &&
        final_wp.positions_rad.size() == active_snapshot.joint_positions_rad.size()) {
      double sum = 0.0;
      for (size_t i = 0; i < final_wp.positions_rad.size(); ++i) {
        const double d = final_wp.positions_rad[i] -
                         active_snapshot.joint_positions_rad[i];
        sum += d * d;
      }
      result.stop_distance_rad = std::sqrt(sum);
    }
    result.diagnostics = planned.value().diagnostics;
    result.diagnostics["command_source"] = "native_simulated_path_ramp";
    result.diagnostics["runtime_scale_isolated"] = "true";
    result.diagnostics["soft_on_path_phase"] = "4.5G_native";
    return result;
  }

  if (profile.stop_mode != core::MotionInterruptStopMode::kSpringBackHold) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "RoboPlanExecutionBackend supports only kSpringBackHold and kSoftOnPath in v2",
        {{"reason", "stop_mode_not_supported_by_backend"},
         {"backend_capability_profile_id", "roboplan_native_v2_soft_on_path"}}};
    return result;
  }

  // Capture observed joint positions BEFORE requesting the worker to
  // stop so the recorded anchor reflects the kernel's pre-stop trip
  // sample, not whatever the worker happens to publish after the
  // stop_requested flag fires. `latest_positions` inside the worker
  // is updated on every tick; reading the published snapshot here is
  // the closest the native simulator has to "controller terminal
  // result confirmation."
  std::vector<double> observed_anchor;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    observed_anchor = state_.snapshot.joint_positions_rad;
  }

  // If no execution is in flight, there's nothing to preempt. The
  // kernel never calls this without an active trip, but defense in
  // depth handles the case anyway.
  if (!state_.running.load()) {
    if (observed_anchor.empty()) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "execute_controlled_stop invoked with no execution in flight "
          "and no recorded observed state",
          {{"reason", "controlled_stop_no_observed_state"}}};
      return result;
    }
    // Already stopped with a recorded anchor: report immediate
    // confirmation. This collapses to the "spring-back to here" path
    // when the trip latch arrives right as the worker is winding down.
    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kNativeSimulatedStop;
    result.confirmation_latency_ns = 0;
    return result;
  }

  const auto call_start_ns = clock_.now();
  // Request the worker to stop. The worker's stop_token wakes it,
  // it publishes a kStopped snapshot at its current observed
  // position, then clears `state_.running`.
  state_.pause_requested.store(false);
  worker_thread_.request_stop();
  execution_cv_.notify_all();

  // Wait bounded by the profile's max_stop_time_s when set; the
  // ExecutionKernelConfig default fallback is applied by the kernel
  // before this method is called (the kernel enforces deadline
  // overflow on top of whatever value the backend reports). Default
  // backend wait is 1 s which exceeds the 500 ms kernel default so
  // the kernel's enforcement always wins; we still bound here to
  // avoid pinning the kernel's worker forever on a misbehaving
  // simulator stop.
  const auto wait_deadline_s =
      profile.max_stop_time_s.value_or(1.0);
  std::unique_lock<std::mutex> lock(execution_mutex_);
  const bool stopped = execution_cv_.wait_for(
      lock,
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::duration<double>(wait_deadline_s)),
      [this]() { return !state_.running.load(); });
  lock.unlock();

  const auto call_end_ns = clock_.now();
  result.confirmation_latency_ns = std::max<std::int64_t>(0, call_end_ns -
                                                                 call_start_ns);

  if (!stopped) {
    result.accepted = true;  // command was accepted (worker was notified)
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "worker did not exit within the controlled-stop deadline",
        {{"reason", "backend_stop_not_confirmed"},
         {"deadline_s", std::to_string(wait_deadline_s)}}};
    return result;
  }

  // Worker stopped. The post-stop snapshot already carries the
  // observed joint positions at the moment the worker exited the
  // execution loop (publish_snapshot was called with state=kStopped
  // and the latest interpolated positions). The kernel's snapshot
  // capture pulls that via get_execution_snapshot().
  result.accepted = true;
  result.backend_stop_confirmed = true;
  result.stop_command_source =
      core::MotionInterruptStopCommandSource::kNativeSimulatedStop;
  return result;
}

core::PauseResultDTO RoboPlanExecutionBackend::pause() {
  if (!state_.running.load()) {
    return core::PauseResultDTO{
        false, core::ErrorDTO{core::kErrorInvalidRequest, "No execution in flight", {}}};
  }
  state_.pause_requested.store(true);
  execution_cv_.notify_all();
  return core::PauseResultDTO{true, std::nullopt};
}

core::ExecutionHandle RoboPlanExecutionBackend::resume(
    const core::TrajectoryDTO& trajectory) {
  if (state_.running.load() && state_.pause_requested.load()) {
    state_.pause_requested.store(false);
    execution_cv_.notify_all();
    return core::ExecutionHandle{
        [this](int64_t timeout_ns) {
          std::unique_lock<std::mutex> lock(execution_mutex_);
          if (timeout_ns > 0) {
            return execution_cv_.wait_for(
                lock, std::chrono::nanoseconds(timeout_ns),
                [this]() { return !state_.running.load(); });
          }
          execution_cv_.wait(lock, [this]() { return !state_.running.load(); });
          return true;
        },
        [this]() { return get_execution_snapshot(); }};
  }
  return execute_trajectory(trajectory);
}

core::ResetResultDTO RoboPlanExecutionBackend::reset() {
  const auto reset_timeout =
      std::chrono::duration<double>(config_.reset_completion_timeout_s);
  if (streaming_thread_.joinable()) {
    streaming_thread_.request_stop();
    streaming_cv_.notify_all();
    {
      std::unique_lock<std::mutex> lock(streaming_mutex_);
      const bool stopped = streaming_cv_.wait_for(
          lock, reset_timeout,
          [this]() { return !streaming_active_.load(); });
      if (!stopped) {
        publish_snapshot(
            core::ExecutionState::kInterventionRequired,
            get_execution_snapshot().progress,
            get_execution_snapshot().joint_positions_rad,
            core::ErrorDTO{
                core::kErrorResetFailed,
                "Native execution reset timed out waiting for streaming "
                "worker completion",
                {{"requires_operator_intervention", "true"},
                 {"reason_code", "reset_timeout"},
                 {"phase", "streaming_worker_reset"},
                 {"timeout_s", seconds_string(reset_timeout)}}});
        return core::ResetResultDTO{
            false,
            core::ErrorDTO{
                core::kErrorResetFailed,
                "Native execution reset timed out waiting for streaming "
                "worker completion",
                {{"requires_operator_intervention", "true"},
                 {"reason_code", "reset_timeout"},
                 {"phase", "streaming_worker_reset"},
                 {"timeout_s", seconds_string(reset_timeout)}}}};
      }
    }
    streaming_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_target_.reset();
    streaming_commanded_joints_.reset();
    streaming_commanded_velocities_.reset();
    streaming_hold_emitted_ = false;
  }

  state_.pause_requested.store(false);
  if (worker_thread_.joinable()) {
    worker_thread_.request_stop();
  }
  execution_cv_.notify_all();
  {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    const bool stopped = execution_cv_.wait_for(
        lock, reset_timeout,
        [this]() { return !state_.running.load(); });
    if (!stopped) {
      publish_snapshot(
          core::ExecutionState::kInterventionRequired,
          get_execution_snapshot().progress,
          get_execution_snapshot().joint_positions_rad,
          core::ErrorDTO{
              core::kErrorResetFailed,
              "Native execution reset timed out waiting for worker completion",
              {{"requires_operator_intervention", "true"},
               {"reason_code", "reset_timeout"},
               {"phase", "execution_worker_reset"},
               {"timeout_s", seconds_string(reset_timeout)}}});
      return core::ResetResultDTO{
          false,
          core::ErrorDTO{
              core::kErrorResetFailed,
              "Native execution reset timed out waiting for worker completion",
              {{"requires_operator_intervention", "true"},
               {"reason_code", "reset_timeout"},
               {"phase", "execution_worker_reset"},
               {"timeout_s", seconds_string(reset_timeout)}}}};
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.active_trajectory.reset();
    state_.active_trajectory_time_s = 0.0;
    state_.active_runtime_velocity_scale = 1.0;
  }
  publish_snapshot(core::ExecutionState::kIdle, 0.0, {});
  return core::ResetResultDTO{true, std::nullopt};
}

core::ExecutionSnapshot RoboPlanExecutionBackend::get_execution_snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_.snapshot;
}

// =================================================================
// Streaming-session mode (ADR-0016).
//
// v0.1 simulated semantics:
//   - Only joint targets are accepted; cartesian targets are rejected
//     with unsupported_capability (the ros2_control adapter handles
//     cartesian IK via roboplan internally; the simulated path doesn't
//     expose IK to the backend, by design).
//   - The streaming worker reads the latest target from a mutex-
//     protected register at a tick driven by max_dispatch_rate_hz, and
//     snaps the simulated scene to the target. The simulated backend
//     does not model controller dynamics; "smoothness" is a property of
//     the real ros2_control backend, not this one. Tests that need
//     trajectory shape go through that backend or through a mock
//     controller.
//   - Soft-stop (spring_back or velocity_scaling) is reduced to "stop
//     the worker; latch the current scene position as the terminal
//     state". The simulated backend doesn't distinguish strategies; the
//     selection is preserved in the emitted events for audit only.
// =================================================================

void RoboPlanExecutionBackend::streaming_worker(std::stop_token st) {
  const double tick_s = [&]() {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    const double rate = streaming_config_.max_dispatch_rate_hz;
    return rate > 0.0 ? (1.0 / rate) : 0.033;
  }();
  const auto tick = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::duration<double>(tick_s));

  // Simulated motion model: at each tick, advance toward the latest
  // target by the smaller of two limits:
  //   1. joint-velocity cap (per-joint step cap = v_cap * tick_s);
  //   2. cartesian cap proxy = tick_s / min_time_from_start_s, which
  //      throttles the fraction-toward-goal so the simulated TCP can't
  //      exceed `global_max_tcp_linear_speed_mps` (ADR-0015).
  // This makes the simulated lane visually respect the deployment's
  // speed cap. Real ros2_control already enforces it via the FJT goal's
  // `time_from_start` in the dispatch worker.

  while (!st.stop_requested()) {
    std::optional<core::StreamingTargetDTO> target;
    core::StreamingSessionConfigDTO config;
    {
      std::lock_guard<std::mutex> lock(streaming_mutex_);
      target = streaming_target_;
      config = streaming_config_;
    }

    if (target.has_value() &&
        target->kind == core::StreamingInputMode::kJoint &&
        !target->joint_positions_rad.empty()) {
      const auto& goal = target->joint_positions_rad;

      // Initialize / re-seed the commanded joint vector on first tick
      // (or if size changed across sessions). Seed from the current
      // observed snapshot, NOT from `goal`: the operator's first
      // slider/gizmo push is typically the target pose, not the
      // start pose; seeding from `goal` would teleport the sim arm
      // there in one tick. Seeding from the snapshot lets the
      // velocity-cap interpolation below smoothly bring `current`
      // toward `goal` over many ticks. If the snapshot's joint
      // vector doesn't match the session size (e.g. fresh boot,
      // snapshot not yet populated), fall back to `goal` to avoid
      // stalling — visually still a small bump on that first
      // session, but doesn't block the operator.
      // Seed the commanded-joint vector from the observed snapshot
      // (NOT the goal — seeding from goal teleports the sim arm in
      // one tick). If the snapshot doesn't yet cover every session
      // joint, hold position this tick and retry on the next one:
      // operator briefly sees no motion, never a teleport. This is
      // the industrial-predictability path the spec hints at — the
      // alternative (treat goal as the start) violates the velocity
      // and acceleration caps the operator just configured.
      bool can_pursue = true;
      if (!streaming_commanded_joints_.has_value() ||
          streaming_commanded_joints_->size() != goal.size()) {
        const auto snap = get_execution_snapshot();
        if (snap.joint_positions_rad.size() == goal.size()) {
          streaming_commanded_joints_ = snap.joint_positions_rad;
          streaming_commanded_velocities_ =
              std::vector<double>(goal.size(), 0.0);
        } else {
          // Hold scene at last published; the else-branch below
          // (effectively this iteration's no-op) re-publishes the
          // existing snapshot. No motion, no teleport.
          can_pursue = false;
        }
      }
      if (can_pursue
          && (!streaming_commanded_velocities_.has_value()
              || streaming_commanded_velocities_->size() != goal.size())) {
        streaming_commanded_velocities_ =
            std::vector<double>(goal.size(), 0.0);
      }
      if (!can_pursue) {
        // Re-publish with a structured error so observability
        // surfaces show "worker holding" instead of the silent
        // deadlock we hit before — `/api/scene/state` reports
        // the error every tick the hold persists, and the
        // failure archive sees it on the kernel event stream
        // once kernel-event bridging is wired (today the
        // snapshot.error is the only surface).
        const auto snapshot = get_execution_snapshot();
        core::ErrorDTO hold_err{
            "streaming_seed_size_mismatch",
            "streaming worker holding: scene snapshot joint vector "
            "does not cover every session joint",
            {
                {"snapshot_size",
                 std::to_string(snapshot.joint_positions_rad.size())},
                {"goal_size", std::to_string(goal.size())},
                {"session_id", std::to_string(streaming_session_id_)},
            },
        };
        if (!streaming_hold_emitted_) {
          streaming_hold_emitted_ = true;
          std::cerr
              << "[manyforge_roboplan] streaming worker hold: snapshot "
              << snapshot.joint_positions_rad.size() << " joints vs "
              << "session goal " << goal.size()
              << " — slider/gizmo will appear stuck. Most common cause: "
                 "deployment joint_names is a subset of the URDF model "
                 "(e.g. arm-only against arm+gripper). Fix the session's "
                 "joint_names or rerun begin_streaming_session.\n";
        }
        publish_snapshot(core::ExecutionState::kStreaming,
                         snapshot.progress,
                         snapshot.joint_positions_rad,
                         hold_err);
        std::unique_lock<std::mutex> lock(streaming_mutex_);
        streaming_cv_.wait_for(lock, tick);
        continue;
      }
      std::vector<double>& current = *streaming_commanded_joints_;
      std::vector<double>& prev_v = *streaming_commanded_velocities_;

      // Engine defaults when the session config didn't supply
      // per-joint values (or sizes don't match the goal). 0.5 rad/s
      // is the operator-perceivable-motion baseline; 2.0 rad/s² lets
      // the arm reach v_cap in ~0.25 s — smooth start without
      // feeling sluggish.
      constexpr double kDefaultVelRadS = 0.5;
      constexpr double kDefaultAccelRadS2 = 2.0;
      const bool have_per_joint_v =
          config.per_joint_velocity_caps_rad_s.size() == goal.size();
      const bool have_per_joint_a =
          config.per_joint_acceleration_caps_rad_s2.size() == goal.size();
      // ADR-0018: read the runtime velocity scale fresh on every tick.
      const double v_scale = std::max(
          runtime_velocity_scale_.load(std::memory_order_acquire),
          0.0);

      // Cartesian-cap-derived fraction. The kernel pre-computed
      // min_time_from_start_s from |Δpos| / max_tcp_linear_speed_mps.
      // Advancing by `tick_s / min_time_from_start_s` per tick yields
      // a Cartesian rate that matches the cap exactly.
      double cart_fraction = 1.0;
      if (target->min_time_from_start_s > tick_s) {
        cart_fraction = tick_s / target->min_time_from_start_s;
      }

      for (size_t i = 0; i < goal.size(); ++i) {
        // Per-joint caps. Zero/missing values fall back to engine
        // defaults so a partially-configured deployment still works.
        double v_cap_i = kDefaultVelRadS;
        if (have_per_joint_v
            && config.per_joint_velocity_caps_rad_s[i] > 0.0) {
          v_cap_i = config.per_joint_velocity_caps_rad_s[i];
        }
        v_cap_i *= v_scale;
        double a_cap_i = kDefaultAccelRadS2;
        if (have_per_joint_a
            && config.per_joint_acceleration_caps_rad_s2[i] > 0.0) {
          a_cap_i = config.per_joint_acceleration_caps_rad_s2[i];
        }

        // Desired velocity to reach goal in one tick, after the
        // cartesian-cap-derived slowdown. Clamp to per-joint v_cap.
        const double pos_delta_desired = (goal[i] - current[i]) * cart_fraction;
        double desired_v = pos_delta_desired / tick_s;
        if (desired_v > v_cap_i) desired_v = v_cap_i;
        if (desired_v < -v_cap_i) desired_v = -v_cap_i;

        // Acceleration limiter: velocity change per tick is bounded by
        // a_cap_i * tick_s. Without this, slider drags produce a
        // step-velocity profile (instant jump to v_cap), which still
        // looks jerky at human time scales. With this, velocity ramps
        // up smoothly.
        const double dv_max = a_cap_i * tick_s;
        double v_new = desired_v;
        if (v_new > prev_v[i] + dv_max) v_new = prev_v[i] + dv_max;
        if (v_new < prev_v[i] - dv_max) v_new = prev_v[i] - dv_max;

        // Overshoot clamp: snap to `goal` only when the step would
        // *cross* it. Detect the crossing by comparing the sign of
        // `goal - current` before and after the step — direction of
        // velocity is incidental.
        //
        // The earlier formulation `v_new > 0 && next_pos >= goal`
        // misfired when the target jumped to the opposite side of
        // `current` mid-motion: the accel limiter can leave velocity
        // pointing toward the *old* goal for a tick or two, so
        // `v_new > 0` is still true while `next_pos >= goal` is now
        // trivially true (current is already past the new goal in
        // velocity direction). That collapses to an instant teleport
        // every time the operator clicks the slider on the opposite
        // side of where the robot is travelling. The crossing test
        // here only fires when there's a real overshoot to clamp;
        // a direction reversal naturally decelerates through the
        // accel-limited path on subsequent ticks.
        const double next_pos = current[i] + v_new * tick_s;
        const bool settled =
            std::abs(goal[i] - current[i]) < 1e-9 &&
            std::abs(prev_v[i]) < 1e-6;
        const bool crosses_from_below =
            current[i] < goal[i] && next_pos >= goal[i];
        const bool crosses_from_above =
            current[i] > goal[i] && next_pos <= goal[i];
        if (settled || crosses_from_below || crosses_from_above) {
          current[i] = goal[i];
          prev_v[i] = 0.0;
        } else {
          current[i] = next_pos;
          prev_v[i] = v_new;
        }
      }

      update_scene_positions(current);
      publish_snapshot(core::ExecutionState::kStreaming, 0.0, current);
    } else {
      const auto snapshot = get_execution_snapshot();
      publish_snapshot(core::ExecutionState::kStreaming,
                       snapshot.progress, snapshot.joint_positions_rad);
    }

    std::unique_lock<std::mutex> lock(streaming_mutex_);
    streaming_cv_.wait_for(lock, tick, [&st]() {
      return st.stop_requested();
    });
  }

  const auto final_snapshot = get_execution_snapshot();
  publish_snapshot(core::ExecutionState::kStopped, final_snapshot.progress,
                   final_snapshot.joint_positions_rad);
  streaming_active_.store(false);
  streaming_cv_.notify_all();
}

core::StreamingSessionBeginResultDTO
RoboPlanExecutionBackend::begin_streaming_session(
    const core::StreamingSessionConfigDTO& config) {
  core::StreamingSessionBeginResultDTO result;

  uint64_t session_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    if (streaming_active_.load() || state_.running.load()) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidStateTransition)),
          "backend is already executing or streaming",
          {}};
      return result;
    }
    session_id = ++streaming_session_id_;
    streaming_config_ = config;
    streaming_target_.reset();
    streaming_commanded_joints_.reset();
    streaming_commanded_velocities_.reset();
    streaming_hold_emitted_ = false;
    next_streaming_backend_seq_ = 1;
    // Seed the runtime atomic from the session config so the first tick reads
    // the same value as the begin payload. Subsequent operator slider changes
    // update this atomic through set_runtime_velocity_scale.
    {
      double initial = config.velocity_scale;
      if (!std::isfinite(initial) || initial < 0.0) initial = 0.0;
      if (initial > 1.0) initial = 1.0;
      runtime_velocity_scale_.store(
          initial, std::memory_order_release);
    }
    streaming_active_.store(true);
  }

  // Start worker. jthread auto-joins on destruction; this also tears down
  // any previous lingering thread cleanly.
  streaming_thread_ = std::jthread(
      [this](std::stop_token st) { this->streaming_worker(st); });

  // Publish the streaming snapshot seeded with the CURRENT scene
  // joint positions in SESSION-JOINT-ORDER (not the full URDF
  // joint vector). Without the subsetting, a robot with N model
  // joints (e.g. 6 arm + 2 gripper = 8 for UR5+robotiq) versus a
  // session sized to just the arm group (6) made the worker's
  // size-check fail every first push, and after the M2 hold-on-
  // incomplete-snapshot guard, that became a silent deadlock —
  // slider stops following with no error or label, since both
  // sim and kernel see the session as "still active".
  std::vector<double> initial_positions;
  if (scene_) {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const auto current = scene_->getCurrentJointPositions();
    if (!config.joint_names.empty()) {
      // Extract by name to match the session size + order.
      const Eigen::VectorXi indices =
          scene_->getJointPositionIndices(config.joint_names);
      initial_positions.reserve(config.joint_names.size());
      for (Eigen::Index i = 0; i < indices.size(); ++i) {
        const Eigen::Index idx = indices[i];
        if (idx < 0 || idx >= current.size()) {
          // Unknown joint name — leave initial_positions empty;
          // the worker's hold-path will publish stale snapshot
          // until the misconfiguration clears. (The kernel's
          // begin_streaming_session fallback should have caught
          // this earlier via the URDF-binding validation.)
          initial_positions.clear();
          break;
        }
        initial_positions.push_back(current[idx]);
      }
    } else {
      // Legacy/uncommon path: caller didn't supply joint_names.
      // Publish the full vector — the worker still does its
      // size check against the goal vector, which will match
      // whatever the kernel sends.
      initial_positions.reserve(static_cast<size_t>(current.size()));
      for (Eigen::Index i = 0; i < current.size(); ++i) {
        initial_positions.push_back(current[i]);
      }
    }
  }
  publish_snapshot(core::ExecutionState::kStreaming, 0.0, initial_positions);

  result.accepted = true;
  result.session_id = session_id;
  return result;
}

core::StreamingPushResultDTO
RoboPlanExecutionBackend::push_streaming_target(
    uint64_t session_id,
    const core::StreamingTargetDTO& target) {
  core::StreamingPushResultDTO result;

  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    if (!streaming_active_.load() || streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {}};
      return result;
    }

    if (target.kind == core::StreamingInputMode::kCartesian) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(
              core::to_string(core::ErrorCode::kUnsupportedCapability)),
          "simulated execution backend does not support cartesian "
          "streaming targets; pre-resolve to joint targets",
          {}};
      return result;
    }

    if (target.joint_positions_rad.empty()) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
          "joint streaming target carries no joint_positions_rad",
          {}};
      return result;
    }

    streaming_target_ = target;
    result.backend_sequence = next_streaming_backend_seq_++;
  }

  // Nudge the worker — it will read the new target at the next tick or
  // sooner if it was waiting.
  streaming_cv_.notify_all();

  result.accepted = true;
  return result;
}

core::StreamingSessionEndResultDTO
RoboPlanExecutionBackend::end_streaming_session(
    uint64_t session_id,
    core::SoftStopStrategy /*strategy*/) {
  core::StreamingSessionEndResultDTO result;

  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    if (!streaming_active_.load() || streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {}};
      return result;
    }
  }

  // Request worker exit; jthread.request_stop() signals the stop_token
  // we passed into the lambda. Wake the cv so the worker exits its wait.
  streaming_thread_.request_stop();
  streaming_cv_.notify_all();

  // Bounded wait for the worker to settle. This is independently tunable
  // from full reset because an operator release should fail quickly with a
  // clear soft-stop timeout while reset can allow a longer drain.
  const auto stop_timeout =
      std::chrono::duration<double>(
          config_.streaming_stop_completion_timeout_s);
  const auto deadline = std::chrono::steady_clock::now() + stop_timeout;
  while (streaming_active_.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // The worker latches the final snapshot to kStopped on exit. If it
  // didn't terminate, surface a soft_stop_timeout-equivalent error and
  // let the kernel route to InterventionRequired.
  if (streaming_active_.load()) {
    result.accepted = false;
    result.error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kStopFailed)),
        "streaming worker did not exit within bounded soft-stop window",
        {{"reason_code", "soft_stop_timeout"},
         {"timeout_s", seconds_string(stop_timeout)}}};
    return result;
  }

  result.accepted = true;
  return result;
}

void RoboPlanExecutionBackend::set_runtime_velocity_scale(double scale) {
  // ADR-0018: clamp + atomic store. Caller (kernel) already validated
  // finiteness, but defense-in-depth keeps the workers from ever reading
  // NaN/Inf via this path.
  if (!std::isfinite(scale)) return;
  if (scale < 0.0) scale = 0.0;
  if (scale > 1.0) scale = 1.0;
  runtime_velocity_scale_.store(scale, std::memory_order_release);
  // Wake both workers so a scale=0 hold or resume takes effect promptly.
  execution_cv_.notify_all();
  streaming_cv_.notify_all();
}

}  // namespace manyforge::roboplan_backend
