// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The xArm launch contribution of every supported robot model, generated
// from the adapter's vendor folder and the models' `lanes.real` rows, and
// checked against `vendor/manyforge_settings.yaml`.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <yaml-cpp/yaml.h>

#include <manyforge_xarm_control/xarm_launch_contribution.hpp>

namespace manyforge::xarm_control {
namespace {

const std::filesystem::path kRoot{MANYFORGE_XARM_CONTROL_SOURCE_ROOT};
const std::filesystem::path kVendor = kRoot / "manyforge_xarm_control" / "vendor";

[[nodiscard]] std::string read(const std::filesystem::path &path) {
  std::ifstream stream(path);
  std::ostringstream text;
  text << stream.rdbuf();
  return text.str();
}

/// The share folders of `vendor_share`'s packages.
[[nodiscard]] DriverShare in(const std::filesystem::path &root) {
  return [root](const std::string &package) { return root / package; };
}

/// A folder holding the driver packages' share folders with the vendor base
/// files' shape (xarm_ros2).
[[nodiscard]] std::filesystem::path vendor_share(const std::string &model,
                                                 const std::string &controller) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("manyforge_xarm_share_" + model + "_" + std::to_string(::getpid()));
  std::filesystem::create_directories(root / "xarm_controller" / "config");
  std::filesystem::create_directories(root / "xarm_api" / "config");
  std::ofstream(root / "xarm_controller" / "config" / (model + "_controllers.yaml"))
      << "controller_manager:\n  ros__parameters:\n    update_rate: 150\n"
         "    joint_state_broadcaster:\n      type: joint_state_broadcaster/JointStateBroadcaster\n"
         "    " << controller << ":\n      type: joint_trajectory_controller/JointTrajectoryController\n"
      << controller << ":\n  ros__parameters:\n    command_interfaces: [position, velocity]\n"
         "    state_interfaces: [position, velocity]\n"
         "    joints: [joint1, joint2, joint3, joint4, joint5, joint6]\n"
         "    constraints:\n      goal_time: 0.5\n      stopped_velocity_tolerance: 0.0\n"
         "      joint1: {trajectory: 1.0, goal: 0.01}\n      joint6: {trajectory: 1.0, goal: 0.01}\n"
         "    action_monitor_rate: 10.0\n";
  std::ofstream(root / "xarm_api" / "config" / "xarm_params.yaml")
      << "ufactory_driver:\n  ros__parameters:\n    joint_states: {rate: -1}\n"
         "    services:\n      debug: false\n      clean_error: true\n      clean_warn: true\n"
         "      move_joint: true\n      set_cgpio_digital: false\n";
  return root;
}

/// The generic part of a model's `lanes.real` row, as the generator reads it.
[[nodiscard]] roboplan_backend::ModelLaneRow lane_row(const YAML::Node &real) {
  roboplan_backend::ModelLaneRow row;
  row.adapter = real["adapter"].as<std::string>();
  row.controller = real["controller"].as<std::string>();
  row.arrival_tolerance_rad = real["arrival_tolerance_rad"].as<double>();
  for (const auto &entry : real)
    if (entry.second.IsScalar()) row.adapter_keys.emplace(entry.first.as<std::string>(),
                                                           entry.second.as<std::string>());
  return row;
}

TEST(XArmLaunchContribution, EverySupportedModelGeneratesItsTemplate) {
  const auto settings = YAML::LoadFile((kVendor / "manyforge_settings.yaml").string());
  for (const std::string model : {"lite6", "uf850"}) {
    const auto model_dir = kRoot / "manyforge_example_models" / "models" / model;
    const auto text = read(model_dir / "component.yaml");
    ASSERT_TRUE(parse_xarm_lane_row(text).ok()) << model;
    const auto row = lane_row(YAML::Load(text)["lanes"]["real"]);
    EXPECT_EQ(row.adapter, "xarm");
    EXPECT_EQ(row.adapter_keys.at("vendor_model"), model);
    const auto share = vendor_share(model, row.controller);
    const auto made = make_xarm_launch_contribution(row, kVendor, in(share), true);
    ASSERT_TRUE(made.ok()) << model << ": " << made.error().message;
    const auto &contribution = made.value();

    EXPECT_EQ(contribution.hardware.plugin, "uf_robot_hardware/UFRobotSystemHardware");
    EXPECT_EQ(contribution.hardware.parameters.at("report_type"), "rich");
    EXPECT_EQ(contribution.hardware.parameters.at("robot_ip"), "R${address}");
    EXPECT_EQ(contribution.hardware.parameters.at("dof"), "6");
    EXPECT_EQ(contribution.hardware.parameters.at("robot_type"),
              row.adapter_keys.at("robot_type"));
    EXPECT_EQ(contribution.hardware.joint_command_interfaces,
              (std::vector<std::string>{"position", "velocity"}));

    // The generator's template lines, exactly once each.
    const auto &yaml = contribution.controllers_yaml;
    const auto count = [&](const std::string &needle) {
      std::size_t found = 0U;
      for (auto at = yaml.find(needle); at != std::string::npos; at = yaml.find(needle, at + 1U))
        ++found;
      return found;
    };
    EXPECT_EQ(count("/**/controller_manager:"), 1U) << yaml;
    EXPECT_EQ(count("joints: [__joint__]"), 1U) << yaml;
    EXPECT_EQ(count("__joint__: {trajectory: 1.0, max_deceleration_on_cancel: null}"), 1U)
        << yaml;
    EXPECT_EQ(count("goal_time:"), 1U) << yaml;
    EXPECT_EQ(count("goal:"), 0U) << yaml;
    EXPECT_EQ(count(row.controller), 0U) << yaml;
    const auto controllers = YAML::Load(yaml);
    const auto manager = controllers["/**/controller_manager"]["ros__parameters"];
    EXPECT_EQ(manager["update_rate"].as<int>(), 150);
    EXPECT_FALSE(manager["enforce_command_limits"].as<bool>());
    EXPECT_TRUE(manager["__controller__"]["type"].IsDefined());
    EXPECT_EQ(manager["hardware_components_initial_state"]["inactive"][0].as<std::string>(),
              "${hardware_component}");
    const auto arm = controllers["/**/__controller__"]["ros__parameters"];
    // The generator writes the hardware's command interfaces.
    EXPECT_EQ(arm["command_interfaces"][0].as<std::string>(), "__command_interfaces__");
    EXPECT_DOUBLE_EQ(arm["action_monitor_rate"].as<double>(), 20.0);
    EXPECT_TRUE(arm["constraints"]["decelerate_on_cancel"].as<bool>());

    ASSERT_TRUE(contribution.driver_parameters_yaml.has_value());
    const auto driver =
        YAML::Load(*contribution.driver_parameters_yaml)["/**/ufactory_driver"]["ros__parameters"];
    std::size_t on = 0U;
    for (const auto &service : driver["services"]) on += service.second.as<bool>() ? 1U : 0U;
    EXPECT_EQ(on, settings["driver"]["services"].size());
    for (const auto &name : settings["driver"]["services"])
      EXPECT_TRUE(driver["services"][name.as<std::string>()].as<bool>()) << name.as<std::string>();
    EXPECT_FALSE(driver["services"]["move_joint"].as<bool>());
    // M7's list: clean_warn, get_state, get_servo_angle, set_mode on; the
    // digital-input reads off (the I/O stream replaced them).
    for (const auto *name : {"clean_warn", "get_state", "get_servo_angle", "set_mode"})
      EXPECT_TRUE(driver["services"][name].as<bool>()) << name;
    EXPECT_TRUE(driver["io_states"]["enable"].as<bool>());
    EXPECT_EQ(driver["io_states"]["rate"].as<int>(), 100);
    EXPECT_EQ(driver["io_states"]["tgpio_monitor_frequency"].as<int>(), 100);

    ASSERT_EQ(contribution.remappings.size(), 3U);
    EXPECT_EQ(contribution.remappings[0].to, "${namespace}/controller_manager/list_controllers");
    std::filesystem::remove_all(share);
  }
}

TEST(XArmLaunchContribution, MockAndIsaacRowsTakeTheVendorControllersWithoutTheRealHardware) {
  for (const std::string model : {"lite6", "uf850"}) {
    const auto lanes = YAML::LoadFile(
        (kRoot / "manyforge_example_models" / "models" / model / "component.yaml").string())["lanes"];
    const auto real_row = lane_row(lanes["real"]);
    const auto share = vendor_share(model, real_row.controller);
    const auto real = make_xarm_launch_contribution(real_row, kVendor, in(share), true);
    ASSERT_TRUE(real.ok()) << real.error().message;
    for (const std::string lane : {"mock", "isaac"}) {
      if (!lanes[lane]) continue;
      const auto row = lane_row(lanes[lane]);
      EXPECT_EQ(row.adapter_keys.at("vendor"), "xarm") << model << " " << lane;
      EXPECT_EQ(row.controller, real_row.controller) << model << " " << lane;
      EXPECT_FALSE(row.controllers_file.has_value());
      const auto made = make_xarm_controllers(row, kVendor, in(share));
      ASSERT_TRUE(made.ok()) << model << " " << lane << ": " << made.error().message;
      // The real lane's controllers, less only its hardware's inactive start.
      auto expected = YAML::Load(real.value().controllers_yaml);
      expected["/**/controller_manager"]["ros__parameters"]["hardware_components_initial_state"]
          .remove("inactive");
      const auto controllers = YAML::Load(made.value());
      EXPECT_EQ(YAML::Dump(controllers), YAML::Dump(expected)) << model << " " << lane;
      EXPECT_FALSE(controllers["/**/controller_manager"]["ros__parameters"]
                              ["hardware_components_initial_state"]["inactive"]);
    }
    std::filesystem::remove_all(share);
  }
}

// CONTRACTS §16, AUD-5b revised: the machine's process with its arm
// deactivated: the same hardware (starting inactive) and driver parameters
// (the I/O stream), and no arm controller at all.
TEST(XArmLaunchContribution, ArmDeactivatedProcessHasNoArmController) {
  for (const std::string model : {"lite6", "uf850"}) {
    const auto row = lane_row(YAML::LoadFile((kRoot / "manyforge_example_models" / "models" /
                                              model / "component.yaml")
                                                 .string())["lanes"]["real"]);
    const auto share = vendor_share(model, row.controller);
    const auto arm = make_xarm_launch_contribution(row, kVendor, in(share), true);
    const auto armless = make_xarm_launch_contribution(row, kVendor, in(share), false);
    ASSERT_TRUE(arm.ok() && armless.ok()) << model;
    EXPECT_EQ(armless.value().hardware.plugin, arm.value().hardware.plugin);
    EXPECT_EQ(armless.value().hardware.parameters, arm.value().hardware.parameters);
    EXPECT_EQ(armless.value().driver_parameters_yaml, arm.value().driver_parameters_yaml);
    const auto &yaml = armless.value().controllers_yaml;
    EXPECT_EQ(yaml.find("__controller__"), std::string::npos) << yaml;
    EXPECT_EQ(yaml.find("__joint__"), std::string::npos) << yaml;
    const auto manager = YAML::Load(yaml)["/**/controller_manager"]["ros__parameters"];
    EXPECT_EQ(manager["hardware_components_initial_state"]["inactive"][0].as<std::string>(),
              "${hardware_component}");
    EXPECT_TRUE(manager["update_rate"].IsDefined());
    std::filesystem::remove_all(share);
  }
}

TEST(XArmLaunchContribution, SettingsGiveTheStreamAndRebootValues) {
  const auto settings = load_xarm_vendor_settings(kVendor);
  ASSERT_TRUE(settings.ok()) << settings.error().message;
  EXPECT_DOUBLE_EQ(settings.value().io.rate_hz, 100.0);
  EXPECT_DOUBLE_EQ(settings.value().io.tgpio_monitor_frequency_hz, 100.0);
  EXPECT_EQ(xarm_io_stale_after_ns(settings.value().io), 100'000'000);
  EXPECT_EQ(xarm_io_confirmation_bound_ns(settings.value().io), 50'000'000);
  EXPECT_EQ(settings.value().reboot_wait, std::chrono::seconds(120));
  EXPECT_EQ(settings.value().reboot_probe_interval, std::chrono::milliseconds(200));
  // The stale bound follows the publish rate.
  EXPECT_EQ(xarm_io_stale_after_ns(XArmIoSettings{50.0, 100.0}), 200'000'000);
}

TEST(XArmLaunchContribution, AnIoRowGivesItsBankAndTheBitOfEachDeclaredSignal) {
  const auto tool = parse_xarm_io_row(
      R"({"adapter": "xarm", "bank": "tool", "inputs": ["TI0"], "outputs": ["TO0", "TO1"]})");
  ASSERT_TRUE(tool.ok()) << tool.error().message;
  EXPECT_EQ(tool.value().bank, XArmIoBank::kTool);
  std::vector<std::pair<std::string, int>> outputs;
  for (const auto &signal : tool.value().signals)
    if (signal.direction == core::IoSignalDirection::kOutput)
      outputs.emplace_back(signal.name, signal.pin);
  EXPECT_EQ(outputs, (std::vector<std::pair<std::string, int>>{{"TO0", 0}, {"TO1", 1}}));
  const auto controller =
      parse_xarm_io_row("{adapter: xarm, bank: controller, inputs: [CI0], outputs: [CO1, DO2]}");
  ASSERT_TRUE(controller.ok()) << controller.error().message;
  EXPECT_EQ(controller.value().bank, XArmIoBank::kController);
  ASSERT_EQ(controller.value().signals.size(), 3U);
  EXPECT_EQ(controller.value().signals[1].pin, 1);
  EXPECT_EQ(controller.value().signals[2].pin, 10);
}

TEST(XArmLaunchContribution, InvalidRowsAreRefusedTyped) {
  for (const auto *text :
       {"lanes: {}\n", "lanes: {real: {adapter: other}}\n",
        "lanes: {real: {adapter: xarm, dof: 9}}\n"}) {
    const auto row = parse_xarm_lane_row(text);
    ASSERT_FALSE(row.ok()) << text;
    EXPECT_EQ(row.error().code, "adapter_configuration_invalid");
  }
  for (const auto *text :
       {"{adapter: other, bank: tool}", "{adapter: xarm, bank: arm}",
        "{adapter: xarm, bank: tool, outputs: [TO7]}",
        "{adapter: xarm, bank: controller, inputs: [CO1]}",
        "{adapter: xarm, bank: tool, outputs: [CO1]}"}) {
    const auto row = parse_xarm_io_row(text);
    ASSERT_FALSE(row.ok()) << text;
    EXPECT_EQ(row.error().code, "adapter_configuration_invalid");
  }
}

} // namespace
} // namespace manyforge::xarm_control
