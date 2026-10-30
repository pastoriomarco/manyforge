#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

// Pre-dispatch collision validation checks the executed curve, not only its
// stored waypoints, at the collision-check step its planning attempt used
// (collision_check_step_for_attempt over Deployment
// `collision_check_step_rad`). A contact thinner than one step can be missed
// between two samples; the collision clearance makes that less likely.

/// Upper bound on the configurations one validation pass may check. Above
/// it, validation refuses (`collision_validation_sample_budget_exceeded`)
/// instead of allocating or checking without bound.
inline constexpr std::size_t kValidationMaxCheckedStates = 200'000;

/// The stop check is polled before each stored segment and after every this
/// many inserted samples, so one long segment cannot delay cancellation or
/// the validation deadline by more than this many samples.
inline constexpr std::size_t kValidationSamplingPollInterval = 1024;

/// Returns null to continue, or the refusal reason to stop sampling (for
/// example `collision_validation_cancelled`).
using ValidationSamplingStopCheck = std::function<const char*()>;

/// The configurations pre-dispatch validation checks for one trajectory.
struct CollisionValidationSamples {
  /// Same joint names and resources as the input; every stored waypoint in
  /// order, with samples of the executed curve inserted between them.
  /// Positions and times only.
  TrajectoryDTO trajectory;
  /// For each sample: (segment index + time fraction within the segment) /
  /// (stored waypoint count - 1), so contact windows keep referring to the
  /// stored trajectory exactly as before densification.
  std::vector<double> original_fractions;
  /// Samples inserted between stored waypoints (0: `trajectory` holds only
  /// the stored waypoints).
  std::size_t inserted = 0;
};

/// Sample `trajectory` along the per-segment polynomial the controller
/// executes (`trajectory_sampler`: linear, or cubic / quintic Hermite when
/// the waypoints carry velocities / accelerations). Each segment gets equal
/// time steps, enough that a bound on the joint-space speed along the
/// segment (each joint's bound from the polynomial's Bezier control points,
/// so reversals and overshoot between equal endpoints are covered, combined
/// as a Euclidean norm like RoboPlan's configuration distance) keeps every
/// step within `max_step_rad`. `stop` is polled before each segment and
/// every kValidationSamplingPollInterval inserted samples.
///
/// Errors use `validation_collision` with `reason`: the stop check's reason,
/// `collision_validation_sample_budget_exceeded`, or
/// `collision_validation_trajectory_unsampleable`.
[[nodiscard]] Result<CollisionValidationSamples> sample_for_collision_validation(
    const TrajectoryDTO& trajectory, double max_step_rad, std::size_t max_states,
    const ValidationSamplingStopCheck& stop);

}  // namespace manyforge::core
