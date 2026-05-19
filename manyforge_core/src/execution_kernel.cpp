#include <manyforge_core/execution_kernel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <string>

namespace manyforge::core {
namespace {

constexpr int64_t kExecutionPollNs = 10'000'000;  // 10 ms
constexpr int kExecutionDrainMaxPolls = 100;      // bounded 1 s software drain
constexpr double kExecutionDrainTimeoutS =
    static_cast<double>(kExecutionPollNs * kExecutionDrainMaxPolls) / 1e9;

double clamp_unit_interval(double value) {
  return std::max(0.0, std::min(1.0, value));
}

Pose3DTO multiply_poses(const Pose3DTO& a, const Pose3DTO& b) {
  auto pose_to_matrix = [](const Pose3DTO& pose) {
    std::array<std::array<double, 4>, 4> m{{
        {{1.0, 0.0, 0.0, 0.0}},
        {{0.0, 1.0, 0.0, 0.0}},
        {{0.0, 0.0, 1.0, 0.0}},
        {{0.0, 0.0, 0.0, 1.0}},
    }};
    const double x = pose.orientation.x;
    const double y = pose.orientation.y;
    const double z = pose.orientation.z;
    const double w = pose.orientation.w;
    m[0][0] = 1 - 2 * y * y - 2 * z * z;
    m[0][1] = 2 * x * y - 2 * z * w;
    m[0][2] = 2 * x * z + 2 * y * w;
    m[1][0] = 2 * x * y + 2 * z * w;
    m[1][1] = 1 - 2 * x * x - 2 * z * z;
    m[1][2] = 2 * y * z - 2 * x * w;
    m[2][0] = 2 * x * z - 2 * y * w;
    m[2][1] = 2 * y * z + 2 * x * w;
    m[2][2] = 1 - 2 * x * x - 2 * y * y;
    m[0][3] = pose.position_m.x_m;
    m[1][3] = pose.position_m.y_m;
    m[2][3] = pose.position_m.z_m;
    return m;
  };

  auto matrix_to_pose = [](const std::array<std::array<double, 4>, 4>& m) {
    Pose3DTO pose;
    pose.position_m = {m[0][3], m[1][3], m[2][3]};

    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0) {
      const double s = std::sqrt(trace + 1.0) * 2.0;
      pose.orientation.w = 0.25 * s;
      pose.orientation.x = (m[2][1] - m[1][2]) / s;
      pose.orientation.y = (m[0][2] - m[2][0]) / s;
      pose.orientation.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
      pose.orientation.w = (m[2][1] - m[1][2]) / s;
      pose.orientation.x = 0.25 * s;
      pose.orientation.y = (m[0][1] + m[1][0]) / s;
      pose.orientation.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
      pose.orientation.w = (m[0][2] - m[2][0]) / s;
      pose.orientation.x = (m[0][1] + m[1][0]) / s;
      pose.orientation.y = 0.25 * s;
      pose.orientation.z = (m[1][2] + m[2][1]) / s;
    } else {
      const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
      pose.orientation.w = (m[1][0] - m[0][1]) / s;
      pose.orientation.x = (m[0][2] + m[2][0]) / s;
      pose.orientation.y = (m[1][2] + m[2][1]) / s;
      pose.orientation.z = 0.25 * s;
    }
    return pose;
  };

  const auto ma = pose_to_matrix(a);
  const auto mb = pose_to_matrix(b);
  std::array<std::array<double, 4>, 4> out{};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (size_t k = 0; k < 4; ++k) {
        sum += ma[i][k] * mb[k][j];
      }
      out[i][j] = sum;
    }
  }
  return matrix_to_pose(out);
}

Pose3DTO pose_with_position_and_orientation(
    const Vector3DTO& position,
    const QuaternionDTO& orientation) {
  Pose3DTO pose;
  pose.position_m = position;
  pose.orientation = orientation;
  return pose;
}

ErrorCode error_code_from_dto(const ErrorDTO& error) {
  if (error.code == kErrorStartStateMismatch) {
    return ErrorCode::kStartStateMismatch;
  }
  if (error.code == kErrorMissingReference) {
    return ErrorCode::kMissingReference;
  }
  if (error.code == kErrorAmbiguousReference) {
    return ErrorCode::kAmbiguousReference;
  }
  if (error.code == kErrorValidationLimitsViolation) {
    return ErrorCode::kValidationLimitsViolation;
  }
  if (error.code == kErrorValidationCollision) {
    return ErrorCode::kValidationCollision;
  }
  if (error.code == kErrorExecutionTimeout) {
    return ErrorCode::kExecutionTimeout;
  }
  if (error.code == kErrorStopFailed) {
    return ErrorCode::kStopFailed;
  }
  if (error.code == kErrorPauseFailed) {
    return ErrorCode::kPauseFailed;
  }
  if (error.code == kErrorResumeFailed) {
    return ErrorCode::kResumeFailed;
  }
  if (error.code == kErrorResetFailed) {
    return ErrorCode::kResetFailed;
  }
  if (error.code == kErrorInvalidRequest) {
    return ErrorCode::kInvalidRequest;
  }
  return ErrorCode::kInternalError;
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
                                     uint64_t execution_id,
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
                                              uint64_t execution_id,
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

double quaternion_angular_distance_rad(const QuaternionDTO& a,
                                       const QuaternionDTO& b) {
  const double norm_a = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
  const double norm_b = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
  if (norm_a <= 0.0 || norm_b <= 0.0) {
    return std::numeric_limits<double>::infinity();
  }
  const double dot = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) /
                     (norm_a * norm_b);
  const double clamped = std::min(1.0, std::max(-1.0, std::abs(dot)));
  return 2.0 * std::acos(clamped);
}

double translation_distance_m(const Pose3DTO& a, const Pose3DTO& b) {
  const double dx = a.position_m.x_m - b.position_m.x_m;
  const double dy = a.position_m.y_m - b.position_m.y_m;
  const double dz = a.position_m.z_m - b.position_m.z_m;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool positions_match_by_index(const std::vector<double>& a,
                              const std::vector<double>& b,
                              double tolerance_rad) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i] - b[i]) > tolerance_rad) {
      return false;
    }
  }
  return true;
}

bool names_cover_positions(const std::vector<std::string>& names,
                           size_t position_count) {
  return !names.empty() && names.size() == position_count &&
         std::all_of(names.begin(), names.end(),
                     [](const std::string& name) { return !name.empty(); });
}

struct CacheCompatibilityResult {
  bool compatible = false;
  std::string reason;
};

CacheCompatibilityResult compatible_cache() {
  return CacheCompatibilityResult{true, ""};
}

CacheCompatibilityResult incompatible_cache(std::string reason) {
  return CacheCompatibilityResult{false, std::move(reason)};
}

CacheCompatibilityResult cached_joint_goal_matches_request(
    const TrajectoryWaypointDTO& end_wp,
    const std::vector<std::string>& trajectory_joint_names,
    const JointGoalDTO& joint_goal,
    double tolerance_rad) {
  if (end_wp.positions_rad.size() != joint_goal.positions_rad.size()) {
    return incompatible_cache("goal_changed");
  }

  const bool trajectory_names_complete =
      names_cover_positions(trajectory_joint_names, end_wp.positions_rad.size());
  const bool goal_names_complete =
      names_cover_positions(joint_goal.joint_names, joint_goal.positions_rad.size());

  if (trajectory_names_complete && goal_names_complete) {
    for (size_t goal_idx = 0; goal_idx < joint_goal.joint_names.size(); ++goal_idx) {
      const auto& goal_name = joint_goal.joint_names[goal_idx];
      const auto matches =
          std::count(trajectory_joint_names.begin(), trajectory_joint_names.end(), goal_name);
      if (matches != 1) {
        return incompatible_cache("goal_unverifiable");
      }
      const auto traj_it =
          std::find(trajectory_joint_names.begin(), trajectory_joint_names.end(), goal_name);
      const auto traj_idx =
          static_cast<size_t>(std::distance(trajectory_joint_names.begin(), traj_it));
      if (std::abs(end_wp.positions_rad[traj_idx] -
                   joint_goal.positions_rad[goal_idx]) > tolerance_rad) {
        return incompatible_cache("goal_changed");
      }
    }
    return compatible_cache();
  }

  if (trajectory_joint_names.empty() && joint_goal.joint_names.empty() &&
      positions_match_by_index(end_wp.positions_rad, joint_goal.positions_rad, tolerance_rad)) {
    return compatible_cache();
  }

  if (trajectory_joint_names.empty() && joint_goal.joint_names.empty()) {
    return incompatible_cache("goal_changed");
  }
  return incompatible_cache("goal_unverifiable");
}

CacheCompatibilityResult cached_pose_goal_matches_request(
    IPlanningContext& context,
    const TrajectoryDTO& trajectory,
    const TrajectoryWaypointDTO& end_wp,
    const Pose3DTO& requested_goal,
    const ExecuteRequestDTO& request) {
  if (request.tcp_frame.empty() || request.base_frame.empty()) {
    return incompatible_cache("goal_unverifiable");
  }
  RobotStateDTO final_state;
  final_state.joint_names = trajectory.joint_names;
  final_state.positions_rad = end_wp.positions_rad;
  final_state.velocities_rad_s = end_wp.velocities_rad_s;

  try {
    const auto final_tcp_pose = context.forward_kinematics(
        FkRequestDTO{final_state, request.tcp_frame, request.base_frame});
    if (translation_distance_m(final_tcp_pose, requested_goal) >
        request.tolerances.translation_tol_m) {
      return incompatible_cache("goal_changed");
    }
    if (quaternion_angular_distance_rad(final_tcp_pose.orientation,
                                        requested_goal.orientation) >
        request.tolerances.rotation_tol_rad) {
      return incompatible_cache("goal_changed");
    }
  } catch (...) {
    return incompatible_cache("fk_failed");
  }

  return compatible_cache();
}

