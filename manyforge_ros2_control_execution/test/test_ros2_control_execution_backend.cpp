#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>

#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <atomic>
#include <chrono>
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

}  // namespace
}  // namespace manyforge::ros2_control_execution
