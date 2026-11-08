#include <manyforge_roboplan/controller_launch_artifacts.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw ControllerLaunchArtifactError("controller launch artifacts: " +
                                      message);
}

[[nodiscard]] bool is_ros_name_token(const std::string_view token) noexcept {
  if (token.empty() || token.size() > 255U) return false;
  const auto alpha = [](const char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
  };
  const auto digit = [](const char value) { return value >= '0' && value <= '9'; };
  if (!alpha(token.front()) && token.front() != '_') return false;
  return std::all_of(token.begin() + 1, token.end(), [&](const char value) {
    return alpha(value) || digit(value) || value == '_';
  });
}

void require_token(const std::string_view value, const std::string_view role) {
  if (!is_ros_name_token(value)) {
    fail(std::string(role) + " '" + std::string(value) +
         "' is not one bounded ROS name token");
  }
}

/// The shared launch's `hardware_remappings` value: `from=to` pairs joined
/// by `;` (neither character occurs in a ROS name).
[[nodiscard]] std::string
remapping_argument(const std::vector<std::pair<std::string, std::string>> &remappings) {
  std::string out;
  for (const auto &[from, to] : remappings) {
    for (const auto &name : {from, to}) {
      if (name.empty() || name.front() != '/' ||
          name.find_first_of("=;") != std::string::npos) {
        fail("remapping name '" + name + "' is not one absolute ROS name");
      }
    }
    if (!out.empty()) out += ';';
    out += from + "=" + to;
  }
  return out;
}

/// A YAML double that ROS reads as a double parameter (never as an integer).
[[nodiscard]] std::string yaml_double(const double value,
                                      const std::string_view what) {
  if (!std::isfinite(value) || !(value > 0.0)) {
    fail(std::string(what) + " must be a finite positive number");
  }
  char buffer[64];
  const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
    fail("a numeric value could not be formatted");
  }
  std::string text(buffer, static_cast<std::size_t>(written));
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

/// A positive tolerance in its shortest round-trip form (0.05, not
/// 0.050000000000000003), always read as a double.
[[nodiscard]] std::string yaml_tolerance(const double value,
                                         const std::string_view what) {
  if (!std::isfinite(value) || !(value > 0.0)) {
    fail(std::string(what) + " must be a finite positive number");
  }
  char buffer[64];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) fail("a numeric value could not be formatted");
  std::string text(buffer, end);
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

[[nodiscard]] std::vector<std::string> lines_of(const std::string_view text) {
  std::vector<std::string> lines;
  std::size_t cursor = 0U;
  while (cursor < text.size()) {
    const auto end = text.find('\n', cursor);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(cursor));
      break;
    }
    lines.emplace_back(text.substr(cursor, end - cursor));
    cursor = end + 1U;
  }
  return lines;
}

[[nodiscard]] bool is_comment(const std::string_view line) {
  const auto first = line.find_first_not_of(' ');
  return first != std::string_view::npos && line[first] == '#';
}

[[nodiscard]] std::string indentation(const std::string_view line) {
  return std::string(line.substr(0U, line.find_first_not_of(' ')));
}

[[nodiscard]] std::string trimmed(const std::string_view line) {
  const auto first = line.find_first_not_of(' ');
  if (first == std::string_view::npos) return {};
  return std::string(line.substr(first));
}

[[nodiscard]] std::string join_lines(const std::vector<std::string> &lines) {
  std::string out;
  for (const auto &line : lines) {
    out += line;
    out += '\n';
  }
  return out;
}

void write_exact(const std::filesystem::path &path, const std::string &bytes) {
  const auto temporary = path.string() + ".partial";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) fail("cannot create " + temporary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) fail("cannot write " + temporary);
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) fail("cannot publish " + path.string() + ": " + error.message());
}

[[nodiscard]] std::string json_string(const std::string_view value) {
  std::string out = "\"";
  for (const char character : value) {
    switch (character) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '\r': out += "\\r"; break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                      static_cast<unsigned>(static_cast<unsigned char>(character)));
        out += buffer;
      } else {
        out.push_back(character);
      }
    }
  }
  out += '"';
  return out;
}

/// Replace every occurrence of `token`; returns how many there were.
std::size_t replace_all(std::string &text, const std::string_view token,
                        const std::string_view value) {
  std::size_t count = 0U;
  for (auto cursor = text.find(token); cursor != std::string::npos;
       cursor = text.find(token, cursor + value.size())) {
    text.replace(cursor, token.size(), value);
    ++count;
  }
  return count;
}

/// The instance values of a launch contribution in one contribution string.
[[nodiscard]] std::string with_instance(std::string text,
                                        const std::string &runtime_namespace,
                                        const std::string_view address) {
  replace_all(text, kLaunchNamespaceToken, runtime_namespace);
  replace_all(text, kLaunchHardwareComponentToken, kHardwareComponentName);
  if (text.find(kLaunchAddressToken) != std::string::npos) {
    const auto host_character = [](const char value) {
      return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
             (value >= '0' && value <= '9') || value == '.' || value == '-' ||
             value == ':';
    };
    if (address.empty() || address.size() > 253U ||
        !std::all_of(address.begin(), address.end(), host_character)) {
      fail("the component needs its device address from the site settings, got '" +
           std::string(address) + "'");
    }
    replace_all(text, kLaunchAddressToken, address);
  }
  return text;
}

constexpr std::string_view kTemplateManagerSelector = "/**/controller_manager:";

/// OD-59: under the strict real-time resource profile (its one source of
/// truth, `MANYFORGE_RT_RESOURCE_PROFILE`, also read by the scheduler) every
/// lane's controller manager locks its memory (stock `lock_memory`, which
/// upstream only warns about when it fails; the resource probe checks VmLck).
[[nodiscard]] bool strict_rt_resource_profile() {
  const char *profile = std::getenv("MANYFORGE_RT_RESOURCE_PROFILE");
  if (profile == nullptr) return false;
  std::string value(profile);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value == "strict";
}

