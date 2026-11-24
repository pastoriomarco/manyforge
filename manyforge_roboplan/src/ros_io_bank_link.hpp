// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_io_interfaces/msg/io_bank_command.hpp>
#include <manyforge_io_interfaces/msg/io_bank_state.hpp>
#include <manyforge_roboplan/adapter_transport_consumer.hpp>
#include <manyforge_roboplan/io_bank_component.hpp>
#include <rclcpp/rclcpp.hpp>

namespace manyforge::ros2_control_execution {
class Ros2TransportResources;
}

namespace manyforge::roboplan_backend {

/// The `ros_io_bank` topics of one component: `<prefix>/state`
/// (`IoBankState`, subscribed) and `<prefix>/command` (`IoBankCommand`,
/// published), with `<prefix>` = `/<component_id>/io_bank`.
[[nodiscard]] std::string io_bank_topic_prefix(const std::string &component_id);

/// The bank's ROS entities, created and destroyed only through the
/// component's ROS entity owner. The state subscription runs in a
/// time-critical group (its inputs feed interrupt rules); its callback stamps
/// the sample at receipt and hands it on, waiting on nothing. `Send`
/// publishes directly from the writer's thread.
class RosIoBankLink final : public IIoBankLink, public AdapterTransportConsumer {
public:
  using OnState = std::function<void(const IoBankStateSample &)>;

  RosIoBankLink(rclcpp::Node::SharedPtr node, std::string component_id,
                std::shared_ptr<core::IClock> clock, OnState on_state,
                std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner = {});

  [[nodiscard]] bool Send(const IoBankCommandMessage &command) override;
  void Record(const IoBankWriteRecord &record) override;

  std::shared_ptr<void> Stage(const std::shared_ptr<Owner> &expected,
                              const std::shared_ptr<Owner> &replacement, Gate active) override;
  bool Validate(const std::shared_ptr<void> &staged) const override;
  void Commit(const std::shared_ptr<void> &staged) noexcept override;

private:
  struct State;
  struct Rebind;
  std::shared_ptr<State>
  CreateState(rclcpp::Node::SharedPtr node,
              std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner,
              std::shared_ptr<std::atomic<bool>> active);

  const std::string component_id_;
  const std::string prefix_;
  const std::shared_ptr<core::IClock> clock_;
  const OnState on_state_;
  rclcpp::Logger logger_;
  /// Guards the `state_` pointer only (snapshot, as the other links).
  mutable std::mutex state_mutex_;
  std::shared_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend
