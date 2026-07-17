#include <manyforge_roboplan/roboplan_execution_backend.hpp>

#include <Eigen/Dense>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <roboplan/core/scene_utils.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

constexpr auto kExecutionTick = std::chrono::milliseconds(10);
constexpr int kTerminalSceneProjectionRetryAttempts = 10;
constexpr auto kTerminalSceneProjectionRetryDelay = std::chrono::milliseconds(20);
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
  if (!config_.observed_joint_names.empty()) {
    core::ObservedRobotStateSourceConfig source;
    source.kind = core::ObservedRobotStateSourceKind::kPredictedState;
    source.source_id = "native_predicted_state";
    source.authority_profile =
        core::ObservedRobotStateAuthorityProfile::kPredictedNative;
    source.freshness_budget_ns = 0;
    source.authority_profile_id = "native_predicted_v1";
    observed_state_publication_ =
        std::make_unique<core::ObservedRobotStatePublication>(
            config_.observed_joint_names,
            std::vector<core::ObservedRobotStateSourceConfig>{
                std::move(source)});
    observed_at_rest_velocities_.assign(config_.observed_joint_names.size(),
                                        0.0);
  }
  execution_progress_publication_ =
      std::make_unique<core::ExecutionProgressPublication>(
          core::ExecutionProgressSourceKind::kBackendCommanded,
          core::ExecutionProgressAuthorityProfile::kPredictedNative,
          "native_predicted_progress_v1",
          /*freshness_budget_ns=*/0);
  state_.snapshot.timestamp_ns = clock_.now();
}

RoboPlanExecutionBackend::~RoboPlanExecutionBackend() {
  stop_terminal_scene_projection_retry_worker();
  (void)reset();
  // jthread auto-joins
}

std::int64_t RoboPlanExecutionBackend::publish_snapshot(
    core::ExecutionState state, double progress,
    const std::vector<double>& joint_positions,
    std::optional<core::ErrorDTO> error) {
  const auto timestamp_ns = clock_.now();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.snapshot.timestamp_ns = timestamp_ns;
    state_.snapshot.sequence_number++;
    state_.snapshot.state = state;
    state_.snapshot.progress = clamp_progress(progress);
    state_.snapshot.joint_positions_rad = joint_positions;
    state_.snapshot.error = std::move(error);
  }
  // RT-8C publication is deliberately outside state_mutex_: the RT0 writer
  // has its own bounded preallocated lane and cannot queue behind lifecycle
  // snapshot readers.
  if (observed_state_publication_ != nullptr &&
      joint_positions.size() == config_.observed_joint_names.size()) {
    const bool at_rest_state =
        state == core::ExecutionState::kIdle ||
        state == core::ExecutionState::kPaused ||
        state == core::ExecutionState::kStopped ||
        state == core::ExecutionState::kCompleted ||
        state == core::ExecutionState::kFailed ||
        state == core::ExecutionState::kInterventionRequired;
    const std::span<const double> velocities =
        at_rest_state
            ? std::span<const double>(observed_at_rest_velocities_)
            : std::span<const double>{};
    (void)publish_observed_state_authority(
        joint_positions, velocities, timestamp_ns, at_rest_state);
  } else if (observed_state_publication_ != nullptr &&
             joint_positions.empty() &&
             (state == core::ExecutionState::kIdle ||
              state == core::ExecutionState::kStopped ||
              state == core::ExecutionState::kCompleted ||
              state == core::ExecutionState::kFailed ||
              state == core::ExecutionState::kInterventionRequired)) {
    std::lock_guard<std::mutex> writer_lock(
        observed_state_publication_mutex_);
    const auto authority_epoch =
        observed_state_authority_epoch_.load(std::memory_order_relaxed);
    if ((authority_epoch & 1u) == 0u) {
      observed_state_authority_epoch_.store(authority_epoch + 1,
                                            std::memory_order_release);
    }
  }
  return timestamp_ns;
}

