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

#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>

#include <mutex>
#include <stdexcept>
#include <utility>

#include <rclcpp/qos.hpp>

namespace manyforge::xarm_control {

struct XArmRobotStatesTransport::Cache final {
  explicit Cache(core::IClock &clock_in) : clock(clock_in) {}

  void Store(const xarm_msgs::msg::RobotMsg &message) {
    XArmObservedState report;
    report.received_timestamp_ns = clock.now();
    report.state = message.state;
    report.mode = message.mode;
    report.error_code = message.err;
    report.warning_code = message.warn;
    report.motion_enable_mask = message.mt_able;
    // Only for the Enable gate's agreement check (R7 §3.6), never a state.
    report.joint_angles_rad.assign(message.angle.begin(), message.angle.end());
    // Item 44: compared with the TCP offset before each motion.
    report.tcp_offset.assign(message.offset.begin(), message.offset.end());
    std::lock_guard lock(mutex);
    latest = report;
  }

  core::IClock &clock;
  mutable std::mutex mutex;
  XArmObservedState latest;
};

std::string xarm_robot_states_topic(const std::string_view driver_namespace) {
  if (driver_namespace.empty() || driver_namespace.front() != '/' ||
      driver_namespace.back() == '/') {
    throw std::invalid_argument(
        "the xArm driver namespace must be one absolute ROS namespace");
  }
  return std::string(driver_namespace) + "/robot_states";
}

XArmRobotStatesTransport::XArmRobotStatesTransport(
    rclcpp::Node &node, const std::string_view driver_namespace,
    core::IClock &clock)
    : topic_(xarm_robot_states_topic(driver_namespace)),
      cache_(std::make_shared<Cache>(clock)) {
  // The driver publishes reliably at 5 Hz; only the latest report matters.
  subscription_ = node.create_subscription<xarm_msgs::msg::RobotMsg>(
      topic_, rclcpp::SensorDataQoS().keep_last(1),
      [cache = cache_](const xarm_msgs::msg::RobotMsg::ConstSharedPtr message) {
        cache->Store(*message);
      });
}

XArmObservedState XArmRobotStatesTransport::observe() const {
  std::lock_guard lock(cache_->mutex);
  return cache_->latest;
}

void XArmRobotStatesTransport::Receive(const xarm_msgs::msg::RobotMsg &message) {
  cache_->Store(message);
}

} // namespace manyforge::xarm_control
