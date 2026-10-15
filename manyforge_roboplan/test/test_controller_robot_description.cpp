#include <manyforge_roboplan/controller_robot_description.hpp>

#include <expat.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace manyforge::roboplan_backend;

// ---------------------------------------------------------------------------
// A tiny independent XML tree (test oracle) built with Expat. Whitespace-only
// character data is not significant; everything else is compared exactly.

struct Node final {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::string text;
  std::vector<Node> children;

  [[nodiscard]] std::string attribute(const std::string &key) const {
    for (const auto &[name_, value] : attributes) {
      if (name_ == key) return value;
    }
    return {};
  }
  [[nodiscard]] std::vector<const Node *> all(const std::string &tag) const {
    std::vector<const Node *> out;
    for (const auto &child : children) {
      if (child.name == tag) out.push_back(&child);
    }
    return out;
  }
};

struct TreeBuilder final {
  std::vector<Node *> stack;
  std::optional<Node> root;
};

void XMLCALL tree_start(void *data, const XML_Char *name, const XML_Char **atts) {
  auto &builder = *static_cast<TreeBuilder *>(data);
  Node node;
  node.name = name;
  for (std::size_t index = 0; atts[index] != nullptr; index += 2U) {
    node.attributes.emplace_back(atts[index], atts[index + 1U]);
  }
  if (builder.stack.empty()) {
    builder.root = std::move(node);
    builder.stack.push_back(&*builder.root);
  } else {
    builder.stack.back()->children.push_back(std::move(node));
    builder.stack.push_back(&builder.stack.back()->children.back());
  }
}

void XMLCALL tree_end(void *data, const XML_Char *) {
  static_cast<TreeBuilder *>(data)->stack.pop_back();
}

void XMLCALL tree_text(void *data, const XML_Char *text, int length) {
  auto &builder = *static_cast<TreeBuilder *>(data);
  if (!builder.stack.empty()) builder.stack.back()->text.append(text, length);
}

[[nodiscard]] Node parse(const std::string &xml) {
  TreeBuilder builder;
  const std::unique_ptr<std::remove_pointer_t<XML_Parser>,
                        decltype(&XML_ParserFree)>
      parser(XML_ParserCreate("UTF-8"), &XML_ParserFree);
  XML_SetUserData(parser.get(), &builder);
  XML_SetElementHandler(parser.get(), &tree_start, &tree_end);
  XML_SetCharacterDataHandler(parser.get(), &tree_text);
  if (XML_Parse(parser.get(), xml.data(), static_cast<int>(xml.size()),
                XML_TRUE) != XML_STATUS_OK) {
    throw std::runtime_error(std::string("oracle parse failed: ") +
                             XML_ErrorString(XML_GetErrorCode(parser.get())));
  }
  return std::move(*builder.root);
}

[[nodiscard]] std::string trimmed(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1U);
}

[[nodiscard]] std::string canonical(const Node &node) {
  std::string out = "<" + node.name;
  for (const auto &[name, value] : node.attributes) {
    out += " " + name + "=\"" + value + "\"";
  }
  out += ">" + trimmed(node.text);
  for (const auto &child : node.children) out += canonical(child);
  return out + "</" + node.name + ">";
}

/// Every top-level child except <ros2_control>, in order, canonicalized.
[[nodiscard]] std::vector<std::string> model_children(const Node &robot) {
  std::vector<std::string> out;
  for (const auto &child : robot.children) {
    if (child.name != "ros2_control") out.push_back(canonical(child));
  }
  return out;
}

[[nodiscard]] std::size_t count_elements(const Node &node,
                                         const std::string &tag) {
  std::size_t count = node.name == tag ? 1U : 0U;
  for (const auto &child : node.children) count += count_elements(child, tag);
  return count;
}

