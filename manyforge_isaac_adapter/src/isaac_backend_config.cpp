#include <manyforge_isaac_adapter/isaac_backend_config.hpp>

#include <manyforge_core/types/typed_error.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::isaac_adapter {

using roboplan_backend::RetainedRos2ControlBackendConfig;

RetainedRos2ControlBackendConfig make_isaac_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding, const IsaacArmFacts &arm,
    const IsaacLaneSettings &settings, const roboplan_backend::ControllerLaneRates &rates,
    const std::map<std::string, double, std::less<>> &max_deceleration_on_cancel_by_source_joint,
    const std::span<const core::StreamParameterOverrideDTO> stream_overrides) {
  const auto &component = motion_binding.value().controllable_component_id;
  const auto runtime_namespace = roboplan_backend::retained_ros2_control_namespace(motion_binding);
  if (!motion_binding.value().control_path.has_value() ||
      motion_binding.value().control_path->state_authority_class !=
          core::StateAuthorityClass::kCommanded) {
    throw std::invalid_argument("an Isaac robot requires a commanded motion control path");
  }
  const auto compiled_joints = roboplan_backend::retained_ros2_control_compiled_joints(motion_binding);
  // The arm's source joints in controller order, compiled into its profile.
  const auto *profile = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(runtime_configuration, component);
  if (profile == nullptr)
    throw std::invalid_argument("an Isaac robot has no controller profile");
  const auto &transport_joints = profile->joint_names;
  // A single-component unit's initial state is this component's own; a
  // grouped unit's is sliced by the host before composition.
  if (compiled_joints.empty() ||
      (runtime_configuration.controller_profiles.size() == 1U &&
       compiled_joints.size() != runtime_configuration.initial_joint_positions_rad.size()) ||
      compiled_joints.size() != transport_joints.size()) {
    throw std::invalid_argument("an Isaac robot's initial state differs from its motion resources");
  }
  if (std::set<std::string>(transport_joints.begin(), transport_joints.end()).size() !=
      transport_joints.size()) {
    throw std::invalid_argument("an Isaac robot's transport joint identities are duplicated");
  }
  if (max_deceleration_on_cancel_by_source_joint.size() != transport_joints.size()) {
    throw std::invalid_argument(
        "an Isaac robot's cancel deceleration does not cover exactly the arm joints");
  }

  auto measured_lane = roboplan_backend::retained_measured_joint_state_lane(
      runtime_configuration, component, isaac_joint_states_topic(settings));
  auto config = roboplan_backend::make_ros2_control_jtc_config(
      runtime_namespace, arm.controller, roboplan_backend::kHardwareComponentName,
      transport_joints, rates, arm.hard_stop_completion_timeout_s,
      roboplan_backend::retained_ros2_control_measured_joints_budget_ns(measured_lane, component,
                                                                        stream_overrides));
  config.arm.measured_joint_state_lane = std::move(measured_lane);
  for (std::size_t index = 0; index < compiled_joints.size(); ++index) {
    const auto found = max_deceleration_on_cancel_by_source_joint.find(transport_joints[index]);
    if (found == max_deceleration_on_cancel_by_source_joint.end() ||
        !std::isfinite(found->second) || !(found->second > 0.0)) {
      throw std::invalid_argument("Isaac arm joint '" + transport_joints[index] +
                                  "' has no positive cancel deceleration");
    }
    config.participant.stop.max_deceleration_on_cancel_rad_s2.emplace(compiled_joints[index],
                                                                      found->second);
  }
  return config;
}

void configure_isaac_gripper_action(
    RetainedRos2ControlBackendConfig &config,
    const deployment::ros2_control_execution_runtime_contract::Configuration &configuration,
    const core::RuntimeParticipantBinding &gripper, const IsaacGripperFacts &facts,
    const IsaacLaneSettings &settings, const std::string_view runtime_namespace) {
  const auto &value = gripper.value();
  if (!value.control_path.has_value() ||
      value.control_path->state_authority_class != core::StateAuthorityClass::kCommanded) {
    throw std::invalid_argument("an Isaac gripper requires a commanded control path");
  }
  if (value.resources.size() != 1U || value.resources.front().compiled_resource_name.empty() ||
      !value.resources.front().source_joint_name.has_value() ||
      value.resources.front().source_joint_name->empty()) {
    throw std::invalid_argument("an Isaac gripper has no exact coordinate mapping");
  }
  const auto authored = std::find_if(
      configuration.gripper_target_configurations.begin(),
      configuration.gripper_target_configurations.end(), [&](const auto &row) {
        return row.controllable_component_id == value.controllable_component_id &&
               row.command_target_id == value.command_target_id;
      });
  if (authored != configuration.gripper_target_configurations.end() && authored->pneumatic_io)
    throw core::TypedError("pneumatic_io_unsupported_on_isaac",
                           "pneumatic_io_unsupported_on_isaac: Isaac drives the gripper itself");

  ros2_control_execution::Ros2GripperActionConfig gripper_config;
  gripper_config.action_name =
      roboplan_backend::retained_ros2_control_scoped_name(runtime_namespace, facts.action_name);
  gripper_config.transport_kind = facts.transport_kind;
  gripper_config.command_topic = facts.command_topic;
  gripper_config.state_topic = facts.state_topic;
  // The tool's gripper joint, compiled into its controller profile.
  const auto *profile =
      deployment::ros2_control_execution_runtime_contract::find_controller_profile(
          configuration, value.controllable_component_id);
  if (profile == nullptr || !profile->gripper_joint_name.has_value())
    throw std::invalid_argument("an Isaac gripper has no controller profile gripper joint");
  gripper_config.command_joint_names = {*profile->gripper_joint_name};
  gripper_config.state_joint_name = *profile->gripper_joint_name;
  gripper_config.group_id =
      roboplan_backend::retained_ros2_control_sole_group(gripper, "an Isaac gripper binding");
  gripper_config.joint_name = value.resources.front().compiled_resource_name;
  gripper_config.completion_policy = facts.completion_policy;
  gripper_config.open_position = facts.open_position;
  gripper_config.closed_position = facts.closed_position;
  gripper_config.open_effort = facts.open_effort;
  gripper_config.close_effort = facts.close_effort;
  gripper_config.publish_nominal_position = facts.publish_nominal_position;
  gripper_config.wait_for_action_server_timeout_s =
      settings.gripper_wait_for_action_server_timeout_s;
  gripper_config.result_timeout_s = facts.result_timeout_s;
  gripper_config.stop_finish_tolerance = facts.stop_finish_tolerance;
  config.gripper = std::move(gripper_config);
  roboplan_backend::configure_retained_gripper_output_mode(config, configuration, gripper);
}

}  // namespace manyforge::isaac_adapter