/// The controllers template with the row's and the
/// instance's values (see `LaunchContribution`).
[[nodiscard]] std::string
expand_controllers(const ModelLaneRow &row, const std::string &controllers,
                   const LaunchInstance &instance,
                   const std::string &runtime_namespace, const bool lock_memory) {
  // The joints an arm controller moves: none with the arm deactivated.
  const auto &arm = instance.arm_deactivated ? std::vector<std::string>{}
                                             : instance.arm_joint_names;
  std::string joint_list;
  for (const auto &joint : arm) {
    if (!joint_list.empty()) joint_list += ", ";
    joint_list += joint;
  }
  std::vector<std::string> expanded;
  std::size_t managers = 0U;
  std::size_t joint_lists = 0U;
  std::size_t constraint_lines = 0U;
  std::size_t goal_times = 0U;
  bool in_manager = false;
  for (const auto &line : lines_of(controllers)) {
    if (is_comment(line)) continue;
    const auto content = trimmed(line);
    if (line == kTemplateManagerSelector) {
      expanded.push_back(runtime_namespace + "/" +
                         std::string(kControllerManagerNodeName) + ":");
      ++managers;
      in_manager = true;
      continue;
    }
    if (!line.empty() && line.front() != ' ') in_manager = false;
    if (lock_memory && in_manager) {
      // The lane's own `lock_memory` is replaced by the strict profile's.
      if (content.starts_with("lock_memory:")) continue;
      if (content == "ros__parameters:") {
        expanded.push_back(line);
        expanded.push_back(indentation(line) + "  lock_memory: true");
        continue;
      }
    }
    if (content == "joints: [__joint__]") {
      if (arm.empty()) fail("the controllers list arm joints for a component without an arm");
      expanded.push_back(indentation(line) + "joints: [" + joint_list + "]");
      ++joint_lists;
      continue;
    }
    if (content.starts_with("__joint__: {")) {
      constexpr std::string_view kOpen = "__joint__: {";
      constexpr std::string_view kNullBound = "max_deceleration_on_cancel: null}";
      if (arm.empty() || !content.ends_with(kNullBound)) {
        fail("the controllers' joint constraint has no arm or no cancel bound placeholder");
      }
      const auto head = content.substr(kOpen.size(),
                                       content.size() - kOpen.size() - kNullBound.size());
      if (head.find("goal:") != std::string::npos) {
        fail("the controllers carry their own goal constraint; the row's arrival "
             "tolerance is the goal");
      }
      if (!row.arrival_tolerance_rad.has_value()) {
        fail("the lane row of a robot needs arrival_tolerance_rad");
      }
      for (const auto &joint : arm) {
        const auto found = instance.max_deceleration_on_cancel_rad_s2.find(joint);
        if (found == instance.max_deceleration_on_cancel_rad_s2.end()) {
          fail("the deployment's operator Stop profile has no maximum "
               "deceleration for arm joint '" + joint + "'");
        }
        expanded.push_back(
            indentation(line) + joint + ": {" + head + "goal: " +
            yaml_tolerance(*row.arrival_tolerance_rad, "arrival_tolerance_rad") +
            ", max_deceleration_on_cancel: " +
            yaml_double(found->second, "max_deceleration_on_cancel of " + joint) + "}");
      }
      ++constraint_lines;
      continue;
    }
    if (row.goal_time_s.has_value() && content.starts_with("goal_time:")) {
      expanded.push_back(indentation(line) + "goal_time: " +
                         yaml_tolerance(*row.goal_time_s, "goal_time_s"));
      ++goal_times;
      continue;
    }
    expanded.push_back(line);
  }
  if (managers != 1U) fail("the controllers do not carry exactly one " +
                           std::string(kTemplateManagerSelector) + " block");
  if (!arm.empty() && (joint_lists == 0U || constraint_lines != 1U)) {
    fail("the controllers of an arm need its joint list and exactly one joint "
         "constraint placeholder");
  }
  if (!instance.arm_deactivated && row.goal_time_s.has_value() && goal_times != 1U) {
    fail("the controllers do not carry exactly one goal_time for the row's goal_time_s");
  }
  for (const auto &[joint, limit] : instance.max_deceleration_on_cancel_rad_s2) {
    static_cast<void>(limit);
    if (std::find(arm.begin(), arm.end(), joint) == arm.end()) {
      fail("the operator Stop profile names joint '" + joint +
           "', which is not an arm joint of the binding");
    }
  }
  std::string out = join_lines(expanded);
  if (instance.arm_deactivated) return out;  // no controller to name
  require_token(row.controller, "lane row controller");
  if (replace_all(out, "__controller__", row.controller) == 0U) {
    fail("the controllers do not name the row's controller (__controller__)");
  }
  if (out.find("__gripper_joint__") != std::string::npos) {
    if (!instance.gripper.has_value() || !instance.gripper->source_joint_name.has_value()) {
      fail("the controllers name a gripper joint the component does not have");
    }
    require_token(*instance.gripper->source_joint_name, "gripper joint");
    replace_all(out, "__gripper_joint__", *instance.gripper->source_joint_name);
  }
  return out;
}

/// `__reset_writer__`: the stock writer of the Deployment's initial pose.
[[nodiscard]] MockResetPositionTarget
reset_writer(const LaunchInstance &instance, const std::string &plugin,
             const std::string &runtime_namespace,
             const std::optional<std::string> &command_echo_topic) {
  if (instance.arm_joint_names.empty() ||
      instance.initial_joint_positions_rad.size() != instance.arm_joint_names.size()) {
    fail("a reset-position writer needs the arm's initial joint positions");
  }
  if (!content_identity::is_sha256_digest(instance.component_runtime_identity)) {
    fail("a reset-position writer requires the exact component runtime identity");
  }
  using Value = content_identity::CanonicalValue;
  Value::Array joints;
  Value::Array positions;
  for (const auto &joint : instance.arm_joint_names) joints.emplace_back(joint);
  for (const auto position : instance.initial_joint_positions_rad)
    positions.emplace_back(position == 0.0 ? 0.0 : position);
  const auto digest = content_identity::framed_sha256(
      "manyforge.mock-reset-position-target", "v1",
      Value(Value::Object{
          {"component_runtime_identity", Value(instance.component_runtime_identity)},
          {"component", Value(instance.controllable_component_id)},
          {"provider", Value(plugin)},
          {"joints", Value(std::move(joints))},
          {"positions", Value(std::move(positions))}}));
  const auto writer = "manyforge_reset_position_" + digest.substr(7U);
  return MockResetPositionTarget{instance.component_runtime_identity,
                                 writer,
                                 runtime_namespace + "/" + writer + "/commands",
                                 instance.arm_joint_names,
                                 instance.initial_joint_positions_rad,
                                 digest,
                                 command_echo_topic};
}

