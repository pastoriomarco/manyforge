#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/mock_lane.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <manyforge_content_identity/content_identity.hpp>

namespace {

using namespace manyforge::roboplan_backend;

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

[[nodiscard]] std::filesystem::path repository(const std::string &relative) {
  return std::filesystem::path(MANYFORGE_TEST_REPOSITORY_ROOT) / relative;
}

[[nodiscard]] std::size_t count(const std::string &text, const std::string &needle) {
  std::size_t found = 0U;
  for (auto at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size()))
    ++found;
  return found;
}

class TemporaryRoot final {
public:
  TemporaryRoot() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-launch-artifacts-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TemporaryRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

const std::vector<std::string> kLite6Arm{"joint1", "joint2", "joint3",
                                         "joint4", "joint5", "joint6"};
const std::string kIdentity = "sha256:" + std::string(64, 'c');

[[nodiscard]] std::map<std::string, double, std::less<>> decelerations(double value) {
  std::map<std::string, double, std::less<>> result;
  for (const auto &joint : kLite6Arm) result.emplace(joint, value);
  return result;
}

/// The Lite 6's `lanes.mock` row as the registry hands it to the Mock lane:
/// `vendor: xarm`, with the xArm family's controllers in their shape (the
/// vendor base plus ManyForge's additions, the hardware's command interfaces
/// left to the generator).
[[nodiscard]] ModelLaneRow lite6_mock_row() {
  ModelLaneRow row;
  row.adapter = "mock";
  row.controller = "lite6_traj_controller";
  row.arrival_tolerance_rad = 0.01;
  row.adapter_keys = {{"vendor", "xarm"},
                      {"vendor_model", "lite6"},
                      {"hard_stop_completion_timeout_s", "1.5"},
                      {"gripper_result_timeout_s", "5.0"}};
  row.vendor_controllers_yaml =
      "/**/controller_manager:\n"
      "  ros__parameters:\n"
      "    update_rate: 150\n"
      "    joint_state_broadcaster:\n"
      "      type: joint_state_broadcaster/JointStateBroadcaster\n"
      "    __controller__:\n"
      "      type: joint_trajectory_controller/JointTrajectoryController\n"
      "    enforce_command_limits: false\n"
      "/**/__controller__:\n"
      "  ros__parameters:\n"
      "    command_interfaces: [__command_interfaces__]\n"
      "    state_interfaces: [position, velocity]\n"
      "    joints: [__joint__]\n"
      "    constraints:\n"
      "      goal_time: 0.5\n"
      "      stopped_velocity_tolerance: 0.0\n"
      "      __joint__: {trajectory: 1.0, max_deceleration_on_cancel: null}\n"
      "      decelerate_on_cancel: true\n"
      "    interface_name: position\n"
      "    state_publish_rate: 25.0\n"
      "    action_monitor_rate: 20.0\n";
  return row;
}

/// The UR10e's `lanes.mock` row: a model without a vendor adapter names its
/// own controllers file.
[[nodiscard]] ModelLaneRow ur10e_mock_row() {
  ModelLaneRow row;
  row.adapter = "mock";
  row.controller = "joint_trajectory_controller";
  row.arrival_tolerance_rad = 0.05;
  row.controllers_file =
      repository("manyforge_example_models/models/ur10e/mock_controllers.yaml");
  return row;
}

[[nodiscard]] LaunchInstance lite6_instance(const TemporaryRoot &root,
                                            const std::string &model_urdf) {
  LaunchInstance instance;
  instance.artifacts_root = root.path();
  instance.controllable_component_id = "lite6_robot";
  instance.model_urdf = model_urdf;
  instance.arm_joint_names = kLite6Arm;
  instance.initial_joint_positions_rad = {0.0, 0.0, 1.57, 0.0, 0.0, 0.0};
  instance.max_deceleration_on_cancel_rad_s2 = decelerations(8.0);
  instance.component_runtime_identity = kIdentity;
  return instance;
}

/// A vendor adapter's contribution in the generic shape (the xArm
/// adapter's: vendor plugin, the device address, driver parameters and
/// remappings, an inactive hardware start).
[[nodiscard]] LaunchContribution vendor_contribution() {
  LaunchContribution contribution;
  contribution.hardware.plugin = "uf_robot_hardware/UFRobotSystemHardware";
  contribution.hardware.parameters = {{"robot_ip", "R${address}"},
                                      {"hw_ns", "xarm"},
                                      {"report_type", "rich"}};
  contribution.hardware.joint_command_interfaces = {"position", "velocity"};
  contribution.controllers_yaml =
      "/**/controller_manager:\n"
      "  ros__parameters:\n"
      "    update_rate: 150\n"
      "    hardware_components_initial_state:\n"
      "      inactive: [${hardware_component}]\n"
      "    joint_state_broadcaster:\n"
      "      type: joint_state_broadcaster/JointStateBroadcaster\n"
      "    __controller__:\n"
      "      type: joint_trajectory_controller/JointTrajectoryController\n"
      "\n"
      "/**/__controller__:\n"
      "  ros__parameters:\n"
      "    joints: [__joint__]\n"
      "    command_interfaces: [position, velocity]\n"
      "    state_publish_rate: 25.0\n"
      "    action_monitor_rate: 10.0\n"
      "    constraints:\n"
      "      goal_time: 0.5\n"
      "      __joint__: {trajectory: 1.0, max_deceleration_on_cancel: null}\n";
  contribution.driver_parameters_yaml =
      "${namespace}/ufactory_driver:\n  ros__parameters:\n    services:\n"
      "      clean_error: true\n";
  contribution.remappings = {
      {"/controller_manager/list_controllers",
       "${namespace}/controller_manager/list_controllers"}};
  return contribution;
}

TEST(LaunchArtifacts, MockRowWritesTheDescriptionControllersAndWriter) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  const auto row = lite6_mock_row();
  const auto written =
      write_launch_artifacts(row, mock_launch_contribution(row, true), lite6_instance(root, urdf));

