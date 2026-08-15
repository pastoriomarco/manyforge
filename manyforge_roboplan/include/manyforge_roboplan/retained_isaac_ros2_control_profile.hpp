#pragma once

#include <optional>
#include <string>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

namespace manyforge::roboplan_backend {

/// Exact ROS namespace assigned to one retained Isaac controllable component.
/// Component identities that cannot be represented as one bounded ROS token
/// are rejected instead of being lossy-normalized into a crossed endpoint.
[[nodiscard]] std::string retained_isaac_ros_namespace(
    const core::RuntimeParticipantBinding &binding);

/// Expand one closed retained Isaac profile into the exact generic ROS/JTC
/// adapter configuration used by the installed execution host.
///
/// The retained runtime product owns only the small portable profile selector,
/// cadence, seed, and initial state. Endpoint names, controller type, hardware
/// identity, command/state interfaces, Stop mechanism, and gripper transport
/// are build-owned profile facts. Runtime participant bindings supply the
/// compiled/source coordinate map and are checked exactly here.
[[nodiscard]] RetainedRos2ControlBackendConfig
make_retained_isaac_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding);

} // namespace manyforge::roboplan_backend
