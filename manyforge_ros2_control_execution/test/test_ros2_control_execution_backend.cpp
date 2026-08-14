#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#define MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#undef MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING

#include "../src/scene_projection_worker.hpp"

#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <controller_manager_msgs/srv/list_controllers.hpp>
#include <controller_manager_msgs/srv/list_hardware_components.hpp>
#include <gtest/gtest.h>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <algorithm>
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
  core::ScheduleHandle schedule(core::TimePoint, std::function<void()>,
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

core::ResolvedParticipantStopStageDTO direct_stage_fixture() {
  const core::OperationId operation_id("operation.direct");
  const core::OperationRevision operation_revision(1U);
  const core::OperationParticipantId participant_id("participant.direct");
  return core::ResolvedParticipantStopStageDTO{
      core::StopId("stop.direct"),
      core::StopGeneration(1U),
      operation_id,
      operation_revision,
      participant_id,
      core::ControllerGoalId("goal.direct"),
      core::ControllerGoalGeneration(1U),
      core::ControllableComponentId("robot.1"),
      core::CommandTargetId("arm.target"),
      core::CapabilityId("execution.trajectory.execute"),
      core::ControlDomainId("domain.arm"),
      core::ControlDomainRevision("domain.revision"),
      core::ControlBindingId("binding.arm"),
      core::ControlBindingRevision("binding.revision"),
      core::ActiveControlConfigurationId("active.arm"),
      core::ActiveControlConfigurationGeneration(1U),
      core::ControllerTopologyId("topology.arm"),
      core::TopologyRevision("topology.revision"),
      core::ControlRouteId("route.arm"),
      std::nullopt,
      std::nullopt,
      core::RuntimeEpoch("runtime.epoch"),
      {},
      core::ContainmentGranularity::kResource,
      core::StopEvidencePolicyId("policy.arm"),
      core::StopEvidencePolicyRevision("policy.revision"),
      core::PreparedParticipantStopPlanDTO{
          core::ContentDigest("plan.digest"),
          core::ResolvedStopBasePlanId("base.plan"),
          operation_id,
          operation_revision,
          participant_id,
          core::ProgramId("program.direct"),
          core::ProgramRevision("program.revision"),
          core::ProgramBindingId("program.binding"),
          core::ProgramRunId("program.run"),
          std::nullopt,
          core::ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt,
          core::StopDeadlineBudgetDTO{1'000'000'000, 1, {}},
          {}},
      0U,
      core::ContentDigest("scope.digest")};
}

ControllerBindingContract
binding_contract_for(const Ros2ControlExecutionBackendConfig &config,
                     const std::string &controller_manager) {
  ControllerBindingContract contract;
  contract.generation_id = "binding-" + unique_suffix();
  contract.controller_manager = controller_manager;
  contract.activity_topic = controller_manager + "/activity";
  contract.hardware_component_name = "test_hardware";
  contract.trajectory_controller_manager_name = "test_trajectory_controller";
  contract.trajectory_controller_ros_name =
      controller_manager.substr(
          0, controller_manager.rfind("/controller_manager")) +
      "/test_trajectory_controller";
  contract.trajectory_controller_type =
      "joint_trajectory_controller/JointTrajectoryController";
  contract.follow_joint_trajectory_action = config.action_name;
  contract.controller_state_topic = config.controller_state_topic;
  contract.joint_state_topic = config.joint_state_topic;
  contract.required_joints = config.required_joints;
  for (const auto &joint : config.required_joints) {
    contract.claimed_command_interfaces.push_back(joint + "/position");
    contract.claimed_command_interfaces.push_back(joint + "/velocity");
    contract.required_state_interfaces.push_back(joint + "/position");
    contract.required_state_interfaces.push_back(joint + "/velocity");
  }
  contract.snapshot_max_age_ns = 2'000'000'000;
  return contract;
}

ExecutionAdmissionSnapshot
admitted_snapshot_for(const ControllerBindingContract &contract,
                      std::int64_t now_ns, std::uint64_t generation = 1,
                      std::uint64_t lifecycle_generation = 1) {
  ExecutionAdmissionSnapshot admission;
  admission.generation = generation;
  admission.lifecycle_generation = lifecycle_generation;
  admission.robot_id = "lite6-a";
  admission.binding_contract_generation_id = contract.generation_id;
  admission.runtime_generation_id = "runtime-1";
  admission.provider_component_epoch = "provider-epoch-1";
  admission.transport_owner_component_epoch = "owner-epoch-1";
  admission.authority_component_epoch = "authority-epoch-1";
  admission.cell_supervisor_component_epoch = "supervisor-epoch-1";
  admission.observed_timestamp_ns = now_ns;
  admission.received_timestamp_ns = now_ns;
  admission.fresh_until_ns = now_ns + 10'000'000'000;
  admission.controller_ready = true;
  admission.motion_admitted = true;
  return admission;
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

class FakeFjtServer {
public:
  explicit FakeFjtServer(const std::string &action_name) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);

    rclcpp::NodeOptions options;
    options.context(context_);
    node_ = std::make_shared<rclcpp::Node>(
        "manyforge_fjt_server_test_" + unique_suffix(), options);
    server_ = rclcpp_action::create_server<FollowJointTrajectory>(
        node_, action_name,
        [this](const rclcpp_action::GoalUUID &,
               std::shared_ptr<const FollowJointTrajectory::Goal>) {
          std::unique_lock<std::mutex> lock(goal_response_mutex_);
          if (block_goal_response_) {
            goal_response_entered_ = true;
            goal_response_cv_.notify_all();
            goal_response_cv_.wait_for(
                lock, 2s, [this]() { return release_goal_response_; });
          }
          return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        },
        [this](const std::shared_ptr<ServerGoalHandleFJT>) {
          cancel_requests_.fetch_add(1, std::memory_order_relaxed);
          return rclcpp_action::CancelResponse::ACCEPT;
        },
        [this](const std::shared_ptr<ServerGoalHandleFJT> goal_handle) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            goal_handle_ = goal_handle;
            ++goal_count_;
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
    release_goal_response();
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

  bool wait_for_goal() const { return wait_for_goal_count(1); }

  bool wait_for_goal_count(std::uint64_t expected) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 2s,
                        [this, expected]() { return goal_count_ >= expected; });
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

  bool cancel_current_goal() {
    std::shared_ptr<ServerGoalHandleFJT> goal_handle;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal_handle = goal_handle_;
    }
    if (!goal_handle) {
      return false;
    }
    auto result = std::make_shared<FollowJointTrajectory::Result>();
    goal_handle->canceled(result);
    return true;
  }

  std::uint64_t goal_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return goal_count_;
  }

  std::uint64_t cancel_requests() const {
    return cancel_requests_.load(std::memory_order_relaxed);
  }

  bool has_goal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return goal_handle_ != nullptr;
  }

  void block_goal_response() {
    std::lock_guard<std::mutex> lock(goal_response_mutex_);
    block_goal_response_ = true;
    goal_response_entered_ = false;
    release_goal_response_ = false;
  }

  bool wait_for_goal_response_entry() const {
    std::unique_lock<std::mutex> lock(goal_response_mutex_);
    return goal_response_cv_.wait_for(
        lock, 2s, [this]() { return goal_response_entered_; });
  }

  void release_goal_response() {
    {
      std::lock_guard<std::mutex> lock(goal_response_mutex_);
      release_goal_response_ = true;
      block_goal_response_ = false;
    }
    goal_response_cv_.notify_all();
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
  std::uint64_t goal_count_ = 0;
  std::atomic<std::uint64_t> cancel_requests_{0};
  mutable std::mutex goal_response_mutex_;
  mutable std::condition_variable goal_response_cv_;
  bool block_goal_response_ = false;
  mutable bool goal_response_entered_ = false;
  bool release_goal_response_ = false;
};

class FakeControllerManager {
public:
  FakeControllerManager(std::string controller_manager,
                        ControllerBindingContract contract,
                        bool publish_initial_activity = true)
      : controller_manager_(std::move(controller_manager)),
        contract_(std::move(contract)) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);

    rclcpp::NodeOptions options;
    options.context(context_);
    node_ = std::make_shared<rclcpp::Node>(
        "manyforge_controller_manager_test_" + unique_suffix(), options);
    activity_publisher_ = node_->create_publisher<
        controller_manager_msgs::msg::ControllerManagerActivity>(
        contract_.activity_topic, rclcpp::QoS(1).reliable().transient_local());
    list_controllers_service_ =
        node_->create_service<controller_manager_msgs::srv::ListControllers>(
            controller_manager_ + "/list_controllers",
            [this](const std::shared_ptr<
                       controller_manager_msgs::srv::ListControllers::Request>,
                   std::shared_ptr<
                       controller_manager_msgs::srv::ListControllers::Response>
                       response) { fill_controllers(*response); });
    list_hardware_service_ = node_->create_service<
        controller_manager_msgs::srv::ListHardwareComponents>(
        controller_manager_ + "/list_hardware_components",
        [this](
            const std::shared_ptr<
                controller_manager_msgs::srv::ListHardwareComponents::Request>,
            std::shared_ptr<
                controller_manager_msgs::srv::ListHardwareComponents::Response>
                response) { fill_hardware(*response); });

    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context_;
    executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>(
        executor_options);
    executor_->add_node(node_);
    spin_thread_ = std::thread([this]() { executor_->spin(); });
    if (publish_initial_activity) {
      publish_activity();
    }
  }

  ~FakeControllerManager() {
    if (executor_) {
      executor_->cancel();
    }
    if (spin_thread_.joinable()) {
      spin_thread_.join();
    }
    if (executor_ && node_) {
      executor_->remove_node(node_);
    }
    list_controllers_service_.reset();
    list_hardware_service_.reset();
    activity_publisher_.reset();
    node_.reset();
    executor_.reset();
    if (context_) {
      context_->shutdown("test shutdown");
      context_.reset();
    }
  }

  void set_controller_active(bool active) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      controller_active_ = active;
    }
    publish_activity();
  }

  void set_conflicting_claim(bool conflicting) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      conflicting_claim_ = conflicting;
    }
    publish_activity();
  }

  bool wait_for_activity_subscription() const {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (activity_publisher_->get_subscription_count() > 0) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return activity_publisher_->get_subscription_count() > 0;
  }

  void announce_activity() { publish_activity(); }

