#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_deployment/model_source.hpp>
#include <manyforge_roboplan/retained_static_frames.hpp>

namespace manyforge::roboplan_backend {
namespace {

namespace runtime_contract = deployment::ros2_control_execution_runtime_contract;

// TCP-A: the TF static publication is the robot_state_publisher description:
// each static frame becomes one empty link and one fixed joint with the
// frame's origin, appended before </robot>; every other byte is kept.
TEST(RetainedStaticFrames, DescriptionCarriesOffsetFramesAsFixedLinks) {
  const std::string urdf =
      "<?xml version=\"1.0\"?>\n<robot name=\"arm\"><link name=\"link_eef\"/>"
      "<link name=\"link_base\"/><joint name=\"j\" type=\"fixed\"><parent "
      "link=\"link_base\"/><child link=\"link_eef\"/></joint></robot>\n";
  const core::PlacementPoseDTO pen{{0.0, 0.0, 0.12}, {0.0, 0.0, 0.0, 1.0}};
  const std::vector<core::ModelFrameRoleBindingDTO> roles{
      {core::FrameRoleId("flange"), "link_eef"},
      {core::FrameRoleId("pen"), "link_eef", pen}};
  const std::vector<core::ModelCustomTcpDTO> custom{
      {core::FrameRoleId("probe"), "pen", {{0.01, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}}};
  const auto frames = deployment::occurrence_offset_frames(roles, custom);
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[0].link_name, "pen");
  EXPECT_FALSE(frames[0].custom);
  EXPECT_EQ(frames[1].parent_link_name, "pen");
  EXPECT_TRUE(frames[1].custom);

  std::vector<deployment::UrdfJointIr> joints;
  for (const auto &frame : frames) joints.push_back(deployment::offset_frame_joint(frame));
  const auto description = deployment::append_fixed_links(urdf, joints);
  EXPECT_EQ(description.substr(0U, urdf.rfind("</robot>")),
            urdf.substr(0U, urdf.rfind("</robot>")));
  EXPECT_NE(description.find("<link name=\"pen\"/><joint name=\"~tcp/pen\" type=\"fixed\">"
                             "<origin xyz=\"0 0 0.12\"/><parent link=\"link_eef\"/>"
                             "<child link=\"pen\"/></joint>"),
            std::string::npos);
  EXPECT_NE(description.find("<link name=\"probe\"/><joint name=\"~tcp/probe\" type=\"fixed\">"
                             "<origin xyz=\"0.01 0 0\"/><parent link=\"pen\"/>"
                             "<child link=\"probe\"/></joint>"),
            std::string::npos);
  EXPECT_EQ(description.substr(description.size() - 9U), "</robot>\n");
  // The result is a URDF robot_state_publisher can read: it parses, and the
  // new links hang off their parents.
  const auto parsed = deployment::parse_model_documents_for_projection(description, std::nullopt);
  EXPECT_EQ(parsed.urdf.links.size(), 4U);
  EXPECT_EQ(parsed.urdf.joints.size(), 3U);

  EXPECT_EQ(deployment::append_fixed_links(urdf, {}), urdf);
  EXPECT_THROW((void)deployment::append_fixed_links("<robot name=\"x\">", joints),
               deployment::ModelSourceError);
}

// A process-less tool's static part: the links fixed to its root, with
// renamed joints; a moving finger and what hangs below it stay out.
TEST(RetainedStaticFrames, FixedLinksBelowStopAtMovingJoints) {
  const auto parsed = deployment::parse_model_documents_for_projection(
      "<robot name=\"tool\"><link name=\"mount\"/><link name=\"body\"/>"
      "<link name=\"tcp_link\"/><link name=\"finger\"/><link name=\"pad\"/>"
      "<joint name=\"a\" type=\"fixed\"><parent link=\"mount\"/><child link=\"body\"/>"
      "<origin xyz=\"0 0 0.05\"/></joint>"
      "<joint name=\"b\" type=\"fixed\"><parent link=\"body\"/><child link=\"tcp_link\"/></joint>"
      "<joint name=\"c\" type=\"prismatic\"><parent link=\"body\"/><child link=\"finger\"/>"
      "<axis xyz=\"1 0 0\"/><limit lower=\"0\" upper=\"0.04\" effort=\"1\" velocity=\"1\"/></joint>"
      "<joint name=\"d\" type=\"fixed\"><parent link=\"finger\"/><child link=\"pad\"/></joint>"
      "</robot>",
      std::nullopt);
  const auto joints = fixed_links_below(parsed.urdf, "mount");
  ASSERT_EQ(joints.size(), 2U);
  std::vector<std::string> children;
  for (const auto &joint : joints) {
    children.push_back(joint.child_link_name);
    EXPECT_EQ(joint.name, "~fixed/" + joint.child_link_name);
  }
  std::sort(children.begin(), children.end());
  EXPECT_EQ(children, (std::vector<std::string>{"body", "tcp_link"}));
}

TEST(RetainedStaticFrames, OnlyANoOutputToolRunsNoProcess) {
  runtime_contract::Configuration configuration{};
  const core::ControllableComponentId robot("robot"), tool("tool"), mock("mock_tool");
  configuration.motion_target_limits.push_back(
      {robot, core::CommandTargetId("arm"), 1.0, {}});
  const auto row = [](const core::ControllableComponentId &id, core::GripperOutputMode mode) {
    deployment::execution_runtime_contract::Configuration::GripperTargetConfiguration base{
        id, core::CommandTargetId("gripper"), {}, "pneumatic", {}, {}, 0.0, {}, {}, {}, {}, {}};
    return runtime_contract::Configuration::GripperTargetConfiguration(std::move(base), mode);
  };
  configuration.gripper_target_configurations.push_back(row(tool, core::GripperOutputMode::kNone));
  configuration.gripper_target_configurations.push_back(
      row(mock, core::GripperOutputMode::kCommanded));
  EXPECT_FALSE(retained_component_runs_no_process(configuration, robot));
  EXPECT_TRUE(retained_component_runs_no_process(configuration, tool));
  EXPECT_FALSE(retained_component_runs_no_process(configuration, mock));
  EXPECT_FALSE(retained_component_runs_no_process(configuration, core::ControllableComponentId("absent")));
}

} // namespace
} // namespace manyforge::roboplan_backend
