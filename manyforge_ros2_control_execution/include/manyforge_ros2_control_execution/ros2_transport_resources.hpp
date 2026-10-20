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
#include <functional>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
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
  /// Optional host-owned context, acquired only during construction. The
  /// provider may initialize it on first use; it must return a valid context.
  /// Nodes/executors remain local and quiesce() never shuts a supplied context
  /// down. The provider itself is not retained by this transport holder.
  using ContextProvider = std::function<std::shared_ptr<rclcpp::Context>()>;

  /// Transport-only resources for adapters without a scene projection.
  Ros2TransportResources(std::string node_name, std::string node_namespace,
                         std::size_t executor_thread_count,
                         ContextProvider context_provider = {});
  Ros2TransportResources(
      std::string node_name, std::string node_namespace,
      std::size_t executor_thread_count,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      std::string arm_group_id, core::IClock &clock,
      ContextProvider context_provider = {});
  ~Ros2TransportResources();
  Ros2TransportResources(const Ros2TransportResources &) = delete;
  Ros2TransportResources &operator=(const Ros2TransportResources &) = delete;

  rclcpp::Node::SharedPtr node() const noexcept { return node_; }
  /// Refuses transport-only holders, which own no scene projection worker.
  detail::SceneProjectionWorker &projection_worker() const;
  bool started() const noexcept {
    return started_.load(std::memory_order_acquire);
  }
  void start();
  void quiesce() noexcept;

private:
  friend class JtcExecutionPortTestPeer;
  std::shared_ptr<rclcpp::Context> context_;
  bool owns_private_context_ = true;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  std::unique_ptr<detail::SceneProjectionWorker> scene_projection_worker_;
  std::atomic<bool> started_{false};
  std::atomic<bool> spin_finished_{false};
  std::mutex lifecycle_mutex_;
  bool quiesced_ = false;
  std::thread spin_thread_;
};
} // namespace manyforge::ros2_control_execution
