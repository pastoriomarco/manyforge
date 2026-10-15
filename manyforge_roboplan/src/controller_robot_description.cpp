#include <manyforge_roboplan/controller_robot_description.hpp>

#include <expat.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw ControllerRobotDescriptionError(
      "controller robot_description: " + message);
}

struct ModelJoint final {
  std::string type;
  bool mimic_follower{};
};

struct ByteSpan final {
  std::size_t begin{};
  std::size_t end{};
};

/// One pass over the model: the exact byte spans of every outermost
/// `<ros2_control>` element, the root end tag, and the root's joints. Nothing
/// is interpreted beyond what the generated section must be checked against.
struct ModelScan final {
  XML_Parser parser{};
  std::size_t depth{};
  std::optional<std::string> error;
  bool root_seen{};
  std::optional<std::size_t> root_end_tag;
  std::size_t ros2_control_depth{};
  std::size_t ros2_control_begin{};
  std::vector<ByteSpan> ros2_control_spans;
  std::map<std::string, ModelJoint> joints;
  ModelJoint *current_joint{};
  std::size_t current_joint_depth{};
};

void stop(ModelScan &scan, std::string message) {
  if (!scan.error.has_value()) scan.error = std::move(message);
  XML_StopParser(scan.parser, XML_FALSE);
}

[[nodiscard]] const char *attribute(const XML_Char **attributes,
                                    const std::string_view name) {
  for (std::size_t index = 0; attributes[index] != nullptr; index += 2U) {
    if (name == attributes[index]) return attributes[index + 1U];
  }
  return nullptr;
}

[[nodiscard]] std::size_t current_index(const ModelScan &scan) {
  return static_cast<std::size_t>(XML_GetCurrentByteIndex(scan.parser));
}

void XMLCALL start_element(void *user_data, const XML_Char *name,
                           const XML_Char **attributes) {
  auto &scan = *static_cast<ModelScan *>(user_data);
  if (scan.error.has_value()) return;
  const std::string_view element(name);
  ++scan.depth;
  if (scan.depth == 1U) {
    if (scan.root_seen || element != "robot") {
      stop(scan, "the model root element is not one <robot>");
      return;
    }
    scan.root_seen = true;
    return;
  }
  if (scan.ros2_control_depth == 0U && element == "ros2_control") {
    scan.ros2_control_depth = scan.depth;
    scan.ros2_control_begin = current_index(scan);
    return;
  }
  if (scan.ros2_control_depth != 0U) return;
  if (scan.depth == 2U && element == "joint") {
    const char *joint_name = attribute(attributes, "name");
    const char *joint_type = attribute(attributes, "type");
    if (joint_name == nullptr || *joint_name == '\0' || joint_type == nullptr) {
      stop(scan, "a model joint has no name or type");
      return;
    }
    const auto [inserted, fresh] =
        scan.joints.emplace(joint_name, ModelJoint{joint_type, false});
    if (!fresh) {
      stop(scan, "the model declares joint '" + std::string(joint_name) +
                     "' more than once");
      return;
    }
    scan.current_joint = &inserted->second;
    scan.current_joint_depth = scan.depth;
    return;
  }
  if (scan.current_joint != nullptr &&
      scan.depth == scan.current_joint_depth + 1U && element == "mimic") {
    scan.current_joint->mimic_follower = true;
  }
}

void XMLCALL end_element(void *user_data, const XML_Char *) {
  auto &scan = *static_cast<ModelScan *>(user_data);
  if (scan.error.has_value()) return;
  const auto index = current_index(scan);
  const auto count = static_cast<std::size_t>(
      std::max(0, XML_GetCurrentByteCount(scan.parser)));
  if (scan.ros2_control_depth != 0U && scan.depth == scan.ros2_control_depth) {
    // An empty element's end handler reports no bytes of its own; its start
    // tag already ended the element.
    const auto end = count == 0U ? index : index + count;
    scan.ros2_control_spans.push_back(ByteSpan{scan.ros2_control_begin, end});
    scan.ros2_control_depth = 0U;
  } else if (scan.current_joint != nullptr &&
             scan.depth == scan.current_joint_depth) {
    scan.current_joint = nullptr;
  } else if (scan.depth == 1U) {
    if (count == 0U) {
      stop(scan, "the model <robot> element is empty");
      return;
    }
    scan.root_end_tag = index;
  }
  --scan.depth;
}

