#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/trajectory/trajectory_progress_projection.hpp>

namespace manyforge::core {

const char* trajectory_progress_projection_status_name(
    TrajectoryProgressProjectionStatus status) noexcept {
  switch (status) {
    case TrajectoryProgressProjectionStatus::kProjected:
      return "projected";
    case TrajectoryProgressProjectionStatus::kMalformedTrajectory:
      return "malformed_trajectory";
    case TrajectoryProgressProjectionStatus::kObservedStateShapeMismatch:
      return "observed_state_shape_mismatch";
    case TrajectoryProgressProjectionStatus::kObservedStateNotFinite:
      return "observed_state_not_finite";
    case TrajectoryProgressProjectionStatus::kProjectionTooFar:
      return "projection_too_far";
    case TrajectoryProgressProjectionStatus::kProjectionAmbiguous:
      return "projection_ambiguous";
    case TrajectoryProgressProjectionStatus::kProgressRegressed:
      return "progress_regressed";
  }
  return "unknown";
}

TrajectoryProgressProjectionResult project_trajectory_progress(
    const TrajectoryDTO& trajectory, std::span<const double> observed_positions_rad,
    double previous_trajectory_time_s, const TrajectoryProgressProjectionConfig& config) noexcept {
  TrajectoryProgressProjectionResult result;
  if (trajectory.joint_names.empty() || trajectory.waypoints.empty() ||
      !std::isfinite(previous_trajectory_time_s) || previous_trajectory_time_s < 0.0 ||
      !std::isfinite(config.max_rms_distance_rad) || config.max_rms_distance_rad < 0.0 ||
      !std::isfinite(config.ambiguity_distance_epsilon_rad) ||
      config.ambiguity_distance_epsilon_rad < 0.0 ||
      !std::isfinite(config.ambiguity_time_separation_s) ||
      config.ambiguity_time_separation_s < 0.0 || !std::isfinite(config.regression_tolerance_s) ||
      config.regression_tolerance_s < 0.0) {
    return result;
  }
  if (!std::all_of(observed_positions_rad.begin(), observed_positions_rad.end(),
                   [](double value) { return std::isfinite(value); })) {
    result.status = TrajectoryProgressProjectionStatus::kObservedStateNotFinite;
    return result;
  }
  if (observed_positions_rad.size() != trajectory.joint_names.size()) {
    result.status = TrajectoryProgressProjectionStatus::kObservedStateShapeMismatch;
    return result;
  }
  double previous_waypoint_time = -1.0;
  for (const auto& waypoint : trajectory.waypoints) {
    if (!std::isfinite(waypoint.time_from_start_s) || waypoint.time_from_start_s < 0.0 ||
        waypoint.time_from_start_s <= previous_waypoint_time ||
        waypoint.positions_rad.size() != trajectory.joint_names.size() ||
        !std::all_of(waypoint.positions_rad.begin(), waypoint.positions_rad.end(),
                     [](double value) { return std::isfinite(value); })) {
      return result;
    }
    previous_waypoint_time = waypoint.time_from_start_s;
  }
  struct Candidate {
    double distance_sq = std::numeric_limits<double>::infinity();
    double time_s = 0.0;
    std::size_t segment = 0;
    bool valid = false;
  } best, second;

  auto project_segment = [&](std::size_t segment) {
    Candidate candidate;
    const auto& first = trajectory.waypoints[segment];
    const auto& last = trajectory.waypoints[segment + 1];
    if (last.time_from_start_s + config.regression_tolerance_s < previous_trajectory_time_s) {
      return candidate;
    }
    double direction_sq = 0.0;
    double projection_numerator = 0.0;
    for (std::size_t joint = 0; joint < observed_positions_rad.size(); ++joint) {
      const auto direction = last.positions_rad[joint] - first.positions_rad[joint];
      direction_sq += direction * direction;
      projection_numerator +=
          (observed_positions_rad[joint] - first.positions_rad[joint]) * direction;
    }
    const auto alpha =
        direction_sq <= 1e-18 ? 0.0 : std::clamp(projection_numerator / direction_sq, 0.0, 1.0);
    double distance_sq = 0.0;
    for (std::size_t joint = 0; joint < observed_positions_rad.size(); ++joint) {
      const auto projected =
          first.positions_rad[joint] +
          alpha * (last.positions_rad[joint] - first.positions_rad[joint]);
      const auto delta = observed_positions_rad[joint] - projected;
      distance_sq += delta * delta;
    }
    const auto time_s =
        first.time_from_start_s + alpha * (last.time_from_start_s - first.time_from_start_s);
    if (time_s + config.regression_tolerance_s < previous_trajectory_time_s) {
      return candidate;
    }
    return Candidate{distance_sq, time_s, segment, true};
  };

  auto consider = [&](Candidate candidate) {
    if (!candidate.valid) {
      return;
    }
    if (candidate.distance_sq < best.distance_sq) {
      second = best;
      best = candidate;
    } else if (candidate.distance_sq < second.distance_sq) {
      second = candidate;
    }
  };

  if (trajectory.waypoints.size() == 1) {
    double distance_sq = 0.0;
    for (std::size_t joint = 0; joint < observed_positions_rad.size(); ++joint) {
      const auto delta =
          observed_positions_rad[joint] - trajectory.waypoints.front().positions_rad[joint];
      distance_sq += delta * delta;
    }
    best = Candidate{distance_sq, trajectory.waypoints.front().time_from_start_s, 0, true};
  } else {
    for (std::size_t segment = 0; segment + 1 < trajectory.waypoints.size(); ++segment) {
      consider(project_segment(segment));
    }
  }

  if (!best.valid) {
    result.status = TrajectoryProgressProjectionStatus::kProgressRegressed;
    return result;
  }
  const auto joint_count = static_cast<double>(observed_positions_rad.size());
  const auto best_rms = std::sqrt(best.distance_sq / joint_count);
  result.trajectory_time_s = std::max(previous_trajectory_time_s, best.time_s);
  result.segment_index = best.segment;
  result.rms_distance_rad = best_rms;
  if (best_rms > config.max_rms_distance_rad) {
    result.status = TrajectoryProgressProjectionStatus::kProjectionTooFar;
    return result;
  }

  auto is_forward_continuation = [&](std::size_t first_segment,
                                     std::size_t second_segment) {
    const auto earlier_segment = std::min(first_segment, second_segment);
    const auto later_segment = std::max(first_segment, second_segment);
    if (later_segment != earlier_segment + 1) {
      return false;
    }
    double first_direction_sq = 0.0;
    double second_direction_sq = 0.0;
    double direction_dot = 0.0;
    for (std::size_t joint = 0; joint < observed_positions_rad.size(); ++joint) {
      const auto first_direction =
          trajectory.waypoints[earlier_segment + 1].positions_rad[joint] -
          trajectory.waypoints[earlier_segment].positions_rad[joint];
      const auto second_direction =
          trajectory.waypoints[earlier_segment + 2].positions_rad[joint] -
          trajectory.waypoints[earlier_segment + 1].positions_rad[joint];
      first_direction_sq += first_direction * first_direction;
      second_direction_sq += second_direction * second_direction;
      direction_dot += first_direction * second_direction;
    }
    return first_direction_sq > 1e-18 && second_direction_sq > 1e-18 &&
           direction_dot > 0.0;
  };

  auto is_materially_ambiguous = [&](const Candidate& candidate) {
    if (!candidate.valid || candidate.segment == best.segment) {
      return false;
    }
    const auto candidate_rms = std::sqrt(candidate.distance_sq / joint_count);
    return std::abs(candidate_rms - best_rms) <=
               config.ambiguity_distance_epsilon_rad &&
           std::abs(candidate.time_s - best.time_s) >=
               config.ambiguity_time_separation_s;
  };

  if (second.valid) {
    if (is_materially_ambiguous(second)) {
      if (!is_forward_continuation(best.segment, second.segment)) {
        result.status = TrajectoryProgressProjectionStatus::kProjectionAmbiguous;
        return result;
      }

      // A slow, forward-continuous start can project onto the beginning of two
      // adjacent segments with nearly equal distance but different timestamps.
      // They are one local branch, not a retrace. Scan only this rare case so a
      // third, genuinely distinct branch remains fail-closed.
      for (std::size_t segment = 0; segment + 1 < trajectory.waypoints.size(); ++segment) {
        if (segment == best.segment ||
            is_forward_continuation(best.segment, segment)) {
          continue;
        }
        if (is_materially_ambiguous(project_segment(segment))) {
          result.status = TrajectoryProgressProjectionStatus::kProjectionAmbiguous;
          return result;
        }
      }
    }
  }
  result.status = TrajectoryProgressProjectionStatus::kProjected;
  return result;
}

}  // namespace manyforge::core
