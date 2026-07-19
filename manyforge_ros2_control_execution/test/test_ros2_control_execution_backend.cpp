#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#define MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#undef MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING

#include "../src/scene_projection_worker.hpp"

#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
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
using FollowJointTrajectory = control_msgs::action::FollowJointTrajectory;
using ServerGoalHandleFJT =
    rclcpp_action::ServerGoalHandle<FollowJointTrajectory>;

class RejectingScheduler final : public core::IScheduler {
 public:
  core::ScheduleHandle schedule(
      core::TimePoint, std::function<void()>,
      core::SchedulerPriority) override {
    throw std::runtime_error("injected schedule failure");
  }

  void cancel(core::ScheduleHandle) override {}
};

std::string unique_suffix() {
  static std::atomic<int> counter{0};
  return std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

Ros2ControlExecutionBackendConfig base_config() {
  const std::string suffix = unique_suffix();
  Ros2ControlExecutionBackendConfig config;
  config.node_name = "manyforge_backend_test_" + suffix;
  config.action_name = "/manyforge_backend_test_fjt_" + suffix;
  config.joint_state_topic = "/manyforge_backend_test_joint_states_" + suffix;
  config.controller_state_topic.clear();
  config.required_joints = {"joint_1"};
  config.wait_for_state_timeout_s = 0.0;
  config.wait_for_action_server_timeout_s = 0.0;
  config.monitor_period_s = 0.01;
  config.enable_jtc_speed_scaling_input = false;
  return config;
}

core::TrajectoryDTO make_single_joint_trajectory(double start, double finish) {
  core::TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1"};
  core::TrajectoryWaypointDTO first;
  first.time_from_start_s = 0.0;
  first.positions_rad = {start};
  first.velocities_rad_s = {0.0};
  trajectory.waypoints.push_back(std::move(first));
  core::TrajectoryWaypointDTO last;
  last.time_from_start_s = 0.1;
  last.positions_rad = {finish};
  last.velocities_rad_s = {0.0};
  trajectory.waypoints.push_back(std::move(last));
  return trajectory;
}

core::GripperCommandRequestDTO make_gripper_request(
    double target_position,
    double duration_s,
    double max_effort,
    std::string transport_kind,
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
  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string& group_id,
      const std::vector<double>& positions_rad,
      core::ExecutionStateUpdateMode mode) override {
    std::unique_lock<std::mutex> lock(mutex_);
    group_updates_.push_back({group_id, positions_rad, mode});
    if (block_group_update_) {
      group_update_entered_ = true;
      group_update_cv_.notify_all();
      group_update_cv_.wait_for(lock, 2s, [this]() {
        return release_group_update_;
      });
    }
    if (fail_group_updates_remaining_ > 0) {
      --fail_group_updates_remaining_;
      return {fail_group_update_status_};
    }
    return {group_status_};
  }

  std::optional<double> group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const override {
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

  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name,
      double position_rad,
      core::ExecutionStateUpdateMode mode) override {
    std::unique_lock<std::mutex> lock(mutex_);
    single_updates_.push_back({group_id, joint_name, position_rad, mode});
    if (block_single_update_) {
      single_update_entered_ = true;
      single_update_cv_.notify_all();
      single_update_cv_.wait_for(lock, 2s, [this]() {
        return release_single_update_;
      });
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

  void set_joint_position(std::string group_id,
                          std::string joint_name,
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

  void fail_next_group_updates(
      std::size_t count,
      core::ExecutionStateSinkStatus status =
          core::ExecutionStateSinkStatus::kBusy) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_group_updates_remaining_ = count;
    fail_group_update_status_ = status;
  }

  void fail_next_single_updates(
      std::size_t count,
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
  explicit JointStatePublisher(const std::string& topic) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char** argv = nullptr;
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

  void publish(const std::vector<std::string>& names,
               const std::vector<double>& positions) const {
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

class FakeFjtServer {
 public:
  explicit FakeFjtServer(const std::string& action_name) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char** argv = nullptr;
    context_->init(argc, argv);

    rclcpp::NodeOptions options;
    options.context(context_);
    node_ = std::make_shared<rclcpp::Node>(
        "manyforge_fjt_server_test_" + unique_suffix(), options);
    server_ = rclcpp_action::create_server<FollowJointTrajectory>(
        node_,
        action_name,
        [](const rclcpp_action::GoalUUID&,
           std::shared_ptr<const FollowJointTrajectory::Goal>) {
          return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        },
        [](const std::shared_ptr<ServerGoalHandleFJT>) {
          return rclcpp_action::CancelResponse::ACCEPT;
        },
        [this](const std::shared_ptr<ServerGoalHandleFJT> goal_handle) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            goal_handle_ = goal_handle;
          }
          cv_.notify_all();
        });

    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context_;
    executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>(
        executor_options);
    executor_->add_node(node_);
    spin_thread_ = std::thread([this]() { executor_->spin(); });
  }

  ~FakeFjtServer() {
    server_.reset();
    if (executor_) {
      executor_->cancel();
    }
    if (spin_thread_.joinable()) {
      spin_thread_.join();
    }
    if (executor_ && node_) {
      executor_->remove_node(node_);
    }
    node_.reset();
    executor_.reset();
    if (context_) {
      context_->shutdown("test shutdown");
      context_.reset();
    }
  }

  bool wait_for_goal() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 2s,
                        [this]() { return goal_handle_ != nullptr; });
  }

  bool succeed_current_goal() {
    std::shared_ptr<ServerGoalHandleFJT> goal_handle;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal_handle = goal_handle_;
    }
    if (!goal_handle) {
      return false;
    }
    auto result = std::make_shared<FollowJointTrajectory::Result>();
    result->error_code = FollowJointTrajectory::Result::SUCCESSFUL;
    goal_handle->succeed(result);
    return true;
  }

  bool publish_feedback(double actual_position) {
    std::shared_ptr<ServerGoalHandleFJT> goal_handle;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal_handle = goal_handle_;
    }
    if (!goal_handle) {
      return false;
    }
    auto feedback = std::make_shared<FollowJointTrajectory::Feedback>();
    feedback->joint_names = {"joint_1"};
    feedback->desired.positions = {actual_position};
    feedback->actual.positions = {actual_position};
    feedback->error.positions = {0.0};
    goal_handle->publish_feedback(feedback);
    return true;
  }

 private:
  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  rclcpp_action::Server<FollowJointTrajectory>::SharedPtr server_;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::thread spin_thread_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::shared_ptr<ServerGoalHandleFJT> goal_handle_;
};

