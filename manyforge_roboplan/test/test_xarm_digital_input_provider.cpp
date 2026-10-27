// Copyright 2026 Flexin Group SRL
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/xarm_digital_input_provider.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <xarm_msgs/srv/get_digital_io.hpp>

namespace manyforge::roboplan_backend {
namespace {
using namespace std::chrono_literals;
using Service = xarm_msgs::srv::GetDigitalIO;
const core::InterruptSourceId kSource("source.io");
const core::IoSourceIncarnation kIncarnation("activation.1");

class RecordingIngress final : public core::IIoSignalIngress {
public:
  bool TryEnqueue(const core::IoSourceAcquisitionDTO &frame) noexcept override {
    frames.push_back(frame);
    return true;
  }
  void NotifyLoss(const core::IoIngressLossNotificationDTO &loss) noexcept override {
    losses.push_back(loss);
  }
  std::vector<core::IoSourceAcquisitionDTO> frames;
  std::vector<core::IoIngressLossNotificationDTO> losses;
};

core::IoSignalDescriptorDTO descriptor(std::string name, core::Duration age) {
  return {{core::ControllableComponentId("component.io"), std::move(name)},
          kSource, core::IoSignalValueType::kBoolean,
          core::IoSignalDirection::kInput, core::IoSignalUpdateMode::kSampled,
          core::IoTransitionCapability::kSampledOnly,
          {core::IoFreshnessMode::kSampleAge, age}, std::nullopt,
          core::IoSignalLivenessMode::kPeriodic};
}

class XArmDigitalInputProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions options;
    options.context(context);
    options.use_global_arguments(false);
    node = std::make_shared<rclcpp::Node>("xarm_input_test", options);
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>(executor_options);
    executor->add_node(node);
    sink = std::make_shared<RecordingIngress>();
    service = node->create_service<Service>(
        "/test/get_tgpio_digital", [&](Service::Request::SharedPtr,
                                        Service::Response::SharedPtr response) {
          ++requests;
          response->ret = vendor_return;
          response->digitals = values;
        });
    probe = node->create_client<Service>("/test/get_tgpio_digital");
    ASSERT_TRUE(SpinUntil([&] { return probe->service_is_ready(); }));
    clock->set(1);
  }
  void TearDown() override {
    if (provider)
      provider->Shutdown();
    executor->cancel();
    executor->remove_node(node);
    provider.reset();
    service.reset();
    probe.reset();
    executor.reset();
    node.reset();
    context->shutdown("xArm input fixture complete");
  }
  bool SpinUntil(const std::function<bool()> &condition) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!condition() && std::chrono::steady_clock::now() < until)
      executor->spin_once(5ms);
    return condition();
  }
  XArmDigitalInputBinding binding() const {
    return {kSource, kIncarnation, "/test/get_tgpio_digital",
            XArmDigitalBank::kTool,
            {{descriptor("short", 50'000'000), 0U},
             {descriptor("long", 250'000'000), 1U}}};
  }
  void Connect() {
    auto made = XArmDigitalInputProvider::Create(node, sink, clock, scheduler,
                                                  binding(), 1U);
    ASSERT_TRUE(made.ok());
    provider = std::move(made.value());
    ASSERT_TRUE(provider->Resume(2U));
  }
  void Advance(core::TimePoint time) {
    clock->set(time);
    scheduler->advance_to(time);
  }
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<core::ManualScheduler> scheduler = std::make_shared<core::ManualScheduler>();
  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr node;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  rclcpp::Service<Service>::SharedPtr service;
  rclcpp::Client<Service>::SharedPtr probe;
  std::shared_ptr<RecordingIngress> sink;
  std::unique_ptr<XArmDigitalInputProvider> provider;
  int requests{};
  std::int16_t vendor_return{};
  std::vector<std::int16_t> values{0, 1};
};

TEST_F(XArmDigitalInputProviderTest, OneReadSamplesTheWholeBankAtRequestTime) {
  Connect();
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  EXPECT_EQ(requests, 1);
  const auto &frame = sink->frames.front();
  EXPECT_EQ(frame.trusted_received_at_ns, 1);
  ASSERT_EQ(frame.updates.size(), 2U);
  EXPECT_EQ(frame.updates[0].value, false);
  EXPECT_EQ(frame.updates[1].value, true);
  Advance(50'000'001);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 2U; }));
  EXPECT_EQ(requests, 2);
  EXPECT_EQ(sink->frames.back().trusted_received_at_ns, 50'000'001);
}

