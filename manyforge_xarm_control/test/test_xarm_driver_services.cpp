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

// Each driver service the adapters call is one request; no answer in time or a
// non-zero vendor return is an unknown outcome, never retried.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numbers>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
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

/// The driver's `clean_error` with a configurable vendor return and delay,
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
    }
    executor.add_node(client_node);
    executor.add_node(server_node);
    spinner = std::thread([this] { executor.spin(); });
    if (serve) {
      // The services' own clients see the servers on the same node graph.
      EXPECT_TRUE(client_node->create_client<Call>(ns + "/clean_error")->wait_for_service(10s));
    }
  }

  ~World() {
    executor.cancel();
    if (spinner.joinable()) spinner.join();
  }

  [[nodiscard]] XArmServiceCallResult Clear(const std::chrono::nanoseconds timeout =
                                                kXArmServiceResponseTimeout) {
    return services->CleanError(timeout, std::stop_token{});
  }

  std::string ns;
  rclcpp::Node::SharedPtr client_node;
  rclcpp::Node::SharedPtr server_node;
  std::unique_ptr<XArmDriverServices> services;
  rclcpp::Service<Call>::SharedPtr clean_error;
  std::atomic<int> clean_error_calls{0};
  std::atomic<int> error_return{0};
  std::atomic<std::chrono::milliseconds> delay{0ms};
  rclcpp::executors::MultiThreadedExecutor executor{rclcpp::ExecutorOptions(), 4U};
  std::thread spinner;
};

TEST(XArmDriverServices, CleanErrorIsOneCallAndReportsTheVendorReturn) {
  World world;
  const auto cleared = world.Clear();
  EXPECT_EQ(cleared.outcome, Outcome::kSucceeded);
  EXPECT_EQ(cleared.vendor_return, 0);
  EXPECT_TRUE(cleared.reason.empty());
  EXPECT_EQ(world.clean_error_calls.load(), 1);

  // `ret=1` (an error still present) is answered, never retried.
  world.error_return = 1;
  const auto remaining = world.Clear();
  EXPECT_EQ(remaining.outcome, Outcome::kUnknown);
  EXPECT_EQ(remaining.reason, "xarm_service_returned_nonzero");
  EXPECT_EQ(remaining.vendor_return, 1);
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(world.clean_error_calls.load(), 2);
}

// The owner's Recover clears a warning with `clean_warn`: one call on its own
// service name.
TEST(XArmDriverServices, CleanWarnIsOneCallOnItsOwnService) {
  World world;
  std::atomic<int> calls{0};
  const auto server = world.server_node->create_service<Call>(
      world.ns + "/clean_warn",
      [&calls](const Call::Request::SharedPtr, Call::Response::SharedPtr response) {
        ++calls;
        response->ret = 0;
      });
  ASSERT_TRUE(world.client_node->create_client<Call>(world.ns + "/clean_warn")
                  ->wait_for_service(10s));
  const auto cleared = world.services->CleanWarn(kXArmServiceResponseTimeout, std::stop_token{});
  EXPECT_EQ(cleared.outcome, Outcome::kSucceeded);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(world.clean_error_calls.load(), 0);
}

// TOOLCTX: the payload reaches `set_tcp_load` once, in kg and millimetres.
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

// TOOLCTX: the TCP reaches `set_tcp_offset` once, in mm and radians.
TEST(XArmDriverServices, SetTcpOffsetSendsMillimetresAndRadiansOnce) {
  using SetFloat32List = xarm_msgs::srv::SetFloat32List;
  World world;
  std::atomic<int> calls{0};
  std::vector<float> data;
  std::atomic<int> vendor_return{0};
  const auto server = world.server_node->create_service<SetFloat32List>(
      world.ns + "/set_tcp_offset",
      [&](const SetFloat32List::Request::SharedPtr request,
          SetFloat32List::Response::SharedPtr response) {
        ++calls;
        data = request->datas;
        response->ret = static_cast<std::int16_t>(vendor_return.load());
      });
  ASSERT_TRUE(world.client_node->create_client<SetFloat32List>(world.ns + "/set_tcp_offset")
                  ->wait_for_service(10s));
  const double h = std::sqrt(0.5);
  const auto sent = world.services->SetTcpOffset(
      {{0.0, 0.01, 0.12}, {0.0, 0.0, h, h}}, kXArmServiceResponseTimeout,
      std::stop_token{});
  EXPECT_EQ(sent.outcome, Outcome::kSucceeded);
  EXPECT_EQ(calls.load(), 1);
  ASSERT_EQ(data.size(), 6U);
  EXPECT_FLOAT_EQ(data[0], 0.0F);
  EXPECT_FLOAT_EQ(data[1], 10.0F);
  EXPECT_FLOAT_EQ(data[2], 120.0F);
  EXPECT_NEAR(data[3], 0.0F, 1e-6F);
  EXPECT_NEAR(data[4], 0.0F, 1e-6F);
  EXPECT_NEAR(data[5], static_cast<float>(std::numbers::pi / 2.0), 1e-6F);
  vendor_return = 1;
  const auto refused = world.services->SetTcpOffset(
      {}, kXArmServiceResponseTimeout, std::stop_token{});
  EXPECT_EQ(refused.outcome, Outcome::kUnknown);
  EXPECT_EQ(refused.reason, "xarm_service_returned_nonzero");
  EXPECT_EQ(calls.load(), 2);
}

