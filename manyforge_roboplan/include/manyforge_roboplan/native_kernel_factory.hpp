#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <stop_token>

#include <roboplan/core/scene.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_toppra/toppra.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>
#include <manyforge_roboplan/roboplan_execution_backend.hpp>

namespace manyforge::roboplan_backend {

/// Wall clock using std::chrono::steady_clock.
class WallClock final : public core::IClock {
 public:
  core::TimePoint now() const override;
};

/// Production scheduler with role-split worker threads.
///
/// Each SchedulerPriority owns an independent single-thread worker. This keeps
/// controller-command progress, runtime collision ticks, stop-control deadlines,
/// lifecycle work, monitoring, and background callbacks from starving each
/// other while preserving deterministic per-role drain semantics.
///
/// Cancellation is serialised through a shared registry that records
/// the worker each handle belongs to.
class SimpleScheduler final : public core::IScheduler {
 public:
  SimpleScheduler();
  ~SimpleScheduler() override;

  core::ScheduleHandle schedule(
      core::TimePoint deadline, std::function<void()> callback,
      core::SchedulerPriority priority =
          core::SchedulerPriority::Lifecycle) override;
  void cancel(core::ScheduleHandle handle) override;
  void drain(core::SchedulerPriority priority) override;
  bool drain_for(core::SchedulerPriority priority,
                 std::chrono::nanoseconds timeout) override;
  void drain_all() override;
  bool drain_all_for(std::chrono::nanoseconds timeout) override;

 private:
  struct ScheduledTask {
    core::TimePoint deadline = 0;
    std::function<void()> callback;
  };

  /// One worker pool. Each priority lane owns one of these.
  struct Worker {
    SimpleScheduler* owner = nullptr;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::condition_variable drain_cv;
    std::map<core::ScheduleHandle, ScheduledTask> tasks;
    std::jthread thread;
    std::size_t callbacks_in_flight = 0;
    std::uint64_t queue_generation = 0;
    core::SchedulerPriority priority = core::SchedulerPriority::Lifecycle;
    const char* thread_name = nullptr;

    void worker_loop(std::stop_token st);
    void ensure_started_locked(Worker* self);
    void drain();
    bool drain_for(std::chrono::nanoseconds timeout);
  };

  /// Pick the worker that should own a callback for the given priority.
  Worker& worker_for(core::SchedulerPriority priority);

  std::atomic<uint64_t> next_handle_{1};

  /// Maps handle -> owning worker so cancel() can route correctly
  /// without locking both workers.
  std::mutex registry_mutex_;
  std::map<core::ScheduleHandle, core::SchedulerPriority> handle_to_priority_;

  std::array<Worker, core::kSchedulerPriorityCount> workers_;
};

/// Thread-safe kernel event collector.
/// Subscribes to ExecutionKernel events and buffers them for polling.
class KernelEventCollector {
 public:
  static constexpr size_t kMaxBufferedEvents = 4096;

  /// Start collecting events from the given kernel.
  void subscribe(core::ExecutionKernel& kernel);

  /// Unsubscribe from the kernel (safe to call if not subscribed).
  void unsubscribe(core::ExecutionKernel& kernel);

  /// Drain all buffered events (returns and clears the buffer).
  std::vector<core::KernelEvent> drain();

  struct DrainStats {
    std::vector<core::KernelEvent> events;
    std::uint64_t dropped_total = 0;  // monotonic overflow-drop count
  };

  /// Drain plus the monotonic count of events dropped to ring overflow.
  /// Loss anywhere in the pipeline must be countable, never silent
  /// (observability plan P4).
  DrainStats drain_with_stats();

 private:
  std::mutex mutex_;
  std::deque<core::KernelEvent> buffer_;
  std::uint64_t dropped_total_ = 0;
  core::SubscriptionHandle subscription_{0};
  bool subscribed_ = false;
};

/// Bundle that owns all components. Kernel is destroyed first (declared last).
/// Python holds a shared_ptr to this bundle to keep everything alive.
struct NativeKernelBundle {
  std::string group_name;

  // Robot Model Authority metadata. Empty for legacy/example models that do
  // not require a private RoboPlan import view. When populated, public binding
  // readout maps private view joint names back to canonical deployment names.
  std::string canonical_model_fingerprint;
  std::string roboplan_view_fingerprint;
  std::map<std::string, std::string> canonical_to_view_joint_names;
  std::map<std::string, std::string> view_to_canonical_joint_names;
  std::map<std::string, std::vector<std::string>>
      group_commandable_joint_names;

  // Owned components — order matters for destruction
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<roboplan::RRT> rrt;
  std::shared_ptr<roboplan::PathShortcutter> shortcutter;
  std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra;
  std::shared_ptr<roboplan::SimpleIk> ik;
  std::shared_ptr<roboplan::SimpleIk> single_shot_ik;

  std::unique_ptr<RoboPlanPlannerBackend> planner;
  std::unique_ptr<RoboPlanPlanningContext> context;
  std::unique_ptr<WallClock> clock;
  std::unique_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<core::TrajectoryCache> cache;
  std::unique_ptr<core::IExecutionBackend> execution;
  std::shared_ptr<core::IExecutionStateSink> execution_state_sink;
  std::shared_ptr<core::ICollisionQueryEngine> collision_runtime_monitor_engine;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      collision_validation_engines;

 private:
  mutable std::mutex close_mutex_;
  bool closed_{false};

 public:
  // Event collector — subscribes to kernel, must be destroyed before kernel
  KernelEventCollector event_collector;

  // Kernel holds references to everything above. close() resets the bundle's
  // owner before component teardown; in-flight bindings hold short snapshots.
  std::shared_ptr<core::ExecutionKernel> kernel;

  void close();
  bool closed() const;
  std::shared_ptr<core::ExecutionKernel> kernel_snapshot() const;
  ~NativeKernelBundle();
};

/// Create a fully wired native kernel with real RoboPlan backends.
///
/// @param model_name Robot model: "ur5_gripper", "fr3", "kinova", "so101"
/// @param group_name Joint group name (e.g., "arm")
/// @param urdf_path  Path to URDF file
/// @param srdf_path  Path to SRDF file
/// @param package_paths  Package directories for URDF resource resolution
/// @return Shared pointer to bundle owning all components
std::shared_ptr<NativeKernelBundle> create_native_kernel(
    const std::string& model_name,
    const std::string& group_name,
    const std::string& urdf_path,
    const std::string& srdf_path,
    const std::vector<std::string>& package_paths,
    const core::ExecutionKernelConfig& kernel_config = {});

/// Convenience overload using roboplan example_models to resolve paths.
std::shared_ptr<NativeKernelBundle> create_native_kernel(
    const std::string& model_name = "ur5_gripper",
    const std::string& group_name = "arm",
    const core::ExecutionKernelConfig& kernel_config = {});

}  // namespace manyforge::roboplan_backend
