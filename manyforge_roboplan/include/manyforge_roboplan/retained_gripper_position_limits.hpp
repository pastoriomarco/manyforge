#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>

namespace manyforge::roboplan_backend {

struct RetainedGripperPositionLimits final {
  core::CoordinateKind coordinate_kind{core::CoordinateKind::kAngular};
  double lower_position{};
  double upper_position{};
};

} // namespace manyforge::roboplan_backend
