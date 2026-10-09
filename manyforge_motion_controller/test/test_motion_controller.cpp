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

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "controller_interface/controller_interface.hpp"
#include "controller_interface/controller_interface_params.hpp"
#include "hardware_interface/handle.hpp"
#include "hardware_interface/hardware_info.hpp"
#include "hardware_interface/loaned_command_interface.hpp"
#include "hardware_interface/loaned_state_interface.hpp"
#include "lifecycle_msgs/msg/state.hpp"
#include "pluginlib/class_loader.hpp"
#include "rclcpp/rclcpp.hpp"

namespace
{

using Controller = controller_interface::ControllerInterface;
using LifecycleState = lifecycle_msgs::msg::State;

class MotionControllerTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    rclcpp::init(0, nullptr);
  }

  static void TearDownTestSuite()
  {
    rclcpp::shutdown();
  }

  std::shared_ptr<Controller> load_controller(const std::vector<std::string> & joints)
  {
    auto controller = loader_.createSharedInstance("manyforge_motion_controller/MotionController");
    controller_interface::ControllerInterfaceParams params;
    params.controller_name = "manyforge_motion_controller_test";
    params.update_rate = 100U;
    params.controller_manager_update_rate = 100U;
    params.node_options.enable_rosout(false);
    params.node_options.start_parameter_services(false);
    params.node_options.enable_logger_service(false);
    params.node_options.parameter_overrides({rclcpp::Parameter("joints", joints)});
    EXPECT_EQ(controller->init(params), controller_interface::return_type::OK);
    return controller;
  }

  // The loader outlives every local controller instance.
  pluginlib::ClassLoader<Controller> loader_{
    "controller_interface", "controller_interface::ControllerInterface"};
};

TEST_F(MotionControllerTest, LoadsAndConfiguresInactiveWithOrderedResourceRequests)
{
  const auto controller = load_controller({"joint_b", "joint_a"});
  EXPECT_EQ(controller->get_lifecycle_state().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  EXPECT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_INACTIVE);

  const auto command = controller->command_interface_configuration();
  EXPECT_EQ(command.type, controller_interface::interface_configuration_type::INDIVIDUAL);
  EXPECT_EQ(command.names, (std::vector<std::string>{"joint_b/position", "joint_a/position"}));

  const auto state = controller->state_interface_configuration();
  EXPECT_EQ(state.type, controller_interface::interface_configuration_type::INDIVIDUAL);
  EXPECT_EQ(
    state.names,
    (std::vector<std::string>{
      "joint_b/position", "joint_b/velocity", "joint_a/position", "joint_a/velocity"}));
}

TEST_F(MotionControllerTest, RefusesEmptyJointList)
{
  const auto controller = load_controller({});
  EXPECT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  EXPECT_TRUE(controller->command_interface_configuration().names.empty());
  EXPECT_TRUE(controller->state_interface_configuration().names.empty());
}

TEST_F(MotionControllerTest, RefusesEmptyJointName)
{
  const auto controller = load_controller({"joint_a", ""});
  EXPECT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  EXPECT_TRUE(controller->command_interface_configuration().names.empty());
  EXPECT_TRUE(controller->state_interface_configuration().names.empty());
}

TEST_F(MotionControllerTest, RefusesDuplicateJointNames)
{
  const auto controller = load_controller({"joint_a", "joint_a"});
  EXPECT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  EXPECT_TRUE(controller->command_interface_configuration().names.empty());
  EXPECT_TRUE(controller->state_interface_configuration().names.empty());
}

TEST_F(MotionControllerTest, CleanupDropsOldClaimsAndReconfigureUsesCurrentJoints)
{
  const auto controller = load_controller({"joint_a"});
  ASSERT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_INACTIVE);
  EXPECT_EQ(controller->get_node()->cleanup().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  EXPECT_TRUE(controller->command_interface_configuration().names.empty());
  EXPECT_TRUE(controller->state_interface_configuration().names.empty());

  ASSERT_TRUE(
    controller->get_node()->set_parameter(
      rclcpp::Parameter("joints", std::vector<std::string>{"joint_c"})).successful);
  ASSERT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_INACTIVE);
  EXPECT_EQ(
    controller->command_interface_configuration().names,
    (std::vector<std::string>{"joint_c/position"}));
}

