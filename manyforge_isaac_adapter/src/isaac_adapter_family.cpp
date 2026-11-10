// The Isaac lane adapter family `isaac`: one
// library, registered in the ament resource index
// `manyforge_component_adapters`. The execution host reaches every Isaac
// launch and backend hook only through this family; it neither links nor
// includes this package.

#include <manyforge_isaac_adapter/isaac_backend_config.hpp>
#include <manyforge_isaac_adapter/isaac_lane.hpp>
#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/ros2_control_lane_family.hpp>

#include <optional>

namespace {

using namespace manyforge::roboplan_backend;
namespace isaac = manyforge::isaac_adapter;
namespace contract = manyforge::deployment::ros2_control_execution_runtime_contract;

[[nodiscard]] isaac::IsaacLaneSettings lane_settings() {
  return isaac::load_isaac_lane_settings(isaac::installed_isaac_lane_dir());
}

/// The lane adapter is the component's adapter: Isaac has no vendor steps.
ComponentAdapters make_isaac_adapter(const ComponentAdapterContext &context) {
  return {context.lane_adapter, {}, std::nullopt, std::nullopt};
}

/// Every Isaac robot: the lane's topic-based hardware with its vendor's
/// controllers or the lane's template, and for an arm the reset-position
/// writer (a tool runs its own launch, `isaac_tool`).
LaunchContribution isaac_contribution(const ModelLaneRow &row, const bool arm) {
  return isaac::isaac_launch_contribution(lane_settings(), row, arm);
}

/// The arm's execution configuration with its launch's reset-position
/// writer: the adapter declares `reset_position`.
RetainedRos2ControlBackendConfig isaac_arm_backend_config(const Ros2ControlArmLaneInput &input) {
  auto config = isaac::make_isaac_ros2_control_backend_config(
      input.configuration, input.motion_binding, isaac::isaac_arm_facts(input.row),
      lane_settings(), input.artifacts.rates.value(),
      input.row.runtime_scale_supported.value(),
      input.max_deceleration_on_cancel_by_source_joint, input.stream_overrides);
  config.arm.mock_reset_position = input.artifacts.mock_reset_position;
  return config;
}

/// An Isaac tool: its joints stay in the robot's articulation; its launch
/// publishes its description and runs the lane's gripper bridge (an action
/// transport), fed its own `gripper_feedback` budget; it reads its joints
/// from the shared Isaac joint states.
ControllerLaunchCommand isaac_tool(const Ros2ControlToolLaneInput &input,
                                   RetainedRos2ControlBackendConfig &config) {
  const auto &component = input.gripper_binding.value().controllable_component_id;
  const auto settings = lane_settings();
  const auto facts = isaac::isaac_gripper_facts(input.row, settings);
  isaac::configure_isaac_gripper_action(config, input.configuration, input.gripper_binding,
                                        facts, settings, input.runtime_namespace);
  config.arm.measured_joint_state_lane = retained_measured_joint_state_lane(
      input.configuration, component, isaac::isaac_joint_states_topic(settings));
  std::optional<IsaacGripperBridgeLane> bridge;
  if (facts.bridge_parameters_file)
    bridge = IsaacGripperBridgeLane{facts.action_name, facts.command_topic, facts.state_topic,
                                    facts.bridge_parameters_file->string()};
  const auto budget_s =
      bridge ? std::optional<double>(
                   static_cast<double>(retained_component_stream(
                                           input.configuration, component,
                                           contract::kGripperFeedbackStreamId)
                                           .budget_ns) /
                   1e9)
             : std::nullopt;
  return isaac_gripper_launch_command(
      write_isaac_gripper_launch_artifacts(input.artifacts_root, component.value(), input.row,
                                           input.profile.description,
                                           input.profile.frame_prefix),
      bridge, budget_s);
}

/// The articulation starts at the Deployment's initial pose: the generator
/// seeds the topic-based hardware's command and state interfaces.
constexpr Ros2ControlLaneHooks kLane{true, true, &isaac_arm_backend_config, &isaac_tool};

constexpr ComponentAdapterFamily kFamily{"isaac", &make_isaac_adapter, &isaac_contribution,
                                         &kLane, nullptr};

}  // namespace

extern "C" const manyforge::roboplan_backend::ComponentAdapterFamily *
manyforge_component_adapter_family() {
  return &kFamily;
}
