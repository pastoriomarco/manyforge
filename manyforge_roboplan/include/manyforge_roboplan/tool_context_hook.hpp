#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/tool_context.hpp>

namespace manyforge::roboplan_backend {

/// CONTRACTS §6: the tool-context hook a vendor family exports for its robot
/// (e.g. xArm `ApplyToolContext`); the host sets it on the robot's
/// ros2_control connection. ROS-free: a vendor library includes it without
/// linking RoboPlan.
struct ToolContextHook final {
  /// Sends the TCP offset (absent: keep; the identity pose: no TCP) and the
  /// payload (absent: keep). Ok(true) when an offset was sent and the robot
  /// re-armed. A failed call that may still act names it in
  /// `details.effect` (`possible`, or `observed` once the robot re-armed). At
  /// Enable (`at_enable`) the call follows the step's deadline and
  /// cancellation.
  std::function<core::Result<bool>(
      const std::optional<core::Pose3DTO> &offset,
      const std::optional<core::ToolContextPointMassDTO> &payload, bool at_enable,
      std::optional<std::chrono::steady_clock::time_point> step_deadline,
      std::stop_token stop)>
      apply;
  /// How long the driver takes to bring its controllers back after a re-arm.
  std::chrono::nanoseconds rearm_bound{};
};

} // namespace manyforge::roboplan_backend
