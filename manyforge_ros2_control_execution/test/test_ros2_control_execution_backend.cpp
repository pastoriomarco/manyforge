#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#define MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#undef MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING

#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    std::lock_guard<std::mutex> lock(mutex_);
    group_updates_.push_back({group_id, positions_rad, mode});
    return {group_status_};
  }

  std::optional<double> group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const override {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    single_updates_.push_back({group_id, joint_name, position_rad, mode});
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

 private:
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, double> joint_positions_;
  mutable std::vector<SingleJointRead> reads_;
  std::vector<SingleJointUpdate> single_updates_;
  std::vector<GroupUpdate> group_updates_;
  core::ExecutionStateSinkStatus group_status_ =
      core::ExecutionStateSinkStatus::kApplied;
  core::ExecutionStateSinkStatus single_update_status_ =
      core::ExecutionStateSinkStatus::kApplied;
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
    publisher_.reset();
    node_.reset();
    if (context_) {
      context_->shutdown("test shutdown");
      context_.reset();
    }
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
          auto feedback = std::make_shared<FollowJointTrajectory::Feedback>();
          feedback->joint_names = {"joint_1"};
          feedback->desired.positions = {0.0};
          feedback->actual.positions = {0.0};
          feedback->error.positions = {0.0};
          goal_handle->publish_feedback(feedback);
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
  EXPECT_THROW(
      Ros2ControlExecutionBackend(nullptr, "arm", clock, base_config()),
      std::invalid_argument);
}

TEST(Ros2ControlExecutionBackendTest,
     GripperProjectionReadsStartPositionFromExecutionStateSink) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, base_config());

  Ros2GripperActionConfig gripper;
  gripper.action_name = "/manyforge_missing_gripper_" + unique_suffix();
  gripper.group_id = "gripper";
  gripper.joint_name = "finger_joint";
  gripper.wait_for_action_server_timeout_s = 0.0;
  backend.configure_gripper_action("tool", gripper);

  const auto result = backend.command_gripper("tool", 0.7, 0.0, 1.0);

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason_code, ros2_control_fault_reason::kGripperNotConfigured);
  const auto reads = sink->reads();
  ASSERT_EQ(reads.size(), 1u);
  EXPECT_EQ(reads.front().group_id, "gripper");
  EXPECT_EQ(reads.front().joint_name, "finger_joint");
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2ControlExecutionBackendTest,
     GripperGraspLossInvalidatesBackendOwnedStatus) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, base_config());

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

  const auto command = backend.command_gripper("tool", 0.0, 0.2, 0.0);
  ASSERT_TRUE(command.accepted) << command.error;

  const auto lost = backend.mark_gripper_grasp_lost(
      "tool", "attached_drift_monitor",
      "Attached object drift exceeded measured TCP/object threshold",
      "grasp_confirmation_lost");
  ASSERT_TRUE(lost.accepted) << lost.error;

  const auto failed = backend.get_gripper_status("tool");
  EXPECT_EQ(failed.state, "failed");
  EXPECT_EQ(failed.reason_code, "grasp_confirmation_lost");
  EXPECT_EQ(failed.evidence_state, "pressure_or_part_lost");
  ASSERT_EQ(failed.evidence.size(), 1u);
  EXPECT_EQ(failed.evidence.front(), "attached_drift_monitor");
  EXPECT_NE(failed.error.find("Attached object drift exceeded"),
            std::string::npos);

  const auto next = backend.command_gripper("tool", 0.7, 0.0, 0.0);
  ASSERT_TRUE(next.accepted) << next.error;
  const auto reset = backend.get_gripper_status("tool");
  EXPECT_EQ(reset.state, "executing");
  EXPECT_TRUE(reset.evidence.empty());
  EXPECT_TRUE(reset.evidence_state.empty());
  EXPECT_TRUE(reset.reason_code.empty());
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedStateSyncAppliesGroupPositionsThroughExecutionStateSink) {
  const std::string topic = "/manyforge_joint_state_projection_" + unique_suffix();

  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  auto config = base_config();
  config.required_joints = {"joint_1", "joint_2"};
  config.joint_state_topic = topic;
  config.wait_for_state_timeout_s = 1.0;
  config.joint_state_max_age_s = 5.0;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, config);
  JointStatePublisher publisher(topic);

  ASSERT_TRUE(publisher.wait_for_subscription());
  for (int i = 0; i < 5; ++i) {
    publisher.publish({"joint_2", "joint_1"}, {0.2, 0.1});
    std::this_thread::sleep_for(20ms);
  }

  const auto result = backend.sync_observed_state_before_planning();
  ASSERT_TRUE(result.ok()) << result.error().message;
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
  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, config);

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
  ASSERT_TRUE(server.wait_for_goal());

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
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_NEAR(snapshot.joint_positions_rad.front(), 0.42, 1e-9);
}

TEST(Ros2ControlExecutionBackendTest,
     ControlledStopCommandReferenceValidatesOnceAndPublishesSamples) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  auto config = base_config();
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      config);

  const auto started_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(backend.testing_install_controlled_stop_command_reference(
      make_single_joint_trajectory(0.0, 1.0), started_at));
  ASSERT_TRUE(backend.testing_publish_controlled_stop_command_reference_snapshot(
      started_at + 50ms));

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
  auto config = base_config();
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
      config);

  const auto t0 = std::chrono::steady_clock::now();
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.1}, t0);
  backend.testing_sync_idle_scene_from_latest_sample();
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
  ASSERT_EQ(sink->group_updates().size(), 3u)
      << "the first sample after an execution generation is not rate limited";
  EXPECT_NEAR(sink->group_updates().back().positions_rad.front(), 0.5, 1e-9);
}

TEST(Ros2ControlExecutionBackendTest,
     ActionFeedbackSampleStorageReusesVectorCapacity) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
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

TEST(Ros2ControlExecutionBackendTest,
     ManagedFjtTerminalResultRequiresCurrentGenerationAndGoalId) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      sink,
      "arm",
      clock,
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

}  // namespace
}  // namespace manyforge::ros2_control_execution