CacheCompatibilityResult cached_trajectory_matches_request_goal(
    IPlanningContext& context,
    const TrajectoryDTO& trajectory,
    const ExecuteRequestDTO& request) {
  if (trajectory.waypoints.empty()) {
    return incompatible_cache("empty_trajectory");
  }

  const auto& end_wp = trajectory.waypoints.back();
  if (const auto* joint_goal = std::get_if<JointGoalDTO>(&request.motion)) {
    return cached_joint_goal_matches_request(
        end_wp, trajectory.joint_names, *joint_goal,
        request.tolerances.joint_position_tol_rad);
  }

  if (const auto* pose_goal = std::get_if<PoseGoalDTO>(&request.motion)) {
    return cached_pose_goal_matches_request(
        context, trajectory, end_wp, pose_goal->goal, request);
  }

  if (const auto* cartesian_path = std::get_if<CartesianPathDTO>(&request.motion)) {
    if (cartesian_path->waypoints.empty()) {
      return incompatible_cache("goal_unverifiable");
    }
    return cached_pose_goal_matches_request(
        context, trajectory, end_wp, cartesian_path->waypoints.back(), request);
  }

  return incompatible_cache("goal_unverifiable");
}

CacheCompatibilityResult cached_trajectory_matches_current_start(
    const TrajectoryDTO& trajectory,
    const RobotStateDTO& current_state,
    double tolerance_rad) {
  if (trajectory.waypoints.empty()) {
    return incompatible_cache("empty_trajectory");
  }
  const auto& start_wp = trajectory.waypoints.front();
  if (start_wp.positions_rad.size() != current_state.positions_rad.size()) {
    return incompatible_cache("start_state_changed");
  }

  const bool trajectory_names_complete =
      names_cover_positions(trajectory.joint_names, start_wp.positions_rad.size());
  const bool state_names_complete =
      names_cover_positions(current_state.joint_names, current_state.positions_rad.size());

  if (trajectory_names_complete && state_names_complete) {
    for (size_t state_idx = 0; state_idx < current_state.joint_names.size(); ++state_idx) {
      const auto& state_name = current_state.joint_names[state_idx];
      const auto matches =
          std::count(trajectory.joint_names.begin(), trajectory.joint_names.end(), state_name);
      if (matches != 1) {
        return incompatible_cache("start_state_changed");
      }
      const auto traj_it =
          std::find(trajectory.joint_names.begin(), trajectory.joint_names.end(), state_name);
      const auto traj_idx =
          static_cast<size_t>(std::distance(trajectory.joint_names.begin(), traj_it));
      if (std::abs(start_wp.positions_rad[traj_idx] -
                   current_state.positions_rad[state_idx]) > tolerance_rad) {
        return incompatible_cache("start_state_changed");
      }
    }
    return compatible_cache();
  }

  if (trajectory.joint_names.empty() && current_state.joint_names.empty()) {
    for (size_t i = 0; i < start_wp.positions_rad.size(); ++i) {
      if (std::abs(start_wp.positions_rad[i] - current_state.positions_rad[i]) >
          tolerance_rad) {
        return incompatible_cache("start_state_changed");
      }
    }
    return compatible_cache();
  }
  return incompatible_cache("start_state_changed");
}

struct PreparedExecutionStart {
  bool cache_hit = false;
  bool cache_miss = false;
  bool cache_invalidated = false;
  std::string cache_invalidation_reason;
  TrajectoryDTO trajectory;
};

}  // namespace

ExecutionKernel::ExecutionKernel(IPlannerBackend& planner,
                                 IPlanningContext& context,
                                 IExecutionBackend& execution_backend,
                                 IClock& clock,
                                 IScheduler& scheduler,
                                 TrajectoryCache& cache,
                                 ExecutionKernelConfig config)
    : planner_(planner),
      context_(context),
      execution_backend_(execution_backend),
      clock_(clock),
      scheduler_(scheduler),
      cache_(cache),
      config_(std::move(config)),
      sm_(clock),
      stop_policy_(sm_, scheduler, clock, [this]() { cache_.invalidate_all(); }),
      collision_policy_(stop_policy_,
                        [this]() { cache_.invalidate_all(); }) {
  stop_policy_.set_stop_callback([this]() { (void)execution_backend_.stop(); });
  sync_snapshot_from_context();
}

ExecutionKernel::~ExecutionKernel() {
  // Join before member destruction so worker callbacks cannot notify the
  // condition variable after it has been destroyed. This is intentionally
  // explicit instead of relying on std::jthread member teardown order.
  if (worker_thread_.joinable()) {
    worker_thread_.request_stop();
    completion_cv_.notify_all();
    worker_thread_.join();
  }
}

void ExecutionKernel::emit_event(const KernelEvent& event) {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  for (const auto& [id, callback] : subscribers_) {
    try {
      callback(event);
    } catch (...) {
      // Ignore exceptions from callbacks
    }
  }
}

SubscriptionHandle ExecutionKernel::subscribe_events(EventCallback callback) {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  uint64_t id = next_subscription_id_++;
  subscribers_[id] = std::move(callback);
  return SubscriptionHandle{id};
}

void ExecutionKernel::unsubscribe_events(SubscriptionHandle handle) {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  subscribers_.erase(handle.id);
}

void ExecutionKernel::publish_execution_snapshot(ExecutionSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  if (snapshot.timestamp_ns == 0) {
    snapshot.timestamp_ns = clock_.now();
  }
  snapshot.sequence_number = next_snapshot_sequence_++;
  published_snapshot_ = std::move(snapshot);

  {
    std::lock_guard<std::mutex> mlock(metrics_mutex_);
    metrics_.snapshots_published++;
  }
}

bool ExecutionKernel::wait_for_completion(int64_t timeout_ns) {
  std::unique_lock<std::mutex> lock(completion_mutex_);
  if (timeout_ns > 0) {
    return completion_cv_.wait_for(
        lock, std::chrono::nanoseconds(timeout_ns),
        [this]() { return execution_complete_.load(); });
  }
  completion_cv_.wait(lock, [this]() { return execution_complete_.load(); });
  return true;
}

void ExecutionKernel::start_collision_monitoring() {
  std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
  collision_monitor_active_ = true;
  ++collision_monitor_generation_;
  if (collision_monitor_handle_ != 0) {
    scheduler_.cancel(collision_monitor_handle_);
    collision_monitor_handle_ = 0;
  }
  schedule_next_collision_monitor_tick(collision_monitor_generation_);
}

void ExecutionKernel::stop_collision_monitoring() {
  std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
  collision_monitor_active_ = false;
  ++collision_monitor_generation_;
  if (collision_monitor_handle_ != 0) {
    scheduler_.cancel(collision_monitor_handle_);
    collision_monitor_handle_ = 0;
  }
}

void ExecutionKernel::schedule_next_collision_monitor_tick(uint64_t generation) {
  collision_monitor_handle_ = scheduler_.schedule(
      clock_.now() + config_.collision_monitor_period_ns, [this, generation]() {
        {
          std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
          if (!collision_monitor_active_ || collision_monitor_generation_ != generation) {
            return;
          }
          collision_monitor_handle_ = 0;
        }

        (void)collision_check_tick();

        std::lock_guard<std::mutex> lock(collision_monitor_mutex_);
        if (!collision_monitor_active_ ||
            collision_monitor_generation_ != generation ||
            sm_.status().state != ExecutionState::kExecuting) {
          return;
        }
        schedule_next_collision_monitor_tick(generation);
      });
}

std::vector<double> ExecutionKernel::interpolate_joint_positions(
    const TrajectoryDTO& trajectory, double time_s) const {
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

    const double alpha =
        clamp_unit_interval((time_s - prev.time_from_start_s) / dt);
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

TrajectoryDTO ExecutionKernel::build_remaining_trajectory(
    const TrajectoryDTO& trajectory, const ExecutionSnapshot& snapshot) const {
  if (trajectory.waypoints.empty()) {
    return trajectory;
  }

  TrajectoryDTO remaining;
  remaining.joint_names = trajectory.joint_names;

  const double total_duration_s = trajectory.waypoints.back().time_from_start_s;
  const double elapsed_s =
      total_duration_s <= 1e-9
          ? 0.0
          : clamp_unit_interval(snapshot.progress) * total_duration_s;

  std::vector<double> current_positions = snapshot.joint_positions_rad;
  if (current_positions.empty()) {
    current_positions = interpolate_joint_positions(trajectory, elapsed_s);
  }

  TrajectoryWaypointDTO current_wp;
  current_wp.time_from_start_s = 0.0;
  current_wp.positions_rad = current_positions;
  remaining.waypoints.push_back(current_wp);

  for (const auto& waypoint : trajectory.waypoints) {
    if (waypoint.time_from_start_s <= elapsed_s + 1e-9) {
      continue;
    }
    auto shifted = waypoint;
    shifted.time_from_start_s -= elapsed_s;
    remaining.waypoints.push_back(std::move(shifted));
  }

  if (remaining.waypoints.size() == 1) {
    auto final_wp = trajectory.waypoints.back();
    final_wp.time_from_start_s = std::max(0.0, total_duration_s - elapsed_s);
    if (final_wp.positions_rad != current_positions ||
        final_wp.time_from_start_s > 1e-9) {
      remaining.waypoints.push_back(std::move(final_wp));
    }
  }

  return remaining;
}

void ExecutionKernel::async_execute_worker(
    std::stop_token st, ExecuteRequestDTO request, uint64_t exec_id,
    std::optional<TrajectoryDTO> cached_trajectory) {
  try {
    // Capture initial state for correct prev_state in events (B-2)
    const auto worker_entry_state = sm_.status().state;

    // Check for early cancellation
    if (st.stop_requested()) {
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    TrajectoryDTO trajectory;
    const bool cache_hit = cached_trajectory.has_value();
    if (cache_hit) {
      trajectory = std::move(*cached_trajectory);
    } else {
      // Schedule planning timeout (W-2)
      ScheduleHandle planning_timeout_handle = 0;
      const auto planning_start_ns = clock_.now();
      const int64_t planning_timeout_ns =
          (request.timeouts.planning_timeout_s > 0)
              ? static_cast<int64_t>(request.timeouts.planning_timeout_s * 1e9)
              : 0;
      if (planning_timeout_ns > 0) {
        const int64_t deadline = planning_start_ns + planning_timeout_ns;
        planning_timeout_handle = scheduler_.schedule(deadline, [this]() {
          stop_policy_.request_hard_stop();
        });
      }

      auto plan_result = plan_trajectory(request);
      const bool planning_timed_out =
          planning_timeout_ns > 0 &&
          (clock_.now() - planning_start_ns) >= planning_timeout_ns;

      // Cancel planning timeout (may have already fired)
      if (planning_timeout_handle != 0) {
        scheduler_.cancel(planning_timeout_handle);
      }

      // Check for stop/reset after planning (spec 230 §6.2)
      if (planning_timed_out || stop_policy_.stop_requested()) {
        auto prev = worker_entry_state;  // B-2: use captured state
        const auto message = planning_timed_out
                                 ? "Planning timed out"
                                 : "Planning interrupted by stop";
        sm_.set_last_error(ErrorCode::kPlanningTimeout);
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorPlanningTimeout, message, {}}, {}});
        sm_.try_transition(ExecutionState::kStopped);
        auto stopped_snapshot = get_execution_snapshot();
        stopped_snapshot.timestamp_ns = clock_.now();
        stopped_snapshot.execution_id = exec_id;
        stopped_snapshot.move_id = request.move_id;
        stopped_snapshot.state = ExecutionState::kStopped;
        stopped_snapshot.error = ErrorDTO{kErrorPlanningTimeout, message, {}};
        publish_execution_snapshot(std::move(stopped_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kStopped, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      // Check if reset was requested (state would be Idle)
      if (sm_.status().state == ExecutionState::kIdle) {
        auto prev = worker_entry_state;  // B-2: use captured state
        sm_.set_last_error(ErrorCode::kPlanningTimeout);
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by reset", {}}, {}});
        auto idle_snapshot = get_execution_snapshot();
        idle_snapshot.timestamp_ns = clock_.now();
        idle_snapshot.execution_id = 0;
        idle_snapshot.move_id.clear();
        idle_snapshot.state = ExecutionState::kIdle;
        idle_snapshot.progress = 0.0;
        idle_snapshot.error = ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by reset", {}};
        publish_execution_snapshot(std::move(idle_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kIdle, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      if (!plan_result.ok()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, plan_result.error(), {}});
        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.plans_failed++;
        }
        sm_.set_last_error(error_code_from_dto(plan_result.error()));
        sm_.try_transition(ExecutionState::kFailed);
        auto failed_snapshot = get_execution_snapshot();
        failed_snapshot.timestamp_ns = clock_.now();
        failed_snapshot.execution_id = exec_id;
        failed_snapshot.move_id = request.move_id;
        failed_snapshot.state = ExecutionState::kFailed;
        failed_snapshot.error = plan_result.error();
        publish_execution_snapshot(std::move(failed_snapshot));
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     ExecutionState::kPlanning, ExecutionState::kFailed, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      trajectory = std::move(plan_result.value());

      emit_event({KernelEventType::kPlanCompleted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.plans_succeeded++;
      }

      // Planning -> Validating
      auto prev_state2 = sm_.status().state;
      auto r2 = sm_.try_transition(ExecutionState::kValidating);
      if (!r2.ok()) {
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev_state2, ExecutionState::kFailed, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state2, ExecutionState::kValidating, std::nullopt, {}});
      auto validating_snapshot = get_execution_snapshot();
      validating_snapshot.timestamp_ns = clock_.now();
      validating_snapshot.execution_id = exec_id;
      validating_snapshot.move_id = request.move_id;
      validating_snapshot.state = ExecutionState::kValidating;
      validating_snapshot.progress = 0.0;
      publish_execution_snapshot(std::move(validating_snapshot));
    }

    emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});

    auto val_result = validate_trajectory(trajectory, request);
    if (!val_result.ok()) {
      // Emit validation failed event BEFORE error raised
      emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                   {}, {}, std::nullopt, {{"reason", "validation failed"}}});
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, val_result.error(), {}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.validations_failed++;
      }
      sm_.set_last_error(ErrorCode::kValidationCollision);
      sm_.try_transition(ExecutionState::kFailed);
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kFailed;
      snapshot.error = val_result.error();
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   ExecutionState::kValidating, ExecutionState::kFailed, std::nullopt, {}});
      if (request.cache_mode != CacheMode::kDisable) {
        cache_.invalidate(request.move_id);
        // Emit cache invalidated event
        emit_event({KernelEventType::kCacheInvalidated, exec_id, clock_.now(),
                     {}, {}, std::nullopt,
                     {{"move_id", request.move_id},
                      {"reason", "validation_failed"}}});
        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.cache_invalidations++;
        }
      }
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    emit_event({KernelEventType::kValidationPassed, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.validations_passed++;
    }

    // Check for stop/reset after validation (spec 230 §6.2)
    if (stop_policy_.stop_requested()) {
      auto prev = sm_.status().state;
      // Stop -> go to Stopped with kInternalError
      sm_.set_last_error(ErrorCode::kInternalError);
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}}, {}});
      sm_.try_transition(ExecutionState::kStopped);
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kStopped;
      snapshot.error = ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}};
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev, ExecutionState::kStopped, std::nullopt, {}});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }
    // Check if reset was requested (state would be Idle)
    if (sm_.status().state == ExecutionState::kIdle) {
      auto prev = sm_.status().state;
      sm_.set_last_error(ErrorCode::kInternalError);
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, ErrorDTO{kErrorInternalError, "Validation interrupted by reset", {}}, {}});
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = 0;
      snapshot.move_id.clear();
      snapshot.state = ExecutionState::kIdle;
      snapshot.progress = 0.0;
      snapshot.error = ErrorDTO{kErrorInternalError, "Validation interrupted by reset", {}};
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev, ExecutionState::kIdle, std::nullopt, {}});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    // Store in cache
    if (request.cache_mode != CacheMode::kDisable && !cache_hit) {
      cache_.store(request.move_id, trajectory);
    }

    // Validating -> Executing
    auto prev_state3 = sm_.status().state;
    auto r3 = sm_.try_transition(ExecutionState::kExecuting);
    if (!r3.ok()) {
      sm_.set_last_error(ErrorCode::kInternalError);
      sm_.try_transition(ExecutionState::kFailed);
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state3, ExecutionState::kFailed, std::nullopt, {}});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state3, ExecutionState::kExecuting, std::nullopt, {}});

    emit_event({KernelEventType::kExecuteStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = ExecutionState::kExecuting;
    publish_execution_snapshot(std::move(snapshot));

    // Execution timeout enforcement (W-1)
    const auto exec_start_ns = clock_.now();
    const int64_t exec_timeout_ns = (request.timeouts.execution_timeout_s > 0)
        ? static_cast<int64_t>(request.timeouts.execution_timeout_s * 1e9) : 0;

    auto execution_handle = execution_backend_.execute_trajectory(trajectory);
    auto active_trajectory = trajectory;
    core::ExecutionSnapshot final_snapshot;
    core::ExecutionSnapshot latest_backend_snapshot;
    start_collision_monitoring();

    int64_t paused_accumulated_ns = 0;
    bool pause_in_progress = false;
    int64_t pause_started_ns = 0;

    while (true) {
      bool completed = true;
      if (execution_handle.wait_for_completion) {
        completed = execution_handle.wait_for_completion(kExecutionPollNs);
      }

      if (execution_handle.get_snapshot) {
        latest_backend_snapshot = execution_handle.get_snapshot();
        latest_backend_snapshot.execution_id = exec_id;
        latest_backend_snapshot.move_id = request.move_id;
        publish_execution_snapshot(latest_backend_snapshot);
        sm_.set_progress(latest_backend_snapshot.progress);
      }

      const auto now_ns = clock_.now();
      const bool paused =
          latest_backend_snapshot.state == ExecutionState::kPaused ||
          sm_.status().state == ExecutionState::kPaused;
      if (paused) {
        if (!pause_in_progress) {
          pause_in_progress = true;
          pause_started_ns = now_ns;
        }
      } else if (pause_in_progress) {
        paused_accumulated_ns += now_ns - pause_started_ns;
        pause_in_progress = false;
      }

      const int64_t current_pause_ns =
          pause_in_progress ? now_ns - pause_started_ns : 0;
      const int64_t effective_elapsed_ns =
          now_ns - exec_start_ns - paused_accumulated_ns - current_pause_ns;

      // Check execution timeout (W-1)
      if (exec_timeout_ns > 0 && effective_elapsed_ns >= exec_timeout_ns) {
        stop_collision_monitoring();
        (void)execution_backend_.stop();
        const bool stop_completed = drain_execution_handle_bounded(execution_handle);
        auto prev = sm_.status().state;
        sm_.set_last_error(stop_completed ? ErrorCode::kExecutionTimeout
                                           : ErrorCode::kStopFailed);
        const auto terminal_state = stop_completed ? ExecutionState::kFailed
                                                   : ExecutionState::kInterventionRequired;
        sm_.try_transition(terminal_state);

        auto to_snapshot = latest_backend_snapshot;
        to_snapshot.timestamp_ns = clock_.now();
        to_snapshot.execution_id = exec_id;
        to_snapshot.move_id = request.move_id;
        to_snapshot.state = terminal_state;
        ErrorDTO terminal_error = stop_completed
            ? ErrorDTO{kErrorExecutionTimeout, "Execution timeout exceeded", {}}
            : operator_intervention_error(
                  ErrorCode::kStopFailed,
                  "Execution timeout exceeded and software stop completion was not observed",
                  "execution_timeout_stop_drain",
                  prev,
                  exec_id,
                  request.move_id);
        to_snapshot.error = terminal_error;
        publish_execution_snapshot(std::move(to_snapshot));

        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, terminal_error, {}});
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, terminal_state, std::nullopt, {}});

        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.executions_failed++;
        }
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      if (latest_backend_snapshot.state == ExecutionState::kPaused ||
          sm_.status().state == ExecutionState::kPaused) {
        stop_collision_monitoring();

        if (resume_requested_.exchange(false)) {
          emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(),
                      {}, {}, std::nullopt, {}});

          auto resumed_trajectory =
              build_remaining_trajectory(active_trajectory, latest_backend_snapshot);
          auto resume_validation = validate_trajectory(resumed_trajectory, request);
          if (!resume_validation.ok()) {
            emit_event({KernelEventType::kValidationFailed, exec_id, clock_.now(),
                        {}, {}, std::nullopt,
                        {{"reason", "resume validation failed"}}});
            emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                        {}, {}, resume_validation.error(), {}});
            {
              std::lock_guard<std::mutex> lock(metrics_mutex_);
              metrics_.validations_failed++;
            }
            sm_.set_last_error(error_code_from_dto(resume_validation.error()));
            (void)execution_backend_.stop();
            const bool stop_completed = drain_execution_handle_bounded(execution_handle);
            auto prev_state = sm_.status().state;
            const auto terminal_state = stop_completed
                ? ExecutionState::kFailed
                : ExecutionState::kInterventionRequired;
            if (!stop_completed) {
              sm_.set_last_error(ErrorCode::kStopFailed);
            }
            sm_.try_transition(terminal_state);
            auto failed_snapshot = latest_backend_snapshot;
            failed_snapshot.timestamp_ns = clock_.now();
            failed_snapshot.execution_id = exec_id;
            failed_snapshot.move_id = request.move_id;
            failed_snapshot.state = terminal_state;
            failed_snapshot.error = stop_completed
                ? resume_validation.error()
                : operator_intervention_error(
                      ErrorCode::kStopFailed,
                      "Resume validation failed and software stop completion was not observed",
                      "resume_validation_stop_drain",
                      prev_state,
                      exec_id,
                      request.move_id);
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                        prev_state, terminal_state, std::nullopt, {}});
            execution_complete_.store(true);
            completion_cv_.notify_all();
            return;
          }

          emit_event({KernelEventType::kValidationPassed, exec_id, clock_.now(),
                      {}, {}, std::nullopt, {}});
          {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            metrics_.validations_passed++;
          }

          auto prev_state = sm_.status().state;
          auto resume_transition = sm_.try_transition(ExecutionState::kExecuting);
          if (!resume_transition.ok()) {
            sm_.set_last_error(ErrorCode::kResumeFailed);
            emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                        {}, {},
                        ErrorDTO{kErrorResumeFailed,
                                 "Resume transition to Executing failed", {}},
                        {}});
            sm_.try_transition(ExecutionState::kFailed);
            auto failed_snapshot = latest_backend_snapshot;
            failed_snapshot.timestamp_ns = clock_.now();
            failed_snapshot.execution_id = exec_id;
            failed_snapshot.move_id = request.move_id;
            failed_snapshot.state = ExecutionState::kFailed;
            failed_snapshot.error = ErrorDTO{kErrorResumeFailed,
                                             "Resume transition to Executing failed", {}};
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                        prev_state, ExecutionState::kFailed, std::nullopt, {}});
            execution_complete_.store(true);
            completion_cv_.notify_all();
            return;
          }

          emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                      prev_state, ExecutionState::kExecuting, std::nullopt, {}});
          emit_event({KernelEventType::kExecuteStarted, exec_id, clock_.now(),
                      {}, {}, std::nullopt, {}});

          auto resumed_snapshot = latest_backend_snapshot;
          resumed_snapshot.timestamp_ns = clock_.now();
          resumed_snapshot.execution_id = exec_id;
          resumed_snapshot.move_id = request.move_id;
          resumed_snapshot.state = ExecutionState::kExecuting;
          resumed_snapshot.error.reset();
          publish_execution_snapshot(std::move(resumed_snapshot));

          execution_handle = execution_backend_.resume(resumed_trajectory);
          active_trajectory = std::move(resumed_trajectory);
          start_collision_monitoring();
        }

        continue;
      }

      if (completed) {
        break;
      }
    }

    stop_collision_monitoring();

    if (execution_handle.get_snapshot) {
      final_snapshot = execution_handle.get_snapshot();
      final_snapshot.execution_id = exec_id;
      final_snapshot.move_id = request.move_id;
      publish_execution_snapshot(final_snapshot);
      sm_.set_progress(final_snapshot.progress);
    } else {
      final_snapshot.execution_id = exec_id;
      final_snapshot.move_id = request.move_id;
      final_snapshot.state = ExecutionState::kCompleted;
      final_snapshot.progress = 1.0;
      publish_execution_snapshot(final_snapshot);
      sm_.set_progress(1.0);
    }

    if (final_snapshot.state == ExecutionState::kStopped ||
        sm_.status().state == ExecutionState::kStopped ||
        sm_.status().state == ExecutionState::kIdle) {
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    if (final_snapshot.state == ExecutionState::kFailed) {
      if (final_snapshot.error.has_value()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                    {}, {}, final_snapshot.error, {}});
        sm_.set_last_error(error_code_from_dto(*final_snapshot.error));
      } else {
        sm_.set_last_error(ErrorCode::kInternalError);
      }
      auto prev_state4 = sm_.status().state;
      sm_.try_transition(ExecutionState::kFailed);
      auto snapshot = final_snapshot;
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kFailed;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                  prev_state4, ExecutionState::kFailed, std::nullopt, {}});
      {
        std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
        metrics_.executions_failed++;
      }
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    if (final_snapshot.state == ExecutionState::kInterventionRequired) {
      if (final_snapshot.error.has_value()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                    {}, {}, final_snapshot.error, {}});
        sm_.set_last_error(error_code_from_dto(*final_snapshot.error));
      } else {
        sm_.set_last_error(ErrorCode::kStopFailed);
      }
      auto prev_state4 = sm_.status().state;
      sm_.try_transition(ExecutionState::kInterventionRequired);
      auto snapshot = final_snapshot;
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kInterventionRequired;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                  prev_state4, ExecutionState::kInterventionRequired,
                  std::nullopt, {}});
      {
        std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
        metrics_.executions_failed++;
      }
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    emit_event({KernelEventType::kExecuteCompleted, exec_id, clock_.now(),
                {}, {}, std::nullopt, {}});

    auto prev_state4 = sm_.status().state;
    sm_.try_transition(ExecutionState::kCompleted);
    auto snapshot2 = final_snapshot;
    snapshot2.timestamp_ns = clock_.now();
    snapshot2.execution_id = exec_id;
    snapshot2.move_id = request.move_id;
    snapshot2.state = ExecutionState::kCompleted;
    snapshot2.progress = 1.0;
    publish_execution_snapshot(std::move(snapshot2));
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                prev_state4, ExecutionState::kCompleted, std::nullopt, {}});
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_completed++;
    }

  } catch (const std::exception& e) {
    stop_collision_monitoring();
    sm_.set_last_error(ErrorCode::kInternalError);
    auto prev_state = sm_.status().state;
    sm_.try_transition(ExecutionState::kFailed);
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = ExecutionState::kFailed;
    snapshot.error = ErrorDTO{kErrorInternalError, e.what(), {}};
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state, ExecutionState::kFailed, std::nullopt, {}});
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
  }

  execution_complete_.store(true);
  completion_cv_.notify_all();
}

