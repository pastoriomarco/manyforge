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

#include <manyforge_roboplan/ros_boolean_input_provider.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <manyforge_core/time/fake_clock.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {
using namespace std::chrono_literals;

struct Envelope {
  core::IoSourceAcquisitionDTO acquisition;
  std::size_t order;
};

class RecordingIngress final : public core::IIoSignalIngress {
public:
  bool TryEnqueue(
      const core::IoSourceAcquisitionDTO &acquisition) noexcept override {
    if (advance_on_enqueue)
      advance_on_enqueue->advance(1000);
    if (reject || count == samples.size()) {
      // Model the shared ingress contract: it records capacity loss itself.
      if (record_rejection_loss)
        NotifyLoss(
            {acquisition.source_id, acquisition.incarnation,
             acquisition.trusted_received_at_ns, acquisition.source_sequence,
             core::IoIngressLossReason::kHostIngressRejected, std::nullopt});
      return false;
    }
    samples[count++] = Envelope{acquisition, ++order};
    return true;
  }
  void
  NotifyLoss(const core::IoIngressLossNotificationDTO &loss) noexcept override {
    if (loss_count < losses.size()) {
      loss_orders[loss_count] = ++order;
      losses[loss_count++] = loss;
    }
  }
  bool reject = false;
  bool record_rejection_loss = true;
  core::FakeClock *advance_on_enqueue = nullptr;
  std::array<std::optional<Envelope>, 16> samples;
  std::array<std::optional<core::IoIngressLossNotificationDTO>, 16> losses;
  std::array<std::size_t, 16> loss_orders{};
  std::size_t count = 0;
  std::size_t loss_count = 0;
  std::size_t order = 0;
};

class RosBooleanInputProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions node_options;
    node_options.context(context);
    node = std::make_shared<rclcpp::Node>("manyforge_boolean_input_test",
                                          node_options);
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>(
        executor_options);
    executor->add_node(node);
    sink = std::make_shared<RecordingIngress>();
    publisher = node->create_publisher<std_msgs::msg::Bool>(binding.topic, qos);
  }
  void TearDown() override {
    if (provider)
      provider->Shutdown();
    executor->cancel();
    executor->remove_node(node);
    provider.reset();
    publisher.reset();
    executor.reset();
    node.reset();
    context->shutdown("boolean input fixture complete");
  }
  bool SpinUntil(const std::function<bool()> &condition) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition() && std::chrono::steady_clock::now() < deadline)
      executor->spin_once(5ms);
    return condition();
  }
  void Connect() {
    auto created = RosBooleanInputProvider::Create(node, sink, clock, binding,
                                                   descriptor, qos);
    ASSERT_TRUE(created.ok());
    provider = std::move(created.value());
    ASSERT_TRUE(
        SpinUntil([&] { return publisher->get_subscription_count() == 1; }));
  }
  void Publish(bool value) {
    std_msgs::msg::Bool message;
    message.data = value;
    publisher->publish(message);
  }

  core::FakeClock clock;
  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr node;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  std::shared_ptr<RecordingIngress> sink;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr publisher;
  std::unique_ptr<RosBooleanInputProvider> provider;
  rclcpp::QoS qos{rclcpp::KeepLast(8)};
  RosBooleanInputBinding binding{
      {core::ControllableComponentId("component.io"), "sensor"},
      core::InterruptSourceId("source.io"),
      core::IoSourceIncarnation("activation.1"),
      "/test/boolean_input"};
  core::IoSignalDescriptorDTO descriptor{
      binding.signal_id,
      binding.source_id,
      core::IoSignalValueType::kBoolean,
      core::IoSignalDirection::kInput,
      core::IoSignalUpdateMode::kSampled,
      core::IoTransitionCapability::kSampledOnly,
      {core::IoFreshnessMode::kSampleAge, 100},
      std::nullopt,
      core::IoSignalLivenessMode::kPeriodic};
};

TEST_F(RosBooleanInputProviderTest,
       DoesNotFabricateAnInitialSampleOrHeartbeat) {
  Connect();
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(sink->count, 0U);
  EXPECT_EQ(sink->loss_count, 0U);
  clock.advance(1000);
  executor->spin_some();
  EXPECT_EQ(sink->count, 0U);
  EXPECT_EQ(sink->loss_count, 0U);
  EXPECT_EQ(provider->status().last_ingress_source_sequence, 0U);
}

