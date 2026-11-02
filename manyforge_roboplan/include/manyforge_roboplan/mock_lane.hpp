#pragma once

#include <string_view>

#include <manyforge_roboplan/launch_contribution.hpp>

namespace manyforge::roboplan_backend {

/// The Mock lane adapter (ros2_control with stock mock hardware). Its
/// settings and their reasons: manyforge_ros2_control_execution/lane/.
inline constexpr std::string_view kMockLaneAdapter = "mock";

/// The Mock lane's `LaunchContribution` (CONTRACTS §12) for one model row:
/// `mock_components/GenericSystem` hardware and the vendor's controllers (a
/// row naming `vendor`) or else the row's controllers file, plus, for an arm, the
/// stock reset-position writer that serves `reset_position`.
/// Throws `ControllerLaunchArtifactError` for an unusable row.
[[nodiscard]] LaunchContribution mock_launch_contribution(const ModelLaneRow &row,
                                                          bool arm);

} // namespace manyforge::roboplan_backend
