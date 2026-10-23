#include <manyforge_roboplan/controller_launch_artifacts.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>

namespace {

using namespace manyforge::roboplan_backend;

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

[[nodiscard]] std::string read_repository_file(const std::string &relative) {
  return read_file(std::filesystem::path(MANYFORGE_TEST_REPOSITORY_ROOT) /
                   relative);
}

class TemporaryRoot final {
public:
  TemporaryRoot() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-controller-artifacts-" +
             std::to_string(::getpid()) + "-" +
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

const std::vector<std::string> kUFactoryArm{"joint1", "joint2", "joint3",
                                            "joint4", "joint5", "joint6"};
const std::vector<std::string> kUrArm{"shoulder_pan_joint", "shoulder_lift_joint",
                                      "elbow_joint",        "wrist_1_joint",
                                      "wrist_2_joint",      "wrist_3_joint"};

[[nodiscard]] std::map<std::string, double, std::less<>>
cancel_limits(const std::vector<std::string> &joints, const double value) {
  std::map<std::string, double, std::less<>> result;
  for (const auto &joint : joints) result.emplace(joint, value);
  return result;
}

/// One offline mock profile with the model URDF its retained model asset
/// uses and the joints its retained bindings name (maintained sources).
struct OfflineProfile final {
  std::string mock_profile;
  std::string model_path;
  std::vector<std::string> arm;
  std::optional<std::string> gripper_joint;
  std::string hardware_component_name;
  std::string arm_controller;
  std::optional<std::string> gripper_controller;
};

[[nodiscard]] std::vector<OfflineProfile> offline_profiles() {
  return {
      {"lite6_lite_gripper",
       "manyforge_example_models/models/lite6_lite_gripper/"
       "lite6_lite_gripper.urdf",
       kUFactoryArm, "right_finger_joint", "MockLite6LiteGripperSystem",
       "lite6_traj_controller", "lite_gripper_controller"},
      {"ur10e_robotiq_2f_140",
       "manyforge_example_models/models/ur10e_robotiq_2f_140/"
       "ur10e_robotiq_2f_140.structural.urdf",
       kUrArm, "finger_joint", "MockUR10eRobotiq2F140System",
       "joint_trajectory_controller", "robotiq_gripper_controller"},
      {"ur5_roboplan",
       "manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/"
       "ur5_gripper.structural.urdf",
       kUrArm, std::nullopt, "MockUR5System", "joint_trajectory_controller",
       std::nullopt},
      {"ur5e_robotiq_2f_85",
       "manyforge_example_models/models/ur5e_robotiq_2f_85/"
       "ur5e_robotiq_2f_85.urdf",
       kUrArm, "robotiq_85_left_knuckle_joint", "MockUR5eRobotiq2F85System",
       "joint_trajectory_controller", "robotiq_gripper_controller"},
  };
}

[[nodiscard]] ControllerLaunchArtifactsRequest
mock_request(const std::filesystem::path &root, const std::string &component,
             const OfflineProfile &profile, const std::string &model_text,
             const double cancel_limit = 10.0) {
  std::optional<MockGripperBinding> gripper;
  if (profile.gripper_joint.has_value()) {
    gripper = MockGripperBinding{profile.gripper_joint, 0.0042};
  } else {
    // UR5's retained gripper is logical (no model joint).
    gripper = MockGripperBinding{std::nullopt, std::nullopt};
  }
  return ControllerLaunchArtifactsRequest{
      root,
      component,
      model_text,
      profile.arm,
      MockControllerArtifactsInput{profile.mock_profile,
                                   {0.0, -1.57, 1.57, -1.57, -1.57, 0.0},
                                   cancel_limits(profile.arm, cancel_limit),
                                   gripper},
      {},
      {},
      manyforge::content_identity::sha256_digest("test-component-runtime")};
}

[[nodiscard]] std::size_t count(const std::string &text,
                                const std::string &token) {
  std::size_t result = 0U;
  for (auto at = text.find(token); at != std::string::npos;
       at = text.find(token, at + token.size())) {
    ++result;
  }
  return result;
}

// ---------------------------------------------------------------------------

TEST(ControllerLaunchArtifacts, Lite6MockControllersFileIsTheTemplatesFilledIn) {
  const auto yaml = generate_mock_controllers_yaml(
      mock_controller_profile_facts("lite6_lite_gripper"), "lite6_robot",
      kUFactoryArm,
      {{"joint1", 10.0}, {"joint2", 10.0}, {"joint3", 12.5},
       {"joint4", 10.0}, {"joint5", 10.0}, {"joint6", 7.25}},
      std::string("right_finger_joint"));
  EXPECT_EQ(
      yaml,
      "# Generated by the ManyForge host at activation (Q63-Q69, Q74) from\n"
      "# Track 2's UFactory mock arm template and the mock gripper template.\n"
      "/manyforge/lite6_robot/controller_manager:\n"
      "  ros__parameters:\n"
      "    update_rate: 150\n"
      "    enforce_command_limits: false\n"
      "    hardware_components_initial_state:\n"
      "      shutdown_on_initial_state_failure: true\n"
      "    joint_state_broadcaster:\n"
      "      type: joint_state_broadcaster/JointStateBroadcaster\n"
      "    lite6_traj_controller:\n"
      "      type: joint_trajectory_controller/JointTrajectoryController\n"
      "    lite_gripper_controller:\n"
      "      type: position_controllers/GripperActionController\n"
      "\n"
      "/**/lite6_traj_controller:\n"
      "  ros__parameters:\n"
      "    joints: [joint1, joint2, joint3, joint4, joint5, joint6]\n"
      "    command_interfaces: [position]\n"
      "    state_interfaces: [position, velocity]\n"
      "    state_publish_rate: 25.0\n"
      "    action_monitor_rate: 20.0\n"
      "    interface_name: position\n"
      "    interpolate_from_desired_state: true\n"
      "    allow_trajectory_replacement: true\n"
      "    constraints:\n"
      "      goal_time: 0.5\n"
      "      stopped_velocity_tolerance: 0.0\n"
      "      decelerate_on_cancel: true\n"
      "      joint1: {trajectory: 1.0, goal: 0.01, max_deceleration_on_cancel: "
      "10.0}\n"
      "      joint2: {trajectory: 1.0, goal: 0.01, max_deceleration_on_cancel: "
      "10.0}\n"
      "      joint3: {trajectory: 1.0, goal: 0.01, max_deceleration_on_cancel: "
      "12.5}\n"
      "      joint4: {trajectory: 1.0, goal: 0.01, max_deceleration_on_cancel: "
      "10.0}\n"
      "      joint5: {trajectory: 1.0, goal: 0.01, max_deceleration_on_cancel: "
      "10.0}\n"
      "      joint6: {trajectory: 1.0, goal: 0.01, max_deceleration_on_cancel: "
      "7.25}\n"
      "\n"
      "/**/lite_gripper_controller:\n"
      "  ros__parameters:\n"
      "    joint: right_finger_joint\n"
      "    goal_tolerance: 0.01\n"
      "    stall_velocity_threshold: 0.001\n"
      "    stall_timeout: 1.0\n"
      "    allow_stalling: false\n");
}

TEST(ControllerLaunchArtifacts, MockArtifactsKeepStockJtcReplacementOptIn) {
  for (const auto &profile : offline_profiles()) {
    SCOPED_TRACE(profile.mock_profile);
    const TemporaryRoot root;
    const auto model = read_repository_file(profile.model_path);
    const auto written = write_controller_launch_artifacts(
        mock_request(root.path(), "robot", profile, model));
    const auto yaml = read_file(written.controllers_file);
    EXPECT_EQ(yaml, written.controllers_yaml);
    // Stock JTC scheduled replacement (Mock option1) needs only the upstream
    // opt-in; nothing else selects a replacement timeline.
    EXPECT_EQ(count(yaml, "allow_trajectory_replacement: true"), 1U);
    EXPECT_EQ(yaml.find("replacement_time_reference"), std::string::npos);
  }
}

TEST(ControllerLaunchArtifacts, UrMockControllersKeepTheirRetainedConstraints) {
  const auto with_gripper = generate_mock_controllers_yaml(
      mock_controller_profile_facts("ur5e_robotiq_2f_85"), "ur5e_robot", kUrArm,
      cancel_limits(kUrArm, 10.0),
      std::string("robotiq_85_left_knuckle_joint"));
  EXPECT_EQ(with_gripper.rfind("/manyforge/ur5e_robot/controller_manager:\n"
                               "  ros__parameters:\n"
                               "    update_rate: 100\n",
                               std::string::npos) != std::string::npos,
            true);
  EXPECT_NE(with_gripper.find(
                "    joints: [shoulder_pan_joint, shoulder_lift_joint, "
                "elbow_joint, wrist_1_joint, wrist_2_joint, wrist_3_joint]\n"),
            std::string::npos);
  EXPECT_NE(with_gripper.find("      wrist_3_joint: {trajectory: 0.2, goal: 0.1, "
                              "max_deceleration_on_cancel: 10.0}\n"),
            std::string::npos);
  EXPECT_NE(with_gripper.find("    allow_partial_joints_goal: false\n"),
            std::string::npos);
  EXPECT_NE(with_gripper.find("    interpolate_from_desired_state: true\n"),
            std::string::npos);
  EXPECT_NE(with_gripper.find("/**/robotiq_gripper_controller:\n  ros__parameters:\n"
                              "    joint: robotiq_85_left_knuckle_joint\n"),
            std::string::npos);
  EXPECT_EQ(count(with_gripper, "controller_manager:"), 1U);
  EXPECT_EQ(count(with_gripper, "__"), count(with_gripper, "ros__parameters"));
  EXPECT_EQ(with_gripper.find("null"), std::string::npos);

  // UR5: no gripper joint, no gripper controller.
  const auto arm_only = generate_mock_controllers_yaml(
      mock_controller_profile_facts("ur5_roboplan"), "ur5_robot", kUrArm,
      cancel_limits(kUrArm, 9.0), std::nullopt);
  EXPECT_EQ(arm_only.find("GripperActionController"), std::string::npos);
  EXPECT_EQ(count(arm_only, "max_deceleration_on_cancel: 9.0}"), 6U);
  EXPECT_EQ(count(arm_only, "/**/joint_trajectory_controller:"), 1U);
}

TEST(ControllerLaunchArtifacts, ControllersFileFailsClosed) {
  const auto lite6 = mock_controller_profile_facts("lite6_lite_gripper");
  // Q48: every arm joint needs the operator Stop profile's deceleration.
  auto missing = cancel_limits(kUFactoryArm, 10.0);
  missing.erase("joint4");
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   lite6, "lite6_robot", kUFactoryArm, missing,
                   std::string("right_finger_joint"))),
               ControllerLaunchArtifactError);
  auto foreign = cancel_limits(kUFactoryArm, 10.0);
  foreign.emplace("joint7", 10.0);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   lite6, "lite6_robot", kUFactoryArm, foreign,
                   std::string("right_finger_joint"))),
               ControllerLaunchArtifactError);
  auto zero = cancel_limits(kUFactoryArm, 10.0);
  zero["joint1"] = 0.0;
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   lite6, "lite6_robot", kUFactoryArm, zero,
                   std::string("right_finger_joint"))),
               ControllerLaunchArtifactError);
  // A profile that requires a gripper gets no YAML without one.
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   lite6, "lite6_robot", kUFactoryArm,
                   cancel_limits(kUFactoryArm, 10.0), std::nullopt)),
               ControllerLaunchArtifactError);
  // UR5 has no gripper controller.
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   mock_controller_profile_facts("ur5_roboplan"), "ur5_robot",
                   kUrArm, cancel_limits(kUrArm, 10.0),
                   std::string("finger_joint"))),
               ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   lite6, "lite6.robot", kUFactoryArm,
                   cancel_limits(kUFactoryArm, 10.0),
                   std::string("right_finger_joint"))),
               ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(mock_controller_profile_facts("uf850")),
               ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(retained_isaac_controllers_yaml("ur5e")),
               ControllerLaunchArtifactError);
}

