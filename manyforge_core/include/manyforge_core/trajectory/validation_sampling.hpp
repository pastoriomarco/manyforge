#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Pre-dispatch collision validation checks the executed curve, not only its
/// stored waypoints: no joint moves more than this between two consecutive
/// checked configurations. A contact thinner than one step can still be
/// missed (M55 F-1 was a 0.09 mm finger graze 0.0019 rad wide on one joint;
/// 0.001 is the cheapest step that catches it, 0.0005 keeps a 2x factor).
inline constexpr double kValidationMaxJointStepRad = 0.0005;

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
/// time steps, enough that a bound on every joint's speed along the segment
/// (from the polynomial's Bezier control points, so reversals and overshoot
/// between equal endpoints are covered) keeps every step within
/// `max_joint_step_rad`. `stop` is polled before each segment and every
/// kValidationSamplingPollInterval inserted samples.
///
/// Errors use `validation_collision` with `reason`: the stop check's reason,
/// `collision_validation_sample_budget_exceeded`, or
/// `collision_validation_trajectory_unsampleable`.
[[nodiscard]] Result<CollisionValidationSamples> sample_for_collision_validation(
    const TrajectoryDTO& trajectory, double max_joint_step_rad, std::size_t max_states,
    const ValidationSamplingStopCheck& stop);

}  // namespace manyforge::core