private:
  void fill_controllers(
      controller_manager_msgs::srv::ListControllers::Response &response) {
    std::lock_guard<std::mutex> lock(mutex_);
    controller_manager_msgs::msg::ControllerState selected;
    selected.name = contract_.trajectory_controller_manager_name;
    selected.state = controller_active_ ? "active" : "inactive";
    selected.type = contract_.trajectory_controller_type;
    selected.claimed_interfaces = contract_.claimed_command_interfaces;
    response.controller.push_back(std::move(selected));
    if (conflicting_claim_) {
      controller_manager_msgs::msg::ControllerState conflict;
      conflict.name = "conflicting_controller";
      conflict.state = "active";
      conflict.type = "test/ConflictingController";
      conflict.claimed_interfaces = {
          contract_.claimed_command_interfaces.front()};
      response.controller.push_back(std::move(conflict));
    }
  }

  void
  fill_hardware(controller_manager_msgs::srv::ListHardwareComponents::Response
                    &response) {
    std::lock_guard<std::mutex> lock(mutex_);
    controller_manager_msgs::msg::HardwareComponentState component;
    component.name = contract_.hardware_component_name;
    component.type = "system";
    component.state.id = lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
    component.state.label = "active";
    for (const auto &name : contract_.claimed_command_interfaces) {
      controller_manager_msgs::msg::HardwareInterface interface;
      interface.name = name;
      interface.is_available = true;
      interface.is_claimed = true;
      component.command_interfaces.push_back(std::move(interface));
    }
    for (const auto &name : contract_.required_state_interfaces) {
      controller_manager_msgs::msg::HardwareInterface interface;
      interface.name = name;
      interface.is_available = true;
      component.state_interfaces.push_back(std::move(interface));
    }
    response.component.push_back(std::move(component));
  }

  void publish_activity() {
    controller_manager_msgs::msg::ControllerManagerActivity activity;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      controller_manager_msgs::msg::NamedLifecycleState controller;
      controller.name = contract_.trajectory_controller_manager_name;
      controller.state.id =
          controller_active_
              ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
              : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
      controller.state.label = controller_active_ ? "active" : "inactive";
      activity.controllers.push_back(std::move(controller));
      controller_manager_msgs::msg::NamedLifecycleState hardware;
      hardware.name = contract_.hardware_component_name;
      hardware.state.id = lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
      hardware.state.label = "active";
      activity.hardware_components.push_back(std::move(hardware));
    }
    activity_publisher_->publish(activity);
  }

  std::string controller_manager_;
  ControllerBindingContract contract_;
  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<controller_manager_msgs::msg::ControllerManagerActivity>::
      SharedPtr activity_publisher_;
  rclcpp::Service<controller_manager_msgs::srv::ListControllers>::SharedPtr
      list_controllers_service_;
  rclcpp::Service<controller_manager_msgs::srv::ListHardwareComponents>::
      SharedPtr list_hardware_service_;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::thread spin_thread_;
  mutable std::mutex mutex_;
  bool controller_active_ = true;
  bool conflicting_claim_ = false;
};

template <typename Predicate>
std::optional<ControllerBindingSnapshot>
wait_for_binding_snapshot(const Ros2ControlExecutionBackend &backend,
                          Predicate predicate,
                          std::chrono::milliseconds timeout = 4s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto snapshot = backend.controller_binding_snapshot();
    if (snapshot.has_value() && predicate(*snapshot)) {
      return snapshot;
    }
    std::this_thread::sleep_for(10ms);
  }
  return std::nullopt;
}

bool has_reason(const ControllerBindingSnapshot &snapshot,
                const std::string &reason) {
  return std::find(snapshot.reason_codes.begin(), snapshot.reason_codes.end(),
                   reason) != snapshot.reason_codes.end();
}

TEST(Ros2ControlExecutionBackendTest, RejectsNullExecutionStateSink) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  EXPECT_THROW(Ros2ControlExecutionBackend(nullptr, "arm", clock, scheduler,
                                           base_config()),
               std::invalid_argument);
}

TEST(Ros2ControlExecutionBackendTest,
     InjectedAddressedPublicationIsTheSoleCommandedStateChannel) {
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.25};
  core::ObservedRobotStatePublication publication(
      config.required_joints, ros2_control_addressed_state_sources(config));
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  clock.set(123'000'000);
  core::ManualScheduler scheduler;

  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, publication, sink,
      "arm", clock, scheduler, config);

  EXPECT_EQ(backend.observed_robot_state_publication(), &publication);
  const auto snapshot = publication.snapshot(clock.now());
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->source_kind,
            core::ObservedRobotStateSourceKind::kCommandedState);
  EXPECT_EQ(snapshot->source_id, "commanded_state");
  EXPECT_EQ(snapshot->authority_profile,
            "ros2_control_commanded_sim_v1");
  ASSERT_EQ(snapshot->positions_rad.size(), 1U);
  EXPECT_DOUBLE_EQ(snapshot->positions_rad.front(), 0.25);

  backend.testing_store_latest_joint_sample(
      config.required_joints, {9.0},
      std::chrono::steady_clock::time_point(std::chrono::milliseconds(124)));
  const auto after_feedback = publication.snapshot(clock.now());
  ASSERT_TRUE(after_feedback.has_value());
  ASSERT_EQ(after_feedback->positions_rad.size(), 1U);
  EXPECT_DOUBLE_EQ(after_feedback->positions_rad.front(), 0.25);
  const auto metrics = backend.controller_progress_metrics();
  EXPECT_EQ(metrics.observed_state_published_total, 2U);
  ASSERT_EQ(metrics.observed_state_sources.size(), 4U);
  EXPECT_EQ(metrics.observed_state_sources.front().source_kind,
            core::ObservedRobotStateSourceKind::kCommandedState);
  EXPECT_EQ(metrics.observed_state_sources[2].source_kind,
            core::ObservedRobotStateSourceKind::kJointState);
  EXPECT_EQ(metrics.observed_state_sources[2].published_total, 1U);
}

TEST(Ros2ControlExecutionBackendTest,
     InjectedAddressedPublicationRejectsAnySourceOrJointMismatch) {
  auto config = base_config();
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  core::ObservedRobotStatePublication wrong_joints(
      {"different_joint"}, ros2_control_addressed_state_sources(config));
  EXPECT_THROW(
      Ros2ControlExecutionBackend(
          Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, wrong_joints,
          sink, "arm", clock, scheduler, config),
      std::invalid_argument);

  auto wrong_sources = ros2_control_addressed_state_sources(config);
  wrong_sources.front().source_id = "different_source";
  core::ObservedRobotStatePublication wrong_source(config.required_joints,
                                                    wrong_sources);
  EXPECT_THROW(
      Ros2ControlExecutionBackend(
          Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, wrong_source,
          sink, "arm", clock, scheduler, config),
      std::invalid_argument);
}

TEST(Ros2ControlExecutionBackendTest,
     DirectStageFailsBeforeEffectWhenPreparedGoalMembershipIsUnavailable) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());
  const auto stage = direct_stage_fixture();
  const core::RemainingStopBudgetDTO budget{
      stage.stop_id, stage.stop_generation, 1'000'000'000, {}};

  const auto result =
      backend.TranslateStopStage(stage, budget, std::stop_token{});

  EXPECT_EQ(result.request.stop_id, stage.stop_id);
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "resolved_stop_stage_dispatch_membership_unavailable");
  EXPECT_EQ(backend.get_execution_snapshot().state,
            core::ExecutionState::kIdle);
}

TEST(Ros2ControlExecutionBackendTest,
     ControlledStopRejectsWrongCoordinateAndInexactJointScopeBeforeEffect) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

  core::MotionInterruptStopProfileDTO wrong_settle;
  wrong_settle.profile_id = "hard.profile";
  wrong_settle.stop_mode = core::MotionInterruptStopMode::kHardDeceleration;
  wrong_settle.per_joint_settle_velocity_threshold =
      std::map<std::string, core::CoordinateVelocityDTO>{
          {"joint_1", core::LinearVelocityDTO{0.01}}};
  const auto rejected_settle = backend.execute_controlled_stop(wrong_settle);
  ASSERT_TRUE(rejected_settle.error.has_value());
  EXPECT_EQ(rejected_settle.error->details.at("reason"),
            "settle_velocity_threshold_coordinate_kind_mismatch");

  core::MotionInterruptStopProfileDTO wrong_scope;
  wrong_scope.profile_id = "soft.profile";
  wrong_scope.stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
  wrong_scope.deceleration_time_s = 0.5;
  wrong_scope.per_joint_max_deceleration = {
      {"different_joint", core::AngularAccelerationDTO{1.0}}};
  const auto rejected_scope = backend.execute_controlled_stop(wrong_scope);
  ASSERT_TRUE(rejected_scope.error.has_value());
  EXPECT_EQ(rejected_scope.error->details.at("reason"),
            "stop_acceleration_scope_coverage_mismatch");
}

TEST(Ros2ControlExecutionBackendTest,
     CurrentSpringStopProfilePassesAdapterShapeGateBeforeStateRead) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

  core::MotionInterruptStopProfileDTO profile;
  profile.profile_id = "spring.profile";
  profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  profile.per_joint_max_deceleration = {
      {"joint_1", core::AngularAccelerationDTO{1.0}}};
  const auto result = backend.execute_controlled_stop(profile);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "controlled_stop_no_observed_state");
}

TEST(Ros2ControlExecutionBackendTest,
     MissingControllerManagerPublishesInvalidBinding) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  auto config = base_config();
  config.controller_state_topic =
      "/manyforge_missing_manager_controller_state_" + unique_suffix();
  config.controller_binding =
      binding_contract_for(config, "/manyforge_missing_manager_" +
                                       unique_suffix() + "/controller_manager");
  config.controller_binding_refresh_timeout_s = 0.05;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  const auto snapshot = wait_for_binding_snapshot(
      backend, [](const ControllerBindingSnapshot &candidate) {
        return !candidate.valid &&
               has_reason(candidate, "controller_list_service_unavailable");
      });
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_FALSE(snapshot->valid);
  EXPECT_GT(backend.controller_progress_metrics()
                .controller_binding_refresh_failures_total,
            0u);
}

