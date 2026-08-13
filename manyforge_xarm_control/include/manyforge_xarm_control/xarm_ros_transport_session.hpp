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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rclcpp/client.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/subscription.hpp>

#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <controller_manager_msgs/srv/set_hardware_component_state.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <xarm_msgs/msg/supervised_lifecycle_state.hpp>
#include <xarm_msgs/srv/execute_supervised_lifecycle_command.hpp>
#include <xarm_msgs/srv/set_supervised_command_gate.hpp>
#include <xarm_msgs/srv/shutdown_supervised_controller.hpp>

#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control {

/// Narrow NRT client seam for the restricted ROS boundary owned by xarm_ros2.
/// It exposes the low-level renewable command gate only for its I10 replacement
/// gate; no I7 caller/session authority is attached to it.
class IXArmSupervisedRosSession {
public:
  virtual ~IXArmSupervisedRosSession() = default;

  IXArmSupervisedRosSession(const IXArmSupervisedRosSession &) = delete;
  IXArmSupervisedRosSession &
  operator=(const IXArmSupervisedRosSession &) = delete;

  [[nodiscard]] virtual std::string_view owner_id() const noexcept = 0;
  [[nodiscard]] virtual XArmObservedState
  observe_vendor(std::int64_t now_ns) const = 0;
  virtual XArmLifecycleCommandResult
  execute_primitive(XArmLifecycleCommand command, std::string_view request_id,
                    std::uint64_t expected_generation) = 0;
  virtual bool set_command_gate(bool open, std::int64_t valid_until_ns,
                                std::uint64_t expected_generation) = 0;

protected:
  IXArmSupervisedRosSession() = default;
};

struct XArmRosTransportSessionConfig {
  std::string owner_id;
  std::string state_topic;
  std::string execute_service;
  std::string command_gate_service;
  std::string shutdown_controller_service;
  std::string controller_manager;
  std::string hardware_component_name;
  std::string joint_state_broadcaster_name;
  std::string controller_activity_topic;
  std::string trajectory_controller_name;
  std::int64_t observation_max_age_ns = 250'000'000;
  std::int64_t owner_progress_max_age_ns = 500'000'000;
  /// Endpoint discovery and non-physical command-gate response wait.
  std::int64_t service_timeout_ns = 1'000'000'000;
  /// Total discovery-plus-response budget for one physical lifecycle request.
  ///
  /// The request is sent once. Expiry after dispatch is outcome-unknown and
  /// requires reconciliation; it never triggers an automatic retry.
  std::int64_t execute_timeout_ns = 4'000'000'000;
};

/// NRT ROS client for one exact xarm_ros2 supervised owner.
///
/// A source session-id change is terminal for this object. Reconstructing the
/// client makes process replacement explicit and prevents generation reset
/// from being mistaken for continuity.
class XArmRosTransportSession final : public IXArmSupervisedRosSession {
public:
  explicit XArmRosTransportSession(XArmRosTransportSessionConfig config);
  ~XArmRosTransportSession() override;

  [[nodiscard]] std::string_view owner_id() const noexcept override;
  [[nodiscard]] XArmObservedState
  observe_vendor(std::int64_t now_ns) const override;
  XArmLifecycleCommandResult
  execute_primitive(XArmLifecycleCommand command, std::string_view request_id,
                    std::uint64_t expected_generation) override;
  bool set_command_gate(bool open, std::int64_t valid_until_ns,
                        std::uint64_t expected_generation) override;

private:
  static std::int64_t steady_now_ns();
  static std::uint8_t command_value(XArmLifecycleCommand command);
  static FactState fact(bool value);
  XArmLifecycleCommandResult
  set_hardware_component_state(bool active, std::uint64_t expected_generation);
  XArmLifecycleCommandResult
  switch_trajectory_controllers(bool active, std::uint64_t expected_generation);
  void handle_state(const xarm_msgs::msg::SupervisedLifecycleState &state);
  void handle_controller_activity(
      const controller_manager_msgs::msg::ControllerManagerActivity &activity);
  void validate_config() const;

  XArmRosTransportSessionConfig config_;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  rclcpp::Subscription<xarm_msgs::msg::SupervisedLifecycleState>::SharedPtr
      state_subscription_;
  rclcpp::Subscription<
      controller_manager_msgs::msg::ControllerManagerActivity>::SharedPtr
      controller_activity_subscription_;
  rclcpp::Client<xarm_msgs::srv::ExecuteSupervisedLifecycleCommand>::SharedPtr
      execute_client_;
  rclcpp::Client<xarm_msgs::srv::SetSupervisedCommandGate>::SharedPtr
      command_gate_client_;
  rclcpp::Client<xarm_msgs::srv::ShutdownSupervisedController>::SharedPtr
      shutdown_controller_client_;
  rclcpp::Client<controller_manager_msgs::srv::SetHardwareComponentState>::
      SharedPtr hardware_state_client_;
  rclcpp::Client<controller_manager_msgs::srv::SwitchController>::SharedPtr
      switch_controller_client_;
  std::thread executor_thread_;
  mutable std::mutex state_mutex_;
  xarm_msgs::msg::SupervisedLifecycleState latest_state_;
  std::int64_t latest_receive_ns_ = 0;
  std::uint64_t latest_owner_publication_sequence_ = 0;
  std::string source_session_id_;
  bool have_state_ = false;
  bool terminal_session_change_ = false;
  bool controller_activity_known_ = false;
  bool trajectory_controller_active_ = false;
};

} // namespace manyforge::xarm_control
