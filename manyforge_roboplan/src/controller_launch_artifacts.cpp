#include <manyforge_roboplan/controller_launch_artifacts.hpp>

#include "embedded_controller_templates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw ControllerLaunchArtifactError("controller launch artifacts: " +
                                      message);
}

/// kUFactoryReal: Real UFACTORY components (D7, ManyMove's vendor values).
enum class ArmTemplate { kUFactory, kUr, kUFactoryReal };

[[nodiscard]] std::string_view arm_template_source(const ArmTemplate value) {
  switch (value) {
  case ArmTemplate::kUFactory:
    return detail::kEmbeddedMockUFactoryControllersTemplate;
  case ArmTemplate::kUr:
    return detail::kEmbeddedMockUrControllersTemplate;
  case ArmTemplate::kUFactoryReal:
    return detail::kEmbeddedRealUFactoryControllersTemplate;
  }
  fail("unknown arm controller template");
}

[[nodiscard]] std::string_view arm_template_name(const ArmTemplate value) {
  return value == ArmTemplate::kUFactory      ? "UFactory mock arm"
         : value == ArmTemplate::kUFactoryReal ? "UFactory real arm"
                                               : "UR mock arm";
}

/// Per-joint vendor trajectory/goal constraints copied exactly from the model
/// family's vendor controller file (UFactory template rule); absent for the
/// UR template, which already carries its retained constraints.
struct VendorJointConstraints final {
  std::string_view trajectory;
  std::string_view goal;
};

struct ControllerProfileRow final {
  std::string_view profile_id;
  std::string_view hardware_component_name;
  ArmTemplate arm_template;
  std::string_view arm_controller_name;
  std::optional<VendorJointConstraints> vendor_joint_constraints;
  std::string_view gripper_controller_name;
  bool gripper_required;
  double hard_stop_completion_timeout_s;
  double gripper_result_timeout_s;
};

// Q63-Q66/Q74 offline mock controller profiles, one per offline model family.
// Arm controller names and vendor constraints are the retained controller
// files' own: xarm_ros2 c3172e57 xarm_controller/config/lite6_controllers.yaml
// (`lite6_traj_controller`, {trajectory: 1.0, goal: 0.01} on every joint) and
// ManyMove manymove_bringup/config/ur/ur_with_robotiq_controllers.yaml
// (sha256 06b1e3c5..., `joint_trajectory_controller`; its constraints are in
// Track 2's UR template). Gripper controllers follow Track 2's mapping
// (SX-280); UR5 has no gripper joint. Q79: hard-stop completion and gripper
// result timeouts are Isaac's values per model family (Lite 6 family 1.5 s /
// 5 s; UR family 0.5 s / 15 s; UR5 has no gripper, its value is unused).
constexpr double kLite6FamilyHardStopCompletionTimeoutS = 1.5;
constexpr double kLite6FamilyGripperResultTimeoutS = 5.0;
constexpr double kUrFamilyHardStopCompletionTimeoutS = 0.5;
constexpr double kUrFamilyGripperResultTimeoutS = 15.0;
constexpr std::array<ControllerProfileRow, 4U> kMockProfiles{{
    {"lite6_lite_gripper", "MockLite6LiteGripperSystem", ArmTemplate::kUFactory,
     "lite6_traj_controller", VendorJointConstraints{"1.0", "0.01"},
     "lite_gripper_controller", true, kLite6FamilyHardStopCompletionTimeoutS,
     kLite6FamilyGripperResultTimeoutS},
    {"ur10e_robotiq_2f_140", "MockUR10eRobotiq2F140System", ArmTemplate::kUr,
     "joint_trajectory_controller", std::nullopt, "robotiq_gripper_controller",
     true, kUrFamilyHardStopCompletionTimeoutS, kUrFamilyGripperResultTimeoutS},
    {"ur5_roboplan", "MockUR5System", ArmTemplate::kUr,
     "joint_trajectory_controller", std::nullopt, "", false,
     kUrFamilyHardStopCompletionTimeoutS, kUrFamilyGripperResultTimeoutS},
    {"ur5e_robotiq_2f_85", "MockUR5eRobotiq2F85System", ArmTemplate::kUr,
     "joint_trajectory_controller", std::nullopt, "robotiq_gripper_controller",
     true, kUrFamilyHardStopCompletionTimeoutS, kUrFamilyGripperResultTimeoutS},
}};

[[nodiscard]] const ControllerProfileRow &mock_row(const std::string_view id) {
  const auto found =
      std::find_if(kMockProfiles.begin(), kMockProfiles.end(),
                   [&](const auto &row) { return row.profile_id == id; });
  if (found == kMockProfiles.end()) {
    fail("unsupported mock controller profile '" + std::string(id) + "'");
  }
  return *found;
}

