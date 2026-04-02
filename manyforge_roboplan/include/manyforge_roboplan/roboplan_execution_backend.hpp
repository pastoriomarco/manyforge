#pragma once

#include <memory>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/ports/iexecution_backend.hpp>

namespace manyforge::roboplan_backend {

/// Simulated execution backend that updates the planning scene
/// to the trajectory's final waypoint after each execution.
class RoboPlanExecutionBackend final : public core::IExecutionBackend {
 public:
  explicit RoboPlanExecutionBackend(std::shared_ptr<roboplan::Scene> scene);

  core::ExecutionHandle execute_trajectory(const core::TrajectoryDTO& trajectory) override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO& trajectory) override;
  core::ResetResultDTO reset() override;

 private:
  std::shared_ptr<roboplan::Scene> scene_;
};

}  // namespace manyforge::roboplan_backend
