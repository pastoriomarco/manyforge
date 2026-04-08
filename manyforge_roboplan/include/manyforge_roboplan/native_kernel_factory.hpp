#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <roboplan/core/scene.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_toppra/toppra.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>
#include <manyforge_roboplan/roboplan_execution_backend.hpp>

namespace manyforge::roboplan_backend {

/// Wall clock using std::chrono::steady_clock.
class WallClock final : public core::IClock {
 public:
  core::TimePoint now() const override;
};

/// Simple scheduler using std::thread for deferred callbacks.
class SimpleScheduler final : public core::IScheduler {
 public:
  SimpleScheduler();
  ~SimpleScheduler() override;

  core::ScheduleHandle schedule(core::TimePoint deadline,
                                std::function<void()> callback) override;
  void cancel(core::ScheduleHandle handle) override;

 private:
  struct ScheduledTask {
    core::TimePoint deadline = 0;
    std::function<void()> callback;
  };

  void worker_loop();

  std::atomic<uint64_t> next_handle_{1};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<core::ScheduleHandle, ScheduledTask> tasks_;
  bool shutdown_ = false;
  std::thread worker_thread_;
};

/// Bundle that owns all components. Kernel is destroyed first (declared last).
/// Python holds a shared_ptr to this bundle to keep everything alive.
struct NativeKernelBundle {
  // Owned components — order matters for destruction
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<roboplan::RRT> rrt;
  std::shared_ptr<roboplan::PathShortcutter> shortcutter;
  std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra;
  std::shared_ptr<roboplan::SimpleIk> ik;

  std::unique_ptr<RoboPlanPlannerBackend> planner;
  std::unique_ptr<RoboPlanPlanningContext> context;
  std::unique_ptr<WallClock> clock;
  std::unique_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<core::TrajectoryCache> cache;
  std::unique_ptr<RoboPlanExecutionBackend> execution;

  // Kernel MUST be last — it holds references to everything above
  std::unique_ptr<core::ExecutionKernel> kernel;
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
