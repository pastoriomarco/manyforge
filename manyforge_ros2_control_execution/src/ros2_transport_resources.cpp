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

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include "scene_projection_worker.hpp"
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <chrono>
#include <cstdio>
#include <exception>
#include <rcpputils/scope_exit.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::ros2_control_execution {
namespace {
/// rclcpp's MultiThreadedExecutor::spin() (Lyrical 32.0.x, as Jazzy) has no
/// catch: a throw on a helper thread escapes its std::thread body, and a throw
/// on the spinning thread unwinds past the still-joinable helper threads; both
/// call std::terminate. This executor keeps the same thread count and the
/// stock run() loop (callback groups, wait and dispatch unchanged) but owns
/// its threads, so each one catches and reports its own failure.
class ContainedMultiThreadedExecutor final
    : public rclcpp::executors::MultiThreadedExecutor {
public:
  using FaultSink = std::function<void(const char *)>;
  ContainedMultiThreadedExecutor(const rclcpp::ExecutorOptions &options,
                                 std::size_t thread_count, FaultSink fault)
      : MultiThreadedExecutor(options, thread_count), fault_(std::move(fault)) {}

  // Mirrors MultiThreadedExecutor::spin(), with run_contained() in place of
  // run(). One difference: stock spin() starts its threads while holding the
  // private wait_mutex_, which a subclass cannot reach. That is harmless:
  // run() takes wait_mutex_ itself on every iteration.
  void spin() override {
    if (spinning.exchange(true))
      throw std::runtime_error("spin() called while already spinning");
    RCPPUTILS_SCOPE_EXIT(wait_result_.reset(); this->spinning.store(false););
    const std::size_t thread_count = get_number_of_threads();
    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1U);
    for (std::size_t id = 0U; id + 1U < thread_count; ++id)
      threads.emplace_back([this, id] { run_contained(id); });
    run_contained(thread_count - 1U);
    for (auto &thread : threads)
      thread.join();
  }

private:
  void run_contained(const std::size_t thread_id) noexcept {
    try {
      run(thread_id);
    } catch (const std::exception &error) {
      fault_(error.what());
    } catch (...) {
      fault_("unknown_non_std_exception");
    }
  }

  FaultSink fault_;
};
} // namespace

Ros2TransportResources::Ros2TransportResources(
    std::string node_name, std::string node_namespace,
    std::size_t executor_thread_count, ContextProvider context_provider)
    : owns_private_context_(!context_provider) {
  if (context_provider) {
    context_ = context_provider();
    if (!context_ || !context_->is_valid())
      throw std::invalid_argument("ROS transport requires a valid supplied context");
  } else {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);
  }
  rclcpp::NodeOptions options;
  options.context(context_);
  node_ = std::make_shared<rclcpp::Node>(std::move(node_name),
                                         std::move(node_namespace), options);
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context_;
  executor_ = std::make_shared<ContainedMultiThreadedExecutor>(
      executor_options, executor_thread_count,
      [this](const char *exception_text) { record_executor_fault(exception_text); });
  executor_->add_node(node_);
}

Ros2TransportResources::Ros2TransportResources(
    std::string node_name, std::string node_namespace,
    std::size_t executor_thread_count,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string arm_group_id, core::IClock &clock,
    ContextProvider context_provider)
    : Ros2TransportResources(std::move(node_name), std::move(node_namespace),
                             executor_thread_count, std::move(context_provider)) {
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
    // Every executor thread, this one included, catches its own failure and
    // records a typed component fault (read by the runtime handle's health)
    // instead of terminating the process.
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

void Ros2TransportResources::set_executor_fault_observer(
    ExecutorFaultObserver observer) {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (started() || quiesced_)
    throw std::logic_error(
        "ROS transport fault observer must be set before start");
  executor_fault_observer_ = std::move(observer);
}

std::optional<std::string> Ros2TransportResources::executor_fault() const {
  std::lock_guard<std::mutex> lock(executor_fault_mutex_);
  return executor_fault_;
}

void Ros2TransportResources::record_executor_fault(
    const char *exception_text) noexcept {
  (void)std::fprintf(stderr,
                     "manyforge_fault code=ros_transport_executor_failed "
                     "exception=%s\n",
                     exception_text);
  {
    std::lock_guard<std::mutex> lock(executor_fault_mutex_);
    if (executor_fault_)
      return;
    executor_fault_ = exception_text;
  }
  if (executor_fault_observer_)
    executor_fault_observer_("ros_transport_executor_failed");
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
  // Standalone transports preserve their private-context shutdown. A supplied
  // host context also serves siblings and retained node/readers; its actual
  // last owner determines final shutdown after local callback drains.
  if (owns_private_context_)
    context_->shutdown("manyforge ROS transport shutdown");
  if (scene_projection_worker_)
    core::WaitForDrain("ros_transport.scene_projection_worker",
                       std::chrono::seconds(1),
                       [this](std::chrono::nanoseconds bound) {
                         return scene_projection_worker_->stop_and_join_for(
                             bound);
                       });
}
} // namespace manyforge::ros2_control_execution