Result<uint64_t> ExecutionKernel::execute(const ExecuteRequestDTO& request) {
  std::lock_guard<std::mutex> lock(execute_mutex_);

  if (sm_.status().state == ExecutionState::kInterventionRequired) {
    const auto prev_state = sm_.status().state;
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return Result<uint64_t>::Err(backend_reset.error.value_or(
          ErrorDTO{kErrorResetFailed,
                   "Execution backend could not resync from intervention_required",
                   {{"requires_operator_intervention", "true"},
                    {"resync_required", "true"},
                    {"phase", "execute_resync"}}}));
    }
    auto reset_result = stop_policy_.request_reset();
    if (!reset_result.ok()) {
      return Result<uint64_t>::Err(reset_result.error());
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id,
                clock_.now(), prev_state, ExecutionState::kIdle, std::nullopt, {}});
  }

  if (sm_.status().state == ExecutionState::kFailed ||
      sm_.status().state == ExecutionState::kStopped) {
    const auto prev_state = sm_.status().state;
    auto reset_result = stop_policy_.request_reset();
    if (!reset_result.ok()) {
      return Result<uint64_t>::Err(reset_result.error());
    }
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id,
                clock_.now(), prev_state, ExecutionState::kIdle, std::nullopt, {}});
  }

  if (sm_.status().state != ExecutionState::kIdle) {
    return Result<uint64_t>::Err(ErrorCode::kExecutionRejected);
  }

  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    return Result<uint64_t>::Err(sync_result.error());
  }

  ExecuteRequestDTO resolved_request(request);
  auto resolved_motion = resolve_motion_spec(request);
  if (!resolved_motion.ok()) {
    if (request.cache_mode != CacheMode::kDisable &&
        cache_.lookup(request.move_id).has_value()) {
      cache_.invalidate(request.move_id);
      emit_event({KernelEventType::kCacheInvalidated,
                  0,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"move_id", request.move_id},
                   {"reason", "goal_unverifiable"}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_invalidations++;
    }
    return Result<uint64_t>::Err(resolved_motion.error());
  }
  resolved_request.motion = std::move(resolved_motion.value());

  std::optional<RobotStateDTO> synced_state;
  try {
    synced_state = context_.get_current_state();
  } catch (...) {
    synced_state.reset();
  }

  PreparedExecutionStart prepared;
  if (request.cache_mode != CacheMode::kDisable) {
    auto cached = cache_.lookup(request.move_id);
    if (cached.has_value()) {
      const auto goal_match =
          cached_trajectory_matches_request_goal(context_, *cached, resolved_request);
      const auto start_match =
          synced_state.has_value()
              ? cached_trajectory_matches_current_start(
                    *cached, *synced_state, request.tolerances.joint_position_tol_rad)
              : incompatible_cache("start_state_changed");
      if (goal_match.compatible && start_match.compatible) {
        prepared.cache_hit = true;
        prepared.trajectory = std::move(*cached);
      } else {
        cache_.invalidate(request.move_id);
        prepared.cache_invalidated = true;
        prepared.cache_invalidation_reason =
            !start_match.compatible ? start_match.reason : goal_match.reason;
      }
    } else {
      prepared.cache_miss = true;
    }
  }

  uint64_t exec_id = next_execution_id_++;
  auto prev_state = sm_.status().state;
  sm_.set_execution_id(exec_id);
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_ = request.move_id;
  }
  const auto initial_state =
      prepared.cache_hit ? ExecutionState::kValidating
                         : ExecutionState::kPlanning;
  auto transition = sm_.try_transition(initial_state);
  if (!transition.ok()) {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_.clear();
    sm_.set_execution_id(0);
    return Result<uint64_t>::Err(ErrorCode::kInternalError);
  }

  execution_complete_.store(false);
  resume_requested_.store(false);
  stop_collision_monitoring();

  emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
              prev_state, initial_state, std::nullopt, {}});

  if (prepared.cache_invalidated) {
    emit_event({KernelEventType::kCacheInvalidated,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"move_id", request.move_id},
                 {"reason", prepared.cache_invalidation_reason}}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.cache_invalidations++;
  }

  if (prepared.cache_hit) {
    emit_event(
        {KernelEventType::kCacheHit, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.cache_hits++;
  } else {
    if (prepared.cache_miss) {
      emit_event({KernelEventType::kCacheMiss, exec_id, clock_.now(),
                  {}, {}, std::nullopt, {{"move_id", request.move_id}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_misses++;
    }
    emit_event({KernelEventType::kPlanStarted, exec_id, clock_.now(),
                {}, {}, std::nullopt, {}});
  }

  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = exec_id;
  snapshot.move_id = request.move_id;
  snapshot.state = initial_state;
  snapshot.progress = 0.0;
  try {
    snapshot.joint_positions_rad = context_.get_current_state().positions_rad;
  } catch (...) {
    // Leave joints empty if the planning context cannot provide state here.
  }
  publish_execution_snapshot(std::move(snapshot));

  worker_thread_ = std::jthread(
      [this, request_copy = ExecuteRequestDTO(resolved_request),
       exec_id,
       cached_trajectory = prepared.cache_hit
                               ? std::make_optional(std::move(prepared.trajectory))
                               : std::nullopt](std::stop_token st) mutable {
        async_execute_worker(st, std::move(request_copy), exec_id,
                             std::move(cached_trajectory));
      });

  return Result<uint64_t>::Ok(exec_id);
}

StopResultDTO ExecutionKernel::stop() {
  // ADR-0016: stop() from kStreaming maps to a soft-end of the active
  // streaming session using the session's configured soft-stop strategy
  // (spring_back by default; velocity_scaling when qualification permits).
  // Distinct from the hard cancel path used by reset().
  uint64_t streaming_session_to_end = 0;
  SoftStopStrategy streaming_strategy = SoftStopStrategy::kSpringBack;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (sm_.status().state == ExecutionState::kStreaming &&
        active_streaming_session_id_ != 0) {
      streaming_session_to_end = active_streaming_session_id_;
      streaming_strategy = active_soft_stop_strategy_;
    }
  }
  if (streaming_session_to_end != 0) {
    auto end_result = end_streaming_session(streaming_session_to_end,
                                              streaming_strategy);
    if (end_result.accepted) {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.stops_issued++;
    }
    return StopResultDTO{end_result.accepted, end_result.error};
  }

  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_hard_stop();
  if (result.ok()) {
    stop_collision_monitoring();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    // Emit kStopIssued event
    emit_event({KernelEventType::kStopIssued, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt, {}});
    // Emit state transition event
    auto new_state = sm_.status().state;
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, new_state, std::nullopt, {}});
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.stops_issued++;
    return StopResultDTO{true, std::nullopt};
  }
  return StopResultDTO{false, result.error()};
}

PauseResultDTO ExecutionKernel::pause() {
  auto result = stop_policy_.request_pause();
  if (result.ok()) {
    stop_collision_monitoring();
    (void)execution_backend_.pause();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    // Emit kPauseRequested event
    emit_event({KernelEventType::kPauseRequested, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt, {}});
    return PauseResultDTO{true, std::nullopt};
  }
  return PauseResultDTO{false, result.error()};
}

ResumeResultDTO ExecutionKernel::resume() {
  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_resume();
  if (result.ok()) {
    resume_requested_.store(true);
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kValidating;
    snapshot.error.reset();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    // Emit kResumeRequested event
    emit_event({KernelEventType::kResumeRequested, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt, {}});
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, sm_.status().state, std::nullopt, {}});
    return ResumeResultDTO{true, std::nullopt};
  }
  return ResumeResultDTO{false, result.error()};
}

