#pragma once

namespace manyforge::core {

/// Timeouts DTO (spec §3.4)
struct TimeoutsDTO {
    double planning_timeout_s = 5.0;    // > 0
    double execution_timeout_s = 10.0;  // > 0
    friend bool operator==(const TimeoutsDTO&, const TimeoutsDTO&) = default;
};

}  // namespace manyforge::core
