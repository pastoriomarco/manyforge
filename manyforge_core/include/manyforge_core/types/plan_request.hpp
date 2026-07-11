#pragma once

#include <optional>
#include <string>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/scene_authority.hpp>
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
  double planning_timeout_s = 5.0;  // > 0
  /// Per-joint absolute dynamics caps (Phase 4-P5.1K). Empty preserves prior
  /// behavior. Consumed by the planner backend's time-parameterization.
  PerJointDynamicsLimitsDTO per_joint_dynamics_limits;
  /// Optional caller-owned action-span correlation (spec/210 §3.1.1).
  std::optional<ActionSpanDTO> action_span;
  /// Effective concrete contact-policy binding for contact-tolerant planning.
  std::string contact_policy_id;
  /// Exact planner-native scene projection retained by scene authority for
  /// this request. Production planners must reject an invalid or unavailable
  /// binding; silently planning against the latest scene is not permitted.
  SceneRevisionBindingDTO scene_binding;
};

/// Planning result (spec §3.6)
struct PlanResultDTO {
  std::optional<TrajectoryDTO> trajectory;  // present on success
  std::optional<ErrorDTO> error;             // present on failure; absent on success
  /// Exact scene revision consumed by the planner. Required whenever
  /// `trajectory` is present and must equal the request binding revision.
  SceneRevisionDTO scene_revision;
};

}  // namespace manyforge::core
