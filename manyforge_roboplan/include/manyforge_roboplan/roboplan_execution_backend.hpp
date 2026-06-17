#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::roboplan_backend {

struct RoboPlanExecutionBackendConfig {
  double reset_completion_timeout_s = 5.0;
  /// Native-sim handoff budget for soft_on_path. This is deliberately
  /// small but non-zero so tests exercise the same wall-time contract
  /// as controller deployments.
  double soft_on_path_stop_traj_delta_s = 0.02;
  double soft_on_path_sample_period_s = 0.01;
  std::optional<double> soft_on_path_max_initial_path_deviation_rad;
  std::optional<double> soft_on_path_max_initial_velocity_deviation_rad_s;
  std::map<std::string, double> soft_on_path_max_jerk_rad_s3;
};

struct RoboPlanSceneProjectionMetrics {
  std::uint64_t misses_total = 0;
  std::uint64_t successful_updates_total = 0;
  std::uint64_t lag_max_ns = 0;
  std::int64_t last_success_timestamp_ns = 0;
  std::uint64_t last_success_age_ns = 0;
};

/// Simulated execution backend that updates the planning scene
/// over time and publishes observer-safe execution snapshots.
class RoboPlanExecutionBackend final : public core::IExecutionBackend {
 public:
  RoboPlanExecutionBackend(std::shared_ptr<roboplan::Scene> scene,
                           std::string group_name,
                           core::IClock& clock,
                           std::shared_ptr<std::mutex> scene_mutex = nullptr,
                           RoboPlanExecutionBackendConfig config = {});
  ~RoboPlanExecutionBackend() override;

  core::ExecutionHandle execute_trajectory(const core::TrajectoryDTO& trajectory) override;
  core::Result<void> sync_observed_state_before_planning() override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO& trajectory) override;
  core::ResetResultDTO reset() override;
  core::ExecutionSnapshot get_execution_snapshot() const override;

  // ADR-0018: atomic per-tick read of the runtime velocity scale so the UI
  // slider retimes in-flight discrete motion without stopping.
  void set_runtime_velocity_scale(double scale) override;

  RoboPlanSceneProjectionMetrics scene_projection_metrics() const;

  // Controlled-stop capability + result surface for motion-scoped
  // interrupt outcomes (spec/260 §5.6). The native simulator supports
  // `kSpringBackHold` and native `kSoftOnPath`. `kHardDeceleration`
  // remains a ros2_control/JTC-only mode in the current scope.
  core::MotionInterruptStopCapabilityDTO
  get_motion_interrupt_stop_capability() const override;
  core::ControlledStopResultDTO execute_controlled_stop(
      const core::MotionInterruptStopProfileDTO& profile) override;
  core::ControlledStopResultDTO execute_controlled_stop(
      const core::MotionInterruptStopProfileDTO& profile,
      std::stop_token stop_token) override;

 private:
  struct ExecutionStateData {
    core::ExecutionSnapshot snapshot;
    std::atomic<bool> running{false};
    std::atomic<bool> pause_requested{false};
    std::atomic<bool> natural_completion_stop_requested{false};
    std::optional<core::TrajectoryDTO> active_trajectory;
    double active_trajectory_time_s = 0.0;
    double active_runtime_velocity_scale = 1.0;
  };

  enum class SceneUpdateMode {
    kBlocking,
    kBestEffort,
  };

  std::int64_t publish_snapshot(
      core::ExecutionState state, double progress,
      const std::vector<double>& joint_positions,
      std::optional<core::ErrorDTO> error = std::nullopt);

  // Active execution must never block observed-state publication on
  // `scene_mutex_`. During execution, call publish_snapshot() first and
  // project into the RoboPlan scene with kBestEffort. The runtime collision
  // monitor reads the published snapshot as the authoritative robot state;
  // the scene projection is for visualization/planning convergence and may
  // skip intermediate samples under contention.
  void update_scene_positions(const std::vector<double>& joint_positions,
                              SceneUpdateMode mode = SceneUpdateMode::kBlocking,
                              std::int64_t source_timestamp_ns = 0);
  void execution_worker(std::stop_token st, core::TrajectoryDTO trajectory);
  void begin_controlled_stop_runtime_scale_freeze();
  bool end_controlled_stop_runtime_scale_freeze();
  void record_scene_projection_success(std::int64_t source_timestamp_ns);
  void record_scene_projection_miss();

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  core::IClock& clock_;
  std::shared_ptr<std::mutex> scene_mutex_;
  RoboPlanExecutionBackendConfig config_;

  mutable std::mutex execution_mutex_;
  mutable std::mutex state_mutex_;
  std::condition_variable execution_cv_;
  std::jthread worker_thread_;
  ExecutionStateData state_;

  // Runtime velocity scale read on every worker tick. std::atomic<double> is
  // lock-free on all supported targets; memory_order_release on the setter pairs
  // with memory_order_acquire on the worker's per-tick read.
  std::atomic<double> runtime_velocity_scale_{1.0};
  mutable std::mutex runtime_velocity_scale_mutex_;
  int controlled_stop_runtime_scale_freeze_depth_ = 0;
  std::optional<double> deferred_runtime_velocity_scale_;
  std::atomic<std::uint64_t> scene_projection_misses_total_{0};
  std::atomic<std::uint64_t> scene_projection_successes_total_{0};
  std::atomic<std::uint64_t> scene_projection_lag_max_ns_{0};
  std::atomic<std::int64_t> scene_projection_last_success_timestamp_ns_{0};
};

}  // namespace manyforge::roboplan_backend
