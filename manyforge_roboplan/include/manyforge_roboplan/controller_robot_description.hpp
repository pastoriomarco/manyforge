#pragma once

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/gripper.hpp>

namespace manyforge::roboplan_backend {

/// Controller `robot_description` generation for the lanes ManyForge starts
/// itself (design Q69; ROBOT_DESCRIPTION_GENERATION.md §1-§2). ROS-free: the
/// input is the model bundle's ManyForge URDF text and retained binding facts,
/// the output is URDF text. A Real component (CP8 step 2a) uses the same
/// generator with its vendor plugin; the vendor xacro is not used.
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

/// The deployment's gripper binding as seen by the offline mock lane.
struct MockGripperBinding final {
  /// The binding resource's exact model joint (`source_joint_name`). Absent
  /// for a gripper that is not model-backed.
  std::optional<std::string> source_joint_name;
  /// The retained model projection of that joint at activation (as the native
  /// lane seeds its gripper state from the retained model). Required exactly
  /// when the binding is model-backed.
  std::optional<double> projected_initial_position;
};

/// CP8 step 2a: the ros2_control hardware of one Mock or Real component as
/// data, so their shared start-up path has no per-lane branch. Isaac keeps its
/// own section until CP8 step 2b. The description generator reads the plugin,
/// parameters, command interfaces and seeding rule; the remaining fields are
/// launch data carried to the shared launch.
struct Ros2ControlHardwareSpec final {
  /// The `<hardware><plugin>` value.
  std::string plugin;
  /// Ordered `<hardware><param>` entries, written verbatim.
  std::vector<std::pair<std::string, std::string>> parameters;
  /// Each listed joint's command interfaces, in order. Every joint has
  /// position and velocity state interfaces.
  std::vector<std::string> joint_command_interfaces;
  /// True: the hardware seeds its position state from an explicit
  /// `initial_value` (Mock). False: the hardware reports measured state, so no
  /// initial value is written and none may be supplied (Real).
  bool seeds_initial_positions{};
  /// Exact bytes of a driver parameters file for `ros2_control_node`; empty
  /// passes none.
  std::string driver_parameters_yaml;
  /// `ros2_control_node` remappings (from, to), in order.
  std::vector<std::pair<std::string, std::string>> remappings;
  /// The hardware starts `inactive` through the stock controller manager
  /// `hardware_components_initial_state` instead of active.
  bool initial_state_inactive{};
};

/// Mock's hardware: `mock_components/GenericSystem` with
/// `calculate_dynamics=true`, position command, seeded from the deployment.
[[nodiscard]] Ros2ControlHardwareSpec mock_generic_system_hardware();

/// The ros2_control component lane of Mock and (CP8 step 2a) Real; the name
/// stays until CP8 step 3. Each listed joint has the hardware's command
/// interfaces and position (with an explicit `initial_value` when the hardware
/// seeds it) and velocity state.
struct MockControllerLane final {
  /// The generated element's `name` attribute.
  std::string hardware_component_name;
  /// The deployment's retained `initial_joint_positions_rad`, paired with the
  /// arm joints by order. No zero fallback: the size must match exactly.
  std::vector<double> initial_joint_positions_rad;
  std::optional<MockGripperBinding> gripper;
  /// With Commanded output, the mock profile needs a commanded gripper
  /// joint; without a model-backed gripper binding generation fails.
  bool gripper_required{};
  /// Strict selected output choice; None keeps the real model binding while
  /// emitting no gripper control interfaces. It requires an arm in this writer.
  core::GripperOutputMode gripper_output_mode{core::GripperOutputMode::kCommanded};
  Ros2ControlHardwareSpec hardware{mock_generic_system_hardware()};
};

/// Isaac lane: `topic_based_ros2_control/TopicBasedSystem` with the lane's
/// topics and fixed parameters; arm joints only (the gripper stays on its own
/// bridge).
struct IsaacControllerLane final {
  std::string hardware_component_name;
  std::string joint_commands_topic;
  std::string joint_states_topic;
  /// The selected deployment's backend-commanded initial pose, paired with the
  /// arm joints by order. TopicBasedSystem seeds both command and state
  /// interfaces from these values before JTC activation.
  std::vector<double> initial_joint_positions_rad;
};

struct ControllerRobotDescriptionRequest final {
  /// Exact ManyForge model URDF text from the resolved model bundle.
  std::string_view model_urdf;
  /// Ordered arm joints from the retained binding (never inferred). Empty
  /// only for a mock lane with a genuine model-backed gripper.
  std::vector<std::string> arm_joint_names;
  std::variant<MockControllerLane, IsaacControllerLane> lane;
  /// Structural scalar facts for an explicitly selected no-output Isaac
  /// gripper. These validate the model joint only and add no interfaces.
  /// Absence does not select an output mode; ordinary requests leave it absent.
  std::optional<MockGripperBinding> gripper_without_control_interfaces{};
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
