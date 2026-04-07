#include <manyforge_roboplan/roboplan_execution_backend.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

constexpr auto kExecutionTick = std::chrono::milliseconds(10);

double clamp_progress(double progress) {
  return std::max(0.0, std::min(1.0, progress));
}

}  // namespace

RoboPlanExecutionBackend::RoboPlanExecutionBackend(
    std::shared_ptr<roboplan::Scene> scene, core::IClock& clock,
    std::shared_ptr<std::mutex> scene_mutex)
    : scene_(std::move(scene)),
      clock_(clock),
      scene_mutex_(std::move(scene_mutex)) {
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
  state_.snapshot.timestamp_ns = clock_.now();
}

RoboPlanExecutionBackend::~RoboPlanExecutionBackend() {
  (void)reset();
  join_worker_if_needed();
}

void RoboPlanExecutionBackend::join_worker_if_needed() {
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
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

void RoboPlanExecutionBackend::execution_worker(core::TrajectoryDTO trajectory) {
  const double total_duration_s = trajectory.waypoints.empty()
                                      ? 0.0
                                      : trajectory.waypoints.back().time_from_start_s;
  std::vector<double> latest_positions =
      trajectory.waypoints.empty() ? std::vector<double>{}
                                   : trajectory.waypoints.front().positions_rad;

  update_scene_positions(latest_positions);
  publish_snapshot(core::ExecutionState::kExecuting, 0.0, latest_positions);

  const int64_t start_time_ns = clock_.now();
  int64_t accumulated_pause_ns = 0;

  while (true) {
    const bool stop_requested = state_.stop_requested.load();
    const bool pause_requested = state_.pause_requested.load();

    if (stop_requested) {
      publish_snapshot(core::ExecutionState::kStopped,
                       get_execution_snapshot().progress, latest_positions);
      break;
    }

    if (pause_requested) {
      publish_snapshot(core::ExecutionState::kPaused,
                       get_execution_snapshot().progress, latest_positions);
      const int64_t pause_started_ns = clock_.now();
      std::unique_lock<std::mutex> lock(execution_mutex_);
      execution_cv_.wait(lock, [this]() {
        return state_.stop_requested.load() || !state_.pause_requested.load();
      });
      accumulated_pause_ns += clock_.now() - pause_started_ns;
      if (state_.stop_requested.load()) {
        continue;
      }
      publish_snapshot(core::ExecutionState::kExecuting,
                       get_execution_snapshot().progress, latest_positions);
      continue;
    }

    double elapsed_s = 0.0;
    if (total_duration_s > 0.0) {
      const int64_t elapsed_ns = clock_.now() - start_time_ns - accumulated_pause_ns;
      elapsed_s = static_cast<double>(elapsed_ns) / 1e9;
    }

    const double progress =
        total_duration_s <= 1e-9 ? 1.0 : clamp_progress(elapsed_s / total_duration_s);
    latest_positions = interpolate_joint_positions(trajectory, elapsed_s);
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
    execution_cv_.wait_for(lock, kExecutionTick, [this]() {
      return state_.stop_requested.load() || state_.pause_requested.load();
    });
  }

  state_.running.store(false);
  state_.stop_requested.store(false);
  state_.pause_requested.store(false);
  execution_cv_.notify_all();
}

core::ExecutionHandle RoboPlanExecutionBackend::execute_trajectory(
    const core::TrajectoryDTO& trajectory) {
  join_worker_if_needed();

  const std::vector<double> initial_positions =
      trajectory.waypoints.empty() ? std::vector<double>{}
                                   : trajectory.waypoints.front().positions_rad;

  state_.running.store(true);
  state_.stop_requested.store(false);
  state_.pause_requested.store(false);
  publish_snapshot(core::ExecutionState::kExecuting, 0.0, initial_positions);
  update_scene_positions(initial_positions);

  worker_thread_ = std::thread(&RoboPlanExecutionBackend::execution_worker, this,
                               trajectory);

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

core::StopResultDTO RoboPlanExecutionBackend::stop() {
  if (!state_.running.load()) {
    return core::StopResultDTO{
        false, core::ErrorDTO{core::kErrorInvalidRequest, "No execution in flight", {}}};
  }
  state_.stop_requested.store(true);
  state_.pause_requested.store(false);
  execution_cv_.notify_all();
  return core::StopResultDTO{true, std::nullopt};
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
  state_.stop_requested.store(true);
  state_.pause_requested.store(false);
  execution_cv_.notify_all();
  join_worker_if_needed();
  publish_snapshot(core::ExecutionState::kIdle, 0.0, {});
  return core::ResetResultDTO{true, std::nullopt};
}

core::ExecutionSnapshot RoboPlanExecutionBackend::get_execution_snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_.snapshot;
}

}  // namespace manyforge::roboplan_backend
