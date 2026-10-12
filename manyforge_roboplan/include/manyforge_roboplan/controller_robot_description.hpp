#pragma once

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_core/runtime_participant_binding.hpp>

namespace manyforge::roboplan_backend {

/// Controller `robot_description` generation for the lanes ManyForge starts
/// itself (design Q69; ROBOT_DESCRIPTION_GENERATION.md §1-§2). ROS-free: the
/// input is the model bundle's ManyForge URDF text and retained binding facts,
/// the output is URDF text. Real-robot lanes keep the vendor description and
/// never use this.
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

/// Offline mock lane: `mock_components/GenericSystem` with
/// `calculate_dynamics=true`; each listed joint has a position command and
/// position (with an explicit `initial_value`) and velocity state.
struct MockControllerLane final {
  /// The generated element's `name` attribute.
  std::string hardware_component_name;
  /// The deployment's retained `initial_joint_positions_rad`, paired with the
  /// arm joints by order. No zero fallback: the size must match exactly.
  std::vector<double> initial_joint_positions_rad;
  std::optional<MockGripperBinding> gripper;
  /// The mock profile needs a commanded gripper joint; without a model-backed
  /// gripper binding generation fails.
  bool gripper_required{};
};

/// Isaac lane: `topic_based_ros2_control/TopicBasedSystem` with the lane's
/// topics and fixed parameters; arm joints only (the gripper stays on its own
/// bridge).
struct IsaacControllerLane final {
  std::string hardware_component_name;
  std::string joint_commands_topic;
  std::string joint_states_topic;
  /// The Isaac profile's fixed initial values (they carry no behaviour, Q47),
  /// paired with the arm joints by order.
  std::vector<double> initial_joint_positions_rad;
};

struct ControllerRobotDescriptionRequest final {
  /// Exact ManyForge model URDF text from the resolved model bundle.
  std::string_view model_urdf;
  /// Ordered arm joints from the retained binding (never inferred).
  std::vector<std::string> arm_joint_names;
  std::variant<MockControllerLane, IsaacControllerLane> lane;
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
