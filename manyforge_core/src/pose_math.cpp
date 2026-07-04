#include <manyforge_core/geometry/pose_math.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace manyforge::core {

Pose3DTO multiply_poses(const Pose3DTO& a, const Pose3DTO& b) {
  auto pose_to_matrix = [](const Pose3DTO& pose) {
    std::array<std::array<double, 4>, 4> m{{
        {{1.0, 0.0, 0.0, 0.0}},
        {{0.0, 1.0, 0.0, 0.0}},
        {{0.0, 0.0, 1.0, 0.0}},
        {{0.0, 0.0, 0.0, 1.0}},
    }};
    const double x = pose.orientation.x;
    const double y = pose.orientation.y;
    const double z = pose.orientation.z;
    const double w = pose.orientation.w;
    m[0][0] = 1 - 2 * y * y - 2 * z * z;
    m[0][1] = 2 * x * y - 2 * z * w;
    m[0][2] = 2 * x * z + 2 * y * w;
    m[1][0] = 2 * x * y + 2 * z * w;
    m[1][1] = 1 - 2 * x * x - 2 * z * z;
    m[1][2] = 2 * y * z - 2 * x * w;
    m[2][0] = 2 * x * z - 2 * y * w;
    m[2][1] = 2 * y * z + 2 * x * w;
    m[2][2] = 1 - 2 * x * x - 2 * y * y;
    m[0][3] = pose.position_m.x_m;
    m[1][3] = pose.position_m.y_m;
    m[2][3] = pose.position_m.z_m;
    return m;
  };

  auto matrix_to_pose = [](const std::array<std::array<double, 4>, 4>& m) {
    Pose3DTO pose;
    pose.position_m = {m[0][3], m[1][3], m[2][3]};

    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0) {
      const double s = std::sqrt(trace + 1.0) * 2.0;
      pose.orientation.w = 0.25 * s;
      pose.orientation.x = (m[2][1] - m[1][2]) / s;
      pose.orientation.y = (m[0][2] - m[2][0]) / s;
      pose.orientation.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
      pose.orientation.w = (m[2][1] - m[1][2]) / s;
      pose.orientation.x = 0.25 * s;
      pose.orientation.y = (m[0][1] + m[1][0]) / s;
      pose.orientation.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
      pose.orientation.w = (m[0][2] - m[2][0]) / s;
      pose.orientation.x = (m[0][1] + m[1][0]) / s;
      pose.orientation.y = 0.25 * s;
      pose.orientation.z = (m[1][2] + m[2][1]) / s;
    } else {
      const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
      pose.orientation.w = (m[1][0] - m[0][1]) / s;
      pose.orientation.x = (m[0][2] + m[2][0]) / s;
      pose.orientation.y = (m[1][2] + m[2][1]) / s;
      pose.orientation.z = 0.25 * s;
    }
    return pose;
  };

  const auto ma = pose_to_matrix(a);
  const auto mb = pose_to_matrix(b);
  std::array<std::array<double, 4>, 4> out{};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (size_t k = 0; k < 4; ++k) {
        sum += ma[i][k] * mb[k][j];
      }
      out[i][j] = sum;
    }
  }
  return matrix_to_pose(out);
}

Pose3DTO pose_with_position_and_orientation(
    const Vector3DTO& position,
    const QuaternionDTO& orientation) {
  Pose3DTO pose;
  pose.position_m = position;
  pose.orientation = orientation;
  return pose;
}

}  // namespace manyforge::core
