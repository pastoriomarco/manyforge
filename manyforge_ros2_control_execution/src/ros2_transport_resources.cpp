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

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include "scene_projection_worker.hpp"
#include <chrono>
#include <stdexcept>
#include <utility>

namespace manyforge::ros2_control_execution {
Ros2TransportResources::Ros2TransportResources(
    std::string node_name, std::string node_namespace,
    std::size_t executor_thread_count, std::string command_gate_topic,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string arm_group_id, core::IClock &clock) {
  context_ = std::make_shared<rclcpp::Context>();
  int argc = 0;
  char **argv = nullptr;
  context_->init(argc, argv);
  rclcpp::NodeOptions options;
  options.context(context_);
  node_ = std::make_shared<rclcpp::Node>(std::move(node_name),
                                         std::move(node_namespace), options);
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context_;
  executor_ = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
      executor_options, executor_thread_count);
  executor_->add_node(node_);
  scene_projection_worker_ = std::make_unique<detail::SceneProjectionWorker>(
      std::move(execution_state_sink), std::move(arm_group_id), clock);
  if (!command_gate_topic.empty()) {
    command_gate_pub_ = node_->create_publisher<std_msgs::msg::Bool>(
        command_gate_topic, rclcpp::QoS(1).reliable().transient_local());
    (void)request_command_gate(false);
  }
}

Ros2TransportResources::~Ros2TransportResources() { quiesce(); }

detail::SceneProjectionWorker &
Ros2TransportResources::projection_worker() const noexcept {
  return *scene_projection_worker_;
}

bool Ros2TransportResources::request_command_gate(bool enabled) {
  if (!command_gate_pub_)
    return false;
  command_gate_enabled_.store(enabled, std::memory_order_release);
  std_msgs::msg::Bool msg;
  msg.data = enabled;
  command_gate_pub_->publish(msg);
  return true;
}

void Ros2TransportResources::start() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (started() || quiesced_)
    throw std::logic_error(
        "ROS transport cannot be started twice or after quiescence");
  spin_thread_ = std::thread([this]() { executor_->spin(); });
  started_.store(true, std::memory_order_release);
}

void Ros2TransportResources::quiesce() noexcept {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (quiesced_)
    return;
  quiesced_ = true;
  try {
    (void)request_command_gate(false);
  } catch (...) {
  }
  // Context shutdown is durable even if spin() has not entered its wait yet.
  executor_->cancel();
  context_->shutdown("manyforge ROS transport shutdown");
  if (spin_thread_.joinable())
    spin_thread_.join();
  executor_->remove_node(node_);
  if (!scene_projection_worker_->stop_and_join_for(std::chrono::seconds(1))) {
    RCLCPP_FATAL(node_->get_logger(),
                 "Timed out draining scene projection callbacks during ROS "
                 "transport shutdown");
    std::terminate();
  }
}
} // namespace manyforge::ros2_control_execution