/// One rate key of a controllers file; absent when it has none.
[[nodiscard]] std::optional<double> rate(const std::string_view source,
                                         const std::string_view key) {
  std::optional<double> found;
  for (const auto &line : lines_of(source)) {
    if (is_comment(line)) continue;
    const auto content = trimmed(line);
    if (!content.starts_with(key) || content.size() <= key.size() ||
        content[key.size()] != ':') {
      continue;
    }
    if (found.has_value()) fail("the controllers repeat '" + std::string(key) + "'");
    const auto text = trimmed(content.substr(key.size() + 1U));
    double value = 0.0;
    std::size_t used = 0U;
    try {
      value = std::stod(text, &used);
    } catch (const std::exception &) {
      used = 0U;
    }
    if (text.empty() || used != text.size() || !std::isfinite(value) || !(value > 0.0)) {
      fail("the controllers' '" + std::string(key) + "' is not a positive number");
    }
    found = value;
  }
  return found;
}

// The manager entry and controller block of the reset-position writer
// (the Mock and Isaac lane settings, `reset_position_writer`).
constexpr std::string_view kWriterManagerEntry =
    "    __reset_writer__:\n"
    "      type: forward_command_controller/ForwardCommandController\n";
constexpr std::string_view kWriterBlock =
    "\n/**/__reset_writer__:\n"
    "  ros__parameters:\n"
    "    joints: [__joint__]\n"
    "    interface_name: position\n";
constexpr std::string_view kManagerSelector = "/**/controller_manager:";

} // namespace

std::string with_reset_position_writer(const std::string_view controllers) {
  const auto lines = lines_of(controllers);
  const auto selector = std::find(lines.begin(), lines.end(), kManagerSelector);
  if (selector == lines.end()) fail("the controllers file has no /**/controller_manager: block");
  // The manager block runs up to the next top-level line.
  auto end = std::next(selector);
  while (end != lines.end() && (end->empty() || end->front() == ' ')) ++end;
  while (end != std::next(selector) && std::prev(end)->empty()) --end;
  std::string out;
  for (auto line = lines.begin(); line != lines.end(); ++line) {
    if (line == end) out += kWriterManagerEntry;
    out += *line + "\n";
  }
  if (end == lines.end()) out += kWriterManagerEntry;
  out += kWriterBlock;
  return out;
}

std::optional<ControllerLaneRates>
controller_lane_rates(const std::string_view controllers_yaml) {
  const auto update = rate(controllers_yaml, "update_rate");
  const auto state = rate(controllers_yaml, "state_publish_rate");
  const auto monitor = rate(controllers_yaml, "action_monitor_rate");
  if (!update || !state || !monitor) return std::nullopt;
  return ControllerLaneRates{*update, *state, *monitor};
}

