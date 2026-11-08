#pragma once

#include <cstdint>
#include <atomic>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_roboplan/adapter_transport_consumer.hpp>
#include <manyforge_roboplan/pointcloud_source_adapter.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/header.hpp>
#include <string>

namespace manyforge::ros2_control_execution {
class Ros2TransportResources;
}

namespace manyforge::roboplan_backend {

/// IPointCloudBridgeLink over the bridge's topics in the component's
/// namespace, on the host's adapter node (which the host spins): publishes
/// `forwarding` (latched), reads `watching` (latched) and `received` (one
/// message per cloud the bridge receives; arrival stamped on `clock`).
class RosPointCloudBridgeLink final : public IPointCloudBridgeLink,
                                      public AdapterTransportConsumer {
public:
  RosPointCloudBridgeLink(
      rclcpp::Node::SharedPtr node, const std::string &ros_namespace,
      std::shared_ptr<core::IClock> clock,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner =
          {});

  void SetForwarding(bool forwarding) override;
  [[nodiscard]] PointCloudBridgeObservation Observed() const override;

  /// Build replacement ROS entities without admitting their callbacks.
  /// The host activates the shared gate only after every consumer commits.
  std::shared_ptr<void> Stage(const std::shared_ptr<Owner> &expected,
                              const std::shared_ptr<Owner> &replacement,
                              Gate active) override;
  bool Validate(const std::shared_ptr<void> &staged) const override;
  void Commit(const std::shared_ptr<void> &staged) noexcept override;

private:
  struct State;
  struct Rebind;
  std::shared_ptr<State> CreateState(
      rclcpp::Node::SharedPtr node,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner,
      std::shared_ptr<std::atomic<bool>> active);
  mutable std::mutex state_mutex_;
  std::string ros_namespace_;
  std::shared_ptr<core::IClock> clock_;
  std::shared_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend
