#pragma once

#include <optional>

#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>

namespace manyforge::roboplan_backend {

/// Exact controller-adapter configuration decoded from one retained runtime
/// product. Endpoint names and transport semantics are product facts; the
/// physical participant binding itself is supplied by the verified runtime
/// composition and cannot be replaced here.
struct RetainedRos2ControlBackendConfig final {
  ros2_control_execution::Ros2ControlExecutionBackendConfig execution;
  std::optional<ros2_control_execution::Ros2GripperActionConfig> gripper;
};

/// Derive the sole addressed arm-state authority for a controller-backed
/// retained runtime. Command-authoritative simulation produces one
/// non-expiring commanded lane; measured lanes retain their bounded declared
/// precedence.
[[nodiscard]] RetainedObservedStateChannelConfig
retained_ros2_control_motion_state_config(
    const ros2_control_execution::Ros2ControlExecutionBackendConfig &config);

/// Derive the scalar/group commanded state source written by the configured
/// ROS gripper transport through RetainedExecutionStateSink.
[[nodiscard]] RetainedObservedStateChannelConfig
retained_ros2_control_gripper_state_config(
    const core::RuntimeParticipantBinding &binding);

/// Construct a single-use retained runtime backend factory. The returned ROS
/// adapter is one object implementing motion, gripper, and Stop-stage ports;
/// it is joined to the existing retained Kernel rather than constructing a
/// competing Kernel or state publication.
[[nodiscard]] RetainedRuntimeBackendFactory
make_retained_ros2_control_backend_factory(
    RetainedRos2ControlBackendConfig config);

} // namespace manyforge::roboplan_backend