std::vector<std::string>
controller_command_interfaces(const std::string_view controllers_yaml) {
  constexpr std::string_view key = "command_interfaces:";
  const auto lines = lines_of(controllers_yaml);
  std::vector<std::string> claimed;
  bool found = false;
  const auto unquoted = [](std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == ',')) text.pop_back();
    if (text.size() >= 2U && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front())
      text = text.substr(1U, text.size() - 2U);
    return text;
  };
  for (std::size_t index = 0U; index < lines.size(); ++index) {
    if (is_comment(lines[index])) continue;
    const auto content = trimmed(lines[index]);
    if (!content.starts_with(key)) continue;
    if (found) fail("the controllers repeat 'command_interfaces'");
    found = true;
    const auto rest = trimmed(content.substr(key.size()));
    if (!rest.empty()) {
      if (rest.front() != '[' || rest.find(']') == std::string::npos)
        fail("the controllers' 'command_interfaces' is not a list");
      const auto inner = rest.substr(1U, rest.find(']') - 1U);
      std::size_t cursor = 0U;
      while (cursor <= inner.size()) {
        const auto comma = inner.find(',', cursor);
        auto item = unquoted(trimmed(inner.substr(cursor, comma == std::string::npos
                                                               ? std::string::npos
                                                               : comma - cursor)));
        if (!item.empty()) claimed.push_back(std::move(item));
        if (comma == std::string::npos) break;
        cursor = comma + 1U;
      }
      continue;
    }
    const auto depth = lines[index].find_first_not_of(' ');
    while (index + 1U < lines.size()) {
      const auto &next = lines[index + 1U];
      const auto next_content = trimmed(next);
      if (next_content.empty() || is_comment(next)) {
        ++index;
        continue;
      }
      if (!next_content.starts_with("-") || next.find_first_not_of(' ') < depth) break;
      claimed.push_back(unquoted(trimmed(next_content.substr(1U))));
      ++index;
    }
  }
  return claimed;
}

bool jtc_accepts_speed_scaling(const std::vector<std::string> &command_interfaces) {
  const auto claims = [&](const std::string_view name) {
    return std::find(command_interfaces.begin(), command_interfaces.end(), name) !=
           command_interfaces.end();
  };
  return claims("position") && !claims("velocity") && !claims("acceleration") &&
         !claims("effort");
}

std::string
controller_runtime_namespace(const std::string_view controllable_component_id) {
  require_token(controllable_component_id, "controllable component id");
  return "/manyforge/" + std::string(controllable_component_id);
}

