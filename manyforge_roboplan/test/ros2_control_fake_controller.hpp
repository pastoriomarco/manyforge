#pragma once

// In-process stand-ins for the controller manager and one stock
// joint_trajectory_controller, enough for host tests of the thin JTC adapter:
// FollowJointTrajectory goals (feedback, result, preemption by a replacement,
// cancel with deceleration), `controller_state`, the speed-scaling input, the
// controller manager's `switch_controller` and `set_hardware_component_state`
// services and transient-local `~/activity` topic. Playback interpolates the received points over the
// controller's own cursor (path time advancing at the applied factor). No
// claim is made about the stock controller beyond these message shapes.

#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <control_msgs/msg/joint_trajectory_controller_state.hpp>
#include <control_msgs/msg/speed_scaling_factor.hpp>
#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <controller_manager_msgs/srv/set_hardware_component_state.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::ros2_control_execution::fake_controller {

using FollowJointTrajectory = control_msgs::action::FollowJointTrajectory;
using ServerGoal = rclcpp_action::ServerGoalHandle<FollowJointTrajectory>;

[[nodiscard]] inline std::string unique_suffix() {
  static std::atomic<std::uint64_t> next{1U};
  return std::to_string(next.fetch_add(1U, std::memory_order_relaxed));
}

/// One ROS context, node and spinning executor owned by a test peer.
class PeerNode {
public:
  explicit PeerNode(const std::string &name) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);
    rclcpp::NodeOptions options;
    options.context(context_);
    node_ = std::make_shared<rclcpp::Node>(name + "_" + unique_suffix(), options);
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context_;
    executor_ = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
        executor_options, 2U);
    executor_->add_node(node_);
  }
  PeerNode(const PeerNode &) = delete;
  PeerNode &operator=(const PeerNode &) = delete;
  virtual ~PeerNode() { Stop(); }

  void Start() {
    spin_ = std::thread([this] { executor_->spin(); });
  }
  void Stop() {
    if (stopped_.exchange(true)) return;
    executor_->cancel();
    context_->shutdown("test peer shutdown");
    if (spin_.joinable()) spin_.join();
    executor_->remove_node(node_);
  }
  [[nodiscard]] rclcpp::Node::SharedPtr node() const { return node_; }

private:
  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  std::thread spin_;
  std::atomic<bool> stopped_{false};
};

/// Fake trajectory controller `<controller>` (absolute name).
class FakeJtc final : public PeerNode {
public:
  FakeJtc(std::string controller, std::vector<std::string> joints,
          double rate_hz = 100.0)
      : PeerNode("manyforge_fake_jtc"), controller_(std::move(controller)),
        joints_(std::move(joints)), period_(1.0 / rate_hz),
        position_(joints_.size(), 0.0) {
    state_ = node()->create_publisher<control_msgs::msg::JointTrajectoryControllerState>(
        controller_ + "/controller_state", rclcpp::QoS(10).reliable());
    scale_ = node()->create_subscription<control_msgs::msg::SpeedScalingFactor>(
        controller_ + "/speed_scaling_input", rclcpp::QoS(1).reliable().transient_local(),
        [this](const control_msgs::msg::SpeedScalingFactor::ConstSharedPtr message) {
          std::lock_guard lock(mutex_);
          factor_ = message->factor;
          scale_values_.push_back(message->factor);
        });
    server_ = rclcpp_action::create_server<FollowJointTrajectory>(
        node(), controller_ + "/follow_joint_trajectory",
        [this](const rclcpp_action::GoalUUID &,
               std::shared_ptr<const FollowJointTrajectory::Goal>) {
          std::lock_guard lock(mutex_);
          return active_ ? rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE
                         : rclcpp_action::GoalResponse::REJECT;
        },
        [this](const std::shared_ptr<ServerGoal>) {
          ++cancel_requests_;
          return rclcpp_action::CancelResponse::ACCEPT;
        },
        [this](const std::shared_ptr<ServerGoal> goal) { Accept(goal); });
    Start();
    loop_ = std::thread([this] { Loop(); });
  }

  ~FakeJtc() override {
    running_ = false;
    if (loop_.joinable()) loop_.join();
    Stop();
  }