TEST(Ros2ControlExecutionBackendTest, RejectsNullExecutionStateSink) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  EXPECT_THROW(
      Ros2ControlExecutionBackend(
          nullptr, "arm", clock, scheduler, base_config()),
      std::invalid_argument);
}

TEST(Ros2ControlExecutionBackendTest,
     RejectsExecutorThreadCountBelowCallbackGroupMinimum) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.executor_thread_count = 2;

  EXPECT_THROW(
      Ros2ControlExecutionBackend(
          Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
          sink,
          "arm",
          clock,
          scheduler,
          config),
      std::invalid_argument);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperProjectionReadsStartPositionFromExecutionStateSink) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());

  Ros2GripperActionConfig gripper;
  gripper.action_name = "/manyforge_missing_gripper_" + unique_suffix();
  gripper.group_id = "gripper";
  gripper.joint_name = "finger_joint";
  gripper.wait_for_action_server_timeout_s = 0.0;
  backend.configure_gripper_action("tool", gripper);

  const auto result = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 1.0, "ros_gripper_command", "position_reached"));

  EXPECT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code,
            ros2_control_fault_reason::kGripperNotConfigured);
  const auto reads = sink->reads();
  ASSERT_EQ(reads.size(), 1u);
  EXPECT_EQ(reads.front().group_id, "gripper");
  EXPECT_EQ(reads.front().joint_name, "finger_joint");
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2ControlExecutionBackendTest,
     GripperRejectsNonFiniteStartPositionBeforeDispatch) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint",
                           std::numeric_limits<double>::quiet_NaN());
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());

  auto gripper = isaac_gripper_config(
      "/manyforge_non_finite_start_gripper_" + unique_suffix());
  backend.configure_gripper_action("tool", gripper);

  const auto result = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));

  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code,
            ros2_control_fault_reason::kGripperStateNotFinite);
  EXPECT_TRUE(sink->single_updates().empty());
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperGraspLossInvalidatesBackendOwnedStatus) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());

  Ros2GripperActionConfig gripper;
  gripper.transport_kind = "isaac_joint_state_effort";
  gripper.command_topic = "/manyforge_lost_grasp_gripper_" + unique_suffix();
  gripper.command_joint_names = {"finger_joint"};
  gripper.group_id = "gripper";
  gripper.joint_name = "finger_joint";
  gripper.completion_policy = "dwell_elapsed";
  gripper.open_position = 0.7;
  gripper.closed_position = 0.0;
  gripper.open_effort = 5.0;
  gripper.close_effort = -5.0;
  gripper.publish_nominal_position = true;
  backend.configure_gripper_action("tool", gripper);

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted)
      << (command.error ? command.error->message : "");

  const auto lost = backend.mark_gripper_grasp_lost(
      "tool", command.command_id, command.generation,
      "attached_drift_monitor",
      "Attached object drift exceeded measured TCP/object threshold",
      "grasp_confirmation_lost");
  ASSERT_TRUE(lost.accepted) << (lost.error ? lost.error->message : "");

  const auto failed = backend.get_gripper_status("tool");
  EXPECT_EQ(failed.state, core::GripperCommandState::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  EXPECT_EQ(failed.error->code, "grasp_confirmation_lost");
  EXPECT_EQ(failed.evidence_state, "pressure_or_part_lost");
  ASSERT_EQ(failed.evidence.size(), 1u);
  EXPECT_EQ(failed.evidence.front(), "attached_drift_monitor");
  EXPECT_NE(failed.error->message.find("Attached object drift exceeded"),
            std::string::npos);

  const auto next = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(next.accepted) << (next.error ? next.error->message : "");
  scheduler.advance_to(clock.now());
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  const auto reset = backend.get_gripper_status("tool");
  EXPECT_EQ(reset.state, core::GripperCommandState::kCompleted);
  EXPECT_TRUE(reset.evidence.empty());
  EXPECT_TRUE(reset.evidence_state.empty());
  EXPECT_FALSE(reset.error.has_value());
}

TEST(Ros2ControlExecutionBackendTest,
     StaleGripperGraspLossCannotFailReplacementGeneration) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_stale_grasp_loss_" + unique_suffix()));

  const auto old_command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(old_command.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  backend.authorize_gripper_commands();
  const auto replacement = backend.command_gripper(make_gripper_request(
      0.7, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(replacement.accepted);

  const auto stale = backend.mark_gripper_grasp_lost(
      "tool", old_command.command_id, old_command.generation,
      "attached_drift_monitor", "Delayed object drift",
      "grasp_confirmation_lost");
  ASSERT_FALSE(stale.accepted);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "stale_gripper_state");

  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.command_id, replacement.command_id);
  EXPECT_EQ(status.generation, replacement.generation);
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_FALSE(status.error.has_value());
}

TEST(Ros2ControlExecutionBackendTest,
     IsaacGripperSchedulerFailureRemainsAdmittedAndPublishesFailure) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  RejectingScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_schedule_failure_" + unique_suffix()));

  const auto result = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(result.accepted)
      << "the physical command crossed the transport boundary";

  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.command_id, result.command_id);
  EXPECT_EQ(status.generation, result.generation);
  EXPECT_EQ(status.state, core::GripperCommandState::kFailed);
  ASSERT_TRUE(status.error.has_value());
  EXPECT_EQ(status.error->code,
            ros2_control_fault_reason::kGripperRequestFailed);
}

