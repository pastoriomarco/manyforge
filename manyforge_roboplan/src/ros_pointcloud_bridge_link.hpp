#pragma once

#include <cstdint>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_roboplan/pointcloud_source_adapter.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/header.hpp>
#include <string>

namespace manyforge::roboplan_backend {

/// IPointCloudBridgeLink over the bridge's topics in the component's
/// namespace, on the host's adapter node (which the host spins): publishes
/// `forwarding` (latched), reads `watching` (latched) and `received` (one
/// message per cloud the bridge receives; arrival stamped on `clock`).
class RosPointCloudBridgeLink final : public IPointCloudBridgeLink {
 public:
  RosPointCloudBridgeLink(rclcpp::Node::SharedPtr node, const std::string &ros_namespace,
                          std::shared_ptr<core::IClock> clock);

  void SetForwarding(bool forwarding) override;
  [[nodiscard]] PointCloudBridgeObservation Observed() const override;

 private:
  std::shared_ptr<core::IClock> clock_;
  mutable std::mutex mutex_;
  PointCloudBridgeObservation observed_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr forwarding_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr watching_;
  rclcpp::Subscription<std_msgs::msg::Header>::SharedPtr received_;
};

}  // namespace manyforge::roboplan_backend