TEST(ControllerLaunchArtifacts, EveryOfflineMockProfileWritesBothFilesAndTheLaunchArguments) {
  for (const auto &profile : offline_profiles()) {
    SCOPED_TRACE(profile.mock_profile);
    const TemporaryRoot root;
    const auto model = read_repository_file(profile.model_path);
    const auto component = "robot_" + profile.mock_profile;
    const auto written = write_controller_launch_artifacts(
        mock_request(root.path(), component, profile, model));
    EXPECT_EQ(written.lane, ControllerLaunchLane::kMock);
    EXPECT_EQ(written.controller_profile, profile.mock_profile);
    ASSERT_TRUE(written.mock_reset_position);
    EXPECT_EQ(written.mock_reset_position->joint_names, profile.arm);
    if (profile.gripper_joint) {
      EXPECT_EQ(std::find(written.mock_reset_position->joint_names.begin(),
                          written.mock_reset_position->joint_names.end(),
                          *profile.gripper_joint),
                written.mock_reset_position->joint_names.end());
    }
    EXPECT_EQ(written.directory, root.path() / component);
    EXPECT_EQ(written.robot_description_file,
              root.path() / component / "robot_description.urdf");
    EXPECT_EQ(written.controllers_file,
              root.path() / component / "controllers.yaml");
    const auto description = read_file(written.robot_description_file);
    const auto controllers = read_file(written.controllers_file);
    EXPECT_EQ(written.controllers_yaml, controllers);
    EXPECT_EQ(written.robot_description_sha256,
              manyforge::content_identity::sha256_digest(description));
    EXPECT_EQ(written.controllers_sha256,
              manyforge::content_identity::sha256_digest(controllers));

    // Description: the model, one GenericSystem section with the deployment's
    // initial values and (when model-backed) the commanded gripper joint.
    EXPECT_EQ(count(description, "<ros2_control "), 1U);
    EXPECT_NE(description.find("<ros2_control name=\"" +
                               profile.hardware_component_name +
                               "\" type=\"system\">"),
              std::string::npos);
    EXPECT_NE(description.find("<plugin>mock_components/GenericSystem</plugin>"),
              std::string::npos);
    EXPECT_NE(description.find("<param name=\"calculate_dynamics\">true</param>"),
              std::string::npos);
    EXPECT_EQ(count(description, "<param name=\"initial_value\">"),
              profile.arm.size() + (profile.gripper_joint.has_value() ? 1U : 0U));
    // The deployment's initial joint positions, paired by binding order.
    EXPECT_NE(description.find("<joint name=\"" + profile.arm.at(1) +
                               "\">\n"
                               "      <command_interface name=\"position\"/>\n"
                               "      <state_interface name=\"position\">\n"
                               "        <param name=\"initial_value\">"
                               "-1.5700000000000001</param>"),
              std::string::npos);
    if (profile.gripper_joint.has_value()) {
      EXPECT_NE(description.find("    <joint name=\"" + *profile.gripper_joint +
                                 "\">\n      <command_interface name=\"position\"/>\n"
                                 "      <state_interface name=\"position\">\n"
                                 "        <param name=\"initial_value\">0.0041999999999999997</param>"),
                std::string::npos);
    }
    // The model's root version attribute is kept unchanged (Lyrical target,
    // maintainer 2026-10-11): the Lite 6 model declares 1.2.
    EXPECT_EQ(description.find("version=\"1.2\"") != std::string::npos,
              profile.mock_profile == "lite6_lite_gripper");

    // Controllers: one manager block under the Q66 name.
    EXPECT_EQ(controllers.find("/manyforge/" + component +
                               "/controller_manager:\n"),
              controllers.find("/manyforge/"));
    EXPECT_EQ(count(controllers, "controller_manager:"), 1U);
    EXPECT_NE(controllers.find("/**/" + profile.arm_controller + ":\n"),
              std::string::npos);
    EXPECT_EQ(controllers.find("GripperActionController") != std::string::npos,
              profile.gripper_controller.has_value());

    // Mock launch contract (Track 2 mock_control.launch.py, DX-175). A
    // profile without a gripper omits the argument: `ros2 launch` rejects an
    // empty value.
    std::vector<std::string> expected_arguments{
        "robot_description_file:=" + written.robot_description_file.string(),
        "controllers_file:=" + written.controllers_file.string(),
        "runtime_namespace:=/manyforge/" + component,
        "controller_manager_name:=controller_manager",
        "arm_controller_name:=" + profile.arm_controller,
        "reset_position_controller_name:=" +
            written.mock_reset_position->controller_name};
    if (profile.gripper_controller.has_value()) {
      expected_arguments.push_back("gripper_controller_name:=" +
                                   *profile.gripper_controller);
    }
    EXPECT_EQ(written.launch_arguments, expected_arguments);
    for (const auto &argument : written.launch_arguments) {
      EXPECT_FALSE(argument.ends_with(":=")) << argument;
    }
    EXPECT_TRUE(written.launch_environment.empty());
    const auto evidence = controller_launch_artifacts_evidence(written);
    EXPECT_NE(evidence.find(written.robot_description_sha256), std::string::npos);
    EXPECT_NE(evidence.find(written.controllers_sha256), std::string::npos);
  }
}

