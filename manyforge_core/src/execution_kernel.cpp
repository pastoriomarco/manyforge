#include <manyforge_core/execution_kernel.hpp>

#include <algorithm>
#include <exception>
#include <string>

namespace manyforge::core {
namespace {

double clamp_unit_interval(double value) {
  return std::max(0.0, std::min(1.0, value));
}

ErrorCode error_code_from_dto(const ErrorDTO& error) {
  if (error.code == kErrorStartStateMismatch) {
    return ErrorCode::kStartStateMismatch;
  }
  if (error.code == kErrorValidationLimitsViolation) {
    return ErrorCode::kValidationLimitsViolation;
  }
  if (error.code == kErrorValidationCollision) {
    return ErrorCode::kValidationCollision;
  }
  if (error.code == kErrorPauseFailed) {
    return ErrorCode::kPauseFailed;
  }
  if (error.code == kErrorResumeFailed) {
    return ErrorCode::kResumeFailed;
  }
  if (error.code == kErrorInvalidRequest) {
    return ErrorCode::kInvalidRequest;
  }
  return ErrorCode::kInternalError;
}

}  // namespace

ExecutionKernel::ExecutionKernel(IPlannerBackend& planner,
                                 IPlanningContext& context,
                                 IExecutionBackend& execution_backend,
                                 IClock& clock,
                                 IScheduler& scheduler,
                                 TrajectoryCache& cache)
    : planner_(planner),
      context_(context),
      execution_backend_(execution_backend),
      clock_(clock),
      scheduler_(scheduler),
      cache_(cache),
      sm_(clock),
      stop_policy_(sm_, scheduler, clock, [this]() { cache_.invalidate_all(); }),
      collision_policy_(stop_policy_,
                        [this]() { cache_.invalidate_all(); }) {
  stop_policy_.set_stop_callback([this]() { (void)execution_backend_.stop(); });
  published_snapshot_.timestamp_ns = clock_.now();
  published_snapshot_.state = ExecutionState::kIdle;
}

ExecutionKernel::~ExecutionKernel() {
  if (worker_thread_.joinable()) {
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
      clock_.now() + kCollisionMonitorPeriodNs, [this, generation]() {
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

void ExecutionKernel::async_execute_worker(const ExecuteRequestDTO& request,
                                            uint64_t exec_id) {
  try {
    // Capture initial state for correct prev_state in events (B-2)
    const auto worker_entry_state = sm_.status().state;

    TrajectoryDTO trajectory;
    bool cache_hit = false;

    if (request.cache_mode != CacheMode::kDisable) {
      auto cached = cache_.lookup(request.move_id);
      if (cached.has_value()) {
        cache_hit = true;
        trajectory = std::move(*cached);
      } else {
        // Cache miss - emit event and increment counter
        emit_event({KernelEventType::kCacheMiss, exec_id, clock_.now(),
                     {}, {}, std::nullopt, {{"move_id", request.move_id}}});
        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.cache_misses++;
        }
      }
    }

    if (cache_hit) {
      // Goal-compatibility gate
      if (!trajectory.waypoints.empty()) {
        const auto& end_wp = trajectory.waypoints.back();
        bool goal_compat = true;

        if (auto* jg = std::get_if<JointGoalDTO>(&request.motion)) {
          if (end_wp.positions_rad.size() == jg->positions_rad.size()) {
            for (size_t i = 0; i < jg->positions_rad.size(); ++i) {
              double diff = end_wp.positions_rad[i] - jg->positions_rad[i];
              if (diff < 0) diff = -diff;
              if (diff > request.tolerances.joint_position_tol_rad) {
                goal_compat = false;
                break;
              }
            }
          } else {
            goal_compat = false;
          }
        }

        if (!goal_compat) {
          cache_.invalidate(request.move_id);
          cache_hit = false;
          // Emit cache invalidated event
          emit_event({KernelEventType::kCacheInvalidated, exec_id, clock_.now(),
                       {}, {}, std::nullopt, {{"move_id", request.move_id}}});
          {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            metrics_.cache_invalidations++;
          }
        }
      }
    }

    if (cache_hit) {
      // Idle -> Validating directly
      auto prev_state = sm_.status().state;
      auto r = sm_.try_transition(ExecutionState::kValidating);
      if (!r.ok()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorInternalError, "State transition failed", {}}, {}});
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev_state, ExecutionState::kFailed, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state, ExecutionState::kValidating, std::nullopt, {}});
      emit_event({KernelEventType::kCacheHit, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kValidating;
      snapshot.progress = 0.0;
      publish_execution_snapshot(std::move(snapshot));
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.cache_hits++;
      }
    } else {
      // Idle -> Planning
      auto prev_state = sm_.status().state;
      auto r = sm_.try_transition(ExecutionState::kPlanning);
      if (!r.ok()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorInternalError, "State transition failed", {}}, {}});
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev_state, ExecutionState::kFailed, std::nullopt, {}});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state, ExecutionState::kPlanning, std::nullopt, {}});
      emit_event({KernelEventType::kPlanStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
      auto planning_snapshot = get_execution_snapshot();
      planning_snapshot.timestamp_ns = clock_.now();
      planning_snapshot.execution_id = exec_id;
      planning_snapshot.move_id = request.move_id;
      planning_snapshot.state = ExecutionState::kPlanning;
      planning_snapshot.progress = 0.0;
      publish_execution_snapshot(std::move(planning_snapshot));

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
        sm_.set_last_error(ErrorCode::kPlanningFailed);
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
                     {}, {}, std::nullopt, {{"move_id", request.move_id}}});
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

    auto execution_handle = execution_backend_.execute_trajectory(trajectory);
    auto active_trajectory = trajectory;
    core::ExecutionSnapshot final_snapshot;
    core::ExecutionSnapshot latest_backend_snapshot;
    constexpr int64_t kExecutionPollNs = 10'000'000;  // 10 ms
    start_collision_monitoring();

    // Execution timeout enforcement (W-1)
    const auto exec_start_ns = clock_.now();
    const int64_t exec_timeout_ns = (request.timeouts.execution_timeout_s > 0)
        ? static_cast<int64_t>(request.timeouts.execution_timeout_s * 1e9) : 0;

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

      // Check execution timeout (W-1)
      if (exec_timeout_ns > 0 && (clock_.now() - exec_start_ns) >= exec_timeout_ns) {
        stop_collision_monitoring();
        (void)execution_backend_.stop();
        // Drain any remaining execution
        if (execution_handle.wait_for_completion) {
          while (!execution_handle.wait_for_completion(kExecutionPollNs)) {}
        }
        auto prev = sm_.status().state;
        sm_.set_last_error(ErrorCode::kExecutionTimeout);
        sm_.try_transition(ExecutionState::kFailed);

        auto to_snapshot = latest_backend_snapshot;
        to_snapshot.timestamp_ns = clock_.now();
        to_snapshot.execution_id = exec_id;
        to_snapshot.move_id = request.move_id;
        to_snapshot.state = ExecutionState::kFailed;
        to_snapshot.error = ErrorDTO{kErrorExecutionTimeout, "Execution timeout exceeded", {}};
        publish_execution_snapshot(std::move(to_snapshot));

        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorExecutionTimeout, "Execution timeout exceeded", {}}, {}});
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kFailed, std::nullopt, {}});

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
            if (execution_handle.wait_for_completion) {
              while (!execution_handle.wait_for_completion(kExecutionPollNs)) {
              }
            }
            auto prev_state = sm_.status().state;
            sm_.try_transition(ExecutionState::kFailed);
            auto failed_snapshot = latest_backend_snapshot;
            failed_snapshot.timestamp_ns = clock_.now();
            failed_snapshot.execution_id = exec_id;
            failed_snapshot.move_id = request.move_id;
            failed_snapshot.state = ExecutionState::kFailed;
            failed_snapshot.error = resume_validation.error();
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                        prev_state, ExecutionState::kFailed, std::nullopt, {}});
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

  // Join previous worker if still joinable
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  // Auto-reset from terminal states
  auto current_state = sm_.status().state;
  if (current_state == ExecutionState::kCompleted ||
      current_state == ExecutionState::kFailed ||
      current_state == ExecutionState::kStopped) {
    stop_policy_.request_reset();
    active_move_id_.clear();
  }

  if (sm_.status().state != ExecutionState::kIdle) {
    return Result<uint64_t>::Err(ErrorCode::kExecutionRejected);
  }

  uint64_t exec_id = next_execution_id_++;
  sm_.set_execution_id(exec_id);
  active_move_id_ = request.move_id;

  execution_complete_.store(false);
  resume_requested_.store(false);
  stop_collision_monitoring();

  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = exec_id;
  snapshot.move_id = request.move_id;
  snapshot.state = sm_.status().state;
  snapshot.progress = 0.0;
  try {
    snapshot.joint_positions_rad = context_.get_current_state().positions_rad;
  } catch (...) {
    // Leave joints empty if the planning context cannot provide state here.
  }
  publish_execution_snapshot(std::move(snapshot));

  worker_thread_ = std::thread(&ExecutionKernel::async_execute_worker, this,
                               request, exec_id);

  return Result<uint64_t>::Ok(exec_id);
}

StopResultDTO ExecutionKernel::stop() {
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
  auto result = stop_policy_.request_reset();
  if (result.ok()) {
    stop_collision_monitoring();
    resume_requested_.store(false);
    (void)execution_backend_.reset();
    active_move_id_.clear();
    ExecutionSnapshot snapshot;
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    publish_execution_snapshot(std::move(snapshot));
    // Emit state transition event to Idle
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, ExecutionState::kIdle, std::nullopt, {}});
    return ResetResultDTO{true, std::nullopt};
  }
  return ResetResultDTO{false, result.error()};
}

ExecutionStatus ExecutionKernel::get_status() const {
  auto status = sm_.status();
  status.active_move_id = active_move_id_;
  return status;
}

ExecutionSnapshot ExecutionKernel::get_execution_snapshot() const {
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

  PlanRequestDTO plan_req;
  plan_req.robot_id = request.robot_id;
  plan_req.group_id = request.group_id;
  plan_req.move_id = request.move_id;
  plan_req.base_frame = request.base_frame;
  plan_req.tcp_frame = request.tcp_frame;
  plan_req.start_state = current_state;
  plan_req.motion = request.motion;
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

}  // namespace manyforge::core