// CP8 step 2a / R7 package 1: Real profiles on the unmodified xArm-family
// vendor plugin (xarm_ros2 ef7351f9 `uf_robot_hardware/UFRobotSystemHardware`)
// with the values of the 2026-10-25 live probe. The controllers are Mock's
// UFactory template values (open plan decision CP7.3: Mock's template or
// ManyMove's production values); no gripper controller in this package. The
// hardware component name is the `<ros2_control name>` that starts inactive.
// The measured-state gates are per robot data, to be checked live: the joint
// freshness budget (R7 D3: 0.25 s) and `main`'s measured start/terminal state
// checks (ros2_control_capability.hpp: start and final tolerance 0.02 rad,
// `wait_for_state_timeout_s` 1.0 s). R7 package 4: the Enable gate's
// measured-versus-reported joint tolerance (§3.6; 0.02 rad, the start
// tolerance, to be checked live).
struct RealProfileRow final {
  ControllerProfileRow controllers;
  std::string_view robot_type;
  std::string_view dof;
  double joint_state_freshness_s;
  double start_tolerance_rad;
  double final_tolerance_rad;
  double completion_state_timeout_s;
  double joint_agreement_tolerance_rad;
};
constexpr std::array<RealProfileRow, 1U> kRealProfiles{{
    {{"lite6_real", "RealLite6System", ArmTemplate::kUFactoryReal,
      "lite6_traj_controller", VendorJointConstraints{"1.0", "0.01"}, "", false,
      kLite6FamilyHardStopCompletionTimeoutS, kLite6FamilyGripperResultTimeoutS},
     "lite", "6", 0.25, 0.02, 0.02, 1.0, 0.02},
}};

// The vendor driver's services this component serves. The driver serves only
// services set true (`get_parameter_or(..., false)`), so every motion or
// mode service the vendor file enables by default stays off: ManyForge is the
// only motion authority and the plugin arms through the SDK (R7 D1). Served:
// the error/warning clears (R7 recovery), one joint read, and ManyMove's I/O
// services (plan R7, "xArm I/O") and the tool payload (D11, applied at
// Enable). Every other value of the vendor
// xarm_api/config/xarm_params.yaml equals the driver's own default, so no
// vendor file or Python is read at runtime.
constexpr std::array<std::string_view, 8U> kXArmDriverServices{
    "clean_error",       "clean_warn",        "get_servo_angle",
    "get_tgpio_digital", "get_cgpio_digital", "set_tgpio_digital",
    "set_cgpio_digital", "set_tcp_load"};

// The vendor `hw_ns` of every xArm-family Real component: the driver's topics
// and services sit below `<component namespace>/xarm`.
constexpr std::string_view kXArmDriverNamespace = "xarm";