TEST(ControllerLaunchArtifacts, BothIsaacProfilesUseTheGeneratedDescriptionAndRetainedControllers) {
  struct IsaacCase final {
    std::string profile;
    std::string model_path;
    std::vector<std::string> arm;
    IsaacControllerLane lane;
    std::string retained_controllers;
  };
  const std::vector<IsaacCase> cases{
      {"lite6",
       "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf",
       kUFactoryArm,
       IsaacControllerLane{"IsaacLite6TopicBasedSystem", "/isaac_joint_commands",
                           "/isaac_joint_states",
                           {0.0, 0.0, 1.57, 0.0, 0.0, 0.0}},
       "manyforge_ros_bringup/config/isaac_lite6_topic_based_controllers.yaml"},
      {"ur10e",
       "manyforge_example_models/models/ur10e_robotiq_2f_140/"
       "ur10e_robotiq_2f_140.structural.urdf",
       kUrArm,
       IsaacControllerLane{"IsaacUR10eTopicBasedSystem", "/isaac_joint_commands",
                           "/isaac_joint_states",
                           {2.63422544, -0.875980752, 1.551074106, -2.29231544,
                            -1.560498884, -2.038544566}},
       "manyforge_ros_bringup/config/isaac_ur10e_topic_based_controllers.yaml"},
  };
  for (const auto &item : cases) {
    SCOPED_TRACE(item.profile);
    const TemporaryRoot root;
    const auto model = read_repository_file(item.model_path);
    const auto written = write_controller_launch_artifacts(
        ControllerLaunchArtifactsRequest{
            root.path(), "isaac_robot", model, item.arm,
            IsaacControllerArtifactsInput{item.profile, item.lane}});
    EXPECT_EQ(written.lane, ControllerLaunchLane::kIsaac);
    const auto description = read_file(written.robot_description_file);
    EXPECT_EQ(count(description, "<ros2_control "), 1U);
    EXPECT_NE(description.find("<plugin>topic_based_ros2_control/TopicBasedSystem"
                               "</plugin>"),
              std::string::npos);
    EXPECT_NE(description.find("<ros2_control name=\"" +
                               item.lane.hardware_component_name + "\""),
              std::string::npos);
    EXPECT_EQ(count(description, "<param name=\"initial_value\">"), 6U);
    EXPECT_EQ(description.find("version=\"1.2\"") != std::string::npos,
              item.profile == "lite6");
    // The controllers file is the profile's retained Isaac file, unchanged.
    EXPECT_EQ(read_file(written.controllers_file),
              read_repository_file(item.retained_controllers));
    EXPECT_EQ(written.controllers_yaml,
              read_repository_file(item.retained_controllers));
    EXPECT_EQ(written.controllers_sha256,
              manyforge::content_identity::sha256_digest(
                  read_repository_file(item.retained_controllers)));
    // Track 2's Isaac shell contract (DX-168).
    EXPECT_EQ(written.launch_environment,
              (std::vector<std::pair<std::string, std::string>>{
                  {"MANYFORGE_ROBOT_DESCRIPTION_FILE",
                   written.robot_description_file.string()},
                  {"MANYFORGE_CONTROLLERS_FILE", written.controllers_file.string()},
                  {"ISAAC_TOPIC_CONTROL_PROFILE", item.profile},
                  {"MANYFORGE_ISAAC_COMPONENT_ID", "isaac_robot"},
              }));
    EXPECT_TRUE(written.launch_arguments.empty());
  }
}

