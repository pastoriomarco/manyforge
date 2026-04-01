#pragma once

#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/collision/collision_policy.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/execution_result.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace manyforge::core {

/// Execution Kernel — single authority for motion execution governance.
/// Ties together: state machine, stop/collision policies, trajectory cache,
/// planner backend, planning context, and execution backend.
///
/// Spec 230: all operations are ROS-free. This is a synchronous v0.0
/// implementation; async dispatch is deferred to the ROS wrapper layer.
class ExecutionKernel {
 public:
  ExecutionKernel(IPlannerBackend& planner,
                  IPlanningContext& context,
                  IExecutionBackend& execution_backend,
                  IClock& clock,
                  IScheduler& scheduler,
                  TrajectoryCache& cache);

  /// Execute a motion request. Only valid from Idle.
  /// Synchronously runs: plan → validate → dispatch.
  /// Returns execution_id on success.
  Result<uint64_t> execute(const ExecuteRequestDTO& request);

  /// Non-blocking stop from any non-terminal state.
  StopResultDTO stop();

  /// Non-blocking pause. Only valid from Executing.
  PauseResultDTO pause();

  /// Non-blocking resume. Only valid from Paused.
  ResumeResultDTO resume();

  /// Non-blocking reset. Valid from any state except Idle.
  ResetResultDTO reset();

  /// Get current kernel status.
  ExecutionStatus get_status() const;

  /// Run a single collision monitoring tick.
  /// Called externally (e.g., by a scheduler) during Executing.
  Result<void> collision_check_tick();

 private:
  Result<TrajectoryDTO> plan_trajectory(const ExecuteRequestDTO& request);
  Result<void> validate_trajectory(const TrajectoryDTO& trajectory,
                                   const ExecuteRequestDTO& request);

  IPlannerBackend& planner_;
  IPlanningContext& context_;
  IExecutionBackend& execution_backend_;
  IClock& clock_;
  IScheduler& scheduler_;
  TrajectoryCache& cache_;

  ExecutionStateMachine sm_;
  StopPolicy stop_policy_;
  CollisionPolicy collision_policy_;

  std::mutex execute_mutex_;
  std::atomic<uint64_t> next_execution_id_{1};
  std::string active_move_id_;
};

}  // namespace manyforge::core