void XMLCALL start_doctype(void *user_data, const XML_Char *, const XML_Char *,
                           const XML_Char *, int) {
  stop(*static_cast<ModelScan *>(user_data),
       "the model declares a DOCTYPE, which a URDF does not use");
}

[[nodiscard]] ModelScan scan_model(const std::string_view urdf) {
  if (urdf.empty()) fail("the model URDF is empty");
  if (urdf.size() > static_cast<std::size_t>(INT_MAX)) {
    fail("the model URDF exceeds the parser bound");
  }
  // The input is URDF UTF-8 text; fixing the encoding keeps every reported
  // byte index an index into exactly this input.
  const std::unique_ptr<std::remove_pointer_t<XML_Parser>,
                        decltype(&XML_ParserFree)>
      parser(XML_ParserCreate("UTF-8"), &XML_ParserFree);
  if (!parser) fail("the XML parser could not be created");
  ModelScan scan;
  scan.parser = parser.get();
  XML_SetUserData(parser.get(), &scan);
  XML_SetElementHandler(parser.get(), &start_element, &end_element);
  XML_SetStartDoctypeDeclHandler(parser.get(), &start_doctype);
  const auto status = XML_Parse(parser.get(), urdf.data(),
                                static_cast<int>(urdf.size()), XML_TRUE);
  if (scan.error.has_value()) fail(*scan.error);
  if (status != XML_STATUS_OK) {
    fail(std::string("the model URDF is not well-formed XML (line ") +
         std::to_string(XML_GetCurrentLineNumber(parser.get())) + ": " +
         XML_ErrorString(XML_GetErrorCode(parser.get())) + ")");
  }
  if (!scan.root_end_tag.has_value()) fail("the model has no <robot> end tag");
  scan.parser = nullptr;
  return scan;
}

[[nodiscard]] std::string escaped(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    switch (character) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    case '\'': out += "&apos;"; break;
    default: out.push_back(character);
    }
  }
  return out;
}

