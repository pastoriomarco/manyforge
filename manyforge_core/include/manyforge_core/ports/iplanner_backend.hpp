#pragma once

#include <string>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/movement_config.hpp>
#include <manyforge_core/types/scene_snapshot_dto.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Request DTO for planning.
struct PlanRequest {
  /// Target joint positions (for joint-space goals).
  std::vector<double> goal_joint_positions;

  /// Target pose (for Cartesian goals). Empty goal_joint_positions implies Cartesian.
  Pose goal_pose;

  /// Movement configuration (planner selection, scaling, etc.).
  MovementConfig config;

  /// Start state override. If empty, planner uses current robot state from scene.
  std::vector<double> start_joint_positions;
};

/// Validation result for a trajectory against a scene.
struct ValidationResult {
  bool valid = false;
  ErrorCode error = ErrorCode::kSuccess;
  std::string detail;
};

/// Abstract planner backend interface.
/// ADR-0006: backends are interchangeable; tested via invariant-based contract tests.
/// ADR-0002: port interface in core; adapters implement for MoveItCpp, MoveGroup, etc.
class IPlannerBackend {
 public:
  virtual ~IPlannerBackend() = default;

  /// Plan a trajectory from the current (or specified) state to the goal.
  virtual Result<TrajectoryDTO> plan(const PlanRequest& request,
                                     const SceneSnapshotDTO& scene) = 0;

  /// Validate a trajectory against a scene snapshot.
  /// Returns whether the trajectory is collision-free and within constraints.
  virtual ValidationResult validate(const TrajectoryDTO& trajectory,
                                    const SceneSnapshotDTO& scene) = 0;

  /// Unique identifier for this backend (used in cache keys, ADR-0008).
  virtual std::string backend_id() const = 0;
};

}  // namespace manyforge::core