core::ObservedRobotStatePublishResult
RoboPlanExecutionBackend::publish_observed_state_authority(
    std::span<const double> positions,
    std::span<const double> velocities,
    std::int64_t timestamp_ns,
    bool authority_boundary) noexcept {
  if (observed_state_publication_ == nullptr ||
      positions.size() != config_.observed_joint_names.size()) {
    return core::ObservedRobotStatePublishResult{
        false, false, false, 0, "observed_state_shape_mismatch"};
  }

  std::lock_guard<std::mutex> writer_lock(observed_state_publication_mutex_);
  auto authority_epoch =
      observed_state_authority_epoch_.load(std::memory_order_relaxed);
  if (authority_boundary) {
    if ((authority_epoch & 1u) == 0u) {
      ++authority_epoch;
      observed_state_authority_epoch_.store(authority_epoch,
                                            std::memory_order_release);
    }
  } else if ((authority_epoch & 1u) != 0u) {
    return core::ObservedRobotStatePublishResult{
        false, false, false, 0, "observed_state_authority_invalidated"};
  }

  core::ObservedRobotStatePublishResult published;
  const int attempt_count = authority_boundary ? 8 : 1;
  for (int attempt = 0; attempt < attempt_count; ++attempt) {
    published = observed_state_publication_->publish(
        core::ObservedRobotStateSourceKind::kPredictedState,
        config_.observed_joint_names, positions, velocities, timestamp_ns,
        timestamp_ns);
    if (published.published) {
      if (authority_boundary) {
        observed_state_authority_epoch_.store(authority_epoch + 1,
                                              std::memory_order_release);
      }
      return published;
    }
    if (std::string_view(published.reason) != "publication_slots_exhausted") {
      return published;
    }
    std::this_thread::yield();
  }
  return published;
}

bool RoboPlanExecutionBackend::update_scene_positions(
    const std::vector<double>& joint_positions,
    SceneUpdateMode mode,
    std::int64_t source_timestamp_ns) {
  if (joint_positions.empty()) {
    return true;
  }

  std::unique_lock<std::mutex> scene_lock(*scene_mutex_, std::defer_lock);
  if (mode == SceneUpdateMode::kBlocking) {
    scene_lock.lock();
  } else if (!scene_lock.try_lock()) {
    record_scene_projection_miss();
    return false;
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
  return true;
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
  metrics.terminal_retries_scheduled_total =
      terminal_scene_projection_retries_scheduled_total_.load(
          std::memory_order_relaxed);
  metrics.terminal_retry_successes_total =
      terminal_scene_projection_retry_successes_total_.load(
          std::memory_order_relaxed);
  metrics.terminal_retry_exhausted_total =
      terminal_scene_projection_retry_exhausted_total_.load(
          std::memory_order_relaxed);
  const auto now_ns = clock_.now();
  if (metrics.last_success_timestamp_ns > 0 &&
      now_ns >= metrics.last_success_timestamp_ns) {
    metrics.last_success_age_ns =
        static_cast<std::uint64_t>(now_ns - metrics.last_success_timestamp_ns);
  }
  return metrics;
}

void RoboPlanExecutionBackend::schedule_terminal_scene_projection_retry(
    std::vector<double> joint_positions,
    std::int64_t source_timestamp_ns) {
  if (joint_positions.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(terminal_scene_projection_retry_mutex_);
    pending_terminal_scene_projection_ =
        PendingTerminalSceneProjection{std::move(joint_positions),
                                       source_timestamp_ns};
    terminal_scene_projection_retries_scheduled_total_.fetch_add(
        1, std::memory_order_relaxed);
    if (!terminal_scene_projection_retry_thread_.joinable()) {
      terminal_scene_projection_retry_thread_ = std::jthread(
          [this](std::stop_token stop_token) {
            terminal_scene_projection_retry_worker(stop_token);
          });
    }
  }
  terminal_scene_projection_retry_cv_.notify_one();
}

void RoboPlanExecutionBackend::terminal_scene_projection_retry_worker(
    std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    PendingTerminalSceneProjection pending;
    {
      std::unique_lock<std::mutex> lock(
          terminal_scene_projection_retry_mutex_);
      // Token-aware wait: a plain condition_variable whose predicate reads
      // stop_requested() (state that flips outside this mutex) can lose the
      // stop wakeup between predicate evaluation and enqueue, deadlocking the
      // destructor join. The (lock, stoken, pred) overload re-checks the stop
      // state under the lock and cannot lose it.
      terminal_scene_projection_retry_cv_.wait(
          lock, stop_token, [this]() {
            return pending_terminal_scene_projection_.has_value();
          });
      if (stop_token.stop_requested()) {
        return;
      }
      pending = std::move(*pending_terminal_scene_projection_);
      pending_terminal_scene_projection_.reset();
    }

    bool projected = false;
    for (int attempt = 0; attempt < kTerminalSceneProjectionRetryAttempts;
         ++attempt) {
      if (stop_token.stop_requested()) {
        return;
      }
      if (update_scene_positions(pending.joint_positions,
                                 SceneUpdateMode::kBestEffort,
                                 pending.source_timestamp_ns)) {
        projected = true;
        terminal_scene_projection_retry_successes_total_.fetch_add(
            1, std::memory_order_relaxed);
        break;
      }
      if (attempt + 1 < kTerminalSceneProjectionRetryAttempts) {
        std::this_thread::sleep_for(kTerminalSceneProjectionRetryDelay);
      }
    }
    if (!projected && !stop_token.stop_requested()) {
      terminal_scene_projection_retry_exhausted_total_.fetch_add(
          1, std::memory_order_relaxed);
    }
  }
}

