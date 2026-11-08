// The Mock lane adapter family `mock`: one library,
// registered in the ament resource index `manyforge_component_adapters`. A
// Mock component runs the ros2_control lane on stock mock hardware; its
// settings and their reasons: manyforge_ros2_control_execution/lane/.

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/mock_lane.hpp>
#include <manyforge_roboplan/ros2_control_lane_family.hpp>

namespace {

using namespace manyforge::roboplan_backend;

/// The lane adapter is the component's adapter; Mock has no vendor steps.
ComponentAdapters make_mock_adapter(const ComponentAdapterContext &context) {
  return {context.lane_adapter, {}, std::nullopt, std::nullopt};
}

RetainedRos2ControlBackendConfig mock_arm_backend_config(const Ros2ControlArmLaneInput &input) {
  return make_retained_mock_ros2_control_backend_config(
      input.configuration, input.row, input.artifacts, input.motion_binding,
      input.max_deceleration_on_cancel_by_source_joint, input.stream_overrides);
}

/// A Mock tool: its own controller manager with the stock GripperCommand
/// controller, its state on its own joint states.
ControllerLaunchCommand mock_tool(const Ros2ControlToolLaneInput &input,
                                  RetainedRos2ControlBackendConfig &config) {
  const auto &value = input.gripper_binding.value();
  LaunchInstance instance;
  instance.artifacts_root = input.artifacts_root;
  instance.controllable_component_id = value.controllable_component_id.value();
  instance.model_urdf = input.profile.description;
  instance.gripper = MockGripperBinding{input.profile.gripper_joint_name, input.initial_position};
  instance.frame_prefix = input.profile.frame_prefix;
  const auto artifacts =
      write_launch_artifacts(input.row, mock_launch_contribution(input.row, false), instance);
  configure_retained_mock_gripper_action(config, input.gripper_binding, input.row, artifacts,
                                         input.runtime_namespace,
                                         input.runtime_namespace + "/controller_manager");
  configure_retained_gripper_output_mode(config, input.configuration, input.gripper_binding);
  config.arm.measured_joint_state_lane = retained_measured_joint_state_lane(
      input.configuration, value.controllable_component_id,
      input.runtime_namespace + "/joint_states");
  return ros2_control_launch_command(artifacts);
}

constexpr Ros2ControlLaneHooks kLane{true, false, &mock_arm_backend_config, &mock_tool};

constexpr ComponentAdapterFamily kFamily{kMockLaneAdapter, &make_mock_adapter,
                                         &mock_launch_contribution, &kLane, nullptr};

}  // namespace

extern "C" const manyforge::roboplan_backend::ComponentAdapterFamily *
manyforge_component_adapter_family() {
  return &kFamily;
}
