// The Isaac lane's settings, its LaunchContribution and the model rows.
// MANYFORGE_ISAAC_LANE_DIR is the source lane folder (set by CMake).
#include <gtest/gtest.h>

#include <manyforge_isaac_adapter/isaac_lane.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace manyforge::isaac_adapter {
namespace {

const IsaacLaneSettings &settings() {
  static const auto loaded = load_isaac_lane_settings(MANYFORGE_ISAAC_LANE_DIR);
  return loaded;
}

roboplan_backend::ModelLaneRow robotiq_row() {
  roboplan_backend::ModelLaneRow row;
  row.adapter = "isaac";
  row.controller = "robotiq_gripper_controller";
  row.adapter_keys = {{"gripper_transport", "ros_gripper_command"},
                      {"command_topic", "/isaac_gripper_state"},
                      {"state_topic", "/isaac_joint_states"},
                      {"completion_policy", "position_reached"},
                      {"open_position", "0.0"},
                      {"closed_position", "0.55"},
                      {"open_effort", "0.0"},
                      {"close_effort", "40.0"},
                      {"publish_nominal_position", "false"},
                      {"gripper_result_timeout_s", "15.0"},
                      {"gripper_stop_finish_tolerance", "0.035"},
                      {"gripper_bridge", "robotiq_2f_140.yaml"}};
  return row;
}

TEST(IsaacLaunchContribution, IsTheTopicBasedHardwareAndTheControllersTemplate) {
  roboplan_backend::ModelLaneRow row;
  row.adapter = "isaac";
  const auto contribution = isaac_launch_contribution(settings(), row, false);
  EXPECT_EQ(contribution.hardware.plugin, "topic_based_ros2_control/TopicBasedSystem");
  EXPECT_EQ(contribution.hardware.parameters,
            (std::map<std::string, std::string, std::less<>>{
                {"joint_commands_topic", "/isaac_joint_commands"},
                {"joint_states_topic", "/isaac_joint_states"},
                {"sum_wrapped_joint_states", "false"},
                {"trigger_joint_command_threshold", "-1"}}));
  EXPECT_EQ(contribution.hardware.joint_command_interfaces, std::vector<std::string>{"position"});
  EXPECT_FALSE(contribution.driver_parameters_yaml.has_value());
  EXPECT_TRUE(contribution.remappings.empty());
  // The generator's template tokens (launch_contribution.hpp).
  const auto &yaml = contribution.controllers_yaml;
  EXPECT_NE(yaml.find("/**/controller_manager:"), std::string::npos);
  EXPECT_NE(yaml.find("/**/__controller__:"), std::string::npos);
  EXPECT_NE(yaml.find("joints: [__joint__]"), std::string::npos);
  EXPECT_NE(yaml.find("__joint__: {trajectory: 0.2, max_deceleration_on_cancel: null}"),
            std::string::npos);
  EXPECT_NE(yaml.find("goal_time:"), std::string::npos);
  // The joint constraint's `goal` comes from the row (arrival_tolerance_rad).
  EXPECT_EQ(yaml.find(", goal:"), std::string::npos);
  EXPECT_EQ(isaac_joint_states_topic(settings()), "/isaac_joint_states");
  // The manager's process settings and the cell frame, as before v2.
  EXPECT_NE(yaml.find("/**/controller_manager:\n  ros__parameters:\n    lock_memory: false\n"
                      "    thread_priority: 50\n"),
            std::string::npos)
      << yaml;
  EXPECT_EQ(contribution.world_parent_frame, std::optional<std::string>("universe"));
  // Without an arm there is no reset-position writer.
  EXPECT_EQ(yaml.find("__reset_writer__"), std::string::npos);
  EXPECT_FALSE(contribution.command_echo_topic.has_value());
}

// RP-1b: an Isaac arm declares `reset_position` with Mock's stock writer;
// its write is acknowledged on the hardware's joint commands topic.
TEST(IsaacLaunchContribution, AnArmCarriesTheResetPositionWriter) {
  roboplan_backend::ModelLaneRow row;
  row.adapter = "isaac";
  const auto contribution = isaac_launch_contribution(settings(), row, true);
  const auto &yaml = contribution.controllers_yaml;
  EXPECT_EQ(yaml, roboplan_backend::with_reset_position_writer(
                      isaac_launch_contribution(settings(), row, false).controllers_yaml));
  EXPECT_NE(yaml.find("    __reset_writer__:\n"
                      "      type: forward_command_controller/ForwardCommandController\n"),
            std::string::npos);
  EXPECT_NE(yaml.find("/**/__reset_writer__:\n  ros__parameters:\n    joints: [__joint__]\n"
                      "    interface_name: position\n"),
            std::string::npos);
  EXPECT_EQ(contribution.command_echo_topic, std::optional<std::string>("/isaac_joint_commands"));
  // The lane folder documents the writer it runs.
  std::ifstream file(settings().lane_dir / "manyforge_settings.yaml");
  std::stringstream text;
  text << file.rdbuf();
  EXPECT_NE(text.str().find("reset_position_writer:\n"
                            "  type: forward_command_controller/ForwardCommandController\n"
                            "  interface_name: position\n"
                            "  acknowledged_on: joint_commands_topic\n"),
            std::string::npos);
}

TEST(IsaacLaunchContribution, AVendorsModelRunsTheVendorsControllersOnTheTopicBasedHardware) {
  roboplan_backend::ModelLaneRow row;
  row.adapter = "isaac";
  row.adapter_keys = {{"vendor", "xarm"}};
  row.vendor_controllers_yaml =
      "/**/controller_manager:\n  ros__parameters:\n    update_rate: 150\n"
      "/**/__controller__:\n  ros__parameters:\n    command_interfaces: [__command_interfaces__]\n";
  const auto contribution = isaac_launch_contribution(settings(), row, false);
  EXPECT_EQ(contribution.controllers_yaml,
            "/**/controller_manager:\n  ros__parameters:\n    lock_memory: false\n"
            "    thread_priority: 50\n    update_rate: 150\n"
            "/**/__controller__:\n  ros__parameters:\n    command_interfaces: [__command_interfaces__]\n");
  EXPECT_EQ(contribution.hardware.plugin, "topic_based_ros2_control/TopicBasedSystem");
  EXPECT_EQ(contribution.world_parent_frame, std::optional<std::string>("universe"));
}

TEST(IsaacRows, ArmFacts) {
  roboplan_backend::ModelLaneRow row;
  row.adapter = "isaac";
  row.controller = "isaac_arm_controller";
  row.arrival_tolerance_rad = 0.05;
  row.goal_time_s = 1.0;
  row.adapter_keys = {{"hard_stop_completion_timeout_s", "1.5"}};
  const auto arm = isaac_arm_facts(row);
  EXPECT_EQ(arm.controller, "isaac_arm_controller");
  EXPECT_DOUBLE_EQ(arm.hard_stop_completion_timeout_s, 1.5);
  row.adapter_keys = {{"hard_stop_completion_timeout_s", "0"}};
  EXPECT_THROW(static_cast<void>(isaac_arm_facts(row)), std::invalid_argument);
  row.adapter_keys.clear();
  EXPECT_THROW(static_cast<void>(isaac_arm_facts(row)), std::invalid_argument);
}

TEST(IsaacRows, GripperCommandToolRunsItsBridge) {
  const auto facts = isaac_gripper_facts(robotiq_row(), settings());
  EXPECT_EQ(facts.transport_kind, "ros_gripper_command");
  EXPECT_EQ(facts.action_name, "robotiq_gripper_controller/gripper_cmd");
  EXPECT_DOUBLE_EQ(facts.closed_position, 0.55);
  EXPECT_FALSE(facts.publish_nominal_position);
  EXPECT_DOUBLE_EQ(facts.stop_finish_tolerance, 0.035);
  ASSERT_TRUE(facts.bridge_parameters_file.has_value());
  EXPECT_EQ(*facts.bridge_parameters_file,
            settings().lane_dir / "gripper_bridges" / "robotiq_2f_140.yaml");
}

TEST(IsaacRows, EffortToolHasNoBridge) {
  auto row = robotiq_row();
  row.controller = "lite_gripper_controller";
  row.adapter_keys["gripper_transport"] = "isaac_joint_state_effort";
  row.adapter_keys.erase("gripper_bridge");
  const auto facts = isaac_gripper_facts(row, settings());
  EXPECT_FALSE(facts.bridge_parameters_file.has_value());
  row.adapter_keys["gripper_bridge"] = "robotiq_2f_140.yaml";
  EXPECT_THROW(static_cast<void>(isaac_gripper_facts(row, settings())), std::invalid_argument);
}

TEST(IsaacRows, InvalidToolRowsAreRefused) {
  auto unknown = robotiq_row();
  unknown.adapter_keys["gripper_transport"] = "teleport";
  EXPECT_THROW(static_cast<void>(isaac_gripper_facts(unknown, settings())), std::invalid_argument);
  auto missing_bridge = robotiq_row();
  missing_bridge.adapter_keys["gripper_bridge"] = "absent.yaml";
  EXPECT_THROW(static_cast<void>(isaac_gripper_facts(missing_bridge, settings())),
               std::invalid_argument);
  auto relative_topic = robotiq_row();
  relative_topic.adapter_keys["state_topic"] = "isaac_joint_states";
  EXPECT_THROW(static_cast<void>(isaac_gripper_facts(relative_topic, settings())),
               std::invalid_argument);
}

}  // namespace
}  // namespace manyforge::isaac_adapter