void RoboPlanExecutionBackend::stop_terminal_scene_projection_retry_worker()
    noexcept {
  if (terminal_scene_projection_retry_thread_.joinable()) {
    // request_stop() wakes the token-aware wait through the jthread's
    // internal stop_callback; no manual notify is needed (and a manual
    // notify without the mutex would be the lost-wakeup pattern this
    // codebase has already fixed twice).
    terminal_scene_projection_retry_thread_.request_stop();
    terminal_scene_projection_retry_thread_.join();
  }
}

void RoboPlanExecutionBackend::execution_worker(
    std::stop_token st, core::TrajectoryDTO trajectory,
    std::uint64_t execution_generation) {
  const double total_duration_s = trajectory.waypoints.empty()
                                      ? 0.0
                                      : trajectory.waypoints.back().time_from_start_s;
  std::vector<double> latest_positions =
      trajectory.waypoints.empty() ? std::vector<double>{}
                                   : trajectory.waypoints.front().positions_rad;
  bool completed_normally = false;

  if (st.stop_requested()) {
    state_.running.store(false);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    execution_cv_.notify_all();
    (void)execution_progress_publication_->invalidate_generation(
        execution_generation);
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
    state_.active_trajectory =
        std::make_shared<const core::TrajectoryDTO>(trajectory);
    state_.active_trajectory_time_s = trajectory_time_s;
    state_.active_runtime_velocity_scale = applied_runtime_scale;
  }

  while (true) {
    const bool pause_requested = state_.pause_requested.load();

    if (st.stop_requested()) {
      const auto stopped_snapshot_ns =
          publish_snapshot(core::ExecutionState::kStopped,
                           get_execution_snapshot().progress, latest_positions);
      if (!update_scene_positions(latest_positions,
                                  SceneUpdateMode::kBestEffort,
                                  stopped_snapshot_ns)) {
        schedule_terminal_scene_projection_retry(latest_positions,
                                                 stopped_snapshot_ns);
      }
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
    trajectory_time_s = std::min(
        total_duration_s, trajectory_time_s + dt_s * applied_runtime_scale);
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_.active_trajectory_time_s = trajectory_time_s;
      state_.active_runtime_velocity_scale = applied_runtime_scale;
    }
    (void)execution_progress_publication_->publish(
        execution_generation,
        static_cast<std::int64_t>(
            std::max(0.0, trajectory_time_s) * 1e9),
        now_ns,
        now_ns);

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
      const auto terminal_progress_ns = clock_.now();
      (void)execution_progress_publication_->publish_terminal(
          execution_generation,
          static_cast<std::int64_t>(std::max(0.0, total_duration_s) * 1e9),
          terminal_progress_ns, terminal_progress_ns);
      completed_normally = true;
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
  if (!completed_normally) {
    (void)execution_progress_publication_->invalidate_generation(
        execution_generation);
  }
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
  const auto execution_generation =
      next_execution_progress_generation_.fetch_add(
          1, std::memory_order_acq_rel);
  execution_progress_publication_->invalidate();
  const auto progress_started_at_ns = clock_.now();
  const auto initial_progress = execution_progress_publication_->publish(
      execution_generation, 0, progress_started_at_ns,
      progress_started_at_ns);
  if (!initial_progress.published) {
    state_.running.store(false);
    publish_snapshot(
        core::ExecutionState::kFailed, 0.0, initial_positions,
        core::ErrorDTO{core::kErrorExecutionRejected,
                       "Native execution progress authority could not be armed",
                       {{"reason", initial_progress.reason}}});
    return core::ExecutionHandle{
        [](int64_t) { return true; },
        [this]() { return get_execution_snapshot(); },
        0};
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.active_trajectory =
        std::make_shared<const core::TrajectoryDTO>(trajectory);
    state_.active_trajectory_time_s = 0.0;
    state_.active_runtime_velocity_scale = std::max(
        0.0, std::min(1.0, runtime_velocity_scale_.load(std::memory_order_acquire)));
  }
  const auto initial_snapshot_ns =
      publish_snapshot(core::ExecutionState::kExecuting, 0.0, initial_positions);
  update_scene_positions(initial_positions, SceneUpdateMode::kBestEffort,
                         initial_snapshot_ns);

  worker_thread_ = std::jthread(
      [this, trajectory_copy = core::TrajectoryDTO(trajectory),
       execution_generation](
          std::stop_token st) mutable {
        execution_worker(st, std::move(trajectory_copy),
                         execution_generation);
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
      [this]() { return get_execution_snapshot(); },
      execution_generation};
}

core::Result<void> RoboPlanExecutionBackend::sync_observed_state_before_planning() {
  if (observed_state_publication_ == nullptr) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Native observed-state publication is unavailable",
        {{"reason", "observed_state_publication_unavailable"}}});
  }

  std::vector<double> positions;
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const auto maybe_group = scene_->getJointGroupInfo(group_name_);
    if (!maybe_group) {
      return core::Result<void>::Err(core::ErrorDTO{
          core::kErrorExecutionRejected,
          "Native observed-state group is unavailable",
          {{"reason", "observed_state_group_unavailable"},
           {"group_id", group_name_},
           {"error", maybe_group.error()}}});
    }
    const auto& group = maybe_group.value();
    Eigen::VectorXd compact =
        scene_->getCurrentJointPositions()(group.q_indices);
    if (group.has_continuous_dofs) {
      auto collapsed = roboplan::collapseContinuousJointPositions(
          *scene_, group_name_, compact);
      if (!collapsed) {
        return core::Result<void>::Err(core::ErrorDTO{
            core::kErrorExecutionRejected,
            "Native continuous-joint state could not be normalized",
            {{"reason", "observed_state_continuous_joint_collapse_failed"},
             {"error", collapsed.error()}}});
      }
      compact = std::move(collapsed.value());
    }
    positions.resize(static_cast<std::size_t>(compact.size()));
    for (Eigen::Index index = 0; index < compact.size(); ++index) {
      positions[static_cast<std::size_t>(index)] = compact[index];
    }
  } catch (const std::exception& error) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Native observed state could not be captured before planning",
        {{"reason", "observed_state_capture_failed"},
         {"exception", error.what()}}});
  }

  if (positions.size() != config_.observed_joint_names.size()) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Native observed-state shape does not match the qualified joint order",
        {{"reason", "observed_state_shape_mismatch"},
         {"expected_joint_count",
          std::to_string(config_.observed_joint_names.size())},
         {"observed_joint_count", std::to_string(positions.size())}}});
  }

  const auto now_ns = clock_.now();
  const auto state = get_execution_snapshot().state;
  const bool at_rest = state == core::ExecutionState::kIdle ||
                       state == core::ExecutionState::kPaused ||
                       state == core::ExecutionState::kStopped ||
                       state == core::ExecutionState::kCompleted ||
                       state == core::ExecutionState::kFailed ||
                       state == core::ExecutionState::kInterventionRequired;
  const std::span<const double> velocities =
      at_rest ? std::span<const double>(observed_at_rest_velocities_)
              : std::span<const double>{};
  auto published = publish_observed_state_authority(
      positions, velocities, now_ns, /*authority_boundary=*/true);
  if (!published.published || !published.complete) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Native observed state was rejected by the publication lane",
        {{"reason", published.reason != nullptr && published.reason[0] != '\0'
                        ? published.reason
                        : "observed_state_publish_rejected"}}});
  }
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
  request_active_controlled_stop();
  worker_thread_.request_stop();
  execution_cv_.notify_all();
  return core::StopResultDTO{true, std::nullopt};
}

