#pragma once

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <map>
#include <optional>
#include <string>

namespace manyforge::core::detail {

// Spring-back at-rest join. Forward braking ended at rest at
// `forward_end_s`; the return lasts `return_time_s` and normally starts at the
// half boundary `phase_time_s`. A late forward end moves the return start
// after it. Returns the return start time, or nullopt when the whole Stop
// would end after `deadline_s` (the caller escalates to the hard stop).
std::optional<double> spring_back_return_start_s(double forward_end_s, double phase_time_s,
                                                 double return_time_s, double deadline_s);

struct StopPhaseJerkLimits {
  std::map<std::string, double> limits;
  /// `deployment`, `default`, `mixed`, or `disabled`.
  std::string source;
};

// Jerk limits for one Stop phase (as main). An authored jerk is
// used verbatim; a joint without one gets motion_jerk_preset_limit(accel,
// reference_s), where reference_s is the phase's own duration: deceleration
// time - stop_traj_delta_s for a soft ramp or a spring-back forward half,
// half the deceleration time for a spring-back return.
StopPhaseJerkLimits stop_phase_jerk_limits(const TrajectoryDTO& trajectory,
                                           const std::map<std::string, double>& accel_limits,
                                           const SoftOnPathPlannerConfig& config,
                                           double reference_s);

}  // namespace manyforge::core::detail
