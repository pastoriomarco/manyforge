#pragma once

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <optional>
#include <vector>

namespace manyforge::core::detail {

// A candidate only: the caller must validate the emitted acceleration/jerk.
// Proves a common affine path and 0 <= du/dt <= 1 without assuming the inverse
// of the original time law is itself a constant-jerk polynomial.
std::optional<JerkLimitedStopProfileDTO> affine_path_stop_candidate(
    const TrajectoryDTO& accepted, double cursor, double rate, double rate_accel, double duration,
    const std::vector<double>& max_accel, const std::vector<double>& max_jerk);

}  // namespace manyforge::core::detail
