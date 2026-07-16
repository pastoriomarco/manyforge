#pragma once

#include <cstddef>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <span>
#include <string>

namespace manyforge::core {

enum class TrajectoryProgressProjectionStatus {
  kProjected,
  kMalformedTrajectory,
  kObservedStateShapeMismatch,
  kObservedStateNotFinite,
  kProjectionTooFar,
  kProjectionAmbiguous,
  kProgressRegressed,
};

const char* trajectory_progress_projection_status_name(
    TrajectoryProgressProjectionStatus status) noexcept;

struct TrajectoryProgressProjectionConfig {
  double max_rms_distance_rad = 0.25;
  double ambiguity_distance_epsilon_rad = 1e-4;
  double ambiguity_time_separation_s = 0.05;
  double regression_tolerance_s = 0.005;
};

struct TrajectoryProgressProjectionResult {
  TrajectoryProgressProjectionStatus status =
      TrajectoryProgressProjectionStatus::kMalformedTrajectory;
  double trajectory_time_s = 0.0;
  std::size_t segment_index = 0;
  double rms_distance_rad = 0.0;
};

/// Project one measured joint state onto an accepted joint trajectory. This is
/// backend/lower-priority work, never a controller callback primitive. The
/// algorithm searches the complete remaining polyline so retraced paths can be
/// detected as ambiguous instead of silently choosing a later branch.
TrajectoryProgressProjectionResult project_trajectory_progress(
    const TrajectoryDTO& trajectory, std::span<const double> observed_positions_rad,
    double previous_trajectory_time_s,
    const TrajectoryProgressProjectionConfig& config = {}) noexcept;

}  // namespace manyforge::core
