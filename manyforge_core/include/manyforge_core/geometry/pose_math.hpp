#pragma once

#include <manyforge_core/types/geometry.hpp>

namespace manyforge::core {

Pose3DTO multiply_poses(const Pose3DTO& a, const Pose3DTO& b);

Pose3DTO inverse_pose(const Pose3DTO& pose);

Pose3DTO pose_with_position_and_orientation(
    const Vector3DTO& position,
    const QuaternionDTO& orientation);

}  // namespace manyforge::core
