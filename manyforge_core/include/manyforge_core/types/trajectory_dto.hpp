#pragma once

#include <manyforge_core/types/cartesian_fidelity.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

/// A single waypoint in a trajectory
struct TrajectoryWaypointDTO {
  double time_from_start_s;                  // >= 0, strictly monotonically increasing
  std::vector<double> positions_rad;         // joint positions
  std::vector<double> velocities_rad_s;      // optional; if absent, assumed zero at endpoints
  std::vector<double> accelerations_rad_s2;  // optional
};

/// Immutable, candidate-derived time-law preparation for this exact curve.
/// Part of its canonical identity, not a caller-selected execution setting.
struct TrajectoryRuntimeScaleBoundsDTO {
  double max_rate_per_s;
  double max_rate_change_per_s2;
  /// Cold, effective joint/TCP velocity certificate for the exact curve.
  /// Only a proved same-path Stop interpolant may use it. Ordinary runtime
  /// scaling remains [0,1]. Legacy absence grants no authority above one.
  double max_stop_path_rate{1.0};
  /// Producer-applied dilation from its nominal schedule. Absence is unknown,
  /// not an assumed factor of one, and cannot support coordinated retiming.
  std::optional<double> nominal_time_multiplier{};
};

/// A planned trajectory
struct TrajectoryDTO {
  /// Exact ordered operation resources represented by `joint_names`.
  /// Trajectories are currently all-angular, but their identity is the
  /// compiler-owned ResourceId rather than an adapter-private joint spelling.
  std::vector<ResourceId> resources;
  std::vector<std::string> joint_names;
  std::vector<TrajectoryWaypointDTO> waypoints;
  std::optional<TrajectoryRuntimeScaleBoundsDTO> runtime_scale_bounds{};
  /// Producer-proved source Cartesian waypoint milestones on this exact curve.
  /// Ordinal matches the prepared Cartesian request; no post-Stop path guessing.
  std::vector<double> cartesian_waypoint_times_s{};
  /// Producer-measured evidence for this exact Cartesian candidate. Generic
  /// validation republishes it only after accepting the same trajectory.
  std::optional<CartesianFidelityMetricsDTO> cartesian_fidelity_metrics{};

  bool empty() const { return waypoints.empty(); }
};

}  // namespace manyforge::core