TEST_F(RosBooleanInputProviderTest,
       PreservesFalseTrueOrderIdentityAndIngressTimestamp) {
  Connect();
  ASSERT_NE(provider, nullptr);
  clock.set(10);
  Publish(false);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1; }));
  clock.set(20);
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 2; }));
  const auto &first = *sink->samples[0];
  const auto &second = *sink->samples[1];
  ASSERT_EQ(first.acquisition.updates.size(), 1U);
  ASSERT_EQ(second.acquisition.updates.size(), 1U);
  EXPECT_EQ(first.acquisition.updates.front().signal_id, binding.signal_id);
  EXPECT_EQ(first.acquisition.source_id, binding.source_id);
  EXPECT_EQ(first.acquisition.incarnation, binding.incarnation);
  EXPECT_EQ(first.acquisition.updates.front().validity,
            core::IoSampleValidity::kValid);
  EXPECT_EQ(first.acquisition.updates.front().value, false);
  EXPECT_EQ(second.acquisition.updates.front().value, true);
  EXPECT_EQ(first.acquisition.source_sequence, 1U);
  EXPECT_EQ(second.acquisition.source_sequence, 2U);
  EXPECT_EQ(first.acquisition.trusted_received_at_ns, 10);
  EXPECT_EQ(second.acquisition.trusted_received_at_ns, 20);
  EXPECT_FALSE(first.acquisition.updates.front().error.has_value());
}

TEST_F(RosBooleanInputProviderTest, RetainedDriverRebindNeedsANewActualSample) {
  Connect();
  ASSERT_TRUE(provider);
  clock.set(10);
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1U; }));
  Publish(false); // Buffered on the old subscription, never relabeled.
  auto next = std::make_shared<RecordingIngress>();
  const core::IoSourceIncarnation incarnation("activation.2");
  ASSERT_TRUE(provider->RebindIngress(next, incarnation, 1U).ok());
  executor->spin_some();
  EXPECT_EQ(next->count, 0U);
  EXPECT_EQ(sink->loss_count, 1U);
  EXPECT_TRUE(provider->status().accepting);
  ASSERT_TRUE(
      SpinUntil([&] { return publisher->get_subscription_count() == 1U; }));
  clock.set(20);
  Publish(false);
  ASSERT_TRUE(SpinUntil([&] { return next->count == 1U; }));
  ASSERT_TRUE(next->samples[0]);
  EXPECT_EQ(next->samples[0]->acquisition.incarnation, incarnation);
  EXPECT_EQ(next->samples[0]->acquisition.source_sequence, 1U);
  EXPECT_EQ(next->samples[0]->acquisition.trusted_received_at_ns, 20);
  EXPECT_EQ(next->samples[0]->acquisition.updates[0].value, false);
  EXPECT_TRUE(provider->RebindIngress(next, incarnation, 1U).ok());
  EXPECT_EQ(next->count, 1U);
  EXPECT_FALSE(provider->MarkUnavailable(binding.incarnation, 1U));
}

TEST_F(RosBooleanInputProviderTest,
       ResumeRejectsThePreviousSubscriptionQueuedMessage) {
  Connect();
  ASSERT_TRUE(provider);
  clock.set(10);
  Publish(false);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1U; }));
  Publish(true); // Queued before Disable, not yet passed to its callback.
  ASSERT_TRUE(provider->Pause(1U));
  ASSERT_TRUE(provider->Resume(2U));
  clock.set(20);
  executor->spin_some();
  EXPECT_EQ(sink->count, 1U);
  EXPECT_EQ(sink->loss_count, 1U);
  ASSERT_TRUE(
      SpinUntil([&] { return publisher->get_subscription_count() == 1U; }));
  Publish(false);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 2U; }));
  ASSERT_TRUE(sink->samples[1]);
  EXPECT_EQ(sink->samples[1]->acquisition.incarnation, binding.incarnation);
  EXPECT_EQ(sink->samples[1]->acquisition.source_sequence, 2U);
  EXPECT_EQ(sink->samples[1]->acquisition.trusted_received_at_ns, 20);
  EXPECT_EQ(sink->samples[1]->acquisition.updates[0].value, false);
  EXPECT_TRUE(provider->Resume(2U));
  EXPECT_EQ(sink->count, 2U);
}

TEST_F(RosBooleanInputProviderTest,
       CapturesReceiptBeforeCallingTheHostIngress) {
  Connect();
  ASSERT_NE(provider, nullptr);
  clock.set(25);
  sink->advance_on_enqueue = &clock;
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1; }));
  EXPECT_EQ(clock.now(), 1025);
  EXPECT_EQ(sink->samples[0]->acquisition.trusted_received_at_ns, 25);
  clock.advance(5000);
  EXPECT_EQ(sink->samples[0]->acquisition.trusted_received_at_ns, 25);
}

