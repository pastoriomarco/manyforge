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

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>

#include <rclcpp/client.hpp>
#include <rclcpp/node.hpp>
#include <xarm_msgs/srv/call.hpp>
#include <xarm_msgs/srv/get_int16_list.hpp>
#include <xarm_msgs/srv/set_digital_io.hpp>
#include <xarm_msgs/srv/set_float32_list.hpp>
#include <xarm_msgs/srv/set_int16.hpp>
#include <xarm_msgs/srv/set_tcp_load.hpp>

#include <manyforge_xarm_control/contract.hpp>
#include <manyforge_xarm_control/xarm_ros_owner.hpp>

namespace manyforge::xarm_control {

/// The driver's services below its namespace (`<driver namespace>/<name>`),
/// exactly those listed in `vendor/manyforge_settings.yaml`. The clients
/// answer on the node's executor; a call blocks its caller only.
class XArmDriverServices final : public IXArmDriver {
public:
  XArmDriverServices(rclcpp::Node &node, std::string_view driver_namespace,
                     const XArmRosOwner *ros_owner = nullptr);

  [[nodiscard]] XArmServiceCallResult CleanError(std::chrono::nanoseconds timeout,
                                                 std::stop_token cancellation) const override;
  [[nodiscard]] XArmServiceCallResult CleanWarn(std::chrono::nanoseconds timeout,
                                                std::stop_token cancellation) const override;
  /// `get_err_warn_code` (`xarm_msgs/srv/GetInt16List`, `datas` = [error, warning]).
  [[nodiscard]] XArmErrWarnRead GetErrWarnCode(std::chrono::nanoseconds timeout,
                                               std::stop_token cancellation) const override;
  [[nodiscard]] XArmServiceCallResult SetState(int state, std::chrono::nanoseconds timeout,
                                               std::stop_token cancellation) const override;
  [[nodiscard]] XArmServiceCallResult SetTcpOffset(const core::Pose3DTO &tcp_in_flange,
                                                   std::chrono::nanoseconds timeout,
                                                   std::stop_token cancellation) const override;
  [[nodiscard]] XArmServiceCallResult SetTcpLoad(double mass_kg,
                                                 const std::array<double, 3> &center_of_mass_m,
                                                 std::chrono::nanoseconds timeout,
                                                 std::stop_token cancellation) const override;
  [[nodiscard]] XArmServiceCallResult SetDigitalOutput(XArmIoBank bank, int ionum, bool value,
                                                       std::chrono::nanoseconds timeout,
                                                       std::stop_token cancellation) const override;
  [[nodiscard]] XArmServiceCallResult SystemControl(int value, std::chrono::nanoseconds timeout,
                                                    std::stop_token cancellation) const override;

  /// Preserve this driver object's identity while rebinding all clients.
  void SwapRosState(XArmDriverServices &replacement) noexcept;

private:
  template <class T> std::shared_ptr<T> Snapshot(const std::shared_ptr<T> &client) const {
    std::lock_guard lock(mutex_);
    return client;
  }
  mutable std::mutex mutex_;
  rclcpp::Logger logger_;
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_error_;
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_warn_;
  rclcpp::Client<xarm_msgs::srv::GetInt16List>::SharedPtr get_err_warn_code_;
  rclcpp::Client<xarm_msgs::srv::SetInt16>::SharedPtr set_state_;
  rclcpp::Client<xarm_msgs::srv::SetFloat32List>::SharedPtr set_tcp_offset_;
  rclcpp::Client<xarm_msgs::srv::SetTcpLoad>::SharedPtr set_tcp_load_;
  rclcpp::Client<xarm_msgs::srv::SetDigitalIO>::SharedPtr set_cgpio_digital_;
  rclcpp::Client<xarm_msgs::srv::SetDigitalIO>::SharedPtr set_tgpio_digital_;
  rclcpp::Client<xarm_msgs::srv::SetInt16>::SharedPtr system_control_;
};

} // namespace manyforge::xarm_control
