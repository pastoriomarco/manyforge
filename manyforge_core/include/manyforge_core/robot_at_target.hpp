#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace manyforge::core {

/// One evaluation of the "robot at target" rule.
struct RobotAtTargetDTO final {
  bool arrived{false};
  /// Largest absolute joint error (rad); 0 when the inputs are unusable.
  double max_error_rad{0.0};
  /// Joint index of `max_error_rad`.
  std::size_t joint_index{0U};
};

/// The one "robot at target" rule: arrived only when
/// the controller reports the motion finished (its goal succeeded) AND every
/// joint lies within the lane's arrival tolerance of the target. Either alone
/// is not arrival: a robot within tolerance may still be moving under its
/// goal, and a finished goal may have ended outside the tolerance.
///
/// On every ros2_control lane stock JTC applies this rule itself: the lane's
/// arrival tolerance is each arm joint's `goal` constraint and the port keeps
/// it on every goal, so a succeeded result already means both. Callers with
/// their own position evidence (the Real lane's measured terminal gate) use
/// this function with the same lane value. Positions and target are in the
/// same joint order; a size mismatch, an empty or non-finite input is not
/// arrival.
[[nodiscard]] inline RobotAtTargetDTO robot_at_target(
    const bool controller_finished, const std::span<const double> positions_rad,
    const std::span<const double> target_rad, const double tolerance_rad) noexcept {
  RobotAtTargetDTO result;
  if (positions_rad.empty() || positions_rad.size() != target_rad.size() ||
      !std::isfinite(tolerance_rad) || tolerance_rad < 0.0) {
    return result;
  }
  for (std::size_t index = 0U; index < positions_rad.size(); ++index) {
    const double error = std::abs(positions_rad[index] - target_rad[index]);
    if (!std::isfinite(error)) return RobotAtTargetDTO{};
    if (error > result.max_error_rad) {
      result.max_error_rad = error;
      result.joint_index = index;
    }
  }
  result.arrived = controller_finished && result.max_error_rad <= tolerance_rad;
  return result;
}

}  // namespace manyforge::core
