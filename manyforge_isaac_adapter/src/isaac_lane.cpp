#include <manyforge_isaac_adapter/isaac_lane.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>

#include <ament_index_cpp/get_package_share_path.hpp>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace manyforge::isaac_adapter {
namespace {

constexpr std::string_view kTopicBasedPlugin = "topic_based_ros2_control/TopicBasedSystem";
constexpr std::string_view kEffortTransport = "isaac_joint_state_effort";
constexpr std::string_view kGripperCommandTransport = "ros_gripper_command";

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument("Isaac lane: " + message);
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) fail("can't read " + path.string());
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

[[nodiscard]] YAML::Node required(const YAML::Node &node, const std::string &key,
                                  const std::string &where) {
  const auto value = node[key];
  if (!value) fail(where + " has no " + key);
  return value;
}

[[nodiscard]] const std::string &key(const roboplan_backend::ModelLaneRow &row,
                                     const std::string_view name) {
  const auto found = row.adapter_keys.find(name);
  if (found == row.adapter_keys.end() || found->second.empty())
    fail("the lanes.isaac row has no " + std::string(name));
  return found->second;
}

[[nodiscard]] double number(const roboplan_backend::ModelLaneRow &row,
                            const std::string_view name) {
  const auto &text = key(row, name);
  std::size_t used{};
  double value{};
  try {
    value = std::stod(text, &used);
  } catch (const std::exception &) {
    used = 0U;
  }
  if (used != text.size() || !std::isfinite(value))
    fail("lanes.isaac " + std::string(name) + " must be a number");
  return value;
}

[[nodiscard]] double positive(const roboplan_backend::ModelLaneRow &row,
                              const std::string_view name) {
  const auto value = number(row, name);
  if (value <= 0.0) fail("lanes.isaac " + std::string(name) + " must be positive");
  return value;
}

[[nodiscard]] bool boolean(const roboplan_backend::ModelLaneRow &row,
                           const std::string_view name) {
  const auto &text = key(row, name);
  if (text == "true") return true;
  if (text == "false") return false;
  fail("lanes.isaac " + std::string(name) + " must be true or false");
}

[[nodiscard]] const std::string &topic(const roboplan_backend::ModelLaneRow &row,
                                       const std::string_view name) {
  const auto &text = key(row, name);
  if (text.front() != '/') fail("lanes.isaac " + std::string(name) + " must be an absolute topic");
  return text;
}

}  // namespace

std::filesystem::path installed_isaac_lane_dir() {
  return ament_index_cpp::get_package_share_path("manyforge_isaac_adapter") / "lane";
}

IsaacLaneSettings load_isaac_lane_settings(const std::filesystem::path &lane_dir) {
  const auto settings_file = lane_dir / "manyforge_settings.yaml";
  YAML::Node root;
  try {
    root = YAML::Load(read_file(settings_file));
  } catch (const YAML::Exception &error) {
    fail(settings_file.string() + ": " + error.what());
  }
  IsaacLaneSettings settings;
  settings.lane_dir = lane_dir;
  try {
    const auto hardware = required(root, "hardware", "manyforge_settings.yaml");
    settings.hardware.plugin = required(hardware, "plugin", "hardware").as<std::string>();
    for (const auto &parameter : required(hardware, "parameters", "hardware"))
      settings.hardware.parameters.emplace(parameter.first.as<std::string>(),
                                           parameter.second.as<std::string>());
    settings.hardware.joint_command_interfaces =
        required(hardware, "joint_command_interfaces", "hardware").as<std::vector<std::string>>();
    for (const auto &parameter : required(root, "controller_manager", "manyforge_settings.yaml"))
      settings.controller_manager.emplace(parameter.first.as<std::string>(),
                                          parameter.second.as<std::string>());
    settings.world_parent_frame =
        required(root, "world_parent_frame", "manyforge_settings.yaml").as<std::string>();
    settings.gripper_wait_for_action_server_timeout_s =
        required(required(root, "gripper", "manyforge_settings.yaml"),
                 "wait_for_action_server_timeout_s", "gripper")
            .as<double>();
  } catch (const YAML::Exception &error) {
    fail(settings_file.string() + ": " + error.what());
  }
  if (settings.hardware.plugin != kTopicBasedPlugin)
    fail("the hardware plugin must be " + std::string(kTopicBasedPlugin));
  for (const auto *name : {"joint_commands_topic", "joint_states_topic"}) {
    const auto found = settings.hardware.parameters.find(name);
    if (found == settings.hardware.parameters.end() || found->second.empty() ||
        found->second.front() != '/')
      fail(std::string("hardware.parameters.") + name + " must be an absolute topic");
  }
  if (settings.hardware.joint_command_interfaces.empty())
    fail("hardware.joint_command_interfaces is empty");
  if (!(settings.gripper_wait_for_action_server_timeout_s > 0.0))
    fail("gripper.wait_for_action_server_timeout_s must be positive");
  settings.controllers_template = read_file(lane_dir / "controllers.template.yaml");
  return settings;
}