TEST(ControllerLaunchArtifacts, MixedLite6AndUr5eCellWritesTwoSeparatePairs) {
  const TemporaryRoot root;
  const auto profiles = offline_profiles();
  const auto &lite6 = profiles.at(0);
  const auto &ur5e = profiles.at(3);
  const auto lite6_model = read_repository_file(lite6.model_path);
  const auto ur5e_model = read_repository_file(ur5e.model_path);
  const auto first = write_controller_launch_artifacts(
      mock_request(root.path(), "lite6_robot", lite6, lite6_model, 10.0));
  const auto second = write_controller_launch_artifacts(
      mock_request(root.path(), "ur5e_robot", ur5e, ur5e_model, 8.0));
  EXPECT_NE(first.directory, second.directory);
  EXPECT_TRUE(std::filesystem::is_regular_file(first.robot_description_file));
  EXPECT_TRUE(std::filesystem::is_regular_file(first.controllers_file));
  EXPECT_TRUE(std::filesystem::is_regular_file(second.robot_description_file));
  EXPECT_TRUE(std::filesystem::is_regular_file(second.controllers_file));
  EXPECT_NE(read_file(first.controllers_file).find(
                "/manyforge/lite6_robot/controller_manager:"),
            std::string::npos);
  EXPECT_NE(read_file(second.controllers_file).find(
                "/manyforge/ur5e_robot/controller_manager:"),
            std::string::npos);
  EXPECT_NE(first.robot_description_sha256, second.robot_description_sha256);
}

TEST(ControllerLaunchArtifacts, MockGripperFailsClosedOnlyWhenTheProfileRequiresIt) {
  const TemporaryRoot root;
  const auto profiles = offline_profiles();
  const auto &lite6 = profiles.at(0);
  const auto model = read_repository_file(lite6.model_path);
  auto request = mock_request(root.path(), "lite6_robot", lite6, model);
  // Not model-backed: no projection, no joint; Lite 6 requires one.
  std::get<MockControllerArtifactsInput>(request.lane).gripper =
      MockGripperBinding{std::nullopt, std::nullopt};
  EXPECT_THROW(static_cast<void>(write_controller_launch_artifacts(request)),
               ControllerLaunchArtifactError);
  // Model-backed without a retained projection value: fail closed.
  std::get<MockControllerArtifactsInput>(request.lane).gripper =
      MockGripperBinding{std::string("right_finger_joint"), std::nullopt};
  EXPECT_THROW(static_cast<void>(write_controller_launch_artifacts(request)),
               ControllerLaunchArtifactError);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "lite6_robot" /
                                       "robot_description.urdf"));
  // The relative root is refused.
  auto relative = mock_request("relative/root", "lite6_robot", lite6, model);
  EXPECT_THROW(static_cast<void>(write_controller_launch_artifacts(relative)),
               ControllerLaunchArtifactError);
}

// ---------------------------------------------------------------------------
// Host activation glue (write_component_controller_launch_artifacts): the
// host's retained participant bindings, controller profile selection, Q48
// resource limits, gripper projection and Q82 retained frame prefix.

namespace core = manyforge::core;

struct ResourceSpec final {
  std::string resource_id;
  std::optional<std::string> source_joint;
};

[[nodiscard]] core::RuntimeParticipantBinding
participant_binding(const std::string &component, const std::string &target,
                    const std::string &capability,
                    const std::vector<ResourceSpec> &resources) {
  std::vector<core::RuntimeResourceBindingDTO> resource_rows;
  std::vector<std::string> compiled;
  for (const auto &resource : resources) {
    const auto compiled_name =
        component + "__" + resource.source_joint.value_or(resource.resource_id);
    compiled.push_back(compiled_name);
    resource_rows.push_back(core::RuntimeResourceBindingDTO{
        core::ResourceRoleId(target == "arm" ? "arm_joints" : "fingers"),
        core::ResourceId(resource.resource_id), resource.source_joint,
        compiled_name,
        target == "arm" ? core::CoordinateKind::kAngular
                        : core::CoordinateKind::kLinear});
  }
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.physical"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding." + component + "." + target),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId(target),
      core::ControllableComponentId(component),
      core::ComponentTypeId("robot.type"),
      core::ComponentTypeRevision("robot-type-revision"),
      core::CommandTargetId(target),
      core::CapabilityId(capability),
      core::CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {core::ProgramModelRoleBindingDTO{core::ModelRoleId("arm_model"),
                                        core::ModelInstanceId(component + ".model"),
                                        core::ModelRevision("model-revision")}},
      {core::RuntimeModelGroupBindingDTO{core::ModelGroupRoleId(target + "_group"),
                                         core::ModelInstanceId(component + ".model"),
                                         target, compiled}},
      {core::ProgramFrameRoleBindingDTO{
          core::FrameRoleId("tcp"), core::ModelInstanceId(component + ".model"),
          core::ModelRevision("model-revision"), "tcp"}},
      std::move(resource_rows),
      {},
      core::RuntimeControlPathDTO{
          core::ControlDomainId(component + "." + target + ".control"),
          core::ControlDomainRevision("domain-revision"),
          core::ControlBindingId(component + "." + target + ".binding"),
          core::ControlBindingRevision("binding-revision"),
          core::ControlInterfaceContractId("interface"),
          core::ControlInterfaceRevision("interface-revision"),
          core::ControllerTopologyId("topology.ros2_control"),
          core::TopologyRevision("topology-revision"),
          core::ControlRouteId(target + "_position"),
          core::ActivationSetId("activation.ros2_control"),
          core::ActiveControlConfigurationId("active.ros2_control"),
          core::ActiveControlConfigurationGeneration(1U),
          core::ProviderInstanceId(component + ".ros2_control"),
          core::ProviderManifestId("manyforge.provider.ros2_control"),
          core::ProviderManifestRevision("provider-manifest-revision"),
          core::RuntimeUnitId("cell.ros2_control_execution"),
          core::RuntimeEpoch("runtime-epoch.1"),
          core::StateAuthorityId(component + "." + target + ".commanded_sim_state"),
          core::StateAuthorityClass::kCommanded,
          core::StopEvidencePolicyId("stop"),
          core::StopEvidencePolicyRevision("stop-revision"),
          core::ContainmentGranularity::kResource,
          std::nullopt},
      std::nullopt,
      {},
      {core::RuntimeUnitEpochBindingDTO{
          core::RuntimeUnitId("cell.ros2_control_execution"),
          core::RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) throw std::logic_error(sealed.error().message);
  return std::move(sealed.value());
}

[[nodiscard]] core::RuntimeParticipantBinding
arm_binding(const std::string &component, const std::vector<std::string> &joints) {
  std::vector<ResourceSpec> resources;
  for (std::size_t index = 0; index < joints.size(); ++index) {
    resources.push_back({component + ".arm.coordinate.00000" +
                             std::to_string(index),
                         joints[index]});
  }
  return participant_binding(component, "arm", "execution.trajectory.execute",
                             resources);
}

[[nodiscard]] core::RuntimeParticipantBinding
gripper_binding(const std::string &component,
                const std::optional<std::string> &joint) {
  return participant_binding(
      component, "gripper", "tool.gripper.command",
      {{joint.has_value() ? component + ".gripper.coordinate.000000"
                          : component + ".gripper.logical_position",
        joint}});
}

[[nodiscard]] std::vector<std::pair<core::ResourceId, double>>
resource_limits(const core::RuntimeParticipantBinding &binding,
                const double value) {
  std::vector<std::pair<core::ResourceId, double>> result;
  for (const auto &resource : binding.value().resources) {
    result.emplace_back(resource.resource_id, value);
  }
  return result;
}