TEST(Ros2ControlExecutionBackendTest,
     IsaacGripperLifecycleProgressesWithoutReadsAndHonorsDwell) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_autonomous_gripper_" + unique_suffix()));

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted)
      << (command.error ? command.error->message : "");
  EXPECT_EQ(scheduler.last_priority(),
            core::SchedulerPriority::ControllerCommand);

  const auto admitted = backend.get_gripper_status("tool");
  const auto repeated = backend.get_gripper_status("tool");
  EXPECT_EQ(admitted.revision, repeated.revision);
  EXPECT_EQ(admitted.state, core::GripperCommandState::kExecuting);

  clock.advance(199'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kExecuting);

  clock.advance(1'000'000);
  scheduler.advance_to(clock.now());
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  const auto completed = backend.get_gripper_status("tool");
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_DOUBLE_EQ(completed.progress, 1.0);
  ASSERT_FALSE(sink->single_updates().empty());
  EXPECT_DOUBLE_EQ(sink->single_updates().back().position_rad, 0.0);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperResetFencesQueuedLifecycleBeforeProjection) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_reset_fenced_gripper_" + unique_suffix()));

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  backend.invalidate_gripper_commands(
      core::GripperInvalidationReason::kReset);

  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kIdle);
  EXPECT_NE(status.generation, command.generation);
  EXPECT_TRUE(sink->single_updates().empty());

  const auto blocked = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_FALSE(blocked.accepted);
  ASSERT_TRUE(blocked.error.has_value());
  EXPECT_EQ(blocked.error->code, "gripper_command_not_authorized");
  backend.authorize_gripper_commands();
  EXPECT_TRUE(backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"))
                  .accepted);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperTerminalProjectionRetryIsBoundedAndGenerationOwned) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->fail_next_single_updates(2);
  sink->block_single_update();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_projection_retry_gripper_" + unique_suffix()));

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  scheduler.advance_to(clock.now());
  ASSERT_TRUE(sink->wait_for_single_update());
  auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_EQ(status.projection_state,
            core::GripperProjectionState::kPending);
  const auto while_pending = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_FALSE(while_pending.accepted);
  ASSERT_TRUE(while_pending.error.has_value());
  EXPECT_EQ(while_pending.error->code,
            ros2_control_fault_reason::kGripperBusy);

  sink->release_single_update();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(status.projection_state,
            core::GripperProjectionState::kApplied);
  ASSERT_EQ(sink->single_updates().size(), 3u);
  EXPECT_DOUBLE_EQ(sink->single_updates().back().position_rad, 0.0);
  const auto metrics = backend.scene_projection_metrics();
  EXPECT_EQ(metrics.terminal_retries_scheduled_total, 1u);
  EXPECT_EQ(metrics.terminal_retry_successes_total, 1u);
  EXPECT_EQ(metrics.terminal_retry_exhausted_total, 0u);
  const auto after_projection = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_TRUE(after_projection.accepted);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperInvalidationDoesNotWaitForDequeuedNrtProjectionAndFencesCompletion) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_single_update();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_projection_race_gripper_" + unique_suffix()));

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  auto callback = std::async(std::launch::async, [&]() {
    scheduler.advance_to(clock.now());
  });
  ASSERT_TRUE(sink->wait_for_single_update());
  EXPECT_EQ(callback.wait_for(20ms), std::future_status::ready)
      << "controller-command progression must only enqueue scene projection";

  auto invalidate = std::async(std::launch::async, [&]() {
    backend.invalidate_gripper_commands(
        core::GripperInvalidationReason::kReset);
  });
  EXPECT_EQ(invalidate.wait_for(20ms), std::future_status::ready)
      << "stop/reset invalidation must not wait behind NRT scene projection";

  sink->release_single_update();
  callback.get();
  invalidate.get();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  const auto updates_after_invalidation = sink->single_updates().size();
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(sink->single_updates().size(), updates_after_invalidation);
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
}

TEST(Ros2ControlExecutionBackendTest,
     ResetFencesDequeuedSceneProjectionBeforeReturningIdle) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      base_config());

  sink->block_group_update();
  backend.testing_schedule_active_scene_projection({0.25}, clock.now());
  ASSERT_TRUE(sink->wait_for_group_update());

  auto reset = std::async(std::launch::async, [&]() {
    return backend.reset();
  });
  EXPECT_EQ(reset.wait_for(20ms), std::future_status::timeout)
      << "reset must not return while a pre-reset scene write is still active";
  backend.testing_schedule_active_scene_projection({0.5}, clock.now());

  sink->release_group_update();
  const auto result = reset.get();
  EXPECT_TRUE(result.accepted)
      << (result.error ? result.error->message : "");
  EXPECT_EQ(backend.get_execution_snapshot().state,
            core::ExecutionState::kIdle);
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  EXPECT_EQ(sink->group_updates().size(), 1u)
      << "a frame submitted after reset suspension must not land";
  EXPECT_EQ(
      backend.scene_projection_metrics()
          .requests_rejected_while_suspended_total,
      1u);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperConfigurationReplacementFencesOldScheduledGeneration) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_replaced_gripper_a_" + unique_suffix()));
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);

  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_replaced_gripper_b_" + unique_suffix()));
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());

  const auto replacement = backend.get_gripper_status("tool");
  EXPECT_EQ(replacement.state, core::GripperCommandState::kIdle);
  EXPECT_NE(replacement.generation, command.generation);
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2ControlExecutionBackendTest,
     GripperConfigurationIsClosedBeforeExecutorStartup) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_prestart_gripper_" + unique_suffix()));

  backend.complete_ros_configuration();

  EXPECT_THROW(
      backend.configure_gripper_action(
          "late", isaac_gripper_config(
                      "/manyforge_late_gripper_" + unique_suffix())),
      std::logic_error);
  EXPECT_THROW(backend.complete_ros_configuration(), std::logic_error);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperConfigurationReplacementRejectsInFlightPreflight) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_preflight_gripper_a_" + unique_suffix()));

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_preflight_gripper_b_" + unique_suffix()));
  sink->release_read();

  const auto stale = in_flight.get();
  ASSERT_FALSE(stale.accepted);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "gripper_command_not_authorized");
  const auto replacement = backend.get_gripper_status("tool");
  EXPECT_EQ(replacement.state, core::GripperCommandState::kIdle);
  EXPECT_EQ(replacement.command_id, 0u);

  const auto fresh = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_TRUE(fresh.accepted)
      << (fresh.error ? fresh.error->message : "");
}