ResetResultDTO ExecutionKernel::reset() {
  auto prev_state = sm_.status().state;

  // ADR-0016: reset() is the HARD-cancel path (max-decel cancel via
  // backend.reset(), no soft-stop trajectory). Clear streaming session
  // bookkeeping unconditionally when set — covers the kStreaming case
  // and the recovery-from-InterventionRequired case (an error during
  // streaming might leave the session id non-zero even after the state
  // machine transitioned away from kStreaming). Without this clear, a
  // subsequent begin_streaming_session would be rejected with
  // "session already active".
  uint64_t closed_streaming_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ != 0) {
      closed_streaming_id = active_streaming_session_id_;
      active_streaming_session_id_ = 0;
      active_backend_session_id_ = 0;
      active_streaming_config_ = StreamingSessionConfigDTO{};
      active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
      last_accepted_cartesian_pos_valid_ = false;
      last_accepted_cartesian_ts_ns_ = 0;
      last_commanded_joints_valid_ = false;
      last_commanded_joints_.clear();
    }
  }
  if (closed_streaming_id != 0) {
    emit_event({KernelEventType::kStreamingSessionEnded,
                0,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"session_id", std::to_string(closed_streaming_id)},
                 {"end_reason", "reset_hard_cancel"}}});
  }

  if (prev_state == ExecutionState::kIdle) {
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return backend_reset;
    }
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    return ResetResultDTO{true, std::nullopt};
  }
  stop_collision_monitoring();
  resume_requested_.store(false);

  auto backend_reset = execution_backend_.reset();
  if (!backend_reset.accepted) {
    auto error = backend_reset.error.value_or(
        operator_intervention_error(ErrorCode::kResetFailed,
                                    "Execution backend reset failed",
                                    "backend_reset",
                                    prev_state,
                                    sm_.status().execution_id,
                                    ""));
    error = ensure_operator_intervention_details(std::move(error),
                                                 "backend_reset",
                                                 prev_state,
                                                 sm_.status().execution_id,
                                                 "");
    sm_.set_last_error(error_code_from_dto(error));
    sm_.try_transition(ExecutionState::kInterventionRequired);

    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kInterventionRequired;
    snapshot.error = error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised, sm_.status().execution_id,
                clock_.now(), {}, {}, error, {}});
    if (prev_state != ExecutionState::kInterventionRequired) {
      emit_event({KernelEventType::kStateTransition, sm_.status().execution_id,
                  clock_.now(), prev_state, ExecutionState::kInterventionRequired,
                  std::nullopt, {}});
    }
    return ResetResultDTO{false, error};
  }

  auto result = stop_policy_.request_reset();
  if (result.ok()) {
    {
      std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
      active_move_id_.clear();
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    // Emit state transition event to Idle
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, ExecutionState::kIdle, std::nullopt, {}});
    return ResetResultDTO{true, std::nullopt};
  }
  return ResetResultDTO{false, result.error()};
}

AcknowledgeCompletedResultDTO ExecutionKernel::acknowledge_completed(
    uint64_t execution_id) {
  std::lock_guard<std::mutex> lock(execute_mutex_);

  const auto status = sm_.status();
  if (status.state != ExecutionState::kCompleted) {
    return AcknowledgeCompletedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Can only acknowledge a Completed execution",
                 {{"state", to_string(status.state)}}}};
  }
  if (status.execution_id != execution_id) {
    return AcknowledgeCompletedResultDTO{
        false,
        ErrorDTO{kErrorInvalidRequest,
                 "Execution acknowledgement id mismatch",
                 {{"current_execution_id", std::to_string(status.execution_id)},
                  {"requested_execution_id", std::to_string(execution_id)}}}};
  }

  auto result = sm_.reset();
  if (!result.ok()) {
    return AcknowledgeCompletedResultDTO{false, result.error()};
  }
  sm_.set_execution_id(0);
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    active_move_id_.clear();
  }

  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = 0;
  snapshot.state = ExecutionState::kIdle;
  snapshot.progress = 0.0;
  publish_execution_snapshot(std::move(snapshot));

  emit_event({KernelEventType::kCompletedAcknowledged,
              execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"acknowledged_execution_id", std::to_string(execution_id)}}});
  emit_event({KernelEventType::kStateTransition,
              execution_id,
              clock_.now(),
              ExecutionState::kCompleted,
              ExecutionState::kIdle,
              std::nullopt,
              {}});

  return AcknowledgeCompletedResultDTO{true, std::nullopt};
}

ExecutionStatus ExecutionKernel::get_status() const {
  auto status = sm_.status();
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    status.active_move_id = active_move_id_;
  }
  return status;
}

