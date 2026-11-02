#include "execution_host_responses.hpp"
#include "host_io_signal_runtime.hpp"
#include "ros_host_composition.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {
namespace {

class HostRuleEvidenceWakeupTest : public ::testing::Test {
protected:
  void SetUp() override {
    // A real BB-only service needs no physical provider or invented sample.
    runtime = HostIoSignalRuntime::Create(
        core::RequestOwnerEpoch("host"), 1U, {}, clock, scheduler, 1U);
  }
  void Advance() {
    scheduler->advance_to(clock->now(), core::SchedulerPriority::RuntimeSafety);
  }
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<core::ManualScheduler> scheduler =
      std::make_shared<core::ManualScheduler>();
  std::shared_ptr<HostIoSignalRuntime> runtime;
};

TEST_F(HostRuleEvidenceWakeupTest, CoalescesWakeAndRunsWithoutRuntimeLock) {
  unsigned calls{};
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    // These methods take the runtime mutex. The scheduler callback must have
    // released it before entering its actual owner's processing function.
    EXPECT_TRUE(runtime->has_rule_evidence_processor());
    EXPECT_TRUE(runtime->RuleServiceConfiguration().ok());
    return core::Result<bool>::Ok(true);
  });
  runtime->RequestRuleEvidenceProcessing();
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
  runtime->Start();
  runtime->RequestRuleEvidenceProcessing();
  runtime->RequestRuleEvidenceProcessing();
  EXPECT_EQ(calls, 0U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 1U);
  Advance();
  EXPECT_EQ(calls, 1U);
  // This service has no armed expiring source. There is no periodic poller.
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
}

TEST_F(HostRuleEvidenceWakeupTest, OriginalProcessingErrorDoesNotScheduleRetry) {
  unsigned calls{};
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    return core::Result<bool>::Err(
        {"persistence_degraded", "actual owner error", {{"reason", "test"}}});
  });
  runtime->Start();
  testing::internal::CaptureStderr();
  Advance();
  const auto diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_NE(diagnostic.find("persistence_degraded actual owner error"),
            std::string::npos);
  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
  Advance();
  EXPECT_EQ(calls, 1U);
}

TEST_F(HostRuleEvidenceWakeupTest, DetachAndShutdownCancelQueuedWork) {
  unsigned calls{};
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    return core::Result<bool>::Ok(true);
  });
  runtime->Start();
  runtime->DetachRuleEvidenceProcessor();
  EXPECT_FALSE(runtime->has_rule_evidence_processor());
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
  Advance();
  EXPECT_EQ(calls, 0U);
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    return core::Result<bool>::Ok(true);
  });
  runtime->RequestRuleEvidenceProcessing();
  runtime->Shutdown();
  Advance();
  EXPECT_EQ(calls, 0U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
}

TEST_F(HostRuleEvidenceWakeupTest, CallbackBorrowsActualAccessWithoutRetainingIt) {
  auto access = std::make_shared<ExecutionCompositionAccess>();
  std::weak_ptr<ExecutionCompositionAccess> weak = access;
  // Only the callback attachment is under test; no selected product or
  // publication authority is supplied or inferred by this helper.
  ExecutionProduct product(nullptr, nullptr, nullptr, {},
                           core::ActiveBundlePublicationGeneration(1U));
  product.io_signal_runtime = runtime;
  BindHostIoRuleEvidenceProcessor(product, access);
  runtime->Start();
  // No Kernel is installed in this access object. A queued callback cannot
  // invent one, and the callback's weak pin cannot extend its lifetime.
  access->detach_addressed_owner();
  access.reset();
  EXPECT_TRUE(weak.expired());
  Advance();
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
}

