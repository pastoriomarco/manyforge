#pragma once

#include <array>
#include <cstddef>
#include <span>

#include <manyforge_core/types/geometry.hpp>

namespace manyforge::core {

/// TOOLCTX: what a robot's own controller needs to know about the end of the
/// arm, vendor-neutral, in the robot's flange frame and SI units. ManyForge
/// computes it per robot (Enable: the default TCP; every new motion: that
/// motion's TCP; the payload from the scene at that moment). The robot's
/// vendor adapter maps it to its controller's API and decides when to send.
struct ToolContextDTO final {
  /// The active TCP, T_flange_tcp.
  Pose3DTO tcp_pose_in_flange;
  /// Everything carried beyond the flange: the mounted tool's own payload
  /// plus the mass of every object attached to the robot or its tool.
  double payload_mass_kg{};
  /// Centre of mass of that payload in the flange frame; zero when massless.
  std::array<double, 3> payload_center_of_mass_m{};

  friend bool operator==(const ToolContextDTO&, const ToolContextDTO&) = default;
};

/// One mass of the payload at a point of the flange frame.
struct ToolContextPointMassDTO final {
  double mass_kg{};
  std::array<double, 3> position_in_flange_m{};

  friend bool operator==(const ToolContextPointMassDTO&,
                         const ToolContextPointMassDTO&) = default;
};

/// The context of one TCP and these masses: the total mass and the
/// mass-weighted mean of their positions. Massless rows add nothing.
[[nodiscard]] inline ToolContextDTO compose_tool_context(
    const Pose3DTO& tcp_pose_in_flange,
    std::span<const ToolContextPointMassDTO> masses) {
  ToolContextDTO context{tcp_pose_in_flange, 0.0, {0.0, 0.0, 0.0}};
  std::array<double, 3> moment{0.0, 0.0, 0.0};
  for (const auto& row : masses) {
    if (row.mass_kg <= 0.0) continue;
    context.payload_mass_kg += row.mass_kg;
    for (std::size_t axis = 0; axis < 3U; ++axis)
      moment[axis] += row.mass_kg * row.position_in_flange_m[axis];
  }
  if (context.payload_mass_kg > 0.0) {
    for (std::size_t axis = 0; axis < 3U; ++axis)
      context.payload_center_of_mass_m[axis] =
          moment[axis] / context.payload_mass_kg;
  }
  return context;
}

}  // namespace manyforge::core