struct HostComponent final {
  core::RuntimeParticipantBinding motion;
  ComponentControllerLaunchInput input;
};

[[nodiscard]] ControllerLaunchArtifacts write_host(const HostComponent &component) {
  return write_component_controller_launch_artifacts(component.motion,
                                                     component.input);
}

[[nodiscard]] HostComponent mock_component(const std::filesystem::path &root,
                                           const std::string &component,
                                           const OfflineProfile &profile,
                                           const std::string &frame_prefix,
                                           const double cancel_limit) {
  auto motion = arm_binding(component, profile.arm);
  ComponentControllerLaunchInput input;
  input.artifacts_root = root;
  input.component_runtime_identity =
      manyforge::content_identity::sha256_digest("test-component-runtime");
  input.gripper_binding = gripper_binding(component, profile.gripper_joint);
  if (profile.gripper_joint.has_value()) input.gripper_projected_position = 0.0042;
  input.mock_profile = profile.mock_profile;
  input.model_urdf = read_repository_file(profile.model_path);
  input.initial_joint_positions_rad = {0.0, -1.57, 1.57, -1.57, -1.57, 0.0};
  input.controlled_stop_max_deceleration_rad_s2 =
      resource_limits(motion, cancel_limit);
  input.frame_prefix = frame_prefix;
  return HostComponent{std::move(motion), std::move(input)};
}

TEST(ControllerLaunchArtifactsHost, ActivationWritesBothFilesAndPassesTheLaunchContract) {
  const TemporaryRoot root;
  const auto profiles = offline_profiles();
  // Mock component of a one-robot cell: Q48 resource limits become per-joint
  // cancel bounds through the binding map; empty deployment prefix (Q82).
  const auto mock = write_host(
      mock_component(root.path(), "lite6_robot", profiles.at(0), "", 12.5));
  EXPECT_EQ(mock.lane, ControllerLaunchLane::kMock);
  EXPECT_TRUE(std::filesystem::is_regular_file(mock.robot_description_file));
  EXPECT_TRUE(std::filesystem::is_regular_file(mock.controllers_file));
  EXPECT_EQ(manyforge::content_identity::sha256_digest(
                read_file(mock.controllers_file)),
            mock.controllers_sha256);
  EXPECT_EQ(count(read_file(mock.controllers_file),
                  "goal: 0.01, max_deceleration_on_cancel: 12.5}"),
            6U);
  EXPECT_NE(read_file(mock.robot_description_file)
                .find("<joint name=\"right_finger_joint\">"),
            std::string::npos);
  EXPECT_EQ(
      mock.launch_arguments,
      (std::vector<std::string>{
          "robot_description_file:=" + mock.robot_description_file.string(),
          "controllers_file:=" + mock.controllers_file.string(),
          "runtime_namespace:=/manyforge/lite6_robot",
          "controller_manager_name:=controller_manager",
          "arm_controller_name:=lite6_traj_controller",
          "reset_position_controller_name:=" +
              mock.mock_reset_position->controller_name,
          "gripper_controller_name:=lite_gripper_controller"}));
  EXPECT_TRUE(mock.frame_prefix.empty());

  // Isaac component of a one-robot cell: generated description, retained
  // controllers file, Track 2 shell environment, no prefix.
  const auto isaac_motion = arm_binding("isaac_ur10e_light_robot", kUrArm);
  ComponentControllerLaunchInput isaac;
  isaac.artifacts_root = root.path();
  isaac.gripper_binding =
      gripper_binding("isaac_ur10e_light_robot", std::string("finger_joint"));
  isaac.isaac_profile = "ur10e";
  isaac.isaac_lane = IsaacControllerLane{
      "IsaacUR10eTopicBasedSystem", "/isaac_joint_commands", "/isaac_joint_states",
      {2.63422544, -0.875980752, 1.551074106, -2.29231544, -1.560498884,
       -2.038544566}};
  isaac.model_urdf = read_repository_file(
      "manyforge_example_models/models/ur10e_robotiq_2f_140/"
      "ur10e_robotiq_2f_140.structural.urdf");
  const auto isaac_written =
      write_component_controller_launch_artifacts(isaac_motion, isaac);
  EXPECT_EQ(isaac_written.lane, ControllerLaunchLane::kIsaac);
  EXPECT_EQ(isaac_written.launch_environment,
            (std::vector<std::pair<std::string, std::string>>{
                {"MANYFORGE_ROBOT_DESCRIPTION_FILE",
                 isaac_written.robot_description_file.string()},
                {"MANYFORGE_CONTROLLERS_FILE",
                 isaac_written.controllers_file.string()},
                {"ISAAC_TOPIC_CONTROL_PROFILE", "ur10e"},
                {"MANYFORGE_ISAAC_COMPONENT_ID", "isaac_ur10e_light_robot"}}));
  EXPECT_EQ(read_file(isaac_written.controllers_file),
            read_repository_file("manyforge_ros_bringup/config/"
                                 "isaac_ur10e_topic_based_controllers.yaml"));
  EXPECT_NE(read_file(isaac_written.robot_description_file)
                .find("<param name=\"initial_value\">2.634225"),
            std::string::npos);

  // UR5 (logical gripper, not model-backed): no gripper joint or controller.
  const auto ur5 = write_host(
      mock_component(root.path(), "ur5_robot", profiles.at(2), "", 10.0));
  EXPECT_EQ(ur5.launch_arguments.back(),
            "reset_position_controller_name:=" +
                ur5.mock_reset_position->controller_name);
  for (const auto &argument : ur5.launch_arguments) {
    EXPECT_NE(argument.rfind("gripper_controller_name:=", 0), 0U) << argument;
    EXPECT_NE(argument.rfind("frame_prefix:=", 0), 0U) << argument;
  }
}