deployment::RetainedIoSignalConfiguration
SystemRow(const std::string &component, const std::string &target) {
  core::IoSignalDescriptorDTO descriptor{
      {core::ControllableComponentId(component),
       deployment::CarriedObjectLostSignalName(target)},
      core::InterruptSourceId(
          std::string(deployment::kCarriedObjectLostSourceId)),
      core::IoSignalValueType::kBoolean,
      core::IoSignalDirection::kInput,
      core::IoSignalUpdateMode::kChangeOnly,
      core::IoTransitionCapability::kSampledOnly,
      {core::IoFreshnessMode::kProviderValidity, {}},
      {}};
  descriptor.liveness = core::IoSignalLivenessMode::kInProcess;
  return {core::RuntimeUnitId("host.robot"), core::CommandTargetId(target),
          core::ResourceRoleId(
              std::string(deployment::kCarriedObjectLostResourceRole)),
          std::move(descriptor),
          deployment::IoSignalProviderConfiguration{
              std::string(deployment::kSystemCarriedObjectProfile), {}, {}, {},
              deployment::kCarriedObjectLostIngressDepth}};
}

std::optional<bool> Value(const HostIoSignalRuntime &runtime,
                          const core::IoSignalId &signal) {
  const auto observed = runtime.ReadSignal(signal);
  if (!observed.ok() || !observed.value().latest_sample ||
      observed.value().availability != core::IoSignalAvailability::kFresh)
    return std::nullopt;
  return observed.value().latest_sample->publication.value;
}

TEST_F(HostRuleEvidenceWakeupTest, CarriedObjectLostSignalFollowsTheHostStore) {
  auto &store = host_carried_object_lost_signals();
  const core::ControllableComponentId robot("robot.g1");
  const core::CommandTargetId gripper("gripper");
  const auto signal = HostCarriedObjectLostSignals::SignalId(robot, gripper);
  const auto make = [&](std::uint64_t generation) {
    return HostIoSignalRuntime::Create(core::RequestOwnerEpoch("host"),
                                       generation, {}, clock, scheduler, 0U,
                                       {SystemRow("robot.g1", "gripper")});
  };
  auto system = make(1U);
  system->Start();
  system->AdoptCarriedObjectLostSignals();  // Committed (SX-206).
  Advance();
  // The initial acquisition makes the signal fresh and false.
  EXPECT_EQ(Value(*system, signal), std::optional(false));
  EXPECT_FALSE(store.Level(signal));

  // A loss publishes true with its attachment row.
  EXPECT_EQ(store.Publish(robot, gripper,
                          HostCarriedObjectLostSignals::Loss{
                              robot, gripper, "attachment.1", "box"}),
            std::nullopt);
  Advance();
  EXPECT_EQ(Value(*system, signal), std::optional(true));
  ASSERT_EQ(store.Losses().size(), 1U);
  EXPECT_EQ(store.Losses().front().carried_object_attachment_id, "attachment.1");
  EXPECT_EQ(store.Losses().front().scene_resource_id, "box");

  // A re-Apply keeps the level: the successor starts from the store.
  system->Shutdown();
  system = make(2U);
  system->Start();
  system->AdoptCarriedObjectLostSignals();
  Advance();
  EXPECT_EQ(Value(*system, signal), std::optional(true));

  // The next attach by that gripper publishes false and removes its rows.
  EXPECT_EQ(store.Publish(robot, gripper, std::nullopt), std::nullopt);
  Advance();
  EXPECT_EQ(Value(*system, signal), std::optional(false));
  EXPECT_TRUE(store.Losses().empty());
  // A false for a gripper that is not lost is still delivered (SX-188).
  EXPECT_EQ(store.Publish(robot, gripper, std::nullopt), std::nullopt);
  Advance();
  EXPECT_EQ(Value(*system, signal), std::optional(false));

  // A gripper this composition does not hold is refused but recorded.
  const core::CommandTargetId other("other");
  const auto refused = store.Publish(
      robot, other,
      HostCarriedObjectLostSignals::Loss{robot, other, "attachment.2", "cup"});
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "io_signal_unbound");
  EXPECT_TRUE(store.Level(HostCarriedObjectLostSignals::SignalId(robot, other)));
  EXPECT_EQ(store.Publish(robot, other, std::nullopt)->code, "io_signal_unbound");
  EXPECT_TRUE(store.Losses().empty());
  system->Shutdown();
}