  /// The controller manager switched this controller.
  void SetActive(bool active) {
    std::shared_ptr<ServerGoal> ended;
    {
      std::lock_guard lock(mutex_);
      active_ = active;
      if (!active && goal_) ended = std::exchange(goal_, nullptr);
      cursor_ = 0.0;
      points_.clear();
      if (ended && hold_result_on_deactivate_) {
        held_ = std::exchange(ended, nullptr);
      }
    }
    if (ended && ended->is_active()) {
      auto result = std::make_shared<FollowJointTrajectory::Result>();
      result->error_code = FollowJointTrajectory::Result::INVALID_GOAL;
      result->error_string = "Controller deactivated";
      ended->abort(result);
    }
  }

  /// Keep the result of a goal ended by deactivation until
  /// `ReleaseHeldResult()` (a late result callback on the client side).
  void HoldResultOnDeactivate(bool hold) {
    std::lock_guard lock(mutex_);
    hold_result_on_deactivate_ = hold;
  }
  /// Deliver the held result now; true when one was held.
  bool ReleaseHeldResult() {
    std::shared_ptr<ServerGoal> held;
    {
      std::lock_guard lock(mutex_);
      held = std::exchange(held_, nullptr);
    }
    if (!held || !held->is_active()) return false;
    auto result = std::make_shared<FollowJointTrajectory::Result>();
    result->error_code = FollowJointTrajectory::Result::INVALID_GOAL;
    result->error_string = "Controller deactivated";
    held->abort(result);
    return true;
  }

  [[nodiscard]] std::size_t goals_received() const { return goals_received_.load(); }
  [[nodiscard]] std::size_t cancel_requests() const { return cancel_requests_.load(); }
  [[nodiscard]] std::vector<double> scale_values() const {
    std::lock_guard lock(mutex_);
    return scale_values_;
  }
  [[nodiscard]] std::vector<trajectory_msgs::msg::JointTrajectory> trajectories() const {
    std::lock_guard lock(mutex_);
    return trajectories_;
  }
  /// Publish samples even when no goal runs (the hold).
  [[nodiscard]] std::uint64_t samples_published() const { return samples_.load(); }

private:
  struct Point {
    double t;
    std::vector<double> positions;
  };

  void Accept(const std::shared_ptr<ServerGoal> &goal) {
    std::shared_ptr<ServerGoal> preempted;
    {
      std::lock_guard lock(mutex_);
      ++goals_received_;
      trajectories_.push_back(goal->get_goal()->trajectory);
      preempted = std::exchange(goal_, goal);
      points_.clear();
      for (const auto &point : goal->get_goal()->trajectory.points) {
        points_.push_back({static_cast<double>(point.time_from_start.sec) +
                               static_cast<double>(point.time_from_start.nanosec) * 1e-9,
                           point.positions});
      }
      cursor_ = 0.0;  // a new trajectory starts at its own time 0
      cancel_steps_ = 0;
    }
    if (preempted && preempted->is_active()) {
      auto result = std::make_shared<FollowJointTrajectory::Result>();
      result->error_code = FollowJointTrajectory::Result::INVALID_GOAL;
      result->error_string = "Current goal preempted by new incoming action.";
      preempted->abort(result);
    }
  }

  [[nodiscard]] std::vector<double> SampleLocked(double t) const {
    if (points_.empty()) return position_;
    if (t <= points_.front().t) return points_.front().positions;
    for (std::size_t index = 1U; index < points_.size(); ++index) {
      if (t <= points_[index].t) {
        const auto &a = points_[index - 1U];
        const auto &b = points_[index];
        const double s = (t - a.t) / std::max(1e-9, b.t - a.t);
        std::vector<double> result(a.positions.size());
        for (std::size_t joint = 0U; joint < result.size(); ++joint) {
          result[joint] = a.positions[joint] + s * (b.positions[joint] - a.positions[joint]);
        }
        return result;
      }
    }
    return points_.back().positions;
  }