TEST(Ros2ControlExecutionBackendTest,
     LifecycleInvalidationDoesNotWaitForBlockedGripperPreflight) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_preflight_invalidation_" + unique_suffix()));

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  const auto started = std::chrono::steady_clock::now();
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 50ms);
  sink->release_read();

  const auto stale = in_flight.get();
  ASSERT_FALSE(stale.accepted);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "gripper_command_not_authorized");
}

TEST(Ros2ControlExecutionBackendTest,
     UnrelatedGripperConfigurationDoesNotRejectInFlightPreflight) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_independent_gripper_a_" + unique_suffix()));

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "auxiliary", isaac_gripper_config(
                       "/manyforge_independent_gripper_b_" + unique_suffix()));
  sink->release_read();

  const auto admitted = in_flight.get();
  EXPECT_TRUE(admitted.accepted)
      << (admitted.error ? admitted.error->message : "");
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedStateSyncAppliesGroupPositionsThroughExecutionStateSink) {
  const std::string topic = "/manyforge_joint_state_projection_" + unique_suffix();

  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.required_joints = {"joint_1", "joint_2"};
  config.joint_state_topic = topic;
  config.wait_for_state_timeout_s = 1.0;
  config.joint_state_max_age_s = 5.0;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);
  JointStatePublisher publisher(topic);

  ASSERT_TRUE(publisher.wait_for_subscription());
  for (int i = 0; i < 5; ++i) {
    publisher.publish({"joint_2", "joint_1"}, {0.2, 0.1});
    std::this_thread::sleep_for(20ms);
  }

  const auto result = backend.sync_observed_state_before_planning();
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  const auto updates = sink->group_updates();
  ASSERT_FALSE(updates.empty());
  bool saw_blocking_pre_plan_update = false;
  bool saw_best_effort_idle_mirror = false;
  for (const auto& update : updates) {
    EXPECT_EQ(update.group_id, "arm");
    EXPECT_EQ(update.positions_rad, (std::vector<double>{0.1, 0.2}));
    if (update.mode == core::ExecutionStateUpdateMode::kBlocking) {
      saw_blocking_pre_plan_update = true;
    }
    if (update.mode == core::ExecutionStateUpdateMode::kBestEffort) {
      saw_best_effort_idle_mirror = true;
    }
  }
  EXPECT_TRUE(saw_blocking_pre_plan_update);
  EXPECT_TRUE(saw_best_effort_idle_mirror);
}

TEST(Ros2ControlExecutionBackendTest,
     CompletionFreshSampleWaitDoesNotSelfStallActionResultCallback) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_e2_fjt_" + suffix;
  const std::string joint_state_topic = "/manyforge_e2_joint_states_" + suffix;

  FakeFjtServer server(action_name);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  config.executor_thread_count = 3;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);
  EXPECT_TRUE(backend.testing_action_progress_has_dedicated_callback_group());
  const auto* execution_progress = backend.execution_progress_publication();
  ASSERT_NE(execution_progress, nullptr);
  EXPECT_EQ(execution_progress->source_kind(),
            core::ExecutionProgressSourceKind::kMeasuredProjection);
  EXPECT_EQ(execution_progress->authority_profile(),
            core::ExecutionProgressAuthorityProfile::kMeasuredPhysical);
  EXPECT_GT(execution_progress->freshness_budget_ns(), 0);

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool initial_sample_observed = false;
  const auto initial_sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < initial_sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    auto sync = backend.sync_observed_state_before_planning();
    if (sync.ok()) {
      initial_sample_observed = true;
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_TRUE(initial_sample_observed);

  auto handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 0.1));
  ASSERT_NE(handle.execution_generation, 0u);
  ASSERT_TRUE(server.wait_for_goal());

  ASSERT_TRUE(server.publish_feedback(0.0));
  bool feedback_observed = false;
  const auto feedback_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < feedback_deadline) {
    const auto metrics = backend.controller_progress_metrics();
    if (metrics.action_feedback_callbacks_total > 0 &&
        metrics.command_progress_samples_total > 0) {
      feedback_observed = true;
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(feedback_observed)
      << "backend did not consume an explicitly published action feedback";

  core::ExecutionProgressCursorSnapshotDTO measured_progress;
  const auto progress_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < progress_deadline) {
    measured_progress = execution_progress->acquire();
    if (measured_progress.valid) {
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(measured_progress.valid);
  EXPECT_EQ(measured_progress.execution_generation,
            handle.execution_generation);
  EXPECT_EQ(measured_progress.source_kind,
            core::ExecutionProgressSourceKind::kMeasuredProjection);

  ASSERT_TRUE(server.succeed_current_goal());
  bool observed_terminal_flag = false;
  const auto result_delivery_deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < result_delivery_deadline) {
    if (handle.wait_for_completion(1'000'000)) {
      observed_terminal_flag = true;
      break;
    }
  }
  ASSERT_TRUE(observed_terminal_flag)
      << "backend did not observe the action result";

  const auto completion_wait_started = std::chrono::steady_clock::now();
  std::optional<core::ExecutionSnapshot> terminal_snapshot;
  const auto deadline = completion_wait_started + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    publisher.publish({"joint_1"}, {0.42});
    auto snapshot = backend.get_execution_snapshot();
    if (snapshot.state == core::ExecutionState::kCompleted ||
        snapshot.state == core::ExecutionState::kFailed ||
        snapshot.state == core::ExecutionState::kInterventionRequired) {
      terminal_snapshot = std::move(snapshot);
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  const auto completion_wait_duration =
      std::chrono::steady_clock::now() - completion_wait_started;
  ASSERT_TRUE(terminal_snapshot.has_value())
      << "backend did not publish a terminal snapshot";
  ASSERT_TRUE(handle.wait_for_completion(0));

  EXPECT_LT(completion_wait_duration, 500ms)
      << "completion should consume state callbacks while the action result "
         "callback waits, not sleep until wait_for_state_timeout_s";
  const auto& snapshot = *terminal_snapshot;
  EXPECT_EQ(snapshot.state, core::ExecutionState::kCompleted);
  const auto terminal_progress = execution_progress->acquire();
  EXPECT_TRUE(terminal_progress.valid);
  EXPECT_TRUE(terminal_progress.terminal);
  EXPECT_EQ(terminal_progress.execution_generation,
            handle.execution_generation);
  backend.retire_execution_progress_generation(handle.execution_generation);
  EXPECT_FALSE(execution_progress->acquire().valid);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_NEAR(snapshot.joint_positions_rad.front(), 0.42, 1e-9);

  const auto metrics = backend.controller_progress_metrics();
  EXPECT_EQ(metrics.executor_thread_count, 3u);
  EXPECT_GT(metrics.state_callbacks_total, 0u);
  EXPECT_GT(metrics.action_feedback_callbacks_total, 0u);
  EXPECT_GT(metrics.action_result_callbacks_total, 0u);
  // Explicit feedback must count as command progress before terminal delivery.
  EXPECT_GT(metrics.command_progress_samples_total, 0u);
  EXPECT_GT(metrics.last_action_result_latency_ns, 0u);
}

TEST(Ros2ControlExecutionBackendTest,
     ControlledStopCommandReferenceValidatesOnceAndPublishesSamples) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      config);

  const auto started_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(backend.testing_install_controlled_stop_command_reference(
      make_single_joint_trajectory(0.0, 1.0), started_at));
  ASSERT_TRUE(backend.testing_publish_controlled_stop_command_reference_snapshot(
      started_at + 50ms));
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));

  const auto updates = sink->group_updates();
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates.front().group_id, "arm");
  ASSERT_EQ(updates.front().positions_rad.size(), 1u);
  EXPECT_NEAR(updates.front().positions_rad.front(), 0.5, 1e-9);
  EXPECT_EQ(updates.front().mode, core::ExecutionStateUpdateMode::kBestEffort);

  const auto snapshot = backend.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, core::ExecutionState::kStopping);
  EXPECT_NEAR(snapshot.progress, 0.5, 1e-9);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_NEAR(snapshot.joint_positions_rad.front(), 0.5, 1e-9);

  auto invalid = make_single_joint_trajectory(0.0, 1.0);
  invalid.waypoints.back().time_from_start_s =
      invalid.waypoints.front().time_from_start_s;
  EXPECT_FALSE(backend.testing_install_controlled_stop_command_reference(
      invalid, started_at));
}

