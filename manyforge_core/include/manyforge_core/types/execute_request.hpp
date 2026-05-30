#pragma once

#include <string>
#include <vector>

#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/timeouts.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>

namespace manyforge::core {

/// Execute request - kernel input (spec §3.10)
struct ExecuteRequestDTO {
  std::string robot_id;
  std::string group_id;
  std::string move_id;
  std::string base_frame;
  std::string tcp_frame;
  TolerancesDTO tolerances;
  SpeedProfileDTO speed_profile;
  TimeoutsDTO timeouts;
  CacheMode cache_mode;
  MotionSpecDTO motion;
  /// Per-move bindings that arm `scope_kind == kMove` interrupt
  /// policies for exactly this execute request (spec/260 §2.6 +
  /// spec/210 §3.10). Atomic with dispatch: the kernel arms scopes
  /// after planning and validation pass and immediately before
  /// backend dispatch, then auto-reaps on any terminal execution
  /// state. Empty preserves free-move semantics.
  std::vector<MotionInterruptScopeDTO> motion_interrupt_scopes;
  /// Per-joint absolute dynamics caps (Phase 4-P5.1K). Empty preserves prior
  /// behavior. Resolved against model/backend/controller limits and enforced in
  /// time-parameterization; copied into the PlanRequestDTO by the kernel.
  PerJointDynamicsLimitsDTO per_joint_dynamics_limits;
};

}  // namespace manyforge::core
