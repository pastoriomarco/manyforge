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

Result<uint64_t> ExecutionKernel::execute(const ExecuteRequestDTO& request) {
  std::lock_guard<std::mutex> lock(execute_mutex_);

  // Auto-reset from terminal states so sequential moves work without
  // explicit reset() calls (e.g., behavior tree sequences).
  auto current_state = sm_.status().state;
  if (current_state == ExecutionState::kCompleted ||
      current_state == ExecutionState::kFailed ||
      current_state == ExecutionState::kStopped) {
    stop_policy_.request_reset();
    active_move_id_.clear();
  }

  // Only valid from Idle.
  if (sm_.status().state != ExecutionState::kIdle) {
    return Result<uint64_t>::Err(ErrorCode::kExecutionRejected);
  }

  uint64_t exec_id = next_execution_id_++;
  sm_.set_execution_id(exec_id);
  active_move_id_ = request.move_id;

  try {
    // Check trajectory cache for move_id.
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
      // Goal-compatibility gate: verify cached trajectory end state matches
      // the requested goal. On mismatch, invalidate and fall through to plan.
      if (!trajectory.waypoints.empty()) {
        const auto& end_wp = trajectory.waypoints.back();
        bool goal_compat = true;

        if (auto* jg = std::get_if<JointGoalDTO>(&request.motion)) {
          // Compare end waypoint positions against joint goal within tolerance.
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
        // PoseGoalDTO: cache keyed by move_id which encodes the goal;
        // full FK comparison deferred to async kernel (v0.x).

        if (!goal_compat) {
          cache_.invalidate(request.move_id);
          cache_hit = false;
        }
      }
    }

    if (cache_hit) {
      // Cache hit: Idle → Validating directly.
      auto r = sm_.try_transition(ExecutionState::kValidating);
      if (!r.ok()) return Result<uint64_t>::Err(r.error());
    } else {
      // Cache miss: Idle → Planning.
      auto r = sm_.try_transition(ExecutionState::kPlanning);
      if (!r.ok()) return Result<uint64_t>::Err(r.error());

      // Plan the trajectory.
      auto plan_result = plan_trajectory(request);
      if (!plan_result.ok()) {
        sm_.set_last_error(ErrorCode::kPlanningFailed);
        sm_.try_transition(ExecutionState::kFailed);
        return Result<uint64_t>::Err(plan_result.error());
      }
      trajectory = std::move(plan_result.value());

      // Check if stop was requested during planning.
      if (stop_policy_.stop_requested()) {
        return Result<uint64_t>::Err(ErrorCode::kExecutionAborted);
      }

      // Planning → Validating.
      auto r2 = sm_.try_transition(ExecutionState::kValidating);
      if (!r2.ok()) return Result<uint64_t>::Err(r2.error());
    }

    // Validate trajectory (start-state gate + full validation).
    auto val_result = validate_trajectory(trajectory, request);
    if (!val_result.ok()) {
      sm_.set_last_error(ErrorCode::kValidationCollision);
      sm_.try_transition(ExecutionState::kFailed);
      // Invalidate cache entry on validation failure.
      if (request.cache_mode != CacheMode::kDisable) {
        cache_.invalidate(request.move_id);
      }
      return Result<uint64_t>::Err(val_result.error());
    }

    // Store in cache (if not disabled and not already cached).
    if (request.cache_mode != CacheMode::kDisable && !cache_hit) {
      cache_.store(request.move_id, trajectory);
    }

    // Validating → Executing.
    auto r3 = sm_.try_transition(ExecutionState::kExecuting);
    if (!r3.ok()) return Result<uint64_t>::Err(r3.error());

    // Dispatch to execution backend.
    execution_backend_.execute_trajectory(trajectory);

    // Executing → Completed.
    sm_.set_progress(1.0);
    sm_.try_transition(ExecutionState::kCompleted);

    return Result<uint64_t>::Ok(exec_id);

  } catch (const std::exception& e) {
    // Port implementation threw (e.g. Python override via pybind11 trampoline).
    // Transition to Failed so the kernel can be reset and reused.
    sm_.set_last_error(ErrorCode::kInternalError);
    sm_.try_transition(ExecutionState::kFailed);
    return Result<uint64_t>::Err(
        ErrorDTO{kErrorInternalError,
                 std::string("Backend exception: ") + e.what(), {}});
  }
}

StopResultDTO ExecutionKernel::stop() {
  auto result = stop_policy_.request_hard_stop();
  if (result.ok()) {
    return StopResultDTO{true, std::nullopt};
  }
  return StopResultDTO{false, result.error()};
}

PauseResultDTO ExecutionKernel::pause() {
  auto result = stop_policy_.request_pause();
  if (result.ok()) {
    return PauseResultDTO{true, std::nullopt};
  }
  return PauseResultDTO{false, result.error()};
}

ResumeResultDTO ExecutionKernel::resume() {
  auto result = stop_policy_.request_resume();
  if (result.ok()) {
    return ResumeResultDTO{true, std::nullopt};
  }
  return ResumeResultDTO{false, result.error()};
}

ResetResultDTO ExecutionKernel::reset() {
  auto result = stop_policy_.request_reset();
  if (result.ok()) {
    active_move_id_.clear();
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
  // Start-state gate: compare current state to trajectory start.
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

  // Full trajectory validation.
  ValidationOptionsDTO opts;
  opts.tcp_frame = request.tcp_frame;
  opts.base_frame = request.base_frame;
  auto val_result = context_.validate_trajectory(trajectory, opts);
  if (!val_result.valid) {
    if (val_result.error.has_value()) {
      return Result<void>::Err(std::move(*val_result.error));
    }
    return Result<void>::Err(ErrorCode::kValidationCollision);
  }

  // Pre-execution collision check.
  CollisionCheckRequestDTO collision_req;
  collision_req.robot_state = current_state;
  auto collision_result = context_.collision_check(collision_req);
  return collision_policy_.pre_execute_check(collision_result);
}

}  // namespace manyforge::core
