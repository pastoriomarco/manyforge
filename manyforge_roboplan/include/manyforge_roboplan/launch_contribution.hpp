#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CONTRACTS §12: what a vendor or lane adapter hands the generic, vendor-free
// launch generator (`controller_launch_artifacts`). Plain data and the
// standard library only, so a vendor adapter package includes it without
// linking the generator.

namespace manyforge::roboplan_backend {

/// Instance values the generator substitutes in every string of a
/// contribution (hardware parameters, controllers YAML, driver parameters,
/// remappings). A contribution never knows them; it names them.
/// The component's runtime namespace (`/manyforge/<component_id>`).
inline constexpr std::string_view kLaunchNamespaceToken = "${namespace}";
/// The device address from the site settings (a machine fact).
inline constexpr std::string_view kLaunchAddressToken = "${address}";
/// The generated `<ros2_control name>` (e.g. a hardware initially inactive).
inline constexpr std::string_view kLaunchHardwareComponentToken = "${hardware_component}";

/// The `<ros2_control><hardware>` section of the generated description.
struct LaunchHardware final {
  /// `<hardware><plugin>`.
  std::string plugin;
  /// `<hardware><param>` entries, written verbatim after substitution.
  std::map<std::string, std::string, std::less<>> parameters;
  /// Each joint's command interfaces (every joint has position and velocity
  /// state interfaces). Not in CONTRACTS §12's list; the plugin's exported
  /// interfaces must be declared, and only the adapter knows them.
  std::vector<std::string> joint_command_interfaces{"position"};

  friend bool operator==(const LaunchHardware &, const LaunchHardware &) = default;
};

struct LaunchRemapping final {
  std::string from;
  std::string to;

  friend bool operator==(const LaunchRemapping &, const LaunchRemapping &) = default;
};

/// CONTRACTS §12 `LaunchContribution`.
///
/// `controllers_yaml` is a controllers template the generator expands:
/// - `/**/controller_manager:` (exactly once) becomes the component's manager;
/// - `__controller__` is the model row's `controller`;
/// - every `joints: [__joint__]` lists the arm joints;
/// - one `__joint__: {<entries>max_deceleration_on_cancel: null}` line becomes
///   one line per arm joint with the row's arrival tolerance as `goal` and the
///   Deployment's operator Stop deceleration (the template has no `goal`);
/// - `goal_time:` (exactly once, when the row sets `goal_time_s`) takes it;
/// - `__gripper_joint__` is a tool's commanded gripper joint;
/// - `__reset_writer__` names a reset-position writer whose target is the
///   Deployment's initial joint positions (Mock, Isaac; `with_reset_position_writer`);
/// - `[__command_interfaces__]` is the hardware's `joint_command_interfaces`
///   (a vendor's controllers run on every lane's hardware).
struct LaunchContribution final {
  LaunchHardware hardware;
  std::string controllers_yaml;
  std::optional<std::string> driver_parameters_yaml;
  std::vector<LaunchRemapping> remappings;
  /// The frame the component's `<frame_prefix>world` hangs from, published by
  /// the launch as an identity static transform (Isaac: `universe`); absent
  /// publishes none. Not in CONTRACTS §12's list: a lane fact the generic
  /// launch must carry, and only the adapter knows it.
  std::optional<std::string> world_parent_frame;
  /// The topic on which the hardware publishes its commanded joint positions
  /// every controller cycle (`sensor_msgs/JointState`, by joint name): a
  /// reset-position writer's target is acknowledged there (Isaac's
  /// topic-based hardware). Absent: the hardware's state loops the command
  /// back and the measured joint states acknowledge it (Mock).
  std::optional<std::string> command_echo_topic;

  friend bool operator==(const LaunchContribution &, const LaunchContribution &) = default;
};

/// CONTRACTS §12 `ModelLaneRow`: a model's `component.yaml` `lanes.<lane>`.
struct ModelLaneRow final {
  /// The adapter that runs the component on this lane (`mock`, `isaac`, `xarm`).
  std::string adapter;
  /// The arm's JointTrajectoryController, or a tool's gripper controller.
  std::string controller;
  /// Every arm joint's JTC `goal` constraint. Robots only; a tool row has none.
  std::optional<double> arrival_tolerance_rad;
  /// The JTC `goal_time` settle window; absent keeps the template's.
  std::optional<double> goal_time_s;
  /// The adapter-specific keys, as their YAML scalar text.
  std::map<std::string, std::string, std::less<>> adapter_keys;
  /// The model's own controllers file, resolved by the caller against the
  /// model folder (a model without a vendor adapter, or a deliberate override).
  std::optional<std::filesystem::path> controllers_file;
  /// A lane row naming a `vendor` key (Mock and Isaac rows of a vendor's
  /// model): that vendor family's controllers for this row (its base plus
  /// ManyForge's additions), filled by `ComponentAdapterRegistry` before the
  /// lane family makes its contribution, so every lane runs the vendor's
  /// controller configuration (plan, "Vendor driver configuration").
  std::optional<std::string> vendor_controllers_yaml;

  friend bool operator==(const ModelLaneRow &, const ModelLaneRow &) = default;
};

} // namespace manyforge::roboplan_backend
