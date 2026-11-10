#include <manyforge_roboplan/retained_ros2_control_lane_config.hpp>

#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_core/types/typed_error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>


namespace manyforge::roboplan_backend {
namespace {

// Lane facts common to every ros2_control lane's Stop: the soft Stop's
// lead-in and sample period and the tracking tolerance (`main`'s values).
constexpr double kStopTrajDeltaS = 0.1;
constexpr double kStopSamplePeriodS = 0.01;
constexpr double kTrackingErrorToleranceRad = 0.05;
constexpr double kTrackingErrorTimeoutS = 0.0;

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

// The motion component's controller profile: its compiled arm joints (source
// joints in controller order) are the controller's joints.
[[nodiscard]] const deployment::ros2_control_execution_runtime_contract::Configuration::
    ControllerProfile &
require_profile(
    const deployment::ros2_control_execution_runtime_contract::Configuration &configuration,
    const core::RuntimeParticipantBinding &motion_binding, const std::string_view lane) {
  const auto *profile = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(configuration, motion_binding.value().controllable_component_id);
  if (profile == nullptr)
    throw std::invalid_argument("retained " + std::string(lane) +
                                " ROS component has no controller profile");
  return *profile;
}

} // namespace

std::vector<std::string>
retained_ros2_control_compiled_joints(const core::RuntimeParticipantBinding &binding) {
  std::vector<std::string> result;
  result.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    if (resource.compiled_resource_name.empty() ||
        !resource.source_joint_name.has_value() ||
        resource.source_joint_name->empty()) {
      throw std::invalid_argument(
          "retained ROS binding has an incomplete coordinate map");
    }
    result.push_back(resource.compiled_resource_name);
  }
  return result;
}

std::string
retained_ros2_control_sole_group(const core::RuntimeParticipantBinding &binding,
                    const char *description) {
  if (binding.value().model_groups.size() != 1U ||
      binding.value().model_groups.front().compiled_group_name.empty()) {
    throw std::invalid_argument(std::string(description) +
                                " has no sole compiled model group");
  }
  return binding.value().model_groups.front().compiled_group_name;
}

std::string retained_ros2_control_scoped_name(
    const std::string_view runtime_namespace,
    const std::string_view relative_name) {
  std::string result(runtime_namespace);
  result.push_back('/');
  result.append(relative_name);
  return result;
}

namespace {

// EXPERIMENTAL independent opt-ins for live A/B tests on stock JTC 6.10, read
// where the scheduled (Mock) lane is configured; all default off and only "1"
// enables one. The timer's expected latency is optional milliseconds.
core::ExecutionReplacementExperimentFlagsDTO experimental_replacement_flags() {
  const auto enabled = [](const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
  };
  core::ExecutionReplacementExperimentFlagsDTO flags;
  flags.release_timer = enabled("MANYFORGE_EXPERIMENTAL_RELEASE_TIMER");
  flags.midtick_stamp = enabled("MANYFORGE_EXPERIMENTAL_MIDTICK_STAMP");
  flags.v0_payload = enabled("MANYFORGE_EXPERIMENTAL_V0_PAYLOAD");
  flags.scheduled_spring_back_fast =
      enabled("MANYFORGE_EXPERIMENTAL_SCHEDULED_SPRING_BACK_FAST");
  constexpr auto kLatency = "MANYFORGE_EXPERIMENTAL_RELEASE_TIMER_LATENCY_MS";
  if (const char *value = std::getenv(kLatency)) {
    const std::string_view text(value);
    double milliseconds{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), milliseconds);
    if (error != std::errc{} || end != text.data() + text.size() ||
        !std::isfinite(milliseconds) || milliseconds < 0.0 ||
        milliseconds > 1000.0)
      throw std::invalid_argument(std::string(kLatency) +
                                  " must be milliseconds in [0, 1000]");
    flags.expected_latency_s = milliseconds / 1000.0;
  }
  return flags;
}

