#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_roboplan/retained_isaac_arm_guard_binding.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>

namespace manyforge::roboplan_backend {

/// Exact ROS execution configuration decoded from one retained runtime
/// product: the shared transport, the arm's JTC lane (adapter, controller and
/// the Core participant's lane inputs) and the gripper transport. The
/// physical participant binding itself is supplied by the verified runtime
/// composition and cannot be replaced here.
struct RetainedRos2ControlBackendConfig final {
  std::string node_name;
  /// Absolute ROS namespace of this runtime instance.
  std::string node_namespace;
  std::size_t executor_thread_count{0U};
  /// The arm's lane: adapter configuration and controller facts in the
  /// controller's own joint names (the binding's source joint names). The
  /// factory supplies the compiled names from the binding.
  RetainedRos2ControlArmTransport arm;
  /// Lane and deployment inputs of the Core participant. Required values
  /// that no lane decision supplies yet stay absent and refuse composition.
  core::ExecutionPortParticipantConfig participant;
  /// State-authority profile id of the arm's addressed commanded state.
  std::string observed_state_authority_profile_id;
  std::optional<ros2_control_execution::Ros2GripperActionConfig> gripper;
  /// The Isaac lane's command-age guard sequencing (absent: no guard).
  std::optional<RetainedIsaacArmGuardBindingConfig> isaac_guard;
  /// Managed first installation starts the executor only after fenced
  /// ownership.
  bool defer_ros_start{false};
  /// Exact successor computed from the owned predecessor before retirement.
  /// Absent on ordinary startup; never derived from caller counters here.
  std::optional<core::SelectedControllerOccurrenceDTO> successor_occurrence;
  /// Actual owner of the same clock passed to the factory callback. Required
  /// for a measured lane; retained by the existing backend handle through
  /// transport teardown. Never substitute a borrowed shared-pointer alias.
  std::shared_ptr<core::IClock> measured_receiver_clock;
  /// Constructor-only acquisition of the native host's compatible ROS context.
  /// Absent for standalone callers, which retain private context ownership.
  ros2_control_execution::Ros2TransportResources::ContextProvider ros_context_provider{};
};

/// The sole addressed arm-state authority of a controller-backed runtime:
/// one non-expiring commanded source written from the controller's own
/// reference (item 20, commanded simulation).
[[nodiscard]] RetainedObservedStateChannelConfig
retained_ros2_control_motion_state_config(
    const RetainedRos2ControlBackendConfig &config);

/// Derive the scalar/group commanded state source written by the configured
/// ROS gripper transport through RetainedExecutionStateSink.
[[nodiscard]] RetainedObservedStateChannelConfig
retained_ros2_control_gripper_state_config(
    const core::RuntimeParticipantBinding &binding);

/// Construct a single-use retained runtime backend factory: the shared ROS
/// transport, the arm's JTC adapter for the initial binding epoch and the Core
/// participant over it, and the gripper adapter, all owned by `runtime_handle`
/// and joined to the retained Kernel and state publication.
[[nodiscard]] RetainedRuntimeBackendFactory
make_retained_ros2_control_backend_factory(
    RetainedRos2ControlBackendConfig config,
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle = {});

/// Same owned ROS transport for a component with only its scalar gripper.
/// The actual gripper controller/profile facts are supplied without an arm.
[[nodiscard]] std::shared_ptr<core::IGripperCommandBackend>
make_retained_ros2_control_gripper_backend(
    RetainedRos2ControlBackendConfig config,
    const core::RuntimeParticipantBinding& binding,
    const std::shared_ptr<RetainedObservedStateChannel>& state_channel,
    core::IClock& clock, core::IScheduler& scheduler,
    const std::shared_ptr<RetainedRos2ControlRuntimeHandle>& runtime_handle);

} // namespace manyforge::roboplan_backend
