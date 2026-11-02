#pragma once

#include <algorithm>
#include <optional>

namespace manyforge::core {

/// Timeouts DTO (spec §3.4)
struct TimeoutsDTO {
    double planning_timeout_s = 5.0;    // > 0
    double execution_timeout_s = 10.0;  // > 0
    /// Optional user-set zero-speed hold timeout (> 0 when set). When the
    /// speed slider has held the dispatched motion at zero for this long, the
    /// motion ends Stopped with no error and resumes like an operator Stop.
    /// Unset keeps `main`'s behaviour: a zero-speed hold never ends a motion.
    std::optional<double> zero_speed_hold_timeout_s{};
    friend bool operator==(const TimeoutsDTO&, const TimeoutsDTO&) = default;
};

/// A normal motion's no-report budget for a trajectory of `duration_s`: the
/// Kernel's execution watchdog fires only after max(10 s, `minimum_s`,
/// 1.5 x duration + 2 s), so the controller's own verdict (end of trajectory
/// plus its `goal_time`) always comes first. Also the bound of a dispatched
/// Stop trajectory (maintainer M6: a Stop finishes like a normal motion).
[[nodiscard]] inline double trajectory_execution_budget_s(const double duration_s,
                                                          const double minimum_s = 10.0) {
    return std::max({10.0, minimum_s, 1.5 * duration_s + 2.0});
}

}  // namespace manyforge::core
