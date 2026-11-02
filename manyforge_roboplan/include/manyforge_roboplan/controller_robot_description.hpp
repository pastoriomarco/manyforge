#pragma once

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/gripper.hpp>

namespace manyforge::roboplan_backend {

/// Controller `robot_description` generation for the ros2_control components
/// ManyForge starts itself (design Q69; ROBOT_DESCRIPTION_GENERATION.md
/// §1-§2). ROS-free and vendor-free: the input is the model bundle's ManyForge
/// URDF text, retained binding facts and the adapter's hardware section, the
/// output is URDF text. No vendor xacro is used.
///
/// Rules: every `<ros2_control>` element of the model is removed; every other
/// byte of the model is kept unchanged (links, joints, limits, mimic tags,
/// meshes, inertials, safety controllers, transmissions, simulator tags and
/// unknown elements/attributes are passed through, not interpreted); exactly
/// one generated `<ros2_control type="system">` element is added before the
/// root's end tag. Joint names come only from the retained binding and must
/// name commandable (non-fixed, non-mimic) joints of the model.

/// Failure of a generation input. Generation fails closed: no partial output.
class ControllerRobotDescriptionError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// A gripper binding's model joint as the description generator sees it.
struct MockGripperBinding final {
  /// The binding resource's exact model joint (`source_joint_name`). Absent
  /// for a gripper that is not model-backed.
  std::optional<std::string> source_joint_name;
  /// The retained model projection of that joint at activation (as the native
  /// lane seeds its gripper state from the retained model). Required exactly
  /// when the binding is model-backed.
  std::optional<double> projected_initial_position;
};

/// The one generated `<ros2_control type="system">` element of a ros2_control
/// component, on every lane (Mock, Isaac, Real): the adapter's hardware
/// section (CONTRACTS §12) with the instance's joints.
struct Ros2ControlLane final {
  /// The generated element's `name` attribute.
  std::string hardware_component_name;
  /// The `<hardware><plugin>` value and its `<param>` entries, verbatim.
  std::string plugin;
  std::vector<std::pair<std::string, std::string>> parameters;
  /// Each listed joint's command interfaces, in order. Every joint has
  /// position and velocity state interfaces.
  std::vector<std::string> joint_command_interfaces;
  /// Seeds every arm joint's position state with an explicit
  /// `initial_value`, paired by order (Mock, simulators). Empty for hardware
  /// that reports measured state (Real).
  std::vector<double> initial_joint_positions_rad;
  /// A tool's commanded, model-backed gripper joint; seeded exactly when its
  /// projected position is present.
  std::optional<MockGripperBinding> gripper;
};

/// The GripperCommand bridge an Isaac tool (gripper-only) component runs in
/// its own namespace when its profile's gripper transport is the action
/// (`ros_gripper_command`). A direct-effort transport has none.
struct IsaacGripperBridgeLane final {
  /// Relative to the tool's runtime namespace.
  std::string action_name;
  std::string command_topic;
  std::string state_topic;
  /// The bridge parameters file in `manyforge_ros_bringup/config`.
  std::string parameters_file;
};

struct ControllerRobotDescriptionRequest final {
  /// Exact ManyForge model URDF text from the resolved model bundle.
  std::string_view model_urdf;
  /// Ordered arm joints from the retained binding (never inferred). Empty
  /// only for a tool with a genuine model-backed gripper.
  std::vector<std::string> arm_joint_names;
  Ros2ControlLane lane;
};

/// Generate the controller `robot_description` URDF text. Throws
/// ControllerRobotDescriptionError with a diagnostic on any invalid input.
[[nodiscard]] std::string generate_controller_robot_description(
    const ControllerRobotDescriptionRequest &request);

/// The ordered model joints of one retained binding: each resource's exact
/// `source_joint_name`, in binding order. A resource without one (not
/// model-backed) fails: names are never invented or derived from a
/// convention.
[[nodiscard]] std::vector<std::string> controller_joint_names_from_binding(
    std::span<const core::RuntimeResourceBindingDTO> resources);

} // namespace manyforge::roboplan_backend
