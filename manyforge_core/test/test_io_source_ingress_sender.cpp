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

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <manyforge_core/io_source_ingress_sender.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <mutex>
#include <vector>

namespace manyforge::core {
namespace {
using namespace std::chrono_literals;
const InterruptSourceId kSource("input.source");
const IoSourceIncarnation kIncarnation("activation.1");

std::vector<IoSignalUpdateDTO> updates() {
  return {
      {{ControllableComponentId("io"), "sensor"}, IoSampleValidity::kValid, false, std::nullopt}};
}

class RecordingIngress final : public IIoSignalIngress {
 public:
  bool TryEnqueue(const IoSourceAcquisitionDTO& acquisition) noexcept override {
    if (on_enqueue) on_enqueue();
    std::lock_guard lock(mutex);
    if (!accept) {
      ++capacity_losses;
      return false;
    }
    acquisitions.push_back(acquisition);
    return true;
  }
  void NotifyLoss(const IoIngressLossNotificationDTO& loss) noexcept override {
    std::lock_guard lock(mutex);
    losses.push_back(loss);
  }
  std::function<void()> on_enqueue;
  bool accept = true;
  std::mutex mutex;
  std::vector<IoSourceAcquisitionDTO> acquisitions;
  std::vector<IoIngressLossNotificationDTO> losses;
  std::size_t capacity_losses = 0;
};

TEST(IoSourceIngressSenderTest, SelectedIngressAndShutdownSerializeWithoutASecondAcquisition) {
  FakeClock clock;
  clock.set(7);
  auto ingress = std::make_shared<RecordingIngress>();
  IoSourceIngressSender sender(ingress, clock, kSource, kIncarnation);
  std::promise<void> entered, release, closing_started;
  auto entered_future = entered.get_future();
  auto released = release.get_future().share();
  auto closing_future = closing_started.get_future();
  // Fault-injection gate only: production ingress may briefly lock, never wait
  // on an external promise. This selects the handoff being raced by shutdown.
  ingress->on_enqueue = [&] {
    clock.set(100);
    entered.set_value();
    released.wait();
  };
  auto publishing = std::async(std::launch::async, [&] {
    sender.Receive(updates(), sender.CaptureAcquisition(kIncarnation, 0U));
  });
  EXPECT_EQ(entered_future.wait_for(2s), std::future_status::ready);
  auto closing = std::async(std::launch::async, [&] {
    closing_started.set_value();
    sender.Shutdown();
  });
  EXPECT_EQ(closing_future.wait_for(2s), std::future_status::ready);
  release.set_value();
  publishing.get();
  closing.get();
  sender.Shutdown();
  sender.Receive(updates(), sender.CaptureAcquisition(kIncarnation, 0U));
  ASSERT_EQ(ingress->acquisitions.size(), 1U);
  EXPECT_EQ(ingress->acquisitions.front().trusted_received_at_ns, 7);
  EXPECT_EQ(ingress->acquisitions.front().source_sequence, 1U);
  ASSERT_EQ(ingress->losses.size(), 1U);
  EXPECT_EQ(ingress->losses.front().reason, IoIngressLossReason::kDisconnected);
  EXPECT_EQ(ingress->losses.front().after_ingress_source_sequence, 1U);
  EXPECT_FALSE(sender.status().accepting);
}

TEST(IoSourceIngressSenderTest, ActualMiddlewareLossStaysDistinctFromSharedCapacityBarrier) {
  FakeClock clock;
  auto ingress = std::make_shared<RecordingIngress>();
  IoSourceIngressSender sender(ingress, clock, kSource, kIncarnation);
  ingress->accept = false;
  sender.Receive(updates(), IoSourceIngressAcquisitionReceipt{0, 0, kIncarnation});
  EXPECT_EQ(ingress->capacity_losses, 1U);
  EXPECT_TRUE(ingress->losses.empty());
  clock.set(4);
  sender.NotifyLoss(IoIngressLossReason::kMiddlewareMessageLost,
                    sender.CaptureAcquisition(kIncarnation, 0U), 3);
  ASSERT_EQ(ingress->losses.size(), 1U);
  const auto& loss = ingress->losses.front();
  EXPECT_EQ(loss.source_id, kSource);
  EXPECT_EQ(loss.incarnation, kIncarnation);
  EXPECT_EQ(loss.trusted_received_at_ns, 4);
  EXPECT_EQ(loss.after_ingress_source_sequence, 1U);
  EXPECT_EQ(loss.reason, IoIngressLossReason::kMiddlewareMessageLost);
  EXPECT_EQ(loss.middleware_reported_lost_messages, 3U);
  EXPECT_EQ(sender.status().last_enqueued_source_sequence, 0U);
}

TEST(IoSourceIngressSenderTest, PreparedDriverCannotPublishBeforeActualEnable) {
  FakeClock clock;
  auto ingress = std::make_shared<RecordingIngress>();
  IoSourceIngressSender sender(ingress, clock, kSource, kIncarnation, 1U);
  sender.Receive(updates(), sender.CaptureAcquisition(kIncarnation, 1U));
  EXPECT_TRUE(ingress->acquisitions.empty());
  EXPECT_TRUE(ingress->losses.empty());
  EXPECT_FALSE(sender.Resume(1U));
  ASSERT_TRUE(sender.Resume(2U));
  sender.Receive(updates(), sender.CaptureAcquisition(kIncarnation, 2U));
  ASSERT_EQ(ingress->acquisitions.size(), 1U);
  EXPECT_EQ(ingress->acquisitions.front().source_sequence, 1U);
}

TEST(IoSourceIngressSenderTest, DisableEnableRejectsOldCallbacksEvenAtIdenticalClockTime) {
  FakeClock clock;
  auto ingress = std::make_shared<RecordingIngress>();
  IoSourceIngressSender sender(ingress, clock, kSource, kIncarnation);
  const auto old_callback = sender.CaptureAcquisition(kIncarnation, 0U);
  ASSERT_TRUE(sender.Pause(1U));
  ASSERT_TRUE(sender.Resume(2U));
  sender.Receive(updates(), old_callback);
  sender.NotifyLoss(IoIngressLossReason::kMiddlewareMessageLost, old_callback, 1U);
  // First execution of a buffered OLD subscription callback after Resume
  // keeps its immutable old token even though its receiver timestamp is new.
  clock.set(10);
  const auto buffered_old_subscription = sender.CaptureAcquisition(kIncarnation, 0U);
  sender.Receive(updates(), buffered_old_subscription);
  sender.NotifyLoss(IoIngressLossReason::kMiddlewareMessageLost, buffered_old_subscription, 2U);
  EXPECT_TRUE(ingress->acquisitions.empty());
  ASSERT_EQ(ingress->losses.size(), 1U);
  EXPECT_EQ(ingress->losses.front().reason, IoIngressLossReason::kDisconnected);
  sender.Receive(updates(), sender.CaptureAcquisition(kIncarnation, 2U));
  ASSERT_EQ(ingress->acquisitions.size(), 1U);
  EXPECT_EQ(ingress->acquisitions.front().incarnation, kIncarnation);
  EXPECT_EQ(ingress->acquisitions.front().source_sequence, 1U);
  EXPECT_FALSE(sender.Pause(1U));
  EXPECT_TRUE(sender.Resume(2U));
  sender.Shutdown();
  EXPECT_FALSE(sender.Resume(3U));
}

TEST(IoSourceIngressSenderTest, RetainedDriverRebindRejectsOldBufferedCallbackIncarnation) {
  FakeClock clock;
  clock.set(10);
  auto old = std::make_shared<RecordingIngress>();
  auto next = std::make_shared<RecordingIngress>();
  IoSourceIngressSender sender(old, clock, kSource, kIncarnation);
  sender.Receive(updates(), sender.CaptureAcquisition(kIncarnation, 0U));
  const auto entered_before_apply = sender.CaptureAcquisition(kIncarnation, 0U);
  const IoSourceIncarnation successor("activation.2");
  ASSERT_TRUE(sender.RebindIngress(next, successor, 1U));
  EXPECT_TRUE(sender.status().accepting);
  ASSERT_EQ(old->losses.size(), 1U);
  EXPECT_EQ(old->losses[0].incarnation, kIncarnation);
  EXPECT_EQ(old->losses[0].after_ingress_source_sequence, 1U);
  sender.Receive(updates(), entered_before_apply);
  // Even a buffered old subscription callback that first executes after the
  // handoff still carries its immutable old incarnation.
  const auto old_subscription = sender.CaptureAcquisition(kIncarnation, 0U);
  sender.Receive(updates(), old_subscription);
  sender.NotifyLoss(IoIngressLossReason::kMiddlewareMessageLost, old_subscription, 3U);
  EXPECT_TRUE(next->acquisitions.empty());
  EXPECT_TRUE(next->losses.empty());
  clock.set(20);
  sender.Receive(updates(), sender.CaptureAcquisition(successor, 1U));
  ASSERT_EQ(next->acquisitions.size(), 1U);
  EXPECT_EQ(next->acquisitions[0].incarnation, successor);
  EXPECT_EQ(next->acquisitions[0].source_sequence, 1U);
  EXPECT_EQ(next->acquisitions[0].trusted_received_at_ns, 20);
  EXPECT_FALSE(sender.MarkUnavailable(kIncarnation, 1U));
  EXPECT_TRUE(sender.RebindIngress(next, successor, 1U));
  EXPECT_EQ(next->acquisitions.size(), 1U);
  EXPECT_FALSE(sender.RebindIngress(old, kIncarnation, 1U));
}

}  // namespace
}  // namespace manyforge::core
