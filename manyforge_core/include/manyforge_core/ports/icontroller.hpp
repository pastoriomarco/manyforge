#pragma once

#include <functional>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Feedback during trajectory execution.
struct ExecutionFeedback {
  /// Index of the current waypoint being executed.
  size_t current_waypoint = 0;

  /// Total waypoints in the trajectory.
  size_t total_waypoints = 0;

  /// Current joint positions.
  std::vector<double> current_positions;

  /// Timestamp from IClock.
  int64_t timestamp_ns = 0;
};

/// Callback for execution progress feedback.
using ExecutionFeedbackCallback = std::function<void(const ExecutionFeedback&)>;

/// Abstract controller interface for trajectory execution.
/// ADR-0002: port interface in core; adapters wrap FollowJointTrajectory action.
/// ADR-0005: Kernel calls execute/cancel; controller does not own stop semantics.
class IController {
 public:
  virtual ~IController() = default;

  /// Begin executing a trajectory. Non-blocking.
  /// Feedback is delivered via the callback.
  /// Returns success if execution was started, error if controller is unavailable.
  virtual Result<void> execute(const TrajectoryDTO& trajectory,
                               ExecutionFeedbackCallback feedback_cb) = 0;

  /// Cancel the currently executing trajectory.
  /// Returns success if cancellation was issued, error if nothing to cancel.
  virtual Result<void> cancel() = 0;
};

}  // namespace manyforge::core
