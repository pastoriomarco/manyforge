#pragma once

#include <map>
#include <manyforge_isaac_adapter/isaac_lane.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/retained_ros2_control_lane_config.hpp>
#include <span>
#include <string>
#include <string_view>

namespace manyforge::isaac_adapter {

/// The ROS execution configuration of one Isaac robot (arm only; its tool is
/// its own component). The shared ros2_control JTC wiring with the robot's
/// `lanes.isaac` facts, the cadence of its generated controllers file, the
/// Deployment's operator Stop deceleration per model joint (the same values
/// the generated controllers file carries) and the measured lane on the
/// articulation's joint states topic. Throws std::invalid_argument for a
/// binding that doesn't fit.
[[nodiscard]] roboplan_backend::RetainedRos2ControlBackendConfig make_isaac_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding, const IsaacArmFacts &arm,
    const IsaacLaneSettings &settings, const roboplan_backend::ControllerLaneRates &rates,
    const std::map<std::string, double, std::less<>> &max_deceleration_on_cancel_by_source_joint,
    std::span<const core::StreamParameterOverrideDTO> stream_overrides = {});

/// The gripper transport of one Isaac tool in `runtime_namespace`, then the
/// authored output mode. The tool's joints stay in the robot's articulation:
/// its state is on the shared joint states topic and only its own joint is
/// declared. A pneumatic I/O mapping is refused
/// (`pneumatic_io_unsupported_on_isaac`): Isaac drives the gripper itself.
void configure_isaac_gripper_action(
    roboplan_backend::RetainedRos2ControlBackendConfig &config,
    const deployment::ros2_control_execution_runtime_contract::Configuration &configuration,
    const core::RuntimeParticipantBinding &gripper, const IsaacGripperFacts &facts,
    const IsaacLaneSettings &settings, std::string_view runtime_namespace);

}  // namespace manyforge::isaac_adapter