  EXPECT_EQ(written.lane, ControllerLaunchLane::kRos2Control);
  EXPECT_EQ(written.controller_profile, "mock");
  EXPECT_EQ(written.runtime_namespace, "/manyforge/lite6_robot");
  EXPECT_EQ(written.arm_controller_name, "lite6_traj_controller");
  EXPECT_FALSE(written.gripper_controller_name.has_value());
  EXPECT_EQ(written.hardware_component_name, kHardwareComponentName);
  EXPECT_TRUE(written.driver_parameters_file.empty());

  const auto description = read_file(written.robot_description_file);
  EXPECT_NE(description.find("<plugin>mock_components/GenericSystem</plugin>"),
            std::string::npos);
  EXPECT_NE(description.find("<param name=\"calculate_dynamics\">true</param>"),
            std::string::npos);
  EXPECT_EQ(count(description, "<command_interface name=\"position\"/>"), 6U);
  EXPECT_EQ(count(description, "<param name=\"initial_value\">"), 6U);
  EXPECT_NE(description.find("<param name=\"initial_value\">1.5700000000000001</param>"),
            std::string::npos);

  const auto &controllers = written.controllers_yaml;
  EXPECT_EQ(read_file(written.controllers_file), controllers);
  EXPECT_EQ(written.controllers_sha256, manyforge::content_identity::sha256_digest(controllers));
  EXPECT_NE(controllers.find("/manyforge/lite6_robot/controller_manager:\n"), std::string::npos);
  EXPECT_NE(controllers.find("/**/lite6_traj_controller:\n"), std::string::npos);
  EXPECT_NE(controllers.find("joints: [joint1, joint2, joint3, joint4, joint5, joint6]"),
            std::string::npos);
  // The vendor's arm claims the mock hardware's command interfaces.
  EXPECT_NE(controllers.find("command_interfaces: [position]\n"), std::string::npos);
  EXPECT_NE(controllers.find(
                "joint3: {trajectory: 1.0, goal: 0.01, max_deceleration_on_cancel: 8.0}"),
            std::string::npos);
  EXPECT_EQ(count(controllers, "__"), count(controllers, "ros__parameters"));
  EXPECT_EQ(controllers.find('#'), std::string::npos);

