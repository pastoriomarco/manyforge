// Copyright 2026 Flexin Group SRL
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

#pragma once

#include <atomic>
#include <cstddef>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <string>
#include <thread>

namespace manyforge::ros2_control_execution {
namespace detail {
class SceneProjectionWorker;
}

/// Existing ROS transport resources, retained by the host across both adapters.
/// Construct entities before start(); shutdown adapters, then quiesce(), before
/// destroying either adapter. This holder contains no command or Stop policy.
class Ros2TransportResources final {
public:
  Ros2TransportResources(
      std::string node_name, std::string node_namespace,
      std::size_t executor_thread_count, std::string command_gate_topic,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      std::string arm_group_id, core::IClock &clock);
  ~Ros2TransportResources();
  Ros2TransportResources(const Ros2TransportResources &) = delete;
  Ros2TransportResources &operator=(const Ros2TransportResources &) = delete;

  rclcpp::Node::SharedPtr node() const noexcept { return node_; }
  detail::SceneProjectionWorker &projection_worker() const noexcept;
  bool command_gate_configured() const noexcept {
    return static_cast<bool>(command_gate_pub_);
  }
  bool command_gate_enabled() const noexcept {
    return command_gate_enabled_.load(std::memory_order_acquire);
  }
  bool request_command_gate(bool enabled);
  bool started() const noexcept {
    return started_.load(std::memory_order_acquire);
  }
  void start();
  void quiesce() noexcept;

private:
  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr command_gate_pub_;
  std::unique_ptr<detail::SceneProjectionWorker> scene_projection_worker_;
  std::atomic<bool> command_gate_enabled_{false};
  std::atomic<bool> started_{false};
  std::mutex lifecycle_mutex_;
  bool quiesced_ = false;
  std::thread spin_thread_;
};
} // namespace manyforge::ros2_control_execution
