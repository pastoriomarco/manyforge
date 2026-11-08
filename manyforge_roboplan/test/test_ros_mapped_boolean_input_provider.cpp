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

#include <atomic>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
#include <mutex>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/ros_mapped_boolean_input_provider.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>

namespace manyforge::ros2_control_execution {
class Ros2TransportOwnerTestPeer {
public:
  static std::thread::id time_critical_thread(Ros2TransportResources &owner) {
    std::lock_guard lock(owner.dispatch_mutex_);
    return owner.time_critical_thread_id_;
  }
};
} // namespace manyforge::ros2_control_execution

namespace manyforge::roboplan_backend {
namespace {
using namespace std::chrono_literals;
using GpioMessage = control_msgs::msg::DynamicInterfaceGroupValues;
const core::InterruptSourceId kSource("source.io");
const core::IoSourceIncarnation kIncarnation("activation.1");

class RecordingMappedIngress final : public core::IIoSignalIngress {
public:
  bool TryEnqueue(const core::IoSourceAcquisitionDTO &frame) noexcept override {
    frames.push_back(frame);
    return true;
  }
  void
  NotifyLoss(const core::IoIngressLossNotificationDTO &loss) noexcept override {
    losses.push_back(loss);
  }
  std::vector<core::IoSourceAcquisitionDTO> frames;
  std::vector<core::IoIngressLossNotificationDTO> losses;
};
core::IoSignalDescriptorDTO descriptor(std::string name) {
  return {{core::ControllableComponentId("component.io"), std::move(name)},
          kSource,
          core::IoSignalValueType::kBoolean,
          core::IoSignalDirection::kInput,
          core::IoSignalUpdateMode::kSampled,
          core::IoTransitionCapability::kSampledOnly,
          {core::IoFreshnessMode::kSampleAge, 100},
          std::nullopt,
          core::IoSignalLivenessMode::kPeriodic};
}
class RosMappedBooleanInputProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions options;
    options.context(context);
    options.use_global_arguments(false);
    node = std::make_shared<rclcpp::Node>("mapped_boolean_input_test", options);
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>(
        executor_options);
    executor->add_node(node);
    sink = std::make_shared<RecordingMappedIngress>();
  }
  void TearDown() override {
    if (provider)
      provider->Shutdown();
    if (executor) {
      executor->cancel();
      if (node) executor->remove_node(node);
    }
    provider.reset();
    executor.reset();
    node.reset();
    if (context && context->is_valid()) context->shutdown("mapped input fixture complete");
  }
  bool SpinUntil(const std::function<bool()> &condition) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!condition() && std::chrono::steady_clock::now() < until)
      executor->spin_once(5ms);
    return condition();
  }
  void Connect(std::vector<RosMappedBooleanInputSignal> signals,
               std::optional<std::uint64_t> generation = std::nullopt) {
    auto created = RosMappedBooleanInputProvider::Create(
        node, sink, clock,
        {kSource, kIncarnation, "/test/mapped_inputs", std::move(signals)}, qos,
        generation);
    ASSERT_TRUE(created.ok()) << created.error().message;
    provider = std::move(created.value());
  }
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr node;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  std::shared_ptr<RecordingMappedIngress> sink;
  std::unique_ptr<RosMappedBooleanInputProvider> provider;
  rclcpp::QoS qos{rclcpp::KeepLast(8)};
};

// OD-66: an owned (interrupt) input and its replacement run on the owner's
// time-critical thread, never on the owner thread.
TEST_F(RosMappedBooleanInputProviderTest, OwnedReplacementDeliversOnTimeCriticalThread) {
  class ThreadIngress final : public core::IIoSignalIngress {
  public:
    bool TryEnqueue(const core::IoSourceAcquisitionDTO &) noexcept override {
      if (!delivered.exchange(true))
        thread.set_value(std::this_thread::get_id());
      return true;
    }
    void
    NotifyLoss(const core::IoIngressLossNotificationDTO &) noexcept override {}
    std::atomic<bool> delivered{false};
    std::promise<std::thread::id> thread;
  };
  auto transport =
      ros2_control_execution::Ros2TransportResources::create(
          "manyforge_mapped_owned_test", "", 1U, [this] { return context; });
  auto ingress = std::make_shared<ThreadIngress>();
  auto observed = ingress->thread.get_future();
  auto publisher =
      node->create_publisher<GpioMessage>("/test/mapped_inputs", qos);
  auto made = RosMappedBooleanInputProvider::Create(
      transport->node(), ingress, clock,
      {kSource,
       kIncarnation,
       "/test/mapped_inputs",
       {{descriptor("a"), {"bank/a", "in.0"}}}},
      qos, 1U, transport);
  ASSERT_TRUE(made.ok());
  auto owned_provider = std::move(made.value());
  transport->start();
  std::thread::id owner_thread;
  transport->invoke([&] { owner_thread = std::this_thread::get_id(); });
  ASSERT_TRUE(owned_provider->Resume(2U));
  ASSERT_TRUE(
      SpinUntil([&] { return publisher->get_subscription_count() >= 1U; }));
  GpioMessage message;
  message.interface_groups = {"bank/a"};
  control_msgs::msg::InterfaceValue values;
  values.interface_names = {"in.0"};
  values.values = {1.0};
  message.interface_values.push_back(values);
  ASSERT_TRUE(SpinUntil([&] {
    publisher->publish(message);
    return observed.wait_for(0s) == std::future_status::ready;
  }));
  const auto delivered_on = observed.get();
  EXPECT_NE(delivered_on, owner_thread);
  EXPECT_EQ(delivered_on,
            ros2_control_execution::Ros2TransportOwnerTestPeer::time_critical_thread(*transport));
  auto drop = std::async(std::launch::async, [&] { owned_provider.reset(); });
  drop.get();
  transport->quiesce();
}

