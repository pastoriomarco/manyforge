#include <manyforge_roboplan/roboplan_execution_backend.hpp>

#include <Eigen/Dense>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>

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

double controlled_stop_wait_timeout_s(
    const core::MotionInterruptStopProfileDTO& profile,
    double stop_completion_tolerance_s) {
  if (profile.max_stop_time_s.has_value()) {
    return *profile.max_stop_time_s;
  }
  return core::motion_interrupt_deceleration_time_or_default(profile) +
         std::max(0.0, stop_completion_tolerance_s);
}

double clamp_progress(double progress) {
  return std::max(0.0, std::min(1.0, progress));
}

void atomic_max(std::atomic<std::uint64_t>& target, std::uint64_t value) {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current,
                                       value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
  }
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

std::int64_t RoboPlanExecutionBackend::publish_snapshot(
    core::ExecutionState state, double progress,
    const std::vector<double>& joint_positions,
    std::optional<core::ErrorDTO> error) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto timestamp_ns = clock_.now();
  state_.snapshot.timestamp_ns = timestamp_ns;
  state_.snapshot.sequence_number++;
  state_.snapshot.state = state;
  state_.snapshot.progress = clamp_progress(progress);
  state_.snapshot.joint_positions_rad = joint_positions;
  state_.snapshot.error = std::move(error);
  return timestamp_ns;
}

void RoboPlanExecutionBackend::update_scene_positions(
    const std::vector<double>& joint_positions,
    SceneUpdateMode mode,
    std::int64_t source_timestamp_ns) {
  if (joint_positions.empty()) {
    return;
  }

  std::unique_lock<std::mutex> scene_lock(*scene_mutex_, std::defer_lock);
  if (mode == SceneUpdateMode::kBlocking) {
    scene_lock.lock();
  } else if (!scene_lock.try_lock()) {
    record_scene_projection_miss();
    return;
  }

  Eigen::VectorXd q(joint_positions.size());
  for (size_t i = 0; i < joint_positions.size(); ++i) {
    q[static_cast<Eigen::Index>(i)] = joint_positions[i];
  }

  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }

  scene_->setJointPositions(q);
  record_scene_projection_success(source_timestamp_ns);
}

void RoboPlanExecutionBackend::record_scene_projection_success(
    std::int64_t source_timestamp_ns) {
  const auto now_ns = clock_.now();
  scene_projection_successes_total_.fetch_add(1, std::memory_order_relaxed);
  scene_projection_last_success_timestamp_ns_.store(
      now_ns, std::memory_order_relaxed);
  if (source_timestamp_ns > 0 && now_ns >= source_timestamp_ns) {
    atomic_max(scene_projection_lag_max_ns_,
               static_cast<std::uint64_t>(now_ns - source_timestamp_ns));
  }
}

void RoboPlanExecutionBackend::record_scene_projection_miss() {
  scene_projection_misses_total_.fetch_add(1, std::memory_order_relaxed);
}

