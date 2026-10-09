// Copyright 2026 Flexin Group SRL
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

#include <cmath>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "manyforge_isaac_hardware/isaac_system.hpp"
#include "hardware_interface/types/hardware_component_interface_params.hpp"
#include "hardware_interface/system.hpp"
#include "pluginlib/class_loader.hpp"

namespace
{
using hardware_interface::CallbackReturn;

class ScopedRosContext
{
public:
  ScopedRosContext() {rclcpp::init(0, nullptr);}
  ~ScopedRosContext() {rclcpp::shutdown();}
};

hardware_interface::HardwareComponentInterfaceParams parameters()
{
  hardware_interface::HardwareComponentInterfaceParams params;
  params.hardware_info = hardware_interface::HardwareInfo{};
  auto & info = params.hardware_info;
  info.name = "test_arm";
  info.type = "system";
  info.hardware_parameters = {
    {"joint_states_topic", "/isaac_joint_states"},
    {"feedback_source_clock", "system"},
    {"source_max_age_ns", "100000000"}, {"receive_max_age_ns", "50000000"}};
  for (int i = 0; i < 6; ++i) {
    hardware_interface::ComponentInfo joint{};
    joint.name = "joint" + std::to_string(i);
    hardware_interface::InterfaceInfo position{};
    position.name = "position";
    position.initial_value = "42.0";
    joint.state_interfaces.push_back(position);
    joint.command_interfaces.push_back(position);
    info.joints.push_back(joint);
  }
  return params;
}

TEST(IsaacSystem, AuthoredInitialValuesNeverMasqueradeAsCurrentStateOrCommand)
{
  manyforge_isaac_hardware::IsaacSystem system;
  ASSERT_EQ(system.on_init(parameters()), CallbackReturn::SUCCESS);
  const auto states = system.on_export_state_interfaces();
  const auto commands = system.on_export_command_interfaces();
  ASSERT_EQ(states.size(), 6U);
  ASSERT_EQ(commands.size(), 6U);
  for (const auto & state : states) {
    const auto value = state->get_optional<double>();
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(std::isnan(*value));
  }
  for (const auto & command : commands) {
    const auto value = command->get_optional<double>();
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(std::isnan(*value));
  }
  EXPECT_EQ(system.on_activate(rclcpp_lifecycle::State{}), CallbackReturn::FAILURE);
  EXPECT_EQ(system.write(rclcpp::Time{0}, rclcpp::Duration{0, 0}),
      hardware_interface::return_type::OK);
  EXPECT_EQ(system.read(rclcpp::Time{0}, rclcpp::Duration{0, 0}),
      hardware_interface::return_type::OK);
  EXPECT_EQ(system.on_cleanup(rclcpp_lifecycle::State{}), CallbackReturn::SUCCESS);
}

TEST(IsaacSystem, RequiresSixPositionJointsAndExplicitPositiveFreshnessLimits)
{
  auto params = parameters();
  params.hardware_info.joints.pop_back();
  manyforge_isaac_hardware::IsaacSystem short_arm;
  EXPECT_EQ(short_arm.on_init(params), CallbackReturn::ERROR);
  params = parameters();
  params.hardware_info.hardware_parameters.erase("source_max_age_ns");
  manyforge_isaac_hardware::IsaacSystem missing_limit;
  EXPECT_EQ(missing_limit.on_init(params), CallbackReturn::ERROR);
  params = parameters();
  params.hardware_info.hardware_parameters["receive_max_age_ns"] = "0";
  manyforge_isaac_hardware::IsaacSystem zero_limit;
  EXPECT_EQ(zero_limit.on_init(params), CallbackReturn::ERROR);
  params.hardware_info.hardware_parameters["receive_max_age_ns"] = "10bad";
  manyforge_isaac_hardware::IsaacSystem malformed_limit;
  EXPECT_EQ(malformed_limit.on_init(params), CallbackReturn::ERROR);
}

TEST(IsaacSystem, RejectsDuplicateNamesMimicAndMixedVelocityScope)
{
  auto params = parameters();
  params.hardware_info.joints[5].name = params.hardware_info.joints[0].name;
  manyforge_isaac_hardware::IsaacSystem duplicate;
  EXPECT_EQ(duplicate.on_init(params), CallbackReturn::ERROR);
  params = parameters();
  params.hardware_info.joints[0].parameters["mimic"] = "joint1";
  manyforge_isaac_hardware::IsaacSystem mimic;
  EXPECT_EQ(mimic.on_init(params), CallbackReturn::ERROR);
  params = parameters();
  hardware_interface::InterfaceInfo velocity{};
  velocity.name = "velocity";
  params.hardware_info.joints[0].state_interfaces.push_back(velocity);
  manyforge_isaac_hardware::IsaacSystem mixed;
  EXPECT_EQ(mixed.on_init(params), CallbackReturn::ERROR);
}

TEST(IsaacSystem, RejectsNonDoubleCommandInterfaces)
{
  auto params = parameters();
  params.hardware_info.joints[0].command_interfaces[0].data_type = "bool";
  manyforge_isaac_hardware::IsaacSystem system;
  EXPECT_EQ(system.on_init(params), CallbackReturn::ERROR);
}

TEST(IsaacSystem, RequiresAnExplicitKnownFeedbackClock)
{
  auto params = parameters();
  params.hardware_info.hardware_parameters.erase("feedback_source_clock");
  manyforge_isaac_hardware::IsaacSystem missing;
  EXPECT_EQ(missing.on_init(params), CallbackReturn::ERROR);
  params.hardware_info.hardware_parameters["feedback_source_clock"] = "guessed";
  manyforge_isaac_hardware::IsaacSystem unknown;
  EXPECT_EQ(unknown.on_init(params), CallbackReturn::ERROR);
  params.hardware_info.hardware_parameters["feedback_source_clock"] = "ros_simulation";
  manyforge_isaac_hardware::IsaacSystem simulation;
  EXPECT_EQ(simulation.on_init(params), CallbackReturn::SUCCESS);
}

TEST(IsaacSystem, InstalledPluginConfiguresInactiveAndImmediatelyCleansUpRepeatedly)
{
  ScopedRosContext context;
  pluginlib::ClassLoader<hardware_interface::SystemInterface> loader(
    "hardware_interface", "hardware_interface::SystemInterface");
  // The framework wrapper takes a standard unique_ptr. The loader outlives the
  // component, so this unmanaged plugin is deleted before its library unloads.
  std::unique_ptr<hardware_interface::SystemInterface> plugin(
    loader.createUnmanagedInstance("manyforge_isaac_hardware/IsaacSystem"));
  ASSERT_NE(plugin, nullptr);
  hardware_interface::System hardware(std::move(plugin));
  hardware_interface::HardwareComponentParams component_params;
  component_params.hardware_info = parameters().hardware_info;
  component_params.clock = std::make_shared<rclcpp::Clock>(RCL_SYSTEM_TIME);
  ASSERT_EQ(
    hardware.initialize(component_params).id(),
    lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED);
  ASSERT_EQ(hardware.export_state_interfaces().size(), 6U);
  ASSERT_EQ(hardware.export_command_interfaces().size(), 6U);
  for (int cycle = 0; cycle < 8; ++cycle) {
    ASSERT_EQ(hardware.configure().id(), lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE);
    EXPECT_EQ(hardware.activate().id(), lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE);
    // Immediate cleanup deliberately gives the worker no startup grace/sleep.
    EXPECT_EQ(hardware.cleanup().id(), lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED);
  }
}

}  // namespace
