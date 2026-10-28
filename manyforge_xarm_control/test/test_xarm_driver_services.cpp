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

// R7 package 4 (D10): the operator's Recover reaches the unmodified driver's
// `clean_error` and `clean_warn` once each; no answer in time or a non-zero
// vendor return is an unknown outcome, never retried.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <manyforge_xarm_control/xarm_driver_services.hpp>

namespace manyforge::xarm_control {
namespace {

using namespace std::chrono_literals;
using Call = xarm_msgs::srv::Call;
using Outcome = XArmServiceCallOutcome;

class RosEnvironment final : public ::testing::Environment {
public:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};

[[maybe_unused]] auto *const kRosEnvironment =
    ::testing::AddGlobalTestEnvironment(new RosEnvironment);

[[nodiscard]] std::string unique_namespace() {
  std::random_device device;
  return "/xarm_services_test_" + std::to_string(device()) + "/manyforge/lite6/xarm";
}

/// The driver's two services with a configurable vendor return and delay,
/// and the client node, spun on one background executor.
struct World final {
  explicit World(const bool serve = true) : ns(unique_namespace()) {
    client_node = std::make_shared<rclcpp::Node>("xarm_services_client");
    server_node = std::make_shared<rclcpp::Node>("xarm_services_server");
    services = std::make_unique<XArmDriverServices>(*client_node, ns);
    if (serve) {
      clean_error = server_node->create_service<Call>(
          ns + "/clean_error",
          [this](const Call::Request::SharedPtr, Call::Response::SharedPtr response) {
            ++clean_error_calls;
            std::this_thread::sleep_for(delay.load());
            response->ret = error_return.load();
          });
      clean_warn = server_node->create_service<Call>(
          ns + "/clean_warn",
          [this](const Call::Request::SharedPtr, Call::Response::SharedPtr response) {
            ++clean_warn_calls;
            response->ret = warn_return.load();
          });
    }
    executor.add_node(client_node);
    executor.add_node(server_node);
    spinner = std::thread([this] { executor.spin(); });
    if (serve) {
      // The services' own clients see the servers on the same node graph.
      for (const auto *name : {"/clean_error", "/clean_warn"})
        EXPECT_TRUE(client_node->create_client<Call>(ns + name)->wait_for_service(10s));
    }
  }

  ~World() {
    executor.cancel();
    if (spinner.joinable()) spinner.join();
  }

  [[nodiscard]] XArmServiceCallResult Send(const XArmLifecycleCommand command,
                                           const std::chrono::nanoseconds timeout =
                                               kXArmServiceResponseTimeout) {
    return services->Call(command, timeout, std::stop_token{});
  }