  void Loop() {
    while (running_) {
      std::this_thread::sleep_for(std::chrono::duration<double>(period_));
      control_msgs::msg::JointTrajectoryControllerState state;
      std::shared_ptr<ServerGoal> finished;
      std::shared_ptr<ServerGoal> cancelled;
      std::shared_ptr<FollowJointTrajectory::Feedback> feedback;
      std::shared_ptr<ServerGoal> feedback_goal;
      {
        std::lock_guard lock(mutex_);
        if (!active_) continue;
        std::vector<double> velocity(joints_.size(), 0.0);
        if (goal_ && goal_->is_canceling()) {
          // Cancel: switch to a short braking trajectory (cursor restarts),
          // its reference velocity reaching exactly zero.
          if (cancel_steps_ == 0) cursor_ = 0.0;
          ++cancel_steps_;
          cursor_ += period_;
          const double remaining = std::max(0.0, 1.0 - cancel_steps_ / 4.0);
          for (auto &value : velocity) value = 0.1 * remaining;
          if (remaining == 0.0) cancelled = std::exchange(goal_, nullptr);
        } else if (goal_) {
          const auto previous = SampleLocked(cursor_);
          cursor_ += period_ * factor_;
          position_ = SampleLocked(cursor_);
          for (std::size_t joint = 0U; joint < velocity.size(); ++joint) {
            velocity[joint] = (position_[joint] - previous[joint]) / period_;
          }
          feedback = std::make_shared<FollowJointTrajectory::Feedback>();
          feedback_goal = goal_;
          if (!points_.empty() && cursor_ >= points_.back().t) {
            finished = std::exchange(goal_, nullptr);
            std::fill(velocity.begin(), velocity.end(), 0.0);
          }
        } else {
          cursor_ += period_;  // hold
        }
        state.header.stamp = node()->now();
        state.joint_names = joints_;
        state.reference.positions = position_;
        state.reference.velocities = velocity;
        state.reference.accelerations.assign(joints_.size(), 0.0);
        state.reference.time_from_start = rclcpp::Duration::from_seconds(cursor_);
        state.output.positions = position_;
        state.feedback.positions = position_;
        state.feedback.velocities = velocity;
        state.error.positions.assign(joints_.size(), 0.0);
        state.speed_scaling_factor = factor_;
        if (feedback) {
          feedback->header.stamp = state.header.stamp;
          feedback->joint_names = joints_;
          feedback->desired = state.reference;
          feedback->actual.positions = position_;
          feedback->actual.velocities = velocity;
        }
      }
      state_->publish(state);
      ++samples_;
      if (feedback && feedback_goal->is_active()) feedback_goal->publish_feedback(feedback);
      if (finished && finished->is_active()) {
        auto result = std::make_shared<FollowJointTrajectory::Result>();
        result->error_code = FollowJointTrajectory::Result::SUCCESSFUL;
        finished->succeed(result);
      }
      if (cancelled && cancelled->is_canceling()) {
        auto result = std::make_shared<FollowJointTrajectory::Result>();
        result->error_code = FollowJointTrajectory::Result::SUCCESSFUL;
        cancelled->canceled(result);
      }
    }
  }

  std::string controller_;
  std::vector<std::string> joints_;
  double period_;
  mutable std::mutex mutex_;
  bool active_{false};
  double factor_{1.0};
  double cursor_{0.0};
  int cancel_steps_{0};
  std::vector<double> position_;
  std::vector<Point> points_;
  std::shared_ptr<ServerGoal> goal_;
  bool hold_result_on_deactivate_{false};
  std::shared_ptr<ServerGoal> held_;
  std::vector<double> scale_values_;
  std::vector<trajectory_msgs::msg::JointTrajectory> trajectories_;
  std::atomic<std::size_t> goals_received_{0U};
  std::atomic<std::size_t> cancel_requests_{0U};
  std::atomic<std::uint64_t> samples_{0U};
  std::atomic<bool> running_{true};
  rclcpp::Publisher<control_msgs::msg::JointTrajectoryControllerState>::SharedPtr state_;
  rclcpp::Subscription<control_msgs::msg::SpeedScalingFactor>::SharedPtr scale_;
  rclcpp_action::Server<FollowJointTrajectory>::SharedPtr server_;
  std::thread loop_;
};

/// Fake controller manager `<manager>` (absolute name) for one controller on
/// one hardware component. The controller is loaded inactive (the spawner's
/// `--inactive`); `switch_controller` activates or deactivates it.
class FakeControllerManager final : public PeerNode {
public:
  FakeControllerManager(std::string manager, std::string controller,
                        std::string hardware, FakeJtc *jtc,
                        std::string reset_writer = {})
      : PeerNode("manyforge_fake_controller_manager"),
        controller_(std::move(controller)), hardware_(std::move(hardware)),
        jtc_(jtc), reset_writer_(std::move(reset_writer)) {
    activity_ = node()->create_publisher<controller_manager_msgs::msg::ControllerManagerActivity>(
        manager + "/activity", rclcpp::QoS(1).reliable().transient_local());
    switch_ = node()->create_service<controller_manager_msgs::srv::SwitchController>(
        manager + "/switch_controller",
        [this](const std::shared_ptr<controller_manager_msgs::srv::SwitchController::Request> request,
               std::shared_ptr<controller_manager_msgs::srv::SwitchController::Response> response) {
          ++switch_requests_;
          {
            std::lock_guard lock(reset_mutex_);
            switch_history_.emplace_back(request->activate_controllers,
                                         request->deactivate_controllers);
          }
          if (!reset_writer_.empty()) {
            const auto contains_writer = [&](const auto &names) {
              return std::find(names.begin(), names.end(), reset_writer_) !=
                     names.end();
            };
            if (contains_writer(request->activate_controllers))
              writer_active_ = true;
            if (contains_writer(request->deactivate_controllers))
              writer_active_ = false;
          }
          // A listed broadcaster switches like the stock manager's strict
          // request: activating an active one is refused.
          constexpr const char *broadcaster = "joint_state_broadcaster";
          if (std::find(request->activate_controllers.begin(),
                        request->activate_controllers.end(),
                        broadcaster) != request->activate_controllers.end()) {
            if (broadcaster_active_) {
              response->ok = false;
              return;
            }
            broadcaster_listed_ = true;
            broadcaster_active_ = true;
          }
          bool next = active_;
          if (std::find(request->activate_controllers.begin(), request->activate_controllers.end(),
                        controller_) != request->activate_controllers.end()) {
            if (active_) {
              response->ok = false;
              return;
            }
            next = true;
          }
          if (std::find(request->deactivate_controllers.begin(),
                        request->deactivate_controllers.end(),
                        controller_) != request->deactivate_controllers.end()) {
            if (!active_) {
              response->ok = false;
              return;
            }
            next = false;
          }
          SetControllerActive(next);
          {
            std::unique_lock lock(reply_mutex_);
            reply_waiting_ = replies_paused_;
            reply_changed_.wait(lock, [&] { return !replies_paused_; });
            reply_waiting_ = false;
          }
          response->ok = true;
        });
    // R7 package 4: a Real arm's hardware is (de)activated by name.
    hardware_state_ = node()->create_service<
        controller_manager_msgs::srv::SetHardwareComponentState>(
        manager + "/set_hardware_component_state",
        [this](const std::shared_ptr<
                   controller_manager_msgs::srv::SetHardwareComponentState::Request>
                   request,
               std::shared_ptr<
                   controller_manager_msgs::srv::SetHardwareComponentState::Response>
                   response) {
          const bool active =
              request->target_state.id ==
              lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
          {
            std::lock_guard lock(reset_mutex_);
            hardware_history_.push_back(active);
          }
          response->ok = request->name == hardware_ && !hardware_refused_;
          if (response->ok) SetHardwareActive(active);
          std::function<void()> hook;
          {
            std::lock_guard lock(reset_mutex_);
            hook = hardware_activate_hook_;
          }
          // Before the answer, like the vendor plugin's on_activate/write().
          if (response->ok && active && hook) hook();
        });
    if (!reset_writer_.empty()) {
      const auto manager_suffix = manager.rfind('/');
      reset_subscription_ =
          node()->create_subscription<std_msgs::msg::Float64MultiArray>(
              manager.substr(0U, manager_suffix) + "/" + reset_writer_ +
                  "/commands",
              rclcpp::QoS(1).reliable(),
              [this](std_msgs::msg::Float64MultiArray::ConstSharedPtr command) {
                std::lock_guard lock(reset_mutex_);
                reset_commands_.push_back(command->data);
              });
    }
    Start();
    Publish();
  }

  ~FakeControllerManager() override {
    SetSwitchRepliesPaused(false);
    Stop();
  }

  void SetSwitchRepliesPaused(bool paused) {
    {
      std::lock_guard lock(reply_mutex_);
      replies_paused_ = paused;
    }
    reply_changed_.notify_all();
  }
  [[nodiscard]] bool reply_waiting() const { return reply_waiting_.load(); }

  /// End the peer executor before destroying its actual ROS endpoints.
  /// Context shutdown alone leaves these shared endpoint owners alive.
  void StopAndDestroyEndpoints() {
    PeerNode::Stop();
    switch_.reset();
    hardware_state_.reset();
    activity_.reset();
  }

  /// Switch outside CCLC (an unrequested deactivation or restart).
  void SetControllerActive(bool active) {
    active_ = active;
    if (jtc_ != nullptr) jtc_->SetActive(active);
    Publish();
  }

  /// Publish a real transitional controller-manager activity sample.
  void SetControllerActivity(bool controller_active, bool writer_active) {
    active_ = controller_active;
    writer_active_ = writer_active;
    if (jtc_ != nullptr) jtc_->SetActive(controller_active);
    Publish();
  }

  void SetHardwareActive(bool active) {
    hardware_active_ = active;
    Publish();
  }

  /// R7 package 4: list the joint state broadcaster in the activity (absent
  /// unless set, as for the Mock tests).
  void SetBroadcasterActive(bool active) {
    broadcaster_listed_ = true;
    broadcaster_active_ = active;
    Publish();
  }

  /// The plugin's deactivate-all / reactivate-all in one activity sample.
  void SetAllControllersActive(bool active) {
    broadcaster_listed_ = true;
    broadcaster_active_ = active;
    SetControllerActive(active);
  }

  void SetHardwareRefused(bool refused) { hardware_refused_ = refused; }

  /// Run on every successful hardware activation, before its answer (the
  /// vendor plugin's first write() cycles after `on_activate`).
  void SetHardwareActivateHook(std::function<void()> hook) {
    std::lock_guard lock(reset_mutex_);
    hardware_activate_hook_ = std::move(hook);
  }

  /// Every `set_hardware_component_state` request, `true` for active.
  [[nodiscard]] std::vector<bool> hardware_history() const {
    std::lock_guard lock(reset_mutex_);
    return hardware_history_;
  }

  /// Deterministically hold the old transient-local activity while a switch
  /// can still return its positive service ACK; releasing publishes truth.
  void SetActivityPublicationEnabled(bool enabled) {
    activity_publication_enabled_ = enabled;
    if (enabled)
      Publish();
  }

  [[nodiscard]] std::size_t switch_requests() const { return switch_requests_.load(); }
  [[nodiscard]] bool writer_active() const { return writer_active_.load(); }
  [[nodiscard]] std::vector<std::vector<double>> reset_commands() const {
    std::lock_guard lock(reset_mutex_);
    return reset_commands_;
  }
  [[nodiscard]] auto switch_history() const {
    std::lock_guard lock(reset_mutex_);
    return switch_history_;
  }

private:
  void Publish() {
    if (!activity_publication_enabled_)
      return;
    controller_manager_msgs::msg::ControllerManagerActivity message;
    message.header.stamp = node()->now();
    controller_manager_msgs::msg::NamedLifecycleState controller;
    controller.name = controller_;
    controller.state.id = active_ ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
                                  : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
    message.controllers.push_back(controller);
    if (!reset_writer_.empty()) {
      controller.name = reset_writer_;
      controller.state.id =
          writer_active_ ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
                         : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
      message.controllers.push_back(controller);
    }
    if (broadcaster_listed_) {
      controller.name = "joint_state_broadcaster";
      controller.state.id =
          broadcaster_active_ ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
                              : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
      message.controllers.push_back(controller);
    }
    controller_manager_msgs::msg::NamedLifecycleState hardware;
    hardware.name = hardware_;
    hardware.state.id = hardware_active_ ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
                                         : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
    message.hardware_components.push_back(hardware);
    activity_->publish(message);
  }

  std::string controller_;
  std::string hardware_;
  FakeJtc *jtc_;
  std::mutex reply_mutex_;
  std::condition_variable reply_changed_;
  bool replies_paused_{false};
  std::atomic<bool> reply_waiting_{false};
  std::string reset_writer_;
  std::atomic<bool> writer_active_{false};
  mutable std::mutex reset_mutex_;
  std::vector<std::vector<double>> reset_commands_;
  std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>
      switch_history_;
  rclcpp::Subscription<std_msgs::msg::Float64MultiArray>::SharedPtr
      reset_subscription_;
  std::atomic<bool> active_{false};
  std::atomic<bool> activity_publication_enabled_{true};
  std::atomic<bool> hardware_active_{true};
  std::atomic<bool> broadcaster_listed_{false};
  std::atomic<bool> broadcaster_active_{false};
  std::atomic<bool> hardware_refused_{false};
  std::vector<bool> hardware_history_;
  std::function<void()> hardware_activate_hook_;
  std::atomic<std::size_t> switch_requests_{0U};
  rclcpp::Publisher<controller_manager_msgs::msg::ControllerManagerActivity>::SharedPtr activity_;
  rclcpp::Service<controller_manager_msgs::srv::SwitchController>::SharedPtr switch_;
  rclcpp::Service<controller_manager_msgs::srv::SetHardwareComponentState>::SharedPtr
      hardware_state_;
};

template <typename Predicate>
[[nodiscard]] bool WaitUntil(Predicate predicate,
                             std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

}  // namespace manyforge::ros2_control_execution::fake_controller