  ASSERT_TRUE(written.rates.has_value());
  EXPECT_EQ(written.rates->controller_update_rate_hz, 150.0);
  EXPECT_EQ(written.rates->state_publish_rate_hz, 25.0);
  EXPECT_EQ(written.rates->action_monitor_rate_hz, 20.0);

  // Mock keeps reset_position: one stock writer, named by its exact target.
  ASSERT_TRUE(written.mock_reset_position.has_value());
  const auto &writer = *written.mock_reset_position;
  EXPECT_EQ(writer.joint_names, kLite6Arm);
  EXPECT_EQ(writer.positions, (std::vector<double>{0.0, 0.0, 1.57, 0.0, 0.0, 0.0}));
  EXPECT_EQ(writer.controller_name, "manyforge_reset_position_" + writer.target_digest.substr(7U));
  EXPECT_EQ(writer.commands_topic,
            "/manyforge/lite6_robot/" + writer.controller_name + "/commands");
  // Mock's write is acknowledged by its hardware's loopback state.
  EXPECT_FALSE(writer.command_echo_topic.has_value());
  EXPECT_NE(controllers.find("    " + writer.controller_name +
                             ":\n      type: forward_command_controller/ForwardCommandController"),
            std::string::npos);
  EXPECT_NE(controllers.find("/**/" + writer.controller_name + ":\n"), std::string::npos);

  EXPECT_EQ(written.launch_arguments,
            (std::vector<std::string>{
                "robot_description_file:=" + written.robot_description_file.string(),
                "controllers_file:=" + written.controllers_file.string(),
                "runtime_namespace:=/manyforge/lite6_robot",
                "controller_manager_name:=controller_manager",
                "hardware_plugin:=mock_components/GenericSystem",
                "arm_controller_name:=lite6_traj_controller",
                "reset_position_controller_name:=" + writer.controller_name,
            }));
}

TEST(LaunchArtifacts, MockToolRowWritesItsGripperController) {
  TemporaryRoot root;
  const auto model = "manyforge_example_models/models/lite_gripper/";
  const auto urdf = read_file(repository(std::string(model) + "lite_gripper.urdf"));
  ModelLaneRow row;
  row.adapter = "mock";
  row.controller = "lite_gripper_controller";
  row.controllers_file = repository(std::string(model) + "mock_controllers.yaml");
  LaunchInstance instance;
  instance.artifacts_root = root.path();
  instance.controllable_component_id = "lite_gripper";
  instance.model_urdf = urdf;
  instance.gripper = MockGripperBinding{"right_finger_joint", 0.0};
  const auto written =
      write_launch_artifacts(row, mock_launch_contribution(row, false), instance);
  EXPECT_TRUE(written.arm_controller_name.empty());
  EXPECT_EQ(written.gripper_controller_name, std::optional<std::string>("lite_gripper_controller"));
  EXPECT_FALSE(written.mock_reset_position.has_value());
  EXPECT_FALSE(written.rates.has_value());
  EXPECT_NE(written.controllers_yaml.find("joint: right_finger_joint"), std::string::npos);
  EXPECT_NE(written.controllers_yaml.find(
                "type: parallel_gripper_action_controller/GripperActionController"),
            std::string::npos);
  EXPECT_NE(std::find(written.launch_arguments.begin(), written.launch_arguments.end(),
                      "gripper_controller_name:=lite_gripper_controller"),
            written.launch_arguments.end());
}

