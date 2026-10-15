#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>

#include <manyforge_roboplan/controller_launch_artifacts.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>


namespace manyforge::roboplan_backend {
namespace {

struct IsaacProfileFacts final {
  std::string_view profile_id;
  std::string_view hardware_component_name;
  /// The lane controller YAML's per-joint `max_deceleration_on_cancel`.
  double cancel_deceleration_rad_s2;
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
  /// Controller robot_description `initial_value`s, in arm-joint binding
  /// order: the fixed values the Isaac launch used as its argument defaults
  /// (no caller overrode them). They carry no behaviour: activation holds at
  /// Isaac's measured joints and the guard stays closed until authorization
  /// (Q41, Q47, Q69).
  std::array<double, 6> initial_joint_positions_rad;
};

// Lane facts common to both Isaac profiles: the controller YAML
// (`update_rate`, `state_publish_rate`, `action_monitor_rate`, position-only
// command, position+velocity state) and the launch's topics. The arm command
// topic is the one host-side value the Manager passes as the launch argument
// and the guard derives its authorization/status topics from (Q46).
constexpr std::string_view kArmControllerName = "isaac_arm_controller";
constexpr std::string_view kArmJointCommandsTopic = "/isaac_joint_commands";
constexpr std::string_view kIsaacJointStatesTopic = "/isaac_joint_states";
/// `main`'s Isaac lane values (retained profile before the cutover).
constexpr double kJointStateMaxAgeS = 0.25;
constexpr double kStopTrajDeltaS = 0.1;
constexpr double kStopSamplePeriodS = 0.01;
constexpr double kTrackingErrorToleranceRad = 0.05;
constexpr double kTrackingErrorTimeoutS = 0.0;

constexpr IsaacProfileFacts kLite6Profile{
    "lite6",
    "IsaacLite6TopicBasedSystem",
    8.0,
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
    {0.0, 0.0, 1.57, 0.0, 0.0, 0.0},
};

constexpr IsaacProfileFacts kUr10eProfile{
    "ur10e",
    "IsaacUR10eTopicBasedSystem",
    10.0,
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
    {2.63422544, -0.875980752, 1.551074106, -2.29231544, -1.560498884,
     -2.038544566},
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

// Shared JTC wiring and retained simulation policy. Lane builders still own
// their validation, cancellation bounds, guard and gripper configuration.
[[nodiscard]] RetainedRos2ControlBackendConfig
make_simulation_jtc_config(const std::string &runtime_namespace,
                           const std::string_view arm_controller_name,
                           const std::string_view hardware_component_name,
                           const std::vector<std::string> &transport_joints,
                           const MockControllerLaneRates &rates,
                           const double stop_completion_timeout_s) {
  RetainedRos2ControlBackendConfig config;
  config.node_name = "manyforge_ros2_control_execution_backend";
  config.node_namespace = runtime_namespace;
  config.executor_thread_count = 3U;

  const auto arm_controller =
      scoped_name(runtime_namespace, arm_controller_name);
  auto &adapter = config.arm.adapter;
  adapter.action_name = arm_controller + "/follow_joint_trajectory";
  adapter.state_topic = arm_controller + "/controller_state";
  adapter.scale_topic = arm_controller + "/speed_scaling_input";
  auto &lane = adapter.lane_facts;
  lane.joint_names = transport_joints;
  lane.supports_scale = true;
  lane.has_measured_velocity = true;
  lane.cancel_semantics = core::ExecutionCancelSemantics::kDecelerate;
  lane.feedback_rate_hz = rates.action_monitor_rate_hz;
  lane.state_rate_hz = rates.state_publish_rate_hz;
  lane.controller_update_rate_hz = rates.controller_update_rate_hz;
  lane.goal_tolerance_policy = core::ExecutionGoalTolerancePolicy::kErased;
  config.arm.controller = RetainedRos2ControlArmController{
      scoped_name(runtime_namespace, kControllerManagerNodeName),
      std::string(arm_controller_name), std::string(hardware_component_name)};

  auto &participant = config.participant;
  participant.stop.trip_authority = core::StateAuthorityClass::kCommanded;
  participant.stop.stop_traj_delta_s = kStopTrajDeltaS;
  participant.stop.sample_period_s = kStopSamplePeriodS;
  participant.stop.hard_stop_completion_timeout_ns =
      static_cast<core::Duration>(stop_completion_timeout_s * 1e9);
  // Q59 (`main`'s `joint_state_max_age_s`): stale controller state during a
  // motion or a Stop is a fault on this lane.
  participant.stop.controller_state_max_age_ns =
      static_cast<core::Duration>(kJointStateMaxAgeS * 1e9);
  // Q55: decided simulation-lane allowance and two actual CM periods;
  // live lanes still require measurement. Q54 keeps ROS timing monitor-only.
  participant.stop.replacement_start_allowance_s = 0.020;
  participant.late_cycle_margin_s = 2.0 / rates.controller_update_rate_hz;
  participant.tracking = core::ExecutionTrackingPolicy{
      kTrackingErrorToleranceRad, kTrackingErrorTimeoutS};
  participant.initial_speed_factor = 1.0;
  config.observed_state_authority_profile_id = "manyforge.commanded_simulation";
  return config;
}

} // namespace

core::Result<core::MeasuredStateSourceDescriptor>
make_retained_ros2_control_measured_state_descriptor(
    const core::RuntimeParticipantBinding &physical_binding,
    const core::ActiveControlConfigurationDTO &configuration,
    const RetainedMeasuredJointStateLane &lane) {
  using Result = core::Result<core::MeasuredStateSourceDescriptor>;
  const auto &value = physical_binding.value();
  const auto unavailable = [&](std::string reason) {
    return Result::Err(core::ErrorDTO{
        "configured_robot_measured_state_unavailable",
        "Measured source has no exact qualified physical route",
        {{"component_id", value.controllable_component_id.value()},
         {"reason", std::move(reason)}}});
  };
  if (!value.control_path || lane.absolute_topic.empty() ||
      lane.absolute_topic.front() != '/' || lane.freshness_budget_ns <= 0 ||
      lane.measured_state_authority_id.value() !=
          kMeasuredJointStateAuthorityId ||
      lane.measured_source_profile_id != kMeasuredJointStateProfileId)
    return unavailable("measured_lane_source_unqualified");
  const auto &path = *value.control_path;
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      configuration.control_domain_id != path.control_domain_id ||
      configuration.control_domain_revision != path.control_domain_revision ||
      configuration.activation_set_id != path.activation_set_id ||
      configuration.controller_topology_id != path.controller_topology_id ||
      configuration.topology_revision != path.topology_revision ||
      configuration.source_runtime_unit_id != path.provider_runtime_unit_id ||
      configuration.source_runtime_epoch != path.provider_runtime_epoch ||
      configuration.active_control_configuration_id !=
          path.active_control_configuration_id ||
      configuration.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      !std::any_of(configuration.binding_routes.begin(),
                   configuration.binding_routes.end(), [&](const auto &route) {
                     return route.control_binding_id ==
                                path.control_binding_id &&
                            route.control_binding_revision ==
                                path.control_binding_revision &&
                            route.control_route_id == path.control_route_id;
                   }))
    return unavailable("measured_source_route_identity_mismatch");
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_names;
  std::vector<core::CoordinateKind> kinds;
  std::vector<std::string> source_names;
  resources.reserve(value.resources.size());
  compiled_names.reserve(value.resources.size());
  kinds.reserve(value.resources.size());
  source_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    if (!resource.source_joint_name || resource.source_joint_name->empty())
      return unavailable("measured_source_coordinate_mapping_invalid");
    resources.push_back(resource.resource_id);
    compiled_names.push_back(resource.compiled_resource_name);
    kinds.push_back(resource.coordinate_kind);
    source_names.push_back(*resource.source_joint_name);
  }
  core::ObservedStateQueryContextDTO query{
      value.controllable_component_id,
      value.command_target_id,
      value.capability_id,
      path.control_domain_id,
      path.control_domain_revision,
      path.control_binding_id,
      path.control_binding_revision,
      path.controller_topology_id,
      path.topology_revision,
      path.control_route_id,
      std::move(resources),
      std::move(compiled_names),
      std::move(kinds),
      path.control_interface_contract_id,
      path.control_interface_revision,
      configuration.active_control_configuration_id,
      configuration.active_control_configuration_generation,
      path.provider_instance_id,
      path.provider_manifest_id,
      path.provider_manifest_revision,
      path.provider_runtime_epoch,
      lane.measured_state_authority_id,
      core::StateAuthorityClass::kMeasured,
      lane.freshness_budget_ns};
  core::MeasuredStateSourceDescriptor descriptor{
      {std::move(query),
       {{core::ObservedRobotStateSourceKind::kJointState, lane.absolute_topic,
         core::ObservedRobotStateAuthorityProfile::kMeasured,
         lane.freshness_budget_ns, lane.measured_source_profile_id}},
       configuration.active_control_configuration_generation.value()},
      std::move(source_names)};
  const auto valid = descriptor.validate();
  if (!valid.ok())
    return Result::Err(valid.error());
  return Result::Ok(std::move(descriptor));
}

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