ControllerLaunchArtifacts write_launch_artifacts(const ModelLaneRow &row,
                                                 const LaunchContribution &contribution,
                                                 const LaunchInstance &instance) {
  const auto runtime_namespace =
      controller_runtime_namespace(instance.controllable_component_id);
  if (instance.artifacts_root.empty() || !instance.artifacts_root.is_absolute()) {
    fail("the artifacts root must be an absolute host-owned directory");
  }
  const auto component = "component " + instance.controllable_component_id + ": ";
  if (instance.arm_deactivated &&
      (instance.arm_joint_names.empty() || instance.gripper.has_value() ||
       !instance.max_deceleration_on_cancel_rad_s2.empty())) {
    fail(component + "a machine's process with its arm deactivated has the arm's joints "
         "and no controller values");
  }
  const bool arm = !instance.arm_joint_names.empty() && !instance.arm_deactivated;
  if (!instance.arm_deactivated && arm == instance.gripper.has_value()) {
    fail(component + "a ros2_control component moves either an arm or a tool's "
         "commanded gripper");
  }
  if (std::set<std::string>(instance.arm_joint_names.begin(),
                            instance.arm_joint_names.end())
          .size() != instance.arm_joint_names.size()) {
    fail(component + "the binding names an arm joint more than once");
  }
  for (const auto &joint : instance.arm_joint_names) require_token(joint, "arm joint");

  ControllerLaunchArtifacts result;
  result.lane = ControllerLaunchLane::kRos2Control;
  result.controllable_component_id = instance.controllable_component_id;
  result.controller_profile = row.adapter;
  result.runtime_namespace = runtime_namespace;
  result.frame_prefix = instance.frame_prefix;
  result.hardware_component_name = std::string(kHardwareComponentName);

  const auto plugin = with_instance(contribution.hardware.plugin, runtime_namespace,
                                    instance.address);
  Ros2ControlLane lane;
  lane.hardware_component_name = result.hardware_component_name;
  lane.plugin = plugin;
  for (const auto &[name, value] : contribution.hardware.parameters) {
    lane.parameters.emplace_back(name,
                                 with_instance(value, runtime_namespace, instance.address));
  }
  lane.joint_command_interfaces = contribution.hardware.joint_command_interfaces;
  lane.initial_joint_positions_rad = instance.initial_joint_positions_rad;
  lane.gripper = instance.gripper;
  std::string description;
  try {
    description = generate_controller_robot_description(ControllerRobotDescriptionRequest{
        instance.model_urdf, instance.arm_joint_names, std::move(lane)});
  } catch (const ControllerRobotDescriptionError &error) {
    fail(component + error.what());
  }

  auto controllers = with_instance(
      expand_controllers(row, contribution.controllers_yaml, instance, runtime_namespace,
                         strict_rt_resource_profile()),
      runtime_namespace, instance.address);
  if (controllers.find("__command_interfaces__") != std::string::npos) {
    std::string interfaces;
    for (const auto &interface : contribution.hardware.joint_command_interfaces) {
      require_token(interface, "hardware command interface");
      if (!interfaces.empty()) interfaces += ", ";
      interfaces += interface;
    }
    if (interfaces.empty()) fail(component + "the hardware exports no command interface");
    replace_all(controllers, "[__command_interfaces__]", "[" + interfaces + "]");
  }
  if (controllers.find("__reset_writer__") != std::string::npos) {
    result.mock_reset_position = reset_writer(instance, plugin, runtime_namespace,
                                              contribution.command_echo_topic);
    replace_all(controllers, "__reset_writer__",
                result.mock_reset_position->controller_name);
  }
  // Every `__name__` placeholder and `${...}` must be filled; ROS's own
  // `ros__parameters` key is no placeholder.
  for (auto at = controllers.find("__"); at != std::string::npos;
       at = controllers.find("__", at + 2U)) {
    auto end = at + 2U;
    while (end < controllers.size() &&
           (std::isalnum(static_cast<unsigned char>(controllers[end])) != 0 ||
            controllers[end] == '_') &&
           controllers.compare(end, 2U, "__") != 0) {
      ++end;
    }
    if (end > at + 2U && controllers.compare(end, 2U, "__") == 0) {
      fail(component + "the controllers keep the unsubstituted '" +
           controllers.substr(at, end + 2U - at) + "'");
    }
    at = end - 2U;
  }
  if (controllers.find("${") != std::string::npos) {
    fail(component + "the controllers keep an unsubstituted '${'");
  }
  if (arm) {
    result.arm_controller_name = row.controller;
  } else if (instance.gripper.has_value()) {
    result.gripper_controller_name = row.controller;
  }
  result.rates = controller_lane_rates(controllers);
  if (arm) result.arm_command_interfaces = controller_command_interfaces(controllers);

  result.directory = instance.artifacts_root / instance.controllable_component_id;
  std::error_code error;
  std::filesystem::create_directories(result.directory, error);
  if (error || !std::filesystem::is_directory(result.directory)) {
    fail("cannot create " + result.directory.string());
  }
  result.robot_description_file = result.directory / "robot_description.urdf";
  result.controllers_file = result.directory / "controllers.yaml";
  write_exact(result.robot_description_file, description);
  write_exact(result.controllers_file, controllers);
  if (contribution.driver_parameters_yaml.has_value()) {
    const auto parameters = with_instance(*contribution.driver_parameters_yaml,
                                          runtime_namespace, instance.address);
    result.driver_parameters_file = result.directory / "driver_parameters.yaml";
    write_exact(result.driver_parameters_file, parameters);
    result.driver_parameters_sha256 = content_identity::sha256_digest(parameters);
  }
  result.robot_description_sha256 = content_identity::sha256_digest(description);
  result.controllers_sha256 = content_identity::sha256_digest(controllers);
  result.controllers_yaml = controllers;

  // ros2_control.launch.py; `ros2 launch` rejects an empty `name:=` value, so
  // an absent controller, writer or prefix omits its argument.
  result.launch_arguments = {
      "robot_description_file:=" + result.robot_description_file.string(),
      "controllers_file:=" + result.controllers_file.string(),
      "runtime_namespace:=" + runtime_namespace,
      "controller_manager_name:=" + std::string(kControllerManagerNodeName),
      "hardware_plugin:=" + plugin,
  };
  if (arm) {
    result.launch_arguments.push_back("arm_controller_name:=" + result.arm_controller_name);
  } else if (result.gripper_controller_name.has_value()) {
    result.launch_arguments.push_back("gripper_controller_name:=" +
                                      *result.gripper_controller_name);
  }
  if (result.mock_reset_position.has_value()) {
    result.launch_arguments.push_back("reset_position_controller_name:=" +
                                      result.mock_reset_position->controller_name);
  }
  if (!result.frame_prefix.empty()) {
    result.launch_arguments.push_back("frame_prefix:=" + result.frame_prefix);
  }
  // A deactivated arm is not in the cell: its frames stay off the cell's TF.
  if (instance.arm_deactivated) result.launch_arguments.push_back("publish_tf:=false");
  if (!result.driver_parameters_file.empty()) {
    result.launch_arguments.push_back("driver_parameters_file:=" +
                                      result.driver_parameters_file.string());
  }
  if (contribution.world_parent_frame.has_value()) {
    require_token(*contribution.world_parent_frame, "world parent frame");
    result.launch_arguments.push_back("world_parent_frame:=" +
                                      *contribution.world_parent_frame);
  }
  if (!contribution.remappings.empty()) {
    std::vector<std::pair<std::string, std::string>> remappings;
    for (const auto &[from, to] : contribution.remappings) {
      remappings.emplace_back(with_instance(from, runtime_namespace, instance.address),
                              with_instance(to, runtime_namespace, instance.address));
    }
    result.launch_arguments.push_back("hardware_remappings:=" +
                                      remapping_argument(remappings));
  }
  return result;
}