TEST(Ros2ControlExecutionBackendTest,
     IdleSceneSyncRateLimitsAndSkipsWhileRunning) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      config);

  const auto t0 = std::chrono::steady_clock::now();
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.1}, t0);
  backend.testing_sync_idle_scene_from_latest_sample();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  ASSERT_EQ(sink->group_updates().size(), 1u);

  backend.testing_sync_idle_scene_from_latest_sample();
  EXPECT_EQ(sink->group_updates().size(), 1u)
      << "same sample should not be mirrored repeatedly";

  backend.testing_store_latest_joint_sample({"joint_1"}, {0.2}, t0 + 10ms);
  backend.testing_sync_idle_scene_from_latest_sample();
  EXPECT_EQ(sink->group_updates().size(), 1u)
      << "new samples inside the idle projection period are rate limited";

  std::this_thread::sleep_for(120ms);
  backend.testing_store_latest_joint_sample(
      {"joint_1"}, {0.3}, std::chrono::steady_clock::now());
  backend.testing_sync_idle_scene_from_latest_sample();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  ASSERT_EQ(sink->group_updates().size(), 2u);

  backend.testing_set_running_state(true);
  backend.testing_store_latest_joint_sample(
      {"joint_1"}, {0.4}, std::chrono::steady_clock::now());
  backend.testing_sync_idle_scene_from_latest_sample();
  EXPECT_EQ(sink->group_updates().size(), 2u)
      << "running executions must not mirror passive idle scene updates";

  backend.testing_set_running_state(false);
  backend.testing_store_latest_joint_sample(
      {"joint_1"}, {0.5}, std::chrono::steady_clock::now());
  backend.testing_sync_idle_scene_from_latest_sample();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  ASSERT_EQ(sink->group_updates().size(), 3u)
      << "the first sample after an execution generation is not rate limited";
  EXPECT_NEAR(sink->group_updates().back().positions_rad.front(), 0.5, 1e-9);
}

TEST(Ros2ControlExecutionBackendTest,
     ActiveSceneProjectionIsAsyncAndCoalescesToLatestState) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      base_config());

  sink->block_group_update();
  backend.testing_schedule_active_scene_projection({0.1}, 1);
  ASSERT_TRUE(sink->wait_for_group_update());

  const auto submitted_at = std::chrono::steady_clock::now();
  backend.testing_schedule_active_scene_projection({0.2}, 2);
  backend.testing_schedule_active_scene_projection({0.3}, 3);
  EXPECT_LT(std::chrono::steady_clock::now() - submitted_at, 20ms)
      << "controller progress must not wait for an in-flight scene projection";

  sink->release_group_update();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));

  const auto updates = sink->group_updates();
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_EQ(updates.front().positions_rad, (std::vector<double>{0.1}));
  EXPECT_EQ(updates.back().positions_rad, (std::vector<double>{0.3}));
  EXPECT_EQ(updates.back().mode,
            core::ExecutionStateUpdateMode::kBestEffort);

  const auto metrics = backend.scene_projection_metrics();
  EXPECT_EQ(metrics.active_requests_total, 3u);
  EXPECT_EQ(metrics.requests_coalesced_total, 1u);
  EXPECT_EQ(metrics.successful_updates_total, 2u);
  EXPECT_GT(metrics.apply_duration_max_ns, 0u);
}

