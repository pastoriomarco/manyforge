#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_roboplan/controller_robot_description.hpp>
#include <manyforge_roboplan/launch_contribution.hpp>

namespace manyforge::roboplan_backend {

/// The generic launch generator of every ros2_control component (CONTRACTS
/// §12; plan "Vendor driver configuration", startup step 3). ROS-free and
/// vendor-free: the adapter's `LaunchContribution` (from the model row and its
/// vendor or lane folder) plus the instance values (namespace, the address
/// from the site settings, joints, initial pose, the operator Stop's
/// decelerations, frame prefix) become the three run files in the run
/// directory: `robot_description.urdf`, `controllers.yaml` and, when the
/// contribution has them, `driver_parameters.yaml`. `ros2_control.launch.py`
/// runs them.

/// Failure of a launch input. Generation fails closed.
class ControllerLaunchArtifactError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Q66: the host's fixed controller manager of one controllable component.
[[nodiscard]] std::string
controller_runtime_namespace(std::string_view controllable_component_id);
inline constexpr std::string_view kControllerManagerNodeName =
    "controller_manager";
/// The generated `<ros2_control name>`: one hardware component per
/// component, in the component's own controller manager.
inline constexpr std::string_view kHardwareComponentName = "manyforge_hardware";

/// The controller cadence as the generated controllers file configures it
/// (the manager's `update_rate`, the arm controller's `state_publish_rate`
/// and `action_monitor_rate`), for the execution participant's lane facts.
struct ControllerLaneRates final {
  double controller_update_rate_hz{};
  double state_publish_rate_hz{};
  double action_monitor_rate_hz{};
};

/// The cadence of a controllers file; absent when it has no arm controller.
[[nodiscard]] std::optional<ControllerLaneRates>
controller_lane_rates(std::string_view controllers_yaml);

/// One configuration-scoped stock writer (`__reset_writer__`) serving
/// `reset_position` (Mock, Isaac). Its topic is never reused for a different
/// provider, ordered joint set or Deployment initial target.
struct MockResetPositionTarget final {
  std::string component_runtime_identity{};
  std::string controller_name;
  std::string commands_topic;
  std::vector<std::string> joint_names;
  std::vector<double> positions;
  std::string target_digest;
  /// The contribution's `command_echo_topic`: where the write is
  /// acknowledged (Isaac); absent, the measured joint states (Mock).
  std::optional<std::string> command_echo_topic{};