  std::string ns;
  rclcpp::Node::SharedPtr client_node;
  rclcpp::Node::SharedPtr server_node;
  std::unique_ptr<XArmDriverServices> services;
  rclcpp::Service<Call>::SharedPtr clean_error;
  rclcpp::Service<Call>::SharedPtr clean_warn;
  std::atomic<int> clean_error_calls{0};
  std::atomic<int> clean_warn_calls{0};
  std::atomic<int> error_return{0};
  std::atomic<int> warn_return{0};
  std::atomic<std::chrono::milliseconds> delay{0ms};
  rclcpp::executors::MultiThreadedExecutor executor{rclcpp::ExecutorOptions(), 4U};
  std::thread spinner;
};

TEST(XArmDriverServices, EachRecoverStepIsOneCallAndOnlyZeroReturnSucceeds) {
  World world;
  const auto cleared = world.Send(XArmLifecycleCommand::kClearError);
  EXPECT_EQ(cleared.outcome, Outcome::kSucceeded);
  EXPECT_EQ(cleared.vendor_return, 0);
  EXPECT_TRUE(cleared.reason.empty());
  EXPECT_EQ(world.clean_error_calls.load(), 1);
  EXPECT_EQ(world.clean_warn_calls.load(), 0);

  // A non-zero vendor return is not a known failure: unknown, not retried.
  world.warn_return = 1;
  const auto warned = world.Send(XArmLifecycleCommand::kClearWarning);
  EXPECT_EQ(warned.outcome, Outcome::kUnknown);
  EXPECT_EQ(warned.reason, "xarm_service_returned_nonzero");
  EXPECT_EQ(warned.vendor_return, 1);
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(world.clean_warn_calls.load(), 1);
  EXPECT_EQ(world.clean_error_calls.load(), 1);
}

// D11: the tool payload reaches `set_tcp_load` once, in kg and millimetres.
TEST(XArmDriverServices, SetTcpLoadSendsKilogramsAndMillimetresOnce) {
  using SetTcpLoad = xarm_msgs::srv::SetTcpLoad;
  World world;
  std::atomic<int> calls{0};
  float weight{};
  std::vector<float> center;
  std::atomic<int> vendor_return{0};
  const auto server = world.server_node->create_service<SetTcpLoad>(
      world.ns + "/set_tcp_load",
      [&](const SetTcpLoad::Request::SharedPtr request,
          SetTcpLoad::Response::SharedPtr response) {
        ++calls;
        weight = request->weight;
        center = request->center_of_gravity;
        response->ret = vendor_return.load();
      });
  ASSERT_TRUE(world.client_node->create_client<SetTcpLoad>(world.ns + "/set_tcp_load")
                  ->wait_for_service(10s));
  const auto sent = world.services->SetTcpLoad(
      0.25, {0.0, 0.01, 0.05}, kXArmServiceResponseTimeout, std::stop_token{});
  EXPECT_EQ(sent.outcome, Outcome::kSucceeded);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_FLOAT_EQ(weight, 0.25F);
  ASSERT_EQ(center.size(), 3U);
  EXPECT_FLOAT_EQ(center[0], 0.0F);
  EXPECT_FLOAT_EQ(center[1], 10.0F);
  EXPECT_FLOAT_EQ(center[2], 50.0F);
  vendor_return = 1;
  const auto refused = world.services->SetTcpLoad(
      0.25, {0.0, 0.0, 0.0}, kXArmServiceResponseTimeout, std::stop_token{});
  EXPECT_EQ(refused.outcome, Outcome::kUnknown);
  EXPECT_EQ(refused.reason, "xarm_service_returned_nonzero");
  EXPECT_EQ(calls.load(), 2);
}

TEST(XArmDriverServices, NoAnswerInTimeIsAnUnknownOutcomeAndIsNeverRetried) {
  World world;
  world.delay = 600ms;
  const auto started = std::chrono::steady_clock::now();
  const auto result = world.Send(XArmLifecycleCommand::kClearError, 100ms);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
  EXPECT_EQ(result.outcome, Outcome::kUnknown);
  EXPECT_EQ(result.reason, "xarm_service_timeout");
  EXPECT_FALSE(result.vendor_return.has_value());
  // The late answer is dropped; the request was sent exactly once.
  std::this_thread::sleep_for(800ms);
  EXPECT_EQ(world.clean_error_calls.load(), 1);
}

TEST(XArmDriverServices, CancelledInFlightIsUnknown) {
  World world;
  world.delay = 600ms;
  std::stop_source stop;
  std::thread canceller([&] {
    std::this_thread::sleep_for(100ms);
    stop.request_stop();
  });
  const auto result = world.services->Call(XArmLifecycleCommand::kClearError,
                                            kXArmServiceResponseTimeout,
                                            stop.get_token());
  canceller.join();
  EXPECT_EQ(result.outcome, Outcome::kUnknown);
  EXPECT_EQ(result.reason, "xarm_service_cancelled");
}

TEST(XArmDriverServices, RefusesBeforeEffectWithoutServiceCommandOrBudget) {
  World unserved(false);
  const auto missing = unserved.Send(XArmLifecycleCommand::kClearError);
  EXPECT_EQ(missing.outcome, Outcome::kNotSent);
  EXPECT_EQ(missing.reason, "xarm_service_unavailable");

  World world;
  // Arming is the hardware activation, never a driver service.
  const auto arm = world.Send(XArmLifecycleCommand::kArm);
  EXPECT_EQ(arm.outcome, Outcome::kNotSent);
  EXPECT_EQ(arm.reason, "xarm_service_command_unsupported");
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = world.services->Call(
      XArmLifecycleCommand::kClearWarning, kXArmServiceResponseTimeout,
      stop.get_token());
  EXPECT_EQ(cancelled.outcome, Outcome::kNotSent);
  EXPECT_EQ(cancelled.reason, "xarm_service_cancelled");
  EXPECT_EQ(world.clean_error_calls.load(), 0);
  EXPECT_EQ(world.clean_warn_calls.load(), 0);
  EXPECT_THROW(XArmDriverServices(*world.client_node, "relative/xarm"),
               std::invalid_argument);
}

} // namespace
} // namespace manyforge::xarm_control