TEST(Ros2ControlExecutionBackendTest,
     ControllerManagerServicesCannotBypassInitialActivityFence) {
  const std::string suffix = unique_suffix();
  const std::string controller_manager =
      "/manyforge_binding_activity_fence_" + suffix + "/controller_manager";
  auto config = base_config();
  config.action_name = "/manyforge_binding_activity_fence_" + suffix +
                       "/follow_joint_trajectory";
  config.joint_state_topic =
      "/manyforge_binding_activity_fence_" + suffix + "/joint_states";
  config.controller_state_topic =
      "/manyforge_binding_activity_fence_" + suffix + "/controller_state";
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeControllerManager manager(controller_manager, *config.controller_binding,
                                /*publish_initial_activity=*/false);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);
  ASSERT_TRUE(manager.wait_for_activity_subscription());

  const auto fenced = wait_for_binding_snapshot(
      backend, [](const ControllerBindingSnapshot &candidate) {
        return !candidate.valid &&
               has_reason(candidate, "controller_activity_unobserved");
      });
  ASSERT_TRUE(fenced.has_value());

  manager.announce_activity();
  const auto admitted = wait_for_binding_snapshot(
      backend, [](const ControllerBindingSnapshot &candidate) {
        return candidate.activity_generation > 0 && candidate.valid;
      });
  ASSERT_TRUE(admitted.has_value());
}

TEST(Ros2ControlExecutionBackendTest,
     ControllerManagerMonitorRejectsConflictAndRestartChurn) {
  const std::string suffix = unique_suffix();
  const std::string controller_manager =
      "/manyforge_binding_matrix_" + suffix + "/controller_manager";
  auto config = base_config();
  config.action_name =
      "/manyforge_binding_matrix_" + suffix + "/follow_joint_trajectory";
  config.joint_state_topic =
      "/manyforge_binding_matrix_" + suffix + "/joint_states";
  config.controller_state_topic =
      "/manyforge_binding_matrix_" + suffix + "/controller_state";
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeControllerManager manager(controller_manager, *config.controller_binding);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);
  ASSERT_TRUE(manager.wait_for_activity_subscription());

  const auto initial = wait_for_binding_snapshot(
      backend, [](const ControllerBindingSnapshot &candidate) {
        return candidate.valid;
      });
  ASSERT_TRUE(initial.has_value());

  manager.set_conflicting_claim(true);
  const auto conflict = wait_for_binding_snapshot(
      backend, [initial](const ControllerBindingSnapshot &candidate) {
        return candidate.activity_generation > initial->activity_generation &&
               !candidate.valid &&
               has_reason(candidate, "trajectory_controller_claim_conflict");
      });
  ASSERT_TRUE(conflict.has_value());
  EXPECT_FALSE(conflict->conflicting_claims_absent);

  manager.set_conflicting_claim(false);
  const auto recovered = wait_for_binding_snapshot(
      backend, [conflict](const ControllerBindingSnapshot &candidate) {
        return candidate.activity_generation > conflict->activity_generation &&
               candidate.valid;
      });
  ASSERT_TRUE(recovered.has_value());

  manager.set_controller_active(false);
  const auto inactive = wait_for_binding_snapshot(
      backend, [recovered](const ControllerBindingSnapshot &candidate) {
        return candidate.activity_generation > recovered->activity_generation &&
               !candidate.valid &&
               has_reason(candidate, "trajectory_controller_inactive");
      });
  ASSERT_TRUE(inactive.has_value());
  EXPECT_FALSE(inactive->controller_active);

  manager.set_controller_active(true);
  const auto restarted = wait_for_binding_snapshot(
      backend, [inactive](const ControllerBindingSnapshot &candidate) {
        return candidate.activity_generation > inactive->activity_generation &&
               candidate.valid;
      });
  ASSERT_TRUE(restarted.has_value());
  EXPECT_TRUE(restarted->controller_active);
}

TEST(Ros2ControlExecutionBackendTest,
     AdmissionPublicationIsMonotonicAndIdempotent) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000);
  auto config = base_config();
  config.controller_state_topic =
      "/manyforge_admission_controller_state_" + unique_suffix();
  config.controller_binding =
      binding_contract_for(config, "/manyforge_admission/controller_manager");
  config.require_execution_admission_snapshot = true;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  ExecutionAdmissionSnapshot admission;
  admission.generation = 4;
  admission.lifecycle_generation = 2;
  admission.robot_id = "lite6-a";
  admission.binding_contract_generation_id =
      config.controller_binding->generation_id;
  admission.runtime_generation_id = "runtime-1";
  admission.provider_component_epoch = "provider-epoch-1";
  admission.transport_owner_component_epoch = "owner-epoch-1";
  admission.authority_component_epoch = "authority-epoch-1";
  admission.cell_supervisor_component_epoch = "supervisor-epoch-1";
  admission.observed_timestamp_ns = clock.now();
  admission.received_timestamp_ns = clock.now();
  admission.fresh_until_ns = clock.now() + 10'000;
  admission.controller_ready = true;
  admission.motion_admitted = true;

  auto malformed = admission;
  malformed.robot_id.clear();
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(malformed));
  malformed = admission;
  malformed.observed_timestamp_ns = malformed.received_timestamp_ns + 1;
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(malformed));
  malformed = admission;
  malformed.controller_ready = false;
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(malformed));
  malformed = admission;
  malformed.reason_codes = {"duplicate", "duplicate"};
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(malformed));
  malformed = admission;
  malformed.binding_contract_generation_id = "another-robot-binding";
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(malformed));

  EXPECT_TRUE(backend.publish_execution_admission_snapshot(admission));
  EXPECT_TRUE(backend.publish_execution_admission_snapshot(admission))
      << "an exact retry is idempotent";

  auto conflicting = admission;
  conflicting.motion_admitted = false;
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(conflicting))
      << "one generation cannot describe two lifecycle states";

  auto older = admission;
  older.generation -= 1;
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(older));

  auto semantic_change_without_lifecycle_generation = admission;
  semantic_change_without_lifecycle_generation.generation += 1;
  semantic_change_without_lifecycle_generation.received_timestamp_ns += 1;
  semantic_change_without_lifecycle_generation.fresh_until_ns += 1;
  semantic_change_without_lifecycle_generation.motion_admitted = false;
  semantic_change_without_lifecycle_generation.reason_codes = {
      "operator_gate_closed"};
  EXPECT_FALSE(backend.publish_execution_admission_snapshot(
      semantic_change_without_lifecycle_generation));

  auto refreshed = admission;
  refreshed.generation += 1;
  refreshed.observed_timestamp_ns += 1;
  refreshed.received_timestamp_ns += 1;
  refreshed.fresh_until_ns += 1;
  EXPECT_TRUE(backend.publish_execution_admission_snapshot(refreshed))
      << "one lifecycle generation may renew its observation lease";

  auto regressed_lifecycle = refreshed;
  regressed_lifecycle.generation += 1;
  regressed_lifecycle.lifecycle_generation -= 1;
  regressed_lifecycle.observed_timestamp_ns += 1;
  regressed_lifecycle.received_timestamp_ns += 1;
  regressed_lifecycle.fresh_until_ns += 1;
  EXPECT_FALSE(
      backend.publish_execution_admission_snapshot(regressed_lifecycle));

  auto newer = refreshed;
  newer.generation += 1;
  newer.lifecycle_generation += 1;
  newer.observed_timestamp_ns += 1;
  newer.received_timestamp_ns += 1;
  newer.fresh_until_ns += 1;
  newer.motion_admitted = false;
  newer.reason_codes = {"operator_gate_closed"};
  EXPECT_TRUE(backend.publish_execution_admission_snapshot(newer));

  const auto observed = backend.execution_admission_snapshot();
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->generation, newer.generation);
  EXPECT_EQ(observed->lifecycle_generation, newer.lifecycle_generation);
  EXPECT_FALSE(observed->motion_admitted);
  EXPECT_EQ(observed->reason_codes,
            (std::vector<std::string>{"operator_gate_closed"}));
}

TEST(Ros2ControlExecutionBackendTest,
     MissingAdmissionRejectsBeforeFjtDispatch) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_binding_reject_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_binding_reject_joint_states_" + suffix;
  const std::string controller_state_topic =
      "/manyforge_binding_reject_controller_state_" + suffix;
  const std::string controller_manager =
      "/manyforge_binding_reject_" + suffix + "/controller_manager";

  FakeFjtServer server(action_name);
  FakeControllerManager manager(controller_manager, [&]() {
    auto provisional = base_config();
    provisional.action_name = action_name;
    provisional.joint_state_topic = joint_state_topic;
    provisional.controller_state_topic = controller_state_topic;
    return binding_contract_for(provisional, controller_manager);
  }());
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.controller_state_topic = controller_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.require_execution_admission_snapshot = true;
  config.controller_binding_refresh_timeout_s = 1.0;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(manager.wait_for_activity_subscription());
  const auto binding_deadline = std::chrono::steady_clock::now() + 4s;
  while (std::chrono::steady_clock::now() < binding_deadline) {
    const auto binding = backend.controller_binding_snapshot();
    if (binding.has_value() && binding->valid) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  const auto binding = backend.controller_binding_snapshot();
  ASSERT_TRUE(binding.has_value());
  ASSERT_TRUE(binding->valid);

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool preplanning_rejection_observed = false;
  const auto sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    const auto sync = backend.sync_observed_state_before_planning();
    if (!sync.ok() && sync.error().details.contains("reason") &&
        sync.error().details.at("reason") ==
            "execution_admission_unavailable") {
      preplanning_rejection_observed = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(preplanning_rejection_observed);
  EXPECT_FALSE(backend.testing_planning_environment_pin().has_value());

  const auto handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 0.1));
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000));
  const auto snapshot = handle.get_snapshot();
  EXPECT_EQ(snapshot.state, core::ExecutionState::kFailed);
  ASSERT_TRUE(snapshot.error.has_value());
  EXPECT_EQ(snapshot.error->details.at("reason_code"),
            ros2_control_fault_reason::kExecutionAdmissionRejected);
  EXPECT_EQ(snapshot.error->details.at("admission_failure_reason"),
            "execution_admission_unavailable");
  EXPECT_FALSE(server.has_goal());
  EXPECT_EQ(backend.controller_progress_metrics()
                .execution_admission_dispatch_rejections_total,
            1u);
}

