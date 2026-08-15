#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::roboplan_backend {
namespace {

struct IsaacProfileFacts final {
  std::string_view profile_id;
  std::string_view hardware_component_name;
  double cancel_deceleration_rad_s2;
  double soft_stop_jerk_rad_s3;
  double stop_completion_timeout_s;
  std::string_view gripper_transport_kind;
  std::string_view gripper_action_name;
  std::string_view gripper_command_topic;
  std::string_view gripper_state_topic;
  std::string_view gripper_completion_policy;
  double gripper_open_position;
  double gripper_closed_position;
  double gripper_open_effort;
  double gripper_close_effort;
  bool gripper_publish_nominal_position;
  double gripper_result_timeout_s;
};

constexpr IsaacProfileFacts kLite6Profile{
    "lite6",
    "IsaacLite6TopicBasedSystem",
    8.0,
    40.0,
    1.5,
    "isaac_joint_state_effort",
    "lite_gripper_controller/gripper_cmd",
    "/isaac_joint_commands_gripper",
    "/isaac_joint_states",
    "dwell_elapsed",
    0.0081,
    0.0,
    5.0,
    -5.0,
    true,
    5.0,
};

constexpr IsaacProfileFacts kUr10eProfile{
    "ur10e",
    "IsaacUR10eTopicBasedSystem",
    10.0,
    50.0,
    0.5,
    "ros_gripper_command",
    "robotiq_gripper_controller/gripper_cmd",
    "/isaac_gripper_state",
    "/isaac_joint_states",
    "position_reached",
    0.0,
    0.55,
    0.0,
    40.0,
    false,
    15.0,
};

[[nodiscard]] const IsaacProfileFacts &
profile_facts(const std::string_view profile_id) {
  if (profile_id == kLite6Profile.profile_id) {
    return kLite6Profile;
  }
  if (profile_id == kUr10eProfile.profile_id) {
    return kUr10eProfile;
  }
  throw std::invalid_argument("unsupported retained Isaac ROS profile");
}

[[nodiscard]] std::vector<std::string>
compiled_resources(const core::RuntimeParticipantBinding &binding) {
  std::vector<std::string> result;
  result.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    if (resource.compiled_resource_name.empty() ||
        !resource.source_joint_name.has_value() ||
        resource.source_joint_name->empty()) {
      throw std::invalid_argument(
          "retained Isaac ROS binding has an incomplete coordinate map");
    }
    result.push_back(resource.compiled_resource_name);
  }
  return result;
}

[[nodiscard]] std::vector<std::string>
transport_resources(const core::RuntimeParticipantBinding &binding) {
  std::vector<std::string> result;
  result.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    if (!resource.source_joint_name.has_value() ||
        resource.source_joint_name->empty()) {
      throw std::invalid_argument(
          "retained Isaac ROS binding has no transport joint identity");
    }
    result.push_back(*resource.source_joint_name);
  }
  return result;
}

[[nodiscard]] std::string
sole_compiled_group(const core::RuntimeParticipantBinding &binding,
                    const char *description) {
  if (binding.value().model_groups.size() != 1U ||
      binding.value().model_groups.front().compiled_group_name.empty()) {
    throw std::invalid_argument(std::string(description) +
                                " has no sole compiled model group");
  }
  return binding.value().model_groups.front().compiled_group_name;
}

[[nodiscard]] std::vector<std::string>
interfaces(const std::vector<std::string> &joints,
           const std::span<const std::string_view> names) {
  std::vector<std::string> result;
  result.reserve(joints.size() * names.size());
  for (const auto &joint : joints) {
    for (const auto name : names) {
      result.push_back(joint + "/" + std::string(name));
    }
  }
  return result;
}

[[nodiscard]] std::string controller_binding_generation(
    const IsaacProfileFacts &profile,
    const std::string_view runtime_namespace,
    const std::vector<std::string> &transport_joints) {
  std::string preimage("manyforge.isaac-controller-binding.v2");
  preimage.push_back('\0');
  preimage.append(profile.profile_id);
  preimage.push_back('\0');
  preimage.append(profile.hardware_component_name);
  preimage.push_back('\0');
  preimage.append(runtime_namespace);
  for (const auto &joint : transport_joints) {
    preimage.push_back('\0');
    preimage.append(joint);
  }
  return manyforge::content_identity::sha256_digest(preimage);
}