ExecutionSnapshot ExecutionKernel::get_execution_snapshot() const {
  const auto kernel_state = sm_.status().state;
  if (kernel_state == ExecutionState::kStreaming ||
      kernel_state == ExecutionState::kStopping) {
    auto backend_snapshot = execution_backend_.get_execution_snapshot();
    backend_snapshot.state = kernel_state;
    if (backend_snapshot.timestamp_ns == 0) {
      backend_snapshot.timestamp_ns = clock_.now();
    }
    if (backend_snapshot.timestamp_ns > 0) {
      const auto now_ns = clock_.now();
      if (now_ns >= backend_snapshot.timestamp_ns) {
        const auto lag_ns =
            static_cast<uint64_t>(now_ns - backend_snapshot.timestamp_ns);
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.observer_lag_max_ns =
            std::max(metrics_.observer_lag_max_ns, lag_ns);
      }
    }
    return backend_snapshot;
  }

  ExecutionSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot = published_snapshot_;
  }
  if (snapshot.timestamp_ns > 0) {
    const auto now_ns = clock_.now();
    if (now_ns >= snapshot.timestamp_ns) {
      const auto lag_ns =
          static_cast<uint64_t>(now_ns - snapshot.timestamp_ns);
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.observer_lag_max_ns =
          std::max(metrics_.observer_lag_max_ns, lag_ns);
    }
  }
  return snapshot;
}

void ExecutionKernel::sync_snapshot_from_context() {
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = sm_.status().execution_id;
  {
    std::lock_guard<std::mutex> move_lock(active_move_id_mutex_);
    snapshot.move_id = active_move_id_;
  }
  snapshot.state = sm_.status().state;
  snapshot.progress = sm_.status().progress;
  try {
    snapshot.joint_positions_rad = context_.get_current_state().positions_rad;
  } catch (...) {
    snapshot.joint_positions_rad.clear();
  }
  publish_execution_snapshot(std::move(snapshot));
}

MetricsSnapshot ExecutionKernel::get_metrics() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  return metrics_;
}

Result<void> ExecutionKernel::collision_check_tick() {
  auto prev_state = sm_.status().state;
  if (prev_state != ExecutionState::kExecuting) {
    return Result<void>::Ok();
  }

  try {
    auto state = context_.get_current_state();
    CollisionCheckRequestDTO req;
    req.robot_state = state;
    auto check = context_.collision_check(req);

    if (check.in_collision) {
      // Emit collision detected event
      KernelEvent collision_event;
      collision_event.type = KernelEventType::kCollisionDetected;
      collision_event.execution_id = sm_.status().execution_id;
      collision_event.timestamp_ns = clock_.now();
      collision_event.details["contact_a"] = check.contact_body_a.value_or("");
      collision_event.details["contact_b"] = check.contact_body_b.value_or("");
      emit_event(collision_event);
      {
        std::lock_guard<std::mutex> collision_metrics_lock(metrics_mutex_);
        metrics_.collision_detections++;
      }
    }

    auto result = collision_policy_.handle_collision(check);
    auto new_state = sm_.status().state;
    if (new_state != prev_state) {
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.state = new_state;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                  prev_state, new_state, std::nullopt, {}});
      if (new_state == ExecutionState::kStopped) {
        stop_collision_monitoring();
      }
    }
    return result;
  } catch (const std::exception& e) {
    return Result<void>::Err(
        ErrorDTO{kErrorInternalError,
                 std::string("Collision check exception: ") + e.what(), {}});
  }
}

Result<TrajectoryDTO> ExecutionKernel::plan_trajectory(
    const ExecuteRequestDTO& request) {
  auto current_state = context_.get_current_state();
  auto resolved_motion = resolve_motion_spec(request);
  if (!resolved_motion.ok()) {
    return Result<TrajectoryDTO>::Err(resolved_motion.error());
  }

  PlanRequestDTO plan_req;
  plan_req.robot_id = request.robot_id;
  plan_req.group_id = request.group_id;
  plan_req.move_id = request.move_id;
  plan_req.base_frame = request.base_frame;
  plan_req.tcp_frame = request.tcp_frame;
  plan_req.start_state = current_state;
  plan_req.motion = std::move(resolved_motion.value());
  plan_req.tolerances = request.tolerances;
  plan_req.speed_profile = request.speed_profile;
  plan_req.planning_timeout_s = request.timeouts.planning_timeout_s;

  auto result = planner_.plan(plan_req);

  if (result.trajectory.has_value()) {
    return Result<TrajectoryDTO>::Ok(std::move(*result.trajectory));
  }

  if (result.error.has_value()) {
    return Result<TrajectoryDTO>::Err(std::move(*result.error));
  }

  return Result<TrajectoryDTO>::Err(ErrorCode::kPlanningFailed);
}

Result<MotionSpecDTO> ExecutionKernel::resolve_motion_spec(
    const ExecuteRequestDTO& request) {
  if (auto* relative_move = std::get_if<RelativeMoveDTO>(&request.motion)) {
    auto target_pose =
        context_.resolve_reference_pose(relative_move->target_reference, request.base_frame);
    if (!target_pose.ok()) {
      return Result<MotionSpecDTO>::Err(target_pose.error());
    }

    Pose3DTO offset_basis_pose = target_pose.value();
    if (relative_move->offset_reference.has_value()) {
      auto offset_reference_pose = context_.resolve_reference_pose(
          *relative_move->offset_reference, request.base_frame);
      if (!offset_reference_pose.ok()) {
        return Result<MotionSpecDTO>::Err(offset_reference_pose.error());
      }
      offset_basis_pose = offset_reference_pose.value();
    }

    PoseGoalDTO pose_goal;
    pose_goal.goal = multiply_poses(
        pose_with_position_and_orientation(
            target_pose.value().position_m,
            offset_basis_pose.orientation),
        relative_move->offset);
    return Result<MotionSpecDTO>::Ok(MotionSpecDTO{pose_goal});
  }

  return Result<MotionSpecDTO>::Ok(request.motion);
}

Result<void> ExecutionKernel::validate_trajectory(
    const TrajectoryDTO& trajectory, const ExecuteRequestDTO& request) {
  // Start-state gate
  auto current_state = context_.get_current_state();
  if (!trajectory.waypoints.empty()) {
    const auto& start_wp = trajectory.waypoints.front();
    if (current_state.positions_rad.size() == start_wp.positions_rad.size()) {
      for (size_t i = 0; i < current_state.positions_rad.size(); ++i) {
        double diff = current_state.positions_rad[i] - start_wp.positions_rad[i];
        if (diff < 0) diff = -diff;
        if (diff > request.tolerances.joint_position_tol_rad) {
          return Result<void>::Err(
              ErrorDTO{kErrorStartStateMismatch, "Start state mismatch", {}});
        }
      }
    }
  }

  // Full trajectory validation (including TCP speed limit per spec 230 §3.3)
  ValidationOptionsDTO opts;
  opts.tcp_frame = request.tcp_frame;
  opts.base_frame = request.base_frame;
  opts.max_tcp_linear_speed_mps =
      request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
  auto val_result = context_.validate_trajectory(trajectory, opts);
  if (!val_result.valid) {
    if (val_result.error.has_value()) {
      return Result<void>::Err(std::move(*val_result.error));
    }
    return Result<void>::Err(ErrorCode::kValidationCollision);
  }

  // Pre-execution collision check
  CollisionCheckRequestDTO collision_req;
  collision_req.robot_state = current_state;
  auto collision_result = context_.collision_check(collision_req);
  return collision_policy_.pre_execute_check(collision_result);
}

// -----------------------------------------------------------------
// Streaming-session API (ADR-0016).
//
// Lifecycle invariants enforced here:
//   - begin requires state == kIdle; transitions to kStreaming on accept.
//   - push requires state == kStreaming and session_id match.
//   - end transitions kStreaming -> kStopping -> kStopped (after the
//     backend soft-stop completes).
//
// Concurrency: streaming_session_mutex_ protects the active session
// metadata. We do NOT hold it across the backend call (the backend has
// its own internal locks; we mustn't invert lock order with
// execute_mutex_).
// -----------------------------------------------------------------

StreamingSessionBeginResultDTO ExecutionKernel::begin_streaming_session(
    const StreamingSessionConfigDTO& config) {
  StreamingSessionBeginResultDTO result;

  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ != 0) {
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidStateTransition)),
          "streaming session already active",
          {{"active_session_id",
            std::to_string(active_streaming_session_id_)}}};
      return result;
    }
  }

  const auto prev_state = sm_.status().state;
  if (prev_state != ExecutionState::kIdle) {
    result.accepted = false;
    result.error = ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "begin_streaming_session valid only from Idle",
        {{"state", to_string(prev_state)}}};
    return result;
  }

  // Defense-in-depth: if the caller did not specify the session's
  // joint list, fall back to the backend's current joint snapshot.
  // The frontend SHOULD pass `joint_names` explicitly (the canonical
  // path), but if it ever forgets, the push-validation chain would
  // reject every partial frame as `unknown_joint_name`. Populating
  // from the live snapshot here keeps the session functional with
  // the natural ordering the backend already uses.
  StreamingSessionConfigDTO populated_config = config;
  if (populated_config.joint_names.empty()) {
    auto current_state = context_.get_current_state();
    if (!current_state.joint_names.empty()) {
      populated_config.joint_names = current_state.joint_names;
    }
  }

  auto backend_result =
      execution_backend_.begin_streaming_session(populated_config);
  if (!backend_result.accepted) {
    return backend_result;
  }

  auto sm_result = sm_.try_transition(ExecutionState::kStreaming);
  if (!sm_result.ok()) {
    // Roll back the backend session — kernel state machine refused the
    // transition. End the session with spring-back as a defensive default.
    (void)execution_backend_.end_streaming_session(
        backend_result.session_id, SoftStopStrategy::kSpringBack);
    result.accepted = false;
    result.error = sm_result.error();
    return result;
  }

  uint64_t kernel_session_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    kernel_session_id = next_streaming_session_id_++;
    active_streaming_session_id_ = kernel_session_id;
    active_backend_session_id_ = backend_result.session_id;
    active_streaming_config_ = populated_config;
    active_soft_stop_strategy_ = populated_config.default_soft_stop_strategy;
    last_accepted_cartesian_pos_valid_ = false;
    last_accepted_cartesian_ts_ns_ = 0;
    last_commanded_joints_valid_ = false;
    last_commanded_joints_.clear();
  }

  emit_event({KernelEventType::kStreamingSessionStarted,
              0,  // no execution_id for streaming
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"session_id", std::to_string(kernel_session_id)},
               {"backend_session_id",
                std::to_string(backend_result.session_id)},
               {"group_id", config.group_id},
               {"input_mode", to_string(config.default_input_mode)},
               {"principal_id", config.principal_id}}});
  emit_event({KernelEventType::kStateTransition,
              0,
              clock_.now(),
              prev_state,
              ExecutionState::kStreaming,
              std::nullopt,
              {{"session_id", std::to_string(kernel_session_id)}}});

  result.accepted = true;
  result.session_id = kernel_session_id;
  return result;
}

