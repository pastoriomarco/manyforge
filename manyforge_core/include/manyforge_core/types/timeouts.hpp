#pragma once

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

}  // namespace manyforge::core