TEST(LaunchArtifacts, VendorContributionGetsItsInstanceValues) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  ModelLaneRow row;
  row.adapter = "xarm";
  row.controller = "lite6_traj_controller";
  row.arrival_tolerance_rad = 0.01;
  row.goal_time_s = 1.0;
  auto instance = lite6_instance(root, urdf);
  instance.initial_joint_positions_rad.clear();  // the robot reports its own state
  instance.address = "192.168.1.150";
  const auto written = write_launch_artifacts(row, vendor_contribution(), instance);

  const auto description = read_file(written.robot_description_file);
  EXPECT_NE(description.find("<plugin>uf_robot_hardware/UFRobotSystemHardware</plugin>"),
            std::string::npos);
  EXPECT_NE(description.find("<param name=\"robot_ip\">R192.168.1.150</param>"),
            std::string::npos);
  EXPECT_EQ(count(description, "<command_interface name=\"velocity\"/>"), 6U);
  EXPECT_EQ(description.find("initial_value"), std::string::npos);
  EXPECT_NE(description.find("<ros2_control name=\"manyforge_hardware\""), std::string::npos);

  EXPECT_NE(written.controllers_yaml.find("inactive: [manyforge_hardware]"), std::string::npos);
  EXPECT_NE(written.controllers_yaml.find("goal_time: 1.0"), std::string::npos);
  EXPECT_FALSE(written.mock_reset_position.has_value());

  ASSERT_FALSE(written.driver_parameters_file.empty());
  EXPECT_EQ(read_file(written.driver_parameters_file),
            "/manyforge/lite6_robot/ufactory_driver:\n  ros__parameters:\n    services:\n"
            "      clean_error: true\n");
  EXPECT_NE(std::find(written.launch_arguments.begin(), written.launch_arguments.end(),
                      "hardware_remappings:=/controller_manager/list_controllers="
                      "/manyforge/lite6_robot/controller_manager/list_controllers"),
            written.launch_arguments.end());
  EXPECT_NE(std::find(written.launch_arguments.begin(), written.launch_arguments.end(),
                      "hardware_plugin:=uf_robot_hardware/UFRobotSystemHardware"),
            written.launch_arguments.end());
}

TEST(LaunchArtifacts, HardwareCommandInterfacesAndWorldParentFrameReachTheRunFiles) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  ModelLaneRow row;
  row.adapter = "xarm";
  row.controller = "lite6_traj_controller";
  row.arrival_tolerance_rad = 0.01;
  auto instance = lite6_instance(root, urdf);
  instance.initial_joint_positions_rad.clear();
  instance.address = "192.168.1.150";
  auto contribution = vendor_contribution();
  const std::string explicit_interfaces = "command_interfaces: [position, velocity]";
  contribution.controllers_yaml.replace(contribution.controllers_yaml.find(explicit_interfaces),
                                        explicit_interfaces.size(),
                                        "command_interfaces: [__command_interfaces__]");
  contribution.world_parent_frame = "universe";
  const auto written = write_launch_artifacts(row, contribution, instance);
  EXPECT_NE(written.controllers_yaml.find(explicit_interfaces + "\n"), std::string::npos);
  EXPECT_EQ(written.controllers_yaml.find("__command_interfaces__"), std::string::npos);
  EXPECT_NE(std::find(written.launch_arguments.begin(), written.launch_arguments.end(),
                      "world_parent_frame:=universe"),
            written.launch_arguments.end());
  contribution.world_parent_frame = "not a frame";
  EXPECT_THROW(static_cast<void>(write_launch_artifacts(row, contribution, instance)),
               ControllerLaunchArtifactError);
}

/// The vendor contribution of a machine whose arm is deactivated (the xArm
/// family's `arm` false): the manager block only, the hardware inactive.
[[nodiscard]] LaunchContribution armless_vendor_contribution() {
  auto contribution = vendor_contribution();
  contribution.controllers_yaml =
      "/**/controller_manager:\n"
      "  ros__parameters:\n"
      "    update_rate: 150\n"
      "    hardware_components_initial_state:\n"
      "      inactive: [${hardware_component}]\n"
      "    joint_state_broadcaster:\n"
      "      type: joint_state_broadcaster/JointStateBroadcaster\n";
  return contribution;
}

