// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0

#include "ros_io_bank_link.hpp"

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {

namespace {

[[nodiscard]] std::uint64_t mask(const std::array<bool, 64> &bits) {
  std::uint64_t made = 0U;
  for (std::size_t index = 0U; index < bits.size(); ++index)
    if (bits[index]) made |= std::uint64_t{1} << index;
  return made;
}

} // namespace

std::string io_bank_topic_prefix(const std::string &component_id) {
  return "/" + component_id + "/io_bank";
}

struct RosIoBankLink::State final {
  rclcpp::Node::SharedPtr node;
  std::weak_ptr<ros2_control_execution::Ros2TransportResources> owner;
  std::shared_ptr<std::atomic<bool>> active;
  rclcpp::CallbackGroup::SharedPtr group;
  rclcpp::Subscription<manyforge_io_interfaces::msg::IoBankState>::SharedPtr state;
  rclcpp::Publisher<manyforge_io_interfaces::msg::IoBankCommand>::SharedPtr command;
};

struct RosIoBankLink::Rebind final {
  std::shared_ptr<State> old_state;
  std::shared_ptr<State> replacement;
};

RosIoBankLink::RosIoBankLink(
    rclcpp::Node::SharedPtr node, std::string component_id, std::shared_ptr<core::IClock> clock,
    OnState on_state, std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner)
    : component_id_(std::move(component_id)), prefix_(io_bank_topic_prefix(component_id_)),
      clock_(std::move(clock)), on_state_(std::move(on_state)),
      logger_(node ? node->get_logger().get_child("io_bank") : rclcpp::get_logger("io_bank")) {
  if (!node || !clock_ || !on_state_)
    throw std::invalid_argument("the I/O bank link needs a node, a clock and its component");
  state_ = CreateState(std::move(node), std::move(owner),
                       std::make_shared<std::atomic<bool>>(true));
}

std::shared_ptr<RosIoBankLink::State> RosIoBankLink::CreateState(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner,
    std::shared_ptr<std::atomic<bool>> active) {
  std::shared_ptr<State> result;
  auto create = [&] {
    auto state = std::make_shared<State>();
    state->node = std::move(node);
    state->owner = owner;
    state->active = std::move(active);
    // Inputs feed interrupt rules: the time-critical group (OD-66).
    if (owner) state->group = owner->create_time_critical_callback_group();
    rclcpp::SubscriptionOptions options;
    options.callback_group = state->group;
    // Only the latest sample matters; each one carries the whole bank.
    state->state = state->node->create_subscription<manyforge_io_interfaces::msg::IoBankState>(
        prefix_ + "/state", rclcpp::SensorDataQoS().keep_last(1),
        [clock = clock_, on_state = on_state_, gate = state->active](
            const manyforge_io_interfaces::msg::IoBankState::ConstSharedPtr message) {
          if (!gate->load(std::memory_order_acquire)) return;
          on_state(IoBankStateSample{message->cycle, message->applied_seq,
                                     message->applied_result, message->device_code,
                                     mask(message->inputs), mask(message->outputs),
                                     mask(message->refused), clock->now()});
        },
        options);
    state->command = state->node->create_publisher<manyforge_io_interfaces::msg::IoBankCommand>(
        prefix_ + "/command", rclcpp::QoS(1).reliable());
    auto group = state->group;
    result = owner ? owner->retain_entity(std::move(state), std::move(group)) : std::move(state);
  };
  if (owner)
    owner->invoke_creation(create);
  else
    create();
  return result;
}

bool RosIoBankLink::Send(const IoBankCommandMessage &command) {
  std::shared_ptr<State> state;
  {
    std::lock_guard lock(state_mutex_);
    state = state_;
  }
  manyforge_io_interfaces::msg::IoBankCommand message;
  message.seq = command.seq;
  message.values = command.values;
  if (const auto owner = state->owner.lock())
    return owner->try_sdk_send([&] { state->command->publish(message); });
  state->command->publish(message);
  return true;
}

void RosIoBankLink::Record(const IoBankWriteRecord &record) {
  // For latency measurement on the device: one line per command.
  RCLCPP_DEBUG(logger_,
               "io_bank command component=%s seq=%u outputs=%zu sent_at_ns=%lld "
               "decided_at_ns=%lld latency_ms=%.3f outcome=%s",
               component_id_.c_str(), record.seq, record.outputs,
               static_cast<long long>(record.sent_at_ns),
               static_cast<long long>(record.decided_at_ns.value_or(-1)),
               record.decided_at_ns
                   ? static_cast<double>(*record.decided_at_ns - record.sent_at_ns) / 1e6
                   : -1.0,
               std::string(core::to_string(record.outcome)).c_str());
}

std::shared_ptr<void> RosIoBankLink::Stage(const std::shared_ptr<Owner> &expected,
                                           const std::shared_ptr<Owner> &owner, Gate active) {
  if (!expected || !owner || !active || active->load(std::memory_order_acquire))
    throw std::invalid_argument("I/O bank ROS rebind requires an inactive gate and owner");
  std::shared_ptr<State> old;
  {
    std::lock_guard lock(state_mutex_);
    old = state_;
  }
  if (old->owner.lock() != expected)
    throw std::invalid_argument("I/O bank ROS rebind owner changed");
  return std::make_shared<Rebind>(
      Rebind{std::move(old), CreateState(owner->node(), owner, std::move(active))});
}

bool RosIoBankLink::Validate(const std::shared_ptr<void> &staged) const {
  const auto rebind = std::static_pointer_cast<Rebind>(staged);
  if (!rebind || rebind->replacement->active->load(std::memory_order_acquire)) return false;
  std::lock_guard lock(state_mutex_);
  return state_ == rebind->old_state;
}

void RosIoBankLink::Commit(const std::shared_ptr<void> &staged) noexcept {
  const auto rebind = std::static_pointer_cast<Rebind>(staged);
  std::lock_guard lock(state_mutex_);
  state_ = rebind->replacement;
}

} // namespace manyforge::roboplan_backend