IsaacControllerLane
retained_isaac_controller_lane(const std::string_view isaac_profile) {
  const auto &profile = profile_facts(isaac_profile);
  return IsaacControllerLane{
      std::string(profile.hardware_component_name),
      std::string(kArmJointCommandsTopic), std::string(kIsaacJointStatesTopic),
      std::vector<double>(profile.initial_joint_positions_rad.begin(),
                          profile.initial_joint_positions_rad.end())};
}

std::vector<std::string>
retained_isaac_scene_required_topics(const std::string_view isaac_profile) {
  const auto &profile = profile_facts(isaac_profile);
  return {"/clock", std::string(kArmJointCommandsTopic),
          std::string(kIsaacJointStatesTopic),
          std::string(profile.gripper_command_topic)};
}

RetainedRos2ControlBackendConfig
make_retained_isaac_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding) {
  // Q68: the component's own controller profile inside the one ROS unit.
  const auto *selected = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(runtime_configuration,
                              motion_binding.value().controllable_component_id);
  if (selected == nullptr) {
    throw std::invalid_argument(
        "retained ROS component has no controller profile");
  }
  if (!selected->isaac_profile.has_value()) {
    throw std::invalid_argument(
        "retained ROS component selects the offline mock lane; its "
        "configuration is make_retained_mock_ros2_control_backend_config");
  }
  const auto &profile = profile_facts(*selected->isaac_profile);
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
  // A single-component unit's initial state is this component's own; a
  // grouped unit's is sliced by the host before composition.
  if (compiled_joints.empty() ||
      (runtime_configuration.controller_profiles.size() == 1U &&
       compiled_joints.size() !=
           runtime_configuration.initial_joint_positions_rad.size()) ||
      compiled_joints.size() != transport_joints.size()) {
    throw std::invalid_argument(
        "retained Isaac ROS initial state differs from its motion resources");
  }
  if (std::set<std::string>(transport_joints.begin(), transport_joints.end())
          .size() != transport_joints.size()) {
    throw std::invalid_argument(
        "retained Isaac ROS transport joint identities are duplicated");
  }

  const auto rates = isaac_controller_lane_rates(*selected->isaac_profile);
  auto config = make_simulation_jtc_config(
      runtime_namespace, kArmControllerName, profile.hardware_component_name,
      transport_joints, rates, profile.stop_completion_timeout_s);
  auto &participant = config.participant;
  for (const auto &joint : compiled_joints) {
    participant.stop.max_deceleration_on_cancel_rad_s2.emplace(
        joint, profile.cancel_deceleration_rad_s2);
  }
  config.isaac_guard = RetainedIsaacArmGuardBindingConfig{
      std::string(kArmJointCommandsTopic), std::string(kIsaacJointStatesTopic),
      static_cast<core::Duration>(kJointStateMaxAgeS * 1e9),
      static_cast<core::Duration>(1e9 / rates.state_publish_rate_hz)};
  config.arm.measured_joint_state_lane = RetainedMeasuredJointStateLane{
      config.isaac_guard->joint_states_topic,
      config.participant.stop.controller_state_max_age_ns,
      core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
      std::string(kMeasuredJointStateProfileId)};

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

  config.gripper = std::move(gripper_config);
  configure_retained_gripper_output_mode(config, runtime_configuration, gripper);
  return config;
}

