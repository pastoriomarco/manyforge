#pragma once

#include <cstdint>

namespace manyforge::core {

/// 3D vector in meters (spec §2)
struct Vector3DTO {
    double x_m;
    double y_m;
    double z_m;
};

/// Unit quaternion (spec §2)
struct QuaternionDTO {
    double x;
    double y;
    double z;
    double w;
};

/// 6-DOF pose (spec §2)
struct Pose3DTO {
    Vector3DTO position_m;
    QuaternionDTO orientation;
};

}  // namespace manyforge::core
