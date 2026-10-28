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

#include <memory>
#include <string>
#include <string_view>

#include <rclcpp/node.hpp>
#include <rclcpp/subscription.hpp>
#include <xarm_msgs/msg/robot_msg.hpp>

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_xarm_control/xarm_brand_lifecycle_port.hpp>

namespace manyforge::xarm_control {

/// The unmodified driver's status topic below its namespace
/// (`<component namespace>/<hw_ns>`).
[[nodiscard]] std::string xarm_robot_states_topic(std::string_view driver_namespace);

/// The production transport: one subscription to the driver's
/// `robot_states` (standard `xarm_msgs/msg/RobotMsg`). It keeps the status
/// fields of the latest report, stamped at receipt on the given clock; the
/// joint angles are kept only for the Enable gate's agreement check and the
/// pose is never read. Message callbacks run on the node's
/// executor and hold only the shared report cache, never this object.
class XArmRobotStatesTransport final : public IXArmBrandLifecycleTransport {
public:
  /// `clock` outlives every callback of the subscription.
  XArmRobotStatesTransport(rclcpp::Node &node, std::string_view driver_namespace,
                           core::IClock &clock);

  [[nodiscard]] XArmObservedState observe() const override;

  /// The subscription's reception, applied to the cache (also the test seam).
  void Receive(const xarm_msgs::msg::RobotMsg &message);

  [[nodiscard]] const std::string &topic() const noexcept { return topic_; }

private:
  struct Cache;

  std::string topic_;
  std::shared_ptr<Cache> cache_;
  rclcpp::Subscription<xarm_msgs::msg::RobotMsg>::SharedPtr subscription_;
};

} // namespace manyforge::xarm_control