/// A positive number of the model lane row's adapter keys (required).
[[nodiscard]] double row_number(const ModelLaneRow &row, const std::string_view key) {
  const auto found = row.adapter_keys.find(key);
  double value{};
  if (found != row.adapter_keys.end()) {
    const auto &text = found->second;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error == std::errc{} && end == text.data() + text.size() && std::isfinite(value) &&
        value > 0.0)
      return value;
  }
  throw std::invalid_argument("the " + row.adapter + " lane row needs a positive " +
                              std::string(key));
}

/// The written controllers' cadence; an arm's controllers always carry it.
[[nodiscard]] const ControllerLaneRates &arm_rates(const ControllerLaunchArtifacts &artifacts) {
  if (!artifacts.rates || artifacts.arm_controller_name.empty())
    throw std::invalid_argument("the component's controllers have no arm controller cadence");
  return *artifacts.rates;
}

} // namespace

RetainedRos2ControlBackendConfig
make_ros2_control_jtc_config(const std::string &runtime_namespace,
                           const std::string_view arm_controller_name,
                           const std::string_view hardware_component_name,
                           const std::vector<std::string> &transport_joints,
                           const ControllerLaneRates &rates,
                           const double stop_completion_timeout_s,
                           const core::Duration controller_state_max_age_ns,
                           const bool supports_scale) {
  RetainedRos2ControlBackendConfig config;
  config.node_name = "manyforge_ros2_control_execution_backend";
  config.node_namespace = runtime_namespace;
  config.executor_thread_count = 3U;

  const auto arm_controller =
      retained_ros2_control_scoped_name(runtime_namespace, arm_controller_name);
  auto &adapter = config.arm.adapter;
  adapter.action_name = arm_controller + "/follow_joint_trajectory";
  adapter.state_topic = arm_controller + "/controller_state";
  adapter.scale_topic = arm_controller + "/speed_scaling_input";
  auto &lane = adapter.lane_facts;
  lane.joint_names = transport_joints;
  // Only a controller that listens on `speed_scaling_input` can scale; the
  // participant, the Kernel and the Stop path read this one lane fact.
  lane.supports_scale = supports_scale;
  lane.has_measured_velocity = true;
  lane.cancel_semantics = core::ExecutionCancelSemantics::kDecelerate;
  lane.feedback_rate_hz = rates.action_monitor_rate_hz;
  lane.state_rate_hz = rates.state_publish_rate_hz;
  lane.controller_update_rate_hz = rates.controller_update_rate_hz;
  // Arrival: every lane keeps its controller file's
  // per-joint `goal` constraint (the profile row's arrival tolerance), so
  // stock JTC succeeds only when the trajectory finished with every joint
  // within it, and otherwise aborts with a goal-tolerance result.
  lane.goal_tolerance_policy = core::ExecutionGoalTolerancePolicy::kKept;
  config.arm.controller = RetainedRos2ControlArmController{
      retained_ros2_control_scoped_name(runtime_namespace, kControllerManagerNodeName),
      std::string(arm_controller_name), std::string(hardware_component_name)};

  auto &participant = config.participant;
  participant.stop.trip_authority = core::StateAuthorityClass::kCommanded;
  participant.stop.stop_traj_delta_s = kStopTrajDeltaS;
  participant.stop.sample_period_s = kStopSamplePeriodS;
  participant.stop.hard_stop_completion_timeout_ns =
      static_cast<core::Duration>(stop_completion_timeout_s * 1e9);
  // Stale controller state during a
  // motion or a Stop is a fault on this lane. The age is the component's
  // effective `measured_joints` budget (measured_joints_budget_ns).
  participant.stop.controller_state_max_age_ns = controller_state_max_age_ns;
  // Decided simulation-lane allowance and two actual CM periods;
  // live lanes still require measurement. ROS timing stays monitor-only.
  participant.stop.replacement_start_allowance_s = 0.020;
  participant.late_cycle_margin_s = 2.0 / rates.controller_update_rate_hz;
  participant.tracking = core::ExecutionTrackingPolicy{
      kTrackingErrorToleranceRad, kTrackingErrorTimeoutS};
  participant.initial_speed_factor = 1.0;
  config.observed_state_authority_profile_id = "manyforge.commanded_simulation";
  return config;
}