TEST(Ros2ControlExecutionBackendTest,
     ComponentEpochChangeBetweenPlanningAndDispatchRejectsWithoutFjtGoal) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_environment_change_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_environment_change_joint_states_" + suffix;
  const std::string controller_state_topic =
      "/manyforge_environment_change_controller_state_" + suffix;
  const std::string controller_manager =
      "/manyforge_environment_change_" + suffix + "/controller_manager";

  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.controller_state_topic = controller_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.require_execution_admission_snapshot = true;
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeFjtServer server(action_name);
  FakeControllerManager manager(controller_manager, *config.controller_binding);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(manager.wait_for_activity_subscription());
  ASSERT_TRUE(
      wait_for_binding_snapshot(backend,
                                [](const ControllerBindingSnapshot &candidate) {
                                  return candidate.valid;
                                })
          .has_value());

  auto admission =
      admitted_snapshot_for(*config.controller_binding, clock.now());
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(admission));

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool environment_pinned = false;
  const auto sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
      environment_pinned = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(environment_pinned);
  ASSERT_TRUE(backend.testing_planning_environment_pin().has_value());

  admission.generation += 1;
  admission.lifecycle_generation += 1;
  admission.observed_timestamp_ns += 1;
  admission.received_timestamp_ns += 1;
  admission.fresh_until_ns += 1;
  admission.transport_owner_component_epoch = "owner-epoch-2";
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(admission));

  const auto handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 0.1));
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000));
  const auto terminal = handle.get_snapshot();
  EXPECT_EQ(terminal.state, core::ExecutionState::kFailed);
  ASSERT_TRUE(terminal.error.has_value());
  EXPECT_EQ(terminal.error->details.at("reason_code"),
            ros2_control_fault_reason::kExecutionAdmissionRejected);
  EXPECT_EQ(terminal.error->details.at("phase"),
            "planning_to_dispatch_environment_pin");
  EXPECT_EQ(terminal.error->details.at("reason"),
            "execution_environment_epoch_changed");
  EXPECT_EQ(server.goal_count(), 0u);
}

TEST(Ros2ControlExecutionBackendTest,
     ActivityChangeCancelsActiveFjtAndFailsClosed) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_binding_loss_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_binding_loss_joint_states_" + suffix;
  const std::string controller_state_topic =
      "/manyforge_binding_loss_controller_state_" + suffix;
  const std::string controller_manager =
      "/manyforge_binding_loss_" + suffix + "/controller_manager";

  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.controller_state_topic = controller_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.require_execution_admission_snapshot = true;
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeFjtServer server(action_name);
  FakeControllerManager manager(controller_manager, *config.controller_binding);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(manager.wait_for_activity_subscription());
  const auto binding_deadline = std::chrono::steady_clock::now() + 4s;
  while (std::chrono::steady_clock::now() < binding_deadline) {
    const auto binding = backend.controller_binding_snapshot();
    if (binding.has_value() && binding->valid) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  const auto binding = backend.controller_binding_snapshot();
  ASSERT_TRUE(binding.has_value());
  ASSERT_TRUE(binding->valid);

  auto admission =
      admitted_snapshot_for(*config.controller_binding, clock.now());
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(admission));

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool initial_sample_observed = false;
  const auto sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
      initial_sample_observed = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(initial_sample_observed);

  const auto handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 1.0));
  ASSERT_NE(handle.execution_generation, 0u);
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(server.publish_feedback(0.0));

  manager.set_controller_active(false);
  std::optional<core::ExecutionSnapshot> terminal;
  const auto terminal_deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < terminal_deadline) {
    const auto snapshot = handle.get_snapshot();
    if (snapshot.state == core::ExecutionState::kFailed) {
      terminal = snapshot;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(terminal.has_value());
  ASSERT_TRUE(terminal->error.has_value());
  EXPECT_EQ(terminal->error->details.at("reason_code"),
            ros2_control_fault_reason::kControllerBindingLost);
  EXPECT_EQ(terminal->error->details.at("admission_failure_reason"),
            "controller_binding_invalid");
  const auto cancellation_deadline = std::chrono::steady_clock::now() + 2s;
  while (server.cancel_requests() == 0 &&
         std::chrono::steady_clock::now() < cancellation_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GT(server.cancel_requests(), 0u);
  EXPECT_EQ(
      backend.controller_progress_metrics().dispatch_authority_losses_total,
      1u);
}

TEST(Ros2ControlExecutionBackendTest,
     LifecycleAdmissionLossCancelsActiveFjtAndFailsClosed) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_admission_loss_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_admission_loss_joint_states_" + suffix;
  const std::string controller_state_topic =
      "/manyforge_admission_loss_controller_state_" + suffix;
  const std::string controller_manager =
      "/manyforge_admission_loss_" + suffix + "/controller_manager";

  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.controller_state_topic = controller_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.require_execution_admission_snapshot = true;
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeFjtServer server(action_name);
  FakeControllerManager manager(controller_manager, *config.controller_binding);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(manager.wait_for_activity_subscription());
  ASSERT_TRUE(
      wait_for_binding_snapshot(backend,
                                [](const ControllerBindingSnapshot &candidate) {
                                  return candidate.valid;
                                })
          .has_value());
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(
      admitted_snapshot_for(*config.controller_binding, clock.now())));

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool initial_sample_observed = false;
  const auto sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
      initial_sample_observed = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(initial_sample_observed);

  const auto handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 1.0));
  ASSERT_NE(handle.execution_generation, 0u);
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(server.publish_feedback(0.0));

  auto revoked =
      admitted_snapshot_for(*config.controller_binding, clock.now(),
                            /*generation=*/2, /*lifecycle_generation=*/2);
  revoked.controller_ready = false;
  revoked.motion_admitted = false;
  revoked.reason_codes = {"operator_gate_closed"};
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(revoked));

  std::optional<core::ExecutionSnapshot> terminal;
  const auto terminal_deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < terminal_deadline) {
    const auto snapshot = handle.get_snapshot();
    if (snapshot.state == core::ExecutionState::kFailed) {
      terminal = snapshot;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(terminal.has_value());
  ASSERT_TRUE(terminal->error.has_value());
  EXPECT_EQ(terminal->error->details.at("reason_code"),
            ros2_control_fault_reason::kExecutionAdmissionLost);
  EXPECT_EQ(terminal->error->details.at("admission_failure_reason"),
            "execution_controller_not_ready");
  const auto cancellation_deadline = std::chrono::steady_clock::now() + 2s;
  while (server.cancel_requests() == 0 &&
         std::chrono::steady_clock::now() < cancellation_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GT(server.cancel_requests(), 0u);
}

TEST(Ros2ControlExecutionBackendTest,
     LifecycleGenerationChangeCannotCommitSuccessfulTerminalResult) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_terminal_authority_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_terminal_authority_joint_states_" + suffix;
  const std::string controller_state_topic =
      "/manyforge_terminal_authority_controller_state_" + suffix;
  const std::string controller_manager =
      "/manyforge_terminal_authority_" + suffix + "/controller_manager";

  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.controller_state_topic = controller_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  // Keep the periodic monitor asleep so this case exercises the terminal
  // result boundary itself rather than relying on the next monitor tick.
  config.monitor_period_s = 5.0;
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.require_execution_admission_snapshot = true;
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeFjtServer server(action_name);
  FakeControllerManager manager(controller_manager, *config.controller_binding);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(manager.wait_for_activity_subscription());
  ASSERT_TRUE(
      wait_for_binding_snapshot(backend,
                                [](const ControllerBindingSnapshot &candidate) {
                                  return candidate.valid;
                                })
          .has_value());
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(
      admitted_snapshot_for(*config.controller_binding, clock.now())));

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool initial_sample_observed = false;
  const auto sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
      initial_sample_observed = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(initial_sample_observed);

  const auto handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 1.0));
  ASSERT_NE(handle.execution_generation, 0u);
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(server.publish_feedback(0.0));

  auto changed =
      admitted_snapshot_for(*config.controller_binding, clock.now(),
                            /*generation=*/2, /*lifecycle_generation=*/2);
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(changed));
  ASSERT_TRUE(server.succeed_current_goal());
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000));

  const auto terminal = handle.get_snapshot();
  EXPECT_EQ(terminal.state, core::ExecutionState::kFailed);
  ASSERT_TRUE(terminal.error.has_value());
  EXPECT_EQ(terminal.error->details.at("reason_code"),
            ros2_control_fault_reason::kExecutionAdmissionLost);
  EXPECT_EQ(terminal.error->details.at("admission_failure_reason"),
            "execution_admission_generation_changed");
  ASSERT_TRUE(terminal.error->details.contains("phase"));
  const auto &phase = terminal.error->details.at("phase");
  EXPECT_TRUE(phase == "active_goal_authority" ||
              phase == "terminal_result_authority");
}

