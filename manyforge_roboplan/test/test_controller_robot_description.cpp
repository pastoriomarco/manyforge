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
             std::optional<MockGripperBinding> gripper = std::nullopt) {
  return ControllerRobotDescriptionRequest{
      urdf, arm,
      Ros2ControlLane{"ManyForgeMockSystem", "mock_components/GenericSystem",
                      {{"calculate_dynamics", "true"}}, {"position"}, std::move(initial),
                      std::move(gripper)}};
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
        mock_request(text, model.arm_joints, initial, gripper));
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


TEST(ControllerRobotDescription, GripperOnlyMockPreservesEveryModelElement) {
  for (const auto &model : offline_models()) {
    if (!model.gripper_joint.has_value()) continue;
    SCOPED_TRACE(model.path);
    const auto text = read_repository_file(model.path);
    const auto generated = generate_controller_robot_description(mock_request(
        text, {}, {}, MockGripperBinding{model.gripper_joint, 0.0125}));
    expect_model_unchanged(text, generated, "ManyForgeMockSystem",
                           model.source_ros2_control_count);
    expect_hardware(parse(generated).children.back(), "ManyForgeMockSystem",
                    "mock_components/GenericSystem",
                    {{"calculate_dynamics", "true"}},
                    {{*model.gripper_joint, 0.0125}});
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
      text, {"j1", "j2"}, {0.5, -0.25}, MockGripperBinding{"finger", 0.02}));
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
  // No seeds is valid: hardware that reports measured state (Real) has none.
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
                              MockGripperBinding{"finger", nan}),
                 "initial value for joint 'finger' is not finite");
  expect_refused(mock_request(text, {"j1"}, {0.0},
                              MockGripperBinding{std::nullopt, std::nullopt}),
                 "gripper binding has no model-backed source joint");
  expect_refused(
      ControllerRobotDescriptionRequest{
          text, {"j1"},
          Ros2ControlLane{"", "mock_components/GenericSystem", {}, {"position"}, {0.0}, {}}},
      "no name, plugin or joint command interface");
  // Hardware that reports measured state takes no initial values.
  const auto measured = generate_controller_robot_description(ControllerRobotDescriptionRequest{
      text, {"j1"},
      Ros2ControlLane{"Real", "vendor/Plugin", {{"robot_ip", "R10.0.0.1"}},
                      {"position", "velocity"}, {}, {}}});
  EXPECT_EQ(measured.find("initial_value"), std::string::npos);
  EXPECT_NE(measured.find("<command_interface name=\"velocity\"/>"), std::string::npos);
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
  expect_refused(mock_request(
                     text, {}, {},
                     MockGripperBinding{
                         "finger", std::numeric_limits<double>::quiet_NaN()}),
                 "is not finite");
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

} // namespace