TEST_F(RosMappedBooleanInputProviderTest,
       GpioUsesExactSelectorsAndStrictBooleanValues) {
  auto publisher =
      node->create_publisher<GpioMessage>("/test/mapped_inputs", qos);
  Connect({{descriptor("a"), RosGpioBooleanInputSelector{"bank/a", "in.0"}},
           {descriptor("b"), RosGpioBooleanInputSelector{"bank/b", "in.0"}}});
  ASSERT_TRUE(provider);
  ASSERT_TRUE(
      SpinUntil([&] { return publisher->get_subscription_count() == 1U; }));
  GpioMessage message;
  message.interface_groups = {"bank/b", "bank/a"};
  control_msgs::msg::InterfaceValue values;
  values.interface_names = {"in.0"};
  values.values = {1.0};
  message.interface_values.push_back(values);
  values.values = {0.0};
  message.interface_values.push_back(values);
  publisher->publish(message);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  ASSERT_EQ(sink->frames.front().updates.size(), 2U);
  EXPECT_EQ(sink->frames.front().updates[0].value, false);
  EXPECT_EQ(sink->frames.front().updates[1].value, true);
  message.interface_values[0].values[0] =
      std::numeric_limits<double>::quiet_NaN();
  message.interface_values[1].values[0] = 0.5;
  publisher->publish(message);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 2U; }));
  for (const auto &update : sink->frames.back().updates) {
    EXPECT_EQ(update.validity, core::IoSampleValidity::kInvalid);
    EXPECT_FALSE(update.value);
  }
  message.interface_values[0].values.clear();
  publisher->publish(message);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 3U; }));
  EXPECT_EQ(sink->frames.back().updates[0].validity,
            core::IoSampleValidity::kInvalid);
  EXPECT_EQ(sink->frames.back().updates[1].validity,
            core::IoSampleValidity::kInvalid);
}