TEST_F(MotionControllerTest, FailedReconfigurationKeepsNoPreviousClaims)
{
  const auto controller = load_controller({"joint_a"});
  ASSERT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_INACTIVE);
  ASSERT_EQ(controller->get_node()->cleanup().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  ASSERT_TRUE(
    controller->get_node()->set_parameter(
      rclcpp::Parameter("joints", std::vector<std::string>{"joint_b", "joint_b"})).successful);
  EXPECT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  EXPECT_TRUE(controller->command_interface_configuration().names.empty());
  EXPECT_TRUE(controller->state_interface_configuration().names.empty());
}

TEST_F(MotionControllerTest, ConfigurationRejectedActivationAndUpdatePreserveCommand)
{
  const auto controller = load_controller({"joint_a"});
  hardware_interface::InterfaceInfo info{};
  info.name = "position";
  info.initial_value = "2.5";
  const auto command = std::make_shared<hardware_interface::CommandInterface>(
    hardware_interface::InterfaceDescription("joint_a", info));
  const auto position = std::make_shared<hardware_interface::StateInterface>(
    hardware_interface::InterfaceDescription("joint_a", info));
  info.name = "velocity";
  info.initial_value = "0.0";
  const auto velocity = std::make_shared<hardware_interface::StateInterface>(
    hardware_interface::InterfaceDescription("joint_a", info));

  std::vector<hardware_interface::LoanedCommandInterface> commands;
  commands.emplace_back(command, []() {});
  std::vector<hardware_interface::LoanedStateInterface> states;
  states.emplace_back(position);
  states.emplace_back(velocity);
  controller->assign_interfaces(std::move(commands), std::move(states));

  ASSERT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_INACTIVE);
  EXPECT_EQ(command->get_optional<double>(), 2.5);
  EXPECT_EQ(controller->get_node()->activate().id(), LifecycleState::PRIMARY_STATE_INACTIVE);
  EXPECT_EQ(command->get_optional<double>(), 2.5);

  for (const auto period_ns : {10000000LL, 0LL, -1LL, 1000000000LL}) {
    EXPECT_EQ(
      controller->update(rclcpp::Time(1000000000LL), rclcpp::Duration::from_nanoseconds(period_ns)),
      controller_interface::return_type::ERROR);
    EXPECT_EQ(command->get_optional<double>(), 2.5);
  }
  controller->release_interfaces();
  EXPECT_EQ(controller->get_node()->cleanup().id(), LifecycleState::PRIMARY_STATE_UNCONFIGURED);
  EXPECT_EQ(command->get_optional<double>(), 2.5);
}

TEST_F(MotionControllerTest, UnconfiguredUpdateIsUnavailableWithoutInterfaces)
{
  const auto controller = load_controller({"joint_a"});
  EXPECT_EQ(
    controller->update(rclcpp::Time(0LL), rclcpp::Duration::from_nanoseconds(10000000LL)),
    controller_interface::return_type::ERROR);
}

TEST_F(MotionControllerTest, ShutdownFinalizesWithoutActivatingMotion)
{
  const auto controller = load_controller({"joint_a"});
  ASSERT_EQ(controller->configure().id(), LifecycleState::PRIMARY_STATE_INACTIVE);
  EXPECT_EQ(controller->get_node()->shutdown().id(), LifecycleState::PRIMARY_STATE_FINALIZED);
  EXPECT_TRUE(controller->command_interface_configuration().names.empty());
  EXPECT_TRUE(controller->state_interface_configuration().names.empty());
}

}  // namespace
