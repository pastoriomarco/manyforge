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
#include "../src/scene_projection_worker.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace manyforge::ros2_control_execution {
namespace {
using namespace std::chrono_literals;
class RejectingScheduler final : public core::IScheduler {
public:
  core::ScheduleHandle schedule(core::TimePoint, std::function<void()>,
                                core::SchedulerPriority) override {
    throw std::runtime_error("injected schedule failure");
  }

  void cancel(core::ScheduleHandle) override {}
};

class QueuedCallbackScheduler final : public core::IScheduler {
public:
  core::ScheduleHandle schedule(core::TimePoint, std::function<void()> callback,
                                core::SchedulerPriority priority) override {
    EXPECT_EQ(priority, core::SchedulerPriority::ControllerCommand);
    callbacks.push_back(std::move(callback));
    return next_handle_++;
  }
  void cancel(core::ScheduleHandle) override {
    // Deliberately retain already-queued work: lifecycle fences, rather than
    // successful eager cancellation, must make these callbacks inert.
  }
  std::vector<std::function<void()>> take_callbacks() {
    return std::exchange(callbacks, {});
  }

private:
  core::ScheduleHandle next_handle_ = 1U;
  std::vector<std::function<void()>> callbacks;
};

std::string unique_suffix() {
  static std::atomic<int> counter{0};
  return std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

core::GripperCommandRequestDTO
make_gripper_request(double target_position, double duration_s,
                     double max_effort, std::string transport_kind,
                     std::string completion_policy) {
  core::GripperCommandRequestDTO request;
  request.gripper_id = "tool";
  request.group_id = "gripper";
  request.joint_name = "finger_joint";
  request.target_position = target_position;
  request.duration_s = duration_s;
  request.max_effort = max_effort;
  request.transport_kind = std::move(transport_kind);
  request.completion_policy = std::move(completion_policy);
  request.state_authority = "sim_observed";
  return request;
}

Ros2GripperActionConfig isaac_gripper_config(std::string command_topic) {
  Ros2GripperActionConfig config;
  config.transport_kind = "isaac_joint_state_effort";
  config.command_topic = std::move(command_topic);
  config.command_joint_names = {"finger_joint"};
  config.group_id = "gripper";
  config.joint_name = "finger_joint";
  config.completion_policy = "dwell_elapsed";
  config.open_position = 0.7;
  config.closed_position = 0.0;
  config.open_effort = 5.0;
  config.close_effort = -5.0;
  config.publish_nominal_position = true;
  return config;
}

struct SingleJointRead {
  std::string group_id;
  std::string joint_name;
};

struct SingleJointUpdate {
  std::string group_id;
  std::string joint_name;
  double position_rad = 0.0;
  core::ExecutionStateUpdateMode mode =
      core::ExecutionStateUpdateMode::kBlocking;
};

struct GroupUpdate {
  std::string group_id;
  std::vector<double> positions_rad;
  core::ExecutionStateUpdateMode mode =
      core::ExecutionStateUpdateMode::kBlocking;
};

class RecordingExecutionStateSink final : public core::IExecutionStateSink {
public:
  core::ExecutionStateSinkResult
  apply_group_joint_positions(const std::string &group_id,
                              const std::vector<double> &positions_rad,
                              core::ExecutionStateUpdateMode mode) override {
    std::unique_lock<std::mutex> lock(mutex_);
    group_updates_.push_back({group_id, positions_rad, mode});
    if (block_group_update_) {
      group_update_entered_ = true;
      group_update_cv_.notify_all();
      group_update_cv_.wait_for(lock, 2s,
                                [this]() { return release_group_update_; });
    }
    if (fail_group_updates_remaining_ > 0) {
      --fail_group_updates_remaining_;
      return {fail_group_update_status_};
    }
    return {group_status_};
  }

  std::optional<double>
  group_joint_position(const std::string &group_id,
                       const std::string &joint_name) const override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (block_read_) {
      read_entered_ = true;
      read_cv_.notify_all();
      read_cv_.wait_for(lock, 2s, [this]() { return release_read_; });
    }
    reads_.push_back({group_id, joint_name});
    const auto key = std::make_pair(group_id, joint_name);
    const auto it = joint_positions_.find(key);
    if (it == joint_positions_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  core::ExecutionStateSinkResult
  apply_group_joint_position(const std::string &group_id,
                             const std::string &joint_name, double position_rad,
                             core::ExecutionStateUpdateMode mode) override {
    std::unique_lock<std::mutex> lock(mutex_);
    single_updates_.push_back({group_id, joint_name, position_rad, mode});
    if (block_single_update_) {
      single_update_entered_ = true;
      single_update_cv_.notify_all();
      single_update_cv_.wait_for(lock, 2s,
                                 [this]() { return release_single_update_; });
    }
    if (fail_single_updates_remaining_ > 0) {
      --fail_single_updates_remaining_;
      return {fail_single_update_status_};
    }
    if (single_update_status_ != core::ExecutionStateSinkStatus::kApplied) {
      return {single_update_status_};
    }
    joint_positions_[{group_id, joint_name}] = position_rad;
    return {core::ExecutionStateSinkStatus::kApplied};
  }

  void set_joint_position(std::string group_id, std::string joint_name,
                          double position_rad) {
    std::lock_guard<std::mutex> lock(mutex_);
    joint_positions_[{std::move(group_id), std::move(joint_name)}] =
        position_rad;
  }

  std::vector<SingleJointRead> reads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reads_;
  }

  std::vector<SingleJointUpdate> single_updates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return single_updates_;
  }

  std::vector<GroupUpdate> group_updates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return group_updates_;
  }

