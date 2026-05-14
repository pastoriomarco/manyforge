#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::roboplan_backend {

/// Simulated execution backend that updates the planning scene
/// over time and publishes observer-safe execution snapshots.
class RoboPlanExecutionBackend final : public core::IExecutionBackend {
 public:
  RoboPlanExecutionBackend(std::shared_ptr<roboplan::Scene> scene,
                           std::string group_name,
                           core::IClock& clock,
                           std::shared_ptr<std::mutex> scene_mutex = nullptr);
  ~RoboPlanExecutionBackend() override;

  core::ExecutionHandle execute_trajectory(const core::TrajectoryDTO& trajectory) override;
  core::Result<void> sync_observed_state_before_planning() override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO& trajectory) override;
  core::ResetResultDTO reset() override;
  core::ExecutionSnapshot get_execution_snapshot() const;

 private:
  struct ExecutionStateData {
    core::ExecutionSnapshot snapshot;
    std::atomic<bool> running{false};
    std::atomic<bool> pause_requested{false};
  };

  void publish_snapshot(core::ExecutionState state, double progress,
                        const std::vector<double>& joint_positions,
                        std::optional<core::ErrorDTO> error = std::nullopt);
  void update_scene_positions(const std::vector<double>& joint_positions);
  std::vector<double> interpolate_joint_positions(
      const core::TrajectoryDTO& trajectory, double time_s) const;
  void execution_worker(std::stop_token st, core::TrajectoryDTO trajectory);

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  core::IClock& clock_;
  std::shared_ptr<std::mutex> scene_mutex_;

  mutable std::mutex execution_mutex_;
  mutable std::mutex state_mutex_;
  std::condition_variable execution_cv_;
  std::jthread worker_thread_;
  ExecutionStateData state_;
};

}  // namespace manyforge::roboplan_backend