core::Duration retained_ros2_control_measured_joints_budget_ns(
    const RetainedMeasuredJointStateLane &lane,
    const core::ControllableComponentId &component,
    const std::span<const core::StreamParameterOverrideDTO> stream_overrides) {
  for (const auto &row : stream_overrides) {
    if (row.controllable_component_id == component &&
        row.stream_id == deployment::ros2_control_execution_runtime_contract::
                             kMeasuredJointsStreamId &&
        row.budget_ns.has_value()) {
      return *row.budget_ns;
    }
  }
  return lane.freshness_budget_ns;
}

const deployment::ros2_control_execution_runtime_contract::Configuration::
    ComponentStream &
retained_component_stream(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component,
    const std::string_view stream_id) {
  const auto *row = deployment::ros2_control_execution_runtime_contract::
      find_component_stream(configuration, component, stream_id);
  if (row == nullptr) {
    throw std::invalid_argument("retained ROS component does not declare its " +
                                std::string(stream_id) + " stream");
  }
  return *row;
}

namespace {

/// What a stale stream means is the lane's. A simulated robot's
/// or tool's stream going stale is simulator state (a warning that clears by
/// itself); on every other lane it faults the component.
std::pair<core::StreamSeverityDTO, core::StreamRecoveryDTO> retained_component_stream_meaning(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component) {
  const auto profile = std::find_if(
      configuration.controller_profiles.begin(), configuration.controller_profiles.end(),
      [&](const auto &row) { return row.controllable_component_id == component; });
  if (profile != configuration.controller_profiles.end() && profile->lane == "isaac")
    return {core::StreamSeverityDTO::kWarning, core::StreamRecoveryDTO::kAuto};
  return {core::StreamSeverityDTO::kFault, core::StreamRecoveryDTO::kAck};
}

} // namespace

RetainedMeasuredJointStateLane retained_measured_joint_state_lane(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component,
    std::string absolute_topic) {
  const auto &row = retained_component_stream(
      configuration, component,
      deployment::ros2_control_execution_runtime_contract::kMeasuredJointsStreamId);
  const auto [severity, recovery] = retained_component_stream_meaning(configuration, component);
  return RetainedMeasuredJointStateLane{
      std::move(absolute_topic), row.budget_ns,
      core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
      std::string(kMeasuredJointStateProfileId), severity, recovery, std::nullopt};
}

core::Duration retained_ros2_control_motion_state_budget_ns(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::RuntimeParticipantBinding &motion_binding) {
  const auto &path = motion_binding.value().control_path;
  if (!path.has_value() ||
      path->state_authority_class != core::StateAuthorityClass::kMeasured) {
    return 0;
  }
  return retained_component_stream(
             configuration, motion_binding.value().controllable_component_id,
             deployment::ros2_control_execution_runtime_contract::
                 kMeasuredJointsStreamId)
      .budget_ns;
}

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

std::vector<deployment::ros2_control_execution_runtime_contract::Configuration::Machine>
retained_ros2_control_armless_machines(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration) {
  std::vector<deployment::ros2_control_execution_runtime_contract::Configuration::Machine>
      machines;
  for (const auto &machine : configuration.machines)
    if (std::none_of(configuration.controller_profiles.begin(),
                     configuration.controller_profiles.end(),
                     [&](const auto &profile) {
                       return profile.owner_component_id ==
                              machine.controllable_component_id.value();
                     }))
      machines.push_back(machine);
  return machines;
}