TEST(Ros2ControlExecutionBackendTest,
     ArmAndGripperProjectionUseIndependentLatestOnlySlots) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{},
      sink,
      "arm",
      clock,
      scheduler,
      base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config(
                  "/manyforge_independent_projection_slots_" + unique_suffix()));
  const auto gripper = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(gripper.accepted);

  sink->block_group_update();
  backend.testing_schedule_active_scene_projection({0.1}, 1);
  ASSERT_TRUE(sink->wait_for_group_update());
  scheduler.advance_to(clock.now());
  backend.testing_schedule_active_scene_projection({0.2}, 2);
  backend.testing_schedule_active_scene_projection({0.3}, 3);

  sink->release_group_update();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  const auto group_updates = sink->group_updates();
  const auto joint_updates = sink->single_updates();
  ASSERT_EQ(group_updates.size(), 2u);
  EXPECT_EQ(group_updates.front().positions_rad,
            (std::vector<double>{0.1}));
  EXPECT_EQ(group_updates.back().positions_rad,
            (std::vector<double>{0.3}));
  ASSERT_EQ(joint_updates.size(), 1u);
  EXPECT_DOUBLE_EQ(joint_updates.front().position_rad, 0.0);
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kCompleted);

  const auto metrics = backend.scene_projection_metrics();
  EXPECT_EQ(metrics.joint_requests_total, 1u);
  EXPECT_EQ(metrics.requests_coalesced_total, 1u);
}

TEST(Ros2ControlExecutionBackendTest,
     ActionFeedbackSampleStorageReusesVectorCapacity) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      base_config());

  std::vector<std::string> names;
  std::vector<double> positions;
  names.reserve(64);
  positions.reserve(64);
  for (int i = 0; i < 64; ++i) {
    names.push_back("joint_" + std::to_string(i));
    positions.push_back(static_cast<double>(i));
  }
  const auto t0 = std::chrono::steady_clock::now();
  backend.testing_store_action_feedback_sample(names, positions, {}, t0);
  const auto first_capacity =
      backend.testing_latest_action_feedback_position_capacity();
  ASSERT_GE(first_capacity, positions.size());

  backend.testing_store_action_feedback_sample(
      {"joint_1"}, {1.0}, {}, t0 + 1ms);
  EXPECT_GE(backend.testing_latest_action_feedback_position_capacity(),
            first_capacity);
}

TEST(SceneProjectionWorkerTest,
     CompletionCallbackExceptionIsContainedAndMetriced) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  detail::SceneProjectionWorker worker(sink, "arm", clock);
  const auto slot = worker.register_joint_slot(
      "gripper", "finger_joint",
      [](const detail::SceneProjectionCompletion&) {
        throw std::runtime_error("injected completion callback failure");
      });

  ASSERT_TRUE(worker.submit_active_joint(
      slot, 0.25, clock.now(), detail::SceneProjectionToken{1, 2, 3}));
  ASSERT_TRUE(worker.drain_for(1s));

  const auto metrics = worker.metrics();
  EXPECT_EQ(metrics.successful_updates_total, 1U);
  EXPECT_EQ(metrics.completion_callback_exceptions_total, 1U);
  EXPECT_EQ(sink->single_updates().size(), 1U);
  EXPECT_TRUE(worker.stop_and_join_for(1s));
}

TEST(Ros2ControlExecutionBackendTest,
     TerminalSceneProjectionMissIsMetricedAndRetriedAfterSnapshot) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000);
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      base_config());

  const auto t0 = std::chrono::steady_clock::now();
  backend.testing_store_action_feedback_sample(
      {"joint_1"}, {0.42}, {}, t0);
  sink->fail_next_group_updates(1);

  backend.testing_complete_execution();

  const auto terminal = backend.get_execution_snapshot();
  EXPECT_EQ(terminal.state, core::ExecutionState::kCompleted);
  ASSERT_EQ(terminal.joint_positions_rad.size(), 1u);
  EXPECT_NEAR(terminal.joint_positions_rad.front(), 0.42, 1e-9);

  auto metrics = backend.scene_projection_metrics();
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (metrics.terminal_retry_successes_total == 0u &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
    metrics = backend.scene_projection_metrics();
  }

  EXPECT_EQ(metrics.misses_total, 1u);
  EXPECT_EQ(metrics.terminal_retries_scheduled_total, 1u);
  EXPECT_EQ(metrics.terminal_retry_successes_total, 1u);
  EXPECT_EQ(metrics.terminal_retry_exhausted_total, 0u);
  EXPECT_GE(metrics.successful_updates_total, 1u);
  EXPECT_GT(metrics.last_success_timestamp_ns, 0);

  const auto updates = sink->group_updates();
  ASSERT_GE(updates.size(), 2u);
  EXPECT_EQ(updates.front().mode, core::ExecutionStateUpdateMode::kBestEffort);
  EXPECT_EQ(updates.back().mode, core::ExecutionStateUpdateMode::kBestEffort);
  ASSERT_EQ(updates.back().positions_rad.size(), 1u);
  EXPECT_NEAR(updates.back().positions_rad.front(), 0.42, 1e-9);
}

TEST(Ros2ControlExecutionBackendTest,
     ManagedFjtTerminalResultRequiresCurrentGenerationAndGoalId) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      base_config());

  rclcpp_action::GoalUUID old_goal{};
  old_goal[0] = 1;
  rclcpp_action::GoalUUID current_goal{};
  current_goal[0] = 2;

  backend.testing_begin_managed_fjt_terminal_session(
      /*generation=*/2, current_goal);

  Ros2ControlExecutionBackend::TestingGoalHandleFJT::WrappedResult stale;
  stale.goal_id = old_goal;
  stale.code = rclcpp_action::ResultCode::SUCCEEDED;
  stale.result = std::make_shared<
      Ros2ControlExecutionBackend::TestingFollowJointTrajectory::Result>();
  stale.result->error_code =
      Ros2ControlExecutionBackend::TestingFollowJointTrajectory::Result::
          SUCCESSFUL;

  backend.testing_on_managed_fjt_terminal_result(stale, /*generation=*/1);

  EXPECT_FALSE(backend.testing_managed_fjt_terminal_received())
      << "late result from a previous replacement dispatch must not satisfy "
         "the current managed-stop waiter";

  Ros2ControlExecutionBackend::TestingGoalHandleFJT::WrappedResult current;
  current.goal_id = current_goal;
  current.code = rclcpp_action::ResultCode::SUCCEEDED;
  current.result = std::make_shared<
      Ros2ControlExecutionBackend::TestingFollowJointTrajectory::Result>();
  current.result->error_code =
      Ros2ControlExecutionBackend::TestingFollowJointTrajectory::Result::
          SUCCESSFUL;

  backend.testing_on_managed_fjt_terminal_result(current, /*generation=*/2);

  EXPECT_TRUE(backend.testing_managed_fjt_terminal_received());
  EXPECT_TRUE(backend.testing_managed_fjt_terminal_success());
}

