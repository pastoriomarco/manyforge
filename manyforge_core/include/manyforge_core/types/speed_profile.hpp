#pragma once

#include <optional>

namespace manyforge::core {

/// Speed profile DTO (spec §3.3)
struct SpeedProfileDTO {
    double velocity_scale;              // (0, 1]
    double acceleration_scale;          // (0, 1]
    std::optional<double> max_tcp_linear_speed_mps;  // > 0, optional
};

}  // namespace manyforge::core