[[nodiscard]] const RealProfileRow &real_row(const std::string_view id) {
  const auto found =
      std::find_if(kRealProfiles.begin(), kRealProfiles.end(), [&](const auto &row) {
        return row.controllers.profile_id == id;
      });
  if (found == kRealProfiles.end()) {
    fail("unsupported real controller profile '" + std::string(id) + "'");
  }
  return *found;
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

/// The xArm-family hardware of one Real component. Parameters follow the vendor
/// xarm_description/urdf/<model>/<model>.ros2_control.xacro in order and value
/// format: the plugin drops the first character of `prefix` and `robot_ip`,
/// hence the vendor's `P`/`R`. The vendor prefix stays empty: ManyForge's frame
/// prefix applies to TF frames only, the joints are unprefixed, and the
/// component namespace separates duplicated robots (D6). The plugin exports
/// position and velocity commands, as the vendor xacro declares. The remaps
/// send the plugin's absolute controller-manager and RViz names to this
/// component (D6); the driver parameters key its namespaced `ufactory_driver`.
[[nodiscard]] Ros2ControlHardwareSpec
xarm_hardware(const RealProfileRow &row, const std::string_view robot_ip,
              const std::string &runtime_namespace) {
  const auto host_character = [](const char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '-' ||
           value == ':';
  };
  if (robot_ip.empty() || robot_ip.size() > 253U ||
      !std::all_of(robot_ip.begin(), robot_ip.end(), host_character)) {
    fail("real profile '" + std::string(row.controllers.profile_id) +
         "' requires the robot's address, got '" + std::string(robot_ip) + "'");
  }
  Ros2ControlHardwareSpec hardware;
  hardware.plugin = "uf_robot_hardware/UFRobotSystemHardware";
  hardware.parameters = {{"hw_ns", std::string(kXArmDriverNamespace)},
                         {"velocity_control", "false"},
                         {"prefix", "P"},
                         {"robot_ip", "R" + std::string(robot_ip)},
                         {"report_type", "rich"},
                         {"dof", std::string(row.dof)},
                         {"baud_checkset", "true"},
                         {"default_gripper_baud", "2000000"},
                         {"robot_type", std::string(row.robot_type)},
                         {"add_gripper", "false"}};
  hardware.joint_command_interfaces = {"position", "velocity"};
  hardware.seeds_initial_positions = false;
  hardware.driver_parameters_yaml =
      "# Generated by the ManyForge host at activation (CP8 step 2a): the\n"
      "# vendor driver parameters of this component's ufactory_driver.\n" +
      runtime_namespace + "/ufactory_driver:\n  ros__parameters:\n    services:\n";
  for (const auto service : kXArmDriverServices) {
    hardware.driver_parameters_yaml += "      " + std::string(service) + ": true\n";
  }
  const auto manager =
      runtime_namespace + "/" + std::string(kControllerManagerNodeName);
  hardware.remappings = {
      {"/controller_manager/list_controllers", manager + "/list_controllers"},
      {"/controller_manager/switch_controller", manager + "/switch_controller"},
      {"/rviz/moveit/update_goal_state", runtime_namespace + "/update_goal_state"}};
  hardware.initial_state_inactive = true;
  return hardware;
}

/// The stock controller manager starts this hardware component inactive.
void start_hardware_inactive(std::string &controllers,
                             const std::string_view hardware_component_name) {
  require_token(hardware_component_name, "hardware component name");
  constexpr std::string_view kBlock = "\n    hardware_components_initial_state:\n";
  const auto at = controllers.find(kBlock);
  if (at == std::string::npos ||
      controllers.find(kBlock, at + kBlock.size()) != std::string::npos) {
    fail("the controller template has no single hardware initial state block");
  }
  controllers.insert(at + kBlock.size(),
                     "      inactive: [" + std::string(hardware_component_name) +
                         "]\n");
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

/// Replace every occurrence of `token`; the template must carry exactly
/// `expected` occurrences.
[[nodiscard]] std::string replace_token(std::string text,
                                        const std::string_view token,
                                        const std::string_view value,
                                        const std::size_t expected,
                                        const std::string_view template_name) {
  std::size_t count = 0U;
  std::size_t cursor = 0U;
  while ((cursor = text.find(token, cursor)) != std::string::npos) {
    text.replace(cursor, token.size(), value);
    cursor += value.size();
    ++count;
  }
  if (count != expected) {
    fail(std::string(template_name) + " template carries " +
         std::to_string(count) + " '" + std::string(token) +
         "' substitutions, expected " + std::to_string(expected));
  }
  return text;
}

constexpr std::string_view kTemplateManagerSelector = "/**/controller_manager:";

struct ArmSection final {
  /// Lines of the manager block (selector line excluded).
  std::vector<std::string> manager;
  /// Lines after the manager block (the arm controller block).
  std::vector<std::string> rest;
};

/// Split a template into its manager block and the rest, dropping comments
/// and blank separator lines. The manager selector must appear exactly once.
[[nodiscard]] ArmSection split_manager(const std::string_view text,
                                       const std::string_view name) {
  ArmSection section;
  bool in_manager = false;
  bool seen_manager = false;
  for (const auto &line : lines_of(text)) {
    if (is_comment(line) || trimmed(line).empty()) continue;
    if (line == kTemplateManagerSelector) {
      if (seen_manager) fail(std::string(name) + " template repeats its manager");
      seen_manager = in_manager = true;
      continue;
    }
    if (!line.empty() && line.front() != ' ') in_manager = false;
    (in_manager ? section.manager : section.rest).push_back(line);
  }
  if (!seen_manager) fail(std::string(name) + " template has no manager block");
  return section;
}

[[nodiscard]] std::string
arm_yaml(const ControllerProfileRow &profile,
         const std::vector<std::string> &arm_joint_names,
         const std::map<std::string, double, std::less<>> &cancel_limits,
         std::vector<std::string> &manager, std::vector<std::string> &rest) {
  const auto name = arm_template_name(profile.arm_template);
  const auto source = arm_template_source(profile.arm_template);
  auto split = split_manager(source, name);
  manager = std::move(split.manager);
  rest = std::move(split.rest);

  std::string joint_list;
  for (const auto &joint : arm_joint_names) {
    if (!joint_list.empty()) joint_list += ", ";
    joint_list += joint;
  }
  std::vector<std::string> expanded;
  std::size_t joint_lists = 0U;
  std::size_t constraint_lines = 0U;
  for (const auto &line : rest) {
    const auto content = trimmed(line);
    if (content == "joints: [__joint__]") {
      expanded.push_back(indentation(line) + "joints: [" + joint_list + "]");
      ++joint_lists;
      continue;
    }
    if (content.starts_with("__joint__: {")) {
      constexpr std::string_view kNullBound = "max_deceleration_on_cancel: null}";
      if (!content.ends_with(kNullBound)) {
        fail(std::string(name) + " template joint constraint has no cancel "
             "bound placeholder");
      }
      const auto head = content.substr(
          std::string_view("__joint__: {").size(),
          content.size() - std::string_view("__joint__: {").size() -
              kNullBound.size());
      for (const auto &joint : arm_joint_names) {
        const auto found = cancel_limits.find(joint);
        if (found == cancel_limits.end()) {
          fail("the deployment's operator Stop profile has no maximum "
               "deceleration for arm joint '" + joint + "'");
        }
        std::string entries = head;
        if (profile.vendor_joint_constraints.has_value()) {
          if (!head.empty()) {
            fail(std::string(name) + " template already carries joint "
                 "constraints");
          }
          entries = "trajectory: " +
                    std::string(profile.vendor_joint_constraints->trajectory) +
                    ", goal: " +
                    std::string(profile.vendor_joint_constraints->goal) + ", ";
        }
        expanded.push_back(indentation(line) + joint + ": {" + entries +
                           "max_deceleration_on_cancel: " +
                           yaml_double(found->second,
                                       "max_deceleration_on_cancel of " +
                                           joint) +
                           "}");
      }
      ++constraint_lines;
      continue;
    }
    expanded.push_back(line);
  }
  if (joint_lists != 1U || constraint_lines != 1U) {
    fail(std::string(name) + " template does not carry exactly one joint list "
         "and one joint constraint placeholder");
  }
  for (const auto &[joint, limit] : cancel_limits) {
    static_cast<void>(limit);
    if (std::find(arm_joint_names.begin(), arm_joint_names.end(), joint) ==
        arm_joint_names.end()) {
      fail("the operator Stop profile names joint '" + joint +
           "', which is not an arm joint of the binding");
    }
  }
  rest = std::move(expanded);
  return std::string(name);
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

[[nodiscard]] MockControllerLaneRates
controller_lane_rates(const std::string_view source,
                      const std::string_view name) {
  const auto rate = [&](const std::string_view key) {
    std::optional<double> found;
    for (const auto &line : lines_of(source)) {
      if (is_comment(line)) continue;
      const auto content = trimmed(line);
      if (!content.starts_with(key) || content.size() <= key.size() ||
          content[key.size()] != ':') {
        continue;
      }
      if (found.has_value()) {
        fail(std::string(name) + " template repeats '" + std::string(key) +
             "'");
      }
      const auto text = trimmed(content.substr(key.size() + 1U));
      double value = 0.0;
      std::size_t used = 0U;
      try {
        value = std::stod(text, &used);
      } catch (const std::exception &) {
        used = 0U;
      }
      if (text.empty() || used != text.size() || !std::isfinite(value) ||
          !(value > 0.0)) {
        fail(std::string(name) + " template '" + std::string(key) +
             "' is not a positive number");
      }
      found = value;
    }
    if (!found.has_value()) {
      fail(std::string(name) + " template has no '" + std::string(key) + "'");
    }
    return *found;
  };
  return MockControllerLaneRates{rate("update_rate"), rate("state_publish_rate"),
                                 rate("action_monitor_rate")};
}

} // namespace

std::vector<std::string> mock_controller_profile_ids() {
  std::vector<std::string> result;
  for (const auto &row : kMockProfiles) result.emplace_back(row.profile_id);
  return result;
}

std::vector<std::string> isaac_controller_profile_ids() {
  return {"lite6", "ur10e"};
}

MockControllerProfileFacts
mock_controller_profile_facts(const std::string_view mock_profile) {
  const auto &row = mock_row(mock_profile);
  return MockControllerProfileFacts{
      std::string(row.profile_id), std::string(row.hardware_component_name),
      std::string(row.arm_controller_name),
      row.gripper_controller_name.empty()
          ? std::nullopt
          : std::optional<std::string>(row.gripper_controller_name),
      row.gripper_required, row.hard_stop_completion_timeout_s,
      row.gripper_result_timeout_s};
}

MockControllerLaneRates
mock_controller_lane_rates(const std::string_view mock_profile) {
  const auto &row = mock_row(mock_profile);
  return controller_lane_rates(arm_template_source(row.arm_template),
                               arm_template_name(row.arm_template));
}

std::vector<std::string> real_controller_profile_ids() {
  std::vector<std::string> result;
  for (const auto &row : kRealProfiles)
    result.emplace_back(row.controllers.profile_id);
  return result;
}

RealControllerProfileFacts
real_controller_profile_facts(const std::string_view real_profile) {
  const auto &row = real_row(real_profile);
  return RealControllerProfileFacts{
      std::string(row.controllers.profile_id),
      std::string(row.controllers.hardware_component_name),
      std::string(row.controllers.arm_controller_name),
      row.controllers.hard_stop_completion_timeout_s,
      row.joint_state_freshness_s,
      row.start_tolerance_rad,
      row.final_tolerance_rad,
      row.completion_state_timeout_s};
}

MockControllerLaneRates
real_controller_lane_rates(const std::string_view real_profile) {
  const auto &row = real_row(real_profile);
  return controller_lane_rates(arm_template_source(row.controllers.arm_template),
                               arm_template_name(row.controllers.arm_template));
}

MockControllerLaneRates
isaac_controller_lane_rates(const std::string_view isaac_profile) {
  return controller_lane_rates(retained_isaac_controllers_yaml(isaac_profile),
                               "Isaac arm");
}

std::string
controller_runtime_namespace(const std::string_view controllable_component_id) {
  require_token(controllable_component_id, "controllable component id");
  return "/manyforge/" + std::string(controllable_component_id);
}

namespace {

/// The controllers YAML of one Mock or Real profile row from the embedded
/// templates (the shared generator of CP8 step 2a).
[[nodiscard]] std::string component_controllers_yaml(
    const ControllerProfileRow &row,
    const std::string_view controllable_component_id,
    const std::vector<std::string> &arm_joint_names,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_rad_s2,
    const std::optional<std::string> &gripper_joint_name,
    const core::GripperOutputMode gripper_output_mode) {
  const std::string profile_id(row.profile_id);
  const auto runtime_namespace =
      controller_runtime_namespace(controllable_component_id);
  if (gripper_output_mode != core::GripperOutputMode::kCommanded &&
      gripper_output_mode != core::GripperOutputMode::kNone) {
    fail("the mock lane has an invalid gripper output mode");
  }
  const bool no_gripper_output =
      gripper_output_mode == core::GripperOutputMode::kNone;
  if (no_gripper_output &&
      (arm_joint_names.empty() || !gripper_joint_name.has_value())) {
    fail("no-output controller generation requires an arm and a model-backed gripper joint");
  }
  const bool physical_gripper = gripper_joint_name.has_value() && !no_gripper_output;
  if (arm_joint_names.empty() && !gripper_joint_name.has_value()) {
    fail("the binding names no arm joint or model-backed gripper joint");
  }
  if (std::set<std::string>(arm_joint_names.begin(), arm_joint_names.end())
          .size() != arm_joint_names.size()) {
    fail("the binding names an arm joint more than once");
  }
  for (const auto &joint : arm_joint_names) require_token(joint, "arm joint");
  if (gripper_joint_name.has_value()) {
    if (physical_gripper && row.gripper_controller_name.empty()) {
      fail("mock profile '" + profile_id +
           "' has no gripper controller for gripper joint '" +
           *gripper_joint_name + "'");
    }
    require_token(*gripper_joint_name, "gripper joint");
  } else if (row.gripper_required && !no_gripper_output) {
    fail("mock profile '" + profile_id +
         "' requires a commanded gripper joint");
  }

  std::vector<std::string> manager;
  std::vector<std::string> rest;
  std::string arm_name;
  if (!arm_joint_names.empty()) {
    arm_name = arm_yaml(row, arm_joint_names,
                       max_deceleration_on_cancel_rad_s2, manager, rest);
  } else {
    if (!max_deceleration_on_cancel_rad_s2.empty()) {
      fail("the operator Stop profile names arm joints without an arm binding");
    }
    auto split = split_manager(arm_template_source(row.arm_template),
                               "mock manager");
    manager = std::move(split.manager);
    const auto entry = std::find(manager.begin(), manager.end(),
                                 "    __arm_controller__:");
    if (entry == manager.end() || std::next(entry) == manager.end() ||
        *std::next(entry) !=
            "      type: joint_trajectory_controller/JointTrajectoryController") {
      fail("mock manager template has no exact arm controller type entry");
    }
    manager.erase(entry, std::next(entry, 2));
  }
  std::vector<std::string> gripper_manager;
  std::vector<std::string> gripper_rest;
  if (physical_gripper) {
    auto split = split_manager(detail::kEmbeddedMockGripperControllerTemplate,
                               "mock gripper");
    gripper_manager = std::move(split.manager);
    gripper_rest = std::move(split.rest);
    // The gripper manager block contributes only its controller type entry;
    // it merges into the one manager block (Q66 name, DX-175).
    if (gripper_manager.size() != 3U ||
        trimmed(gripper_manager[0]) != "ros__parameters:") {
      fail("mock gripper template manager block is not one controller type");
    }
    gripper_manager.erase(gripper_manager.begin());
  }

  std::string out;
  out += "# Generated by the ManyForge host at activation (Q63-Q69, Q74) from\n";
  if (arm_joint_names.empty()) {
    out += "# The retained mock manager and mock gripper templates";
  } else {
    out += "# Track 2's " + arm_name + " template";
    if (physical_gripper) out += " and the mock gripper template";
  }
  out += ".\n";
  out += runtime_namespace + "/" + std::string(kControllerManagerNodeName) +
         ":\n";
  out += join_lines(manager);
  out += join_lines(gripper_manager);
  out += "\n";
  out += join_lines(rest);
  if (physical_gripper) {
    out += "\n";
    out += join_lines(gripper_rest);
  }

  if (!arm_joint_names.empty()) {
    out = replace_token(std::move(out), "__arm_controller__",
                        row.arm_controller_name, 2U, arm_name);
  }
  if (physical_gripper) {
    out = replace_token(std::move(out), "__gripper_controller__",
                        row.gripper_controller_name, 2U, "mock gripper");
    out = replace_token(std::move(out), "__gripper_joint__",
                        *gripper_joint_name, 1U, "mock gripper");
  }
  if (out.find("__") != std::string::npos &&
      (out.find("__joint__") != std::string::npos ||
       out.find("__arm_controller__") != std::string::npos ||
       out.find("__gripper") != std::string::npos)) {
    fail("a template placeholder remained unsubstituted");
  }
  return out;
}

} // namespace

std::string generate_mock_controllers_yaml(
    const MockControllerProfileFacts &profile,
    const std::string_view controllable_component_id,
    const std::vector<std::string> &arm_joint_names,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_rad_s2,
    const std::optional<std::string> &gripper_joint_name,
    const core::GripperOutputMode gripper_output_mode) {
  return component_controllers_yaml(
      mock_row(profile.profile_id), controllable_component_id, arm_joint_names,
      max_deceleration_on_cancel_rad_s2, gripper_joint_name, gripper_output_mode);
}

std::string_view
retained_isaac_controllers_yaml(const std::string_view isaac_profile) {
  if (isaac_profile == "lite6") {
    return detail::kEmbeddedIsaacLite6ControllersYaml;
  }
  if (isaac_profile == "ur10e") {
    return detail::kEmbeddedIsaacUr10eControllersYaml;
  }
  fail("unsupported Isaac controller profile '" + std::string(isaac_profile) +
       "'");
}

ControllerLaunchArtifacts
write_controller_launch_artifacts(const ControllerLaunchArtifactsRequest &request) {
  const auto runtime_namespace =
      controller_runtime_namespace(request.controllable_component_id);
  if (request.artifacts_root.empty() || !request.artifacts_root.is_absolute()) {
    fail("the artifacts root must be an absolute host-owned directory");
  }
  ControllerLaunchArtifacts result;
  result.controllable_component_id = request.controllable_component_id;
  result.runtime_namespace = runtime_namespace;
  result.frame_prefix = request.frame_prefix;
  std::string description;
  std::string controllers;
  // Mock and Real share one ros2_control start-up path; only their hardware
  // data differs (CP8 step 2a).
  Ros2ControlHardwareSpec hardware;
  std::string hardware_component_name;
  try {
    std::visit(
        [&](const auto &lane) {
          using Lane = std::decay_t<decltype(lane)>;
          if constexpr (std::is_same_v<Lane, MockControllerArtifactsInput>) {
            const auto profile = mock_controller_profile_facts(lane.mock_profile);
            hardware = mock_generic_system_hardware();
            hardware_component_name = profile.hardware_component_name;
            description = generate_controller_robot_description(
                ControllerRobotDescriptionRequest{
                    request.model_urdf, request.arm_joint_names,
                    MockControllerLane{profile.hardware_component_name,
                                       lane.initial_joint_positions_rad,
                                       lane.gripper, profile.gripper_required,
                                       lane.gripper_output_mode, hardware}});
            const bool model_backed_gripper =
                lane.gripper.has_value() &&
                lane.gripper->source_joint_name.has_value();
            const std::optional<std::string> gripper_joint =
                model_backed_gripper ? lane.gripper->source_joint_name
                                     : std::nullopt;
            const bool physical_gripper = gripper_joint.has_value() &&
                lane.gripper_output_mode == core::GripperOutputMode::kCommanded;
            if (physical_gripper && !profile.gripper_controller_name.has_value()) {
              fail("mock profile '" + profile.profile_id +
                   "' has no gripper controller for the model-backed gripper "
                   "joint '" + *gripper_joint + "'");
            }
            controllers = generate_mock_controllers_yaml(
                profile, request.controllable_component_id,
                request.arm_joint_names, lane.max_deceleration_on_cancel_rad_s2,
                gripper_joint, lane.gripper_output_mode);
            result.lane = ControllerLaunchLane::kMock;
            result.controller_profile = profile.profile_id;
            if (!request.arm_joint_names.empty()) {
              result.arm_controller_name = profile.arm_controller_name;
              if (!content_identity::is_sha256_digest(
                      request.component_runtime_identity)) {
                fail("Mock position writer requires the exact component "
                     "runtime identity");
              }
              using Value = content_identity::CanonicalValue;
              Value::Array joints;
              Value::Array positions;
              for (const auto &joint : request.arm_joint_names)
                joints.emplace_back(joint);
              for (const auto position : lane.initial_joint_positions_rad)
                positions.emplace_back(position == 0.0 ? 0.0 : position);
              const auto digest = content_identity::framed_sha256(
                  "manyforge.mock-reset-position-target", "v1",
                  Value(Value::Object{
                      {"component_runtime_identity",
                       Value(request.component_runtime_identity)},
                      {"component", Value(request.controllable_component_id)},
                      {"provider", Value("mock_components/GenericSystem")},
                      {"profile", Value(profile.profile_id)},
                      {"joints", Value(std::move(joints))},
                      {"positions", Value(std::move(positions))}}));
              const auto writer =
                  "manyforge_reset_position_" + digest.substr(7U);
              result.mock_reset_position = MockResetPositionTarget{
                  request.component_runtime_identity,
                  writer,
                  runtime_namespace + "/" + writer + "/commands",
                  request.arm_joint_names,
                  lane.initial_joint_positions_rad,
                  digest};
              // One installed stock controller, configured and loaded inactive.
              // Its identity includes the exact target, so late DDS delivery
              // can only ever repeat this same immutable configuration target.
              const auto manager_end = controllers.find("\n\n");
              if (manager_end == std::string::npos)
                fail("Mock controllers have no manager block boundary");
              controllers.insert(
                  manager_end,
                  "\n    " + writer +
                      ":\n"
                      "      type: "
                      "forward_command_controller/ForwardCommandController");
              controllers +=
                  "\n/**/" + writer + ":\n  ros__parameters:\n    joints:\n";
              for (const auto &joint : request.arm_joint_names)
                controllers += "      - " + joint + "\n";
              controllers += "    interface_name: position\n";
            }
            if (physical_gripper) {
              result.gripper_controller_name = profile.gripper_controller_name;
            }
          } else if constexpr (std::is_same_v<Lane, RealControllerArtifactsInput>) {
            const auto &row = real_row(lane.real_profile);
            if (request.arm_joint_names.empty()) {
              fail("real profile '" + lane.real_profile + "' requires an arm");
            }
            hardware = xarm_hardware(row, lane.robot_ip, runtime_namespace);
            hardware_component_name = row.controllers.hardware_component_name;
            description = generate_controller_robot_description(
                ControllerRobotDescriptionRequest{
                    request.model_urdf, request.arm_joint_names,
                    MockControllerLane{hardware_component_name, {}, std::nullopt, false,
                                       core::GripperOutputMode::kCommanded,
                                       hardware},
                    request.gripper_without_control_interfaces});
            controllers = component_controllers_yaml(
                row.controllers, request.controllable_component_id,
                request.arm_joint_names, lane.max_deceleration_on_cancel_rad_s2,
                std::nullopt, core::GripperOutputMode::kCommanded);
            result.lane = ControllerLaunchLane::kMock;
            result.controller_profile = lane.real_profile;
            result.arm_controller_name = row.controllers.arm_controller_name;
            result.vendor_adapter = VendorAdapterSelection{
                "xarm", runtime_namespace + "/" + std::string(kXArmDriverNamespace),
                std::stoi(std::string(row.dof)),
                row.joint_agreement_tolerance_rad};
          } else {
            description = generate_controller_robot_description(
                ControllerRobotDescriptionRequest{
                    request.model_urdf, request.arm_joint_names, lane.lane,
                    request.gripper_without_control_interfaces});
            controllers = std::string(
                retained_isaac_controllers_yaml(lane.isaac_profile));
            result.lane = ControllerLaunchLane::kIsaac;
            result.controller_profile = lane.isaac_profile;
            result.arm_controller_name = "isaac_arm_controller";
          }
        },
        request.lane);
  } catch (const ControllerRobotDescriptionError &error) {
    fail(std::string("component ") + request.controllable_component_id + ": " +
         error.what());
  }
  if (hardware.initial_state_inactive) {
    start_hardware_inactive(controllers, hardware_component_name);
  }

  result.directory = request.artifacts_root / request.controllable_component_id;
  std::error_code error;
  std::filesystem::create_directories(result.directory, error);
  if (error || !std::filesystem::is_directory(result.directory)) {
    fail("cannot create " + result.directory.string());
  }
  result.robot_description_file = result.directory / "robot_description.urdf";
  result.controllers_file = result.directory / "controllers.yaml";
  write_exact(result.robot_description_file, description);
  write_exact(result.controllers_file, controllers);
  if (!hardware.driver_parameters_yaml.empty()) {
    result.driver_parameters_file = result.directory / "driver_parameters.yaml";
    write_exact(result.driver_parameters_file, hardware.driver_parameters_yaml);
    result.driver_parameters_sha256 =
        content_identity::sha256_digest(hardware.driver_parameters_yaml);
  }
  result.robot_description_sha256 =
      content_identity::sha256_digest(description);
  result.controllers_sha256 = content_identity::sha256_digest(controllers);
  result.controllers_yaml = controllers;

  if (result.lane == ControllerLaunchLane::kMock) {
    // Track 2's mock_control.launch.py contract (DX-175).
    result.launch_arguments = {
        "robot_description_file:=" + result.robot_description_file.string(),
        "controllers_file:=" + result.controllers_file.string(),
        "runtime_namespace:=" + runtime_namespace,
        "controller_manager_name:=" + std::string(kControllerManagerNodeName),
    };
    if (!result.arm_controller_name.empty()) {
      result.launch_arguments.push_back("arm_controller_name:=" +
                                        result.arm_controller_name);
    }
    if (result.mock_reset_position.has_value()) {
      result.launch_arguments.push_back(
          "reset_position_controller_name:=" +
          result.mock_reset_position->controller_name);
    }
    // `ros2 launch` rejects an empty `name:=` value; an absent controller or
    // frame prefix omits its argument (the launch defaults are empty).
    if (result.gripper_controller_name.has_value()) {
      result.launch_arguments.push_back("gripper_controller_name:=" +
                                        *result.gripper_controller_name);
    }
    if (!result.frame_prefix.empty()) {
      result.launch_arguments.push_back("frame_prefix:=" + result.frame_prefix);
    }
    // The shared launch defaults to Mock's plugin, no driver parameters and no
    // remappings, so a Mock component passes none of these.
    if (hardware.plugin != mock_generic_system_hardware().plugin) {
      result.launch_arguments.push_back("hardware_plugin:=" + hardware.plugin);
    }
    if (!result.driver_parameters_file.empty()) {
      result.launch_arguments.push_back("driver_parameters_file:=" +
                                        result.driver_parameters_file.string());
    }
    if (!hardware.remappings.empty()) {
      result.launch_arguments.push_back("hardware_remappings:=" +
                                        remapping_argument(hardware.remappings));
    }
  } else {
    // Track 2's launch-isaac-topic-based-control.sh contract (DX-168).
    result.launch_environment = {
        {"MANYFORGE_ROBOT_DESCRIPTION_FILE",
         result.robot_description_file.string()},
        {"MANYFORGE_CONTROLLERS_FILE", result.controllers_file.string()},
        {"ISAAC_TOPIC_CONTROL_PROFILE", result.controller_profile},
        {"MANYFORGE_ISAAC_COMPONENT_ID", result.controllable_component_id},
    };
    if (!result.frame_prefix.empty()) {
      result.launch_environment.emplace_back("MANYFORGE_FRAME_PREFIX",
                                             result.frame_prefix);
    }
  }
  return result;
}

ControllerLaunchArtifacts write_component_controller_launch_artifacts(
    const core::RuntimeParticipantBinding &motion_binding,
    const ComponentControllerLaunchInput &input) {
  const auto &motion = motion_binding.value();
  const auto &component = motion.controllable_component_id.value();
  if (input.gripper_output_mode != core::GripperOutputMode::kCommanded &&
      input.gripper_output_mode != core::GripperOutputMode::kNone) {
    fail("component " + component + " has an invalid gripper output mode");
  }
  if (input.gripper_output_mode == core::GripperOutputMode::kNone &&
      (!input.gripper_binding ||
       input.gripper_binding->value().capability_id.value() != "tool.gripper.command" ||
       input.gripper_binding->value().resources.size() != 1U ||
       !input.gripper_binding->value().resources.front().source_joint_name.has_value())) {
    fail("component " + component + " no-output mode requires its genuine scalar model-backed gripper binding");
  }
  if (static_cast<int>(input.isaac_profile.has_value()) +
          static_cast<int>(input.mock_profile.has_value()) +
          static_cast<int>(input.real_profile.has_value()) !=
      1) {
    fail("component " + component +
         " must select exactly one controller profile");
  }
  if (input.gripper_binding.has_value() &&
      input.gripper_binding->value().controllable_component_id !=
          motion.controllable_component_id) {
    fail("component " + component +
         " gripper binding addresses another component");
  }
  ControllerLaunchArtifactsRequest request;
  request.artifacts_root = input.artifacts_root;
  request.controllable_component_id = component;
  request.model_urdf = input.model_urdf;
  request.frame_prefix = input.frame_prefix;
  request.component_runtime_identity = input.component_runtime_identity;
  try {
    request.arm_joint_names =
        controller_joint_names_from_binding(motion.resources);
  } catch (const ControllerRobotDescriptionError &error) {
    fail("component " + component + ": " + error.what());
  }
  if (input.isaac_profile.has_value()) {
    if (!input.isaac_lane.has_value()) {
      fail("component " + component + " has no Isaac controller lane facts");
    }
    if (input.gripper_output_mode == core::GripperOutputMode::kNone) {
      request.gripper_without_control_interfaces = MockGripperBinding{
          input.gripper_binding->value().resources.front().source_joint_name,
          input.gripper_projected_position};
    }
    request.lane =
        IsaacControllerArtifactsInput{*input.isaac_profile, *input.isaac_lane};
    return write_controller_launch_artifacts(request);
  }
  // Q48: resource limits become per-joint bounds through the binding's own
  // resource-to-source-joint map, never by position.
  std::map<std::string, double, std::less<>> cancel_limits;
  for (const auto &[resource_id, limit] :
       input.controlled_stop_max_deceleration_rad_s2) {
    const auto resource = std::find_if(
        motion.resources.begin(), motion.resources.end(),
        [&](const auto &candidate) { return candidate.resource_id == resource_id; });
    if (resource == motion.resources.end()) {
      fail("component " + component + " Stop limit names resource " +
           resource_id.value() + ", which its arm binding does not hold");
    }
    if (!cancel_limits.emplace(*resource->source_joint_name, limit).second) {
      fail("component " + component + " repeats a Stop limit joint");
    }
  }
  if (input.real_profile.has_value()) {
    if (input.gripper_binding.has_value()) {
      const auto &resources = input.gripper_binding->value().resources;
      if (input.gripper_output_mode != core::GripperOutputMode::kNone ||
          input.gripper_actuation_kind != core::GripperActuationKind::kPneumatic ||
          resources.size() != 1U || !resources.front().source_joint_name.has_value()) {
        fail("component " + component +
             " real gripper must be an exact no-output pneumatic target");
      }
      request.gripper_without_control_interfaces = MockGripperBinding{
          resources.front().source_joint_name, input.gripper_projected_position};
    } else if (input.gripper_output_mode != core::GripperOutputMode::kCommanded ||
               input.gripper_actuation_kind.has_value()) {
      fail("component " + component + " has gripper settings without a target");
    }
    if (!input.initial_joint_positions_rad.empty()) {
      fail("component " + component +
           " is a real robot that reports its own pose; the deployment "
           "supplies no initial joint positions");
    }
    request.lane = RealControllerArtifactsInput{
        *input.real_profile, input.robot_ip, std::move(cancel_limits)};
    return write_controller_launch_artifacts(request);
  }
  MockControllerArtifactsInput lane;
  lane.mock_profile = *input.mock_profile;
  lane.gripper_output_mode = input.gripper_output_mode;
  lane.initial_joint_positions_rad = input.initial_joint_positions_rad;
  lane.max_deceleration_on_cancel_rad_s2 = std::move(cancel_limits);
  if (input.gripper_binding.has_value()) {
    const auto &resources = input.gripper_binding->value().resources;
    if (resources.size() != 1U) {
      fail("component " + component +
           " gripper binding has no exact scalar resource");
    }
    MockGripperBinding gripper;
    gripper.source_joint_name = resources.front().source_joint_name;
    if (gripper.source_joint_name.has_value()) {
      gripper.projected_initial_position = input.gripper_projected_position;
    }
    lane.gripper = std::move(gripper);
  }
  request.lane = std::move(lane);
  return write_controller_launch_artifacts(request);
}

ControllerLaunchArtifacts write_component_controller_launch_artifacts(
    const std::optional<core::RuntimeParticipantBinding> &motion_binding,
    const ComponentControllerLaunchInput &input) {
  if (motion_binding.has_value()) {
    return write_component_controller_launch_artifacts(*motion_binding, input);
  }
  if (input.gripper_output_mode != core::GripperOutputMode::kCommanded) {
    fail("a component without motion and physical gripper output has no controller artifacts");
  }
  if (!input.gripper_binding.has_value()) {
    fail("a component without motion must hold a genuine gripper binding");
  }
  const auto &gripper = input.gripper_binding->value();
  const auto &component = gripper.controllable_component_id.value();
  if (gripper.capability_id.value() != "tool.gripper.command") {
    fail("component " + component +
         " without motion must hold the direct gripper command binding");
  }
  if (!input.mock_profile.has_value() || input.isaac_profile.has_value() ||
      input.real_profile.has_value()) {
    fail("component " + component +
         " without motion must select exactly one mock controller profile");
  }
  if (!input.initial_joint_positions_rad.empty() ||
      !input.controlled_stop_max_deceleration_rad_s2.empty()) {
    fail("component " + component +
         " has arm startup positions or Stop limits without an arm binding");
  }
  if (gripper.resources.size() != 1U ||
      !gripper.resources.front().source_joint_name.has_value()) {
    fail("component " + component +
         " gripper binding has no exact scalar model-backed resource");
  }
  const MockGripperBinding binding{
      gripper.resources.front().source_joint_name,
      input.gripper_projected_position};
  return write_controller_launch_artifacts(ControllerLaunchArtifactsRequest{
      input.artifacts_root, component, input.model_urdf, {},
      MockControllerArtifactsInput{*input.mock_profile, {}, {}, binding},
      input.frame_prefix});
}

std::string
controller_launch_artifacts_evidence(const ControllerLaunchArtifacts &value) {
  std::string out = "{";
  out += "\"controllable_component_id\":" +
         json_string(value.controllable_component_id);
  out += ",\"lane\":" +
         json_string(value.lane == ControllerLaunchLane::kMock    ? "mock"
                     : value.lane == ControllerLaunchLane::kIsaac ? "isaac"
                     : value.lane == ControllerLaunchLane::kSimulatedPlc
                         ? "simulated_plc"
                     : value.lane == ControllerLaunchLane::kPoseSource
                         ? "pose_source"
                         : "ai_model");
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
  out += "],\"launch_environment\":{";
  for (std::size_t index = 0; index < value.launch_environment.size(); ++index) {
    if (index != 0U) out += ",";
    out += json_string(value.launch_environment[index].first) + ":" +
           json_string(value.launch_environment[index].second);
  }
  out += "}}";
  return out;
}

} // namespace manyforge::roboplan_backend