// G2: readiness requires a Program-bound system signal; the component-model
// read samples it from the compiled catalog (it is never authored), and the
// sample it reports must count as fresh before any loss.
TEST_F(HostRuleEvidenceWakeupTest, ASystemSignalSampleIsFreshForReadiness) {
  const auto row = SystemRow("robot.g2", "gripper");
  auto system = HostIoSignalRuntime::Create(core::RequestOwnerEpoch("host"), 1U,
                                            {}, clock, scheduler, 0U, {row});
  system->Start();
  system->AdoptCarriedObjectLostSignals();
  Advance();
  const auto observed = system->ReadSignal(row.descriptor.signal_id);
  ASSERT_TRUE(observed.ok());
  const auto sample = SignalSampleValue(core::ToSignalSample(observed.value()),
                                        observed.value().descriptor);
  EXPECT_EQ(sample.find_object_member("signal_id")->as_string(),
            "robot.g2/carried_object_lost.gripper");
  EXPECT_TRUE(sample.find_object_member("fresh")->as_bool());
  EXPECT_FALSE(sample.find_object_member("value")->as_bool());
  system->Shutdown();
}

TEST_F(HostRuleEvidenceWakeupTest, ARefusedApplyCandidateLeavesTheRowsAndTheSinkUntouched) {
  // SX-206: an Apply candidate starts (and replays the levels) before the
  // terminal publication; only the committed composition prunes and takes
  // the sink.
  auto &store = host_carried_object_lost_signals();
  const core::ControllableComponentId robot("robot.g1");
  const core::CommandTargetId gripper("gripper"), other("other");
  const auto signal = HostCarriedObjectLostSignals::SignalId(robot, gripper);
  const auto make = [&](std::uint64_t generation, const char *target) {
    return HostIoSignalRuntime::Create(core::RequestOwnerEpoch("host"),
                                       generation, {}, clock, scheduler, 0U,
                                       {SystemRow("robot.g1", target)});
  };
  auto active = make(1U, "gripper");
  active->Start();
  active->AdoptCarriedObjectLostSignals();
  store.Publish(robot, gripper,
                HostCarriedObjectLostSignals::Loss{robot, gripper, "attachment.1", "box"});
  Advance();
  // A candidate without that gripper starts, then its Apply is refused.
  auto candidate = make(2U, "other");
  candidate->Start();
  candidate->Shutdown();
  candidate.reset();
  ASSERT_EQ(store.Losses().size(), 1U);  // Not pruned.
  EXPECT_EQ(store.Publish(robot, gripper, std::nullopt), std::nullopt);  // Still delivered.
  Advance();
  EXPECT_EQ(Value(*active, signal), std::optional(false));
  // The same replacement committed drops the rows of the gripper it lacks.
  store.Publish(robot, gripper,
                HostCarriedObjectLostSignals::Loss{robot, gripper, "attachment.2", "box"});
  auto committed = make(3U, "other");
  committed->Start();
  committed->AdoptCarriedObjectLostSignals();
  EXPECT_TRUE(store.Losses().empty());
  active->Shutdown();
  committed->Shutdown();
}

TEST(HostCarriedObjectLostSignalsTest, OneRowPerLostAttachmentAndFalseIsAlwaysDelivered) {
  HostCarriedObjectLostSignals store;
  std::vector<bool> delivered;
  const core::ControllableComponentId robot("robot.g1");
  const core::CommandTargetId gripper("gripper");
  store.InstallSink(
      [&](const core::IoSignalId &, bool level) {
        delivered.push_back(level);
        return std::optional<core::ErrorDTO>{};
      },
      {HostCarriedObjectLostSignals::SignalId(robot, gripper)},
      [](const std::vector<core::IoSignalId> &) {});
  using Loss = HostCarriedObjectLostSignals::Loss;
  store.Publish(robot, gripper,
                Loss{robot, gripper, "attachment.1", "box", "part_lost", "loss.1"});
  store.Publish(robot, gripper,
                Loss{robot, gripper, "attachment.2", "cup", std::nullopt, "loss.2"});
  // The same continuing loss detected again (a failed detach): the row and
  // its loss event id stay (contract §10).
  store.Publish(robot, gripper,
                Loss{robot, gripper, "attachment.1", "box", "part_lost", "loss.3"});
  const auto losses = store.Losses();
  ASSERT_EQ(losses.size(), 2U);  // Both keys stay (SX-200).
  EXPECT_EQ(losses[0].carried_object_attachment_id, "attachment.1");
  EXPECT_EQ(losses[0].loss_event_id, "loss.1");
  EXPECT_EQ(losses[0].set_key, std::optional<std::string>("part_lost"));
  EXPECT_EQ(losses[1].carried_object_attachment_id, "attachment.2");
  EXPECT_EQ(losses[1].loss_event_id, "loss.2");
  EXPECT_FALSE(losses[1].set_key.has_value());
  store.Publish(robot, gripper, std::nullopt);  // Removes all of its rows.
  store.Publish(robot, gripper, std::nullopt);  // No row: still delivered.
  EXPECT_TRUE(store.Losses().empty());
  EXPECT_EQ(delivered, (std::vector<bool>{true, true, true, false, false}));
}