// `set_state`, `system_control` and the digital outputs: one request each,
// with the bank's own service; a non-zero return is reported, never retried.
TEST(XArmDriverServices, Int16AndDigitalOutputServicesSendOneRequestEach) {
  using SetInt16 = xarm_msgs::srv::SetInt16;
  using SetDigitalIO = xarm_msgs::srv::SetDigitalIO;
  World world;
  std::vector<std::pair<std::string, int>> received;
  std::mutex mutex;
  std::atomic<int> vendor_return{0};
  const auto serve = [&](const std::string &name) {
    return world.server_node->create_service<SetInt16>(
        world.ns + "/" + name,
        [&, name](const SetInt16::Request::SharedPtr request,
                  SetInt16::Response::SharedPtr response) {
          std::lock_guard lock(mutex);
          received.emplace_back(name, request->data);
          response->ret = static_cast<std::int16_t>(vendor_return.load());
        });
  };
  const auto serve_io = [&](const std::string &name) {
    return world.server_node->create_service<SetDigitalIO>(
        world.ns + "/" + name,
        [&, name](const SetDigitalIO::Request::SharedPtr request,
                  SetDigitalIO::Response::SharedPtr response) {
          std::lock_guard lock(mutex);
          received.emplace_back(name, request->ionum * 10 + request->value);
          EXPECT_FLOAT_EQ(request->delay_sec, 0.0F);
          response->ret = static_cast<std::int16_t>(vendor_return.load());
        });
  };
  const auto state_server = serve("set_state");
  const auto system_server = serve("system_control");
  const auto cgpio_server = serve_io("set_cgpio_digital");
  const auto tgpio_server = serve_io("set_tgpio_digital");
  for (const auto *name : {"/set_state", "/system_control"})
    ASSERT_TRUE(world.client_node->create_client<SetInt16>(world.ns + name)
                    ->wait_for_service(10s));
  for (const auto *name : {"/set_cgpio_digital", "/set_tgpio_digital"})
    ASSERT_TRUE(world.client_node->create_client<SetDigitalIO>(world.ns + name)
                    ->wait_for_service(10s));
  const auto cap = kXArmServiceResponseTimeout;
  EXPECT_EQ(world.services->SetState(0, cap, std::stop_token{}).outcome, Outcome::kSucceeded);
  EXPECT_EQ(world.services->SystemControl(kXArmSystemReboot, cap, std::stop_token{}).outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(world.services->SetDigitalOutput(XArmIoBank::kController, 1, true, cap,
                                             std::stop_token{})
                .outcome,
            Outcome::kSucceeded);
  vendor_return = 1;
  const auto refused = world.services->SetDigitalOutput(XArmIoBank::kTool, 0, false, cap,
                                                        std::stop_token{});
  EXPECT_EQ(refused.outcome, Outcome::kUnknown);
  EXPECT_EQ(refused.reason, "xarm_service_returned_nonzero");
  EXPECT_EQ(refused.vendor_return, 1);
  std::lock_guard lock(mutex);
  EXPECT_EQ(received, (std::vector<std::pair<std::string, int>>{{"set_state", 0},
                                                               {"system_control", 2},
                                                               {"set_cgpio_digital", 11},
                                                               {"set_tgpio_digital", 0}}));
}

TEST(XArmDriverServices, NoAnswerInTimeIsAnUnknownOutcomeAndIsNeverRetried) {
  World world;
  world.delay = 600ms;
  const auto started = std::chrono::steady_clock::now();
  const auto result = world.Clear(100ms);
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
  const auto result =
      world.services->CleanError(kXArmServiceResponseTimeout, stop.get_token());
  canceller.join();
  EXPECT_EQ(result.outcome, Outcome::kUnknown);
  EXPECT_EQ(result.reason, "xarm_service_cancelled");
}

TEST(XArmDriverServices, RefusesBeforeEffectWithoutServiceOrWhenCancelled) {
  World unserved(false);
  const auto missing = unserved.Clear();
  EXPECT_EQ(missing.outcome, Outcome::kNotSent);
  EXPECT_EQ(missing.reason, "xarm_service_unavailable");

  World world;
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled =
      world.services->CleanError(kXArmServiceResponseTimeout, stop.get_token());
  EXPECT_EQ(cancelled.outcome, Outcome::kNotSent);
  EXPECT_EQ(cancelled.reason, "xarm_service_cancelled");
  EXPECT_EQ(world.clean_error_calls.load(), 0);
  EXPECT_THROW(XArmDriverServices(*world.client_node, "relative/xarm"),
               std::invalid_argument);
}

} // namespace
} // namespace manyforge::xarm_control
