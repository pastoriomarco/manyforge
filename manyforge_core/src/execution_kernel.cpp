#include <manyforge_core/execution_kernel.hpp>

#include <exception>
#include <string>

namespace manyforge::core {

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
                        [this]() { cache_.invalidate_all(); }) {}

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

void ExecutionKernel::async_execute_worker(const ExecuteRequestDTO& request,
                                            uint64_t exec_id) {
  try {
    TrajectoryDTO trajectory;
    bool cache_hit = false;

    if (request.cache_mode != CacheMode::kDisable) {
      auto cached = cache_.lookup(request.move_id);
      if (cached.has_value()) {
        cache_hit = true;
        trajectory = std::move(*cached);
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
        }
      }
    }

    if (cache_hit) {
      // Idle -> Validating directly
      auto prev_state = sm_.status().state;
      auto r = sm_.try_transition(ExecutionState::kValidating);
      if (!r.ok()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorInternalError, "State transition failed", {}}});
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev_state, ExecutionState::kFailed, std::nullopt});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state, ExecutionState::kValidating, std::nullopt});
      emit_event({KernelEventType::kCacheHit, exec_id, clock_.now(), {}, {}, std::nullopt});
    } else {
      // Idle -> Planning
      auto prev_state = sm_.status().state;
      auto r = sm_.try_transition(ExecutionState::kPlanning);
      if (!r.ok()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorInternalError, "State transition failed", {}}});
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev_state, ExecutionState::kFailed, std::nullopt});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state, ExecutionState::kPlanning, std::nullopt});
      emit_event({KernelEventType::kPlanStarted, exec_id, clock_.now(), {}, {}, std::nullopt});

      auto plan_result = plan_trajectory(request);
      if (!plan_result.ok()) {
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, plan_result.error()});
        sm_.set_last_error(ErrorCode::kPlanningFailed);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     ExecutionState::kPlanning, ExecutionState::kFailed, std::nullopt});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      trajectory = std::move(plan_result.value());

      emit_event({KernelEventType::kPlanCompleted, exec_id, clock_.now(), {}, {}, std::nullopt});

      // Check for stop/reset after planning (spec 230 §6.2)
      if (stop_policy_.stop_requested()) {
        auto prev = sm_.status().state;
        // Stop -> go to Stopped with kPlanningTimeout
        sm_.set_last_error(ErrorCode::kPlanningTimeout);
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by stop", {}}});
        sm_.try_transition(ExecutionState::kStopped);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kStopped, std::nullopt});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      // Check if reset was requested (state would be Idle)
      if (sm_.status().state == ExecutionState::kIdle) {
        auto prev = sm_.status().state;
        sm_.set_last_error(ErrorCode::kPlanningTimeout);
        emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                     {}, {}, ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by reset", {}}});
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev, ExecutionState::kIdle, std::nullopt});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }

      // Planning -> Validating
      auto prev_state2 = sm_.status().state;
      auto r2 = sm_.try_transition(ExecutionState::kValidating);
      if (!r2.ok()) {
        sm_.set_last_error(ErrorCode::kInternalError);
        sm_.try_transition(ExecutionState::kFailed);
        emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                     prev_state2, ExecutionState::kFailed, std::nullopt});
        execution_complete_.store(true);
        completion_cv_.notify_all();
        return;
      }
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev_state2, ExecutionState::kValidating, std::nullopt});
    }

    emit_event({KernelEventType::kValidationStarted, exec_id, clock_.now(), {}, {}, std::nullopt});

    auto val_result = validate_trajectory(trajectory, request);
    if (!val_result.ok()) {
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, val_result.error()});
      sm_.set_last_error(ErrorCode::kValidationCollision);
      sm_.try_transition(ExecutionState::kFailed);
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   ExecutionState::kValidating, ExecutionState::kFailed, std::nullopt});
      if (request.cache_mode != CacheMode::kDisable) {
        cache_.invalidate(request.move_id);
      }
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }

    emit_event({KernelEventType::kValidationPassed, exec_id, clock_.now(), {}, {}, std::nullopt});

    // Check for stop/reset after validation (spec 230 §6.2)
    if (stop_policy_.stop_requested()) {
      auto prev = sm_.status().state;
      // Stop -> go to Stopped with kInternalError
      sm_.set_last_error(ErrorCode::kInternalError);
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}}});
      sm_.try_transition(ExecutionState::kStopped);
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev, ExecutionState::kStopped, std::nullopt});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }
    // Check if reset was requested (state would be Idle)
    if (sm_.status().state == ExecutionState::kIdle) {
      auto prev = sm_.status().state;
      sm_.set_last_error(ErrorCode::kInternalError);
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(),
                   {}, {}, ErrorDTO{kErrorInternalError, "Validation interrupted by reset", {}}});
      emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                   prev, ExecutionState::kIdle, std::nullopt});
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
                   prev_state3, ExecutionState::kFailed, std::nullopt});
      execution_complete_.store(true);
      completion_cv_.notify_all();
      return;
    }
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state3, ExecutionState::kExecuting, std::nullopt});

    emit_event({KernelEventType::kExecuteStarted, exec_id, clock_.now(), {}, {}, std::nullopt});

    execution_backend_.execute_trajectory(trajectory);

    emit_event({KernelEventType::kExecuteCompleted, exec_id, clock_.now(), {}, {}, std::nullopt});

    sm_.set_progress(1.0);
    auto prev_state4 = sm_.status().state;
    sm_.try_transition(ExecutionState::kCompleted);
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state4, ExecutionState::kCompleted, std::nullopt});

  } catch (const std::exception& e) {
    sm_.set_last_error(ErrorCode::kInternalError);
    auto prev_state = sm_.status().state;
    sm_.try_transition(ExecutionState::kFailed);
    emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(),
                 prev_state, ExecutionState::kFailed, std::nullopt});
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

  worker_thread_ = std::thread(&ExecutionKernel::async_execute_worker, this,
                               request, exec_id);

  return Result<uint64_t>::Ok(exec_id);
}

