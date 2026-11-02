#include "ros_pointcloud_bridge_link.hpp"

#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {

RosPointCloudBridgeLink::RosPointCloudBridgeLink(rclcpp::Node::SharedPtr node,
                                                 const std::string &ros_namespace,
                                                 std::shared_ptr<core::IClock> clock)
    : clock_(std::move(clock)) {
  if (!node || !clock_ || ros_namespace.empty() || ros_namespace.front() != '/')
    throw std::invalid_argument("the point-cloud bridge link needs a node, a clock and a namespace");
  const auto latched = rclcpp::QoS(1).reliable().transient_local();
  forwarding_ = node->create_publisher<std_msgs::msg::Bool>(ros_namespace + "/forwarding", latched);
  watching_ = node->create_subscription<std_msgs::msg::Bool>(
      ros_namespace + "/watching", latched, [this](const std_msgs::msg::Bool &message) {
        const std::lock_guard lock(mutex_);
        observed_.watching = message.data;
        // A new watch starts without a cloud.
        if (!message.data) observed_.last_received_ns.reset();
      });
  received_ = node->create_subscription<std_msgs::msg::Header>(
      ros_namespace + "/received", rclcpp::QoS(1).reliable(),
      [this](const std_msgs::msg::Header &) {
        const auto now = clock_->now();
        const std::lock_guard lock(mutex_);
        observed_.last_received_ns = now;
      });
}

void RosPointCloudBridgeLink::SetForwarding(const bool forwarding) {
  std_msgs::msg::Bool message;
  message.data = forwarding;
  forwarding_->publish(message);
}

PointCloudBridgeObservation RosPointCloudBridgeLink::Observed() const {
  const std::lock_guard lock(mutex_);
  return observed_;
}

}  // namespace manyforge::roboplan_backend