RoboPlanSceneProjectionMetrics
RoboPlanExecutionBackend::scene_projection_metrics() const {
  RoboPlanSceneProjectionMetrics metrics;
  metrics.misses_total =
      scene_projection_misses_total_.load(std::memory_order_relaxed);
  metrics.successful_updates_total =
      scene_projection_successes_total_.load(std::memory_order_relaxed);
  metrics.lag_max_ns =
      scene_projection_lag_max_ns_.load(std::memory_order_relaxed);
  metrics.last_success_timestamp_ns =
      scene_projection_last_success_timestamp_ns_.load(std::memory_order_relaxed);
  const auto now_ns = clock_.now();
  if (metrics.last_success_timestamp_ns > 0 &&
      now_ns >= metrics.last_success_timestamp_ns) {
    metrics.last_success_age_ns =
        static_cast<std::uint64_t>(now_ns - metrics.last_success_timestamp_ns);
  }
  return metrics;
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
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    execution_cv_.notify_all();
    return;
  }

  const auto initial_snapshot_ns =
      publish_snapshot(core::ExecutionState::kExecuting, 0.0, latest_positions);
  update_scene_positions(latest_positions, SceneUpdateMode::kBestEffort,
                         initial_snapshot_ns);

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
    {
      auto sample =
          core::sample_trajectory(trajectory, trajectory_time_s);
      if (sample.ok()) {
        latest_positions = std::move(sample.value().positions_rad);
      } else {
        // Fail closed: an active execution trajectory that the sampler
        // rejects is structurally malformed. Continuing with an empty
        // position vector would publish a meaningless kExecuting
        // snapshot. Publish a structured Failed terminal and stop the
        // worker so the kernel surfaces the fault instead of silently
        // freezing the visual at an empty state.
        publish_snapshot(
            core::ExecutionState::kFailed, progress,
            latest_positions,
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInternalError)),
                "trajectory sampler rejected the active execution "
                "trajectory",
                {{"reason_code", "trajectory_sampler_error"},
                 {"sampler_error", sample.error().code},
                 {"time_s", std::to_string(trajectory_time_s)}}});
        break;
      }
    }
    const auto snapshot_ns = publish_snapshot(
        state_.natural_completion_stop_requested.load(std::memory_order_acquire)
            ? core::ExecutionState::kStopping
            : core::ExecutionState::kExecuting,
        progress, latest_positions);
    update_scene_positions(latest_positions, SceneUpdateMode::kBestEffort,
                           snapshot_ns);

    if (progress >= 1.0) {
      const auto completed_positions =
          trajectory.waypoints.empty()
              ? latest_positions
              : trajectory.waypoints.back().positions_rad;
      const auto completed_snapshot_ns =
          publish_snapshot(core::ExecutionState::kCompleted, 1.0,
                           completed_positions);
      if (!trajectory.waypoints.empty()) {
        update_scene_positions(completed_positions,
                               SceneUpdateMode::kBlocking,
                               completed_snapshot_ns);
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
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
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
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.active_trajectory = trajectory;
    state_.active_trajectory_time_s = 0.0;
    state_.active_runtime_velocity_scale = std::max(
        0.0, std::min(1.0, runtime_velocity_scale_.load(std::memory_order_acquire)));
  }
  const auto initial_snapshot_ns =
      publish_snapshot(core::ExecutionState::kExecuting, 0.0, initial_positions);
  update_scene_positions(initial_positions, SceneUpdateMode::kBestEffort,
                         initial_snapshot_ns);

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
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  worker_thread_.request_stop();
  execution_cv_.notify_all();
  return core::StopResultDTO{true, std::nullopt};
}

// Controlled-stop capability + result surface.
//
// The native simulator supports `kSpringBackHold` and `kSoftOnPath`.
// `kHardDeceleration` is a ros2_control/JTC-only mode in the current
// scope. Declaring only supported modes keeps the kernel's arm-time
// gate honest: scopes requesting unsupported modes fail closed before
// dispatch.

core::MotionInterruptStopCapabilityDTO
RoboPlanExecutionBackend::get_motion_interrupt_stop_capability() const {
  core::MotionInterruptStopCapabilityDTO cap;
  cap.supported_stop_modes = {core::MotionInterruptStopMode::kSpringBackHold,
                              core::MotionInterruptStopMode::kSoftOnPath};
  cap.capability_profile_id = "roboplan_native_v2_soft_on_path";
  cap.hard_stop_level = "native_immediate_hold";
  cap.derivative_proof_level = "manyforge_core_native_path_ramp";
  cap.scaling_source_visibility = "native_runtime_scale";
  cap.scaling_source_probe_status = "not_applicable";
  cap.unsupported_reasons["hard_deceleration"] =
      "ros2_control_jtc_only_in_current_native_backend";
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
  return execute_controlled_stop(profile, std::stop_token{});
}

