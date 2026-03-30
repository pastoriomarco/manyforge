#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace manyforge::core {

/// A single waypoint in a trajectory.
struct TrajectoryPoint {
  /// Joint positions (radians for revolute, meters for prismatic).
  std::vector<double> positions;

  /// Joint velocities.
  std::vector<double> velocities;

  /// Joint accelerations (may be empty if not computed).
  std::vector<double> accelerations;

  /// Time from trajectory start in nanoseconds.
  /// ADR-0006: must be monotonically increasing across points.
  int64_t time_from_start_ns = 0;
};

/// A planned trajectory DTO.
/// ADR-0002: plain struct, no ROS types.
/// ADR-0006: invariants enforced by planner contract tests.
struct TrajectoryDTO {
  /// Joint names in order, matching position/velocity/acceleration arrays.
  std::vector<std::string> joint_names;

  /// Ordered waypoints. Must have monotonic time_from_start_ns.
  std::vector<TrajectoryPoint> points;

  /// Planner backend that produced this trajectory.
  std::string planner_backend_id;

  /// Scene version/hash at planning time. Used for cache keying (ADR-0008).
  uint64_t scene_version = 0;

  /// Planning duration in nanoseconds.
  int64_t planning_duration_ns = 0;

  bool empty() const { return points.empty(); }
};

}  // namespace manyforge::core