StreamingPushResultDTO ExecutionKernel::push_streaming_target(
    uint64_t session_id,
    const StreamingTargetDTO& target) {
  StreamingPushResultDTO result;

  if (sm_.status().state != ExecutionState::kStreaming) {
    result.accepted = false;
    result.error = ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "push_streaming_target valid only from Streaming",
        {{"state", to_string(sm_.status().state)}}};
    return result;
  }

  StreamingSessionConfigDTO active_config;
  uint64_t backend_session_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ == 0 ||
        active_streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {{"requested_session_id", std::to_string(session_id)},
           {"active_session_id",
            std::to_string(active_streaming_session_id_)}}};
      return result;
    }
    active_config = active_streaming_config_;
    backend_session_id = active_backend_session_id_;
  }

  // ADR-0015 / ADR-0016: enforce the deployment's Cartesian speed cap
  // on the ROBOT's commanded motion, NOT on the operator's input rate.
  // The operator can drag the gizmo at any speed; the robot tracks at
  // ≤ the cap and the gizmo simply gets ahead during fast drags (the
  // visual non-coincidence is documented UX — same as the legacy
  // direct-jog path). We translate the cap into a minimum
  // `time_from_start` for the backend's anchored FJT goal:
  //   dt_min_cart = distance(prev_target, this_target) / max_speed_mps
  // and the backend's dispatch worker max'es this with its own joint-
  // velocity-cap-derived floor. The result is that the FJT goal is
  // always scheduled to take long enough for the cap to hold.
  StreamingTargetDTO resolved_target = target;
  if (target.kind == StreamingInputMode::kCartesian &&
      active_config.max_tcp_linear_speed_mps > 0.0) {
    std::array<double, 3> last_pos{0.0, 0.0, 0.0};
    bool have_last = false;
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      last_pos = last_accepted_cartesian_pos_;
      have_last = last_accepted_cartesian_pos_valid_;
    }
    if (have_last) {
      const double dx = target.position_m[0] - last_pos[0];
      const double dy = target.position_m[1] - last_pos[1];
      const double dz = target.position_m[2] - last_pos[2];
      const double distance_m = std::sqrt(dx * dx + dy * dy + dz * dz);
      const double dt_min_cart_s =
          distance_m / active_config.max_tcp_linear_speed_mps;
      if (dt_min_cart_s > resolved_target.min_time_from_start_s) {
        resolved_target.min_time_from_start_s = dt_min_cart_s;
      }
    }
  }

  // ADR-0016 IK pre-resolution: Cartesian targets are resolved to joint
  // targets here via the planner's single-shot IK, then forwarded to the
  // backend as joint targets. This keeps execution backends free of IK
  // logic (per ADR-0007 scene-ownership boundary) and lets every backend
  // implement streaming uniformly against joint targets.
  if (target.kind == StreamingInputMode::kCartesian) {
    auto current_state = context_.get_current_state();
    IkRequestDTO ik_req;
    Pose3DTO pose;
    pose.position_m.x_m = target.position_m[0];
    pose.position_m.y_m = target.position_m[1];
    pose.position_m.z_m = target.position_m[2];
    pose.orientation.x = target.orientation_xyzw[0];
    pose.orientation.y = target.orientation_xyzw[1];
    pose.orientation.z = target.orientation_xyzw[2];
    pose.orientation.w = target.orientation_xyzw[3];
    ik_req.goal_pose = pose;
    ik_req.base_frame = active_config.cartesian_reference_frame.empty()
                             ? std::string("base")
                             : active_config.cartesian_reference_frame;
    ik_req.tcp_frame = active_config.tcp_frame;
    if (ik_req.tcp_frame.empty()) {
      // No TCP frame on the session — Cartesian streaming is not addressable.
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "Cartesian streaming target on a session that did not configure "
          "tcp_frame — set StreamingSessionConfigDTO.tcp_frame at begin time",
          {}};
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "missing_tcp_frame"}}});
      return result;
    }
    ik_req.seed_state = current_state;

    auto ik_result = planner_.solve_ik(ik_req);
    if (!ik_result.ok) {
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ik_result.error.value_or(ErrorDTO{
          std::string(to_string(ErrorCode::kIkFailed)),
          "IK failed for streaming cartesian target",
          {}});
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "cartesian_ik_failed"}}});
      return result;
    }

    // Reorder IK output into session joint order. IK returns whatever
    // order the planner uses; the rest of the kernel + backends rely
    // on session-order vectors (so cartesian-then-joint sequences
    // overlay correctly onto last_commanded_joints_).
    std::vector<double> ordered_positions(active_config.joint_names.size());
    for (size_t i = 0; i < active_config.joint_names.size(); ++i) {
      const auto& sname = active_config.joint_names[i];
      auto it = std::find(ik_result.joint_names.begin(),
                          ik_result.joint_names.end(), sname);
      if (it == ik_result.joint_names.end()) {
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kIkFailed)),
            "IK result missing session joint: " + sname,
            {{"reason_code", "ik_failed"}, {"joint_name", sname}}};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "cartesian_ik_failed"}}});
        return result;
      }
      auto idx = static_cast<size_t>(
          std::distance(ik_result.joint_names.begin(), it));
      ordered_positions[i] = ik_result.positions_rad[idx];
    }
    resolved_target.kind = StreamingInputMode::kJoint;
    resolved_target.joint_names = active_config.joint_names;
    resolved_target.joint_positions_rad = ordered_positions;
  } else if (target.kind == StreamingInputMode::kJoint) {
    // Joint-mode normalization per spec/200 §2. Partial joint maps are
    // valid input; the kernel overlays them onto the session's last
    // commanded full vector before backend dispatch. Validation chain
    // runs BEFORE any state mutation so rejected frames cannot move
    // last_commanded_joints_.
    if (target.joint_names.size() != target.joint_positions_rad.size()) {
      result.accepted = false;
      result.backend_sequence = 0;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "joint_names.size() != joint_positions_rad.size()",
          {{"reason_code", "malformed_frame"},
           {"names_size", std::to_string(target.joint_names.size())},
           {"positions_size",
            std::to_string(target.joint_positions_rad.size())}}};
      emit_event({KernelEventType::kStreamingTargetRejected,
                  0,
                  clock_.now(),
                  {},
                  {},
                  result.error,
                  {{"session_id", std::to_string(session_id)},
                   {"source_sequence",
                    std::to_string(target.source_sequence)},
                   {"kind", "malformed_frame"}}});
      return result;
    }

    // Duplicate-name check. Allowed-name check fused in (any partial
    // entry whose name isn't in the session's joint_names is also
    // implicitly a duplicate-or-unknown). We separate them so the
    // operator sees the correct reason code.
    std::map<std::string, size_t> seen;
    for (size_t pi = 0; pi < target.joint_names.size(); ++pi) {
      const auto& name = target.joint_names[pi];
      auto inserted = seen.emplace(name, pi);
      if (!inserted.second) {
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kInvalidRequest)),
            "duplicate joint name in streaming target: " + name,
            {{"reason_code", "malformed_frame"}, {"joint_name", name}}};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "malformed_frame"}}});
        return result;
      }
    }

    // Unknown-name check.
    for (const auto& name : target.joint_names) {
      if (std::find(active_config.joint_names.begin(),
                    active_config.joint_names.end(), name)
          == active_config.joint_names.end()) {
        result.accepted = false;
        result.backend_sequence = 0;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kUnknownJointName)),
            "joint name not in session: " + name,
            {{"joint_name", name}}};
        emit_event({KernelEventType::kStreamingTargetRejected,
                    0,
                    clock_.now(),
                    {},
                    {},
                    result.error,
                    {{"session_id", std::to_string(session_id)},
                     {"source_sequence",
                      std::to_string(target.source_sequence)},
                     {"kind", "unknown_joint_name"}}});
        return result;
      }
    }

    // Build the full session-order vector. Reuse last_commanded_joints_
    // when valid; otherwise seed from the backend's current snapshot.
    // A snapshot that doesn't cover every session joint is treated as
    // stale and rejects the push without seeding.
    std::vector<double> commanded_full;
    bool need_seed = false;
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      if (last_commanded_joints_valid_
          && last_commanded_joints_.size() == active_config.joint_names.size()) {
        commanded_full = last_commanded_joints_;
      } else {
        need_seed = true;
      }
    }
    if (need_seed) {
      auto current_state = context_.get_current_state();
      commanded_full.assign(active_config.joint_names.size(), 0.0);
      for (size_t i = 0; i < active_config.joint_names.size(); ++i) {
        const auto& sname = active_config.joint_names[i];
        auto cit = std::find(current_state.joint_names.begin(),
                             current_state.joint_names.end(), sname);
        if (cit == current_state.joint_names.end()) {
          result.accepted = false;
          result.backend_sequence = 0;
          result.error = ErrorDTO{
              std::string(to_string(ErrorCode::kInvalidRequest)),
              "current robot state does not cover session joint: " + sname,
              {{"reason_code", "stale_state"}, {"joint_name", sname}}};
          emit_event({KernelEventType::kStreamingTargetRejected,
                      0,
                      clock_.now(),
                      {},
                      {},
                      result.error,
                      {{"session_id", std::to_string(session_id)},
                       {"source_sequence",
                        std::to_string(target.source_sequence)},
                       {"kind", "stale_state"}}});
          return result;
        }
        auto cidx = static_cast<size_t>(
            std::distance(current_state.joint_names.begin(), cit));
        commanded_full[i] = current_state.positions_rad[cidx];
      }
    }

    // Overlay partial onto full. Names already validated as present in
    // active_config.joint_names; the lookup is guaranteed to succeed.
    for (size_t pi = 0; pi < target.joint_names.size(); ++pi) {
      auto sit = std::find(active_config.joint_names.begin(),
                           active_config.joint_names.end(),
                           target.joint_names[pi]);
      auto sidx = static_cast<size_t>(
          std::distance(active_config.joint_names.begin(), sit));
      commanded_full[sidx] = target.joint_positions_rad[pi];
    }

    resolved_target.kind = StreamingInputMode::kJoint;
    resolved_target.joint_names = active_config.joint_names;
    resolved_target.joint_positions_rad = commanded_full;
  }

  // Forward to backend using the BACKEND's session_id (assigned at begin
  // time), not the kernel's. Kernel and backend counters are independent;
  // mapping must use the backend's view.
  auto backend_result =
      execution_backend_.push_streaming_target(backend_session_id,
                                                resolved_target);
  result = backend_result;

  // On accept: record the cartesian position as the new
  // last-accepted-frame anchor for speed-limit enforcement on the next
  // push. Track the kernel clock at acceptance time, not the
  // source_timestamp_ns — the speed limit is on what the kernel
  // actually commanded, not what the upstream client claimed.
  if (result.accepted && target.kind == StreamingInputMode::kCartesian) {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    last_accepted_cartesian_pos_ = target.position_m;
    last_accepted_cartesian_pos_valid_ = true;
    last_accepted_cartesian_ts_ns_ = clock_.now();
  }

  // Update last_commanded_joints_ on backend ACCEPT (target register
  // updated) for both joint and IK-resolved-cartesian targets, so a
  // cartesian drag followed by a partial joint slider overlays onto
  // the post-cartesian joint configuration rather than a stale
  // pre-cartesian one. Spec/200 §2: last-commanded state moves on
  // accept, not on theoretical controller dispatch.
  if (result.accepted
      && resolved_target.kind == StreamingInputMode::kJoint
      && !resolved_target.joint_positions_rad.empty()
      && resolved_target.joint_positions_rad.size()
             == active_config.joint_names.size()) {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    last_commanded_joints_ = resolved_target.joint_positions_rad;
    last_commanded_joints_valid_ = true;
  }

  // Per-target events are coalescible / sampled; spec 200 §4 explicitly
  // allows them to not be 1:1 with calls. Emit on accept and on reject so
  // the failure archive captures rejections.
  if (result.accepted) {
    emit_event({KernelEventType::kStreamingTargetReceived,
                0,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"session_id", std::to_string(session_id)},
                 {"backend_sequence",
                  std::to_string(result.backend_sequence)},
                 {"source_sequence",
                  std::to_string(target.source_sequence)},
                 {"kind", to_string(target.kind)}}});
    // For the v0.1 backends the push call IS the dispatch (the backend's
    // dispatcher worker reads the register at the next tick). Emit
    // kStreamingTargetDispatched so the failure archive / event stream
    // can correlate operator frame → controller goal submission. Future
    // backends with deeper queueing should emit this separately when the
    // goal actually reaches the controller.
    emit_event({KernelEventType::kStreamingTargetDispatched,
                0,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"session_id", std::to_string(session_id)},
                 {"backend_sequence",
                  std::to_string(result.backend_sequence)},
                 {"source_sequence",
                  std::to_string(target.source_sequence)},
                 {"resolved_kind",
                  to_string(resolved_target.kind)}}});
  } else {
    emit_event({KernelEventType::kStreamingTargetRejected,
                0,
                clock_.now(),
                {},
                {},
                result.error,
                {{"session_id", std::to_string(session_id)},
                 {"source_sequence",
                  std::to_string(target.source_sequence)},
                 {"kind", to_string(target.kind)}}});
  }
  return result;
}

