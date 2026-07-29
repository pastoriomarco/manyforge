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

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include <lifecycle_msgs/msg/state.hpp>
#include <manyforge_robot_lifecycle/admission_publication_driver.hpp>
#include <manyforge_robot_lifecycle/local_operator_gateway.hpp>
#include <manyforge_xarm_control/lite6_lifecycle_catalog.hpp>
#include <manyforge_xarm_control/lifecycle_ros_port.hpp>
#include <manyforge_xarm_control/xarm_ros_transport_session.hpp>

namespace manyforge::xarm_control
{
namespace
{

constexpr std::string_view kOwnerId = "lite6-test:sole-owner";
constexpr std::string_view kStateTopic =
  "/lite6_test/ufactory/supervised_lifecycle/state";
constexpr std::string_view kExecuteService =
  "/lite6_test/ufactory/supervised_lifecycle/execute";
constexpr std::string_view kGateService =
  "/lite6_test/ufactory/supervised_lifecycle/set_command_gate";
constexpr std::string_view kShutdownService =
  "/lite6_test/ufactory/supervised_lifecycle/shutdown_controller";
constexpr std::string_view kActivityTopic =
  "/lite6_test/controller_manager/activity";
constexpr std::string_view kTrajectoryController =
  "lite6_traj_controller";

std::int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

class FakeRosOwner
{
public:
  explicit FakeRosOwner(
    std::chrono::milliseconds execute_delay =
    std::chrono::milliseconds::zero(),
    std::chrono::milliseconds gate_delay =
    std::chrono::milliseconds::zero())
  : execute_delay_(execute_delay),
    gate_delay_(gate_delay)
  {
    node_ = std::make_shared<rclcpp::Node>("fake_xarm_supervised_owner");
    const auto qos = rclcpp::QoS(rclcpp::KeepLast(1)).
      reliable().transient_local();
    publisher_ = node_->create_publisher<
      xarm_msgs::msg::SupervisedLifecycleState>(
      std::string(kStateTopic), qos);
    activity_publisher_ = node_->create_publisher<
      controller_manager_msgs::msg::ControllerManagerActivity>(
      std::string(kActivityTopic), qos);
    execute_service_ = node_->create_service<
      xarm_msgs::srv::ExecuteSupervisedLifecycleCommand>(
      std::string(kExecuteService),
      [this](
        const std::shared_ptr<
          xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Request>
        request,
        std::shared_ptr<
          xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Response>
        response)
      {
        execute_count_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(execute_delay_);
        std::scoped_lock lock(mutex_);
        last_command_ = request->command;
        last_request_id_ = request->request_id;
        last_expected_generation_ = request->expected_generation;
        response->permitted =
        request->expected_generation == state_.generation;
        response->attempted = response->permitted;
        response->return_code = response->permitted ? 0 : -1;
        response->reason = response->permitted ?
        "command_succeeded" : "observation_generation_mismatch";
        response->observed_generation = state_.generation;
      });
    gate_service_ = node_->create_service<
      xarm_msgs::srv::SetSupervisedCommandGate>(
      std::string(kGateService),
      [this](
        const std::shared_ptr<
          xarm_msgs::srv::SetSupervisedCommandGate::Request> request,
        std::shared_ptr<
          xarm_msgs::srv::SetSupervisedCommandGate::Response> response)
      {
        gate_update_count_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(gate_delay_);
        std::scoped_lock lock(mutex_);
        last_gate_open_ = request->open;
        last_gate_lease_ns_ = request->lease_duration_ns;
        response->accepted =
        request->expected_generation == state_.generation &&
        (!request->open || request->lease_duration_ns > 0);
        response->reason = response->accepted ?
        "gate_updated" : "gate_rejected";
        response->observed_generation = state_.generation;
        response->valid_until_steady_time_ns = request->open ?
        steady_now_ns() + request->lease_duration_ns : 0;
        gate_response_count_.fetch_add(1, std::memory_order_release);
      });
    shutdown_service_ = node_->create_service<
      xarm_msgs::srv::ShutdownSupervisedController>(
      std::string(kShutdownService),
      [this](
        const std::shared_ptr<
          xarm_msgs::srv::ShutdownSupervisedController::Request>
        request,
        std::shared_ptr<
          xarm_msgs::srv::ShutdownSupervisedController::Response>
        response)
      {
        shutdown_count_.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock(mutex_);
        last_shutdown_request_id_ = request->request_id;
        last_shutdown_expected_generation_ =
        request->expected_generation;
        last_shutdown_confirmation_ = request->confirmation;
        response->permitted =
        request->expected_generation == state_.generation &&
        request->confirmation == "SHUTDOWN_CONTROLLER";
        response->attempted = response->permitted;
        response->return_code = response->permitted ? 0 : -1;
        response->reason = response->permitted ?
        "controller_shutdown_vendor_accepted" :
        "shutdown_controller_rejected";
        response->observed_generation = state_.generation;
        response->process_restart_required = response->attempted;
      });
    executor_ =
      std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
    executor_->add_node(node_);
    // A blocking spin has a startup race in a short-lived fixture: cancel()
    // can run before spin() enters and the subsequent spin can wait forever.
    // A bounded spin_some loop gives teardown an explicit, reliable stop
    // predicate even when the object unwinds during another constructor.
    thread_ = std::thread([this]() {
          while (!stop_.load(std::memory_order_acquire)) {
            executor_->spin_some(std::chrono::milliseconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
  }

  void publish_controller_activity(bool active)
  {
    controller_manager_msgs::msg::ControllerManagerActivity message;
    controller_manager_msgs::msg::NamedLifecycleState controller;
    controller.name = kTrajectoryController;
    controller.state.id = active ?
      lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE :
      lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
    controller.state.label = active ? "active" : "inactive";
    message.controllers.push_back(std::move(controller));
    activity_publisher_->publish(message);
  }

  ~FakeRosOwner()
  {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
      thread_.join();
    }
    executor_->remove_node(node_);
  }

  void publish(
    std::uint64_t generation,
    std::string session_id = "owner-session-1",
    bool fresh = true,
    int state = 2,
    bool hardware_component_active = true,
    bool lifecycle_command_in_flight = false,
    std::chrono::milliseconds lifecycle_command_budget =
    std::chrono::milliseconds::zero())
  {
    xarm_msgs::msg::SupervisedLifecycleState message;
    message.owner_id = kOwnerId;
    message.session_id = std::move(session_id);
    message.owner_publication_sequence =
      ++owner_publication_sequence_;
    message.generation = generation;
    const auto now_ns = steady_now_ns();
    message.owner_published_steady_time_ns = now_ns;
    message.lifecycle_command_in_flight =
      lifecycle_command_in_flight;
    if (lifecycle_command_in_flight) {
      message.lifecycle_command_sequence = 1;
      message.lifecycle_command =
        xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Request::
        ENABLE_MOTION;
      message.lifecycle_command_started_steady_time_ns =
        now_ns - 10'000'000;
      message.lifecycle_command_deadline_steady_time_ns =
        now_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(
        lifecycle_command_budget).count();
    }
    message.source_steady_time_ns =
      fresh ? now_ns : now_ns - 1'000'000'000;
    message.fresh_until_steady_time_ns =
      fresh ? now_ns + 500'000'000 : now_ns - 500'000'000;
    message.transport_connected = true;
    message.report_connected = true;
    message.identity_verified = true;
    message.report_received = true;
    message.position_valid = true;
    message.hardware_component_active = hardware_component_active;
    message.identity_axis = 6;
    message.identity_device_type = 9;
    message.state = state;
    message.mode = 1;
    message.brake_mask = 63;
    message.servo_enable_mask = 63;
    message.error_code = 0;
    message.warning_code = 0;
    message.report_sample_count = generation;
    message.joint_read_attempt_count = generation;
    message.joint_read_success_count = generation;
    {
      std::scoped_lock lock(mutex_);
      state_ = message;
    }
    publisher_->publish(message);
  }

  void redeliver_last_state()
  {
    xarm_msgs::msg::SupervisedLifecycleState message;
    {
      std::scoped_lock lock(mutex_);
      message = state_;
    }
    publisher_->publish(message);
  }

  [[nodiscard]] std::uint8_t last_command() const
  {
    std::scoped_lock lock(mutex_);
    return last_command_;
  }

  [[nodiscard]] std::string last_request_id() const
  {
    std::scoped_lock lock(mutex_);
    return last_request_id_;
  }

  [[nodiscard]] std::uint64_t last_expected_generation() const
  {
    std::scoped_lock lock(mutex_);
    return last_expected_generation_;
  }

  [[nodiscard]] bool last_gate_open() const
  {
    std::scoped_lock lock(mutex_);
    return last_gate_open_;
  }

  [[nodiscard]] std::int64_t last_gate_lease_ns() const
  {
    std::scoped_lock lock(mutex_);
    return last_gate_lease_ns_;
  }

  [[nodiscard]] std::size_t execute_count() const noexcept
  {
    return execute_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t shutdown_count() const noexcept
  {
    return shutdown_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t gate_update_count() const noexcept
  {
    return gate_update_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t gate_response_count() const noexcept
  {
    return gate_response_count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::string last_shutdown_request_id() const
  {
    std::scoped_lock lock(mutex_);
    return last_shutdown_request_id_;
  }

  [[nodiscard]] std::uint64_t
  last_shutdown_expected_generation() const
  {
    std::scoped_lock lock(mutex_);
    return last_shutdown_expected_generation_;
  }

  [[nodiscard]] std::string last_shutdown_confirmation() const
  {
    std::scoped_lock lock(mutex_);
    return last_shutdown_confirmation_;
  }

private:
  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<
    xarm_msgs::msg::SupervisedLifecycleState>::SharedPtr publisher_;
  rclcpp::Publisher<
    controller_manager_msgs::msg::ControllerManagerActivity>::SharedPtr
    activity_publisher_;
  rclcpp::Service<
    xarm_msgs::srv::ExecuteSupervisedLifecycleCommand>::SharedPtr
    execute_service_;
  rclcpp::Service<xarm_msgs::srv::SetSupervisedCommandGate>::SharedPtr
    gate_service_;
  rclcpp::Service<
    xarm_msgs::srv::ShutdownSupervisedController>::SharedPtr
    shutdown_service_;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::thread thread_;
  mutable std::mutex mutex_;
  xarm_msgs::msg::SupervisedLifecycleState state_;
  std::uint8_t last_command_ = 0;
  std::string last_request_id_;
  std::uint64_t last_expected_generation_ = 0;
  std::string last_shutdown_request_id_;
  std::uint64_t last_shutdown_expected_generation_ = 0;
  std::string last_shutdown_confirmation_;
  bool last_gate_open_ = false;
  std::int64_t last_gate_lease_ns_ = 0;
  std::chrono::milliseconds execute_delay_;
  std::chrono::milliseconds gate_delay_;
  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> execute_count_{0};
  std::atomic<std::size_t> shutdown_count_{0};
  std::atomic<std::size_t> gate_update_count_{0};
  std::atomic<std::size_t> gate_response_count_{0};
  std::uint64_t owner_publication_sequence_ = 0;
};

XArmRosTransportSessionConfig config()
{
  XArmRosTransportSessionConfig result;
  result.owner_id = kOwnerId;
  result.state_topic = kStateTopic;
  result.execute_service = kExecuteService;
  result.command_gate_service = kGateService;
  result.shutdown_controller_service = kShutdownService;
  result.controller_manager = "/lite6_test/controller_manager";
  result.hardware_component_name = "lite6_system";
  result.joint_state_broadcaster_name = "joint_state_broadcaster";
  result.controller_activity_topic = kActivityTopic;
  result.trajectory_controller_name = kTrajectoryController;
  result.observation_max_age_ns = 500'000'000;
  result.service_timeout_ns = 500'000'000;
  result.execute_timeout_ns = 500'000'000;
  return result;
}

XArmObservedState await_generation(
  XArmRosTransportSession & session,
  std::uint64_t generation)
{
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  XArmObservedState result;
  while (std::chrono::steady_clock::now() < deadline) {
    result = session.observe_vendor(steady_now_ns());
    if (result.generation == generation) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return result;
}

robot_lifecycle::AuthoritySession authority_session(
  std::int64_t now_ns)
{
  robot_lifecycle::AuthoritySession result;
  result.profile = robot_lifecycle::AuthorityProfile::kLocalOperator;
  result.session_id = "operator-session";
  result.generation = 1;
  result.principal_id = "human:operator";
  result.valid_until_ns = now_ns + 5'000'000'000;
  result.component_epoch = "operator-epoch-1";
  return result;
}

robot_lifecycle::CallerContext authenticated_caller(
  std::int64_t now_ns)
{
  robot_lifecycle::CallerContext result;
  result.command_source = "test.lifecycle_operator";
  result.caller_context_id = "test-context";
  result.assurance =
    robot_lifecycle::CallerAssurance::kAuthenticated;
  result.asserted_at_ns = now_ns;
  result.valid_until_ns = now_ns + 10'000'000'000;
  return result;
}

class ProductionPathTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    if (!rclcpp::ok()) {
      int argc = 0;
      rclcpp::init(argc, nullptr);
    }
  }

  static void TearDownTestSuite()
  {
    rclcpp::shutdown();
  }
};

TEST_F(ProductionPathTest, MapsFreshStateAndNamedRosOperations)
{
  FakeRosOwner owner;
  XArmRosTransportSession session(config());
  owner.publish(17);
  owner.publish_controller_activity(true);

  auto observed = await_generation(session, 17);
  const auto activity_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (
    observed.trajectory_controller_active != FactState::kTrue &&
    std::chrono::steady_clock::now() < activity_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    observed = session.observe_vendor(steady_now_ns());
  }
  ASSERT_EQ(observed.generation, 17U);
  EXPECT_GT(observed.transport_owner_progress_sequence, 0U);
  EXPECT_TRUE(observed.transport_owner_progress_healthy);
  EXPECT_TRUE(observed.transport_owner_progress_reason.empty());
  EXPECT_EQ(observed.transport_connected, FactState::kTrue);
  EXPECT_EQ(observed.identity_verified, FactState::kTrue);
  EXPECT_EQ(observed.position_valid, FactState::kTrue);
  EXPECT_EQ(observed.motion_enabled, FactState::kTrue);
  EXPECT_EQ(observed.vendor_control_ready, FactState::kTrue);
  EXPECT_EQ(
    observed.trajectory_controller_active, FactState::kTrue);
  EXPECT_EQ(observed.fault_present, FactState::kFalse);

  const auto result = session.execute_primitive(
    XArmLifecycleCommand::kDisableMotion, "request-17", 17);
  EXPECT_EQ(
    result.state, XArmLifecycleResultState::kResponseReceived);
  EXPECT_TRUE(result.permitted);
  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(result.return_code, 0);
  EXPECT_EQ(
    owner.last_command(),
    xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Request::
    DISABLE_MOTION);
  EXPECT_EQ(owner.last_request_id(), "request-17");
  EXPECT_EQ(owner.last_expected_generation(), 17U);

  EXPECT_TRUE(session.set_command_gate(
      true, steady_now_ns() + 100'000'000, 17));
  EXPECT_TRUE(owner.last_gate_open());
  EXPECT_GT(owner.last_gate_lease_ns(), 0);
}

TEST_F(
  ProductionPathTest,
  BoundedLifecycleCallKeepsOwnerProgressWithoutRenewingRobotObservation)
{
  FakeRosOwner owner;
  XArmRosTransportSession session(config());
  owner.publish(
    23, "owner-session-1", false, 2, true, true,
    std::chrono::milliseconds(400));

  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  XArmObservedState observed;
  while (std::chrono::steady_clock::now() < deadline) {
    observed = session.observe_vendor(steady_now_ns());
    if (observed.transport_owner_progress_sequence > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(observed.generation, 0U);
  EXPECT_TRUE(observed.transport_owner_progress_healthy);
  EXPECT_TRUE(observed.transport_owner_progress_reason.empty());
  EXPECT_NE(
    std::find(
      observed.blocking_reason_codes.begin(),
      observed.blocking_reason_codes.end(),
      "xarm_ros_state_stale"),
    observed.blocking_reason_codes.end());
}

TEST_F(
  ProductionPathTest,
  StaleRobotObservationFencesMotionWithoutDegradingLiveOwner)
{
  FakeRosOwner owner;
  XArmRosTransportSession session(config());
  owner.publish(24, "owner-session-1", false);

  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  XArmObservedState observed;
  while (std::chrono::steady_clock::now() < deadline) {
    observed = session.observe_vendor(steady_now_ns());
    if (observed.transport_owner_progress_sequence > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(observed.generation, 0U);
  EXPECT_TRUE(observed.transport_owner_progress_healthy);
  EXPECT_TRUE(observed.transport_owner_progress_reason.empty());
  EXPECT_NE(
    std::find(
      observed.blocking_reason_codes.begin(),
      observed.blocking_reason_codes.end(),
      "xarm_ros_state_stale"),
    observed.blocking_reason_codes.end());
}

TEST_F(
  ProductionPathTest,
  ExpiredLifecycleCallDegradesOwnerDespiteFreshOwnerDelivery)
{
  FakeRosOwner owner;
  XArmRosTransportSession session(config());
  owner.publish(25, "owner-session-1", false, 2, true, true);

  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  XArmObservedState observed;
  while (std::chrono::steady_clock::now() < deadline) {
    observed = session.observe_vendor(steady_now_ns());
    if (observed.transport_owner_progress_sequence > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(observed.generation, 0U);
  EXPECT_FALSE(observed.transport_owner_progress_healthy);
  EXPECT_EQ(
    observed.transport_owner_progress_reason,
    "xarm_lifecycle_command_deadline_expired");
}

TEST_F(
  ProductionPathTest,
  DuplicateOwnerPublicationCannotRenewReceiverLocalProgress)
{
  FakeRosOwner owner;
  auto session_config = config();
  session_config.observation_max_age_ns = 50'000'000;
  session_config.owner_progress_max_age_ns = 50'000'000;
  XArmRosTransportSession session(std::move(session_config));
  owner.publish(26);

  const auto delivery_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  XArmObservedState observed;
  while (std::chrono::steady_clock::now() < delivery_deadline) {
    observed = session.observe_vendor(steady_now_ns());
    if (observed.transport_owner_progress_sequence > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(observed.transport_owner_progress_healthy);
  const auto accepted_sequence =
    observed.transport_owner_progress_sequence;

  std::this_thread::sleep_for(std::chrono::milliseconds(75));
  owner.redeliver_last_state();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  observed = session.observe_vendor(steady_now_ns());

  EXPECT_EQ(
    observed.transport_owner_progress_sequence,
    accepted_sequence);
  EXPECT_FALSE(observed.transport_owner_progress_healthy);
  EXPECT_EQ(
    observed.transport_owner_progress_reason,
    "xarm_owner_publication_stale");
}

TEST_F(
  ProductionPathTest,
  WaitsForTheOriginalDelayedLifecycleResponseWithoutResending)
{
  FakeRosOwner owner(std::chrono::milliseconds(100));
  auto transport_config = config();
  transport_config.execute_timeout_ns = 500'000'000;
  XArmRosTransportSession session(std::move(transport_config));
  owner.publish(18);
  ASSERT_EQ(await_generation(session, 18).generation, 18U);

  const auto result = session.execute_primitive(
    XArmLifecycleCommand::kEnableMotion, "delayed-request-18", 18);

  EXPECT_EQ(
    result.state, XArmLifecycleResultState::kResponseReceived);
  EXPECT_TRUE(result.permitted);
  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(result.return_code, 0);
  EXPECT_EQ(owner.execute_count(), 1U);
  EXPECT_EQ(owner.last_request_id(), "delayed-request-18");
}

TEST_F(
  ProductionPathTest,
  ControllerShutdownUsesOnlyItsDedicatedConfirmedService)
{
  FakeRosOwner owner;
  XArmRosTransportSession session(config());
  owner.publish(41);
  ASSERT_EQ(await_generation(session, 41).generation, 41U);

  const auto result = session.execute_primitive(
    XArmLifecycleCommand::kShutdownController,
    "shutdown-request-41", 41);

  EXPECT_EQ(
    result.state, XArmLifecycleResultState::kResponseReceived);
  EXPECT_TRUE(result.permitted);
  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(result.return_code, 0);
  EXPECT_EQ(result.reason, "controller_shutdown_vendor_accepted");
  EXPECT_EQ(result.observed_generation, 41U);
  EXPECT_TRUE(result.process_restart_required);
  EXPECT_EQ(owner.execute_count(), 0U);
  EXPECT_EQ(owner.shutdown_count(), 1U);
  EXPECT_EQ(
    owner.last_shutdown_request_id(), "shutdown-request-41");
  EXPECT_EQ(owner.last_shutdown_expected_generation(), 41U);
  EXPECT_EQ(
    owner.last_shutdown_confirmation(), "SHUTDOWN_CONTROLLER");
}

TEST_F(
  ProductionPathTest,
  TimeoutAfterDispatchIsExplicitlyUnknownAndNeverResent)
{
  FakeRosOwner owner(std::chrono::milliseconds(200));
  auto transport_config = config();
  transport_config.execute_timeout_ns = 50'000'000;
  XArmRosTransportSession session(std::move(transport_config));
  owner.publish(19);
  ASSERT_EQ(await_generation(session, 19).generation, 19U);

  const auto result = session.execute_primitive(
    XArmLifecycleCommand::kEnableMotion, "timeout-request-19", 19);

  EXPECT_EQ(
    result.state,
    XArmLifecycleResultState::kOutcomeUnknownAfterDispatch);
  EXPECT_FALSE(result.permitted);
  EXPECT_FALSE(result.attempted);
  EXPECT_EQ(result.reason, "xarm_ros_execute_service_timeout");
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (owner.execute_count() == 0 &&
    std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(owner.execute_count(), 1U);
}

TEST_F(
  ProductionPathTest,
  CoordinatorRequiresReconciliationAfterDispatchedResponseTimeout)
{
  FakeRosOwner owner(std::chrono::milliseconds(200));
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  port_config.transport.execute_timeout_ns = 50'000'000;
  XArmLifecycleRosPort port(std::move(port_config));
  owner.publish(20);

  const auto observation_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool refreshed = false;
  while (std::chrono::steady_clock::now() < observation_deadline) {
    refreshed = port.refresh_lifecycle(steady_now_ns());
    if (refreshed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(refreshed);

  const std::int64_t now_ns = steady_now_ns();
  ASSERT_TRUE(port.establish_authority_session(
      authority_session(now_ns), now_ns));
  const auto snapshot = port.lifecycle_snapshot(now_ns);
  robot_lifecycle::CommandRequest request;
  request.robot_id = snapshot.robot_id;
  request.request_id = "timeout-command-20";
  request.request_sequence = 1;
  request.action_id = "robot.stop_control";
  request.authority_profile = snapshot.authority_profile;
  request.authority_session_id = snapshot.authority_session->session_id;
  request.authority_session_generation =
    snapshot.authority_session->generation;
  request.principal_id = snapshot.authority_session->principal_id;
  request.expected_snapshot_generation = snapshot.generation;

  const auto decision = port.submit_lifecycle(request, now_ns + 1);

  ASSERT_EQ(
    decision.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  const auto completion_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  robot_lifecycle::LifecycleSnapshot after;
  while (std::chrono::steady_clock::now() < completion_deadline) {
    after = port.lifecycle_snapshot(steady_now_ns());
    if (after.pending_transaction.has_value() &&
      after.pending_transaction->phase ==
      robot_lifecycle::TransactionPhase::kOutcomeUnknown)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(after.pending_transaction.has_value());
  EXPECT_EQ(
    after.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);
  EXPECT_EQ(
    after.pending_transaction->result_reason,
    "xarm_ros_execute_service_timeout");
  EXPECT_TRUE(after.reconciliation_required);
  const auto audit = port.primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_EQ(
    audit.front().result.state,
    XArmLifecycleResultState::kOutcomeUnknownAfterDispatch);
  const auto owner_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (owner.execute_count() == 0 &&
    std::chrono::steady_clock::now() < owner_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(owner.execute_count(), 1U);
}

TEST_F(
  ProductionPathTest,
  SlowPrimitiveDoesNotStarveRefreshStatusOrAuthorityRenewal)
{
  FakeRosOwner owner(std::chrono::milliseconds(250));
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));
  owner.publish(42);

  const auto observation_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < observation_deadline &&
    !port.refresh_lifecycle(steady_now_ns()))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::int64_t now_ns = steady_now_ns();
  ASSERT_TRUE(port.establish_authority_session(
      authority_session(now_ns), now_ns));
  const auto initial = port.lifecycle_snapshot(now_ns);
  robot_lifecycle::CommandRequest request;
  request.robot_id = initial.robot_id;
  request.request_id = "slow-command-42";
  request.request_sequence = 1;
  request.action_id = "robot.stop_control";
  request.authority_profile = initial.authority_profile;
  request.authority_session_id = initial.authority_session->session_id;
  request.authority_session_generation =
    initial.authority_session->generation;
  request.principal_id = initial.authority_session->principal_id;
  request.expected_snapshot_generation = initial.generation;

  const auto submit_started = std::chrono::steady_clock::now();
  const auto decision = port.submit_lifecycle(request, now_ns + 1);
  const auto submit_elapsed =
    std::chrono::steady_clock::now() - submit_started;

  ASSERT_EQ(
    decision.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  EXPECT_LT(submit_elapsed, std::chrono::milliseconds(100));
  const auto dispatch_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (owner.execute_count() == 0 &&
    std::chrono::steady_clock::now() < dispatch_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(owner.execute_count(), 1U);

  const auto health_started = std::chrono::steady_clock::now();
  EXPECT_TRUE(port.refresh_lifecycle(steady_now_ns()));
  const auto during = port.lifecycle_snapshot(steady_now_ns());
  EXPECT_TRUE(during.pending_transaction.has_value());
  EXPECT_TRUE(port.establish_authority_session(
      authority_session(steady_now_ns()), steady_now_ns()));
  const auto health_elapsed =
    std::chrono::steady_clock::now() - health_started;
  EXPECT_LT(health_elapsed, std::chrono::milliseconds(100));

  const auto completion_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::vector<XArmPrimitiveAuditRecord> audit;
  while (std::chrono::steady_clock::now() < completion_deadline) {
    audit = port.primitive_audit_snapshot();
    if (!audit.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_EQ(owner.execute_count(), 1U);
  EXPECT_EQ(
    audit.front().result.state,
    XArmLifecycleResultState::kResponseReceived);
}

TEST_F(
  ProductionPathTest,
  SlowCommandGateLeaseDoesNotStarveLifecycleStatus)
{
  FakeRosOwner owner(
    std::chrono::milliseconds::zero(),
    std::chrono::milliseconds(200));
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));

  const auto initial_gate_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (owner.gate_update_count() < 1 &&
    std::chrono::steady_clock::now() < initial_gate_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(owner.gate_update_count(), 1U);
  const auto initial_gate_response_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (owner.gate_response_count() < 1 &&
    std::chrono::steady_clock::now() < initial_gate_response_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(owner.gate_response_count(), 1U);

  owner.publish(45);
  owner.publish_controller_activity(false);
  const auto inactive_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  robot_lifecycle::LifecycleSnapshot snapshot;
  while (std::chrono::steady_clock::now() < inactive_deadline) {
    static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
    snapshot = port.lifecycle_snapshot(steady_now_ns());
    if (snapshot.observed.trajectory_controller_active ==
      FactState::kFalse)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::int64_t now_ns = steady_now_ns();
  ASSERT_TRUE(port.establish_authority_session(
      authority_session(now_ns), now_ns));
  owner.publish_controller_activity(true);
  const auto open_dispatch_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (owner.gate_update_count() < 2 &&
    std::chrono::steady_clock::now() < open_dispatch_deadline)
  {
    static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GE(owner.gate_update_count(), 2U);

  const auto health_started = std::chrono::steady_clock::now();
  EXPECT_TRUE(port.refresh_lifecycle(steady_now_ns()));
  snapshot = port.lifecycle_snapshot(steady_now_ns());
  EXPECT_TRUE(port.establish_authority_session(
      authority_session(steady_now_ns()), steady_now_ns()));
  EXPECT_LT(
    std::chrono::steady_clock::now() - health_started,
    std::chrono::milliseconds(100));
  EXPECT_FALSE(snapshot.motion_admitted);

  const auto admitted_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < admitted_deadline) {
    static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
    snapshot = port.lifecycle_snapshot(steady_now_ns());
    if (snapshot.motion_admitted) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(snapshot.motion_admitted);
}

TEST_F(
  ProductionPathTest,
  CompletionAfterTransactionDeadlineIsAuditedButCannotMutateOutcome)
{
  FakeRosOwner owner(std::chrono::milliseconds(200));
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));
  owner.publish(43);

  const auto observation_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < observation_deadline &&
    !port.refresh_lifecycle(steady_now_ns()))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::int64_t now_ns = steady_now_ns();
  ASSERT_TRUE(port.establish_authority_session(
      authority_session(now_ns), now_ns));
  const auto initial = port.lifecycle_snapshot(now_ns);
  robot_lifecycle::CommandRequest request;
  request.robot_id = initial.robot_id;
  request.request_id = "deadline-command-43";
  request.request_sequence = 1;
  request.action_id = "robot.stop_control";
  request.authority_profile = initial.authority_profile;
  request.authority_session_id = initial.authority_session->session_id;
  request.authority_session_generation =
    initial.authority_session->generation;
  request.principal_id = initial.authority_session->principal_id;
  request.expected_snapshot_generation = initial.generation;
  ASSERT_EQ(
    port.submit_lifecycle(request, now_ns + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);

  const auto dispatch_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (owner.execute_count() == 0 &&
    std::chrono::steady_clock::now() < dispatch_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(owner.execute_count(), 1U);
  const std::int64_t after_deadline_ns = now_ns + 6'000'000'000;
  static_cast<void>(port.refresh_lifecycle(after_deadline_ns));
  auto timed_out = port.lifecycle_snapshot(after_deadline_ns);
  ASSERT_TRUE(timed_out.pending_transaction.has_value());
  ASSERT_EQ(
    timed_out.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);

  const auto completion_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (port.primitive_audit_snapshot().empty() &&
    std::chrono::steady_clock::now() < completion_deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto audit = port.primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_EQ(
    audit.front().result.state,
    XArmLifecycleResultState::kResponseReceived);
  timed_out = port.lifecycle_snapshot(after_deadline_ns);
  ASSERT_TRUE(timed_out.pending_transaction.has_value());
  EXPECT_EQ(
    timed_out.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);
  EXPECT_EQ(owner.execute_count(), 1U);
}

TEST_F(
  ProductionPathTest,
  PortShutdownJoinsOneInFlightPrimitiveWithoutRedispatch)
{
  FakeRosOwner owner(std::chrono::milliseconds(150));
  const auto shutdown_started = std::chrono::steady_clock::now();
  {
    XArmLifecycleRosPortConfig port_config;
    port_config.robot_id = "lite6-test";
    port_config.supervisor_instance_id = "lite6-test:lifecycle";
    port_config.transport = config();
    XArmLifecycleRosPort port(std::move(port_config));
    owner.publish(44);
    const auto observation_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < observation_deadline &&
      !port.refresh_lifecycle(steady_now_ns()))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::int64_t now_ns = steady_now_ns();
    ASSERT_TRUE(port.establish_authority_session(
        authority_session(now_ns), now_ns));
    const auto initial = port.lifecycle_snapshot(now_ns);
    robot_lifecycle::CommandRequest request;
    request.robot_id = initial.robot_id;
    request.request_id = "shutdown-in-flight-44";
    request.request_sequence = 1;
    request.action_id = "robot.stop_control";
    request.authority_profile = initial.authority_profile;
    request.authority_session_id = initial.authority_session->session_id;
    request.authority_session_generation =
      initial.authority_session->generation;
    request.principal_id = initial.authority_session->principal_id;
    request.expected_snapshot_generation = initial.generation;
    ASSERT_EQ(
      port.submit_lifecycle(request, now_ns + 1).disposition,
      robot_lifecycle::CommandDisposition::kAccepted);
    const auto dispatch_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (owner.execute_count() == 0 &&
      std::chrono::steady_clock::now() < dispatch_deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(owner.execute_count(), 1U);
  }
  EXPECT_LT(
    std::chrono::steady_clock::now() - shutdown_started,
    std::chrono::seconds(1));
  EXPECT_EQ(owner.execute_count(), 1U);
}

TEST_F(ProductionPathTest, StaleStateAndOwnerReplacementFailClosed)
{
  FakeRosOwner owner;
  XArmRosTransportSession session(config());
  owner.publish(5, "owner-session-1", false);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto observed = session.observe_vendor(steady_now_ns());
  EXPECT_EQ(observed.generation, 0U);
  EXPECT_NE(
    std::find(
      observed.blocking_reason_codes.begin(),
      observed.blocking_reason_codes.end(),
      "xarm_ros_state_stale"),
    observed.blocking_reason_codes.end());

  owner.publish(6, "owner-session-1");
  ASSERT_EQ(await_generation(session, 6).generation, 6U);
  owner.publish(1, "owner-session-2");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  observed = session.observe_vendor(steady_now_ns());
  EXPECT_EQ(observed.generation, 0U);
  EXPECT_NE(
    std::find(
      observed.blocking_reason_codes.begin(),
      observed.blocking_reason_codes.end(),
      "xarm_owner_session_replacement_requires_restart"),
    observed.blocking_reason_codes.end());
}

TEST_F(ProductionPathTest, OnlyMovingAndReadyFeedbackAreCommandReady)
{
  FakeRosOwner owner;
  XArmRosTransportSession session(config());

  owner.publish(10, "owner-session-1", true, 0);
  EXPECT_EQ(
    await_generation(session, 10).vendor_control_ready,
    FactState::kFalse);

  owner.publish(11, "owner-session-1", true, 2);
  EXPECT_EQ(
    await_generation(session, 11).vendor_control_ready,
    FactState::kTrue);

  owner.publish(12, "owner-session-1", true, 1);
  EXPECT_EQ(
    await_generation(session, 12).vendor_control_ready,
    FactState::kTrue);

  owner.publish(13, "owner-session-1", true, 5);
  EXPECT_EQ(
    await_generation(session, 13).vendor_control_ready,
    FactState::kFalse);
}

TEST_F(ProductionPathTest, GenericControlPortUsesOnlyTheRosOwner)
{
  FakeRosOwner owner;
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));
  owner.publish(23);

  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool refreshed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    refreshed = port.refresh_lifecycle(steady_now_ns());
    if (refreshed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(refreshed);
  const auto snapshot = port.lifecycle_snapshot(steady_now_ns());
  EXPECT_EQ(snapshot.robot_id, "lite6-test");
  EXPECT_EQ(snapshot.capabilities.size(), 14U);
  EXPECT_EQ(snapshot.observed.transport_connected, FactState::kTrue);
  EXPECT_FALSE(snapshot.motion_admitted);
  EXPECT_FALSE(snapshot.authority_session.has_value());
}

TEST_F(
  ProductionPathTest,
  AdmissionDriverPublishesClosedThenReadyFromTheSoleRosOwner)
{
  FakeRosOwner owner;
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));
  std::optional<robot_lifecycle::AdmissionPublication> captured;
  robot_lifecycle::AdmissionPublicationDriver driver(
    port,
    robot_lifecycle::AdmissionProjectorConfig{
        "lite6-test:provider:session-1",
        "lite6-test",
        "binding-generation-1",
        250'000'000,
        "runtime-generation-1",
        "provider-epoch-1",
        "supervisor-epoch-1"},
    [&captured](
      const robot_lifecycle::AdmissionPublication & publication)
    {
      captured = publication;
      return true;
    });

  owner.publish(24);
  owner.publish_controller_activity(false);
  const auto inactive_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < inactive_deadline) {
    static_cast<void>(driver.tick(steady_now_ns()));
    if (captured.has_value() &&
      std::find(
        captured->reason_codes.begin(),
        captured->reason_codes.end(),
        "trajectory_controller_rearm_required") !=
      captured->reason_codes.end())
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(captured.has_value());
  EXPECT_FALSE(captured->motion_admitted);
  const auto closed_lifecycle_generation =
    captured->lifecycle_generation;

  const std::int64_t authority_now_ns = steady_now_ns();
  ASSERT_TRUE(port.establish_authority_session(
      authority_session(authority_now_ns), authority_now_ns));
  owner.publish_controller_activity(true);
  const auto ready_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < ready_deadline) {
    static_cast<void>(driver.tick(steady_now_ns()));
    if (captured.has_value() && captured->motion_admitted) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(captured.has_value());
  EXPECT_TRUE(captured->controller_ready);
  EXPECT_TRUE(captured->motion_admitted);
  EXPECT_TRUE(captured->reason_codes.empty());
  EXPECT_GT(
    captured->lifecycle_generation,
    closed_lifecycle_generation);
  const auto ready_lifecycle_generation =
    captured->lifecycle_generation;
  const auto ready_publication_generation =
    captured->publication_generation;

  const auto renewal = driver.tick(steady_now_ns());
  EXPECT_TRUE(renewal.refresh_succeeded);
  EXPECT_TRUE(renewal.projection_succeeded);
  EXPECT_TRUE(renewal.sink_succeeded);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(
    captured->lifecycle_generation,
    ready_lifecycle_generation);
  EXPECT_GT(
    captured->publication_generation,
    ready_publication_generation);
  EXPECT_EQ(owner.execute_count(), 0U);
}

TEST_F(
  ProductionPathTest,
  LocalOperatorGatewayAuditsExactRosPrimitiveResult)
{
  FakeRosOwner owner;
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));
  owner.publish(31);

  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool refreshed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    refreshed = port.refresh_lifecycle(steady_now_ns());
    if (refreshed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(refreshed);

  const std::int64_t now_ns = steady_now_ns();
  const auto principal = authenticated_caller(now_ns);
  robot_lifecycle::LocalOperatorGatewayConfig gateway_config;
  gateway_config.robot_id = "lite6-test";
  robot_lifecycle::LocalOperatorGateway gateway(
    std::move(gateway_config), port);
  robot_lifecycle::LocalAuthoritySessionIntent session_intent;
  session_intent.session_id = "operator-session";
  session_intent.generation = 1;
  session_intent.valid_until_ns = now_ns + 5'000'000'000;
  session_intent.component_epoch = "operator-epoch-1";
  ASSERT_TRUE(
    gateway.open_session(
      principal, session_intent, now_ns).accepted);
  const auto snapshot_result = gateway.snapshot(principal, now_ns);
  ASSERT_TRUE(snapshot_result.snapshot.has_value());
  const auto & snapshot = *snapshot_result.snapshot;
  robot_lifecycle::LocalLifecycleCommandIntent command_intent;
  command_intent.request_id = "evidence-request-1";
  command_intent.request_sequence = 1;
  command_intent.action_id = "robot.stop_control";
  command_intent.authority_session_id = session_intent.session_id;
  command_intent.authority_session_generation =
    session_intent.generation;
  command_intent.expected_snapshot_generation = snapshot.generation;
  command_intent.confirmation_evidence =
    "CONFIRM:robot.stop_control";

  const auto mutation =
    gateway.submit(principal, command_intent, now_ns + 1);
  ASSERT_TRUE(mutation.command_decision.has_value());
  ASSERT_EQ(
    mutation.command_decision->disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  const auto gateway_audit = gateway.audit_snapshot();
  ASSERT_EQ(gateway_audit.size(), 2U);
  EXPECT_EQ(gateway_audit.front().operation, "open_session");
  EXPECT_TRUE(gateway_audit.front().accepted);
  EXPECT_EQ(gateway_audit.back().operation, "submit");
  EXPECT_TRUE(gateway_audit.back().accepted);
  const auto completion_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::vector<XArmPrimitiveAuditRecord> audit;
  while (std::chrono::steady_clock::now() < completion_deadline) {
    audit = port.primitive_audit_snapshot();
    if (!audit.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_EQ(audit.front().action_id, "robot.stop_control");
  EXPECT_EQ(
    audit.front().command,
    XArmLifecycleCommand::kSetStoppedState);
  EXPECT_EQ(
    audit.front().result.state,
    XArmLifecycleResultState::kResponseReceived);
  EXPECT_TRUE(audit.front().result.permitted);
  EXPECT_TRUE(audit.front().result.attempted);
  EXPECT_EQ(audit.front().result.return_code, 0);
  EXPECT_EQ(
    owner.last_command(),
    xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Request::
    SET_STOPPED_STATE);
  EXPECT_EQ(
    owner.last_request_id(),
    audit.front().transport_request_id);
  EXPECT_EQ(
    owner.last_request_id(),
    "mf:" + command_intent.request_id + ":step:0");
  EXPECT_EQ(
    owner.last_expected_generation(),
    audit.front().expected_observation_generation);
}

TEST_F(
  ProductionPathTest,
  ControllerActivityChangeRepublishesAnUnchangedOwnerGeneration)
{
  FakeRosOwner owner;
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));
  owner.publish(29);
  owner.publish_controller_activity(false);

  const auto inactive_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  robot_lifecycle::LifecycleSnapshot snapshot;
  while (std::chrono::steady_clock::now() < inactive_deadline) {
    static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
    snapshot = port.lifecycle_snapshot(steady_now_ns());
    if (snapshot.observed.trajectory_controller_active ==
      FactState::kFalse)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(
    snapshot.observed.trajectory_controller_active,
    FactState::kFalse);

  const std::int64_t now_ns = steady_now_ns();
  ASSERT_TRUE(port.establish_authority_session(
      authority_session(now_ns), now_ns));
  owner.publish_controller_activity(true);

  const auto active_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < active_deadline) {
    static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
    snapshot = port.lifecycle_snapshot(steady_now_ns());
    if (snapshot.motion_admitted) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(
    snapshot.observed.trajectory_controller_active,
    FactState::kTrue);
  EXPECT_TRUE(snapshot.motion_admitted);
  EXPECT_TRUE(owner.last_gate_open());
}

TEST_F(
  ProductionPathTest,
  HardwareActivityChangeRepublishesAnUnchangedOwnerGeneration)
{
  FakeRosOwner owner;
  XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = "lite6-test";
  port_config.supervisor_instance_id = "lite6-test:lifecycle";
  port_config.transport = config();
  XArmLifecycleRosPort port(std::move(port_config));
  owner.publish(30, "owner-session-1", true, 2, false);
  owner.publish_controller_activity(false);

  const auto inactive_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  robot_lifecycle::LifecycleSnapshot snapshot;
  while (std::chrono::steady_clock::now() < inactive_deadline) {
    static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
    snapshot = port.lifecycle_snapshot(steady_now_ns());
    if (snapshot.observed.hardware_component_active ==
      FactState::kFalse)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(
    snapshot.observed.hardware_component_active,
    FactState::kFalse);
  const auto inactive_lifecycle_generation = snapshot.generation;

  owner.publish(30, "owner-session-1", true, 2, true);
  const auto active_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < active_deadline) {
    static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
    snapshot = port.lifecycle_snapshot(steady_now_ns());
    if (snapshot.observed.hardware_component_active ==
      FactState::kTrue)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(
    snapshot.observed.hardware_component_active,
    FactState::kTrue);
  EXPECT_GT(snapshot.generation, inactive_lifecycle_generation);
}

TEST(
  ProductionCatalogTest,
  ContainsVendorControllerAndRos2ControlLifecycleOperations)
{
  const auto plans = make_lite6_xarm_action_plans();
  ASSERT_EQ(plans.size(), 12U);
  const std::vector<XArmLifecycleCommand> expected{
    XArmLifecycleCommand::kClearError,
    XArmLifecycleCommand::kClearWarning,
    XArmLifecycleCommand::kEnableMotion,
    XArmLifecycleCommand::kSelectJointServoMode,
    XArmLifecycleCommand::kSetReadyState,
    XArmLifecycleCommand::kActivateHardwareComponent,
    XArmLifecycleCommand::kActivateTrajectoryControllers,
    XArmLifecycleCommand::kDeactivateTrajectoryControllers,
    XArmLifecycleCommand::kDeactivateHardwareComponent,
    XArmLifecycleCommand::kSetStoppedState,
    XArmLifecycleCommand::kDisableMotion,
    XArmLifecycleCommand::kShutdownController,
  };
  std::vector<XArmLifecycleCommand> actual;
  for (const auto & plan : plans) {
    ASSERT_EQ(plan.steps.size(), 1U);
    actual.push_back(plan.steps.front());
  }
  EXPECT_EQ(actual, expected);
}

TEST(
  ProductionCatalogTest,
  CrossVendorTargetsReferenceOnlyAdapterOwnedPrimitiveStages)
{
  const auto targets = make_lite6_xarm_target_plans();
  ASSERT_EQ(targets.size(), 2U);
  EXPECT_EQ(
    targets[0].action_id,
    robot_lifecycle::kPrepareForMotionActionId);
  EXPECT_EQ(
    targets[1].action_id,
    robot_lifecycle::kMakeNoncommandableActionId);

  std::vector<std::string> prepare_stages;
  for (const auto & stage : targets[0].stages) {
    prepare_stages.push_back(stage.action_id);
  }
  EXPECT_EQ(
    prepare_stages,
    (std::vector<std::string>{
        "robot.enable",
        "robot.start_control",
        "robot.select_external_control",
        "robot.activate_hardware",
        "robot.deactivate_trajectory_controller",
        "robot.deactivate_hardware",
        "robot.activate_trajectory_controller",
        "robot.stop_control",
      }));

  std::vector<std::string> disable_stages;
  for (const auto & stage : targets[1].stages) {
    disable_stages.push_back(stage.action_id);
  }
  EXPECT_EQ(
    disable_stages,
    (std::vector<std::string>{
        "robot.deactivate_trajectory_controller",
        "robot.deactivate_hardware",
        "robot.disable",
        "robot.stop_control",
      }));
}

TEST(ProductionCatalogTest, EncodesDocumentedModeChangedToReadySequence)
{
  const auto plans = make_lite6_xarm_action_plans();
  const auto find_plan = [&plans](std::string_view action_id) {
      return std::find_if(
        plans.begin(), plans.end(),
        [action_id](const XArmActionPlan & plan) {
          return plan.action_id == action_id;
        });
    };
  const auto has_condition = [](
    const std::vector<XArmStateCondition> & conditions,
    XArmConditionKind kind,
    int expected_value)
    {
      return std::any_of(
        conditions.begin(), conditions.end(),
        [kind, expected_value](const XArmStateCondition & candidate) {
          return candidate.kind == kind &&
                 candidate.expected_value == expected_value;
        });
    };

  const auto select = find_plan("robot.select_external_control");
  ASSERT_NE(select, plans.end());
  EXPECT_TRUE(has_condition(
      select->preconditions, XArmConditionKind::kStateEquals, 4));
  EXPECT_TRUE(has_condition(
      select->postconditions, XArmConditionKind::kModeEquals, 1));
  EXPECT_TRUE(has_condition(
      select->postconditions, XArmConditionKind::kStateEquals, 5));
  EXPECT_TRUE(has_condition(
      select->postconditions,
      XArmConditionKind::kVendorControlReady, 0));

  const auto start = find_plan("robot.start_control");
  ASSERT_NE(start, plans.end());
  EXPECT_TRUE(has_condition(
      start->preconditions,
      XArmConditionKind::kReadyStateRequestEligible, 1));
  EXPECT_TRUE(has_condition(
      start->postconditions, XArmConditionKind::kModeEquals, 1));
  EXPECT_TRUE(has_condition(
      start->postconditions, XArmConditionKind::kStateEquals, 2));
  EXPECT_TRUE(has_condition(
      start->postconditions,
      XArmConditionKind::kVendorControlReady, 1));
}

}  // namespace
}  // namespace manyforge::xarm_control