[[nodiscard]] bool is_ascii_alpha(const char value) noexcept {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

[[nodiscard]] bool is_ascii_digit(const char value) noexcept {
  return value >= '0' && value <= '9';
}

[[nodiscard]] bool is_ros_name_token(const std::string_view token) noexcept {
  if (token.empty() || token.size() > 255U ||
      (!is_ascii_alpha(token.front()) && token.front() != '_')) {
    return false;
  }
  return std::all_of(token.begin() + 1, token.end(), [](const char character) {
    return is_ascii_alpha(character) || is_ascii_digit(character) ||
           character == '_';
  });
}

[[nodiscard]] std::string scoped_name(
    const std::string_view runtime_namespace,
    const std::string_view relative_name) {
  std::string result(runtime_namespace);
  result.push_back('/');
  result.append(relative_name);
  return result;
}

} // namespace

std::string retained_isaac_ros_namespace(
    const core::RuntimeParticipantBinding &binding) {
  const auto &component_id =
      binding.value().controllable_component_id.value();
  if (!is_ros_name_token(component_id)) {
    throw std::invalid_argument(
        "retained Isaac controllable component id must be one bounded ROS "
        "name token");
  }
  return "/manyforge/" + component_id;
}

RetainedRos2ControlBackendConfig
make_retained_isaac_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding) {
  const auto &profile = profile_facts(runtime_configuration.isaac_profile);
  const auto runtime_namespace =
      retained_isaac_ros_namespace(motion_binding);
  if (!motion_binding.value().control_path.has_value() ||
      motion_binding.value().control_path->state_authority_class !=
          core::StateAuthorityClass::kCommanded ||
      !gripper_binding.has_value() ||
      !gripper_binding->value().control_path.has_value() ||
      gripper_binding->value().control_path->state_authority_class !=
          core::StateAuthorityClass::kCommanded) {
    throw std::invalid_argument(
        "retained Isaac ROS profile requires commanded motion and gripper "
        "control paths");
  }
  if (motion_binding.value().controllable_component_id !=
      gripper_binding->value().controllable_component_id) {
    throw std::invalid_argument(
        "retained Isaac motion and gripper bindings must address the same "
        "controllable component");
  }

  const auto compiled_joints = compiled_resources(motion_binding);
  const auto transport_joints = transport_resources(motion_binding);
  if (compiled_joints.empty() ||
      compiled_joints.size() !=
          runtime_configuration.initial_joint_positions_rad.size() ||
      compiled_joints.size() != transport_joints.size()) {
    throw std::invalid_argument(
        "retained Isaac ROS initial state differs from its motion resources");
  }
  if (std::set<std::string>(transport_joints.begin(), transport_joints.end())
          .size() != transport_joints.size()) {
    throw std::invalid_argument(
        "retained Isaac ROS transport joint identities are duplicated");
  }

  ros2_control_execution::Ros2ControlExecutionBackendConfig execution;
  execution.node_name = "manyforge_ros2_control_execution_backend";
  execution.node_namespace = runtime_namespace;
  execution.action_name = scoped_name(
      runtime_namespace, "isaac_arm_controller/follow_joint_trajectory");
  execution.joint_state_topic =
      scoped_name(runtime_namespace, "joint_states");
  execution.controller_state_topic = scoped_name(
      runtime_namespace, "isaac_arm_controller/controller_state");
  execution.required_joints = compiled_joints;
  execution.transport_joint_names = transport_joints;
  execution.observed_state_source_precedence = {
      "controller_state", "joint_state", "action_feedback"};
  execution.observed_state_authority_profile_id =
      "manyforge.commanded_simulation";
  execution.joint_state_max_age_s = 0.25;
  execution.wait_for_state_timeout_s = 1.0;
  execution.wait_for_action_server_timeout_s = 2.0;
  execution.start_tolerance_rad = 0.15;
  execution.final_tolerance_rad = 0.15;
  execution.tracking_error_tolerance_rad = 0.05;
  execution.tracking_error_timeout_s = 0.0;
  execution.stop_completion_timeout_s =
      profile.stop_completion_timeout_s;
  execution.sim_reset_motion_duration_s = 0.0;
  execution.require_fresh_state_before_planning = false;
  execution.disable_controller_path_tolerances = true;
  execution.execution_state_source_mode = "commanded_sim";
  execution.initial_joint_positions_rad =
      runtime_configuration.initial_joint_positions_rad;
  execution.command_gate_topic = scoped_name(
      runtime_namespace, "manyforge/isaac_arm_command_enable");
  execution.require_execution_admission_snapshot = true;
  // Controller-manager activity invalidates this proof immediately.  The
  // service timeout is only the bounded fallback refresh path; leave enough
  // margin for a loaded DDS executor without turning scheduler jitter into a
  // false lifecycle transition.
  execution.controller_binding_refresh_timeout_s = 0.5;
  execution.enable_jtc_speed_scaling_input = false;
  execution.decelerate_on_cancel = true;
  execution.velocity_state_interface_joints =
      std::set<std::string>(compiled_joints.begin(), compiled_joints.end());
  for (const auto &joint : compiled_joints) {
    execution.max_deceleration_on_cancel_rad_s2.emplace(
        joint, profile.cancel_deceleration_rad_s2);
    execution.soft_on_path_max_jerk_rad_s3.emplace(
        joint, profile.soft_stop_jerk_rad_s3);
  }
  execution.enable_soft_on_path_path_ramp = true;
  execution.soft_on_path_stop_traj_delta_s = 0.1;
  execution.soft_on_path_sample_period_s = 0.01;
  execution.prefer_model_anchor_velocity = true;

  ros2_control_execution::ControllerBindingContract controller;
  controller.generation_id =
      controller_binding_generation(profile, runtime_namespace,
                                    transport_joints);
  controller.controller_manager =
      scoped_name(runtime_namespace, "controller_manager");
  controller.activity_topic =
      scoped_name(runtime_namespace, "controller_manager/activity");
  controller.hardware_component_name = profile.hardware_component_name;
  controller.trajectory_controller_manager_name = "isaac_arm_controller";
  controller.trajectory_controller_ros_name =
      scoped_name(runtime_namespace, "isaac_arm_controller");
  controller.trajectory_controller_type =
      "joint_trajectory_controller/JointTrajectoryController";
  controller.follow_joint_trajectory_action = execution.action_name;
  controller.controller_state_topic = execution.controller_state_topic;
  controller.joint_state_topic = execution.joint_state_topic;
  controller.required_joints = transport_joints;
  constexpr std::array<std::string_view, 1U> kCommandInterfaces{"position"};
  constexpr std::array<std::string_view, 2U> kStateInterfaces{"position",
                                                              "velocity"};
  controller.claimed_command_interfaces =
      interfaces(transport_joints, kCommandInterfaces);
  controller.required_state_interfaces =
      interfaces(transport_joints, kStateInterfaces);
  controller.snapshot_max_age_ns = 2'000'000'000LL;
  execution.controller_binding = std::move(controller);

  const auto &gripper = *gripper_binding;
  if (gripper.value().resources.size() != 1U ||
      gripper.value().resources.front().compiled_resource_name.empty() ||
      !gripper.value().resources.front().source_joint_name.has_value() ||
      gripper.value().resources.front().source_joint_name->empty()) {
    throw std::invalid_argument(
        "retained Isaac ROS gripper has no exact coordinate mapping");
  }
  ros2_control_execution::Ros2GripperActionConfig gripper_config;
  gripper_config.action_name =
      scoped_name(runtime_namespace, profile.gripper_action_name);
  gripper_config.transport_kind = profile.gripper_transport_kind;
  gripper_config.command_topic = profile.gripper_command_topic;
  gripper_config.state_topic = profile.gripper_state_topic;
  gripper_config.command_joint_names = {
      *gripper.value().resources.front().source_joint_name};
  gripper_config.state_joint_name =
      *gripper.value().resources.front().source_joint_name;
  gripper_config.group_id =
      sole_compiled_group(gripper, "retained Isaac ROS gripper binding");
  gripper_config.joint_name =
      gripper.value().resources.front().compiled_resource_name;
  gripper_config.completion_policy = profile.gripper_completion_policy;
  gripper_config.open_position = profile.gripper_open_position;
  gripper_config.closed_position = profile.gripper_closed_position;
  gripper_config.open_effort = profile.gripper_open_effort;
  gripper_config.close_effort = profile.gripper_close_effort;
  gripper_config.publish_nominal_position =
      profile.gripper_publish_nominal_position;
  gripper_config.state_freshness_timeout_s = 1.0;
  gripper_config.wait_for_action_server_timeout_s = 2.0;
  gripper_config.result_timeout_s = profile.gripper_result_timeout_s;

  return RetainedRos2ControlBackendConfig{std::move(execution),
                                          std::move(gripper_config)};
}

} // namespace manyforge::roboplan_backend
