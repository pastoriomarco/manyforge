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

// R7 package 3: the robot_states subscription over the standard
// xarm_msgs/msg/RobotMsg, observed through the xArm vendor adapter.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>

namespace manyforge::xarm_control {
namespace {

using namespace std::chrono_literals;
using Cause = core::LifecycleBlockingCauseClassDTO;

class RosEnvironment final : public ::testing::Environment {
public:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};

[[maybe_unused]] auto *const kRosEnvironment =
    ::testing::AddGlobalTestEnvironment(new RosEnvironment);

[[nodiscard]] std::string unique_namespace() {
  std::random_device device;
  return "/xarm_transport_test_" + std::to_string(device()) + "/manyforge/lite6/xarm";
}

[[nodiscard]] xarm_msgs::msg::RobotMsg emergency_stop_report() {
  // The live report under a pressed emergency stop (R7 probe 2026-10-25):
  // C2, state 4, mode 0, no axis enabled, and all-zero angles.
  xarm_msgs::msg::RobotMsg message;
  message.state = 4;
  message.mode = 0;
  message.err = 2;
  message.warn = 0;
  message.mt_able = 0;
  message.mt_brake = 0;
  message.angle = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  return message;
}

TEST(XArmRobotStatesTransport, TopicIsTheDriverNamespacesRobotStates) {
  EXPECT_EQ(xarm_robot_states_topic("/manyforge/lite6/xarm"),
            "/manyforge/lite6/xarm/robot_states");
  for (const auto *invalid : {"", "manyforge/lite6/xarm", "/manyforge/lite6/xarm/"}) {
    EXPECT_THROW(static_cast<void>(xarm_robot_states_topic(invalid)),
                 std::invalid_argument)
        << invalid;
  }
}

TEST(XArmRobotStatesTransport, SubscribesAndStampsTheLatestReportAtReceipt) {
  core::FakeClock clock;
  clock.set(5'000'000'000LL);
  const auto ns = unique_namespace();
  auto node = std::make_shared<rclcpp::Node>("xarm_transport_test");
  auto transport =
      std::make_shared<XArmRobotStatesTransport>(*node, ns, clock);
  EXPECT_EQ(transport->topic(), ns + "/robot_states");
  EXPECT_FALSE(transport->observe().received_timestamp_ns.has_value());

  // The driver's publisher: reliable, depth 10.
  auto publisher = node->create_publisher<xarm_msgs::msg::RobotMsg>(
      ns + "/robot_states", 10);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!transport->observe().received_timestamp_ns.has_value() &&
         std::chrono::steady_clock::now() < deadline) {
    publisher->publish(emergency_stop_report());
    executor.spin_some(50ms);
  }
  const auto observed = transport->observe();
  ASSERT_TRUE(observed.received_timestamp_ns.has_value());
  EXPECT_EQ(*observed.received_timestamp_ns, 5'000'000'000LL);
  EXPECT_EQ(observed.state, 4);
  EXPECT_EQ(observed.mode, 0);
  EXPECT_EQ(observed.error_code, 2);
  EXPECT_EQ(observed.warning_code, 0);
  EXPECT_EQ(observed.motion_enable_mask, 0);

  // Classified through the adapter: emergency stop with its code.
  XArmBrandLifecyclePort port(transport);
  const auto status = port.Observe(clock.now());
  EXPECT_EQ(status.cause, Cause::kEmergencyStop);
  // 0.5 s without a report is stale.
  clock.set(clock.now() + kXArmStatusFreshnessNs + 1);
  EXPECT_EQ(port.Observe(clock.now()).cause, Cause::kAvailability);
  executor.remove_node(node);
}

TEST(XArmRobotStatesTransport, ReceiveKeepsTheStatusAndAnglesOfTheLatestReport) {
  core::FakeClock clock;
  clock.set(1'000);
  auto node = std::make_shared<rclcpp::Node>("xarm_transport_receive_test");
  XArmRobotStatesTransport transport(*node, unique_namespace(), clock);
  transport.Receive(emergency_stop_report());
  clock.set(2'000);
  auto armed = emergency_stop_report();
  armed.err = 0;
  armed.state = 2;
  armed.mode = 1;
  armed.mt_able = 0x3F;
  armed.warn = 7;
  armed.angle = {0.1F, -0.2F, 0.3F, 0.0F, 0.5F, 0.6F};
  transport.Receive(armed);
  const auto observed = transport.observe();
  EXPECT_EQ(observed.received_timestamp_ns, 2'000);
  EXPECT_EQ(observed.state, 2);
  EXPECT_EQ(observed.mode, 1);
  EXPECT_EQ(observed.error_code, 0);
  EXPECT_EQ(observed.warning_code, 7);
  EXPECT_EQ(observed.motion_enable_mask, 0x3F);
  // R7 package 4: the angles serve only the Enable gate's agreement check.
  ASSERT_EQ(observed.joint_angles_rad.size(), 6U);
  EXPECT_DOUBLE_EQ(observed.joint_angles_rad[1], static_cast<double>(-0.2F));
  EXPECT_DOUBLE_EQ(observed.joint_angles_rad[5], static_cast<double>(0.6F));
}

} // namespace
} // namespace manyforge::xarm_control