TEST(Ros2ControlExecutionBackendTest,
     ResumeCannotInheritPausedExecutionAcrossComponentEpochChange) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_resume_environment_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_resume_environment_joint_states_" + suffix;
  const std::string controller_state_topic =
      "/manyforge_resume_environment_controller_state_" + suffix;
  const std::string controller_manager =
      "/manyforge_resume_environment_" + suffix + "/controller_manager";

  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.controller_state_topic = controller_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.require_execution_admission_snapshot = true;
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeFjtServer server(action_name);
  FakeControllerManager manager(controller_manager, *config.controller_binding);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(manager.wait_for_activity_subscription());
  ASSERT_TRUE(
      wait_for_binding_snapshot(backend,
                                [](const ControllerBindingSnapshot &candidate) {
                                  return candidate.valid;
                                })
          .has_value());

  auto admission =
      admitted_snapshot_for(*config.controller_binding, clock.now());
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(admission));
  ASSERT_TRUE(publisher.wait_for_subscription());
  bool initial_environment_pinned = false;
  const auto initial_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < initial_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
      initial_environment_pinned = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(initial_environment_pinned);

  const auto initial_handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 1.0));
  ASSERT_TRUE(server.wait_for_goal_count(1));
  ASSERT_TRUE(server.publish_feedback(0.0));
  const auto pause = backend.pause();
  ASSERT_TRUE(pause.accepted)
      << (pause.error.has_value() ? pause.error->message : "");
  const auto cancel_deadline = std::chrono::steady_clock::now() + 2s;
  while (server.cancel_requests() == 0 &&
         std::chrono::steady_clock::now() < cancel_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_GT(server.cancel_requests(), 0u);
  ASSERT_TRUE(server.cancel_current_goal());
  ASSERT_TRUE(initial_handle.wait_for_completion(2'000'000'000));
  EXPECT_EQ(initial_handle.get_snapshot().state, core::ExecutionState::kPaused);
  const auto paused_pin = backend.testing_resumable_environment_pin();
  ASSERT_TRUE(paused_pin.has_value());
  EXPECT_EQ(paused_pin->transport_owner_component_epoch, "owner-epoch-1");

  admission.generation += 1;
  admission.lifecycle_generation += 1;
  admission.observed_timestamp_ns += 1;
  admission.received_timestamp_ns += 1;
  admission.fresh_until_ns += 1;
  admission.transport_owner_component_epoch = "owner-epoch-2";
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(admission));

  bool replacement_environment_pinned = false;
  const auto replacement_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < replacement_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
      replacement_environment_pinned = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(replacement_environment_pinned);
  const auto provisional_pin = backend.testing_planning_environment_pin();
  ASSERT_TRUE(provisional_pin.has_value());
  EXPECT_EQ(provisional_pin->transport_owner_component_epoch, "owner-epoch-2");

  const auto resumed_handle =
      backend.resume(make_single_joint_trajectory(0.0, 0.1));
  ASSERT_TRUE(resumed_handle.wait_for_completion(2'000'000'000));
  const auto terminal = resumed_handle.get_snapshot();
  EXPECT_EQ(terminal.state, core::ExecutionState::kFailed);
  ASSERT_TRUE(terminal.error.has_value());
  EXPECT_EQ(terminal.error->details.at("reason_code"),
            ros2_control_fault_reason::kExecutionAdmissionRejected);
  EXPECT_EQ(terminal.error->details.at("reason"),
            "execution_environment_epoch_changed");
  EXPECT_EQ(server.goal_count(), 1u);
  EXPECT_FALSE(backend.testing_resumable_environment_pin().has_value());
}

TEST(Ros2ControlExecutionBackendTest,
     ActivityChangeDuringGoalResponseCannotEscapeGenerationFence) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_binding_goal_race_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_binding_goal_race_joint_states_" + suffix;
  const std::string controller_state_topic =
      "/manyforge_binding_goal_race_controller_state_" + suffix;
  const std::string controller_manager =
      "/manyforge_binding_goal_race_" + suffix + "/controller_manager";

  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.controller_state_topic = controller_state_topic;
  config.wait_for_state_timeout_s = 1.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  config.controller_binding = binding_contract_for(config, controller_manager);
  config.require_execution_admission_snapshot = true;
  config.controller_binding_refresh_timeout_s = 1.0;

  FakeFjtServer server(action_name);
  server.block_goal_response();
  FakeControllerManager manager(controller_manager, *config.controller_binding);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  clock.set(1'000'000'000);
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(manager.wait_for_activity_subscription());
  ASSERT_TRUE(
      wait_for_binding_snapshot(backend,
                                [](const ControllerBindingSnapshot &candidate) {
                                  return candidate.valid;
                                })
          .has_value());

  auto admission =
      admitted_snapshot_for(*config.controller_binding, clock.now());
  ASSERT_TRUE(backend.publish_execution_admission_snapshot(admission));

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool initial_sample_observed = false;
  const auto sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
      initial_sample_observed = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(initial_sample_observed);

  auto dispatch = std::async(std::launch::async, [&backend]() {
    return backend.execute_trajectory(make_single_joint_trajectory(0.0, 1.0));
  });
  ASSERT_TRUE(server.wait_for_goal_response_entry());
  manager.set_controller_active(false);
  ASSERT_TRUE(
      wait_for_binding_snapshot(backend, [](const ControllerBindingSnapshot
                                                &candidate) {
        return !candidate.valid &&
               (has_reason(candidate, "controller_activity_changed") ||
                has_reason(candidate, "trajectory_controller_inactive"));
      }).has_value());
  server.release_goal_response();

  ASSERT_EQ(dispatch.wait_for(3s), std::future_status::ready);
  const auto handle = dispatch.get();
  ASSERT_TRUE(handle.wait_for_completion(3'000'000'000));
  const auto terminal = handle.get_snapshot();
  EXPECT_EQ(terminal.state, core::ExecutionState::kFailed);
  ASSERT_TRUE(terminal.error.has_value());
  EXPECT_EQ(terminal.error->details.at("reason_code"),
            ros2_control_fault_reason::kControllerBindingLost);
  EXPECT_EQ(terminal.error->details.at("phase"), "goal_response_authority");
  const auto cancellation_deadline = std::chrono::steady_clock::now() + 2s;
  while (server.cancel_requests() == 0 &&
         std::chrono::steady_clock::now() < cancellation_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GT(server.cancel_requests(), 0u);
}

TEST(Ros2ControlExecutionBackendTest,
     RejectsExecutorThreadCountBelowCallbackGroupMinimum) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.executor_thread_count = 2;

  EXPECT_THROW(Ros2ControlExecutionBackend(
                   Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink,
                   "arm", clock, scheduler, config),
               std::invalid_argument);
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedModeRequiresPassivePostResultStateSource) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "observed_ros2";
  config.observed_state_source_precedence = {"action_feedback"};
  EXPECT_THROW((Ros2ControlExecutionBackend(
                   Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink,
                   "arm", clock, scheduler, config)),
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

  auto gripper = isaac_gripper_config("/manyforge_non_finite_start_gripper_" +
                                      unique_suffix());
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
      "tool", command.command_id, command.generation, "attached_drift_monitor",
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
      "tool",
      isaac_gripper_config("/manyforge_stale_grasp_loss_" + unique_suffix()));

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
      "tool",
      isaac_gripper_config("/manyforge_schedule_failure_" + unique_suffix()));

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
      "tool",
      isaac_gripper_config("/manyforge_autonomous_gripper_" + unique_suffix()));

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
      "tool", isaac_gripper_config("/manyforge_reset_fenced_gripper_" +
                                   unique_suffix()));

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);

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
  EXPECT_TRUE(
      backend
          .command_gripper(make_gripper_request(
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
      "tool", isaac_gripper_config("/manyforge_projection_retry_gripper_" +
                                   unique_suffix()));

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  scheduler.advance_to(clock.now());
  ASSERT_TRUE(sink->wait_for_single_update());
  auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_EQ(status.projection_state, core::GripperProjectionState::kPending);
  const auto while_pending = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_FALSE(while_pending.accepted);
  ASSERT_TRUE(while_pending.error.has_value());
  EXPECT_EQ(while_pending.error->code, ros2_control_fault_reason::kGripperBusy);

  sink->release_single_update();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(status.projection_state, core::GripperProjectionState::kApplied);
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

TEST(
    Ros2ControlExecutionBackendTest,
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
      "tool", isaac_gripper_config("/manyforge_projection_race_gripper_" +
                                   unique_suffix()));

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  auto callback = std::async(std::launch::async,
                             [&]() { scheduler.advance_to(clock.now()); });
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

  sink->block_group_update();
  backend.testing_schedule_active_scene_projection({0.25}, clock.now());
  ASSERT_TRUE(sink->wait_for_group_update());

  auto reset =
      std::async(std::launch::async, [&]() { return backend.reset(); });
  EXPECT_EQ(reset.wait_for(20ms), std::future_status::timeout)
      << "reset must not return while a pre-reset scene write is still active";
  backend.testing_schedule_active_scene_projection({0.5}, clock.now());

  sink->release_group_update();
  const auto result = reset.get();
  EXPECT_TRUE(result.accepted) << (result.error ? result.error->message : "");
  EXPECT_EQ(backend.get_execution_snapshot().state,
            core::ExecutionState::kIdle);
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  EXPECT_EQ(sink->group_updates().size(), 1u)
      << "a frame submitted after reset suspension must not land";
  EXPECT_EQ(backend.scene_projection_metrics()
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
      "tool",
      isaac_gripper_config("/manyforge_replaced_gripper_a_" + unique_suffix()));
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);

  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_replaced_gripper_b_" + unique_suffix()));
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
      "tool",
      isaac_gripper_config("/manyforge_prestart_gripper_" + unique_suffix()));

  backend.complete_ros_configuration();

  EXPECT_THROW(backend.configure_gripper_action(
                   "late", isaac_gripper_config("/manyforge_late_gripper_" +
                                                unique_suffix())),
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
      "tool", isaac_gripper_config("/manyforge_preflight_gripper_a_" +
                                   unique_suffix()));

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_gripper_b_" +
                                   unique_suffix()));
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
  EXPECT_TRUE(fresh.accepted) << (fresh.error ? fresh.error->message : "");
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
      "tool", isaac_gripper_config("/manyforge_preflight_invalidation_" +
                                   unique_suffix()));

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
      "tool", isaac_gripper_config("/manyforge_independent_gripper_a_" +
                                   unique_suffix()));

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "auxiliary", isaac_gripper_config("/manyforge_independent_gripper_b_" +
                                        unique_suffix()));
  sink->release_read();

  const auto admitted = in_flight.get();
  EXPECT_TRUE(admitted.accepted)
      << (admitted.error ? admitted.error->message : "");
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedStateSyncAppliesGroupPositionsThroughExecutionStateSink) {
  const std::string topic =
      "/manyforge_joint_state_projection_" + unique_suffix();

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
  for (const auto &update : updates) {
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
  const auto *execution_progress = backend.execution_progress_publication();
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
    ASSERT_TRUE(server.publish_feedback(0.0));
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
  const auto gate_deadline = std::chrono::steady_clock::now() + 250ms;
  while (!backend.testing_completion_gate_pending() &&
         std::chrono::steady_clock::now() < gate_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(backend.testing_completion_gate_pending())
      << "the action result callback did not return and hand completion to "
         "the NRT gate promptly";
  EXPECT_EQ(backend.get_execution_snapshot().state,
            core::ExecutionState::kExecuting);
  EXPECT_FALSE(handle.wait_for_completion(1'000'000));

  const auto completion_wait_started = std::chrono::steady_clock::now();
  std::optional<core::ExecutionSnapshot> terminal_snapshot;
  const auto deadline = completion_wait_started + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    publisher.publish({"joint_1"}, {0.1});
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
  const auto &snapshot = *terminal_snapshot;
  EXPECT_EQ(snapshot.state, core::ExecutionState::kCompleted);
  const auto terminal_progress = execution_progress->acquire();
  EXPECT_TRUE(terminal_progress.valid);
  EXPECT_TRUE(terminal_progress.terminal);
  EXPECT_EQ(terminal_progress.execution_generation,
            handle.execution_generation);
  backend.retire_execution_progress_generation(handle.execution_generation);
  EXPECT_FALSE(execution_progress->acquire().valid);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_NEAR(snapshot.joint_positions_rad.front(), 0.1, 1e-9);

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
     NaturalCompletionRejectsFreshMeasuredStateOutsideFinalTolerance) {
  const std::string suffix = unique_suffix();
  const std::string action_name = "/manyforge_final_state_gate_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_final_state_gate_joint_states_" + suffix;

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
  config.final_tolerance_rad = 0.01;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  config.executor_thread_count = 3;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool initial_sample_observed = false;
  const auto initial_sample_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < initial_sample_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    if (backend.sync_observed_state_before_planning().ok()) {
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

  const auto feedback_deadline = std::chrono::steady_clock::now() + 2s;
  while (
      backend.controller_progress_metrics().action_feedback_callbacks_total ==
          0u &&
      std::chrono::steady_clock::now() < feedback_deadline) {
    ASSERT_TRUE(server.publish_feedback(0.0));
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_GT(
      backend.controller_progress_metrics().action_feedback_callbacks_total,
      0u);

  ASSERT_TRUE(server.succeed_current_goal());
  std::optional<core::ExecutionSnapshot> terminal_snapshot;
  const auto terminal_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < terminal_deadline) {
    publisher.publish({"joint_1"}, {0.2});
    auto snapshot = backend.get_execution_snapshot();
    if (snapshot.state == core::ExecutionState::kCompleted ||
        snapshot.state == core::ExecutionState::kFailed ||
        snapshot.state == core::ExecutionState::kInterventionRequired) {
      terminal_snapshot = std::move(snapshot);
      break;
    }
    std::this_thread::sleep_for(5ms);
  }

  ASSERT_TRUE(terminal_snapshot.has_value());
  EXPECT_EQ(terminal_snapshot->state, core::ExecutionState::kFailed);
  ASSERT_TRUE(terminal_snapshot->error.has_value());
  EXPECT_EQ(terminal_snapshot->error->details.at("reason_code"),
            ros2_control_fault_reason::kTrackingErrorExceeded);
  EXPECT_EQ(terminal_snapshot->error->details.at("phase"),
            "terminal_result_state_gate");
  EXPECT_EQ(terminal_snapshot->error->details.at("final_tolerance_rad"),
            "0.01");
  EXPECT_NE(terminal_snapshot->error->message.find("Final state mismatch"),
            std::string::npos);
}

TEST(Ros2ControlExecutionBackendTest,
     StopAndPauseIntentWinWhileMeasuredCompletionGateIsPending) {
  const auto run_case = [](bool pause) {
    const std::string suffix = unique_suffix();
    const std::string action_name = "/manyforge_terminal_intent_fjt_" + suffix;
    const std::string joint_state_topic =
        "/manyforge_terminal_intent_joint_states_" + suffix;

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
    Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

    ASSERT_TRUE(publisher.wait_for_subscription());
    bool synced = false;
    const auto sync_deadline = std::chrono::steady_clock::now() + 2s;
    while (!synced && std::chrono::steady_clock::now() < sync_deadline) {
      publisher.publish({"joint_1"}, {0.0});
      synced = backend.sync_observed_state_before_planning().ok();
      if (!synced) {
        std::this_thread::sleep_for(10ms);
      }
    }
    ASSERT_TRUE(synced);

    auto handle =
        backend.execute_trajectory(make_single_joint_trajectory(0.0, 0.1));
    ASSERT_TRUE(server.wait_for_goal());
    ASSERT_TRUE(server.publish_feedback(0.0));
    const auto feedback_deadline = std::chrono::steady_clock::now() + 1s;
    while (
        backend.controller_progress_metrics().action_feedback_callbacks_total ==
            0u &&
        std::chrono::steady_clock::now() < feedback_deadline) {
      ASSERT_TRUE(server.publish_feedback(0.0));
      std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(server.succeed_current_goal());
    const auto gate_deadline = std::chrono::steady_clock::now() + 250ms;
    while (!backend.testing_completion_gate_pending() &&
           std::chrono::steady_clock::now() < gate_deadline) {
      std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(backend.testing_completion_gate_pending());

    if (pause) {
      EXPECT_TRUE(backend.pause().accepted);
    } else {
      EXPECT_TRUE(backend.stop().accepted);
    }
    ASSERT_TRUE(handle.wait_for_completion(500'000'000));
    EXPECT_EQ(backend.get_execution_snapshot().state,
              pause ? core::ExecutionState::kPaused
                    : core::ExecutionState::kStopped);
  };

  run_case(false);
  run_case(true);
}

TEST(Ros2ControlExecutionBackendTest,
     ResetFencesPendingCompletionBeforeReplacementExecution) {
  const std::string suffix = unique_suffix();
  const std::string action_name =
      "/manyforge_completion_replacement_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_completion_replacement_joint_states_" + suffix;

  FakeFjtServer server(action_name);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.wait_for_state_timeout_s = 0.2;
  config.wait_for_action_server_timeout_s = 2.0;
  config.stop_completion_timeout_s = 1.0;
  config.joint_state_max_age_s = 5.0;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(publisher.wait_for_subscription());
  const auto sync_state = [&]() {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      publisher.publish({"joint_1"}, {0.0});
      if (backend.sync_observed_state_before_planning().ok()) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  };
  ASSERT_TRUE(sync_state());

  auto first =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 0.1));
  ASSERT_TRUE(server.wait_for_goal_count(1));
  ASSERT_TRUE(server.publish_feedback(0.0));
  ASSERT_TRUE(server.succeed_current_goal());
  const auto gate_deadline = std::chrono::steady_clock::now() + 250ms;
  while (!backend.testing_completion_gate_pending() &&
         std::chrono::steady_clock::now() < gate_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(backend.testing_completion_gate_pending());
  ASSERT_TRUE(backend.reset().accepted);
  EXPECT_FALSE(backend.testing_completion_gate_pending());

  ASSERT_TRUE(sync_state());
  auto second =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 0.2));
  ASSERT_GT(second.execution_generation, first.execution_generation);
  ASSERT_TRUE(server.wait_for_goal_count(2));
  ASSERT_TRUE(server.publish_feedback(0.0));
  ASSERT_TRUE(server.succeed_current_goal());
  const auto terminal_deadline = std::chrono::steady_clock::now() + 1s;
  while (!second.wait_for_completion(1'000'000) &&
         std::chrono::steady_clock::now() < terminal_deadline) {
    publisher.publish({"joint_1"}, {0.2});
  }
  ASSERT_TRUE(second.wait_for_completion(0));
  auto snapshot = backend.get_execution_snapshot();
  ASSERT_EQ(snapshot.state, core::ExecutionState::kCompleted);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), 0.2);

  std::this_thread::sleep_for(250ms);
  snapshot = backend.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, core::ExecutionState::kCompleted);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), 0.2);
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedMonitorNeverProjectsPredictionWhenMeasurementBecomesStale) {
  const std::string suffix = unique_suffix();
  const std::string action_name =
      "/manyforge_observed_no_prediction_fjt_" + suffix;
  const std::string joint_state_topic =
      "/manyforge_observed_no_prediction_joint_states_" + suffix;

  FakeFjtServer server(action_name);
  JointStatePublisher publisher(joint_state_topic);
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.action_name = action_name;
  config.joint_state_topic = joint_state_topic;
  config.wait_for_state_timeout_s = 0.5;
  config.wait_for_action_server_timeout_s = 2.0;
  config.joint_state_max_age_s = 0.02;
  config.tracking_error_tolerance_rad = 10.0;
  config.tracking_error_timeout_s = 10.0;
  Ros2ControlExecutionBackend backend(sink, "arm", clock, scheduler, config);

  ASSERT_TRUE(publisher.wait_for_subscription());
  bool synced = false;
  const auto sync_deadline = std::chrono::steady_clock::now() + 2s;
  while (!synced && std::chrono::steady_clock::now() < sync_deadline) {
    publisher.publish({"joint_1"}, {0.0});
    synced = backend.sync_observed_state_before_planning().ok();
    if (!synced) {
      std::this_thread::sleep_for(5ms);
    }
  }
  ASSERT_TRUE(synced);

  auto handle =
      backend.execute_trajectory(make_single_joint_trajectory(0.0, 0.5));
  ASSERT_NE(handle.execution_generation, 0u);
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(server.publish_feedback(0.0));
  const auto feedback_deadline = std::chrono::steady_clock::now() + 1s;
  while (
      backend.controller_progress_metrics().action_feedback_callbacks_total ==
          0u &&
      std::chrono::steady_clock::now() < feedback_deadline) {
    ASSERT_TRUE(server.publish_feedback(0.0));
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_GT(
      backend.controller_progress_metrics().action_feedback_callbacks_total,
      0u);

  // Both the joint-state sample and the one action-feedback sample are now
  // stale while the controller goal remains active. The trajectory sampler
  // reaches 0.5 rad, but that value is intent only on an observed lane.
  std::this_thread::sleep_for(150ms);
  const auto snapshot = backend.get_execution_snapshot();
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), 0.0);
  EXPECT_EQ(snapshot.state_source, "observed_ros2");
  for (const auto &update : sink->group_updates()) {
    ASSERT_EQ(update.positions_rad.size(), 1u);
    EXPECT_DOUBLE_EQ(update.positions_rad.front(), 0.0);
  }
}