TEST_F(RosBooleanInputProviderTest,
       FullHostIngressReportsLossWithoutRetryOrInventingPayload) {
  Connect();
  ASSERT_NE(provider, nullptr);
  sink->reject = true;
  clock.set(30);
  Publish(false);
  ASSERT_TRUE(SpinUntil([&] { return sink->loss_count == 1; }));
  EXPECT_EQ(sink->count, 0U);
  const auto &loss = *sink->losses[0];
  EXPECT_EQ(loss.reason, core::IoIngressLossReason::kHostIngressRejected);
  EXPECT_EQ(loss.source_id, binding.source_id);
  EXPECT_EQ(loss.incarnation, binding.incarnation);
  EXPECT_EQ(loss.trusted_received_at_ns, 30);
  EXPECT_EQ(loss.after_ingress_source_sequence, 1U);
  EXPECT_FALSE(loss.middleware_reported_lost_messages.has_value());
  EXPECT_EQ(provider->status().last_enqueued_source_sequence, 0U);
  sink->reject = false;
  clock.set(40);
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1; }));
  EXPECT_EQ(sink->samples[0]->acquisition.source_sequence, 2U);
  EXPECT_GT(sink->samples[0]->order, sink->loss_orders[0]);
  EXPECT_EQ(sink->loss_count, 1U);
}

TEST_F(RosBooleanInputProviderTest, RefusedFrameDoesNotManufactureSourceLoss) {
  Connect();
  ASSERT_NE(provider, nullptr);
  sink->reject = true;
  // Invalid/stale frames are refused without altering the current source.
  sink->record_rejection_loss = false;
  Publish(true);
  ASSERT_TRUE(SpinUntil(
      [&] { return provider->status().last_ingress_source_sequence == 1; }));
  EXPECT_EQ(sink->count, 0U);
  EXPECT_EQ(sink->loss_count, 0U);
  EXPECT_EQ(provider->status().last_enqueued_source_sequence, 0U);
}

TEST_F(RosBooleanInputProviderTest,
       DelayedDisconnectCannotOvertakeANewerIngress) {
  Connect();
  ASSERT_NE(provider, nullptr);
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1; }));
  EXPECT_FALSE(provider->MarkUnavailable(binding.incarnation, 0));
  EXPECT_EQ(sink->loss_count, 0U);
  clock.set(50);
  EXPECT_TRUE(provider->MarkUnavailable(binding.incarnation, 1));
  ASSERT_EQ(sink->loss_count, 1U);
  EXPECT_EQ(sink->losses[0]->reason, core::IoIngressLossReason::kDisconnected);
  EXPECT_EQ(sink->losses[0]->trusted_received_at_ns, 50);
  EXPECT_GT(sink->loss_orders[0], sink->samples[0]->order);
}

TEST_F(RosBooleanInputProviderTest,
       ShutdownFencesQueuedCallbacksAndIsIdempotent) {
  Connect();
  ASSERT_NE(provider, nullptr);
  Publish(true);
  provider->Shutdown();
  provider->Shutdown();
  executor->spin_some();
  EXPECT_FALSE(provider->status().accepting);
  EXPECT_EQ(sink->count, 0U);
  ASSERT_EQ(sink->loss_count, 1U);
  EXPECT_EQ(sink->losses[0]->after_ingress_source_sequence, 0U);
  EXPECT_FALSE(provider->MarkUnavailable(binding.incarnation, 0));
}

TEST_F(RosBooleanInputProviderTest,
       RefusesUnsupportedDescriptorsAndHistoricalReplayQos) {
  descriptor.transition_capability = core::IoTransitionCapability::kCaptured;
  auto captured = RosBooleanInputProvider::Create(node, sink, clock, binding,
                                                  descriptor, qos);
  ASSERT_FALSE(captured.ok());
  EXPECT_EQ(captured.error().code, "unsupported_capability");
  descriptor.transition_capability = core::IoTransitionCapability::kSampledOnly;
  descriptor.direction = core::IoSignalDirection::kOutput;
  EXPECT_FALSE(RosBooleanInputProvider::Create(node, sink, clock, binding,
                                               descriptor, qos)
                   .ok());
  descriptor.direction = core::IoSignalDirection::kInput;
  auto historical_qos = qos;
  historical_qos.transient_local();
  EXPECT_FALSE(RosBooleanInputProvider::Create(node, sink, clock, binding,
                                               descriptor, historical_qos)
                   .ok());
  descriptor.source_id = core::InterruptSourceId("wrong.source");
  EXPECT_FALSE(RosBooleanInputProvider::Create(node, sink, clock, binding,
                                               descriptor, qos)
                   .ok());
  EXPECT_EQ(sink->count, 0U);
  EXPECT_EQ(sink->loss_count, 0U);
}

