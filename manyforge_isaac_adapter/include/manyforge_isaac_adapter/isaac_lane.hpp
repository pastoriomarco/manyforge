#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <manyforge_roboplan/launch_contribution.hpp>
#include <optional>
#include <string>

namespace manyforge::isaac_adapter {

/// The Isaac lane adapter (lane/README.md). Isaac robots
/// run the ros2_control lane with the topic-based hardware this lane
/// contributes; Isaac tools drive their joints through the robot's
/// articulation. Per-robot values are the models' `lanes.isaac` rows, the
/// lane's own values are in its folder, per-scene pose mappings are Scenario
/// data. ROS-free.

/// The lane folder: `manyforge_settings.yaml`, `controllers.template.yaml`,
/// `gripper_bridges/`.
struct IsaacLaneSettings final {
  std::filesystem::path lane_dir;
  roboplan_backend::LaunchHardware hardware;
  /// `controllers.template.yaml`, unexpanded (the generator expands it).
  std::string controllers_template;
  /// `controller_manager`: the manager's process parameters (YAML scalar
  /// text), added to every Isaac robot's manager block.
  std::map<std::string, std::string, std::less<>> controller_manager;
  /// `world_parent_frame`: the cell frame `<prefix>world` hangs from.
  std::string world_parent_frame;
  double gripper_wait_for_action_server_timeout_s{};
};

/// The installed lane folder (`share/manyforge_isaac_adapter/lane`).
[[nodiscard]] std::filesystem::path installed_isaac_lane_dir();

/// Reads the lane folder; throws std::invalid_argument naming the missing or
/// invalid setting.
[[nodiscard]] IsaacLaneSettings load_isaac_lane_settings(const std::filesystem::path &lane_dir);

/// The lane's `LaunchContribution` for one model row: the topic-based
/// hardware, the row's vendor controllers (`vendor`) or else the lane's
/// controllers template, each with the lane's manager parameters, and the
/// world's parent frame; no driver parameters, no remappings. For an `arm`
/// also the stock reset-position writer that serves `reset_position`,
/// acknowledged on the hardware's joint commands topic (`command_echo_topic`).
[[nodiscard]] roboplan_backend::LaunchContribution isaac_launch_contribution(
    const IsaacLaneSettings &settings, const roboplan_backend::ModelLaneRow &row, bool arm);

/// The joint states topic the articulation publishes (the hardware's).
[[nodiscard]] std::string isaac_joint_states_topic(const IsaacLaneSettings &settings);

/// A robot's `lanes.isaac` row: its arm controller (`controller`) and the
/// hard Stop completion bound (`hard_stop_completion_timeout_s`).
struct IsaacArmFacts final {
  std::string controller;
  double hard_stop_completion_timeout_s{};
};

[[nodiscard]] IsaacArmFacts isaac_arm_facts(const roboplan_backend::ModelLaneRow &row);

/// A tool's `lanes.isaac` row: its gripper transport. `isaac_joint_state_effort`
/// writes effort on `command_topic` (no bridge); `ros_gripper_command` runs the
/// ParallelGripperCommand bridge with the lane's `gripper_bridges/<gripper_bridge>`.
struct IsaacGripperFacts final {
  std::string transport_kind;
  /// `<controller>/gripper_cmd`, relative to the tool's namespace.
  std::string action_name;
  std::string command_topic;
  std::string state_topic;
  std::string completion_policy;
  double open_position{};
  double closed_position{};
  double open_effort{};
  double close_effort{};
  bool publish_nominal_position{};
  double result_timeout_s{};
  /// `gripper_stop_finish_tolerance` (finger position units); only for
  /// `ros_gripper_command`, whose Stop may let a nearly finished goal finish.
  double stop_finish_tolerance{};
  /// The bridge parameters file (absolute); only for `ros_gripper_command`.
  std::optional<std::filesystem::path> bridge_parameters_file;
};

[[nodiscard]] IsaacGripperFacts isaac_gripper_facts(const roboplan_backend::ModelLaneRow &row,
                                                    const IsaacLaneSettings &settings);

}  // namespace manyforge::isaac_adapter