TEST(HostCarriedObjectLostSignalsTest, DeliveriesFollowTheRecordedOrderAlsoAgainstTheInstallReplay) {
  using namespace std::chrono_literals;
  HostCarriedObjectLostSignals store;
  const core::ControllableComponentId robot("robot.g1");
  const core::CommandTargetId gripper("gripper");
  const auto signal = HostCarriedObjectLostSignals::SignalId(robot, gripper);
  // Recorded before any composition: the next one replays it as `true`.
  store.Publish(robot, gripper,
                HostCarriedObjectLostSignals::Loss{robot, gripper, "attachment.1", "box"});
  std::mutex order_mutex;
  std::vector<std::string> order;
  std::atomic<bool> seeding{false};
  std::thread installer([&] {
    store.InstallSink(
        [&](const core::IoSignalId &, bool level) {
          std::lock_guard lock(order_mutex);
          order.push_back(level ? "true" : "false");
          return std::optional<core::ErrorDTO>{};
        },
        {signal},
        [&](const std::vector<core::IoSignalId> &lost) {
          seeding = true;
          std::this_thread::sleep_for(50ms);  // Start()'s replay in progress.
          std::lock_guard lock(order_mutex);
          for (const auto &id : lost)
            order.push_back(id == signal ? "seed" : "other");
        });
  });
  while (!seeding)
    std::this_thread::yield();
  store.Publish(robot, gripper, std::nullopt);  // Arrives during the replay.
  installer.join();
  EXPECT_EQ(order, (std::vector<std::string>{"seed", "false"}));  // Never overwritten.

  // Concurrent publishers: the source's last level is the recorded level.
  std::vector<std::thread> publishers;
  for (int worker = 0; worker < 4; ++worker)
    publishers.emplace_back([&, worker] {
      for (int index = 0; index < 200; ++index)
        store.Publish(robot, gripper,
                      (index + worker) % 2 == 0
                          ? std::optional(HostCarriedObjectLostSignals::Loss{
                                robot, gripper, "attachment." + std::to_string(worker),
                                "box"})
                          : std::nullopt);
    });
  for (auto &publisher : publishers)
    publisher.join();
  std::lock_guard lock(order_mutex);
  EXPECT_EQ(order.back() == "true", store.Level(signal));
}


