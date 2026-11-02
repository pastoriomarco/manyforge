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

#include <manyforge_xarm_control/xarm_io_states_transport.hpp>

#include <cstddef>
#include <utility>

#include <rclcpp/qos.hpp>

#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>

namespace manyforge::xarm_control {

XArmIoStatesTransport::XArmIoStatesTransport(rclcpp::Node &node,
                                             const std::string_view driver_namespace,
                                             core::IClock &clock,
                                             std::shared_ptr<XArmOwnerAdapter> owner) {
  // Validates the namespace the same way as the status topic.
  static_cast<void>(xarm_robot_states_topic(driver_namespace));
  const auto ns = std::string(driver_namespace);
  // Only the latest sample matters; each one carries the whole I/O block.
  io_states_ = node.create_subscription<xarm_msgs::msg::IOStates>(
      ns + "/xarm_io_states", rclcpp::SensorDataQoS().keep_last(1),
      [owner, &clock](const xarm_msgs::msg::IOStates::ConstSharedPtr message) {
        owner->OnIoStates({message->cgpio_digital_inputs, message->cgpio_digital_outputs,
                           message->tgpio_digital_inputs, message->tgpio_digital_outputs,
                           clock.now()});
      });
  cgpio_states_ = node.create_subscription<xarm_msgs::msg::CIOState>(
      ns + "/xarm_cgpio_states", rclcpp::SensorDataQoS().keep_last(1),
      [owner](const xarm_msgs::msg::CIOState::ConstSharedPtr message) {
        XArmGpioModuleStatus status{message->state, message->code, {}};
        for (std::size_t pin = 0U; pin < status.output_functions.size(); ++pin)
          status.output_functions[pin] = message->output_conf[pin];
        owner->OnGpioModuleStatus(status);
      });
}

} // namespace manyforge::xarm_control