/// Round-trip decimal text, as the Isaac launch formats its values (".17g").
[[nodiscard]] std::string number_text(const double value) {
  char buffer[64];
  const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
    fail("a numeric value could not be formatted");
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

void require_commandable_joint(const ModelScan &model, const std::string &name,
                               const std::string_view role) {
  if (name.empty()) fail(std::string(role) + " has an empty joint name");
  const auto found = model.joints.find(name);
  if (found == model.joints.end()) {
    fail(std::string(role) + " joint '" + name +
         "' is not a joint of the model");
  }
  if (found->second.type == "fixed") {
    fail(std::string(role) + " joint '" + name + "' is a fixed joint");
  }
  if (found->second.mimic_follower) {
    fail(std::string(role) + " joint '" + name +
         "' is a mimic follower; mimic followers are not listed");
  }
}

[[nodiscard]] double finite_value(const double value, const std::string &joint,
                                  const std::string_view role) {
  if (!std::isfinite(value)) {
    fail(std::string(role) + " initial value for joint '" + joint +
         "' is not finite");
  }
  return value;
}

void validate_gripper_model_binding(
    const ModelScan& model, const std::vector<std::string>& arm_joint_names,
    const MockGripperBinding& gripper) {
  if (!gripper.source_joint_name.has_value()) {
    fail("the no-output gripper binding has no model-backed source joint");
  }
  const auto& joint = *gripper.source_joint_name;
  require_commandable_joint(model, joint, "gripper");
  if (std::find(arm_joint_names.begin(), arm_joint_names.end(), joint) != arm_joint_names.end()) {
    fail("gripper joint '" + joint + "' is also an arm joint");
  }
  if (gripper.projected_initial_position.has_value()) {
    static_cast<void>(finite_value(*gripper.projected_initial_position, joint,
                                  "the retained model projection"));
  }
}

struct GeneratedJoint final {
  std::string name;
  double initial_value{};
};

void append_joint(std::string &out, const GeneratedJoint &joint) {
  out += "    <joint name=\"" + escaped(joint.name) + "\">\n";
  out += "      <command_interface name=\"position\"/>\n";
  out += "      <state_interface name=\"position\">\n";
  out += "        <param name=\"initial_value\">" +
         number_text(joint.initial_value) + "</param>\n";
  out += "      </state_interface>\n";
  out += "      <state_interface name=\"velocity\"/>\n";
  out += "    </joint>\n";
}

[[nodiscard]] std::string
hardware_section(const std::string &component_name,
                 const std::string_view plugin,
                 const std::vector<std::pair<std::string, std::string>> &params,
                 const std::vector<GeneratedJoint> &joints) {
  std::string out;
  out += "  <ros2_control name=\"" + escaped(component_name) +
         "\" type=\"system\">\n";
  out += "    <hardware>\n";
  out += "      <plugin>" + escaped(plugin) + "</plugin>\n";
  for (const auto &[name, value] : params) {
    out += "      <param name=\"" + escaped(name) + "\">" + escaped(value) +
           "</param>\n";
  }
  out += "    </hardware>\n";
  for (const auto &joint : joints) append_joint(out, joint);
  out += "  </ros2_control>\n";
  return out;
}

[[nodiscard]] std::vector<GeneratedJoint>
paired_arm_joints(const ModelScan &model,
                  const std::vector<std::string> &arm_joint_names,
                  const std::vector<double> &initial_values,
                  const std::string_view lane) {
  if (arm_joint_names.empty()) fail("the retained binding names no arm joint");
  if (std::set<std::string>(arm_joint_names.begin(), arm_joint_names.end())
          .size() != arm_joint_names.size()) {
    fail("the retained binding names an arm joint more than once");
  }
  if (initial_values.size() != arm_joint_names.size()) {
    fail(std::string(lane) + " has " + std::to_string(initial_values.size()) +
         " initial joint positions for " +
         std::to_string(arm_joint_names.size()) + " arm joints");
  }
  std::vector<GeneratedJoint> joints;
  joints.reserve(arm_joint_names.size() + 1U);
  for (std::size_t index = 0; index < arm_joint_names.size(); ++index) {
    require_commandable_joint(model, arm_joint_names[index], "arm");
    joints.push_back(GeneratedJoint{
        arm_joint_names[index],
        finite_value(initial_values[index], arm_joint_names[index], lane)});
  }
  return joints;
}

[[nodiscard]] std::string
mock_section(const ModelScan &model,
             const std::vector<std::string> &arm_joint_names,
             const MockControllerLane &lane) {
  if (lane.hardware_component_name.empty()) {
    fail("the mock lane has no hardware component name");
  }
  const bool model_backed_gripper =
      lane.gripper.has_value() && lane.gripper->source_joint_name.has_value();
  if (lane.gripper_output_mode != core::GripperOutputMode::kCommanded &&
      lane.gripper_output_mode != core::GripperOutputMode::kNone) {
    fail("the mock lane has an invalid gripper output mode");
  }
  const bool no_gripper_output =
      lane.gripper_output_mode == core::GripperOutputMode::kNone;
  if (no_gripper_output && (arm_joint_names.empty() || !model_backed_gripper)) {
    fail("no-output controller generation requires an arm and a model-backed gripper binding");
  }
  std::vector<GeneratedJoint> joints;
  if (arm_joint_names.empty() && model_backed_gripper) {
    if (!lane.initial_joint_positions_rad.empty()) {
      fail("the deployment has initial arm positions without an arm binding");
    }
  } else {
    joints = paired_arm_joints(model, arm_joint_names,
                              lane.initial_joint_positions_rad,
                              "the deployment");
  }
  if (model_backed_gripper) {
    const auto &gripper_joint = *lane.gripper->source_joint_name;
    validate_gripper_model_binding(model, arm_joint_names, *lane.gripper);
    if (!lane.gripper->projected_initial_position.has_value() &&
        !no_gripper_output) {
      fail("gripper joint '" + gripper_joint +
           "' has no retained model projection for its initial value");
    }
    if (!no_gripper_output) {
      joints.push_back(GeneratedJoint{
          gripper_joint, *lane.gripper->projected_initial_position});
    }
  } else if (lane.gripper_required) {
    fail(lane.gripper.has_value()
             ? "the mock profile requires a gripper, but the gripper binding "
               "has no model-backed source joint"
             : "the mock profile requires a gripper, but the deployment has "
               "no gripper binding");
  }
  return hardware_section(lane.hardware_component_name,
                          "mock_components/GenericSystem",
                          {{"calculate_dynamics", "true"}}, joints);
}

[[nodiscard]] std::string
isaac_section(const ModelScan &model,
              const std::vector<std::string> &arm_joint_names,
              const IsaacControllerLane &lane) {
  if (lane.hardware_component_name.empty() ||
      lane.joint_commands_topic.empty() || lane.joint_states_topic.empty()) {
    fail("the Isaac lane has no hardware component name or topics");
  }
  const auto joints = paired_arm_joints(model, arm_joint_names,
                                        lane.initial_joint_positions_rad,
                                        "the Isaac profile");
  return hardware_section(lane.hardware_component_name,
                          "topic_based_ros2_control/TopicBasedSystem",
                          {{"joint_commands_topic", lane.joint_commands_topic},
                           {"joint_states_topic", lane.joint_states_topic},
                           {"trigger_joint_command_threshold", number_text(-1.0)},
                           {"sum_wrapped_joint_states", "false"}},
                          joints);
}

[[nodiscard]] bool horizontal_space(const char character) noexcept {
  return character == ' ' || character == '\t';
}

/// Widen a removed element to its whole line when it stands alone on it, so
/// the removal leaves no blank line. Only whitespace is ever widened over.
[[nodiscard]] ByteSpan whole_line(const std::string_view text, ByteSpan span) {
  auto begin = span.begin;
  while (begin > 0U && horizontal_space(text[begin - 1U])) --begin;
  if (begin != 0U && text[begin - 1U] != '\n') return span;
  auto end = span.end;
  while (end < text.size() && horizontal_space(text[end])) ++end;
  if (end < text.size() && text[end] == '\r') ++end;
  if (end >= text.size() || text[end] != '\n') return span;
  return ByteSpan{begin, end + 1U};
}

} // namespace

