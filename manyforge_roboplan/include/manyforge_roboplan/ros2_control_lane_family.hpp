#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/controller_launch_process.hpp>
#include <manyforge_roboplan/retained_ros2_control_lane_config.hpp>

namespace manyforge::roboplan_backend {

/// What the execution host hands a ros2_control lane family for one arm's
/// execution configuration: its runtime configuration, its bindings, its
/// model row, its written launch, the Deployment's operator Stop
/// deceleration per model source joint (the values its controllers file
/// carries) and its own stream budget overrides (its spec's).
struct Ros2ControlArmLaneInput final {
  const deployment::ros2_control_execution_runtime_contract::Configuration &configuration;
  const core::RuntimeParticipantBinding &motion_binding;
  const ModelLaneRow &row;
  const ControllerLaunchArtifacts &artifacts;
  const std::map<std::string, double, std::less<>> &max_deceleration_on_cancel_by_source_joint;
  std::span<const core::StreamParameterOverrideDTO> stream_overrides;
};

/// What the host hands a ros2_control lane family for one tool (a
/// gripper-only component with its own lane row).
struct Ros2ControlToolLaneInput final {
  const deployment::ros2_control_execution_runtime_contract::Configuration &configuration;
  const core::RuntimeParticipantBinding &gripper_binding;
  /// The tool's controller profile row: its launch values (description with
  /// its static frames, gripper joint, frame prefix).
  const deployment::ros2_control_execution_runtime_contract::Configuration::ControllerProfile
      &profile;
  const ModelLaneRow &row;
  /// Where the tool's launch files are written.
  std::filesystem::path artifacts_root;
  /// The tool's projected initial gripper position (seeds simulated hardware).
  double initial_position{};
  /// The tool's runtime namespace (`/manyforge/<component_id>`).
  std::string runtime_namespace;
};

/// A ros2_control lane family's own execution configuration (Mock, Isaac):
/// the hooks the host calls where it builds a ros2_control component, so
/// that it keeps no lane knowledge of its own. Exported through
/// `ComponentAdapterFamily::lane`; a vendor family on the generic Real
/// configuration exports none.
struct Ros2ControlLaneHooks final {
  /// The hardware starts at the Deployment's initial pose (the generator
  /// seeds it: Mock, Isaac); otherwise the hardware reports its own state.
  bool seeds_initial_pose{false};
  /// The component runs in a simulator: stale streams are simulator state
  /// (`Ros2ControlObservation::simulated`).
  bool simulated{false};
  /// The arm's ros2_control execution configuration.
  RetainedRos2ControlBackendConfig (*arm_backend_config)(const Ros2ControlArmLaneInput &input);
  /// A tool on this lane: writes its launch files, configures its gripper
  /// transport, output mode and measured lane in `config`, and returns its
  /// launch command. Null when the lane runs no tools of their own.
  ControllerLaunchCommand (*tool)(const Ros2ControlToolLaneInput &input,
                                  RetainedRos2ControlBackendConfig &config);
};

} // namespace manyforge::roboplan_backend
