#pragma once

#include <manyforge_core/ports/iexecution_backend.hpp>

namespace manyforge::roboplan_backend {

/// No-op execution backend for simulation / planning-only mode.
/// Returns immediately — no real trajectory dispatch.
class RoboPlanExecutionBackend final : public core::IExecutionBackend {
 public:
  core::ExecutionHandle execute_trajectory(const core::TrajectoryDTO& trajectory) override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO& trajectory) override;
  core::ResetResultDTO reset() override;
};

}  // namespace manyforge::roboplan_backend
