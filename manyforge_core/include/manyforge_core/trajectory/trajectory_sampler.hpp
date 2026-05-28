#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Success-side metadata: the sampler produced a valid sample but at a
/// lower polynomial degree than the trajectory's nominal richness because
/// of partial or inconsistent derivative data at the segment endpoints.
///
/// Structural problems (mismatched position vectors, non-monotonic or
/// duplicate times, empty joint_names) are NOT fallbacks; they are
/// returned via Result<>::Err with a `TrajectorySampleError`.
enum class TrajectorySampleFallback : std::uint8_t {
  kNone,
  kIncompleteVelocityEndpoint,
  kIncompleteAccelerationEndpoint,
  kJointCountMismatchVelocity,
  kJointCountMismatchAcceleration,
};

inline std::string_view to_string(TrajectorySampleFallback f) {
  switch (f) {
    case TrajectorySampleFallback::kNone:
      return "none";
    case TrajectorySampleFallback::kIncompleteVelocityEndpoint:
      return "incomplete_velocity_endpoint";
    case TrajectorySampleFallback::kIncompleteAccelerationEndpoint:
      return "incomplete_acceleration_endpoint";
    case TrajectorySampleFallback::kJointCountMismatchVelocity:
      return "joint_count_mismatch_velocity";
    case TrajectorySampleFallback::kJointCountMismatchAcceleration:
      return "joint_count_mismatch_acceleration";
  }
  return "unknown";
}

/// Malformed-trajectory rejections. Returned via Result<>::Err with the
/// stable string code from `to_string()` in `ErrorDTO::code`.
enum class TrajectorySampleError : std::uint8_t {
  kEmptyJointNames,
  kPositionJointCountMismatch,
  kNonMonotonicWaypointTimes,
  kDuplicateWaypointTimes,
  /// Any waypoint's `time_from_start_s` is NaN or infinity.
  kNonFiniteWaypointTime,
  /// The caller-supplied `time_s` is NaN or infinity. Sampling cannot
  /// be ordered relative to the waypoint timeline, so the call is
  /// rejected before any comparison runs.
  kNonFiniteSampleTime,
};

inline std::string_view to_string(TrajectorySampleError e) {
  switch (e) {
    case TrajectorySampleError::kEmptyJointNames:
      return "trajectory_sample_empty_joint_names";
    case TrajectorySampleError::kPositionJointCountMismatch:
      return "trajectory_sample_position_joint_count_mismatch";
    case TrajectorySampleError::kNonMonotonicWaypointTimes:
      return "trajectory_sample_non_monotonic_waypoint_times";
    case TrajectorySampleError::kDuplicateWaypointTimes:
      return "trajectory_sample_duplicate_waypoint_times";
    case TrajectorySampleError::kNonFiniteWaypointTime:
      return "trajectory_sample_non_finite_waypoint_time";
    case TrajectorySampleError::kNonFiniteSampleTime:
      return "trajectory_sample_non_finite_sample_time";
  }
  return "trajectory_sample_unknown";
}

/// One sampled state of the trajectory at a given wall-clock time.
///
/// Samples are compatible with the controller profile configured by
/// deployment: per-segment polynomial interpolation derived independently
/// from boundary constraints. Linear segments match endpoint positions.
/// Cubic segments additionally match endpoint velocities. Quintic
/// segments additionally match endpoint accelerations.
///
/// Position, velocity, and acceleration vectors have the same size as the
/// trajectory's per-waypoint position vector (or are empty when the
/// trajectory itself is empty).
struct TrajectorySampleResult {
  std::vector<double> positions_rad;
  std::vector<double> velocities_rad_s;
  std::vector<double> accelerations_rad_s2;

  /// Set only when sampling a real segment. nullopt for the pre-start
  /// clamp, the post-end clamp, single-waypoint trajectories, and empty
  /// trajectories.
  std::optional<std::size_t> segment_index;

  /// 1 = linear, 3 = cubic Hermite, 5 = quintic.
  /// 0 = clamped, single-waypoint, or empty.
  std::uint8_t degree;

  /// True when time_s is outside [t_first, t_last] OR the trajectory has
  /// fewer than two waypoints. Inside the trajectory's time range, the
  /// sampler always evaluates a real segment polynomial.
  bool clamped;

  /// kNone on the natural degree path. Other values describe successful
  /// degree downgrades caused by partial or inconsistent derivative
  /// vectors at the segment endpoints.
  TrajectorySampleFallback fallback_reason;
};

/// Sample the trajectory at `time_s`.
///
/// Boundary rules (held position with zero velocity / zero acceleration
/// outside the trajectory's time range; real-segment sampling inside):
///
///   - time_s < t_first  : pre-start clamp.
///   - time_s > t_last   : post-end clamp.
///   - t_first <= time_s <= t_last :
///       With N+1 waypoints defining N segments, segment i covers the
///       half-open interval [t_i, t_{i+1}). An exact internal waypoint
///       time t_k for 0 < k < N is sampled as the start of segment k
///       (alpha = 0), not the end of segment k-1. The final endpoint
///       t_N samples segment N-1 at alpha = 1. The polynomial preserves
///       the endpoint's stored velocity and acceleration at exact
///       endpoint times.
///   - single waypoint   : that waypoint's position with zero velocity and
///                         acceleration, clamped = true.
///   - empty trajectory  : empty position / velocity / acceleration
///                         vectors, degree = 0, clamped = true,
///                         segment_index = nullopt.
///
/// Returns Err when the trajectory itself is structurally malformed
/// (empty joint_names, position vector size mismatch across waypoints,
/// non-monotonic time_from_start_s, duplicate waypoint times). Structural
/// failures short-circuit the sampler and do not degenerate into
/// fallbacks.
Result<TrajectorySampleResult> sample_trajectory(
    const TrajectoryDTO& trajectory, double time_s);

}  // namespace manyforge::core
