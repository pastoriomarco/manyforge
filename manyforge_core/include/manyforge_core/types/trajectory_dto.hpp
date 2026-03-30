#pragma once

#include <string>
#include <vector>

namespace manyforge::core {

/// A single waypoint in a trajectory (spec §3.5)
struct TrajectoryWaypointDTO {
  double time_from_start_s;           // >= 0, strictly monotonically increasing
  std::vector<double> positions_rad;  // joint positions
  std::vector<double> velocities_rad_s;      // optional; if absent, assumed zero at endpoints
  std::vector<double> accelerations_rad_s2; // optional
};

/// A planned trajectory (spec §3.5)
struct TrajectoryDTO {
  std::vector<std::string> joint_names;
  std::vector<TrajectoryWaypointDTO> waypoints;

  bool empty() const { return waypoints.empty(); }
};

}  // namespace manyforge::core