roboplan_backend::LaunchContribution isaac_launch_contribution(
    const IsaacLaneSettings &settings, const roboplan_backend::ModelLaneRow &row,
    const bool arm) {
  roboplan_backend::LaunchContribution contribution;
  contribution.hardware = settings.hardware;
  contribution.controllers_yaml = row.vendor_controllers_yaml.value_or(settings.controllers_template);
  // The manager's process parameters go first in its block.
  constexpr std::string_view kManager = "/**/controller_manager:\n  ros__parameters:\n";
  auto &yaml = contribution.controllers_yaml;
  const auto at = yaml.find(kManager);
  if (at == std::string::npos || yaml.find(kManager, at + 1U) != std::string::npos)
    fail("the controllers have no single controller manager block");
  std::string parameters;
  for (const auto &[name, value] : settings.controller_manager)
    parameters += "    " + name + ": " + value + "\n";
  yaml.insert(at + kManager.size(), parameters);
  contribution.world_parent_frame = settings.world_parent_frame;
  // An arm's `reset_position` is Mock's stock writer; the topic-based
  // hardware publishes its position commands every cycle (threshold -1), so
  // the write is acknowledged on Isaac's joint commands topic.
  if (arm) {
    yaml = roboplan_backend::with_reset_position_writer(yaml);
    contribution.command_echo_topic = settings.hardware.parameters.at("joint_commands_topic");
  }
  return contribution;
}

std::string isaac_joint_states_topic(const IsaacLaneSettings &settings) {
  return settings.hardware.parameters.at("joint_states_topic");
}

IsaacArmFacts isaac_arm_facts(const roboplan_backend::ModelLaneRow &row) {
  if (row.controller.empty()) fail("the robot's lanes.isaac row has no controller");
  return IsaacArmFacts{row.controller, positive(row, "hard_stop_completion_timeout_s")};
}

IsaacGripperFacts isaac_gripper_facts(const roboplan_backend::ModelLaneRow &row,
                                      const IsaacLaneSettings &settings) {
  if (row.controller.empty()) fail("the tool's lanes.isaac row has no controller");
  IsaacGripperFacts facts;
  facts.transport_kind = key(row, "gripper_transport");
  facts.action_name = row.controller + "/gripper_cmd";
  facts.command_topic = topic(row, "command_topic");
  facts.state_topic = topic(row, "state_topic");
  facts.completion_policy = key(row, "completion_policy");
  facts.open_position = number(row, "open_position");
  facts.closed_position = number(row, "closed_position");
  facts.open_effort = number(row, "open_effort");
  facts.close_effort = number(row, "close_effort");
  facts.publish_nominal_position = boolean(row, "publish_nominal_position");
  const bool has_bridge = row.adapter_keys.contains("gripper_bridge");
  if (facts.transport_kind == kGripperCommandTransport) {
    if (!has_bridge) fail("a ros_gripper_command tool needs its gripper_bridge");
    facts.stop_finish_tolerance = positive(row, "gripper_stop_finish_tolerance");
    facts.bridge_parameters_file =
        settings.lane_dir / "gripper_bridges" / key(row, "gripper_bridge");
    if (!std::filesystem::is_regular_file(*facts.bridge_parameters_file))
      fail("no gripper bridge file " + facts.bridge_parameters_file->string());
  } else if (facts.transport_kind == kEffortTransport) {
    if (has_bridge) fail("an isaac_joint_state_effort tool runs no bridge");
  } else {
    fail("unknown gripper_transport '" + facts.transport_kind + "'");
  }
  return facts;
}

}  // namespace manyforge::isaac_adapter