  void fail_next_group_updates(std::size_t count,
                               core::ExecutionStateSinkStatus status =
                                   core::ExecutionStateSinkStatus::kBusy) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_group_updates_remaining_ = count;
    fail_group_update_status_ = status;
  }

  void fail_next_single_updates(std::size_t count,
                                core::ExecutionStateSinkStatus status =
                                    core::ExecutionStateSinkStatus::kBusy) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_single_updates_remaining_ = count;
    fail_single_update_status_ = status;
  }

  void block_single_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_single_update_ = true;
    single_update_entered_ = false;
    release_single_update_ = false;
  }

  void block_group_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_group_update_ = true;
    group_update_entered_ = false;
    release_group_update_ = false;
  }

  void block_read() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_read_ = true;
    read_entered_ = false;
    release_read_ = false;
  }

  bool wait_for_read() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return read_cv_.wait_for(lock, 2s, [this]() { return read_entered_; });
  }

  void release_read() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_read_ = true;
      block_read_ = false;
    }
    read_cv_.notify_all();
  }

  bool wait_for_single_update() {
    std::unique_lock<std::mutex> lock(mutex_);
    return single_update_cv_.wait_for(
        lock, 2s, [this]() { return single_update_entered_; });
  }

  bool wait_for_group_update() {
    std::unique_lock<std::mutex> lock(mutex_);
    return group_update_cv_.wait_for(
        lock, 2s, [this]() { return group_update_entered_; });
  }

  void release_single_update() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_single_update_ = true;
      block_single_update_ = false;
    }
    single_update_cv_.notify_all();
  }

  void release_group_update() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_group_update_ = true;
      block_group_update_ = false;
    }
    group_update_cv_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, double> joint_positions_;
  mutable std::vector<SingleJointRead> reads_;
  std::vector<SingleJointUpdate> single_updates_;
  std::vector<GroupUpdate> group_updates_;
  std::size_t fail_group_updates_remaining_ = 0;
  core::ExecutionStateSinkStatus fail_group_update_status_ =
      core::ExecutionStateSinkStatus::kBusy;
  core::ExecutionStateSinkStatus group_status_ =
      core::ExecutionStateSinkStatus::kApplied;
  core::ExecutionStateSinkStatus single_update_status_ =
      core::ExecutionStateSinkStatus::kApplied;
  std::size_t fail_single_updates_remaining_ = 0;
  core::ExecutionStateSinkStatus fail_single_update_status_ =
      core::ExecutionStateSinkStatus::kBusy;
  std::condition_variable single_update_cv_;
  bool block_single_update_ = false;
  bool single_update_entered_ = false;
  bool release_single_update_ = false;
  std::condition_variable group_update_cv_;
  bool block_group_update_ = false;
  bool group_update_entered_ = false;
  bool release_group_update_ = false;
  mutable std::condition_variable read_cv_;
  mutable bool block_read_ = false;
  mutable bool read_entered_ = false;
  mutable bool release_read_ = false;
};