[[nodiscard]] std::string read_repository_file(const std::string &relative) {
  const std::string path = std::string(MANYFORGE_TEST_REPOSITORY_ROOT) + "/" +
                           relative;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

[[nodiscard]] std::string g17(const double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return buffer;
}

/// Python str.format of the stored Isaac templates with the given values.
[[nodiscard]] std::string format_template(
    std::string text, const std::map<std::string, std::string> &values) {
  for (const auto &[key, value] : values) {
    const std::string placeholder = "{" + key + "}";
    for (auto at = text.find(placeholder); at != std::string::npos;
         at = text.find(placeholder, at + value.size())) {
      text.replace(at, placeholder.size(), value);
    }
  }
  if (text.find('{') != std::string::npos) {
    throw std::runtime_error("unformatted placeholder left in template");
  }
  return text;
}

struct ExpectedJoint final {
  std::string name;
  double initial_value{};
};

void expect_hardware(const Node &section, const std::string &component_name,
                     const std::string &plugin,
                     const std::vector<std::pair<std::string, std::string>> &params,
                     const std::vector<ExpectedJoint> &joints) {
  EXPECT_EQ(section.attribute("name"), component_name);
  EXPECT_EQ(section.attribute("type"), "system");
  ASSERT_EQ(section.attributes.size(), 2U);
  const auto hardware = section.all("hardware");
  ASSERT_EQ(hardware.size(), 1U);
  const auto plugins = hardware.front()->all("plugin");
  ASSERT_EQ(plugins.size(), 1U);
  EXPECT_EQ(trimmed(plugins.front()->text), plugin);
  const auto found_params = hardware.front()->all("param");
  ASSERT_EQ(found_params.size(), params.size());
  for (std::size_t index = 0; index < params.size(); ++index) {
    EXPECT_EQ(found_params[index]->attribute("name"), params[index].first);
    EXPECT_EQ(trimmed(found_params[index]->text), params[index].second);
  }
  EXPECT_EQ(hardware.front()->children.size(), 1U + params.size());
  const auto found_joints = section.all("joint");
  ASSERT_EQ(found_joints.size(), joints.size());
  ASSERT_EQ(section.children.size(), 1U + joints.size());
  for (std::size_t index = 0; index < joints.size(); ++index) {
    const auto &joint = *found_joints[index];
    SCOPED_TRACE(joints[index].name);
    EXPECT_EQ(joint.attribute("name"), joints[index].name);
    ASSERT_EQ(joint.children.size(), 3U);
    EXPECT_EQ(canonical(joint.children[0]),
              "<command_interface name=\"position\"></command_interface>");
    EXPECT_EQ(canonical(joint.children[1]),
              "<state_interface name=\"position\"><param "
              "name=\"initial_value\">" +
                  g17(joints[index].initial_value) +
                  "</param></state_interface>");
    EXPECT_EQ(std::stod(trimmed(joint.children[1].children.at(0).text)),
              joints[index].initial_value);
    EXPECT_EQ(canonical(joint.children[2]),
              "<state_interface name=\"velocity\"></state_interface>");
  }
}

struct OfflineModel final {
  std::string path;
  std::vector<std::string> arm_joints;
  std::optional<std::string> gripper_joint;
  std::size_t source_ros2_control_count{};
};

/// The model URDFs the offline profiles use, with the joints their retained
/// bindings name (manyforge_deployment maintained sources).
[[nodiscard]] std::vector<OfflineModel> offline_models() {
  const std::vector<std::string> ufactory{"joint1", "joint2", "joint3",
                                          "joint4", "joint5", "joint6"};
  const std::vector<std::string> ur{"shoulder_pan_joint", "shoulder_lift_joint",
                                    "elbow_joint",        "wrist_1_joint",
                                    "wrist_2_joint",      "wrist_3_joint"};
  return {
      {"manyforge_example_models/models/lite6_lite_gripper/"
       "lite6_lite_gripper.urdf",
       ufactory, "right_finger_joint", 2U},
      {"manyforge_example_models/models/uf850_xarm_gripper/"
       "uf850_xarm_gripper.urdf",
       ufactory, "drive_joint", 1U},
      {"manyforge_example_models/models/ur10e_robotiq_2f_140/"
       "ur10e_robotiq_2f_140.urdf",
       ur, "finger_joint", 0U},
      {"manyforge_example_models/models/ur5e_robotiq_2f_85/"
       "ur5e_robotiq_2f_85.urdf",
       ur, "robotiq_85_left_knuckle_joint", 0U},
      {"manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/"
       "ur5_gripper.structural.urdf",
       ur, std::nullopt, 0U},
  };
}

[[nodiscard]] std::vector<double> ramp(const std::size_t count) {
  std::vector<double> values;
  for (std::size_t index = 0; index < count; ++index) {
    values.push_back(-0.75 + 0.3125 * static_cast<double>(index));
  }
  return values;
}

void expect_model_unchanged(const std::string &model_text,
                            const std::string &generated,
                            const std::string &section_name,
                            const std::size_t source_ros2_control_count) {
  const auto model = parse(model_text);
  const auto output = parse(generated);
  EXPECT_EQ(count_elements(model, "ros2_control"), source_ros2_control_count);
  EXPECT_EQ(count_elements(output, "ros2_control"), 1U);
  EXPECT_EQ(output.attributes, model.attributes);
  // Links, joints (limits, mimic, safety controllers), transmissions,
  // simulator tags and every other element are unchanged and in order.
  EXPECT_EQ(model_children(output), model_children(model));
  ASSERT_FALSE(output.children.empty());
  EXPECT_EQ(output.children.back().name, "ros2_control");
  EXPECT_EQ(output.children.back().attribute("name"), section_name);
  if (source_ros2_control_count == 0U) {
    // Byte-level: removing the one inserted section restores the model.
    const auto begin = generated.find("  <ros2_control name=\"" + section_name);
    const std::string closing = "  </ros2_control>\n";
    const auto end = generated.find(closing, begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    auto restored = generated;
    restored.erase(begin, end + closing.size() - begin);
    EXPECT_EQ(restored, model_text);
  }
}

[[nodiscard]] ControllerRobotDescriptionRequest
mock_request(const std::string &urdf, const std::vector<std::string> &arm,
             std::vector<double> initial,
             std::optional<MockGripperBinding> gripper = std::nullopt,
             const bool gripper_required = false) {
  return ControllerRobotDescriptionRequest{
      urdf, arm,
      MockControllerLane{"ManyForgeMockSystem", std::move(initial),
                         std::move(gripper), gripper_required}};
}

// ---------------------------------------------------------------------------

TEST(ControllerRobotDescription, MockFormForEveryOfflineModel) {
  for (const auto &model : offline_models()) {
    SCOPED_TRACE(model.path);
    const auto text = read_repository_file(model.path);
    const auto initial = ramp(model.arm_joints.size());
    std::optional<MockGripperBinding> gripper;
    if (model.gripper_joint.has_value()) {
      gripper = MockGripperBinding{model.gripper_joint, 0.0125};
    }
    const auto generated = generate_controller_robot_description(
        mock_request(text, model.arm_joints, initial, gripper,
                     model.gripper_joint.has_value()));
    expect_model_unchanged(text, generated, "ManyForgeMockSystem",
                           model.source_ros2_control_count);
    std::vector<ExpectedJoint> expected;
    for (std::size_t index = 0; index < model.arm_joints.size(); ++index) {
      expected.push_back({model.arm_joints[index], initial[index]});
    }
    if (model.gripper_joint.has_value()) {
      expected.push_back({*model.gripper_joint, 0.0125});
    }
    const auto output = parse(generated);
    expect_hardware(output.children.back(), "ManyForgeMockSystem",
                    "mock_components/GenericSystem",
                    {{"calculate_dynamics", "true"}}, expected);
  }
}

TEST(ControllerRobotDescription, NoOutputMockPreservesGeometryAndOnlyControlsArm) {
  for (const auto& model : offline_models()) {
    if (!model.gripper_joint.has_value()) continue;
    SCOPED_TRACE(model.path);
    const auto text = read_repository_file(model.path);
    const auto initial = ramp(model.arm_joints.size());
    auto request = mock_request(text, model.arm_joints, initial,
                                MockGripperBinding{model.gripper_joint, std::nullopt}, true);
    std::get<MockControllerLane>(request.lane).gripper_output_mode =
        manyforge::core::GripperOutputMode::kNone;
    const auto generated = generate_controller_robot_description(request);
    expect_model_unchanged(text, generated, "ManyForgeMockSystem",
                           model.source_ros2_control_count);
    std::vector<ExpectedJoint> expected;
    for (std::size_t index = 0; index < model.arm_joints.size(); ++index) {
      expected.push_back({model.arm_joints[index], initial[index]});
    }
    expect_hardware(parse(generated).children.back(), "ManyForgeMockSystem",
                    "mock_components/GenericSystem",
                    {{"calculate_dynamics", "true"}}, expected);
    // An actual supplied scalar may be validated, but it is no hardware seed.
    std::get<MockControllerLane>(request.lane).gripper->projected_initial_position = 0.0125;
    EXPECT_EQ(generate_controller_robot_description(request), generated);
  }
}

TEST(ControllerRobotDescription, ExplicitCommandedPreservesTheDefaultDescription) {
  for (const auto& model : offline_models()) {
    SCOPED_TRACE(model.path);
    const auto text = read_repository_file(model.path);
    const auto initial = ramp(model.arm_joints.size());
    const auto gripper = model.gripper_joint.has_value()
        ? std::optional(MockGripperBinding{model.gripper_joint, 0.0125})
        : std::nullopt;
    auto request = mock_request(text, model.arm_joints, initial, gripper,
                                model.gripper_joint.has_value());
    const auto original = generate_controller_robot_description(request);
    std::get<MockControllerLane>(request.lane).gripper_output_mode =
        manyforge::core::GripperOutputMode::kCommanded;
    EXPECT_EQ(generate_controller_robot_description(request), original);
  }
}

TEST(ControllerRobotDescription, GripperOnlyMockPreservesEveryModelElement) {
  for (const auto &model : offline_models()) {
    if (!model.gripper_joint.has_value()) continue;
    SCOPED_TRACE(model.path);
    const auto text = read_repository_file(model.path);
    const auto generated = generate_controller_robot_description(mock_request(
        text, {}, {}, MockGripperBinding{model.gripper_joint, 0.0125}, true));
    expect_model_unchanged(text, generated, "ManyForgeMockSystem",
                           model.source_ros2_control_count);
    expect_hardware(parse(generated).children.back(), "ManyForgeMockSystem",
                    "mock_components/GenericSystem",
                    {{"calculate_dynamics", "true"}},
                    {{*model.gripper_joint, 0.0125}});
  }
}

TEST(ControllerRobotDescription, IsaacFormForEveryOfflineModel) {
  for (const auto &model : offline_models()) {
    SCOPED_TRACE(model.path);
    const auto text = read_repository_file(model.path);
    const auto initial = ramp(model.arm_joints.size());
    const auto generated = generate_controller_robot_description(
        ControllerRobotDescriptionRequest{
            text, model.arm_joints,
            IsaacControllerLane{"IsaacTestTopicBasedSystem", "/cmd", "/state",
                                initial}});
    expect_model_unchanged(text, generated, "IsaacTestTopicBasedSystem",
                           model.source_ros2_control_count);
    std::vector<ExpectedJoint> expected;
    for (std::size_t index = 0; index < model.arm_joints.size(); ++index) {
      expected.push_back({model.arm_joints[index], initial[index]});
    }
    // The gripper stays on its own bridge: arm joints only.
    expect_hardware(parse(generated).children.back(),
                    "IsaacTestTopicBasedSystem",
                    "topic_based_ros2_control/TopicBasedSystem",
                    {{"joint_commands_topic", "/cmd"},
                     {"joint_states_topic", "/state"},
                     {"trigger_joint_command_threshold", "-1"},
                     {"sum_wrapped_joint_states", "false"}},
                    expected);
  }
}

constexpr const char *kSynthetic = R"(<?xml version="1.0" encoding="utf-8"?>
<!-- vendor wrapper with two hardware blocks -->
<robot name="synthetic" xmlns:vendor="urn:vendor" vendor:flavour="x">
  <ros2_control name="ArmHardware" type="system">
    <hardware><plugin>vendor/Driver</plugin><param name="robot_ip">10.0.0.2</param></hardware>
    <joint name="j1"><command_interface name="position"/><state_interface name="position"/></joint>
    <gpio name="io"><state_interface name="di0"/></gpio>
    <sensor name="ft"><state_interface name="force.x"/></sensor>
  </ros2_control>
  <link name="base"><vendor:extra value="kept"/></link>
  <link name="l1"/>
  <link name="l2"/>
  <link name="finger_l"/>
  <link name="finger_r"/>
  <joint name="j1" type="revolute" vendor:note="keep">
    <parent link="base"/><child link="l1"/>
    <limit lower="-4.1" upper="4.1" effort="12" velocity="2.5"/>
    <safety_controller soft_lower_limit="-4" soft_upper_limit="4" k_position="10" k_velocity="1"/>
  </joint>
  <joint name="j2" type="continuous"><parent link="l1"/><child link="l2"/></joint>
  <joint name="finger" type="prismatic"><parent link="l2"/><child link="finger_r"/>
    <limit lower="0" upper="0.04" effort="5" velocity="0.1"/></joint>
  <joint name="finger_mimic" type="prismatic"><parent link="l2"/><child link="finger_l"/>
    <mimic joint="finger" multiplier="-1" offset="0"/><limit lower="-0.04" upper="0" effort="5" velocity="0.1"/></joint>
  <joint name="fixed_tool" type="fixed"><parent link="l2"/><child link="base"/></joint>
  <transmission name="t1"><type>transmission_interface/SimpleTransmission</type></transmission>
  <gazebo><plugin filename="x.so" name="x"/></gazebo>
  <unknown_vendor_element attr="1">text &amp; more</unknown_vendor_element>
  <ros2_control name="HandHardware" type="system"><hardware><plugin>vendor/Hand</plugin></hardware></ros2_control>
  <ros2_control name="Empty" type="system"/>
</robot>
)";

TEST(ControllerRobotDescription, RemovesEveryHardwareBlockAndPassesUnknownContentThrough) {
  const std::string text = kSynthetic;
  const auto generated = generate_controller_robot_description(mock_request(
      text, {"j1", "j2"}, {0.5, -0.25}, MockGripperBinding{"finger", 0.02},
      true));
  expect_model_unchanged(text, generated, "ManyForgeMockSystem", 3U);
  EXPECT_EQ(generated.find("ArmHardware"), std::string::npos);
  EXPECT_EQ(generated.find("HandHardware"), std::string::npos);
  EXPECT_EQ(generated.find("vendor/Driver"), std::string::npos);
  EXPECT_EQ(generated.find("di0"), std::string::npos);
  EXPECT_EQ(generated.find("force.x"), std::string::npos);
  // Comments, the declaration, namespaced attributes and unknown elements are
  // carried byte-for-byte.
  EXPECT_EQ(generated.rfind("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                            "<!-- vendor wrapper with two hardware blocks -->\n",
                            0),
            0U);
  EXPECT_NE(generated.find("<robot name=\"synthetic\" xmlns:vendor=\"urn:vendor\" "
                           "vendor:flavour=\"x\">"),
            std::string::npos);
  EXPECT_NE(generated.find("<unknown_vendor_element attr=\"1\">text &amp; "
                           "more</unknown_vendor_element>"),
            std::string::npos);
  EXPECT_NE(generated.find("<safety_controller soft_lower_limit=\"-4\""),
            std::string::npos);
  expect_hardware(parse(generated).children.back(), "ManyForgeMockSystem",
                  "mock_components/GenericSystem",
                  {{"calculate_dynamics", "true"}},
                  {{"j1", 0.5}, {"j2", -0.25}, {"finger", 0.02}});
  // The mimic follower stays a mimic joint and is not listed.
  EXPECT_EQ(generated.find("<joint name=\"finger_mimic\">"), std::string::npos);
  EXPECT_NE(generated.find("<mimic joint=\"finger\" multiplier=\"-1\""),
            std::string::npos);
}

TEST(ControllerRobotDescription, GripperBindingWithoutModelJointAddsNoGripperUnlessRequired) {
  const std::string text = kSynthetic;
  const auto generated = generate_controller_robot_description(mock_request(
      text, {"j1"}, {0.0}, MockGripperBinding{std::nullopt, std::nullopt},
      false));
  expect_hardware(parse(generated).children.back(), "ManyForgeMockSystem",
                  "mock_components/GenericSystem",
                  {{"calculate_dynamics", "true"}}, {{"j1", 0.0}});
  const auto without_binding = generate_controller_robot_description(
      mock_request(text, {"j1"}, {0.0}));
  EXPECT_EQ(without_binding, generated);
}

void expect_refused(const ControllerRobotDescriptionRequest &request,
                    const std::string &diagnostic) {
  try {
    (void)generate_controller_robot_description(request);
    ADD_FAILURE() << "expected refusal containing: " << diagnostic;
  } catch (const ControllerRobotDescriptionError &error) {
    EXPECT_NE(std::string(error.what()).find(diagnostic), std::string::npos)
        << error.what();
  }
}

TEST(ControllerRobotDescription, FailsClosedOnInvalidInputs) {
  const std::string text = kSynthetic;
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  const auto inf = std::numeric_limits<double>::infinity();
  expect_refused(mock_request(text, {"j1", "j2"}, {0.0}),
                 "1 initial joint positions for 2 arm joints");
  expect_refused(mock_request(text, {"j1"}, {0.0, 0.0}),
                 "2 initial joint positions for 1 arm joints");
  expect_refused(mock_request(text, {"j1", "j2"}, {}),
                 "0 initial joint positions for 2 arm joints");
  expect_refused(mock_request(text, {"j1", "joint_7"}, {0.0, 0.0}),
                 "arm joint 'joint_7' is not a joint of the model");
  expect_refused(mock_request(text, {"j1", "j2"}, {0.0, nan}),
                 "initial value for joint 'j2' is not finite");
  expect_refused(mock_request(text, {"j1", "j2"}, {inf, 0.0}),
                 "initial value for joint 'j1' is not finite");
  expect_refused(mock_request(text, {"j1", "j1"}, {0.0, 0.0}),
                 "names an arm joint more than once");
  expect_refused(mock_request(text, {}, {}), "names no arm joint");
  expect_refused(mock_request(text, {"fixed_tool"}, {0.0}),
                 "arm joint 'fixed_tool' is a fixed joint");
  expect_refused(mock_request(text, {"finger_mimic"}, {0.0}),
                 "arm joint 'finger_mimic' is a mimic follower");
  expect_refused(mock_request(text, {"j1"}, {0.0},
                              MockGripperBinding{"finger_mimic", 0.0}),
                 "gripper joint 'finger_mimic' is a mimic follower");
  expect_refused(mock_request(text, {"j1"}, {0.0},
                              MockGripperBinding{"missing", 0.0}),
                 "gripper joint 'missing' is not a joint of the model");
  expect_refused(mock_request(text, {"j1"}, {0.0},
                              MockGripperBinding{"j1", 0.0}),
                 "gripper joint 'j1' is also an arm joint");
  expect_refused(mock_request(text, {"j1"}, {0.0},
                              MockGripperBinding{"finger", std::nullopt}),
                 "has no retained model projection");
  expect_refused(mock_request(text, {"j1"}, {0.0},
                              MockGripperBinding{"finger", nan}),
                 "initial value for joint 'finger' is not finite");
  expect_refused(mock_request(text, {"j1"}, {0.0},
                              MockGripperBinding{std::nullopt, std::nullopt},
                              true),
                 "gripper binding has no model-backed source joint");
  expect_refused(mock_request(text, {"j1"}, {0.0}, std::nullopt, true),
                 "the deployment has no gripper binding");
  expect_refused(
      ControllerRobotDescriptionRequest{
          text, {"j1"}, MockControllerLane{"", {0.0}, std::nullopt, false}},
      "no hardware component name");
  expect_refused(
      ControllerRobotDescriptionRequest{
          text, {"j1"}, IsaacControllerLane{"Isaac", "", "/state", {0.0}}},
      "no hardware component name or topics");
  expect_refused(
      ControllerRobotDescriptionRequest{
          text, {"j1", "j2"}, IsaacControllerLane{"Isaac", "/c", "/s", {0.0}}},
      "the Isaac profile has 1 initial joint positions for 2 arm joints");
  expect_refused(mock_request("<robot><joint name=\"j1\" type=\"revolute\">",
                              {"j1"}, {0.0}),
                 "not well-formed XML");
  expect_refused(mock_request("<model><joint name=\"j1\" type=\"revolute\"/></model>",
                              {"j1"}, {0.0}),
                 "root element is not one <robot>");
  expect_refused(mock_request("", {"j1"}, {0.0}), "model URDF is empty");
  expect_refused(
      mock_request("<!DOCTYPE robot [<!ENTITY x \"y\">]><robot><joint "
                   "name=\"j1\" type=\"revolute\"/></robot>",
                   {"j1"}, {0.0}),
      "DOCTYPE");
  expect_refused(
      mock_request("<robot><joint name=\"j1\" type=\"revolute\"/><joint "
                   "name=\"j1\" type=\"revolute\"/></robot>",
                   {"j1"}, {0.0}),
      "declares joint 'j1' more than once");
}

TEST(ControllerRobotDescription, GripperOnlyMockRefusesInventedOrInvalidState) {
  const std::string text = kSynthetic;
  expect_refused(mock_request(text, {}, {},
                             MockGripperBinding{std::nullopt, std::nullopt}),
                 "names no arm joint");
  expect_refused(mock_request(text, {}, {0.0},
                             MockGripperBinding{"finger", 0.02}),
                 "initial arm positions without an arm binding");
  expect_refused(mock_request(text, {}, {},
                             MockGripperBinding{"missing", 0.02}),
                 "is not a joint of the model");
  expect_refused(mock_request(text, {}, {},
                             MockGripperBinding{"fixed_tool", 0.02}),
                 "is a fixed joint");
  expect_refused(mock_request(text, {}, {},
                             MockGripperBinding{"finger_mimic", 0.02}),
                 "is a mimic follower");
  expect_refused(mock_request(text, {}, {},
                             MockGripperBinding{"finger", std::nullopt}),
                 "has no retained model projection");
  expect_refused(mock_request(
                     text, {}, {},
                     MockGripperBinding{
                         "finger", std::numeric_limits<double>::quiet_NaN()}),
                 "is not finite");
  expect_refused(ControllerRobotDescriptionRequest{
                     text, {},
                     IsaacControllerLane{"Isaac", "/cmd", "/state", {}}},
                 "names no arm joint");
}

TEST(ControllerRobotDescription, NoOutputRetainsJointValidationAndRefusesNoArmWriter) {
  const std::string text = kSynthetic;
  const auto no_output = [&](std::optional<MockGripperBinding> gripper,
                             std::vector<std::string> arm = {"j1"},
                             std::vector<double> initial = {0.0}) {
    auto request = mock_request(text, arm, std::move(initial), std::move(gripper), true);
    std::get<MockControllerLane>(request.lane).gripper_output_mode =
        manyforge::core::GripperOutputMode::kNone;
    return request;
  };
  expect_refused(no_output(std::nullopt), "model-backed gripper binding");
  expect_refused(no_output(MockGripperBinding{std::nullopt, std::nullopt}),
                 "model-backed gripper binding");
  expect_refused(no_output(MockGripperBinding{"finger", std::nullopt}, {}, {}),
                 "requires an arm");
  expect_refused(no_output(MockGripperBinding{"missing", std::nullopt}),
                 "is not a joint of the model");
  expect_refused(no_output(MockGripperBinding{"fixed_tool", std::nullopt}), "is a fixed joint");
  expect_refused(no_output(MockGripperBinding{"finger_mimic", std::nullopt}), "is a mimic follower");
  expect_refused(no_output(MockGripperBinding{"j1", std::nullopt}), "is also an arm joint");
  expect_refused(no_output(MockGripperBinding{
                     "finger", std::numeric_limits<double>::quiet_NaN()}), "is not finite");
  auto unknown = no_output(MockGripperBinding{"finger", std::nullopt});
  std::get<MockControllerLane>(unknown.lane).gripper_output_mode =
      static_cast<manyforge::core::GripperOutputMode>(99);
  expect_refused(unknown, "invalid gripper output mode");
}

TEST(ControllerRobotDescription, NoOutputIsaacValidatesModelWithoutAddingInterfaces) {
  const std::string text = kSynthetic;
  ControllerRobotDescriptionRequest request{
      text, {"j1", "j2"},
      IsaacControllerLane{"Isaac", "/cmd", "/state", {0.5, -0.25}}};
  const auto original = generate_controller_robot_description(request);
  request.gripper_without_control_interfaces = MockGripperBinding{"finger", std::nullopt};
  const auto generated = generate_controller_robot_description(request);
  EXPECT_EQ(generated, original);
  expect_model_unchanged(text, generated, "Isaac", 3U);
  expect_hardware(parse(generated).children.back(), "Isaac",
                  "topic_based_ros2_control/TopicBasedSystem",
                  {{"joint_commands_topic", "/cmd"}, {"joint_states_topic", "/state"},
                   {"trigger_joint_command_threshold", "-1"}, {"sum_wrapped_joint_states", "false"}},
                  {{"j1", 0.5}, {"j2", -0.25}});
  for (const auto* joint : {"missing", "fixed_tool", "finger_mimic", "j1"}) {
    request.gripper_without_control_interfaces = MockGripperBinding{joint, std::nullopt};
    EXPECT_THROW(static_cast<void>(generate_controller_robot_description(request)),
                 ControllerRobotDescriptionError) << joint;
  }
  request.gripper_without_control_interfaces = MockGripperBinding{std::nullopt, std::nullopt};
  expect_refused(request, "no model-backed source joint");
  request.gripper_without_control_interfaces = MockGripperBinding{
      "finger", std::numeric_limits<double>::infinity()};
  expect_refused(request, "is not finite");
}

TEST(ControllerRobotDescription, JointNamesComeOnlyFromTheBindingSourceJoints) {
  using manyforge::core::ResourceId;
  using manyforge::core::ResourceRoleId;
  std::vector<manyforge::core::RuntimeResourceBindingDTO> resources{
      {ResourceRoleId("joints"), ResourceId("arm_1"), "J1", "mf2_joint_a",
       manyforge::core::CoordinateKind::kAngular},
      {ResourceRoleId("joints"), ResourceId("arm_2"), "lbr_A2", "mf2_joint_b",
       manyforge::core::CoordinateKind::kAngular}};
  EXPECT_EQ(controller_joint_names_from_binding(resources),
            (std::vector<std::string>{"J1", "lbr_A2"}));
  resources[1].source_joint_name.reset();
  EXPECT_THROW((void)controller_joint_names_from_binding(resources),
               ControllerRobotDescriptionError);
}

// ---------------------------------------------------------------------------
// Isaac: equivalence with today's stored, launch-formatted descriptions.

[[nodiscard]] std::map<std::string, std::string> launch_default_values() {
  // isaac_topic_based_control.launch.py argument defaults (no caller
  // overrides them), formatted as the launch does (".17g").
  return {{"joint_commands_topic", "/isaac_joint_commands"},
          {"joint_states_topic", "/isaac_joint_states"},
          {"trigger_joint_command_threshold", g17(-1.0)},
          {"sum_wrapped_joint_states", "false"},
          {"shoulder_pan_initial", g17(2.63422544)},
          {"shoulder_lift_initial", g17(-0.875980752)},
          {"elbow_initial", g17(1.551074106)},
          {"wrist_1_initial", g17(-2.29231544)},
          {"wrist_2_initial", g17(-1.560498884)},
          {"wrist_3_initial", g17(-2.038544566)},
          {"joint1_initial", g17(0.0)},
          {"joint2_initial", g17(0.0)},
          {"joint3_initial", g17(1.57)},
          {"joint4_initial", g17(0.0)},
          {"joint5_initial", g17(0.0)},
          {"joint6_initial", g17(0.0)}};
}

// The retired stored Isaac files' hardware sections (Track 2 retirement of
// manyforge_ros_bringup/urdf/isaac_{lite6,ur10e}_topic_based.ros2_control.urdf,
// sha256 e1da8f69... and 944e5589...), as the Isaac launch formatted them: the
// generated section must stay equal in meaning.
constexpr const char *kRetiredIsaacLite6Section = R"(<robot name="retired">
  <ros2_control name="IsaacLite6TopicBasedSystem" type="system">
    <hardware>
      <plugin>topic_based_ros2_control/TopicBasedSystem</plugin>
      <param name="joint_commands_topic">{joint_commands_topic}</param>
      <param name="joint_states_topic">{joint_states_topic}</param>
      <param name="trigger_joint_command_threshold">{trigger_joint_command_threshold}</param>
      <param name="sum_wrapped_joint_states">{sum_wrapped_joint_states}</param>
    </hardware>
    <joint name="joint1">
      <command_interface name="position" />
      <state_interface name="position">
        <param name="initial_value">{joint1_initial}</param>
      </state_interface>
      <state_interface name="velocity" />
    </joint>
    <joint name="joint2">
      <command_interface name="position" />
      <state_interface name="position">
        <param name="initial_value">{joint2_initial}</param>
      </state_interface>
      <state_interface name="velocity" />
    </joint>
    <joint name="joint3">
      <command_interface name="position" />
      <state_interface name="position">
        <param name="initial_value">{joint3_initial}</param>
      </state_interface>
      <state_interface name="velocity" />
    </joint>
    <joint name="joint4">
      <command_interface name="position" />
      <state_interface name="position">
        <param name="initial_value">{joint4_initial}</param>
      </state_interface>
      <state_interface name="velocity" />
    </joint>
    <joint name="joint5">
      <command_interface name="position" />
      <state_interface name="position">
        <param name="initial_value">{joint5_initial}</param>
      </state_interface>
      <state_interface name="velocity" />
    </joint>
    <joint name="joint6">
      <command_interface name="position" />
      <state_interface name="position">
        <param name="initial_value">{joint6_initial}</param>
      </state_interface>
      <state_interface name="velocity" />
    </joint>
  </ros2_control>
</robot>)";