void RoboPlanExecutionBackend::request_active_controlled_stop() {
  std::shared_ptr<std::stop_source> source;
  {
    std::lock_guard<std::mutex> lock(controlled_stop_mutex_);
    source = active_controlled_stop_source_;
  }
  if (source) {
    source->request_stop();
  }
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
  auto backend_stop_source = std::make_shared<std::stop_source>();
  {
    std::lock_guard<std::mutex> lock(controlled_stop_mutex_);
    active_controlled_stop_source_ = backend_stop_source;
  }
  const auto clear_active_controlled_stop =
      std::shared_ptr<void>(nullptr, [this, backend_stop_source](void*) {
        std::lock_guard<std::mutex> lock(controlled_stop_mutex_);
        if (active_controlled_stop_source_ == backend_stop_source) {
          active_controlled_stop_source_.reset();
        }
      });
  const auto controlled_stop_requested = [&]() {
    return stop_token.stop_requested() ||
           backend_stop_source->stop_requested();
  };

  const auto cancelled_result = [&](std::string phase) {
    state_.running.store(false);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    const auto latest = get_execution_snapshot();
    const auto stopped_snapshot_ns =
        publish_snapshot(core::ExecutionState::kStopped,
                         latest.progress,
                         latest.joint_positions_rad);
    if (!update_scene_positions(latest.joint_positions_rad,
                                SceneUpdateMode::kBestEffort,
                                stopped_snapshot_ns)) {
      schedule_terminal_scene_projection_retry(latest.joint_positions_rad,
                                               stopped_snapshot_ns);
    }
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
    if (controlled_stop_requested()) {
      return cancelled_result("soft_on_path_entry");
    }
    std::shared_ptr<const core::TrajectoryDTO> active_trajectory;
    core::ExecutionSnapshot active_snapshot;
    double active_time_s = 0.0;
    double active_runtime_scale = 1.0;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      active_snapshot = state_.snapshot;
      active_trajectory = state_.active_trajectory;
      active_time_s = state_.active_trajectory_time_s;
      active_runtime_scale = state_.active_runtime_velocity_scale;
    }

    if (!active_trajectory || active_trajectory->waypoints.empty() ||
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
    initial.state.joint_names = active_trajectory->joint_names;
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
        *active_trajectory, profile, initial, planner_config);
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
          while (!controlled_stop_requested()) {
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
        if (controlled_stop_requested()) {
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
      const auto recommended_fallback_it =
          planned.error().details.find("recommended_fallback");
      const bool planner_recommends_spring_back =
          recommended_fallback_it != planned.error().details.end() &&
          recommended_fallback_it->second == "spring_back_hold";
      if (may_fallback && planner_recommends_spring_back) {
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
      while (!controlled_stop_requested()) {
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
      if (controlled_stop_requested()) {
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
      while (!controlled_stop_requested() &&
             std::chrono::steady_clock::now() < wake_time) {
        const auto remaining = wake_time - std::chrono::steady_clock::now();
        const auto max_slice =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                kExecutionTick);
        std::this_thread::sleep_for(std::min(remaining, max_slice));
      }
      if (controlled_stop_requested()) {
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
      if (!waypoint.velocities_rad_s.empty()) {
        std::lock_guard<std::mutex> velocity_lock(state_mutex_);
        state_.snapshot.joint_velocities_rad_s = waypoint.velocities_rad_s;
      }
      update_scene_positions(waypoint.positions_rad,
                             SceneUpdateMode::kBestEffort,
                             snapshot_ns);
    }

    const auto call_end_ns = clock_.now();
    const auto& final_wp = replacement.waypoints.back();
    const auto stopped_snapshot_ns =
        publish_snapshot(core::ExecutionState::kStopped, 1.0, final_wp.positions_rad);
    if (!final_wp.velocities_rad_s.empty()) {
      std::lock_guard<std::mutex> velocity_lock(state_mutex_);
      state_.snapshot.joint_velocities_rad_s = final_wp.velocities_rad_s;
    }
    if (!update_scene_positions(final_wp.positions_rad,
                                SceneUpdateMode::kBestEffort,
                                stopped_snapshot_ns)) {
      schedule_terminal_scene_projection_retry(final_wp.positions_rad,
                                               stopped_snapshot_ns);
    }
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

  std::shared_ptr<const core::TrajectoryDTO> active_trajectory;
  core::ExecutionSnapshot active_snapshot;
  double active_time_s = 0.0;
  double active_runtime_scale = 1.0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_snapshot = state_.snapshot;
    active_trajectory = state_.active_trajectory;
    active_time_s = state_.active_trajectory_time_s;
    active_runtime_scale = state_.active_runtime_velocity_scale;
  }

  // If no execution is in flight, there's nothing to preempt. The kernel never
  // calls this without an active trip, but defense in depth handles the case
  // anyway.
  if (!state_.running.load()) {
    if (active_snapshot.joint_positions_rad.empty()) {
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

  if (!active_trajectory || active_trajectory->waypoints.empty() ||
      active_snapshot.joint_positions_rad.empty()) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "native spring_back_hold requires an active trajectory and observed state",
        {{"reason", "spring_back_missing_active_trajectory_or_state"},
         {"backend_capability_profile_id", "roboplan_native_v2_soft_on_path"}}};
    return result;
  }

  core::SoftOnPathInitialStateDTO initial;
  initial.state.joint_names = active_trajectory->joint_names;
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

  core::MotionInterruptStopProfileDTO spring_profile = profile;
  spring_profile.deceleration_time_s =
      core::motion_interrupt_deceleration_time_or_default(profile);
  auto planned = core::plan_spring_back_on_path_stop(
      *active_trajectory, spring_profile, initial, planner_config);
  if (!planned.ok()) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = planned.error();
    return result;
  }

  const auto call_start_ns = clock_.now();
  if (controlled_stop_requested()) {
    return cancelled_result("spring_back_entry");
  }
  // Request the regular worker to stop, then replay the spring-back replacement
  // on the native command stream. This mirrors the ros2_control lane: the
  // controlled stop itself is a replacement trajectory, not cancel-first hold
  // behavior.
  state_.pause_requested.store(false);
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  worker_thread_.request_stop();
  execution_cv_.notify_all();

  const auto wait_deadline_s = controlled_stop_wait_timeout_s(
      profile, config_.soft_on_path_sample_period_s);
  std::unique_lock<std::mutex> lock(execution_mutex_);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(wait_deadline_s));
  bool stopped = false;
  while (!controlled_stop_requested()) {
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
  if (controlled_stop_requested()) {
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
        steady_start +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(waypoint.time_from_start_s));
    while (!controlled_stop_requested() &&
           std::chrono::steady_clock::now() < wake_time) {
      const auto remaining = wake_time - std::chrono::steady_clock::now();
      const auto max_slice =
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              kExecutionTick);
      std::this_thread::sleep_for(std::min(remaining, max_slice));
    }
    if (controlled_stop_requested()) {
      return cancelled_result("spring_back_replay");
    }
    const double progress =
        total_time_s <= 1e-9
            ? 1.0
            : clamp_progress(waypoint.time_from_start_s / total_time_s);
    const auto snapshot_ns =
        publish_snapshot(core::ExecutionState::kStopping,
                         progress,
                         waypoint.positions_rad);
    if (!waypoint.velocities_rad_s.empty()) {
      std::lock_guard<std::mutex> velocity_lock(state_mutex_);
      state_.snapshot.joint_velocities_rad_s = waypoint.velocities_rad_s;
    }
    update_scene_positions(waypoint.positions_rad,
                           SceneUpdateMode::kBestEffort,
                           snapshot_ns);
  }

  const auto replay_end_ns = clock_.now();
  const auto& final_wp = replacement.waypoints.back();
  const auto stopped_snapshot_ns =
      publish_snapshot(core::ExecutionState::kStopped, 1.0,
                       final_wp.positions_rad);
  if (!final_wp.velocities_rad_s.empty()) {
    std::lock_guard<std::mutex> velocity_lock(state_mutex_);
    state_.snapshot.joint_velocities_rad_s = final_wp.velocities_rad_s;
  }
  if (!update_scene_positions(final_wp.positions_rad,
                              SceneUpdateMode::kBestEffort,
                              stopped_snapshot_ns)) {
    schedule_terminal_scene_projection_retry(final_wp.positions_rad,
                                             stopped_snapshot_ns);
  }
  state_.running.store(false);
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  execution_cv_.notify_all();

  result.accepted = true;
  result.backend_stop_confirmed = true;
  result.executed_stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  result.stop_command_source =
      core::MotionInterruptStopCommandSource::kNativeSimulatedPathRamp;
  result.confirmation_latency_ns =
      std::max<std::int64_t>(0, replay_end_ns - call_start_ns);
  result.path_deviation_max_rad = planned.value().path_deviation_max_rad;
  result.initial_path_deviation_rad =
      planned.value().initial_path_deviation_rad;
  result.stop_distance_rad = 0.0;
  if (!active_snapshot.joint_positions_rad.empty() &&
      final_wp.positions_rad.size() ==
          active_snapshot.joint_positions_rad.size()) {
    double sum = 0.0;
    for (size_t i = 0; i < final_wp.positions_rad.size(); ++i) {
      const double d = final_wp.positions_rad[i] -
                       active_snapshot.joint_positions_rad[i];
      sum += d * d;
    }
    result.stop_distance_rad = std::sqrt(sum);
  }
  result.diagnostics = planned.value().diagnostics;
  result.diagnostics["command_source"] =
      "native_simulated_spring_back_path_ramp";
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
    request_active_controlled_stop();
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
  execution_progress_publication_->invalidate();
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  request_active_controlled_stop();
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