std::string retained_ros2_control_namespace(
    const core::RuntimeParticipantBinding &binding) {
  const auto &component_id =
      binding.value().controllable_component_id.value();
  if (!is_ros_name_token(component_id)) {
    throw std::invalid_argument(
        "retained ROS controllable component id must be one bounded ROS "
        "name token");
  }
  return "/manyforge/" + component_id;
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
  // Opt-in stall-window grasp evidence (position grippers only, by decode).
  if (found->grasp_confirmation && found->grasp_confirmation->stall_window)
    config.gripper->grasp_stall_window = ros2_control_execution::Ros2GripperActionConfig::
        GraspStallWindow{found->grasp_confirmation->stall_window->min_position,
                         found->grasp_confirmation->stall_window->max_position};
  // The Deployment's Stop finish tolerance overrides the component default.
  if (found->stop_finish_tolerance)
    config.gripper->stop_finish_tolerance = *found->stop_finish_tolerance;
  // A mapped gripper's physical writes are separate admitted I/O operations.
  // Its own operation uses the existing no-output logical state semantics.
  config.gripper->output_mode = found->pneumatic_io
                                    ? core::GripperOutputMode::kNone
                                    : found->output_mode;
  // A lane-free pneumatic tool and a Real pneumatic gripper declare no
  // feedback stream: their gripper has no measured feedback.
  const bool streamless_real_pneumatic =
      config.gripper->actuation_kind == core::GripperActuationKind::kPneumatic &&
      deployment::ros2_control_execution_runtime_contract::find_component_stream(
          configuration, binding.value().controllable_component_id,
          deployment::ros2_control_execution_runtime_contract::kGripperFeedbackStreamId) ==
          nullptr;
  if (!streamless_real_pneumatic) {
    const auto &feedback = retained_component_stream(
        configuration, binding.value().controllable_component_id,
        deployment::ros2_control_execution_runtime_contract::kGripperFeedbackStreamId);
    config.gripper->state_freshness_timeout_s =
        static_cast<double>(feedback.budget_ns) / 1e9;
    std::tie(config.gripper->feedback_stream_severity,
             config.gripper->feedback_stream_recovery) =
        retained_component_stream_meaning(configuration,
                                          binding.value().controllable_component_id);
  }
  if (config.gripper->actuation_kind ==
      core::GripperActuationKind::kPneumatic) {
    config.gripper->completion_policy =
        config.gripper->output_mode == core::GripperOutputMode::kNone
            ? "no_output"
            : "output_delivery";
  }
  if (config.gripper->output_mode == core::GripperOutputMode::kCommanded) return;
  if (config.gripper->output_mode != core::GripperOutputMode::kNone ||
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
    const ModelLaneRow& row,
    const ControllerLaunchArtifacts& artifacts,
    const std::string_view runtime_namespace,
    const std::string& controller_manager) {
  if (gripper.value().resources.size() != 1U ||
      gripper.value().resources.front().compiled_resource_name.empty() ||
      !gripper.value().resources.front().source_joint_name.has_value() ||
      gripper.value().resources.front().source_joint_name->empty() ||
      !artifacts.gripper_controller_name.has_value()) {
    throw std::invalid_argument(
        "retained mock ROS gripper has no exact model-backed joint");
  }
  ros2_control_execution::Ros2GripperActionConfig gripper_config;
  // The stock parallel gripper action controller's ParallelGripperCommand action.
  gripper_config.transport_kind = "ros_gripper_command";
  gripper_config.action_name = retained_ros2_control_scoped_name(
      runtime_namespace, *artifacts.gripper_controller_name + "/gripper_cmd");
  config.arm.gripper_controller = RetainedRos2ControlArmController{
      controller_manager,
      *artifacts.gripper_controller_name,
      artifacts.hardware_component_name};
  gripper_config.command_joint_names = {
      *gripper.value().resources.front().source_joint_name};
  gripper_config.state_joint_name =
      *gripper.value().resources.front().source_joint_name;
  gripper_config.group_id =
      retained_ros2_control_sole_group(gripper, "retained mock ROS gripper binding");
  gripper_config.joint_name =
      gripper.value().resources.front().compiled_resource_name;
  gripper_config.completion_policy = "position_reached";
  // Open/closed positions and efforts drive only the simulator's effort topic
  // transport; the action transport carries the request's own target.
  gripper_config.publish_nominal_position = false;
  gripper_config.wait_for_action_server_timeout_s = 2.0;
  gripper_config.stop_finish_tolerance =
      row_number(row, "gripper_stop_finish_tolerance");
  config.gripper = std::move(gripper_config);
}

RetainedRos2ControlBackendConfig
make_retained_mock_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const ModelLaneRow &row, const ControllerLaunchArtifacts &artifacts,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_by_source_joint,
    const std::span<const core::StreamParameterOverrideDTO> stream_overrides) {
  const auto &rates = arm_rates(artifacts);
  // The host's fixed controller-manager rule, the same on every lane.
  const auto runtime_namespace = retained_ros2_control_namespace(motion_binding);
  if (!motion_binding.value().control_path.has_value() ||
      motion_binding.value().control_path->state_authority_class !=
          core::StateAuthorityClass::kCommanded) {
    throw std::invalid_argument(
        "retained mock ROS profile requires a commanded motion control path");
  }
  const auto compiled_joints = retained_ros2_control_compiled_joints(motion_binding);
  const auto &transport_joints =
      require_profile(runtime_configuration, motion_binding, "mock").joint_names;
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

  auto measured_lane = retained_measured_joint_state_lane(
      runtime_configuration, motion_binding.value().controllable_component_id,
      runtime_namespace + "/joint_states");
  auto config = make_ros2_control_jtc_config(
      runtime_namespace, artifacts.arm_controller_name,
      artifacts.hardware_component_name, transport_joints, rates,
      row_number(row, "hard_stop_completion_timeout_s"),
      retained_ros2_control_measured_joints_budget_ns(measured_lane,
          motion_binding.value().controllable_component_id, stream_overrides),
      row.runtime_scale_supported.value());
  config.arm.measured_joint_state_lane = std::move(measured_lane);
  config.arm.mock_reset_position = artifacts.mock_reset_position;
  // Only the generated Mock controllers enable stock JTC scheduled replacement.
  config.arm.adapter.lane_facts.supports_scheduled_replacement = true;
  config.arm.adapter.lane_facts.experiment = experimental_replacement_flags();
  constexpr auto kAllowance = "MANYFORGE_EXPERIMENTAL_REPLACEMENT_START_ALLOWANCE_MS";
  if (const char *value = std::getenv(kAllowance)) {
    const std::string_view text(value);
    double milliseconds{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), milliseconds);
    if (error != std::errc{} || end != text.data() + text.size() ||
        !std::isfinite(milliseconds) || milliseconds <= 0.0 || milliseconds > 200.0)
      throw std::invalid_argument(std::string(kAllowance) +
                                  " must be finite milliseconds in (0, 200]");
    config.participant.stop.replacement_start_allowance_s = milliseconds / 1000.0;
    config.arm.adapter.lane_facts.experiment.start_allowance_override = true;
    static_cast<void>(std::fprintf(stderr,
        "manyforge_replacement_start_allowance component=%.160s allowance_ms=%.17g\n",
        motion_binding.value().controllable_component_id.value().c_str(), milliseconds));
  }
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
  return config;
}

