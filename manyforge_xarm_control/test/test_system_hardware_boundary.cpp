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

#include <rcl/time.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include <hardware_interface/hardware_info.hpp>
#include <hardware_interface/types/hardware_component_interface_params.hpp>
#include <hardware_interface/types/hardware_interface_type_values.hpp>
#include <pluginlib/class_loader.hpp>
#include <rclcpp/duration.hpp>
#include <rclcpp/time.hpp>
#include <rclcpp_lifecycle/state.hpp>

#include <manyforge_xarm_control/coordinator.hpp>
#include <manyforge_xarm_control/system_hardware.hpp>

namespace manyforge::xarm_control
{
namespace
{

std::int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

hardware_interface::InterfaceInfo interface(std::string name)
{
  hardware_interface::InterfaceInfo result;
  result.name = std::move(name);
  result.size = 1;
  result.enable_limits = false;
  return result;
}

hardware_interface::HardwareInfo hardware_info()
{
  hardware_interface::HardwareInfo result;
  result.name = "lite6_a_system";
  result.type = "system";
  result.hardware_plugin_name =
    "manyforge_xarm_control/ManyForgeXArmSystemHardware";
  result.hardware_parameters = {
    {"backend", "fake"},
    {"fake_backend_confirmation", "ALLOW_TEST_ONLY_FAKE_BACKEND"},
    {"robot_id", "lite6-a"},
    {"transport_owner_id", "lite6-a:sole-owner"},
    {"authority_profile", "local_operator"},
    {"trajectory_controller_name", "lite6_a_trajectory_controller"},
    {"observation_lease_ms", "60000"},
  };
  for (std::size_t index = 0; index < 6; ++index) {
    hardware_interface::ComponentInfo joint;
    joint.name = "lite6_a_joint" + std::to_string(index + 1);
    joint.type = "joint";
    joint.command_interfaces = {
      interface(hardware_interface::HW_IF_POSITION),
      interface(hardware_interface::HW_IF_VELOCITY)};
    joint.state_interfaces = {
      interface(hardware_interface::HW_IF_POSITION),
      interface(hardware_interface::HW_IF_VELOCITY)};
    result.joints.push_back(std::move(joint));
  }
  return result;
}

hardware_interface::HardwareInfo resolved_real_hardware_info()
{
  auto result = hardware_info();
  result.hardware_parameters["backend"] = "real";
  result.hardware_parameters.erase("fake_backend_confirmation");
  result.hardware_parameters["robot_ip"] = "192.0.2.10";
  result.hardware_parameters["expected_robot_device_type"] = "9";
  result.hardware_parameters["access_mode"] = "supervised_lifecycle";
  result.hardware_parameters["report_type"] = "rich";
  result.hardware_parameters["dof"] = "6";
  result.hardware_parameters["robot_type"] = "lite";
  result.hardware_parameters["velocity_control"] = "false";
  result.hardware_parameters["add_gripper"] = "false";
  result.hardware_parameters["add_bio_gripper"] = "false";
  return result;
}

hardware_interface::HardwareComponentInterfaceParams params_for(
  hardware_interface::HardwareInfo info)
{
  hardware_interface::HardwareComponentInterfaceParams params;
  params.hardware_info = std::move(info);
  return params;
}

robot_lifecycle::AuthoritySession authority(std::int64_t now_ns)
{
  robot_lifecycle::AuthoritySession result;
  result.profile = robot_lifecycle::AuthorityProfile::kLocalOperator;
  result.session_id = "operator-session";
  result.generation = 1;
  result.principal_id = "human:operator";
  result.valid_until_ns = now_ns + 30'000'000'000;
  result.component_epoch = "operator-epoch-1";
  return result;
}

robot_lifecycle::CommandRequest enable_request(
  const robot_lifecycle::LifecycleSnapshot & snapshot)
{
  robot_lifecycle::CommandRequest request;
  request.robot_id = snapshot.robot_id;
  request.request_id = "enable-request";
  request.request_sequence = 1;
  request.action_id = "robot.enable";
  request.authority_profile = snapshot.authority_profile;
  request.authority_session_id = snapshot.authority_session->session_id;
  request.authority_session_generation =
    snapshot.authority_session->generation;
  request.principal_id = snapshot.authority_session->principal_id;
  request.expected_snapshot_generation = snapshot.generation;
  request.confirmation_evidence = "test-confirmation";
  return request;
}

TEST(SystemHardwareBoundaryTest, OwnershipTypesCannotBeCopied)
{
  static_assert(!std::is_copy_constructible_v<IXArmHardwareBackend>);
  static_assert(!std::is_copy_constructible_v<XArmLifecycleCoordinator>);
  static_assert(!std::is_copy_constructible_v<ManyForgeXArmSystemHardware>);
  SUCCEED();
}

TEST(SystemHardwareBoundaryTest, LoadsThroughTheDeclaredPluginBoundary)
{
  pluginlib::ClassLoader<hardware_interface::SystemInterface> loader(
    "hardware_interface", "hardware_interface::SystemInterface");

  auto instance = loader.createUniqueInstance(
    "manyforge_xarm_control/ManyForgeXArmSystemHardware");

  ASSERT_NE(instance, nullptr);
  EXPECT_NE(
    dynamic_cast<ManyForgeXArmSystemHardware *>(instance.get()), nullptr);
}

TEST(SystemHardwareBoundaryTest, RejectsAnyUnqualifiedBackendBeforeUse)
{
  auto real_info = hardware_info();
  real_info.hardware_parameters["backend"] = "real";
  ManyForgeXArmSystemHardware real_hardware;
  EXPECT_EQ(
    real_hardware.on_init(params_for(std::move(real_info))),
    CallbackReturn::ERROR);
  EXPECT_TRUE(real_hardware.transport_owner_id().empty());

  auto unconfirmed_info = hardware_info();
  unconfirmed_info.hardware_parameters.erase(
    "fake_backend_confirmation");
  ManyForgeXArmSystemHardware unconfirmed_hardware;
  EXPECT_EQ(
    unconfirmed_hardware.on_init(params_for(std::move(unconfirmed_info))),
    CallbackReturn::ERROR);
  EXPECT_TRUE(unconfirmed_hardware.transport_owner_id().empty());
}

TEST(SystemHardwareBoundaryTest, ProductionBackendCannotUseTheFakeHilPlugin)
{
  ManyForgeXArmSystemHardware hardware;
  EXPECT_EQ(
    hardware.on_init(params_for(resolved_real_hardware_info())),
    CallbackReturn::ERROR);
  EXPECT_TRUE(hardware.transport_owner_id().empty());
}

TEST(SystemHardwareBoundaryTest, ActivationObservesButNeverStartsTheRobot)
{
  ManyForgeXArmSystemHardware hardware;
  ASSERT_EQ(
    hardware.on_init(params_for(hardware_info())),
    CallbackReturn::SUCCESS);
  EXPECT_TRUE(hardware.transport_owner_id().empty());

  const rclcpp_lifecycle::State state;
  ASSERT_EQ(hardware.on_configure(state), CallbackReturn::SUCCESS);
  EXPECT_EQ(hardware.transport_owner_id(), "lite6-a:sole-owner");
  ASSERT_EQ(hardware.on_activate(state), CallbackReturn::SUCCESS);

  const auto snapshot = hardware.lifecycle_snapshot(steady_now_ns());
  EXPECT_EQ(
    snapshot.observed.hardware_component_active,
    robot_lifecycle::FactState::kTrue);
  EXPECT_EQ(
    snapshot.observed.robot_enabled,
    robot_lifecycle::FactState::kFalse);
  EXPECT_FALSE(snapshot.last_terminal_transaction.has_value());
  EXPECT_FALSE(snapshot.motion_admitted);

  ASSERT_EQ(hardware.on_deactivate(state), CallbackReturn::SUCCESS);
  ASSERT_EQ(hardware.on_cleanup(state), CallbackReturn::SUCCESS);
  EXPECT_TRUE(hardware.transport_owner_id().empty());
}

TEST(SystemHardwareBoundaryTest, FullFakePathGatesControllerWrites)
{
  ManyForgeXArmSystemHardware hardware;
  ASSERT_EQ(
    hardware.on_init(params_for(hardware_info())),
    CallbackReturn::SUCCESS);
  const rclcpp_lifecycle::State state;
  ASSERT_EQ(hardware.on_configure(state), CallbackReturn::SUCCESS);
  ASSERT_EQ(hardware.on_activate(state), CallbackReturn::SUCCESS);

  auto command_interfaces = hardware.export_command_interfaces();
  ASSERT_EQ(command_interfaces.size(), 12U);
  for (std::size_t index = 0; index < command_interfaces.size(); ++index) {
    ASSERT_TRUE(command_interfaces[index].set_value(
        index % 2 == 0 ? 0.35 : 0.05));
  }
  const rclcpp::Time loop_time(0, 0, RCL_STEADY_TIME);
  const rclcpp::Duration loop_period =
    rclcpp::Duration::from_nanoseconds(1'000'000);
  EXPECT_EQ(
    hardware.write(loop_time, loop_period),
    hardware_interface::return_type::OK);
  ASSERT_EQ(
    hardware.read(loop_time, loop_period),
    hardware_interface::return_type::OK);
  auto state_interfaces = hardware.export_state_interfaces();
  ASSERT_EQ(state_interfaces.size(), 12U);
  ASSERT_TRUE(state_interfaces.front().get_optional<double>().has_value());
  EXPECT_DOUBLE_EQ(
    *state_interfaces.front().get_optional<double>(), 0.0);

  const std::int64_t now_ns = steady_now_ns();
  ASSERT_TRUE(hardware.establish_authority_session(
    authority(now_ns), now_ns));
  ASSERT_TRUE(hardware.refresh_lifecycle(now_ns + 1));
  const auto request = enable_request(
    hardware.lifecycle_snapshot(now_ns + 1));
  ASSERT_EQ(
    hardware.submit_lifecycle(request, now_ns + 2).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  EXPECT_FALSE(
    hardware.lifecycle_snapshot(now_ns + 2).motion_admitted);
  hardware.set_trajectory_controller_active(true);
  EXPECT_TRUE(
    hardware.lifecycle_snapshot(steady_now_ns()).motion_admitted);

  EXPECT_EQ(
    hardware.write(loop_time, loop_period),
    hardware_interface::return_type::OK);
  ASSERT_EQ(
    hardware.read(loop_time, loop_period),
    hardware_interface::return_type::OK);
  state_interfaces = hardware.export_state_interfaces();
  ASSERT_TRUE(state_interfaces.front().get_optional<double>().has_value());
  EXPECT_DOUBLE_EQ(
    *state_interfaces.front().get_optional<double>(), 0.35);
  EXPECT_EQ(
    hardware.test_only_fake_last_joint_velocity_write(),
    std::vector<double>(6, 0.05));

  const std::int64_t after_activation_ns = steady_now_ns();
  ASSERT_TRUE(hardware.clear_authority_session(
    "operator-session", 1, after_activation_ns));
  for (std::size_t index = 0; index < command_interfaces.size(); ++index) {
    ASSERT_TRUE(command_interfaces[index].set_value(
        index % 2 == 0 ? 0.7 : 0.1));
  }
  std::vector<std::string> stopped_interfaces;
  for (std::size_t index = 1; index <= 6; ++index) {
    const std::string joint =
      "lite6_a_joint" + std::to_string(index);
    stopped_interfaces.push_back(joint + "/position");
    stopped_interfaces.push_back(joint + "/velocity");
  }
  EXPECT_EQ(
    hardware.perform_command_mode_switch({}, stopped_interfaces),
    hardware_interface::return_type::OK);
  for (std::size_t index = 0; index < command_interfaces.size(); ++index) {
    const auto value = command_interfaces[index].get_optional<double>();
    ASSERT_TRUE(value.has_value());
    EXPECT_DOUBLE_EQ(*value, index % 2 == 0 ? 0.35 : 0.0);
  }
  EXPECT_EQ(
    hardware.write(loop_time, loop_period),
    hardware_interface::return_type::OK);
  ASSERT_EQ(
    hardware.read(loop_time, loop_period),
    hardware_interface::return_type::OK);
  state_interfaces = hardware.export_state_interfaces();
  ASSERT_TRUE(state_interfaces.front().get_optional<double>().has_value());
  EXPECT_DOUBLE_EQ(
    *state_interfaces.front().get_optional<double>(), 0.35);
}

}  // namespace
}  // namespace manyforge::xarm_control
