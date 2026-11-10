#include "ros_pointcloud_bridge_link.hpp"

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {

struct RosPointCloudBridgeLink::State final {
  rclcpp::Node::SharedPtr node;
  std::weak_ptr<ros2_control_execution::Ros2TransportResources> owner;
  std::shared_ptr<core::IClock> clock;
  std::shared_ptr<std::atomic<bool>> active;
  mutable std::mutex mutex;
  PointCloudBridgeObservation observed;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr forwarding;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr watching;
  rclcpp::Subscription<std_msgs::msg::Header>::SharedPtr received;
};

struct RosPointCloudBridgeLink::Rebind final {
  std::shared_ptr<State> old_state;
  std::shared_ptr<State> replacement;
};

RosPointCloudBridgeLink::RosPointCloudBridgeLink(
    rclcpp::Node::SharedPtr node, const std::string &ros_namespace,
    std::shared_ptr<core::IClock> clock,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner)
    : ros_namespace_(ros_namespace), clock_(std::move(clock)) {
  if (!node || !clock_ || ros_namespace.empty() || ros_namespace.front() != '/')
    throw std::invalid_argument(
        "the point-cloud bridge link needs a node, a clock and a namespace");
  state_ = CreateState(std::move(node), std::move(owner),
                       std::make_shared<std::atomic<bool>>(true));
}

std::shared_ptr<RosPointCloudBridgeLink::State>
RosPointCloudBridgeLink::CreateState(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner,
    std::shared_ptr<std::atomic<bool>> active) {
  std::shared_ptr<State> result;
  auto create = [&] {
    auto state = std::make_shared<State>();
    state->node = std::move(node);
    state->owner = owner;
    state->clock = clock_;
    state->active = std::move(active);
    std::weak_ptr<State> weak = state;
    const auto latched = rclcpp::QoS(1).reliable().transient_local();
    state->forwarding = state->node->create_publisher<std_msgs::msg::Bool>(
        ros_namespace_ + "/forwarding", latched);
    state->watching = state->node->create_subscription<std_msgs::msg::Bool>(
        ros_namespace_ + "/watching", latched,
        [weak](const std_msgs::msg::Bool &message) {
          if (auto state = weak.lock()) {
            if (!state->active->load(std::memory_order_acquire)) return;
            const std::lock_guard lock(state->mutex);
            state->observed.watching = message.data;
            if (!message.data)
              state->observed.last_received_ns.reset();
          }
        });
    state->received = state->node->create_subscription<std_msgs::msg::Header>(
        ros_namespace_ + "/received", rclcpp::QoS(1).reliable(),
        [weak](const std_msgs::msg::Header &) {
          if (auto state = weak.lock()) {
            if (!state->active->load(std::memory_order_acquire)) return;
            const auto now = state->clock->now();
            const std::lock_guard lock(state->mutex);
            state->observed.last_received_ns = now;
          }
        });
    result = owner ? owner->retain_entity(std::move(state)) : std::move(state);
  };
  if (owner)
    owner->invoke_creation(create);
  else
    create();
  return result;
}

std::shared_ptr<void> RosPointCloudBridgeLink::Stage(
    const std::shared_ptr<Owner> &expected, const std::shared_ptr<Owner> &owner,
    Gate active) {
  if (!expected || !owner || !active || active->load(std::memory_order_acquire))
    throw std::invalid_argument("point-cloud ROS rebind requires an inactive gate and owner");
  std::shared_ptr<State> old;
  { std::lock_guard lock(state_mutex_); old = state_; }
  if (old->owner.lock() != expected)
    throw std::invalid_argument("point-cloud ROS rebind owner changed");
  return std::make_shared<Rebind>(Rebind{std::move(old),
      CreateState(owner->node(), owner, std::move(active))});
}

bool RosPointCloudBridgeLink::Validate(const std::shared_ptr<void> &staged) const {
  const auto rebind = std::static_pointer_cast<Rebind>(staged);
  if (!rebind || rebind->replacement->active->load(std::memory_order_acquire)) return false;
  std::lock_guard lock(state_mutex_);
  return state_ == rebind->old_state;
}

void RosPointCloudBridgeLink::Commit(const std::shared_ptr<void> &staged) noexcept {
  const auto rebind = std::static_pointer_cast<Rebind>(staged);
  std::lock_guard lock(state_mutex_);
  {
    std::scoped_lock observations(state_->mutex, rebind->replacement->mutex);
    rebind->replacement->observed = state_->observed;
  }
  state_ = rebind->replacement;
}

void RosPointCloudBridgeLink::SetForwarding(const bool forwarding) {
  std::shared_ptr<State> state;
  { std::lock_guard lock(state_mutex_); state = state_; }
  std_msgs::msg::Bool message;
  message.data = forwarding;
  state->forwarding->publish(message);
}

PointCloudBridgeObservation RosPointCloudBridgeLink::Observed() const {
  std::shared_ptr<State> state;
  { std::lock_guard lock(state_mutex_); state = state_; }
  const std::lock_guard lock(state->mutex);
  return state->observed;
}

} // namespace manyforge::roboplan_backend