StreamingSessionEndResultDTO ExecutionKernel::end_streaming_session(
    uint64_t session_id,
    SoftStopStrategy strategy) {
  StreamingSessionEndResultDTO result;

  const auto prev_state = sm_.status().state;
  if (prev_state != ExecutionState::kStreaming) {
    result.accepted = false;
    result.error = ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "end_streaming_session valid only from Streaming",
        {{"state", to_string(prev_state)}}};
    return result;
  }

  uint64_t backend_session_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    if (active_streaming_session_id_ == 0 ||
        active_streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {{"requested_session_id", std::to_string(session_id)},
           {"active_session_id",
            std::to_string(active_streaming_session_id_)}}};
      return result;
    }
    backend_session_id = active_backend_session_id_;
  }

  // Move through Stopping during the backend soft-stop. The backend's
  // soft-stop trajectory still has to play out; we report Stopping until
  // the backend acks, then transition to Stopped.
  auto stopping_result = sm_.try_transition(ExecutionState::kStopping);
  if (!stopping_result.ok()) {
    result.accepted = false;
    result.error = stopping_result.error();
    return result;
  }
  emit_event({KernelEventType::kStateTransition,
              0,
              clock_.now(),
              prev_state,
              ExecutionState::kStopping,
              std::nullopt,
              {{"session_id", std::to_string(session_id)},
               {"soft_stop_strategy", to_string(strategy)}}});

  auto backend_result =
      execution_backend_.end_streaming_session(backend_session_id, strategy);
  if (!backend_result.accepted) {
    // The soft-stop itself failed; route to InterventionRequired since
    // the backend may still be commanding motion in an unknown state.
    (void)sm_.try_transition(ExecutionState::kInterventionRequired);
    emit_event({KernelEventType::kErrorRaised,
                0,
                clock_.now(),
                {},
                {},
                backend_result.error,
                {{"session_id", std::to_string(session_id)}}});
    // Still clear local bookkeeping — the kernel can't recover this
    // session anyway, and a future begin must be permitted once the
    // operator hits reset.
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      active_streaming_session_id_ = 0;
      active_backend_session_id_ = 0;
      active_streaming_config_ = StreamingSessionConfigDTO{};
      active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
      last_accepted_cartesian_pos_valid_ = false;
      last_accepted_cartesian_ts_ns_ = 0;
      last_commanded_joints_valid_ = false;
      last_commanded_joints_.clear();
    }
    return backend_result;
  }

  auto final_backend_snapshot = execution_backend_.get_execution_snapshot();

  auto stopped_result = sm_.try_transition(ExecutionState::kStopped);
  if (!stopped_result.ok()) {
    // State machine refused the Stopped transition — surface as
    // InterventionRequired rather than papering over.
    (void)sm_.try_transition(ExecutionState::kInterventionRequired);
    result.accepted = false;
    result.error = stopped_result.error();
    {
      std::lock_guard<std::mutex> lock(streaming_session_mutex_);
      active_streaming_session_id_ = 0;
      active_backend_session_id_ = 0;
      active_streaming_config_ = StreamingSessionConfigDTO{};
      active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
      last_accepted_cartesian_pos_valid_ = false;
      last_accepted_cartesian_ts_ns_ = 0;
      last_commanded_joints_valid_ = false;
      last_commanded_joints_.clear();
    }
    return result;
  }

  uint64_t closed_kernel_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_session_mutex_);
    closed_kernel_id = active_streaming_session_id_;
    active_streaming_session_id_ = 0;
    active_backend_session_id_ = 0;
    active_streaming_config_ = StreamingSessionConfigDTO{};
    active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
    last_accepted_cartesian_pos_valid_ = false;
    last_accepted_cartesian_ts_ns_ = 0;
    last_commanded_joints_valid_ = false;
    last_commanded_joints_.clear();
  }

  emit_event({KernelEventType::kStreamingSessionEnded,
              0,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"session_id", std::to_string(closed_kernel_id)},
               {"soft_stop_strategy", to_string(strategy)}}});
  emit_event({KernelEventType::kStateTransition,
              0,
              clock_.now(),
              ExecutionState::kStopping,
              ExecutionState::kStopped,
              std::nullopt,
              {{"session_id", std::to_string(closed_kernel_id)}}});

  // ADR-0016: streaming-session end is operator-acknowledged at release
  // time (mouseUp ≡ "I'm done"). Auto-return to kIdle so the next
  // begin_streaming_session is admitted without requiring an explicit
  // reset click. Distinct from discrete trajectories, which require
  // acknowledge_completed.
  auto idle_result = sm_.try_transition(ExecutionState::kIdle);
  if (idle_result.ok()) {
    emit_event({KernelEventType::kStateTransition,
                0,
                clock_.now(),
                ExecutionState::kStopped,
                ExecutionState::kIdle,
                std::nullopt,
                {{"session_id", std::to_string(closed_kernel_id)},
                 {"reason", "streaming_session_acknowledged"}}});
  }

  final_backend_snapshot.state = sm_.status().state;
  final_backend_snapshot.progress = 0.0;
  final_backend_snapshot.execution_id = 0;
  final_backend_snapshot.move_id.clear();
  final_backend_snapshot.timestamp_ns = clock_.now();
  publish_execution_snapshot(std::move(final_backend_snapshot));

  result.accepted = true;
  return result;
}

}  // namespace manyforge::core