TEST(ControllerLaunchArtifactsHost,
     Q82FramePrefixIsTheRetainedLiteralForEachComponent) {
  const TemporaryRoot root;
  const auto profiles = offline_profiles();
  // Existing robots keep their authored empty prefix when a peer is added.
  const auto lite6 = write_host(
      mock_component(root.path(), "lite6_robot", profiles.at(0), "", 10.0));
  const auto ur5e = write_host(mock_component(
      root.path(), "ur5e_robot", profiles.at(3), "newcomer/custom/", 8.0));
  EXPECT_TRUE(lite6.frame_prefix.empty());
  for (const auto &argument : lite6.launch_arguments) {
    EXPECT_NE(argument.rfind("frame_prefix:=", 0), 0U) << argument;
  }
  EXPECT_EQ(ur5e.frame_prefix, "newcomer/custom/");
  EXPECT_EQ(ur5e.launch_arguments.back(), "frame_prefix:=newcomer/custom/");
  EXPECT_NE(lite6.robot_description_file, ur5e.robot_description_file);
  EXPECT_NE(lite6.controllers_file, ur5e.controllers_file);

  // No count rule, own-ID restriction, slash insertion or normalization.
  for (const std::string prefix :
       {"other/", "without_slash", "custom//branch/"}) {
    const auto prefixed = write_host(mock_component(
        root.path(), "lite6_robot", profiles.at(0), prefix, 10.0));
    EXPECT_EQ(prefixed.frame_prefix, prefix);
    EXPECT_EQ(prefixed.launch_arguments.back(), "frame_prefix:=" + prefix);
    EXPECT_EQ(prefixed.robot_description_sha256,
              lite6.robot_description_sha256);
    EXPECT_EQ(prefixed.controllers_sha256, lite6.controllers_sha256);
  }

  // Isaac forwards the same literal through the optional shell contract.
  const auto isaac_motion = arm_binding("isaac_lite6_robot", kUFactoryArm);
  ComponentControllerLaunchInput isaac;
  isaac.artifacts_root = root.path();
  isaac.isaac_profile = "lite6";
  isaac.isaac_lane = IsaacControllerLane{"IsaacLite6TopicBasedSystem",
                                         "/isaac_joint_commands",
                                         "/isaac_joint_states",
                                         {0.0, 0.0, 1.57, 0.0, 0.0, 0.0}};
  isaac.model_urdf = read_repository_file(profiles.at(0).model_path);
  isaac.frame_prefix = "authored/custom";
  const auto isaac_written =
      write_component_controller_launch_artifacts(isaac_motion, isaac);
  EXPECT_EQ(isaac_written.frame_prefix, "authored/custom");
  EXPECT_EQ(isaac_written.launch_environment.back(),
            (std::pair<std::string, std::string>{"MANYFORGE_FRAME_PREFIX",
                                                 "authored/custom"}));
}

TEST(ControllerLaunchArtifacts, GripperOnlyYamlRejectsArmLimitsAndMissingController) {
  const auto lite6 = mock_controller_profile_facts("lite6_lite_gripper");
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   lite6, "tool", {}, {{"joint1", 10.0}}, "right_finger_joint")),
               ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   lite6, "tool", {}, {}, std::nullopt)),
               ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   mock_controller_profile_facts("ur5_roboplan"), "tool", {}, {},
                   "right_finger_joint")),
               ControllerLaunchArtifactError);
}

TEST(ControllerLaunchArtifactsHost, GenuineGripperOnlyMockUsesExistingProfiles) {
  const TemporaryRoot root;
  for (const auto &profile : offline_profiles()) {
    if (!profile.gripper_joint.has_value()) continue;
    SCOPED_TRACE(profile.mock_profile);
    auto component = mock_component(root.path(), "tool", profile, "tool/", 10.0);
    component.input.initial_joint_positions_rad.clear();
    component.input.controlled_stop_max_deceleration_rad_s2.clear();
    const auto written = write_component_controller_launch_artifacts(
        std::optional<core::RuntimeParticipantBinding>{}, component.input);
    EXPECT_EQ(written.controllable_component_id, "tool");
    EXPECT_EQ(written.controller_profile, profile.mock_profile);
    EXPECT_TRUE(written.arm_controller_name.empty());
    EXPECT_EQ(written.gripper_controller_name, profile.gripper_controller);
    const auto yaml = read_file(written.controllers_file);
    EXPECT_EQ(yaml, written.controllers_yaml);
    EXPECT_EQ(written.controllers_sha256,
              manyforge::content_identity::sha256_digest(yaml));
    EXPECT_NE(yaml.find("/manyforge/tool/controller_manager:"), std::string::npos);
    EXPECT_NE(yaml.find("    joint_state_broadcaster:"), std::string::npos);
    EXPECT_NE(yaml.find("    update_rate: " +
                        std::to_string(static_cast<int>(
                            mock_controller_lane_rates(profile.mock_profile)
                                .controller_update_rate_hz))),
              std::string::npos);
    EXPECT_NE(yaml.find("position_controllers/GripperActionController"),
              std::string::npos);
    EXPECT_NE(yaml.find("    joint: " + *profile.gripper_joint), std::string::npos);
    EXPECT_EQ(yaml.find("JointTrajectoryController"), std::string::npos);
    EXPECT_EQ(yaml.find(profile.arm_controller), std::string::npos);
    // ros__parameters is a required ROS key; only template tokens are unresolved.
    EXPECT_NE(yaml.find("  ros__parameters:\n"), std::string::npos);
    for (const auto* token : {"__arm_controller__", "__joint__",
                              "__gripper_controller__", "__gripper_joint__"}) {
      EXPECT_EQ(yaml.find(token), std::string::npos) << token;
    }
    const auto description = read_file(written.robot_description_file);
    const auto section = description.find("  <ros2_control");
    ASSERT_NE(section, std::string::npos);
    const auto hardware = description.substr(section);
    EXPECT_EQ(count(hardware, "<joint name="), 1U);
    EXPECT_NE(hardware.find("<joint name=\"" + *profile.gripper_joint + "\">"),
              std::string::npos);
    EXPECT_EQ(written.robot_description_sha256,
              manyforge::content_identity::sha256_digest(description));
    EXPECT_EQ(written.launch_arguments,
              (std::vector<std::string>{
                  "robot_description_file:=" + written.robot_description_file.string(),
                  "controllers_file:=" + written.controllers_file.string(),
                  "runtime_namespace:=/manyforge/tool",
                  "controller_manager_name:=controller_manager",
                  "gripper_controller_name:=" + *profile.gripper_controller,
                  "frame_prefix:=tool/"}));
  }
}

TEST(ControllerLaunchArtifacts,
     MockWriterIdentityIsImmutableAndConfigurationScoped) {
  const TemporaryRoot root;
  const auto profile = offline_profiles().at(2);
  const auto model = read_repository_file(profile.model_path);
  auto request = mock_request(root.path(), "robot", profile, model);
  const auto initial = write_controller_launch_artifacts(request);
  ASSERT_TRUE(initial.mock_reset_position);
  const auto &target = *initial.mock_reset_position;
  EXPECT_EQ(target.joint_names, request.arm_joint_names);
  EXPECT_EQ(target.positions,
            std::get<MockControllerArtifactsInput>(request.lane)
                .initial_joint_positions_rad);
  EXPECT_EQ(target.component_runtime_identity,
            request.component_runtime_identity);
  EXPECT_TRUE(
      manyforge::content_identity::is_sha256_digest(target.target_digest));
  EXPECT_EQ(target.commands_topic,
            "/manyforge/robot/" + target.controller_name + "/commands");
  EXPECT_EQ(count(initial.controllers_yaml,
                  "type: forward_command_controller/ForwardCommandController"),
            1U);
  EXPECT_NE(
      initial.controllers_yaml.find("/**/" + target.controller_name + ":"),
      std::string::npos);
  EXPECT_NE(initial.controllers_yaml.find("    interface_name: position"),
            std::string::npos);
  EXPECT_EQ(write_controller_launch_artifacts(request).mock_reset_position,
            initial.mock_reset_position);
  auto changed = request;
  std::get<MockControllerArtifactsInput>(changed.lane)
      .initial_joint_positions_rad[0] = 0.125;
  EXPECT_NE(write_controller_launch_artifacts(changed)
                .mock_reset_position->controller_name,
            target.controller_name);
  changed = request;
  std::swap(changed.arm_joint_names[0], changed.arm_joint_names[1]);
  EXPECT_NE(write_controller_launch_artifacts(changed)
                .mock_reset_position->controller_name,
            target.controller_name);
  changed = request;
  changed.component_runtime_identity =
      manyforge::content_identity::sha256_digest("different-provider-config");
  EXPECT_NE(write_controller_launch_artifacts(changed)
                .mock_reset_position->controller_name,
            target.controller_name);
  changed.component_runtime_identity.clear();
  EXPECT_THROW(static_cast<void>(write_controller_launch_artifacts(changed)),
               ControllerLaunchArtifactError);
}

