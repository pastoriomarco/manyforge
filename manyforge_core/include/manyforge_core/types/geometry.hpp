#pragma once

#include <cstdint>

namespace manyforge::core {

/// 3D vector in meters (spec §2)
struct Vector3DTO {
    double x_m = 0.0;
    double y_m = 0.0;
    double z_m = 0.0;
    friend bool operator==(const Vector3DTO&, const Vector3DTO&) = default;
};

/// Unit quaternion (spec §2)
struct QuaternionDTO {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
    friend bool operator==(const QuaternionDTO&, const QuaternionDTO&) = default;
};

/// 6-DOF pose (spec §2)
struct Pose3DTO {
    Vector3DTO position_m;
    QuaternionDTO orientation;
    friend bool operator==(const Pose3DTO&, const Pose3DTO&) = default;
};

}  // namespace manyforge::core
