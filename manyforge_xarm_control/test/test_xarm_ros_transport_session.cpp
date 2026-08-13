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
#include <string>
#include <thread>
#include <utility>

#include <rclcpp/rclcpp.hpp>

#include <manyforge_xarm_control/xarm_ros_transport_session.hpp>

namespace manyforge::xarm_control {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kOwnerId = "lite6-test:sole-owner";
constexpr std::string_view kStateTopic = "/l06/xarm/supervised/state";
constexpr std::string_view kExecuteService = "/l06/xarm/supervised/execute";
constexpr std::string_view kGateService = "/l06/xarm/supervised/gate";
constexpr std::string_view kShutdownService = "/l06/xarm/supervised/shutdown";

[[nodiscard]] std::int64_t steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class RestrictedOwner final {
public:
  explicit RestrictedOwner(std::chrono::milliseconds execute_delay = 0ms)
      : execute_delay_(execute_delay),
        node_(std::make_shared<rclcpp::Node>("l06_restricted_xarm_owner")) {
    const auto qos =
        rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    publisher_ =
        node_->create_publisher<xarm_msgs::msg::SupervisedLifecycleState>(
            std::string(kStateTopic), qos);
    execute_service_ = node_->create_service<
        xarm_msgs::srv::ExecuteSupervisedLifecycleCommand>(
        std::string(kExecuteService),
        [this](const std::shared_ptr<
                   xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Request>
                   request,
               std::shared_ptr<
                   xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Response>
                   response) {
          execute_count_.fetch_add(1U, std::memory_order_relaxed);
          std::this_thread::sleep_for(execute_delay_);
          response->permitted = request->expected_generation == generation_;
          response->attempted = response->permitted;
          response->return_code = response->permitted ? 0 : -1;
          response->reason =
              response->permitted ? "command_succeeded" : "generation_mismatch";
          response->observed_generation = generation_;
        });
    gate_service_ = node_->create_service<
        xarm_msgs::srv::SetSupervisedCommandGate>(
        std::string(kGateService),
        [this](
            const std::shared_ptr<
                xarm_msgs::srv::SetSupervisedCommandGate::Request>
                request,
            std::shared_ptr<xarm_msgs::srv::SetSupervisedCommandGate::Response>
                response) {
          gate_count_.fetch_add(1U, std::memory_order_relaxed);
          response->accepted =
              request->expected_generation == generation_ &&
              (!request->open || request->lease_duration_ns > 0);
          response->reason =
              response->accepted ? "gate_updated" : "gate_rejected";
          response->observed_generation = generation_;
          response->valid_until_steady_time_ns =
              request->open ? steady_now_ns() + request->lease_duration_ns : 0;
        });
    shutdown_service_ =
        node_->create_service<xarm_msgs::srv::ShutdownSupervisedController>(
            std::string(kShutdownService),
            [this](const std::shared_ptr<
                       xarm_msgs::srv::ShutdownSupervisedController::Request>
                       request,
                   std::shared_ptr<
                       xarm_msgs::srv::ShutdownSupervisedController::Response>
                       response) {
              shutdown_count_.fetch_add(1U, std::memory_order_relaxed);
              response->permitted =
                  request->expected_generation == generation_ &&
                  request->confirmation == "SHUTDOWN_CONTROLLER";
              response->attempted = response->permitted;
              response->return_code = response->permitted ? 0 : -1;
              response->reason = response->permitted
                                     ? "controller_shutdown_vendor_accepted"
                                     : "shutdown_rejected";
              response->observed_generation = generation_;
              response->process_restart_required = response->attempted;
            });
    executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
    executor_->add_node(node_);
    thread_ = std::thread([this] {
      while (!stop_.load(std::memory_order_acquire)) {
        executor_->spin_some(2ms);
        std::this_thread::sleep_for(1ms);
      }
    });
  }

  ~RestrictedOwner() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
      thread_.join();
    }
    executor_->remove_node(node_);
  }

  void Publish(std::uint64_t generation, std::string session = "source-epoch.1",
               bool fresh = true) {
    generation_ = generation;
    xarm_msgs::msg::SupervisedLifecycleState message;
    message.owner_id = kOwnerId;
    message.session_id = std::move(session);
    message.owner_publication_sequence = ++publication_sequence_;
    message.generation = generation;
    const auto now = steady_now_ns();
    message.owner_published_steady_time_ns = now;
    message.source_steady_time_ns = fresh ? now : now - 1'000'000'000;
    message.fresh_until_steady_time_ns = fresh ? now + 500'000'000 : now - 1;
    message.transport_connected = true;
    message.report_connected = true;
    message.identity_verified = true;
    message.report_received = true;
    message.position_valid = true;
    message.identity_axis = 6;
    message.identity_device_type = 9;
    message.state = 2;
    message.mode = 1;
    message.brake_mask = 63;
    message.servo_enable_mask = 63;
    message.error_code = 0;
    message.warning_code = 0;
    message.report_sample_count = generation;
    message.joint_read_attempt_count = generation;
    message.joint_read_success_count = generation;
    publisher_->publish(message);
  }

  [[nodiscard]] std::size_t execute_count() const noexcept {
    return execute_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t gate_count() const noexcept {
    return gate_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t shutdown_count() const noexcept {
    return shutdown_count_.load(std::memory_order_relaxed);
  }

private:
  std::chrono::milliseconds execute_delay_;
  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<xarm_msgs::msg::SupervisedLifecycleState>::SharedPtr
      publisher_;
  rclcpp::Service<xarm_msgs::srv::ExecuteSupervisedLifecycleCommand>::SharedPtr
      execute_service_;
  rclcpp::Service<xarm_msgs::srv::SetSupervisedCommandGate>::SharedPtr
      gate_service_;
  rclcpp::Service<xarm_msgs::srv::ShutdownSupervisedController>::SharedPtr
      shutdown_service_;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> execute_count_{0};
  std::atomic<std::size_t> gate_count_{0};
  std::atomic<std::size_t> shutdown_count_{0};
  std::atomic<std::uint64_t> generation_{0};
  std::uint64_t publication_sequence_ = 0;
};

[[nodiscard]] XArmRosTransportSessionConfig Config() {
  XArmRosTransportSessionConfig result;
  result.owner_id = kOwnerId;
  result.state_topic = kStateTopic;
  result.execute_service = kExecuteService;
  result.command_gate_service = kGateService;
  result.shutdown_controller_service = kShutdownService;
  result.observation_max_age_ns = 500'000'000;
  result.owner_progress_max_age_ns = 500'000'000;
  result.service_timeout_ns = 500'000'000;
  result.execute_timeout_ns = 500'000'000;
  return result;
}

[[nodiscard]] XArmObservedState Await(XArmRosTransportSession &session,
                                      std::uint64_t generation) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  XArmObservedState result;
  while (std::chrono::steady_clock::now() < deadline) {
    result = session.observe_vendor(steady_now_ns());
    if (result.generation == generation) {
      return result;
    }
    std::this_thread::sleep_for(5ms);
  }
  return result;
}

class XArmRosTransportSessionTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    if (!rclcpp::ok()) {
      int argc = 0;
      rclcpp::init(argc, nullptr);
    }
  }
  static void TearDownTestSuite() { rclcpp::shutdown(); }
};

TEST_F(XArmRosTransportSessionTest, MapsFreshStateAndExactNamedOperations) {
  RestrictedOwner owner;
  XArmRosTransportSession session(Config());
  owner.Publish(17);
  const auto observed = Await(session, 17);
  ASSERT_EQ(observed.generation, 17U);
  EXPECT_TRUE(observed.transport_owner_progress_healthy);
  EXPECT_EQ(observed.transport_connected, FactState::kTrue);
  EXPECT_EQ(observed.identity_verified, FactState::kTrue);
  EXPECT_EQ(observed.motion_enabled, FactState::kTrue);
  EXPECT_EQ(observed.vendor_control_ready, FactState::kTrue);

  const auto result = session.execute_primitive(
      XArmLifecycleCommand::kDisableMotion, "request.17", 17);
  EXPECT_EQ(result.state, XArmLifecycleResultState::kResponseReceived);
  EXPECT_TRUE(result.permitted);
  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(owner.execute_count(), 1U);
  EXPECT_TRUE(
      session.set_command_gate(true, steady_now_ns() + 100'000'000, 17));
  EXPECT_EQ(owner.gate_count(), 1U);
}

TEST_F(XArmRosTransportSessionTest,
       TimeoutAfterDispatchIsUncertainAndNeverRetried) {
  RestrictedOwner owner(200ms);
  auto config = Config();
  config.execute_timeout_ns = 50'000'000;
  XArmRosTransportSession session(std::move(config));
  owner.Publish(19);
  ASSERT_EQ(Await(session, 19).generation, 19U);
  const auto result = session.execute_primitive(
      XArmLifecycleCommand::kEnableMotion, "request.19", 19);
  EXPECT_EQ(result.state,
            XArmLifecycleResultState::kOutcomeUnknownAfterDispatch);
  EXPECT_EQ(result.reason, "xarm_ros_execute_service_timeout");
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (owner.execute_count() == 0U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(owner.execute_count(), 1U);
}

TEST_F(XArmRosTransportSessionTest, StaleStateAndOwnerReplacementFailClosed) {
  RestrictedOwner owner;
  XArmRosTransportSession session(Config());
  owner.Publish(5, "source-epoch.1", false);
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(session.observe_vendor(steady_now_ns()).generation, 0U);
  owner.Publish(6);
  ASSERT_EQ(Await(session, 6).generation, 6U);
  owner.Publish(1, "source-epoch.2");
  std::this_thread::sleep_for(30ms);
  const auto replaced = session.observe_vendor(steady_now_ns());
  EXPECT_EQ(replaced.generation, 0U);
  EXPECT_NE(std::find(replaced.blocking_reason_codes.begin(),
                      replaced.blocking_reason_codes.end(),
                      "xarm_owner_session_replacement_requires_restart"),
            replaced.blocking_reason_codes.end());
}

TEST_F(XArmRosTransportSessionTest, ShutdownUsesOnlyDedicatedConfirmedService) {
  RestrictedOwner owner;
  XArmRosTransportSession session(Config());
  owner.Publish(41);
  ASSERT_EQ(Await(session, 41).generation, 41U);
  const auto result = session.execute_primitive(
      XArmLifecycleCommand::kShutdownController, "shutdown.41", 41);
  EXPECT_EQ(result.state, XArmLifecycleResultState::kResponseReceived);
  EXPECT_TRUE(result.process_restart_required);
  EXPECT_EQ(owner.shutdown_count(), 1U);
  EXPECT_EQ(owner.execute_count(), 0U);
}

} // namespace
} // namespace manyforge::xarm_control
