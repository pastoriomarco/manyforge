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
                                             std::shared_ptr<XArmOwnerAdapter> owner,
                                             const XArmRosOwner *ros_owner) {
  // Validates the namespace the same way as the status topic.
  static_cast<void>(xarm_robot_states_topic(driver_namespace));
  const auto ns = std::string(driver_namespace);
  const auto create = [&] {
    // Two bundles: io_states (interrupt inputs) runs in its own time-critical
    // group (OD-66) and retires with it; cgpio_states stays ordinary.
    struct IoEntities final {
      rclcpp::Node::SharedPtr node;
      std::shared_ptr<core::IClock> clock;
      rclcpp::CallbackGroup::SharedPtr group;
      rclcpp::Subscription<xarm_msgs::msg::IOStates>::SharedPtr io_states;
    };
    struct Entities final {
      rclcpp::Node::SharedPtr node;
      std::shared_ptr<core::IClock> clock;
      rclcpp::Subscription<xarm_msgs::msg::CIOState>::SharedPtr cgpio_states;
    };
    auto io = std::make_shared<IoEntities>();
    auto entities = std::make_shared<Entities>();
    if (ros_owner) {
      io->node = entities->node = ros_owner->node;
      io->clock = entities->clock = ros_owner->clock;
      if (ros_owner->time_critical_group) io->group = ros_owner->time_critical_group();
    }
    rclcpp::SubscriptionOptions io_options;
    io_options.callback_group = io->group;
    // Only the latest sample matters; each one carries the whole I/O block.
    io->io_states = node.create_subscription<xarm_msgs::msg::IOStates>(
        ns + "/xarm_io_states", rclcpp::SensorDataQoS().keep_last(1),
        [owner, &clock, active = ros_owner ? ros_owner->active : nullptr](const xarm_msgs::msg::IOStates::ConstSharedPtr message) {
          if (active && !active->load(std::memory_order_acquire)) return;
          owner->OnIoStates({message->cgpio_digital_inputs, message->cgpio_digital_outputs,
                             message->tgpio_digital_inputs, message->tgpio_digital_outputs,
                             clock.now()});
        }, io_options);
    std::shared_ptr<void> io_held = io;
    if (ros_owner)
      io_held = io->group && ros_owner->retain_time_critical
          ? ros_owner->retain_time_critical(std::move(io_held), io->group)
          : ros_owner->retain(std::move(io_held));
    io_states_ = decltype(io_states_)(io_held, static_cast<IoEntities *>(io_held.get())->io_states.get());
    entities->cgpio_states = node.create_subscription<xarm_msgs::msg::CIOState>(
        ns + "/xarm_cgpio_states", rclcpp::SensorDataQoS().keep_last(1),
        [owner, active = ros_owner ? ros_owner->active : nullptr](const xarm_msgs::msg::CIOState::ConstSharedPtr message) {
          if (active && !active->load(std::memory_order_acquire)) return;
          XArmGpioModuleStatus status{message->state, message->code, {}};
          for (std::size_t pin = 0U; pin < status.output_functions.size(); ++pin)
            status.output_functions[pin] = message->output_conf[pin];
          owner->OnGpioModuleStatus(status);
        });
    std::shared_ptr<void> held = ros_owner ? ros_owner->retain(std::move(entities)) : entities;
    auto *raw = static_cast<Entities *>(held.get());
    cgpio_states_ = decltype(cgpio_states_)(held, raw->cgpio_states.get());
  };
  if (ros_owner) ros_owner->invoke(create);
  else create();
}

} // namespace manyforge::xarm_control