TEST(ControllerLaunchArtifactsHost, OptionalMotionPreservesExistingArmArtifacts) {
  const TemporaryRoot root;
  for (const auto &profile : offline_profiles()) {
    SCOPED_TRACE(profile.mock_profile);
    const auto component = mock_component(root.path(), "robot", profile, "robot/", 10.0);
    const auto existing = write_host(component);
    const auto optional = write_component_controller_launch_artifacts(
        std::optional<core::RuntimeParticipantBinding>{component.motion},
        component.input);
    EXPECT_EQ(optional.robot_description_sha256, existing.robot_description_sha256);
    EXPECT_EQ(optional.controllers_sha256, existing.controllers_sha256);
    EXPECT_EQ(optional.controllers_yaml, existing.controllers_yaml);
    EXPECT_EQ(optional.arm_controller_name, existing.arm_controller_name);
    EXPECT_EQ(optional.gripper_controller_name, existing.gripper_controller_name);
    EXPECT_EQ(optional.launch_arguments, existing.launch_arguments);
    EXPECT_EQ(controller_launch_artifacts_evidence(optional),
              controller_launch_artifacts_evidence(existing));
  }
}

TEST(ControllerLaunchArtifactsHost, GripperOnlyInputRefusesMissingOrForeignArmFacts) {
  const TemporaryRoot root;
  const auto profiles = offline_profiles();
  auto component = mock_component(root.path(), "tool", profiles.at(0), "", 10.0);
  component.input.initial_joint_positions_rad.clear();
  component.input.controlled_stop_max_deceleration_rad_s2.clear();
  const auto absent = std::optional<core::RuntimeParticipantBinding>{};
  const auto refused = [&](const ComponentControllerLaunchInput &input) {
    EXPECT_THROW(static_cast<void>(
                     write_component_controller_launch_artifacts(absent, input)),
                 ControllerLaunchArtifactError);
  };
  auto invalid = component.input;
  invalid.gripper_binding.reset();
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = gripper_binding("tool", std::nullopt);
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = participant_binding(
      "tool", "gripper", "execution.trajectory.execute",
      {{"tool.gripper.coordinate.000000", "right_finger_joint"}});
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = participant_binding(
      "tool", "gripper", "tool.gripper.command",
      {{"tool.gripper.first", "right_finger_joint"},
       {"tool.gripper.second", "left_finger_joint"}});
  refused(invalid);
  invalid = component.input;
  invalid.gripper_projected_position.reset();
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = gripper_binding("tool", "missing_joint");
  refused(invalid);
  invalid = component.input;
  invalid.initial_joint_positions_rad = {0.0};
  refused(invalid);
  invalid = component.input;
  invalid.controlled_stop_max_deceleration_rad_s2.emplace_back(
      manyforge::core::ResourceId("other.arm.coordinate.000000"), 10.0);
  refused(invalid);
  invalid = component.input;
  invalid.mock_profile = "ur5_roboplan";
  refused(invalid);
  invalid = component.input;
  invalid.isaac_profile = "lite6";
  refused(invalid);
  invalid = component.input;
  invalid.mock_profile.reset();
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = gripper_binding("foreign", "right_finger_joint");
  EXPECT_THROW(static_cast<void>(write_component_controller_launch_artifacts(
                   std::optional<core::RuntimeParticipantBinding>{component.motion},
                   invalid)),
               ControllerLaunchArtifactError);
}

TEST(ControllerLaunchArtifactsHost, NoOutputMockKeepsArmAndOmitsPhysicalGripper) {
  const TemporaryRoot root;
  for (const auto& profile : offline_profiles()) {
    if (!profile.gripper_joint.has_value()) continue;
    SCOPED_TRACE(profile.mock_profile);
    auto component = mock_component(root.path(), "robot", profile, "robot/", 10.0);
    const auto commanded = write_host(component);
    std::vector<std::string> arm_arguments;
    for (const auto& argument : commanded.launch_arguments) {
      if (!argument.starts_with("gripper_controller_name:=")) arm_arguments.push_back(argument);
    }
    component.input.gripper_output_mode = core::GripperOutputMode::kNone;
    component.input.gripper_projected_position.reset();
    const auto none = write_host(component);
    EXPECT_EQ(none.arm_controller_name, commanded.arm_controller_name);
    EXPECT_EQ(none.controller_profile, commanded.controller_profile);
    EXPECT_EQ(none.runtime_namespace, commanded.runtime_namespace);
    EXPECT_FALSE(none.gripper_controller_name.has_value());
    EXPECT_EQ(none.launch_arguments, arm_arguments);
    const auto yaml = read_file(none.controllers_file);
    EXPECT_NE(yaml.find("  ros__parameters:\n"), std::string::npos);
    EXPECT_NE(yaml.find("    joint_state_broadcaster:"), std::string::npos);
    EXPECT_NE(yaml.find("/**/" + profile.arm_controller + ":"), std::string::npos);
    EXPECT_EQ(yaml.find(*profile.gripper_controller), std::string::npos);
    EXPECT_EQ(yaml.find("GripperActionController"), std::string::npos);
    EXPECT_EQ(yaml.find(*profile.gripper_joint), std::string::npos);
    for (const auto& joint : profile.arm) {
      EXPECT_NE(yaml.find(joint + ": {"), std::string::npos);
    }
    const auto description = read_file(none.robot_description_file);
    const auto section = description.find("  <ros2_control");
    ASSERT_NE(section, std::string::npos);
    const auto hardware = description.substr(section);
    EXPECT_EQ(count(hardware, "<joint name="), profile.arm.size());
    EXPECT_EQ(hardware.find(*profile.gripper_joint), std::string::npos);
    for (const auto& joint : profile.arm) {
      EXPECT_NE(hardware.find("<joint name=\"" + joint + "\">"), std::string::npos);
    }
    EXPECT_EQ(none.robot_description_sha256,
              manyforge::content_identity::sha256_digest(description));
    EXPECT_EQ(none.controllers_sha256, manyforge::content_identity::sha256_digest(yaml));
    component.input.gripper_output_mode = core::GripperOutputMode::kCommanded;
    component.input.gripper_projected_position = 0.0042;
    const auto explicit_commanded = write_host(component);
    EXPECT_EQ(explicit_commanded.controllers_yaml, commanded.controllers_yaml);
    EXPECT_EQ(explicit_commanded.robot_description_sha256, commanded.robot_description_sha256);
    EXPECT_EQ(explicit_commanded.launch_arguments, commanded.launch_arguments);
  }
}

