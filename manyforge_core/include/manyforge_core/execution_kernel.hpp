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
#include <manyforge_core/types/kernel_event.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <vector>

namespace manyforge::core {

/// Execution Kernel — single authority for motion execution governance.
/// Ties together: state machine, stop/collision policies, trajectory cache,
/// planner backend, planning context, and execution backend.
///
/// Spec 230: all operations are ROS-free. This is an async v0.1
/// implementation with event subscription.
class ExecutionKernel {
 public:
  ExecutionKernel(IPlannerBackend& planner,
                  IPlanningContext& context,
                  IExecutionBackend& execution_backend,
                  IClock& clock,
                  IScheduler& scheduler,
                  TrajectoryCache& cache);

  ~ExecutionKernel();

  /// Execute a motion request. Only valid from Idle.
  /// Returns execution_id immediately; plan → validate → dispatch runs
  /// on a worker thread. Callers observe progress via get_status() and events.
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

  /// Subscribe to kernel events.
  /// @param callback Called for each event. Must be thread-safe.
  /// @return Handle for unsubscribing.
  SubscriptionHandle subscribe_events(EventCallback callback);

  /// Unsubscribe from kernel events.
  void unsubscribe_events(SubscriptionHandle handle);

  /// Block until the current execution completes (reaches terminal state).
  /// Useful for tests and synchronous callers.
  /// @param timeout_ns Maximum time to wait (0 = wait forever).
  /// @return true if execution completed, false if timed out.
  bool wait_for_completion(int64_t timeout_ns = 0);

 private:
  Result<TrajectoryDTO> plan_trajectory(const ExecuteRequestDTO& request);
  Result<void> validate_trajectory(const TrajectoryDTO& trajectory,
                                   const ExecuteRequestDTO& request);

  /// Async execution worker function.
  void async_execute_worker(const ExecuteRequestDTO& request, uint64_t exec_id);

  /// Emit an event to all subscribers.
  void emit_event(const KernelEvent& event);

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

  // Event subscription
  std::mutex subscribers_mutex_;
  std::map<uint64_t, EventCallback> subscribers_;
  std::atomic<uint64_t> next_subscription_id_{1};

  // Async execution state
  std::thread worker_thread_;
  std::condition_variable completion_cv_;
  std::mutex completion_mutex_;
  std::atomic<bool> execution_complete_{false};
};

}  // namespace manyforge::core