core::ControlledStopResultDTO
RoboPlanExecutionBackend::execute_controlled_stop(
    const core::MotionInterruptStopProfileDTO& profile,
    std::stop_token stop_token) {
  begin_controlled_stop_runtime_scale_freeze();
  const auto runtime_scale_freeze_scope =
      std::shared_ptr<void>(nullptr, [this](void*) {
        end_controlled_stop_runtime_scale_freeze();
      });

  // Defense in depth: the kernel's arm-time gate already rejects
  // unsupported modes, but a backend that has been mis-wired into
  // running the wrong mode at trip time MUST fail closed rather than
  // silently producing a hold for a path-preserving request.
  core::ControlledStopResultDTO result;
  result.executed_stop_mode = profile.stop_mode;

  const auto cancelled_result = [&](std::string phase) {
    state_.running.store(false);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    const auto latest = get_execution_snapshot();
    publish_snapshot(core::ExecutionState::kStopped,
                     latest.progress,
                     latest.joint_positions_rad);
    execution_cv_.notify_all();
    result.accepted = true;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "native controlled stop was preempted by a later intervention",
        {{"reason", "controlled_stop_cancelled"},
         {"phase", std::move(phase)}}};
    return result;
  };
  std::stop_callback stop_notify(stop_token, [this]() {
    execution_cv_.notify_all();
  });

  if (profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath) {
    if (stop_token.stop_requested()) {
      return cancelled_result("soft_on_path_entry");
    }
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
        state_.natural_completion_stop_requested.store(true,
                                                       std::memory_order_release);
        execution_cv_.notify_all();
        const auto natural_completion_scope =
            std::shared_ptr<void>(nullptr, [this](void*) {
              state_.natural_completion_stop_requested.store(
                  false, std::memory_order_release);
              execution_cv_.notify_all();
            });
        (void)natural_completion_scope;
        const auto call_start_ns = clock_.now();
        const auto wait_deadline_s = controlled_stop_wait_timeout_s(
            profile, config_.soft_on_path_sample_period_s);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<
                                  std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(
                                      wait_deadline_s));
        bool completed = false;
        {
          std::unique_lock<std::mutex> lock(execution_mutex_);
          while (!stop_token.stop_requested()) {
            completed = !state_.running.load();
            if (completed) {
              break;
            }
            if (execution_cv_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
              completed = !state_.running.load();
              break;
            }
          }
        }
        if (stop_token.stop_requested()) {
          return cancelled_result("soft_on_path_natural_completion_wait");
        }
        const auto call_end_ns = clock_.now();
        result.confirmation_latency_ns =
            std::max<std::int64_t>(0, call_end_ns - call_start_ns);
        result.diagnostics = planned.error().details;
        result.diagnostics["command_source"] =
            "native_simulated_natural_completion";
        result.diagnostics["soft_on_path_phase"] =
            "4.5G_natural_completion";
        result.diagnostics["requested_stop_mode"] = "soft_on_path";
        result.diagnostics["wait_s"] = std::to_string(wait_deadline_s);
        if (!profile.max_stop_time_s.has_value()) {
          result.diagnostics["wait_s_default_source"] =
              "deceleration_time_s_plus_soft_on_path_sample_period_s";
        }
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
              "trajectory endpoint within the effective stop deadline",
              {{"reason", "soft_on_path_natural_completion_timeout"},
               {"deadline_s", std::to_string(wait_deadline_s)}}};
          return result;
        }
        result.accepted = true;
        result.backend_stop_confirmed = true;
        result.completed_by_natural_completion = true;
        return result;
      }
      if (core::soft_on_path_error_recommends_hard_stop(planned.error())) {
        // The planner determined no feasible on-path soft stop exists and
        // recommends a hard stop. The native backend has no hard_deceleration
        // mode (ros2_control/JTC-only); its hard-stop floor is the kernel's
        // emergency backend.stop() immediate hold. So REFUSE the soft stop here
        // (do NOT downgrade to a spring-back hold, which is slower than the hard
        // stop the situation demands) and let the kernel escalate to the
        // immediate hold. The backend-agnostic enriched kernel escalation event
        // carries the planner cause (code, reason, recommended_disposition) for
        // observability. Mirrors the ros2_control hard-stop disposition, adapted
        // to the native lane's stop-mode floor.
        result.accepted = false;
        result.backend_stop_confirmed = false;
        result.error = planned.error();
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
        auto fallback = execute_controlled_stop(fallback_profile, stop_token);
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
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    worker_thread_.request_stop();
    execution_cv_.notify_all();
    {
      std::unique_lock<std::mutex> lock(execution_mutex_);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::duration_cast<
                                std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(
                                    controlled_stop_wait_timeout_s(
                                        profile,
                                        config_.soft_on_path_sample_period_s)));
      bool stopped = false;
      while (!stop_token.stop_requested()) {
        stopped = !state_.running.load();
        if (stopped) {
          break;
        }
        if (execution_cv_.wait_until(lock, deadline) ==
            std::cv_status::timeout) {
          stopped = !state_.running.load();
          break;
        }
      }
      if (stop_token.stop_requested()) {
        return cancelled_result("soft_on_path_worker_stop_wait");
      }
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
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    const auto steady_start = std::chrono::steady_clock::now();
    const double total_time_s =
        replacement.waypoints.empty()
            ? 0.0
            : replacement.waypoints.back().time_from_start_s;
    for (const auto& waypoint : replacement.waypoints) {
      const auto wake_time =
          steady_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(waypoint.time_from_start_s));
      while (!stop_token.stop_requested() &&
             std::chrono::steady_clock::now() < wake_time) {
        const auto remaining = wake_time - std::chrono::steady_clock::now();
        const auto max_slice =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                kExecutionTick);
        std::this_thread::sleep_for(std::min(remaining, max_slice));
      }
      if (stop_token.stop_requested()) {
        return cancelled_result("soft_on_path_replay");
      }
      const double progress =
          total_time_s <= 1e-9
              ? 1.0
              : clamp_progress(waypoint.time_from_start_s / total_time_s);
      const auto snapshot_ns =
          publish_snapshot(core::ExecutionState::kStopping,
                           progress,
                           waypoint.positions_rad);
      update_scene_positions(waypoint.positions_rad,
                             SceneUpdateMode::kBestEffort,
                             snapshot_ns);
    }

    const auto call_end_ns = clock_.now();
    const auto& final_wp = replacement.waypoints.back();
    const auto stopped_snapshot_ns =
        publish_snapshot(core::ExecutionState::kStopped, 1.0, final_wp.positions_rad);
    update_scene_positions(final_wp.positions_rad, SceneUpdateMode::kBlocking,
                           stopped_snapshot_ns);
    state_.running.store(false);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
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
    result.diagnostics["runtime_scale_isolation"] =
        "owned_scale_frozen_during_stop";
    result.diagnostics["timeline_authority_tier"] = "owned_scale_asserted";
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
    result.diagnostics["runtime_scale_isolation"] =
        "owned_scale_frozen_during_stop";
    result.diagnostics["timeline_authority_tier"] = "owned_scale_asserted";
    return result;
  }

  const auto call_start_ns = clock_.now();
  if (stop_token.stop_requested()) {
    return cancelled_result("spring_back_entry");
  }
  // Request the worker to stop. The worker's stop_token wakes it,
  // it publishes a kStopped snapshot at its current observed
  // position, then clears `state_.running`.
  state_.pause_requested.store(false);
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  worker_thread_.request_stop();
  execution_cv_.notify_all();

  // Wait bounded by the profile's max_stop_time_s when set; otherwise
  // derive the bound from the requested stop duration plus the native
  // sample-period tolerance. This keeps a 2 s default stop from timing
  // out on a legacy 1 s backend fallback.
  const auto wait_deadline_s = controlled_stop_wait_timeout_s(
      profile, config_.soft_on_path_sample_period_s);
  std::unique_lock<std::mutex> lock(execution_mutex_);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(wait_deadline_s));
  bool stopped = false;
  while (!stop_token.stop_requested()) {
    stopped = !state_.running.load();
    if (stopped) {
      break;
    }
    if (execution_cv_.wait_until(lock, deadline) ==
        std::cv_status::timeout) {
      stopped = !state_.running.load();
      break;
    }
  }
  lock.unlock();
  if (stop_token.stop_requested()) {
    return cancelled_result("spring_back_wait");
  }

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
  result.diagnostics["runtime_scale_isolation"] =
      "owned_scale_frozen_during_stop";
  result.diagnostics["timeline_authority_tier"] = "owned_scale_asserted";
  return result;
}

