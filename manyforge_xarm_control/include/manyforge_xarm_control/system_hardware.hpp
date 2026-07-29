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

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <hardware_interface/system_interface.hpp>
#include <hardware_interface/types/hardware_component_interface_params.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <rclcpp/duration.hpp>
#include <rclcpp/time.hpp>
#include <rclcpp/subscription.hpp>
#include <rclcpp/timer.hpp>
#include <rclcpp_lifecycle/node_interfaces/lifecycle_node_interface.hpp>
#include <rclcpp_lifecycle/state.hpp>

#include <manyforge_robot_lifecycle/control_port.hpp>
#include <manyforge_xarm_control/coordinator.hpp>
#include <manyforge_xarm_control/fake_hardware_backend.hpp>

namespace manyforge::xarm_control
{

using CallbackReturn =
  rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

class ManyForgeXArmSystemHardware final
  : public hardware_interface::SystemInterface,
  public robot_lifecycle::LifecycleControlPort
{
public:
  CallbackReturn on_init(
    const hardware_interface::HardwareComponentInterfaceParams & params)
  override;
  CallbackReturn on_configure(
    const rclcpp_lifecycle::State & previous_state) override;
  CallbackReturn on_cleanup(
    const rclcpp_lifecycle::State & previous_state) override;
  CallbackReturn on_shutdown(
    const rclcpp_lifecycle::State & previous_state) override;
  CallbackReturn on_error(
    const rclcpp_lifecycle::State & previous_state) override;
  std::vector<hardware_interface::StateInterface> export_state_interfaces()
  override;
  std::vector<hardware_interface::CommandInterface>
  export_command_interfaces() override;
  CallbackReturn on_activate(
    const rclcpp_lifecycle::State & previous_state) override;
  CallbackReturn on_deactivate(
    const rclcpp_lifecycle::State & previous_state) override;
  hardware_interface::return_type perform_command_mode_switch(
    const std::vector<std::string> & start_interfaces,
    const std::vector<std::string> & stop_interfaces) override;
  hardware_interface::return_type read(
    const rclcpp::Time & time,
    const rclcpp::Duration & period) override;
  hardware_interface::return_type write(
    const rclcpp::Time & time,
    const rclcpp::Duration & period) override;

  bool establish_authority_session(
    const robot_lifecycle::AuthoritySession & session,
    std::int64_t now_ns) override;
  bool clear_authority_session(
    std::string_view session_id,
    std::uint64_t session_generation,
    std::int64_t now_ns) override;
  robot_lifecycle::CommandDecision submit_lifecycle(
    const robot_lifecycle::CommandRequest & request,
    std::int64_t now_ns) override;
  bool reconcile_lifecycle(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns) override;
  bool refresh_lifecycle(std::int64_t now_ns) override;
  void set_trajectory_controller_active(bool active);
  [[nodiscard]] robot_lifecycle::LifecycleSnapshot lifecycle_snapshot(
    std::int64_t now_ns) const override;
  [[nodiscard]] std::string_view transport_owner_id() const noexcept;
  [[nodiscard]] std::size_t test_only_fake_joint_write_count() const
  noexcept;
  [[nodiscard]] std::vector<double>
  test_only_fake_last_joint_write() const;
  [[nodiscard]] std::vector<double>
  test_only_fake_last_joint_velocity_write() const;

private:
  static std::int64_t steady_now_ns();
  [[nodiscard]] const std::string * hardware_parameter(
    std::string_view name) const;
  [[nodiscard]] bool configure_backend();
  void release_backend();
  void configure_nrt_callbacks(
    std::string activity_topic,
    std::int64_t refresh_period_ms);
  void handle_controller_activity(
    const controller_manager_msgs::msg::ControllerManagerActivity & activity);

  hardware_interface::HardwareInfo hardware_info_;
  std::unique_ptr<XArmLifecycleCoordinator> coordinator_;
  FakeXArmHardwareBackend * fake_backend_view_ = nullptr;
  std::vector<double> position_commands_;
  std::vector<double> velocity_commands_;
  std::vector<double> position_states_;
  std::vector<double> velocity_states_;
  JointStateSample joint_state_sample_;
  std::string trajectory_controller_name_;
  std::string controller_activity_topic_;
  robot_lifecycle::AuthorityProfile authority_profile_ =
    robot_lifecycle::AuthorityProfile::kLocalOperator;
  std::int64_t observation_lease_ns_ = 0;
  std::int64_t observation_refresh_ms_ = 0;
  mutable std::mutex nrt_mutex_;
  rclcpp::Subscription<
    controller_manager_msgs::msg::ControllerManagerActivity>::SharedPtr
    controller_activity_subscription_;
  rclcpp::TimerBase::SharedPtr observation_refresh_timer_;
};

}  // namespace manyforge::xarm_control