core::GripperActuationKind retained_ros2_control_gripper_actuation_kind(
    const deployment::ros2_control_execution_runtime_contract::Configuration& configuration,
    const core::RuntimeParticipantBinding& binding) {
  const auto matches = [&](const auto& row) {
    return row.controllable_component_id == binding.value().controllable_component_id &&
           row.command_target_id == binding.value().command_target_id;
  };
  const auto found = std::find_if(configuration.gripper_target_configurations.begin(),
      configuration.gripper_target_configurations.end(), matches);
  if (found == configuration.gripper_target_configurations.end() ||
      std::count_if(configuration.gripper_target_configurations.begin(),
          configuration.gripper_target_configurations.end(), matches) != 1)
    throw std::invalid_argument("retained ROS gripper requires one exact authored actuation row");
  if (found->actuation_kind == "pneumatic") return core::GripperActuationKind::kPneumatic;
  if (found->actuation_kind == "position_commanded") return core::GripperActuationKind::kPositionCommanded;
  throw std::invalid_argument("retained ROS gripper actuation kind is not supported");
}

void configure_retained_gripper_output_mode(
    RetainedRos2ControlBackendConfig& config,
    const deployment::ros2_control_execution_runtime_contract::Configuration& configuration,
    const core::RuntimeParticipantBinding& binding) {
  const auto matches = [&](const auto& row) {
    return row.controllable_component_id == binding.value().controllable_component_id &&
           row.command_target_id == binding.value().command_target_id;
  };
  const auto found = std::find_if(configuration.gripper_target_configurations.begin(),
      configuration.gripper_target_configurations.end(), matches);
  if (!config.gripper || found == configuration.gripper_target_configurations.end() ||
      std::count_if(configuration.gripper_target_configurations.begin(),
          configuration.gripper_target_configurations.end(), matches) != 1)
    throw std::invalid_argument("retained ROS gripper requires one exact authored output row");
  config.gripper->actuation_kind = retained_ros2_control_gripper_actuation_kind(configuration, binding);
  config.gripper->output_mode = found->output_mode;
  if (config.gripper->actuation_kind ==
      core::GripperActuationKind::kPneumatic) {
    config.gripper->result_timeout_s =
        found->delivery_result_timeout_s.value_or(1.0);
    config.gripper->completion_policy =
        found->output_mode == core::GripperOutputMode::kNone
            ? "no_output"
            : "output_delivery";
  }
  if (found->output_mode == core::GripperOutputMode::kCommanded) return;
  if (found->output_mode != core::GripperOutputMode::kNone ||
      config.gripper->actuation_kind != core::GripperActuationKind::kPneumatic)
    throw std::invalid_argument("retained ROS no output requires its strict pneumatic target");
  config.gripper->transport_kind.clear();
  config.gripper->action_name.clear();
  config.gripper->command_topic.clear();
  config.gripper->command_joint_names.clear();
  config.gripper->completion_policy = "no_output";
  config.arm.gripper_controller.reset();
}