TEST(Ros2ControlExecutionBackendTest,
     CommandProgressMetricsPublishCoherentCountLastAndMaximum) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

  auto metrics = backend.controller_progress_metrics();
  EXPECT_EQ(metrics.command_progress_samples_total, 0u);
  EXPECT_EQ(metrics.last_command_progress_latency_ns, 0u);
  EXPECT_EQ(metrics.max_command_progress_latency_ns, 0u);

  backend.testing_record_command_progress_latency_ns(25);
  backend.testing_record_command_progress_latency_ns(10);
  backend.testing_record_command_progress_latency_ns(-1);

  metrics = backend.controller_progress_metrics();
  EXPECT_EQ(metrics.command_progress_samples_total, 3u);
  EXPECT_EQ(metrics.last_command_progress_latency_ns, 0u);
  EXPECT_EQ(metrics.max_command_progress_latency_ns, 25u);
}

TEST(Ros2ControlExecutionBackendTest,
     CommandProgressMetricsReaderWaitsForCompletePublication) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

  backend.testing_record_command_progress_latency_ns(10);
  backend.testing_begin_command_progress_metrics_publication(20);

  std::promise<void> reader_started;
  auto reader_started_future = reader_started.get_future();
  auto reader = std::async(std::launch::async, [&]() {
    reader_started.set_value();
    return backend.controller_progress_metrics();
  });
  EXPECT_EQ(reader_started_future.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(reader.wait_for(50ms), std::future_status::timeout)
      << "reader exposed count/latency fields while their sequence was odd";

  backend.testing_finish_command_progress_metrics_publication();
  ASSERT_EQ(reader.wait_for(1s), std::future_status::ready);
  const auto metrics = reader.get();
  EXPECT_EQ(metrics.command_progress_samples_total, 2u);
  EXPECT_EQ(metrics.last_command_progress_latency_ns, 20u);
  EXPECT_EQ(metrics.max_command_progress_latency_ns, 20u);
}