TEST_F(XArmDigitalInputProviderTest, ControllerBankReadsTheSixteenthInput) {
  values.assign(16, 0);
  values[15] = 1;
  auto selected = binding();
  selected.bank = XArmDigitalBank::kController;
  selected.signals.back().pin = 15U;
  auto made = XArmDigitalInputProvider::Create(node, sink, clock, scheduler,
                                                std::move(selected), 1U);
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  ASSERT_TRUE(provider->Resume(2U));
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  EXPECT_EQ(sink->frames.front().updates.back().value, true);
}

TEST_F(XArmDigitalInputProviderTest, SlowReadCannotBeRestampedAndExpiredReplyIsIgnored) {
  Connect();
  // Do not service the first request until after its bank's longest budget.
  Advance(50'000'001);
  Advance(100'000'001);
  Advance(150'000'001);
  Advance(200'000'001);
  EXPECT_TRUE(sink->frames.empty());
  Advance(250'000'001);
  ASSERT_TRUE(SpinUntil([&] { return requests == 2; }));
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  EXPECT_EQ(sink->frames.front().trusted_received_at_ns, 250'000'001);
}

TEST_F(XArmDigitalInputProviderTest, PauseFencesPendingRepliesAndResumeSamplesAgain) {
  Connect();
  ASSERT_TRUE(provider->Pause(3U));
  ASSERT_TRUE(SpinUntil([&] { return requests == 1; }));
  EXPECT_TRUE(sink->frames.empty());
  ASSERT_TRUE(provider->Resume(4U));
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  EXPECT_EQ(requests, 2);
  EXPECT_EQ(sink->frames.front().incarnation, kIncarnation);
}

TEST_F(XArmDigitalInputProviderTest, ReadFailureAndMalformedPinRemainUnusable) {
  vendor_return = -1;
  Connect();
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 1U; }));
  for (const auto &update : sink->frames.back().updates) {
    EXPECT_EQ(update.validity, core::IoSampleValidity::kUnavailable);
    EXPECT_FALSE(update.value);
  }
  vendor_return = 0;
  values = {1};
  Advance(50'000'001);
  ASSERT_TRUE(SpinUntil([&] { return sink->frames.size() == 2U; }));
  ASSERT_EQ(sink->frames.back().updates.size(), 2U);
  EXPECT_EQ(sink->frames.back().updates[0].validity, core::IoSampleValidity::kValid);
  EXPECT_EQ(sink->frames.back().updates[0].value, true);
  EXPECT_EQ(sink->frames.back().updates[1].validity, core::IoSampleValidity::kInvalid);
}

TEST_F(XArmDigitalInputProviderTest, RebindFencesTheOldIncarnation) {
  Connect();
  auto replacement = std::make_shared<RecordingIngress>();
  const core::IoSourceIncarnation next("activation.2");
  ASSERT_TRUE(provider->RebindIngress(replacement, next, 3U).ok());
  ASSERT_TRUE(SpinUntil([&] { return requests == 2; }));
  ASSERT_TRUE(SpinUntil([&] { return replacement->frames.size() == 1U; }));
  EXPECT_TRUE(sink->frames.empty());
  EXPECT_EQ(replacement->frames.front().incarnation, next);
}

TEST_F(XArmDigitalInputProviderTest, BadPinsAndNonperiodicInputsAreRefused) {
  auto invalid = binding();
  invalid.signals.front().pin = 2U;
  EXPECT_FALSE(XArmDigitalInputProvider::Create(node, sink, clock, scheduler,
                                                invalid, 1U).ok());
  invalid = binding();
  invalid.signals.front().descriptor.freshness =
      {core::IoFreshnessMode::kProviderValidity, std::nullopt};
  EXPECT_FALSE(XArmDigitalInputProvider::Create(node, sink, clock, scheduler,
                                                invalid, 1U).ok());
}
} // namespace
} // namespace manyforge::roboplan_backend