void configure_retained_mock_gripper_action(
    RetainedRos2ControlBackendConfig& config,
    const core::RuntimeParticipantBinding& gripper,
    const MockControllerProfileFacts& profile,
    const std::string_view runtime_namespace,
    const std::string& controller_manager) {
  if (gripper.value().resources.size() != 1U ||
      gripper.value().resources.front().compiled_resource_name.empty() ||
      !gripper.value().resources.front().source_joint_name.has_value() ||
      gripper.value().resources.front().source_joint_name->empty() ||
      !profile.gripper_controller_name.has_value()) {
    throw std::invalid_argument(
        "retained mock ROS gripper has no exact model-backed joint");
  }
  ros2_control_execution::Ros2GripperActionConfig gripper_config;
  // Q74/Q79: the stock GripperActionController's GripperCommand action.
  gripper_config.transport_kind = "ros_gripper_command";
  gripper_config.action_name = scoped_name(
      runtime_namespace, *profile.gripper_controller_name + "/gripper_cmd");
  config.arm.gripper_controller = RetainedRos2ControlArmController{
      controller_manager,
      *profile.gripper_controller_name,
      std::string(profile.hardware_component_name)};
  gripper_config.command_joint_names = {
      *gripper.value().resources.front().source_joint_name};
  gripper_config.state_joint_name =
      *gripper.value().resources.front().source_joint_name;
  gripper_config.group_id =
      sole_compiled_group(gripper, "retained mock ROS gripper binding");
  gripper_config.joint_name =
      gripper.value().resources.front().compiled_resource_name;
  gripper_config.completion_policy = "position_reached";
  // Open/closed positions and efforts drive only the Isaac effort topic
  // transport; the action transport carries the request's own target.
  gripper_config.publish_nominal_position = false;
  gripper_config.state_freshness_timeout_s = 1.0;
  gripper_config.wait_for_action_server_timeout_s = 2.0;
  gripper_config.result_timeout_s = profile.gripper_result_timeout_s;
  config.gripper = std::move(gripper_config);
}

