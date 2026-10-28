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

#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/ros_mapped_boolean_input_provider.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <ur_msgs/msg/io_states.hpp>

namespace manyforge::roboplan_backend {
namespace {
using namespace std::chrono_literals;
using UrMessage = ur_msgs::msg::IOStates;
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
    executor->cancel();
    executor->remove_node(node);
    provider.reset();
    executor.reset();
    node.reset();
    context->shutdown("mapped input fixture complete");
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
  core::FakeClock clock;
  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr node;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  std::shared_ptr<RecordingMappedIngress> sink;
  std::unique_ptr<RosMappedBooleanInputProvider> provider;
  rclcpp::QoS qos{rclcpp::KeepLast(8)};
};

TEST_F(RosMappedBooleanInputProviderTest,
       UrMessageProducesOneAtomicFrameWithoutBorrowingOtherPins) {
  auto publisher =
      node->create_publisher<UrMessage>("/test/mapped_inputs", qos);
  Connect({{descriptor("pin0"), UrDigitalInputSelector{0}},
           {descriptor("pin17"), UrDigitalInputSelector{17}}});
  ASSERT_TRUE(provider);
  ASSERT_TRUE(
      SpinUntil([&] { return publisher->get_subscription_count() == 1U; }));
  EXPECT_TRUE(sink->frames.empty());
  UrMessage message;
  ur_msgs::msg::Digital pin;
  pin.pin = 0;
  pin.state = false;
  message.digital_in_states.push_back(pin);
  pin.pin = 17;
  pin.state = true;
  message.digital_in_states.push_back(pin);
  // Output/flag arrays cannot masquerade as input evidence.
  message.digital_out_states = message.digital_in_states;
  clock.set(71);
  publisher->publish(message);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  const auto &first = sink->frames.front();
  EXPECT_EQ(first.trusted_received_at_ns, 71);
  EXPECT_EQ(first.source_id, kSource);
  EXPECT_EQ(first.incarnation, kIncarnation);
  EXPECT_EQ(first.source_sequence, 1U);
  ASSERT_EQ(first.updates.size(), 2U);
  EXPECT_EQ(first.updates[0].value, false);
  EXPECT_EQ(first.updates[1].value, true);
  message.digital_in_states.pop_back();
  message.digital_in_states.push_back(message.digital_in_states.front());
  publisher->publish(message);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 2U; }));
  for (const auto &update : sink->frames.back().updates) {
    EXPECT_EQ(update.validity, core::IoSampleValidity::kInvalid);
    EXPECT_FALSE(update.value);
    EXPECT_FALSE(update.error);
  }
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
      node->create_publisher<UrMessage>("/test/mapped_inputs", qos);
  Connect({{descriptor("pin0"), UrDigitalInputSelector{0}}}, 1U);
  ASSERT_TRUE(provider);
  ASSERT_TRUE(
      SpinUntil([&] { return publisher->get_subscription_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(2U));
  UrMessage message;
  ur_msgs::msg::Digital pin;
  pin.pin = 0;
  pin.state = true;
  message.digital_in_states.push_back(pin);
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

TEST_F(RosMappedBooleanInputProviderTest,
       RefusesMixedMessageKindsAndNonperiodicContracts) {
  const auto create = [&](std::vector<RosMappedBooleanInputSignal> signals) {
    return RosMappedBooleanInputProvider::Create(
        node, sink, clock,
        {kSource, kIncarnation, "/test/mapped_inputs", std::move(signals)},
        qos);
  };
  EXPECT_FALSE(
      create({{descriptor("a"), UrDigitalInputSelector{0}},
              {descriptor("b"), RosGpioBooleanInputSelector{"g", "i"}}})
          .ok());
  EXPECT_FALSE(create({{descriptor("a"), UrDigitalInputSelector{18}}}).ok());
  auto change_only = descriptor("a");
  change_only.liveness = core::IoSignalLivenessMode::kChangeOnly;
  change_only.freshness = {core::IoFreshnessMode::kProviderValidity,
                           std::nullopt};
  EXPECT_FALSE(create({{change_only, UrDigitalInputSelector{0}}}).ok());
}
} // namespace
TEST_F(RosMappedBooleanInputProviderTest,
       SealedTypedTopicCannotBeRelativeOrRemapped) {
  auto configuration = RosMappedBooleanInputBinding{
      kSource,
      kIncarnation,
      "relative",
      {{descriptor("a"), UrDigitalInputSelector{0}}}};
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

} // namespace manyforge::roboplan_backend