TEST_F(RosMappedBooleanInputProviderTest,
       ResumeRejectsBufferedOldChannelAndRequiresRealNewMessage) {
  auto publisher =
      node->create_publisher<GpioMessage>("/test/mapped_inputs", qos);
  Connect({{descriptor("a"), RosGpioBooleanInputSelector{"bank/a", "in.0"}}},
          1U);
  ASSERT_TRUE(provider);
  ASSERT_TRUE(
      SpinUntil([&] { return publisher->get_subscription_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(2U));
  GpioMessage message;
  message.interface_groups = {"bank/a"};
  control_msgs::msg::InterfaceValue values;
  values.interface_names = {"in.0"};
  values.values = {1.0};
  message.interface_values.push_back(values);
  publisher->publish(
      message); // old subscription channel, before executor entry
  ASSERT_TRUE(provider->Pause(3U));
  ASSERT_TRUE(provider->Resume(4U));
  executor->spin_some();
  EXPECT_TRUE(sink->frames.empty());
  publisher->publish(message);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  EXPECT_EQ(sink->frames.front().updates.front().value, true);
}

TEST_F(RosMappedBooleanInputProviderTest, MatchedPublisherCountTracksEachCompatibleSender) {
  Connect({{descriptor("a"), {"bank/a", "in.0"}}});
  ASSERT_TRUE(provider);
  EXPECT_EQ(provider->matched_publishers(), 0U);
  auto first = node->create_publisher<GpioMessage>("/test/mapped_inputs", qos);
  ASSERT_TRUE(SpinUntil([&] { return provider->matched_publishers() == 1U; }));
  auto second = node->create_publisher<GpioMessage>("/test/mapped_inputs", qos);
  ASSERT_TRUE(SpinUntil([&] { return provider->matched_publishers() == 2U; }));
  first.reset();
  ASSERT_TRUE(SpinUntil([&] { return provider->matched_publishers() == 1U; }));
  second.reset();
  EXPECT_TRUE(SpinUntil([&] { return provider->matched_publishers() == 0U; }));
}

TEST_F(RosMappedBooleanInputProviderTest,
       RefusesEmptySelectorsAndNonperiodicContracts) {
  const auto create = [&](std::vector<RosMappedBooleanInputSignal> signals) {
    return RosMappedBooleanInputProvider::Create(
        node, sink, clock,
        {kSource, kIncarnation, "/test/mapped_inputs", std::move(signals)},
        qos);
  };
  EXPECT_FALSE(
      create({{descriptor("a"), RosGpioBooleanInputSelector{"", "i"}}}).ok());
  auto change_only = descriptor("a");
  change_only.liveness = core::IoSignalLivenessMode::kChangeOnly;
  change_only.freshness = {core::IoFreshnessMode::kProviderValidity,
                           std::nullopt};
  EXPECT_FALSE(
      create({{change_only, RosGpioBooleanInputSelector{"g", "i"}}}).ok());
}
} // namespace
TEST_F(RosMappedBooleanInputProviderTest,
       SealedTypedTopicCannotBeRelativeOrRemapped) {
  auto configuration = RosMappedBooleanInputBinding{
      kSource,
      kIncarnation,
      "relative",
      {{descriptor("a"), RosGpioBooleanInputSelector{"g", "i"}}}};
  EXPECT_FALSE(RosMappedBooleanInputProvider::Create(node, sink, clock,
                                                     configuration, qos)
                   .ok());
  configuration.topic = "/test/mapped_inputs";
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  options.arguments(
      {"--ros-args", "-r", "/test/mapped_inputs:=/test/other_inputs"});
  auto remapped =
      std::make_shared<rclcpp::Node>("remapped_input_test", options);
  EXPECT_FALSE(RosMappedBooleanInputProvider::Create(remapped, sink, clock,
                                                     configuration, qos)
                   .ok());
  EXPECT_TRUE(sink->frames.empty());
  EXPECT_TRUE(sink->losses.empty());
}

TEST_F(RosMappedBooleanInputProviderTest,
       TransportRecoveryPreservesSenderAndRejectsStaleSnapshot) {
  class AtomicIngress final : public core::IIoSignalIngress {
  public:
    bool TryEnqueue(const core::IoSourceAcquisitionDTO &) noexcept override {
      ++count;
      return true;
    }
    void NotifyLoss(const core::IoIngressLossNotificationDTO &) noexcept override {}
    std::atomic<unsigned> count{0};
  };
  auto ingress = std::make_shared<AtomicIngress>();
  auto old = ros2_control_execution::Ros2TransportResources::create(
      "mapped_recovery_old", "", 1U, [this] { return context; });
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "mapped_recovery_new", "", 1U, [this] { return context; });
  auto made = RosMappedBooleanInputProvider::Create(
      old->node(), ingress, clock,
      {kSource,
       kIncarnation,
       "/test/mapped_inputs",
       {{descriptor("a"), {"bank/a", "in.0"}}}},
      qos, 1U, old);
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  const auto snapshot = provider->CaptureTransportRecovery();
  EXPECT_FALSE(
      RosMappedBooleanInputProvider::StageTransportRecovery(snapshot, {}).ok());
  EXPECT_TRUE(provider->MatchesTransportRecovery(*snapshot));
  auto staged = RosMappedBooleanInputProvider::StageTransportRecovery(
      snapshot, replacement);
  ASSERT_TRUE(staged.ok());
  EXPECT_TRUE(provider->MatchesTransportRecovery(*snapshot));
  EXPECT_FALSE(provider->status().accepting);
  provider->CommitTransportRecovery(*staged.value());
  RosMappedBooleanInputProvider::ActivateTransportRecovery(*staged.value());
  EXPECT_FALSE(provider->MatchesTransportRecovery(*snapshot));
  EXPECT_FALSE(provider->status().accepting);
  EXPECT_TRUE(provider->Resume(2U));
  auto publisher = node->create_publisher<GpioMessage>("/test/mapped_inputs", qos);
  GpioMessage message;
  message.interface_groups = {"bank/a"};
  control_msgs::msg::InterfaceValue values;
  values.interface_names = {"in.0"};
  values.values = {1.0};
  message.interface_values.push_back(values);
  replacement->start();
  ASSERT_TRUE(SpinUntil([&] {
    publisher->publish(message);
    return ingress->count.load() > 0U;
  }));
  provider.reset();
  replacement->quiesce();
}

} // namespace manyforge::roboplan_backend