// With the arm deactivated the machine's
// process keeps the arm's joints in its description (the hardware needs
// them), loads no arm controller and leaves the hardware inactive.
TEST(LaunchArtifacts, ArmDeactivatedMachineProcessLoadsNoArmController) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  ModelLaneRow row;
  row.adapter = "xarm";
  row.controller = "lite6_traj_controller";
  row.arrival_tolerance_rad = 0.01;
  auto instance = lite6_instance(root, urdf);
  instance.controllable_component_id = "lite6";
  instance.initial_joint_positions_rad.clear();
  instance.max_deceleration_on_cancel_rad_s2.clear();
  instance.address = "192.168.1.150";
  instance.arm_deactivated = true;
  const auto written = write_launch_artifacts(row, armless_vendor_contribution(), instance);

  const auto description = read_file(written.robot_description_file);
  EXPECT_NE(description.find("<plugin>uf_robot_hardware/UFRobotSystemHardware</plugin>"),
            std::string::npos);
  EXPECT_EQ(count(description, "<command_interface name=\"velocity\"/>"), 6U);
  EXPECT_NE(written.controllers_yaml.find("inactive: [manyforge_hardware]"), std::string::npos);
  EXPECT_EQ(written.controllers_yaml.find("lite6_traj_controller"), std::string::npos);
  EXPECT_TRUE(written.arm_controller_name.empty());
  EXPECT_FALSE(written.gripper_controller_name.has_value());
  EXPECT_FALSE(written.rates.has_value());
  EXPECT_FALSE(written.driver_parameters_file.empty());
  for (const auto &argument : written.launch_arguments) {
    EXPECT_FALSE(argument.starts_with("arm_controller_name:=")) << argument;
    EXPECT_FALSE(argument.starts_with("gripper_controller_name:=")) << argument;
  }
  // The deactivated arm is not in the cell: no frames on the cell's TF.
  EXPECT_EQ(std::count(written.launch_arguments.begin(), written.launch_arguments.end(),
                       std::string("publish_tf:=false")),
            1);

  // An arm controller in the contribution, controller values or no arm
  // joints are refused.
  EXPECT_THROW(static_cast<void>(write_launch_artifacts(row, vendor_contribution(), instance)),
               ControllerLaunchArtifactError);
  auto with_stop = instance;
  with_stop.max_deceleration_on_cancel_rad_s2 = decelerations(8.0);
  EXPECT_THROW(static_cast<void>(
                   write_launch_artifacts(row, armless_vendor_contribution(), with_stop)),
               ControllerLaunchArtifactError);
  auto jointless = instance;
  jointless.arm_joint_names.clear();
  EXPECT_THROW(static_cast<void>(
                   write_launch_artifacts(row, armless_vendor_contribution(), jointless)),
               ControllerLaunchArtifactError);
}

/// The strict real-time resource profile for one test's scope.
class StrictRtProfile final {
public:
  StrictRtProfile() { ::setenv("MANYFORGE_RT_RESOURCE_PROFILE", "strict", 1); }
  ~StrictRtProfile() { ::unsetenv("MANYFORGE_RT_RESOURCE_PROFILE"); }
  StrictRtProfile(const StrictRtProfile &) = delete;
  StrictRtProfile &operator=(const StrictRtProfile &) = delete;
};

// OD-59: under the strict profile every lane's controller manager locks its
// memory (stock `lock_memory: true`); a lane's own value (Isaac's `false`)
// is replaced. Off: the lane's file is unchanged.
TEST(LaunchArtifacts, TheStrictProfileLocksEveryControllerManagersMemory) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  const auto mock_row = lite6_mock_row();
  ModelLaneRow vendor_row;
  vendor_row.adapter = "xarm";
  vendor_row.controller = "lite6_traj_controller";
  vendor_row.arrival_tolerance_rad = 0.01;
  auto vendor_instance = lite6_instance(root, urdf);
  vendor_instance.initial_joint_positions_rad.clear();
  vendor_instance.address = "192.168.1.150";
  auto isaac_shaped = vendor_contribution();
  const std::string parameters = "  ros__parameters:\n    update_rate: 150\n";
  isaac_shaped.controllers_yaml.replace(
      isaac_shaped.controllers_yaml.find(parameters), parameters.size(),
      "  ros__parameters:\n    lock_memory: false\n    update_rate: 150\n");
  const auto off = write_launch_artifacts(vendor_row, isaac_shaped, vendor_instance);
  EXPECT_NE(off.controllers_yaml.find("    lock_memory: false\n"), std::string::npos);
  EXPECT_EQ(off.controllers_yaml.find("lock_memory: true"), std::string::npos);
  StrictRtProfile strict;
  const auto expect_locked = [](const std::string &controllers) {
    EXPECT_NE(controllers.find("/controller_manager:\n  ros__parameters:\n    lock_memory: true\n"),
              std::string::npos)
        << controllers;
    EXPECT_EQ(count(controllers, "lock_memory:"), 1U) << controllers;
  };
  expect_locked(write_launch_artifacts(mock_row, mock_launch_contribution(mock_row, true),
                                       lite6_instance(root, urdf))
                    .controllers_yaml);
  expect_locked(write_launch_artifacts(vendor_row, vendor_contribution(), vendor_instance)
                    .controllers_yaml);
  expect_locked(write_launch_artifacts(vendor_row, isaac_shaped, vendor_instance).controllers_yaml);
}

