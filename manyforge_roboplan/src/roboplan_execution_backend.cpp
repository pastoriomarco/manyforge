#include <manyforge_roboplan/roboplan_execution_backend.hpp>

#include <Eigen/Dense>
#include <cstdio>

namespace manyforge::roboplan_backend {

RoboPlanExecutionBackend::RoboPlanExecutionBackend(
    std::shared_ptr<roboplan::Scene> scene)
    : scene_(std::move(scene)) {}

core::ExecutionHandle RoboPlanExecutionBackend::execute_trajectory(
    const core::TrajectoryDTO& trajectory) {
  // Update scene to final waypoint so next plan starts from correct state
  if (!trajectory.waypoints.empty()) {
    const auto& final_wp = trajectory.waypoints.back();
    Eigen::VectorXd q(final_wp.positions_rad.size());
    for (size_t i = 0; i < final_wp.positions_rad.size(); ++i) {
      q[i] = final_wp.positions_rad[i];
    }
    scene_->setJointPositions(q);
    fprintf(stderr, "[EXEC] scene updated to final waypoint (%zu joints)\n",
            final_wp.positions_rad.size());
  }
  return core::ExecutionHandle{};
}

core::StopResultDTO RoboPlanExecutionBackend::stop() {
  return core::StopResultDTO{true, std::nullopt};
}

core::PauseResultDTO RoboPlanExecutionBackend::pause() {
  return core::PauseResultDTO{true, std::nullopt};
}

core::ExecutionHandle RoboPlanExecutionBackend::resume(
    const core::TrajectoryDTO& /*trajectory*/) {
  return core::ExecutionHandle{};
}

core::ResetResultDTO RoboPlanExecutionBackend::reset() {
  return core::ResetResultDTO{true, std::nullopt};
}

}  // namespace manyforge::roboplan_backend