TEST(Ros2ControlExecutionBackendTest,
     ControlledStopCommandReferenceValidatesOnceAndPublishesSamples) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.0};
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto started_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(backend.testing_install_controlled_stop_command_reference(
      make_single_joint_trajectory(0.0, 1.0), started_at));
  ASSERT_TRUE(
      backend.testing_publish_controlled_stop_command_reference_snapshot(
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
     ObservedControlledStopCommandReferenceNeverBecomesRobotPose) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "observed_ros2";
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto measured_at = std::chrono::steady_clock::now();
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.2}, measured_at,
                                            {0.0}, 100);
  backend.testing_sync_idle_scene_from_latest_sample();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  ASSERT_EQ(sink->group_updates().size(), 1u);

  const auto started_at = measured_at;
  ASSERT_TRUE(backend.testing_install_controlled_stop_command_reference(
      make_single_joint_trajectory(0.0, 1.0), started_at));
  EXPECT_FALSE(
      backend.testing_publish_controlled_stop_command_reference_snapshot(
          started_at + 50ms));
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));

  const auto snapshot = backend.get_execution_snapshot();
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), 0.2);
  EXPECT_EQ(snapshot.source_timestamp_ns, 100);
  EXPECT_EQ(snapshot.state_source, "observed_ros2");
  ASSERT_EQ(sink->group_updates().size(), 1u);
  EXPECT_DOUBLE_EQ(sink->group_updates().front().positions_rad.front(), 0.2);
}

TEST(Ros2ControlExecutionBackendTest,
     IdleSceneSyncRateLimitsAndSkipsWhileRunning) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

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
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.3},
                                            std::chrono::steady_clock::now());
  backend.testing_sync_idle_scene_from_latest_sample();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  ASSERT_EQ(sink->group_updates().size(), 2u);

  backend.testing_set_running_state(true);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.4},
                                            std::chrono::steady_clock::now());
  backend.testing_sync_idle_scene_from_latest_sample();
  EXPECT_EQ(sink->group_updates().size(), 2u)
      << "running executions must not mirror passive idle scene updates";

  backend.testing_set_running_state(false);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.5},
                                            std::chrono::steady_clock::now());
  backend.testing_sync_idle_scene_from_latest_sample();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));
  ASSERT_EQ(sink->group_updates().size(), 3u)
      << "the first sample after an execution generation is not rate limited";
  EXPECT_NEAR(sink->group_updates().back().positions_rad.front(), 0.5, 1e-9);
}

TEST(Ros2ControlExecutionBackendTest,
     TerminalObservedStateRefreshPreservesLifecycleMetadataAndAges) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "observed_ros2";
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  backend.testing_set_running_state(true);
  backend.testing_set_running_state(false);
  clock.set(1'000'000);
  const core::ErrorDTO terminal_error{
      "test_terminal_failure",
      "terminal metadata must survive measured-state refresh",
      {{"reason", "test"}}};
  ASSERT_TRUE(backend.testing_publish_snapshot(core::ExecutionState::kFailed,
                                               0.75, {0.1}, terminal_error));
  const auto terminal = backend.get_execution_snapshot();

  const auto now = std::chrono::steady_clock::now();
  clock.set(1'500'000);
  backend.testing_store_action_feedback_sample({"joint_1"}, {9.0}, {0.0},
                                               now - 1ms);
  clock.set(2'000'000);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.42}, now, {-0.03},
                                            2'000'000);
  backend.testing_sync_idle_scene_from_latest_sample();
  ASSERT_TRUE(backend.testing_drain_scene_projection_for(1s));

  const auto refreshed = backend.get_execution_snapshot();
  EXPECT_EQ(refreshed.state, core::ExecutionState::kFailed);
  EXPECT_DOUBLE_EQ(refreshed.progress, 0.75);
  ASSERT_TRUE(refreshed.error.has_value());
  EXPECT_EQ(refreshed.error->code, terminal_error.code);
  EXPECT_EQ(refreshed.error->message, terminal_error.message);
  EXPECT_EQ(refreshed.error->details, terminal_error.details);
  EXPECT_GT(refreshed.sequence_number, terminal.sequence_number);
  ASSERT_EQ(refreshed.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(refreshed.joint_positions_rad[0], 0.42);
  ASSERT_EQ(refreshed.joint_velocities_rad_s.size(), 1u);
  EXPECT_DOUBLE_EQ(refreshed.joint_velocities_rad_s[0], -0.03);
  EXPECT_EQ(refreshed.state_source, "observed_ros2");
  EXPECT_EQ(refreshed.source_timestamp_ns, 2'000'000);
  EXPECT_EQ(refreshed.source_age_ns, 0);
  ASSERT_EQ(sink->group_updates().size(), 1u);
  EXPECT_DOUBLE_EQ(sink->group_updates().front().positions_rad.front(), 0.42);

  clock.advance(250'000);
  const auto aged = backend.get_execution_snapshot();
  EXPECT_EQ(aged.source_timestamp_ns, refreshed.source_timestamp_ns);
  EXPECT_EQ(aged.source_age_ns, 250'000);
  EXPECT_EQ(aged.state, core::ExecutionState::kFailed);
  EXPECT_DOUBLE_EQ(aged.progress, 0.75);
  ASSERT_TRUE(aged.error.has_value());
  EXPECT_EQ(aged.error->code, terminal_error.code);
}

TEST(Ros2ControlExecutionBackendTest,
     AuthoredSimulationSnapshotRetainsNonExpiringAuthoritySemantics) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.0};
  clock.set(1'000'000);
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto initial = backend.get_execution_snapshot();
  ASSERT_EQ(initial.state_source, "commanded_sim_predicted");
  clock.advance(250'000);
  const auto later = backend.get_execution_snapshot();
  EXPECT_EQ(later.source_timestamp_ns, initial.source_timestamp_ns);
  EXPECT_EQ(later.source_age_ns, 0);
}

TEST(Ros2ControlExecutionBackendTest,
     ActiveSceneProjectionIsAsyncAndCoalescesToLatestState) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

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
  EXPECT_EQ(updates.back().mode, core::ExecutionStateUpdateMode::kBestEffort);

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
      Ros2ControlExecutionBackend::DeferredRosExecutorStartTag{}, sink, "arm",
      clock, scheduler, base_config());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_independent_projection_slots_" +
                                   unique_suffix()));
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
  EXPECT_EQ(group_updates.front().positions_rad, (std::vector<double>{0.1}));
  EXPECT_EQ(group_updates.back().positions_rad, (std::vector<double>{0.3}));
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

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

  backend.testing_store_action_feedback_sample({"joint_1"}, {1.0}, {},
                                               t0 + 1ms);
  EXPECT_GE(backend.testing_latest_action_feedback_position_capacity(),
            first_capacity);
}

TEST(SceneProjectionWorkerTest,
     CompletionCallbackExceptionIsContainedAndMetriced) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  detail::SceneProjectionWorker worker(sink, "arm", clock);
  const auto slot = worker.register_joint_slot(
      "gripper", "finger_joint", [](const detail::SceneProjectionCompletion &) {
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

  const auto t0 = std::chrono::steady_clock::now();
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.42}, t0);
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

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
  stale.result->error_code = Ros2ControlExecutionBackend::
      TestingFollowJointTrajectory::Result::SUCCESSFUL;

  backend.testing_on_managed_fjt_terminal_result(stale, /*generation=*/1);

  EXPECT_FALSE(backend.testing_managed_fjt_terminal_received())
      << "late result from a previous replacement dispatch must not satisfy "
         "the current managed-stop waiter";

  Ros2ControlExecutionBackend::TestingGoalHandleFJT::WrappedResult current;
  current.goal_id = current_goal;
  current.code = rclcpp_action::ResultCode::SUCCEEDED;
  current.result = std::make_shared<
      Ros2ControlExecutionBackend::TestingFollowJointTrajectory::Result>();
  current.result->error_code = Ros2ControlExecutionBackend::
      TestingFollowJointTrajectory::Result::SUCCESSFUL;

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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, base_config());

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
     FreshPassiveSampleWaitWakesOnStateNotificationBeforeDeadline) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.joint_state_max_age_s = 5.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto not_before = std::chrono::steady_clock::now();
  std::promise<void> waiter_started;
  auto waiter_started_future = waiter_started.get_future();
  auto sample_future = std::async(std::launch::async, [&]() {
    waiter_started.set_value();
    return backend.testing_wait_for_fresh_passive_sample_after(not_before, 5s);
  });
  ASSERT_EQ(waiter_started_future.wait_for(500ms), std::future_status::ready);
  std::this_thread::sleep_for(20ms);
  ASSERT_EQ(sample_future.wait_for(0ms), std::future_status::timeout);

  backend.testing_store_latest_controller_sample(
      {"joint_1"}, {0.42}, std::chrono::steady_clock::now(), {0.0});

  ASSERT_EQ(sample_future.wait_for(500ms), std::future_status::ready)
      << "a fresh post-result sample notification must not wait for the "
         "controlled-stop deadline";
  const auto source = sample_future.get();
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(*source, "controller_state");
}