core::PauseResultDTO RoboPlanExecutionBackend::pause() {
  if (!state_.running.load()) {
    return core::PauseResultDTO{
        false, core::ErrorDTO{core::kErrorInvalidRequest, "No execution in flight", {}}};
  }
  state_.pause_requested.store(true);
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  execution_cv_.notify_all();
  return core::PauseResultDTO{true, std::nullopt};
}

core::ExecutionHandle RoboPlanExecutionBackend::resume(
    const core::TrajectoryDTO& trajectory) {
  if (state_.running.load()) {
    state_.pause_requested.store(false);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    if (worker_thread_.joinable()) {
      worker_thread_.request_stop();
    }
    execution_cv_.notify_all();
    std::unique_lock<std::mutex> lock(execution_mutex_);
    execution_cv_.wait(lock, [this]() { return !state_.running.load(); });
  }
  return execute_trajectory(trajectory);
}

core::ResetResultDTO RoboPlanExecutionBackend::reset() {
  const auto reset_timeout =
      std::chrono::duration<double>(config_.reset_completion_timeout_s);

  state_.pause_requested.store(false);
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
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

void RoboPlanExecutionBackend::set_runtime_velocity_scale(double scale) {
  // ADR-0018: clamp + atomic store. Caller (kernel) already validated
  // finiteness, but defense-in-depth keeps the workers from ever reading
  // NaN/Inf via this path.
  if (!std::isfinite(scale)) return;
  if (scale < 0.0) scale = 0.0;
  if (scale > 1.0) scale = 1.0;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ > 0) {
      // M4: freeze the owned runtime scale during controlled stops. Slider/API
      // intent is applied after the stop so it cannot stretch, freeze, or
      // accelerate the commanded stop profile.
      deferred_runtime_velocity_scale_ = scale;
      return;
    }
    runtime_velocity_scale_.store(scale, std::memory_order_release);
  }
  // Wake the execution worker so a scale=0 hold or resume takes effect promptly.
  execution_cv_.notify_all();
}

void RoboPlanExecutionBackend::begin_controlled_stop_runtime_scale_freeze() {
  std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
  ++controlled_stop_runtime_scale_freeze_depth_;
}

bool RoboPlanExecutionBackend::end_controlled_stop_runtime_scale_freeze() {
  bool applied_deferred_scale = false;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ <= 0) {
      return false;
    }
    --controlled_stop_runtime_scale_freeze_depth_;
    if (controlled_stop_runtime_scale_freeze_depth_ == 0 &&
        deferred_runtime_velocity_scale_.has_value()) {
      runtime_velocity_scale_.store(*deferred_runtime_velocity_scale_,
                                    std::memory_order_release);
      deferred_runtime_velocity_scale_.reset();
      applied_deferred_scale = true;
    }
  }
  if (applied_deferred_scale) {
    execution_cv_.notify_all();
  }
  return applied_deferred_scale;
}

}  // namespace manyforge::roboplan_backend