constexpr const char *kRetiredIsaacUr10eSection = R"(<robot name="retired">
  <ros2_control name="IsaacUR10eTopicBasedSystem" type="system">
    <hardware>
      <plugin>topic_based_ros2_control/TopicBasedSystem</plugin>
      <param name="joint_commands_topic">{joint_commands_topic}</param>
      <param name="joint_states_topic">{joint_states_topic}</param>
      <param name="trigger_joint_command_threshold">{trigger_joint_command_threshold}</param>
      <param name="sum_wrapped_joint_states">{sum_wrapped_joint_states}</param>
    </hardware>
    <joint name="shoulder_pan_joint">
      <command_interface name="position"/>
      <state_interface name="position"><param name="initial_value">{shoulder_pan_initial}</param></state_interface>
      <state_interface name="velocity"/>
    </joint>
    <joint name="shoulder_lift_joint">
      <command_interface name="position"/>
      <state_interface name="position"><param name="initial_value">{shoulder_lift_initial}</param></state_interface>
      <state_interface name="velocity"/>
    </joint>
    <joint name="elbow_joint">
      <command_interface name="position"/>
      <state_interface name="position"><param name="initial_value">{elbow_initial}</param></state_interface>
      <state_interface name="velocity"/>
    </joint>
    <joint name="wrist_1_joint">
      <command_interface name="position"/>
      <state_interface name="position"><param name="initial_value">{wrist_1_initial}</param></state_interface>
      <state_interface name="velocity"/>
    </joint>
    <joint name="wrist_2_joint">
      <command_interface name="position"/>
      <state_interface name="position"><param name="initial_value">{wrist_2_initial}</param></state_interface>
      <state_interface name="velocity"/>
    </joint>
    <joint name="wrist_3_joint">
      <command_interface name="position"/>
      <state_interface name="position"><param name="initial_value">{wrist_3_initial}</param></state_interface>
      <state_interface name="velocity"/>
    </joint>
  </ros2_control>
