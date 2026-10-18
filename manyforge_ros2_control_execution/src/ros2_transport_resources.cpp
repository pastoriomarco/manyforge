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
    std::size_t executor_thread_count) {
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
}

Ros2TransportResources::Ros2TransportResources(
    std::string node_name, std::string node_namespace,
    std::size_t executor_thread_count,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string arm_group_id, core::IClock &clock)
    : Ros2TransportResources(std::move(node_name), std::move(node_namespace),
                             executor_thread_count) {
  scene_projection_worker_ = std::make_unique<detail::SceneProjectionWorker>(
      std::move(execution_state_sink), std::move(arm_group_id), clock);
}

Ros2TransportResources::~Ros2TransportResources() { quiesce(); }

detail::SceneProjectionWorker &
Ros2TransportResources::projection_worker() const {
  if (!scene_projection_worker_)
    throw std::logic_error("ROS transport has no scene projection worker");
  return *scene_projection_worker_;
}

void Ros2TransportResources::start() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (started() || quiesced_ ||
      spin_finished_.load(std::memory_order_acquire))
    throw std::logic_error(
        "ROS transport cannot be started twice or after quiescence");
  spin_thread_ = std::thread([this]() {
    executor_->spin();
    spin_finished_.store(true, std::memory_order_release);
  });
  // spin() establishes its cancellation state internally. Do not let quiesce()
  // cancel before that state exists: spin() would overwrite an early cancel.
  while (!executor_->is_spinning() &&
         !spin_finished_.load(std::memory_order_acquire))
    std::this_thread::yield();
  if (spin_finished_.load(std::memory_order_acquire)) {
    spin_thread_.join();
    throw std::runtime_error("ROS transport executor exited during startup");
  }
  started_.store(true, std::memory_order_release);
}

void Ros2TransportResources::quiesce() noexcept {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (quiesced_)
    return;
  quiesced_ = true;
  // Cancellation stops new executor work; selected SDK callbacks must finish
  // with a valid context before shutdown invalidates their ROS entities.
  executor_->cancel();
  if (spin_thread_.joinable())
    spin_thread_.join();
  executor_->remove_node(node_);
  context_->shutdown("manyforge ROS transport shutdown");
  if (scene_projection_worker_ &&
      !scene_projection_worker_->stop_and_join_for(std::chrono::seconds(1))) {
    RCLCPP_FATAL(node_->get_logger(),
                 "Timed out draining scene projection callbacks during ROS "
                 "transport shutdown");
    std::terminate();
  }
}
} // namespace manyforge::ros2_control_execution
