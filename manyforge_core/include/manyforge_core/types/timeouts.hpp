#pragma once

namespace manyforge::core {

/// Timeouts DTO (spec §3.4)
struct TimeoutsDTO {
    double planning_timeout_s;   // > 0
    double execution_timeout_s;  // > 0
};

}  // namespace manyforge::core
