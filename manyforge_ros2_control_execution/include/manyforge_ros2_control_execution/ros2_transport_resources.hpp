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

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <string_view>
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
  /// Called once, with `ros_transport_executor_failed`, when the first
  /// executor thread fails. Set before start(); used by a transport that has
  /// no health reader of its own (the host's generic I/O transport).
  using ExecutorFaultObserver = std::function<void(std::string_view reason_code)>;
  void set_executor_fault_observer(ExecutorFaultObserver observer);
  void start();
  void quiesce() noexcept;
  /// The exception text of the first executor thread that failed (a callback
  /// throw or an rclcpp/rcl failure). That thread stops; the remaining
  /// executor threads keep serving callbacks. Empty while healthy.
  std::optional<std::string> executor_fault() const;

private:
  void record_executor_fault(const char *exception_text) noexcept;

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
  mutable std::mutex executor_fault_mutex_;
  std::optional<std::string> executor_fault_;
  ExecutorFaultObserver executor_fault_observer_;
  std::thread spin_thread_;
};
} // namespace manyforge::ros2_control_execution