TEST(Ros2ControlExecutionBackendTest,
     ManagedTerminalWaitWakesOnStopTokenWithoutNotify) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      scheduler,
      base_config());

  std::stop_source stop_source;
  const auto started = std::chrono::steady_clock::now();
  auto future = std::async(std::launch::async, [&]() {
    return backend.testing_wait_for_managed_terminal_or_stop(
        stop_source.get_token(), 5s);
  });

  std::this_thread::sleep_for(20ms);
  stop_source.request_stop();
  ASSERT_EQ(future.wait_for(500ms), std::future_status::ready)
      << "token-aware managed-stop waits must not sleep until deadline";
  EXPECT_FALSE(future.get());
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedStatePublicationFallsBackWhenActionFeedbackStops) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.joint_state_max_age_s = 0.1;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);
  const auto* progress = backend.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  EXPECT_EQ(progress->source_kind(),
            core::ExecutionProgressSourceKind::kMeasuredProjection);
  EXPECT_EQ(progress->authority_profile(),
            core::ExecutionProgressAuthorityProfile::kMeasuredPhysical);
  EXPECT_GT(progress->freshness_budget_ns(), 0);

  const auto base = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample(
      {"joint_1"}, {0.25}, {0.5}, base);
  backend.testing_store_latest_joint_sample(
      {"joint_1"}, {0.75}, base + 150ms);

  const auto* publication = backend.observed_robot_state_publication();
  ASSERT_NE(publication, nullptr);
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          (base + 160ms).time_since_epoch())
                          .count();
  auto selected = publication->acquire(now_ns);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.source_kind(),
            core::ObservedRobotStateSourceKind::kJointState);
  EXPECT_EQ(selected.source_id(), "joint_state");
  ASSERT_EQ(selected.positions_rad().size(), 1u);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[0], 0.75);
  EXPECT_EQ(selected.received_timestamp_ns(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                (base + 150ms).time_since_epoch())
                .count());
  const auto metrics = backend.controller_progress_metrics();
  ASSERT_EQ(metrics.observed_state_sources.size(), 3u);
  EXPECT_EQ(metrics.observed_state_sources[1].source_kind,
            core::ObservedRobotStateSourceKind::kJointState);
  EXPECT_EQ(metrics.observed_state_sources[1].published_total, 1u);
  EXPECT_EQ(metrics.observed_state_sources[2].source_kind,
            core::ObservedRobotStateSourceKind::kActionFeedback);
  EXPECT_EQ(metrics.observed_state_sources[2].published_total, 1u);
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedStateSourcePrecedenceIsDeploymentDeclared) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.observed_state_source_precedence = {"joint_state",
                                             "action_feedback"};
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);
  const auto now = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample(
      {"joint_1"}, {0.25}, {0.0}, now);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.75}, now);

  auto selected = backend.observed_robot_state_publication()->acquire(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch())
          .count());
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.source_kind(),
            core::ObservedRobotStateSourceKind::kJointState);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[0], 0.75);
}

TEST(Ros2ControlExecutionBackendTest,
     AuthoredPreviewPublishesCommandedNotMeasuredAuthority) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "authored_preview";
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);
  const auto* progress = backend.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  EXPECT_EQ(progress->source_kind(),
            core::ExecutionProgressSourceKind::kBackendCommanded);
  EXPECT_EQ(progress->authority_profile(),
            core::ExecutionProgressAuthorityProfile::kCommandedSimulation);
  EXPECT_EQ(progress->freshness_budget_ns(), 0);
  const auto now = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample(
      {"joint_1"}, {0.25}, {0.0}, now);
  ASSERT_TRUE(backend.testing_publish_snapshot(
      core::ExecutionState::kIdle, {0.75}));

  auto selected = backend.observed_robot_state_publication()->acquire(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch())
          .count());
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.source_kind(),
            core::ObservedRobotStateSourceKind::kPredictedState);
  EXPECT_EQ(selected.source_id(), "authored_preview_state");
  EXPECT_EQ(selected.authority_profile(),
            core::ObservedRobotStateAuthorityProfile::kCommandedSimulation);
  EXPECT_EQ(selected.authority_profile_id(),
            "ros2_control_authored_preview_v1");
  ASSERT_EQ(selected.positions_rad().size(), 1u);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[0], 0.75);
  ASSERT_EQ(selected.velocities_rad_s().size(), 1u);
  EXPECT_DOUBLE_EQ(selected.velocities_rad_s()[0], 0.0);
}

TEST(Ros2ControlExecutionBackendTest,
     CommandedSimKeepsCommandReferenceAuthoritativeOverMeasuredUpdates) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.0};
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);
  const auto* progress = backend.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  EXPECT_EQ(progress->source_kind(),
            core::ExecutionProgressSourceKind::kBackendCommanded);
  EXPECT_EQ(progress->authority_profile(),
            core::ExecutionProgressAuthorityProfile::kCommandedSimulation);
  EXPECT_EQ(progress->freshness_budget_ns(), 0);

  auto initial = backend.observed_robot_state_publication()->acquire(clock.now());
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial.source_kind(),
            core::ObservedRobotStateSourceKind::kCommandedState);
  ASSERT_EQ(initial.positions_rad().size(), 1u);
  EXPECT_DOUBLE_EQ(initial.positions_rad()[0], 0.0);

  ASSERT_TRUE(backend.testing_publish_snapshot(
      core::ExecutionState::kIdle, {0.75}));
  const auto now = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample(
      {"joint_1"}, {0.25}, {0.5}, now);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.5}, now);

  auto selected = backend.observed_robot_state_publication()->acquire(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch())
          .count());
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.source_kind(),
            core::ObservedRobotStateSourceKind::kCommandedState);
  EXPECT_EQ(selected.source_id(), "commanded_state");
  EXPECT_EQ(selected.authority_profile(),
            core::ObservedRobotStateAuthorityProfile::kCommandedSimulation);
  EXPECT_EQ(selected.authority_profile_id(),
            "ros2_control_commanded_sim_v1");
  ASSERT_EQ(selected.positions_rad().size(), 1u);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[0], 0.75);
  ASSERT_EQ(selected.velocities_rad_s().size(), 1u);
  EXPECT_DOUBLE_EQ(selected.velocities_rad_s()[0], 0.0);
}