RealLaneRowGates real_lane_row_gates(const ModelLaneRow &row) {
  if (!row.arrival_tolerance_rad)
    throw std::invalid_argument("the real lane row needs arrival_tolerance_rad");
  return {row_number(row, "joint_state_freshness_s"), row_number(row, "start_tolerance_rad"),
          row_number(row, "completion_state_timeout_s"),
          row_number(row, "hard_stop_completion_timeout_s")};
}

RetainedRos2ControlBackendConfig
make_retained_real_ros2_control_backend_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &runtime_configuration,
    const ModelLaneRow &row, const ControllerLaunchArtifacts &artifacts,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_by_source_joint,
    const std::span<const core::StreamParameterOverrideDTO> stream_overrides) {
  const auto *selected = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(runtime_configuration,
                              motion_binding.value().controllable_component_id);
  if (!motion_binding.value().control_path.has_value() ||
      motion_binding.value().control_path->state_authority_class !=
          core::StateAuthorityClass::kMeasured) {
    throw std::invalid_argument(
        "retained real ROS component requires a measured motion control path");
  }
  // The measured-state gates are the vendor row's data (lanes.real).
  const auto gates = real_lane_row_gates(row);
  const auto &rates = arm_rates(artifacts);
  const auto runtime_namespace = retained_ros2_control_namespace(motion_binding);
  const auto compiled_joints = retained_ros2_control_compiled_joints(motion_binding);
  const auto &transport_joints =
      require_profile(runtime_configuration, motion_binding, "real").joint_names;
  if (compiled_joints.empty() ||
      compiled_joints.size() != transport_joints.size() ||
      std::set<std::string>(transport_joints.begin(), transport_joints.end())
              .size() != transport_joints.size()) {
    throw std::invalid_argument(
        "retained real ROS transport joints differ from its motion resources");
  }
  if (max_deceleration_on_cancel_by_source_joint.size() !=
      transport_joints.size()) {
    throw std::invalid_argument(
        "retained real ROS cancel deceleration does not cover exactly the "
        "arm joints");
  }

  // The measured lane is the component's `measured_joints` stream row,
  // exactly as on Mock and Isaac; for Real it is also the authority's source.
  auto measured_lane = retained_measured_joint_state_lane(
      runtime_configuration, motion_binding.value().controllable_component_id,
      runtime_namespace + "/joint_states");
  auto config = make_ros2_control_jtc_config(
      runtime_namespace, artifacts.arm_controller_name,
      artifacts.hardware_component_name, transport_joints, rates,
      row_number(row, "hard_stop_completion_timeout_s"),
      retained_ros2_control_measured_joints_budget_ns(measured_lane,
          motion_binding.value().controllable_component_id, stream_overrides),
      row.runtime_scale_supported.value());
  config.participant.stop.trip_authority = core::StateAuthorityClass::kMeasured;
  // The component's Deployment setting: the soft Stop lead-in starts at the
  // controller's command position (absent or `commanded`) or at the measured
  // position (`measured`).
  config.participant.stop.lead_in_anchor =
      selected != nullptr
          ? selected->stop_lead_in_anchor.value_or(core::kDefaultStopLeadInAnchor)
          : core::kDefaultStopLeadInAnchor;
  // The measured authority's start and terminal state gates and its progress
  // cursor freshness, from the profile's data row.
  config.participant.progress_freshness_budget_ns =
      static_cast<core::Duration>(gates.joint_state_freshness_s * 1e9);
  config.participant.start_tolerance_rad = gates.start_tolerance_rad;
  // The terminal gate reads the row's arrival tolerance, the same value as
  // the controllers' JTC `goal` constraint, so it re-checks what JTC's kept
  // goal tolerance already enforces. TODO: remove after the
  // live validation of the kept JTC goal tolerance on Real.
  config.participant.final_tolerance_rad = *row.arrival_tolerance_rad;
  config.participant.completion_state_timeout_ns =
      static_cast<core::Duration>(gates.completion_state_timeout_s * 1e9);
  config.observed_state_authority_profile_id =
      std::string(kMeasuredJointStateProfileId);
  config.arm.measured_joint_state_lane = std::move(measured_lane);
  auto &participant = config.participant;
  for (std::size_t index = 0; index < compiled_joints.size(); ++index) {
    const auto found =
        max_deceleration_on_cancel_by_source_joint.find(transport_joints[index]);
    if (found == max_deceleration_on_cancel_by_source_joint.end() ||
        !std::isfinite(found->second) || !(found->second > 0.0)) {
      throw std::invalid_argument(
          "retained real ROS arm joint '" + transport_joints[index] +
          "' has no positive cancel deceleration");
    }
    participant.stop.max_deceleration_on_cancel_rad_s2.emplace(
        compiled_joints[index], found->second);
  }
  return config;
}

} // namespace manyforge::roboplan_backend
