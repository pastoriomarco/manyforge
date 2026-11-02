#pragma once

#include <array>

namespace manyforge::core {

/// One rigid placement expressed as metres plus an XYZW unit quaternion.
///
/// The passive DTO is shared by immutable catalog declarations and mutable
/// graph records. Exact shape and quaternion validation belong to their
/// respective qualified artifact boundaries.
struct PlacementPoseDTO {
  std::array<double, 3> position_m{};
  std::array<double, 4> orientation_xyzw{0.0, 0.0, 0.0, 1.0};

  friend bool operator==(const PlacementPoseDTO&,
                         const PlacementPoseDTO&) = default;
};

}  // namespace manyforge::core