class JointStatePublisher {
public:
  explicit JointStatePublisher(const std::string &topic) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);

    rclcpp::NodeOptions options;
    options.context(context_);
    node_ = std::make_shared<rclcpp::Node>(
        "manyforge_joint_state_pub_test_" + unique_suffix(), options);
    publisher_ = node_->create_publisher<sensor_msgs::msg::JointState>(
        topic, rclcpp::SystemDefaultsQoS());
  }

  ~JointStatePublisher() {
    // Stop the isolated ROS context before destroying its DDS endpoints so
    // transport callbacks cannot retain payloads across publisher teardown.
    if (context_) {
      context_->shutdown("test shutdown");
    }
    publisher_.reset();
    node_.reset();
    context_.reset();
  }

  bool wait_for_subscription() const {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (publisher_->get_subscription_count() > 0) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return publisher_->get_subscription_count() > 0;
  }

  void publish(const std::vector<std::string> &names,
               const std::vector<double> &positions) const {
    sensor_msgs::msg::JointState msg;
    msg.name = names;
    msg.position = positions;
    publisher_->publish(msg);
  }

private:
  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr publisher_;
};

class JointStateSubscriber {
public:
  explicit JointStateSubscriber(const std::string &topic) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);

    rclcpp::NodeOptions options;
    options.context(context_);
    node_ = std::make_shared<rclcpp::Node>(
        "manyforge_joint_state_sub_test_" + unique_suffix(), options);
    subscription_ = node_->create_subscription<sensor_msgs::msg::JointState>(
        topic, rclcpp::SystemDefaultsQoS(),
        [](const sensor_msgs::msg::JointState::SharedPtr) {});
  }

  ~JointStateSubscriber() {
    if (context_) {
      context_->shutdown("test shutdown");
    }
    subscription_.reset();
    node_.reset();
    context_.reset();
  }

  bool wait_for_publisher() const {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (subscription_->get_publisher_count() > 0) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return subscription_->get_publisher_count() > 0;
  }

private:
  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr subscription_;
};

template <typename Adapter> class TestTransportLifetime final {
public:
  TestTransportLifetime(Ros2TransportResources &resources, Adapter &adapter)
      : resources_(resources), adapter_(adapter) {}
  ~TestTransportLifetime() { close(); }
  void close() {
    if (closed_)
      return;
    closed_ = true;
    adapter_.shutdown();
    resources_.quiesce();
  }
  void start() {
    if (!resources_.started())
      resources_.start();
  }

private:
  Ros2TransportResources &resources_;
  Adapter &adapter_;
  bool closed_ = false;
};

template <typename FirstAdapter, typename SecondAdapter>
class TestSharedTransportLifetime final {
public:
  TestSharedTransportLifetime(Ros2TransportResources &resources,
                              FirstAdapter &first, SecondAdapter &second)
      : resources_(resources), first_(first), second_(second) {}
  ~TestSharedTransportLifetime() {
    first_.shutdown();
    second_.shutdown();
    resources_.quiesce();
  }

private:
  Ros2TransportResources &resources_;
  FirstAdapter &first_;
  SecondAdapter &second_;
};

std::unique_ptr<Ros2TransportResources> test_transport_resources(
    std::shared_ptr<core::IExecutionStateSink> sink, core::IClock& clock,
    const std::string& group) {
  // Transport host setup is independent of adapter config validation tests.
  return std::make_unique<Ros2TransportResources>("manyforge_transport_test_" + unique_suffix(),
                                                  "/", 3, std::move(sink), group, clock);
}
} // namespace
} // namespace manyforge::ros2_control_execution
