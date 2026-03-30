#pragma once

#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/robot_state.hpp>

namespace manyforge::core {

/// Validation options for trajectory validation (spec §3.8)
struct ValidationOptionsDTO {
  double max_tcp_linear_speed_mps = 0.0;  // > 0: effective TCP speed limit to enforce (ADR-0015)
  std::string tcp_frame;            // frame for TCP speed measurement
  std::string base_frame;           // reference frame for TCP speed measurement
};

/// Validation result (spec §3.8)
struct ValidationResultDTO {
  bool valid = false;
  std::optional<ErrorDTO> error;  // present when valid is false; absent on success
};

/// Collision check request (spec §3.8)
struct CollisionCheckRequestDTO {
  RobotStateDTO robot_state;
};

/// Collision check result (spec §3.8)
struct CollisionCheckResultDTO {
  bool in_collision = false;
  std::optional<std::string> contact_body_a;  // optional: first colliding body name
  std::optional<std::string> contact_body_b;  // optional: second colliding body name
};

/// Forward kinematics request (spec §3.8)
struct FkRequestDTO {
  RobotStateDTO robot_state;
  std::string target_frame;  // link/frame to compute pose for
  std::string base_frame;    // frame in which the result is expressed
};

/// Joint limits (spec §3.8)
struct JointLimitsDTO {
  std::vector<std::string> joint_names;
  std::vector<double> position_lower_rad;
  std::vector<double> position_upper_rad;
  std::vector<double> velocity_max_rad_s;
  std::vector<double> acceleration_max_rad_s2;  // optional
};

}  // namespace manyforge::core