TEST_F(RosBooleanInputProviderTest,
       CreationFailureReturnsErrorWithoutIngressOrLoss) {
  auto stopped_context = std::make_shared<rclcpp::Context>();
  stopped_context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(stopped_context);
  auto stopped_node =
      std::make_shared<rclcpp::Node>("stopped_boolean_input_test", options);
  stopped_context->shutdown("exercise input creation failure");
  const auto created = RosBooleanInputProvider::Create(
      stopped_node, sink, clock, binding, descriptor, qos);
  ASSERT_FALSE(created.ok());
  EXPECT_EQ(created.error().code, "io_ros_subscription_failed");
  EXPECT_EQ(sink->count, 0U);
  EXPECT_EQ(sink->loss_count, 0U);
}

} // namespace
TEST_F(RosBooleanInputProviderTest,
       ChangeOnlyUsesActualRetainedPublisherValueAtConnect) {
  publisher.reset();
  auto retained_qos = qos;
  retained_qos.transient_local();
  publisher =
      node->create_publisher<std_msgs::msg::Bool>(binding.topic, retained_qos);
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] {
    const auto publishers = node->get_publishers_info_by_topic(binding.topic);
    return !publishers.empty() &&
           std::all_of(publishers.begin(), publishers.end(),
                       [](const auto &info) {
                         return info.qos_profile().durability() ==
                                rclcpp::DurabilityPolicy::TransientLocal;
                       });
  }));
  descriptor.liveness = core::IoSignalLivenessMode::kChangeOnly;
  descriptor.update_mode = core::IoSignalUpdateMode::kChangeOnly;
  descriptor.freshness = {core::IoFreshnessMode::kProviderValidity,
                          std::nullopt};
  clock.set(50);
  Connect();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1U; }));
  EXPECT_EQ(sink->samples[0]->acquisition.updates.front().value, true);
  EXPECT_EQ(sink->samples[0]->acquisition.trusted_received_at_ns, 50);
  clock.advance(1000);
  executor->spin_some();
  EXPECT_EQ(sink->count, 1U); // no synthetic periodic refresh
}

TEST_F(RosBooleanInputProviderTest,
       KnownBestEffortPublisherRemainsCompatibleAtConnect) {
  publisher.reset();
  auto best_effort = qos;
  best_effort.best_effort();
  publisher =
      node->create_publisher<std_msgs::msg::Bool>(binding.topic, best_effort);
  ASSERT_TRUE(SpinUntil([&] {
    const auto publishers = node->get_publishers_info_by_topic(binding.topic);
    return std::any_of(publishers.begin(), publishers.end(),
                       [](const auto &info) {
                         return info.qos_profile().reliability() ==
                                rclcpp::ReliabilityPolicy::BestEffort;
                       });
  }));
  Connect();
  ASSERT_TRUE(provider);
  EXPECT_EQ(sink->count, 0U);
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1U; }));
  EXPECT_EQ(sink->samples[0]->acquisition.updates.front().value, true);
}

TEST_F(RosBooleanInputProviderTest,
       UnrelatedGraphTypeCannotSuppressSelectedRetainedValue) {
  publisher.reset();
  auto retained = qos;
  retained.transient_local();
  publisher =
      node->create_publisher<std_msgs::msg::Bool>(binding.topic, retained);
  auto other_context = std::make_shared<rclcpp::Context>();
  other_context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(other_context);
  options.use_global_arguments(false);
  auto other_node =
      std::make_shared<rclcpp::Node>("other_topic_type_test", options);
  auto other_publisher =
      other_node->create_publisher<std_msgs::msg::Int32>(binding.topic, qos);
  Publish(true);
  ASSERT_TRUE(SpinUntil([&] {
    const auto publishers = node->get_publishers_info_by_topic(binding.topic);
    bool selected = false, unrelated = false;
    for (const auto &info : publishers) {
      selected = selected || (info.topic_type() == "std_msgs/msg/Bool" &&
                              info.qos_profile().durability() ==
                                  rclcpp::DurabilityPolicy::TransientLocal);
      unrelated = unrelated || info.topic_type() == "std_msgs/msg/Int32";
    }
    return selected && unrelated;
  }));
  descriptor.liveness = core::IoSignalLivenessMode::kChangeOnly;
  descriptor.update_mode = core::IoSignalUpdateMode::kChangeOnly;
  descriptor.freshness = {core::IoFreshnessMode::kProviderValidity,
                          std::nullopt};
  Connect();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(SpinUntil([&] { return sink->count == 1U; }));
  EXPECT_EQ(sink->samples[0]->acquisition.updates.front().value, true);
  other_publisher.reset();
  other_node.reset();
  other_context->shutdown("other source type complete");
}

} // namespace manyforge::roboplan_backend