TEST(ControllerLaunchArtifactsHost, NoOutputRefusesMissingForeignOrInvalidBinding) {
  const TemporaryRoot root;
  auto component = mock_component(root.path(), "robot", offline_profiles().front(), "", 10.0);
  component.input.gripper_output_mode = core::GripperOutputMode::kNone;
  const auto refused = [&](const ComponentControllerLaunchInput& input) {
    EXPECT_THROW(static_cast<void>(write_component_controller_launch_artifacts(component.motion, input)),
                 ControllerLaunchArtifactError);
  };
  auto invalid = component.input;
  invalid.gripper_binding.reset();
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = gripper_binding("robot", std::nullopt);
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = gripper_binding("foreign", "right_finger_joint");
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = gripper_binding("robot", "missing");
  refused(invalid);
  invalid = component.input;
  invalid.gripper_binding = gripper_binding("robot", "joint1");
  refused(invalid);
  invalid = component.input;
  invalid.gripper_output_mode = static_cast<core::GripperOutputMode>(99);
  refused(invalid);
  EXPECT_THROW(static_cast<void>(write_component_controller_launch_artifacts(
                   std::optional<core::RuntimeParticipantBinding>{}, component.input)),
               ControllerLaunchArtifactError);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "robot"));
}

TEST(ControllerLaunchArtifactsHost, NoOutputIsaacKeepsTheActualArmArtifacts) {
  const TemporaryRoot root;
  const auto profile = offline_profiles().front();
  auto component = mock_component(root.path(), "robot", profile, "robot/", 10.0);
  component.input.mock_profile.reset();
  component.input.isaac_profile = "lite6";
  component.input.isaac_lane = IsaacControllerLane{
      "IsaacLite6TopicBasedSystem", "/isaac_joint_commands", "/isaac_joint_states",
      {0.0, 0.0, 1.57, 0.0, 0.0, 0.0}};
  const auto commanded = write_host(component);
  component.input.gripper_output_mode = core::GripperOutputMode::kNone;
  component.input.gripper_projected_position.reset();
  const auto none = write_host(component);
  EXPECT_EQ(none.robot_description_sha256, commanded.robot_description_sha256);
  EXPECT_EQ(none.controllers_sha256, commanded.controllers_sha256);
  EXPECT_EQ(none.launch_environment, commanded.launch_environment);
  EXPECT_EQ(none.arm_controller_name, commanded.arm_controller_name);
  EXPECT_FALSE(none.gripper_controller_name.has_value());
  component.input.gripper_binding = gripper_binding("robot", "missing");
  EXPECT_THROW(static_cast<void>(write_host(component)), ControllerLaunchArtifactError);
}

TEST(ControllerLaunchArtifacts, NoOutputYamlKeepsArmLimitsAndRequiresExplicitJoint) {
  const auto profile = mock_controller_profile_facts("lite6_lite_gripper");
  const auto limits = cancel_limits(kUFactoryArm, 10.0);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   profile, "robot", kUFactoryArm, limits, std::nullopt,
                   core::GripperOutputMode::kNone)), ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   profile, "robot", {}, {}, "right_finger_joint",
                   core::GripperOutputMode::kNone)), ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   profile, "robot", kUFactoryArm, {}, "right_finger_joint",
                   core::GripperOutputMode::kNone)), ControllerLaunchArtifactError);
  EXPECT_THROW(static_cast<void>(generate_mock_controllers_yaml(
                   profile, "robot", kUFactoryArm, limits, "right_finger_joint",
                   static_cast<core::GripperOutputMode>(99))), ControllerLaunchArtifactError);
}

TEST(ControllerLaunchArtifactsHost, HostInputsFailClosed) {
  const TemporaryRoot root;
  const auto profiles = offline_profiles();
  auto both =
      mock_component(root.path(), "lite6_robot", profiles.at(0), "", 10.0);
  both.input.isaac_profile = "lite6";
  EXPECT_THROW(static_cast<void>(write_host(both)),
               ControllerLaunchArtifactError);
  auto foreign =
      mock_component(root.path(), "lite6_robot", profiles.at(0), "", 10.0);
  foreign.input.controlled_stop_max_deceleration_rad_s2.emplace_back(
      core::ResourceId("other.arm.coordinate.000000"), 10.0);
  EXPECT_THROW(
      static_cast<void>(write_host(foreign)),
      ControllerLaunchArtifactError);
  auto missing =
      mock_component(root.path(), "lite6_robot", profiles.at(0), "", 10.0);
  missing.input.controlled_stop_max_deceleration_rad_s2.pop_back();
  EXPECT_THROW(
      static_cast<void>(write_host(missing)),
      ControllerLaunchArtifactError);
  auto no_projection =
      mock_component(root.path(), "lite6_robot", profiles.at(0), "", 10.0);
  no_projection.input.gripper_projected_position.reset();
  EXPECT_THROW(
      static_cast<void>(write_host(no_projection)),
      ControllerLaunchArtifactError);
}

} // namespace

namespace {

TEST(ControllerLaunchArtifacts, MockProfilesCarryQ79FamilyValuesAndTheTemplateCadence) {
  // Q79: Isaac's values per model family, no new numbers.
  const auto lite6 = mock_controller_profile_facts("lite6_lite_gripper");
  EXPECT_DOUBLE_EQ(lite6.hard_stop_completion_timeout_s, 1.5);
  EXPECT_DOUBLE_EQ(lite6.gripper_result_timeout_s, 5.0);
  for (const auto *ur : {"ur10e_robotiq_2f_140", "ur5e_robotiq_2f_85",
                         "ur5_roboplan"}) {
    const auto facts = mock_controller_profile_facts(ur);
    EXPECT_DOUBLE_EQ(facts.hard_stop_completion_timeout_s, 0.5) << ur;
    EXPECT_DOUBLE_EQ(facts.gripper_result_timeout_s, 15.0) << ur;
  }
  EXPECT_FALSE(mock_controller_profile_facts("ur5_roboplan")
                   .gripper_controller_name.has_value());
  // The participant's lane cadence is the one the Track 2 template runs.
  const auto ufactory = mock_controller_lane_rates("lite6_lite_gripper");
  EXPECT_DOUBLE_EQ(ufactory.controller_update_rate_hz, 150.0);
  EXPECT_DOUBLE_EQ(ufactory.state_publish_rate_hz, 25.0);
  EXPECT_DOUBLE_EQ(ufactory.action_monitor_rate_hz, 20.0);
  for (const auto *ur : {"ur10e_robotiq_2f_140", "ur5e_robotiq_2f_85",
                         "ur5_roboplan"}) {
    const auto rates = mock_controller_lane_rates(ur);
    EXPECT_DOUBLE_EQ(rates.controller_update_rate_hz, 100.0) << ur;
    EXPECT_DOUBLE_EQ(rates.state_publish_rate_hz, 100.0) << ur;
    EXPECT_DOUBLE_EQ(rates.action_monitor_rate_hz, 20.0) << ur;
  }
  EXPECT_THROW(static_cast<void>(mock_controller_lane_rates("unknown")),
               ControllerLaunchArtifactError);
}

} // namespace
