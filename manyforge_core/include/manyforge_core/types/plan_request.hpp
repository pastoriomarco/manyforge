#pragma once

#include <optional>
#include <string>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Planning request (spec §3.6)
struct PlanRequestDTO {
  std::string robot_id;
  std::string group_id;
  std::string move_id;
  std::string base_frame;
  std::string tcp_frame;
  RobotStateDTO start_state;
  MotionSpecDTO motion;
  TolerancesDTO tolerances;
  SpeedProfileDTO speed_profile;
  double planning_timeout_s;  // > 0
  /// Per-joint absolute dynamics caps (Phase 4-P5.1K). Empty preserves prior
  /// behavior. Consumed by the planner backend's time-parameterization.
  /// Kept last so the positional aggregate constructor stays aligned.
  PerJointDynamicsLimitsDTO per_joint_dynamics_limits;
};

/// Planning result (spec §3.6)
struct PlanResultDTO {
  std::optional<TrajectoryDTO> trajectory;  // present on success
  std::optional<ErrorDTO> error;             // present on failure; absent on success
};

}  // namespace manyforge::core
