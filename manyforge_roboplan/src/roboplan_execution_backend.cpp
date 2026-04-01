#include <manyforge_roboplan/roboplan_execution_backend.hpp>

namespace manyforge::roboplan_backend {

core::ExecutionHandle RoboPlanExecutionBackend::execute_trajectory(
    const core::TrajectoryDTO& /*trajectory*/) {
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
