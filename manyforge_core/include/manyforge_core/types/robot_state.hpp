#pragma once

#include <string>
#include <vector>

namespace manyforge::core {

/// Robot state DTO
struct RobotStateDTO {
    std::vector<std::string> joint_names;
    std::vector<double> positions_rad;
    std::vector<double> velocities_rad_s;  // optional; absent means unknown/not available
};

}  // namespace manyforge::core
