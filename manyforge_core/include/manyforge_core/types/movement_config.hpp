#pragma once

#include <string>

namespace manyforge::core {

/// Configuration for a single movement request.
/// ADR-0002: plain DTO, no ROS types.
/// Informed by manymove MovementConfig.msg but adapted for core.
struct MovementConfig {
  // Velocity/acceleration scaling (0.0, 1.0]
  double velocity_scaling_factor = 0.1;
  double acceleration_scaling_factor = 0.1;

  // Planner selection
  std::string planning_pipeline = "ompl";
  std::string planner_id = "RRTConnect";
  double planning_time = 5.0;
  int planning_attempts = 10;

  // Cartesian parameters
  double step_size = 0.01;
  double jump_threshold = 0.0;
  double max_cartesian_speed = 0.0;  // 0 = unlimited

  // Smoothing
  std::string smoothing_type = "time_optimal";

  // TCP frame (empty = use default)
  std::string tcp_frame;

  // Stop parameters
  double deceleration_time = 0.1;
  double min_stop_time = 0.0;
};

}  // namespace manyforge::core