TEST(Ros2ControlExecutionBackendTest,
     ObservedStatePublicationFallsBackWhenActionFeedbackStops) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.joint_state_max_age_s = 0.1;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);
  const auto *progress = backend.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  EXPECT_EQ(progress->source_kind(),
            core::ExecutionProgressSourceKind::kMeasuredProjection);
  EXPECT_EQ(progress->authority_profile(),
            core::ExecutionProgressAuthorityProfile::kMeasuredPhysical);
  EXPECT_GT(progress->freshness_budget_ns(), 0);

  const auto base = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample({"joint_1"}, {0.25}, {0.5},
                                               base);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.75}, base + 150ms);

  const auto *publication = backend.observed_robot_state_publication();
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
  config.observed_state_source_precedence = {"joint_state", "action_feedback"};
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);
  const auto now = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample({"joint_1"}, {0.25}, {0.0}, now);
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
     PassivePoseSelectionUsesDeclaredPrecedenceAndExcludesRetiredFeedback) {
  const auto verify = [](std::vector<std::string> precedence,
                         double expected_position) {
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    auto config = base_config();
    config.observed_state_source_precedence = std::move(precedence);
    config.joint_state_max_age_s = 10.0;
    Ros2ControlExecutionBackend backend(
        Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm",
        clock, scheduler, config);

    const auto now = std::chrono::steady_clock::now();
    backend.testing_store_action_feedback_sample({"joint_1"}, {9.0}, {0.0},
                                                 now);
    backend.testing_store_latest_controller_sample({"joint_1"}, {0.4}, now,
                                                   {0.0}, 400);
    backend.testing_store_latest_joint_sample({"joint_1"}, {0.7}, now, {0.0},
                                              700);
    backend.testing_sync_idle_scene_from_latest_sample();

    const auto snapshot = backend.get_execution_snapshot();
    ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
    EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), expected_position);
    EXPECT_NE(snapshot.joint_positions_rad.front(), 9.0);
  };

  verify({"controller_state", "joint_state", "action_feedback"}, 0.4);
  verify({"joint_state", "controller_state", "action_feedback"}, 0.7);
}

TEST(Ros2ControlExecutionBackendTest,
     MalformedHigherPrecedenceSampleFallsBackToCompletePhysicalSource) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "observed_ros2";
  config.observed_state_source_precedence = {"controller_state", "joint_state",
                                             "action_feedback"};
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto now = std::chrono::steady_clock::now();
  backend.testing_store_latest_controller_sample(
      {"joint_1"}, {std::numeric_limits<double>::quiet_NaN()}, now, {0.0}, 100);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.75}, now, {0.01},
                                            200);
  backend.testing_sync_idle_scene_from_latest_sample();

  const auto snapshot = backend.get_execution_snapshot();
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), 0.75);
  ASSERT_EQ(snapshot.joint_velocities_rad_s.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_velocities_rad_s.front(), 0.01);
  EXPECT_EQ(snapshot.source_timestamp_ns, 200);
}

TEST(Ros2ControlExecutionBackendTest,
     PrePlanningStateGateNeverUsesActionFeedbackFromRetiredGoal) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "observed_ros2";
  config.observed_state_source_precedence = {"action_feedback",
                                             "controller_state", "joint_state"};
  config.wait_for_state_timeout_s = 0.001;
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto now = std::chrono::steady_clock::now();
  backend.testing_store_action_feedback_sample({"joint_1"}, {9.0}, {0.0}, now);
  const auto action_only = backend.sync_observed_state_before_planning();
  EXPECT_FALSE(action_only.ok());

  backend.testing_store_latest_joint_sample(
      {"joint_1"}, {0.25}, std::chrono::steady_clock::now(), {0.0}, 250);
  const auto passive = backend.sync_observed_state_before_planning();
  ASSERT_TRUE(passive.ok()) << passive.error().message;
  const auto snapshot = backend.get_execution_snapshot();
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), 0.25);
  EXPECT_EQ(snapshot.source_timestamp_ns, 250);
}

TEST(Ros2ControlExecutionBackendTest,
     ManagedStopRetiresOriginalGoalActionFeedbackAuthority) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.observed_state_source_precedence = {"action_feedback", "joint_state"};
  config.joint_state_max_age_s = 10.0;
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto now = std::chrono::steady_clock::now();
  backend.testing_set_action_feedback_authority(true);
  backend.testing_store_action_feedback_sample({"joint_1"}, {9.0}, {0.0}, now);
  backend.testing_store_latest_joint_sample({"joint_1"}, {0.5}, now, {0.0},
                                            500);
  ASSERT_EQ(backend.testing_selected_sample_source(now),
            std::optional<std::string>("action_feedback"));

  // Mirrors the managed-stop preemption callback: the execution remains
  // running, but the original FJT goal has terminalized.
  backend.testing_set_action_feedback_authority(false);
  EXPECT_EQ(backend.testing_selected_sample_source(now),
            std::optional<std::string>("joint_state"));
}

TEST(Ros2ControlExecutionBackendTest,
     PassiveObservedRefreshCannotRegressPhysicalSampleTimestamp) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "observed_ros2";
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);
  const auto generation = backend.testing_running_generation();

  ASSERT_TRUE(backend.testing_publish_passive_observed_snapshot(
      {0.6}, {0.01}, 600, generation));
  EXPECT_FALSE(backend.testing_publish_passive_observed_snapshot(
      {0.2}, {-0.01}, 200, generation));

  const auto snapshot = backend.get_execution_snapshot();
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad.front(), 0.6);
  ASSERT_EQ(snapshot.joint_velocities_rad_s.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_velocities_rad_s.front(), 0.01);
  EXPECT_EQ(snapshot.source_timestamp_ns, 600);
}

TEST(Ros2ControlExecutionBackendTest,
     AuthoredPreviewPublishesCommandedNotMeasuredAuthority) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.execution_state_source_mode = "authored_preview";
  Ros2ControlExecutionBackend backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);
  const auto *progress = backend.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  EXPECT_EQ(progress->source_kind(),
            core::ExecutionProgressSourceKind::kBackendCommanded);
  EXPECT_EQ(progress->authority_profile(),
            core::ExecutionProgressAuthorityProfile::kCommandedSimulation);
  EXPECT_EQ(progress->freshness_budget_ns(), 0);
  const auto now = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample({"joint_1"}, {0.25}, {0.0}, now);
  ASSERT_TRUE(
      backend.testing_publish_snapshot(core::ExecutionState::kIdle, {0.75}));

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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);
  const auto *progress = backend.execution_progress_publication();
  ASSERT_NE(progress, nullptr);
  EXPECT_EQ(progress->source_kind(),
            core::ExecutionProgressSourceKind::kBackendCommanded);
  EXPECT_EQ(progress->authority_profile(),
            core::ExecutionProgressAuthorityProfile::kCommandedSimulation);
  EXPECT_EQ(progress->freshness_budget_ns(), 0);

  auto initial =
      backend.observed_robot_state_publication()->acquire(clock.now());
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial.source_kind(),
            core::ObservedRobotStateSourceKind::kCommandedState);
  ASSERT_EQ(initial.positions_rad().size(), 1u);
  EXPECT_DOUBLE_EQ(initial.positions_rad()[0], 0.0);

  ASSERT_TRUE(
      backend.testing_publish_snapshot(core::ExecutionState::kIdle, {0.75}));
  const auto now = std::chrono::steady_clock::time_point{1s};
  backend.testing_store_action_feedback_sample({"joint_1"}, {0.25}, {0.5}, now);
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
  EXPECT_EQ(selected.authority_profile_id(), "ros2_control_commanded_sim_v1");
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);
  backend.testing_set_running_state(true);

  const auto first_token =
      backend.testing_begin_jtc_cancel_reference_authority();
  ASSERT_NE(first_token, 0u);
  ASSERT_TRUE(backend.testing_publish_jtc_cancel_reference(
      {"joint_2", "joint_1"}, {0.8, 0.4}, {0.2, 0.1}, 100, first_token));

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
      {"joint_1", "joint_2"}, {0.9, 0.9}, {0.0, 0.0}, 200, first_token));
  ASSERT_TRUE(backend.testing_publish_jtc_cancel_reference(
      {"joint_1", "joint_2"}, {0.5, 0.6}, {0.0, 0.0}, 201, second_token));
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);
  backend.testing_set_running_state(true);

  EXPECT_EQ(backend.testing_begin_jtc_cancel_reference_authority(), 0u);
  EXPECT_FALSE(backend.testing_publish_jtc_cancel_reference({"joint_1"}, {0.5},
                                                            {0.0}, 100, 1));
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  backend.testing_set_running_state(true);
  const auto execution_generation = backend.testing_running_generation();
  ASSERT_TRUE(backend.testing_publish_snapshot_for_generation(
      core::ExecutionState::kExecuting, {0.5}, execution_generation));

  backend.testing_set_running_state(false);
  ASSERT_TRUE(backend.testing_publish_snapshot(core::ExecutionState::kCompleted,
                                               {1.0}));
  EXPECT_FALSE(backend.testing_publish_snapshot_for_generation(
      core::ExecutionState::kExecuting, {0.75}, execution_generation));

  const auto snapshot = backend.get_execution_snapshot();
  EXPECT_EQ(snapshot.state, core::ExecutionState::kCompleted);
  ASSERT_EQ(snapshot.joint_positions_rad.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.joint_positions_rad[0], 1.0);
  auto observed =
      backend.observed_robot_state_publication()->acquire(clock.now());
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
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{}, sink, "arm", clock,
      scheduler, config);

  const auto *publication = backend.observed_robot_state_publication();
  ASSERT_NE(publication, nullptr);
  auto first = publication->acquire(clock.now());
  ASSERT_TRUE(first);
  ASSERT_TRUE(backend.testing_publish_snapshot(core::ExecutionState::kExecuting,
                                               {0.25}));
  auto second = publication->acquire(clock.now());
  ASSERT_TRUE(second);
  ASSERT_TRUE(backend.testing_publish_snapshot(core::ExecutionState::kExecuting,
                                               {0.5}));

  // The current slot is never overwritten and both older slots are pinned.
  // A terminal authority replacement must fail closed instead of leaving the
  // non-expiring 0.5 command reference admissible for the next plan.
  EXPECT_FALSE(backend.testing_publish_snapshot(
      core::ExecutionState::kCompleted, {1.0}));
  ASSERT_NE(backend.authored_state_authority_epoch(), nullptr);
  const auto invalid_epoch =
      backend.authored_state_authority_epoch()->load(std::memory_order_acquire);
  EXPECT_NE(invalid_epoch & 1u, 0u);

  first = {};
  second = {};
  EXPECT_TRUE(
      backend.testing_publish_snapshot(core::ExecutionState::kIdle, {1.0}));
  const auto restored_epoch =
      backend.authored_state_authority_epoch()->load(std::memory_order_acquire);
  EXPECT_EQ(restored_epoch & 1u, 0u);
  EXPECT_GT(restored_epoch, invalid_epoch);
}

} // namespace
} // namespace manyforge::ros2_control_execution