ControllerLaunchArtifacts write_isaac_gripper_launch_artifacts(
    const std::filesystem::path &artifacts_root,
    const std::string_view controllable_component_id, const ModelLaneRow &row,
    const std::string &model_urdf, const std::string &frame_prefix) {
  if (artifacts_root.empty() || !artifacts_root.is_absolute()) {
    fail("the artifacts root must be an absolute host-owned directory");
  }
  ControllerLaunchArtifacts result;
  result.lane = ControllerLaunchLane::kIsaac;
  result.controllable_component_id = std::string(controllable_component_id);
  result.controller_profile = row.adapter;
  result.gripper_controller_name = row.controller;
  result.runtime_namespace = controller_runtime_namespace(controllable_component_id);
  result.frame_prefix = frame_prefix;
  result.directory = artifacts_root / result.controllable_component_id;
  std::error_code error;
  std::filesystem::create_directories(result.directory, error);
  if (error || !std::filesystem::is_directory(result.directory)) {
    fail("cannot create " + result.directory.string());
  }
  result.robot_description_file = result.directory / "robot_description.urdf";
  write_exact(result.robot_description_file, model_urdf);
  result.robot_description_sha256 = content_identity::sha256_digest(model_urdf);
  return result;
}

std::string
controller_launch_artifacts_evidence(const ControllerLaunchArtifacts &value) {
  std::string out = "{";
  out += "\"controllable_component_id\":" +
         json_string(value.controllable_component_id);
  out += ",\"lane\":" +
         json_string(value.lane == ControllerLaunchLane::kRos2Control ? "ros2_control"
                     : value.lane == ControllerLaunchLane::kIsaac     ? "isaac"
                     : value.lane == ControllerLaunchLane::kSimulatedPlc
                         ? "simulated_plc"
                     : value.lane == ControllerLaunchLane::kPoseSource
                         ? "pose_source"
                     : value.lane == ControllerLaunchLane::kAiModel
                         ? "ai_model"
                         : "component");
  out += ",\"controller_profile\":" + json_string(value.controller_profile);
  out += ",\"robot_description_file\":" +
         json_string(value.robot_description_file.string());
  out += ",\"robot_description_sha256\":" +
         json_string(value.robot_description_sha256);
  out += ",\"controllers_file\":" + json_string(value.controllers_file.string());
  out += ",\"controllers_sha256\":" + json_string(value.controllers_sha256);
  out += ",\"frame_prefix\":" + json_string(value.frame_prefix);
  if (!value.driver_parameters_sha256.empty()) {
    out += ",\"driver_parameters_sha256\":" +
           json_string(value.driver_parameters_sha256);
  }
  out += ",\"launch_arguments\":[";
  for (std::size_t index = 0; index < value.launch_arguments.size(); ++index) {
    if (index != 0U) out += ",";
    out += json_string(value.launch_arguments[index]);
  }
  out += "]}";
  return out;
}

} // namespace manyforge::roboplan_backend