RetainedRos2ControlBackendConfig
make_retained_mock_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_by_source_joint) {
  const auto *selected = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(runtime_configuration,
                              motion_binding.value().controllable_component_id);
  if (selected == nullptr) {
    throw std::invalid_argument(
        "retained ROS component has no controller profile");
  }
  if (!selected->mock_profile.has_value()) {
    throw std::invalid_argument(
        "retained ROS component does not select the offline mock lane");
  }
  const auto profile = mock_controller_profile_facts(*selected->mock_profile);
  const auto rates = mock_controller_lane_rates(*selected->mock_profile);
  // Q66: the host's fixed controller-manager rule, as on Isaac.
  const auto runtime_namespace = retained_isaac_ros_namespace(motion_binding);
  if (!motion_binding.value().control_path.has_value() ||
      motion_binding.value().control_path->state_authority_class !=
          core::StateAuthorityClass::kCommanded) {
    throw std::invalid_argument(
        "retained mock ROS profile requires a commanded motion control path");
  }
  if (profile.gripper_required != gripper_binding.has_value()) {
    throw std::invalid_argument(
        profile.gripper_required
            ? "retained mock ROS profile requires its gripper binding"
            : "retained mock ROS profile has no gripper for a gripper binding");
  }
  if (gripper_binding.has_value() &&
      (!gripper_binding->value().control_path.has_value() ||
       gripper_binding->value().control_path->state_authority_class !=
           core::StateAuthorityClass::kCommanded ||
       gripper_binding->value().controllable_component_id !=
           motion_binding.value().controllable_component_id)) {
    throw std::invalid_argument(
        "retained mock ROS gripper must be a commanded path of the same "
        "controllable component");
  }

  const auto compiled_joints = compiled_resources(motion_binding);
  const auto transport_joints = transport_resources(motion_binding);
  if (compiled_joints.empty() ||
      (runtime_configuration.controller_profiles.size() == 1U &&
       compiled_joints.size() !=
           runtime_configuration.initial_joint_positions_rad.size()) ||
      compiled_joints.size() != transport_joints.size()) {
    throw std::invalid_argument(
        "retained mock ROS initial state differs from its motion resources");
  }
  if (std::set<std::string>(transport_joints.begin(), transport_joints.end())
          .size() != transport_joints.size()) {
    throw std::invalid_argument(
        "retained mock ROS transport joint identities are duplicated");
  }
  if (max_deceleration_on_cancel_by_source_joint.size() !=
      transport_joints.size()) {
    throw std::invalid_argument(
        "retained mock ROS cancel deceleration does not cover exactly the "
        "arm joints");
  }

  auto config = make_simulation_jtc_config(
      runtime_namespace, profile.arm_controller_name,
      profile.hardware_component_name, transport_joints, rates,
      profile.hard_stop_completion_timeout_s);
  config.arm.measured_joint_state_lane = RetainedMeasuredJointStateLane{
      runtime_namespace + "/joint_states",
      config.participant.stop.controller_state_max_age_ns,
      core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
      std::string(kMeasuredJointStateProfileId)};
  auto &participant = config.participant;
  for (std::size_t index = 0; index < compiled_joints.size(); ++index) {
    const auto found =
        max_deceleration_on_cancel_by_source_joint.find(transport_joints[index]);
    if (found == max_deceleration_on_cancel_by_source_joint.end() ||
        !std::isfinite(found->second) || !(found->second > 0.0)) {
      throw std::invalid_argument(
          "retained mock ROS arm joint '" + transport_joints[index] +
          "' has no positive cancel deceleration");
    }
    participant.stop.max_deceleration_on_cancel_rad_s2.emplace(
        compiled_joints[index], found->second);
  }
  if (gripper_binding.has_value()) {
    configure_retained_mock_gripper_action(config, *gripper_binding, profile,
        runtime_namespace, config.arm.controller.controller_manager);
    configure_retained_gripper_output_mode(config, runtime_configuration, *gripper_binding);
  }
  return config;
}

} // namespace manyforge::roboplan_backend
