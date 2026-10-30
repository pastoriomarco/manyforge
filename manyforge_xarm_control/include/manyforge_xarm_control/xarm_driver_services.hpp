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

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <rclcpp/client.hpp>
#include <rclcpp/node.hpp>
#include <array>
#include <xarm_msgs/srv/call.hpp>
#include <xarm_msgs/srv/get_int16.hpp>
#include <xarm_msgs/srv/get_int16_list.hpp>
#include <xarm_msgs/srv/set_float32_list.hpp>
#include <xarm_msgs/srv/set_int16.hpp>
#include <xarm_msgs/srv/set_tcp_load.hpp>

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control {

/// R7 §2.3: one vendor service call is bounded by the shared component call
/// cap (M6 XADAPT: 10 s until measured live); a timeout is an unknown
/// outcome that the next status report reconciles.
inline constexpr std::chrono::nanoseconds kXArmServiceResponseTimeout{
    core::kComponentCallResponseCapNs};

enum class XArmServiceCallOutcome : std::uint8_t {
  /// Refused before the request left (service missing, unsupported command,
  /// cancelled first).
  kNotSent,
  /// The driver answered `ret == 0`.
  kSucceeded,
  /// Sent, but no answer in time, cancelled, or a non-zero vendor return:
  /// the effect is unknown and is never retried.
  kUnknown,
};

struct XArmServiceCallResult final {
  XArmServiceCallOutcome outcome{XArmServiceCallOutcome::kNotSent};
  /// Typed reason token (`xarm_service_*`); empty on success.
  std::string reason;
  /// The vendor's `ret`, when an answer arrived.
  std::optional<std::int32_t> vendor_return;
};

/// M6 XADAPT D11: one on-demand `get_err_warn_code` read: the call, and the
/// controller's error and warning codes when it answered `ret == 0`.
struct XArmErrWarnRead final {
  XArmServiceCallResult call;
  std::optional<int> error;
  std::optional<int> warning;
};

/// DX-2984: one on-demand `get_state` read and its state value.
struct XArmStateRead final {
  XArmServiceCallResult call;
  std::optional<int> state;
};

/// The unmodified driver's two recovery services below its namespace
/// (`<driver namespace>/clean_error` and `clean_warn`, `xarm_msgs/srv/Call`),
/// dispatched as the catalog's `device.clear_error` and `xarm.clear_warning`.
/// The clients answer on the node's executor; `Call` blocks its caller only.
class XArmDriverServices final {
public:
  XArmDriverServices(rclcpp::Node &node, std::string_view driver_namespace);

  /// One request, never retried (R7 §1): `kClearError` or `kClearWarning`.
  [[nodiscard]] XArmServiceCallResult
  Call(XArmLifecycleCommand command, std::chrono::nanoseconds timeout,
       std::stop_token cancellation) const;

  /// TOOLCTX: one `set_tcp_load` request (`xarm_msgs/srv/SetTcpLoad`), never
  /// retried: the payload in kg and its centre of mass in metres in the
  /// flange frame (sent to the driver in millimetres).
  [[nodiscard]] XArmServiceCallResult
  SetTcpLoad(double mass_kg, const std::array<double, 3> &center_of_mass_m,
             std::chrono::nanoseconds timeout,
             std::stop_token cancellation) const;

  /// TOOLCTX: one `set_tcp_offset` request (`xarm_msgs/srv/SetFloat32List`),
  /// never retried: the TCP in the flange frame, sent as `xarm_tcp_offset`.
  [[nodiscard]] XArmServiceCallResult
  SetTcpOffset(const core::Pose3DTO &tcp_in_flange,
               std::chrono::nanoseconds timeout,
               std::stop_token cancellation) const;

  /// M6 ADAPTERM: one `set_mode` / `set_state` request
  /// (`xarm_msgs/srv/SetInt16`), never retried: re-engaging the robot at rest
  /// after a move from its own interface (joint-servo mode 1, state 0).
  [[nodiscard]] XArmServiceCallResult
  SetMode(int mode, std::chrono::nanoseconds timeout,
          std::stop_token cancellation) const;
  [[nodiscard]] XArmServiceCallResult
  SetState(int state, std::chrono::nanoseconds timeout,
           std::stop_token cancellation) const;

  /// M6 XADAPT D11: one `get_err_warn_code` request
  /// (`xarm_msgs/srv/GetInt16List`; the driver answers `datas` = [error,
  /// warning], `xarm_driver_service.cpp:521-530`), never retried here.
  [[nodiscard]] XArmErrWarnRead
  GetErrWarnCode(std::chrono::nanoseconds timeout, std::stop_token cancellation) const;

  /// DX-2984: one on-demand `get_state` request (`xarm_msgs/srv/GetInt16`),
  /// never retried here.
  [[nodiscard]] XArmStateRead
  GetState(std::chrono::nanoseconds timeout, std::stop_token cancellation) const;

private:
  // M6 XADAPT: the node's logger, for each call's measured duration.
  rclcpp::Logger logger_;
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_error_;
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_warn_;
  rclcpp::Client<xarm_msgs::srv::SetTcpLoad>::SharedPtr set_tcp_load_;
  rclcpp::Client<xarm_msgs::srv::SetFloat32List>::SharedPtr set_tcp_offset_;
  rclcpp::Client<xarm_msgs::srv::SetInt16>::SharedPtr set_mode_;
  rclcpp::Client<xarm_msgs::srv::SetInt16>::SharedPtr set_state_;
  rclcpp::Client<xarm_msgs::srv::GetInt16>::SharedPtr get_state_;
  rclcpp::Client<xarm_msgs::srv::GetInt16List>::SharedPtr get_err_warn_code_;
};

/// TOOLCTX: the driver's TCP offset of a pose in the flange frame:
/// `[x, y, z]` in millimetres, then `[roll, pitch, yaw]` in radians (the
/// driver's SDK runs with `is_radian`), fixed axes X-Y-Z, so
/// R = Rz(yaw) Ry(pitch) Rx(roll), as xArm Studio shows it. Verify live.
[[nodiscard]] std::array<double, 6> xarm_tcp_offset(const core::Pose3DTO &pose);

} // namespace manyforge::xarm_control