  friend bool operator==(const MockResetPositionTarget &,
                         const MockResetPositionTarget &) = default;
};

/// The hardware's latest command output for the reset target's joints (its
/// `command_echo_topic`, Isaac), in the target's joint order, with its
/// receipt time (the handle's clock) and a count of the samples received.
struct ResetCommandEcho final {
  std::vector<double> positions;
  std::int64_t received_ns{};
  std::uint64_t sequence{};
};

/// `controllers` (a lane's or vendor's controllers template) with the stock
/// reset-position writer (`__reset_writer__`, a `ForwardCommandController` on
/// the arm joints' `position`): its entry at the end of the
/// `/**/controller_manager:` block and its controller block appended. The
/// lane adapters that declare `reset_position` add it for an arm.
[[nodiscard]] std::string with_reset_position_writer(std::string_view controllers);

/// The instance values of one component (one robot or tool in a cell).
struct LaunchInstance final {
  /// Host-owned per-activation runtime root (created if absent).
  std::filesystem::path artifacts_root;
  std::string controllable_component_id;
  /// The component's launched description (its controller profile's
  /// compiled `description`; a deactivated arm's machine row description).
  std::string_view model_urdf;
  /// Ordered arm joints (its controller profile's `joint_names`); empty for
  /// a tool.
  std::vector<std::string> arm_joint_names;
  /// CONTRACTS §16: the machine's process with its arm deactivated. The arm's
  /// joints stay in the description (the hardware needs them) but no arm
  /// controller is configured or loaded (the contribution's controllers have
  /// none); the hardware is left as the contribution starts it. Its frames
  /// stay off the cell's TF (`publish_tf:=false`): the arm is not in the cell.
  bool arm_deactivated{};
  /// A tool's commanded model-backed gripper joint and its projected
  /// position (seeds the hardware when present).
  std::optional<MockGripperBinding> gripper;
  /// The Deployment's initial joint positions, paired with the arm joints;
  /// they seed the hardware state. Empty for hardware that reports measured
  /// state.
  std::vector<double> initial_joint_positions_rad;
  /// Q48: the Deployment's operator Stop per-joint maximum deceleration, by
  /// model source joint. Required for every arm joint.
  std::map<std::string, double, std::less<>> max_deceleration_on_cancel_rad_s2;
  /// The device address from the site settings (`${address}`); empty when
  /// the contribution names none.
  std::string address;
  /// Q82: the retained deployment frame prefix, passed literally.
  std::string frame_prefix;
  /// Exact deployment::derive_component_runtime_identity of this component;
  /// required by a `__reset_writer__`.
  std::string component_runtime_identity;
};

/// kRos2Control: every ros2_control component (`ros2_control.launch.py`).
/// kIsaac: the Isaac adapter's own launches. kSimulatedPlc, kPoseSource,
/// kAiModel: their own processes; none of these is produced here.
/// kComponent: a component's own process, started by its adapter family
/// through the host's component process launcher (CONTRACTS §6).
enum class ControllerLaunchLane {
  kRos2Control,
  kIsaac,
  kSimulatedPlc,
  kPoseSource,
  kAiModel,
  kComponent
};

struct ControllerLaunchArtifacts final {
  ControllerLaunchLane lane{};
  std::string controllable_component_id;
  /// The model row's adapter (the Isaac launch reads its profile here).
  std::string controller_profile;
  std::filesystem::path directory;
  std::filesystem::path robot_description_file;
  std::filesystem::path controllers_file;
  /// Lowercase `sha256:<hex>` of the exact written bytes.
  std::string robot_description_sha256;
  std::string controllers_sha256;
  /// Exact effective controller configuration retained from generation memory.
  std::string controllers_yaml;
  std::string runtime_namespace;
  std::string hardware_component_name;
  /// A robot's arm controller; empty for a tool and for a machine's process
  /// with its arm deactivated.
  std::string arm_controller_name;
  /// A tool's gripper controller.
  std::optional<std::string> gripper_controller_name;
  std::optional<ControllerLaneRates> rates;
  std::string frame_prefix;
  std::optional<MockResetPositionTarget> mock_reset_position;
  /// With driver parameters: its path and the `sha256:<hex>` of its bytes.
  std::filesystem::path driver_parameters_file;
  std::string driver_parameters_sha256;
  /// `ros2_control.launch.py` arguments (`name:=value`).
  std::vector<std::string> launch_arguments;
};

/// Expand the contribution with the instance values, write the run files
/// below `artifacts_root/<component>/` and return their paths, hashes and
/// the launch arguments.
[[nodiscard]] ControllerLaunchArtifacts
write_launch_artifacts(const ModelLaneRow &row,
                       const LaunchContribution &contribution,
                       const LaunchInstance &instance);

/// An Isaac tool (gripper-only) component: only its model description. The
/// tool's joints are in the robot's Isaac articulation, so it has no
/// controller manager and no controllers file; its launch publishes the
/// description and runs its gripper bridge, if any.
[[nodiscard]] ControllerLaunchArtifacts write_isaac_gripper_launch_artifacts(
    const std::filesystem::path &artifacts_root,
    std::string_view controllable_component_id, const ModelLaneRow &row,
    const std::string &model_urdf, const std::string &frame_prefix);

/// One-line JSON evidence record of written artifacts (paths, hashes, launch
/// arguments) for activation diagnostics.
[[nodiscard]] std::string
controller_launch_artifacts_evidence(const ControllerLaunchArtifacts &value);

} // namespace manyforge::roboplan_backend