TEST(LaunchArtifacts, MissingInstanceValuesFailClosed) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  ModelLaneRow row;
  row.adapter = "xarm";
  row.controller = "lite6_traj_controller";
  row.arrival_tolerance_rad = 0.01;
  auto instance = lite6_instance(root, urdf);
  instance.initial_joint_positions_rad.clear();
  EXPECT_THROW(static_cast<void>(write_launch_artifacts(row, vendor_contribution(), instance)),
               ControllerLaunchArtifactError);  // no address
  instance.address = "192.168.1.150";
  instance.max_deceleration_on_cancel_rad_s2.erase("joint4");
  EXPECT_THROW(static_cast<void>(write_launch_artifacts(row, vendor_contribution(), instance)),
               ControllerLaunchArtifactError);  // no Stop deceleration
  instance.max_deceleration_on_cancel_rad_s2 = decelerations(8.0);
  auto unknown = vendor_contribution();
  unknown.controllers_yaml += "/**/__unknown__:\n  ros__parameters: {}\n";
  EXPECT_THROW(static_cast<void>(write_launch_artifacts(row, unknown, instance)),
               ControllerLaunchArtifactError);  // a placeholder nobody fills
  row.arrival_tolerance_rad.reset();
  EXPECT_THROW(static_cast<void>(write_launch_artifacts(row, vendor_contribution(), instance)),
               ControllerLaunchArtifactError);  // a robot row without its arrival tolerance
}

TEST(LaunchArtifacts, MockRowNeedsAVendorOrItsControllersFile) {
  auto row = lite6_mock_row();
  row.vendor_controllers_yaml.reset();
  EXPECT_THROW(static_cast<void>(mock_launch_contribution(row, true)),
               ControllerLaunchArtifactError);
  row = ur10e_mock_row();
  EXPECT_NE(mock_launch_contribution(row, true).controllers_yaml.find("/**/__controller__:"),
            std::string::npos);
  row.controllers_file.reset();
  EXPECT_THROW(static_cast<void>(mock_launch_contribution(row, true)),
               ControllerLaunchArtifactError);
  row = lite6_mock_row();
  row.adapter = "xarm";
  EXPECT_THROW(static_cast<void>(mock_launch_contribution(row, true)),
               ControllerLaunchArtifactError);
}

// A contribution whose hardware echoes its commands (Isaac's
// topic-based hardware) names where the writer's target is acknowledged; the
// target itself, and so the writer's name, are the same as without it.
TEST(LaunchArtifacts, AWritersCommandEchoTopicComesFromTheContribution) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  const auto row = lite6_mock_row();
  auto contribution = mock_launch_contribution(row, true);
  const auto plain = write_launch_artifacts(row, contribution, lite6_instance(root, urdf));
  contribution.command_echo_topic = "/isaac_joint_commands";
  const auto echoed = write_launch_artifacts(row, contribution, lite6_instance(root, urdf));
  ASSERT_TRUE(plain.mock_reset_position.has_value());
  ASSERT_TRUE(echoed.mock_reset_position.has_value());
  EXPECT_EQ(echoed.mock_reset_position->command_echo_topic,
            std::optional<std::string>("/isaac_joint_commands"));
  EXPECT_EQ(echoed.mock_reset_position->controller_name, plain.mock_reset_position->controller_name);
  EXPECT_EQ(echoed.mock_reset_position->positions, plain.mock_reset_position->positions);
}