TEST(HostCarriedObjectLostSignalsTest, LastLossesCoalescePerGripperAndKeyAndOutliveTheLevel) {
  // Contract §10 "Coalescing" (SX-229/231).
  HostCarriedObjectLostSignals store;
  const core::ControllableComponentId robot("robot.g1");
  const core::CommandTargetId gripper("gripper"), other("other");
  using Loss = HostCarriedObjectLostSignals::Loss;
  const auto entries = [&] {
    std::vector<std::tuple<std::string, std::string, std::string>> result;
    for (const auto &last : store.LastLosses())
      result.emplace_back(last.command_target_id.value(), last.set_key,
                          last.loss_event_id);
    return result;
  };
  using Entries = std::vector<std::tuple<std::string, std::string, std::string>>;
  // True then false before any reader: the last loss stays (no level, no row).
  store.Publish(robot, gripper, Loss{robot, gripper, "attachment.1", "box", "k1", "loss.1"});
  store.Publish(robot, gripper, std::nullopt);
  EXPECT_TRUE(store.Losses().empty());
  EXPECT_EQ(entries(), (Entries{{"gripper", "k1", "loss.1"}}));
  // Two keys: both arrive; the same key again: one entry with the later id.
  store.Publish(robot, gripper, Loss{robot, gripper, "attachment.2", "cup", "k2", "loss.2"});
  store.Publish(robot, gripper, Loss{robot, gripper, "attachment.3", "can", "k1", "loss.3"});
  EXPECT_EQ(entries(), (Entries{{"gripper", "k1", "loss.3"}, {"gripper", "k2", "loss.2"}}));
  // A repeated detection of the same continuing loss keeps its id; a loss
  // without a key needs no entry.
  store.Publish(robot, gripper, Loss{robot, gripper, "attachment.3", "can", "k1", "loss.4"});
  store.Publish(robot, other, Loss{robot, other, "attachment.5", "pin", std::nullopt, "loss.5"});
  store.Publish(robot, other, Loss{robot, other, "attachment.6", "nut", "k3", "loss.6"});
  EXPECT_EQ(entries(), (Entries{{"gripper", "k1", "loss.3"}, {"gripper", "k2", "loss.2"},
                                {"other", "k3", "loss.6"}}));
  // A committed composition without `other` drops its entry, keeps the rest.
  store.InstallSink(
      [](const core::IoSignalId &, bool) { return std::optional<core::ErrorDTO>{}; },
      {HostCarriedObjectLostSignals::SignalId(robot, gripper)},
      [](const std::vector<core::IoSignalId> &) {});
  EXPECT_EQ(entries(), (Entries{{"gripper", "k1", "loss.3"}, {"gripper", "k2", "loss.2"}}));
}

TEST(HostCarriedObjectLostSignalsTest, AnAttachByTheGripperClearsItsRows) {
  // SX-202 (a): the Manager publishes `false` for every attach by a gripper.
  HostCarriedObjectLostSignals store;
  const core::ControllableComponentId robot("robot.g1");
  const core::CommandTargetId left("left"), right("right");
  using Loss = HostCarriedObjectLostSignals::Loss;
  store.Publish(robot, left, Loss{robot, left, "attachment.1", "box"});
  store.Publish(robot, left, Loss{robot, left, "attachment.2", "cup"});
  store.Publish(robot, right, Loss{robot, right, "attachment.3", "can"});
  store.Publish(robot, left, std::nullopt);  // The next attach by `left`.
  const auto losses = store.Losses();
  ASSERT_EQ(losses.size(), 1U);
  EXPECT_EQ(losses[0].carried_object_attachment_id, "attachment.3");
}

TEST(HostCarriedObjectLostSignalsTest, ACompositionReplacementDropsTheRowsOfGrippersItDoesNotHold) {
  // SX-202 (b): rows of targets still composed survive; the others drop.
  HostCarriedObjectLostSignals store;
  const core::ControllableComponentId robot("robot.g1");
  const core::CommandTargetId kept("kept"), gone("gone");
  using Loss = HostCarriedObjectLostSignals::Loss;
  store.Publish(robot, kept, Loss{robot, kept, "attachment.1", "box"});
  store.Publish(robot, gone, Loss{robot, gone, "attachment.2", "cup"});
  std::vector<core::IoSignalId> seeded;
  store.InstallSink(
      [](const core::IoSignalId &, bool) { return std::optional<core::ErrorDTO>{}; },
      {HostCarriedObjectLostSignals::SignalId(robot, kept)},
      [&](const std::vector<core::IoSignalId> &lost) { seeded = lost; });
  const auto losses = store.Losses();
  ASSERT_EQ(losses.size(), 1U);
  EXPECT_EQ(losses[0].command_target_id, kept);
  EXPECT_EQ(seeded, (std::vector{HostCarriedObjectLostSignals::SignalId(robot, kept)}));
  EXPECT_FALSE(store.Level(HostCarriedObjectLostSignals::SignalId(robot, gone)));
  // A composition with no gripper drops every row.
  store.InstallSink(
      [](const core::IoSignalId &, bool) { return std::optional<core::ErrorDTO>{}; }, {},
      [](const std::vector<core::IoSignalId> &) {});
  EXPECT_TRUE(store.Losses().empty());
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