std::string generate_controller_robot_description(
    const ControllerRobotDescriptionRequest &request) {
  const auto model = scan_model(request.model_urdf);
  if (request.gripper_without_control_interfaces.has_value()) {
    validate_gripper_model_binding(model, request.arm_joint_names,
                                   *request.gripper_without_control_interfaces);
  }
  const auto section = std::visit(
      [&](const auto &lane) {
        using Lane = std::decay_t<decltype(lane)>;
        if constexpr (std::is_same_v<Lane, MockControllerLane>) {
          return mock_section(model, request.arm_joint_names, lane);
        } else {
          return isaac_section(model, request.arm_joint_names, lane);
        }
      },
      request.lane);

  const std::string_view text = request.model_urdf;
  const auto root_end = *model.root_end_tag;
  std::string out;
  out.reserve(text.size() + section.size());
  std::size_t cursor = 0U;
  for (const auto &found : model.ros2_control_spans) {
    const auto span = whole_line(text, found);
    out.append(text.substr(cursor, span.begin - cursor));
    cursor = span.end;
  }
  out.append(text.substr(cursor, root_end - cursor));
  if (!out.empty() && out.back() != '\n') out.push_back('\n');
  out.append(section);
  out.append(text.substr(root_end));
  return out;
}

std::vector<std::string> controller_joint_names_from_binding(
    const std::span<const core::RuntimeResourceBindingDTO> resources) {
  std::vector<std::string> names;
  names.reserve(resources.size());
  for (const auto &resource : resources) {
    if (!resource.source_joint_name.has_value() ||
        resource.source_joint_name->empty()) {
      fail("binding resource '" + resource.resource_id.value() +
           "' has no model source joint");
    }
    names.push_back(*resource.source_joint_name);
  }
  return names;
}

} // namespace manyforge::roboplan_backend
