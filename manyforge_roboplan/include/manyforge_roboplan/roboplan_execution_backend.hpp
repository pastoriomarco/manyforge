#pragma once

#include <atomic>
#include <condition_variable>
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
  double streaming_stop_completion_timeout_s = 5.0;
  /// Native-sim handoff budget for soft_on_path. This is deliberately
  /// small but non-zero so tests exercise the same wall-time contract
  /// as controller deployments.
  double soft_on_path_stop_traj_delta_s = 0.02;
  double soft_on_path_sample_period_s = 0.01;
  std::optional<double> soft_on_path_max_initial_path_deviation_rad;
  std::optional<double> soft_on_path_max_initial_velocity_deviation_rad_s;
  std::map<std::string, double> soft_on_path_max_jerk_rad_s3;
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

  // Streaming-session mode (ADR-0016). v0.1 of the simulated path supports
  // joint-mode targets only; cartesian targets are rejected with
  // unsupported_capability and the caller is expected to pre-resolve IK
  // (the ros2_control adapter handles cartesian IK internally via roboplan).
  core::StreamingSessionBeginResultDTO begin_streaming_session(
      const core::StreamingSessionConfigDTO& config) override;
  core::StreamingPushResultDTO push_streaming_target(
      uint64_t session_id,
      const core::StreamingTargetDTO& target) override;
  core::StreamingSessionEndResultDTO end_streaming_session(
      uint64_t session_id,
      core::SoftStopStrategy strategy) override;
  // ADR-0018: atomic per-tick read of the runtime velocity scale so the HMI
  // slider retimes in-flight discrete and streaming motion without stopping.
  void set_runtime_velocity_scale(double scale) override;

  // Controlled-stop capability + result surface for motion-scoped
  // interrupt outcomes (spec/260 §5.6). The native simulator supports
  // `kSpringBackHold` and native `kSoftOnPath`. `kHardDeceleration`
  // remains a ros2_control/JTC-only mode in the current scope.
  core::MotionInterruptStopCapabilityDTO
  get_motion_interrupt_stop_capability() const override;
  core::ControlledStopResultDTO execute_controlled_stop(
      const core::MotionInterruptStopProfileDTO& profile) override;

 private:
  struct ExecutionStateData {
    core::ExecutionSnapshot snapshot;
    std::atomic<bool> running{false};
    std::atomic<bool> pause_requested{false};
    std::optional<core::TrajectoryDTO> active_trajectory;
    double active_trajectory_time_s = 0.0;
    double active_runtime_velocity_scale = 1.0;
  };

  void publish_snapshot(core::ExecutionState state, double progress,
                        const std::vector<double>& joint_positions,
                        std::optional<core::ErrorDTO> error = std::nullopt);
  void update_scene_positions(const std::vector<double>& joint_positions);
  std::vector<double> interpolate_joint_positions(
      const core::TrajectoryDTO& trajectory, double time_s) const;
  void execution_worker(std::stop_token st, core::TrajectoryDTO trajectory);

  // Streaming-session worker. Drives the scene toward the latest joint
  // target at a tick cadence governed by the session's max_dispatch_rate_hz.
  // Cartesian targets are rejected at push time; the simulated path expects
  // pre-resolved joint targets.
  void streaming_worker(std::stop_token st);

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

  // Streaming-session state (ADR-0016).
  mutable std::mutex streaming_mutex_;
  std::condition_variable streaming_cv_;
  std::jthread streaming_thread_;
  std::atomic<bool> streaming_active_{false};
  uint64_t streaming_session_id_ = 0;
  uint64_t next_streaming_backend_seq_ = 1;
  core::StreamingSessionConfigDTO streaming_config_;
  std::optional<core::StreamingTargetDTO> streaming_target_;  // latest-wins
  // Last-commanded joint vector for the streaming session. The worker
  // interpolates toward the latest target one tick at a time, respecting
  // both the joint-velocity cap and `min_time_from_start_s` (which the
  // kernel populates from the deployment's
  // `global_max_tcp_linear_speed_mps`, ADR-0015). Cleared on begin/end.
  std::optional<std::vector<double>> streaming_commanded_joints_;
  // Last-tick velocity per joint for the streaming session, parallel to
  // `streaming_commanded_joints_`. Used by the acceleration limiter:
  // velocity change per tick is bounded by `a_cap[i] * tick_s` so the
  // arm doesn't snap to its velocity cap in one step. Cleared on
  // begin/end alongside the position vector.
  std::optional<std::vector<double>> streaming_commanded_velocities_;
  // Runtime velocity scale read on every worker tick. std::atomic<double> is
  // lock-free on all supported targets; memory_order_release on the setter pairs
  // with memory_order_acquire on the worker's per-tick read.
  std::atomic<double> runtime_velocity_scale_{1.0};
  // One-shot diagnostic flag: set true when the streaming worker
  // first enters the hold branch (because the published snapshot's
  // joint vector doesn't cover every session joint, so it can't be
  // used as a seed). Used to emit a stderr line once per session
  // entry so docker logs catches the failure without spamming a
  // log line every tick. Reset on each begin_streaming_session.
  bool streaming_hold_emitted_ = false;
};

}  // namespace manyforge::roboplan_backend