</robot>)";

TEST(ControllerRobotDescription, IsaacLite6FormEqualsTheRetiredLaunchDescription) {
  const auto model_text = read_repository_file(
      "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf");
  const auto retired =
      parse(format_template(kRetiredIsaacLite6Section, launch_default_values()));
  const auto generated = parse(generate_controller_robot_description(
      ControllerRobotDescriptionRequest{
          model_text,
          {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6"},
          IsaacControllerLane{"IsaacLite6TopicBasedSystem",
                              "/isaac_joint_commands", "/isaac_joint_states",
                              {0.0, 0.0, 1.57, 0.0, 0.0, 0.0}}}));
  ASSERT_EQ(retired.all("ros2_control").size(), 1U);
  ASSERT_EQ(generated.all("ros2_control").size(), 1U);
  EXPECT_EQ(canonical(*generated.all("ros2_control").front()),
            canonical(*retired.all("ros2_control").front()));
  // The rest of the description is the model itself.
  EXPECT_EQ(model_children(generated), model_children(parse(model_text)));
  EXPECT_EQ(generated.attribute("name"), "lite6_lite_gripper");
}

TEST(ControllerRobotDescription, IsaacUr10eFormDiffersFromTheRetiredFileOnlyInModelParts) {
  const auto model_text = read_repository_file(
      "manyforge_example_models/models/ur10e_robotiq_2f_140/"
      "ur10e_robotiq_2f_140.urdf");
  const std::vector<std::string> arm{"shoulder_pan_joint", "shoulder_lift_joint",
                                     "elbow_joint",        "wrist_1_joint",
                                     "wrist_2_joint",      "wrist_3_joint"};
  const auto retired =
      parse(format_template(kRetiredIsaacUr10eSection, launch_default_values()));
  const auto generated = parse(generate_controller_robot_description(
      ControllerRobotDescriptionRequest{
          model_text, arm,
          IsaacControllerLane{"IsaacUR10eTopicBasedSystem",
                              "/isaac_joint_commands", "/isaac_joint_states",
                              {2.63422544, -0.875980752, 1.551074106,
                               -2.29231544, -1.560498884, -2.038544566}}}));
  // The hardware section is identical.
  ASSERT_EQ(generated.all("ros2_control").size(), 1U);
  EXPECT_EQ(canonical(*generated.all("ros2_control").front()),
            canonical(*retired.all("ros2_control").front()));
  // Everything else comes from the model (accepted Q69 change): the official
  // UR10e limits replace the retired hand-written 3.15/3.2 rad/s placeholders.
  EXPECT_EQ(model_children(generated), model_children(parse(model_text)));
  const std::map<std::string, std::string> velocity{
      {"shoulder_pan_joint", "2.0943951023931953"},
      {"shoulder_lift_joint", "2.0943951023931953"},
      {"elbow_joint", "3.141592653589793"},
      {"wrist_1_joint", "3.141592653589793"},
      {"wrist_2_joint", "3.141592653589793"},
      {"wrist_3_joint", "3.141592653589793"}};
  for (const auto &name : arm) {
    SCOPED_TRACE(name);
    const Node *joint = nullptr;
    for (const auto *candidate : generated.all("joint")) {
      if (candidate->attribute("name") == name) joint = candidate;
    }
    ASSERT_NE(joint, nullptr);
    EXPECT_EQ(joint->all("limit").front()->attribute("velocity"),
              velocity.at(name));
  }
}

} // namespace