// The writer's entry closes the manager block; its controller block follows.
TEST(LaunchArtifacts, TheResetPositionWriterJoinsTheManagerBlock) {
  EXPECT_EQ(with_reset_position_writer("/**/controller_manager:\n  ros__parameters:\n"
                                       "    update_rate: 100\n\n/**/arm:\n  ros__parameters:\n"
                                       "    joints: [__joint__]\n"),
            "/**/controller_manager:\n  ros__parameters:\n    update_rate: 100\n"
            "    __reset_writer__:\n"
            "      type: forward_command_controller/ForwardCommandController\n"
            "\n/**/arm:\n  ros__parameters:\n    joints: [__joint__]\n"
            "\n/**/__reset_writer__:\n  ros__parameters:\n    joints: [__joint__]\n"
            "    interface_name: position\n");
  EXPECT_THROW(static_cast<void>(with_reset_position_writer("/**/arm:\n")),
               ControllerLaunchArtifactError);
}

// The Mock lane folder documents exactly the values its contribution uses.
TEST(LaunchArtifacts, MockContributionMatchesTheLaneSettings) {
  const auto settings =
      read_file(repository("manyforge_ros2_control_execution/lane/manyforge_settings.yaml"));
  const auto contribution = mock_launch_contribution(lite6_mock_row(), true);
  EXPECT_NE(settings.find("  plugin: " + contribution.hardware.plugin + "\n"), std::string::npos);
  for (const auto &[name, value] : contribution.hardware.parameters)
    EXPECT_NE(settings.find("    " + name + ": \"" + value + "\"\n"), std::string::npos) << name;
  EXPECT_NE(settings.find("  joint_command_interfaces: [position]\n"), std::string::npos);
  EXPECT_NE(settings.find("  type: forward_command_controller/ForwardCommandController\n"),
            std::string::npos);
  EXPECT_FALSE(contribution.driver_parameters_yaml.has_value());
  EXPECT_TRUE(contribution.remappings.empty());
}

TEST(LaunchArtifacts, EvidenceNamesTheWrittenFiles) {
  TemporaryRoot root;
  const auto urdf = read_file(repository("manyforge_example_models/models/lite6/lite6.urdf"));
  const auto row = lite6_mock_row();
  const auto written =
      write_launch_artifacts(row, mock_launch_contribution(row, true), lite6_instance(root, urdf));
  const auto evidence = controller_launch_artifacts_evidence(written);
  EXPECT_NE(evidence.find("\"lane\":\"ros2_control\""), std::string::npos);
  EXPECT_NE(evidence.find(written.controllers_sha256), std::string::npos);
}

// OD-62 (a): a lane can scale only when its JTC claims position and no
// velocity, acceleration or effort command interface (stock JTC 6.10
// subscribes to `~/speed_scaling_input` only then).
TEST(LaunchArtifacts, ClaimedCommandInterfacesDecideSpeedScaling) {
  EXPECT_EQ(controller_command_interfaces("arm:\n  ros__parameters:\n"
                                          "    command_interfaces: [position]\n"),
            (std::vector<std::string>{"position"}));
  EXPECT_EQ(controller_command_interfaces("arm:\n  ros__parameters:\n"
                                          "    command_interfaces: [position, velocity]\n"),
            (std::vector<std::string>{"position", "velocity"}));
  EXPECT_EQ(controller_command_interfaces("arm:\n  ros__parameters:\n"
                                          "    command_interfaces:\n"
                                          "      - position\n"
                                          "    state_interfaces:\n"
                                          "      - position\n"
                                          "      - velocity\n"),
            (std::vector<std::string>{"position"}));
  EXPECT_TRUE(controller_command_interfaces("manager:\n  update_rate: 100\n").empty());

  EXPECT_TRUE(jtc_accepts_speed_scaling({"position"}));
  EXPECT_FALSE(jtc_accepts_speed_scaling({"position", "velocity"}));
  EXPECT_FALSE(jtc_accepts_speed_scaling({"position", "acceleration"}));
  EXPECT_FALSE(jtc_accepts_speed_scaling({"effort"}));
  EXPECT_FALSE(jtc_accepts_speed_scaling({}));
}

} // namespace
