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
#include <manyforge_core/types/metrics.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {

struct ExecutionKernelConfig {
  Duration collision_monitor_period_ns = 10'000'000;  // 10 ms
};

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
                  TrajectoryCache& cache,
                  ExecutionKernelConfig config = {});

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

  /// Get the latest observer-safe execution snapshot published by the kernel.
  ExecutionSnapshot get_execution_snapshot() const;

  /// Refresh the observer-facing snapshot from the current planning-context
  /// state. Intended for explicit simulator/bootstrap paths, not motion
  /// execution semantics.
  void sync_snapshot_from_context();

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

  /// Get current metrics snapshot.
  MetricsSnapshot get_metrics() const;

 private:
  Result<MotionSpecDTO> resolve_motion_spec(const ExecuteRequestDTO& request);
  Result<TrajectoryDTO> plan_trajectory(const ExecuteRequestDTO& request);
  Result<void> validate_trajectory(const TrajectoryDTO& trajectory,
                                   const ExecuteRequestDTO& request);
  TrajectoryDTO build_remaining_trajectory(
      const TrajectoryDTO& trajectory,
      const ExecutionSnapshot& snapshot) const;
  std::vector<double> interpolate_joint_positions(
      const TrajectoryDTO& trajectory,
      double time_s) const;

  /// Async execution worker function.
  void async_execute_worker(std::stop_token st,
                            ExecuteRequestDTO request,
                            uint64_t exec_id,
                            std::optional<TrajectoryDTO> cached_trajectory);

  /// Emit an event to all subscribers.
  void emit_event(const KernelEvent& event);

  /// Publish an observer-safe execution snapshot.
  void publish_execution_snapshot(ExecutionSnapshot snapshot);
  void start_collision_monitoring();
  void stop_collision_monitoring();
  void schedule_next_collision_monitor_tick(uint64_t generation);

  IPlannerBackend& planner_;
  IPlanningContext& context_;
  IExecutionBackend& execution_backend_;
  IClock& clock_;
  IScheduler& scheduler_;
  TrajectoryCache& cache_;
  ExecutionKernelConfig config_;

  ExecutionStateMachine sm_;
  StopPolicy stop_policy_;
  CollisionPolicy collision_policy_;

  std::mutex execute_mutex_;
  std::atomic<uint64_t> next_execution_id_{1};
  mutable std::mutex active_move_id_mutex_;
  std::string active_move_id_;

  // Event subscription
  std::mutex subscribers_mutex_;
  std::map<uint64_t, EventCallback> subscribers_;
  std::atomic<uint64_t> next_subscription_id_{1};

  // Observer-facing execution snapshot
  mutable std::mutex snapshot_mutex_;
  ExecutionSnapshot published_snapshot_;
  uint64_t next_snapshot_sequence_ = 1;

  // Async execution state
  std::jthread worker_thread_;
  std::condition_variable completion_cv_;
  std::mutex completion_mutex_;
  std::atomic<bool> execution_complete_{false};
  std::atomic<bool> resume_requested_{false};

  mutable std::mutex collision_monitor_mutex_;
  ScheduleHandle collision_monitor_handle_{0};
  uint64_t collision_monitor_generation_ = 0;
  bool collision_monitor_active_ = false;

  // Metrics (atomic counters for thread safety)
  mutable std::mutex metrics_mutex_;
  mutable MetricsSnapshot metrics_;
};

}  // namespace manyforge::core
