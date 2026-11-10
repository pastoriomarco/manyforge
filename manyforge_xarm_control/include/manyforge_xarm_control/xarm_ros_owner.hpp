// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <memory>
#include <atomic>

#include <rclcpp/node.hpp>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::xarm_control {

/// Narrow access to the host's ROS entity owner. The callbacks are used only
/// during construction; the returned handle retires its contents on the owner.
struct XArmRosOwner final {
  rclcpp::Node::SharedPtr node;
  std::shared_ptr<core::IClock> clock;
  std::function<void(std::function<void()>)> invoke;
  std::function<std::shared_ptr<void>(std::shared_ptr<void>)> retain;
  /// OD-66: a new group of the owner's time-critical thread (null: the
  /// owner's default group), and the retain that removes it from that thread
  /// before the owner's final release. One group per retained entity.
  std::function<rclcpp::CallbackGroup::SharedPtr()> time_critical_group;
  std::function<std::shared_ptr<void>(std::shared_ptr<void>, rclcpp::CallbackGroup::SharedPtr)>
      retain_time_critical;
  std::shared_ptr<std::atomic<bool>> active;
};

} // namespace manyforge::xarm_control