TEST(Ros2ControlExecutionBackendTest,
     CommandedSimPublishesJtcCancelReferenceWithScopedToken) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.required_joints = {"joint_1", "joint_2"};
  config.initial_joint_positions_rad = {0.0, 0.0};
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);
  backend.testing_set_running_state(true);

  const auto first_token =
      backend.testing_begin_jtc_cancel_reference_authority();
  ASSERT_NE(first_token, 0u);
  ASSERT_TRUE(backend.testing_publish_jtc_cancel_reference(
      {"joint_2", "joint_1"}, {0.8, 0.4}, {0.2, 0.1}, 100,
      first_token));

  auto selected = backend.observed_robot_state_publication()->acquire(100);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.source_kind(),
            core::ObservedRobotStateSourceKind::kCommandedState);
  ASSERT_EQ(selected.positions_rad().size(), 2u);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[0], 0.4);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[1], 0.8);
  ASSERT_EQ(selected.velocities_rad_s().size(), 2u);
  EXPECT_DOUBLE_EQ(selected.velocities_rad_s()[0], 0.1);
  EXPECT_DOUBLE_EQ(selected.velocities_rad_s()[1], 0.2);

  backend.testing_end_jtc_cancel_reference_authority(first_token);
  const auto second_token =
      backend.testing_begin_jtc_cancel_reference_authority();
  ASSERT_NE(second_token, 0u);
  ASSERT_NE(second_token, first_token);
  EXPECT_FALSE(backend.testing_publish_jtc_cancel_reference(
      {"joint_1", "joint_2"}, {0.9, 0.9}, {0.0, 0.0}, 200,
      first_token));
  ASSERT_TRUE(backend.testing_publish_jtc_cancel_reference(
      {"joint_1", "joint_2"}, {0.5, 0.6}, {0.0, 0.0}, 201,
      second_token));
  backend.testing_end_jtc_cancel_reference_authority(second_token);

  selected = backend.observed_robot_state_publication()->acquire(201);
  ASSERT_TRUE(selected);
  ASSERT_EQ(selected.positions_rad().size(), 2u);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[0], 0.5);
  EXPECT_DOUBLE_EQ(selected.positions_rad()[1], 0.6);
  const auto metrics = backend.controller_progress_metrics();
  EXPECT_EQ(metrics.jtc_cancel_reference_publications_total, 2u);
  EXPECT_EQ(metrics.jtc_cancel_reference_rejections_total, 1u);
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedRos2NeverDelegatesAuthorityToJtcCommandReference) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "observed_ros2";
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);
  backend.testing_set_running_state(true);

  EXPECT_EQ(backend.testing_begin_jtc_cancel_reference_authority(), 0u);
  EXPECT_FALSE(backend.testing_publish_jtc_cancel_reference(
      {"joint_1"}, {0.5}, {0.0}, 100, 1));
  EXPECT_EQ(backend.controller_progress_metrics()
                .jtc_cancel_reference_publications_total,
            0u);
  EXPECT_EQ(backend.controller_progress_metrics()
                .jtc_cancel_reference_rejections_total,
            1u);
}

TEST(Ros2ControlExecutionBackendTest,
     TerminalGenerationRejectsLateMonitorSnapshotAndCommandAuthority) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.0};
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);

  backend.testing_set_running_state(true);
  const auto execution_generation = backend.testing_running_generation();
  ASSERT_TRUE(backend.testing_publish_snapshot_for_generation(
      core::ExecutionState::kExecuting, {0.5}, execution_generation));

  backend.testing_set_running_state(false);
  ASSERT_TRUE(backend.testing_publish_snapshot(
      core::ExecutionState::kCompleted, {1.0}));
  EXPECT_FALSE(backend.testing_publish_snapshot_for_generation(
      core::ExecutionState::kExecuting, {0.75}, execution_generation));

  const auto snapshot = backend.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, core::ExecutionState::kCompleted);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[0], 1.0);
  auto observed = backend.observed_robot_state_publication()->acquire(clock.now());
  ASSERT_TRUE(observed);
  ASSERT_EQ(observed.positions_rad().size(), 1u);
  EXPECT_DOUBLE_EQ(observed.positions_rad()[0], 1.0);
}

TEST(Ros2ControlExecutionBackendTest,
     TerminalCommandPublicationFailureInvalidatesPlanningAuthority) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.0};
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
      clock, scheduler, config);

  const auto* publication = backend.observed_robot_state_publication();
  ASSERT_NE(publication, nullptr);
  auto first = publication->acquire(clock.now());
  ASSERT_TRUE(first);
  ASSERT_TRUE(backend.testing_publish_snapshot(
      core::ExecutionState::kExecuting, {0.25}));
  auto second = publication->acquire(clock.now());
  ASSERT_TRUE(second);
  ASSERT_TRUE(backend.testing_publish_snapshot(
      core::ExecutionState::kExecuting, {0.5}));

  // The current slot is never overwritten and both older slots are pinned.
  // A terminal authority replacement must fail closed instead of leaving the
  // non-expiring 0.5 command reference admissible for the next plan.
  EXPECT_FALSE(backend.testing_publish_snapshot(
      core::ExecutionState::kCompleted, {1.0}));
  ASSERT_NE(backend.authored_state_authority_epoch(), nullptr);
  const auto invalid_epoch = backend.authored_state_authority_epoch()->load(
      std::memory_order_acquire);
  EXPECT_NE(invalid_epoch & 1u, 0u);

  first = {};
  second = {};
  EXPECT_TRUE(backend.testing_publish_snapshot(
      core::ExecutionState::kIdle, {1.0}));
  const auto restored_epoch = backend.authored_state_authority_epoch()->load(
      std::memory_order_acquire);
  EXPECT_EQ(restored_epoch & 1u, 0u);
  EXPECT_GT(restored_epoch, invalid_epoch);
}

}  // namespace
}  // namespace manyforge::ros2_control_execution
