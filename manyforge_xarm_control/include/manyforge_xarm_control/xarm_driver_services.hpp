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
#include <xarm_msgs/srv/set_tcp_load.hpp>

#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control {

/// R7 §2.3: one vendor service call is bounded at 4 s; a timeout is an
/// unknown outcome that the next status report reconciles.
inline constexpr std::chrono::nanoseconds kXArmServiceResponseTimeout =
    std::chrono::seconds(4);

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

/// The unmodified driver's two recovery services below its namespace
/// (`<driver namespace>/clean_error` and `clean_warn`, `xarm_msgs/srv/Call`),
/// dispatched as the catalog's `xarm.clear_error` and `xarm.clear_warning`.
/// The clients answer on the node's executor; `Call` blocks its caller only.
class XArmDriverServices final {
public:
  XArmDriverServices(rclcpp::Node &node, std::string_view driver_namespace);

  /// One request, never retried (R7 §1): `kClearError` or `kClearWarning`.
  [[nodiscard]] XArmServiceCallResult
  Call(XArmLifecycleCommand command, std::chrono::nanoseconds timeout,
       std::stop_token cancellation) const;

  /// D11: one `set_tcp_load` request (`xarm_msgs/srv/SetTcpLoad`), never
  /// retried: the tool payload in kg and its centre of mass in metres in the
  /// flange frame (sent to the driver in millimetres).
  [[nodiscard]] XArmServiceCallResult
  SetTcpLoad(double mass_kg, const std::array<double, 3> &center_of_mass_m,
             std::chrono::nanoseconds timeout,
             std::stop_token cancellation) const;

private:
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_error_;
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_warn_;
  rclcpp::Client<xarm_msgs::srv::SetTcpLoad>::SharedPtr set_tcp_load_;
};

} // namespace manyforge::xarm_control
