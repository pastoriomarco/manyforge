#pragma once

#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/execution_result.hpp>

namespace manyforge::core {

/// Execution backend interface (spec 220 §4)
/// 
/// Responsibilities:
/// - Execute a time-parameterized joint trajectory
/// - Provide phase feedback for kernel events
/// - Provide non-blocking stop/pause/resume/reset primitives
///
/// Threading:
/// - Stop/Pause/Resume/Reset MUST be non-blocking from the kernel caller perspective
/// - Stop/Pause/Resume/Reset MUST be safe to call from a different thread than ExecuteTrajectory
/// - The kernel MUST NOT issue concurrent ExecuteTrajectory calls to the same backend instance
class IExecutionBackend {
 public:
    virtual ~IExecutionBackend() = default;

    /// Execute a trajectory (spec 220 §4)
    /// 
    /// @param trajectory The trajectory to execute
    /// @return ExecutionHandle for tracking the execution
    virtual ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) = 0;

    /// Stop the current execution (spec 220 §4)
    /// 
    /// If no execution is in flight, returns accepted=false with invalid_request.
    /// 
    /// @return StopResultDTO indicating whether stop was accepted
    virtual StopResultDTO stop() = 0;

    /// Pause the current execution (spec 220 §4)
    /// 
    /// If no execution is in flight, returns accepted=false with invalid_request.
    /// 
    /// @return PauseResultDTO indicating whether pause was accepted
    virtual PauseResultDTO pause() = 0;

    /// Resume execution with a new trajectory (spec 220 §4)
    /// 
    /// Resume takes a trajectory because the kernel may replan from the paused state;
    /// the backend executes it as a fresh trajectory dispatch.
    /// 
    /// @param trajectory The trajectory to resume with
    /// @return ExecutionHandle for tracking the resumed execution
    virtual ExecutionHandle resume(const TrajectoryDTO& trajectory) = 0;

    /// Reset the execution backend (spec 220 §4)
    /// 
    /// @return ResetResultDTO indicating whether reset was accepted
    virtual ResetResultDTO reset() = 0;
};

}  // namespace manyforge::core