StopResultDTO ExecutionKernel::stop() {
  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_hard_stop();
  if (result.ok()) {
    // Emit kStopIssued event
    emit_event({KernelEventType::kStopIssued, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt});
    // Emit state transition event
    auto new_state = sm_.status().state;
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, new_state, std::nullopt});
    return StopResultDTO{true, std::nullopt};
  }
  return StopResultDTO{false, result.error()};
}

PauseResultDTO ExecutionKernel::pause() {
  auto result = stop_policy_.request_pause();
  if (result.ok()) {
    // Emit kPauseRequested event
    emit_event({KernelEventType::kPauseRequested, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt});
    return PauseResultDTO{true, std::nullopt};
  }
  return PauseResultDTO{false, result.error()};
}

ResumeResultDTO ExecutionKernel::resume() {
  auto result = stop_policy_.request_resume();
  if (result.ok()) {
    // Emit kResumeRequested event
    emit_event({KernelEventType::kResumeRequested, sm_.status().execution_id, clock_.now(),
                {}, {}, std::nullopt});
    return ResumeResultDTO{true, std::nullopt};
  }
  return ResumeResultDTO{false, result.error()};
}

ResetResultDTO ExecutionKernel::reset() {
  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_reset();
  if (result.ok()) {
    active_move_id_.clear();
    // Emit state transition event to Idle
    emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                prev_state, ExecutionState::kIdle, std::nullopt});
    return ResetResultDTO{true, std::nullopt};
  }
  return ResetResultDTO{false, result.error()};
}

ExecutionStatus ExecutionKernel::get_status() const {
  auto status = sm_.status();
  status.active_move_id = active_move_id_;
  return status;
}

Result<void> ExecutionKernel::collision_check_tick() {
  if (sm_.status().state != ExecutionState::kExecuting) {
    return Result<void>::Ok();
  }

  try {
    auto state = context_.get_current_state();
    CollisionCheckRequestDTO req;
    req.robot_state = state;
    auto check = context_.collision_check(req);

    return collision_policy_.handle_collision(check);
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