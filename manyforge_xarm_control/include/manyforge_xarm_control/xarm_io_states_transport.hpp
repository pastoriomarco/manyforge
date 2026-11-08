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
#include <string_view>

#include <rclcpp/node.hpp>
#include <rclcpp/subscription.hpp>
#include <xarm_msgs/msg/cio_state.hpp>
#include <xarm_msgs/msg/io_states.hpp>

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_xarm_control/xarm_owner_adapter.hpp>
#include <manyforge_xarm_control/xarm_ros_owner.hpp>

namespace manyforge::xarm_control {

/// The machine's two I/O subscriptions below its driver namespace, held by
/// its owner: `xarm_io_states` (`xarm_msgs/IOStates`, the fork's port-30000
/// stream), stamped at receipt and handed to the owner, which
/// hands each I/O member its half; and `xarm_cgpio_states`
/// (`xarm_msgs/CIOState`, the rich report) for the GPIO module's health and
/// pin configuration. Callbacks run on the node's executor and hold only the
/// owner adapter.
class XArmIoStatesTransport final {
public:
  XArmIoStatesTransport(rclcpp::Node &node, std::string_view driver_namespace,
                        core::IClock &clock, std::shared_ptr<XArmOwnerAdapter> owner,
                        const XArmRosOwner *ros_owner = nullptr);

private:
  rclcpp::Subscription<xarm_msgs::msg::IOStates>::SharedPtr io_states_;
  rclcpp::Subscription<xarm_msgs::msg::CIOState>::SharedPtr cgpio_states_;
};

} // namespace manyforge::xarm_control
