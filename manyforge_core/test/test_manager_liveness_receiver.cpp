#include <gtest/gtest.h>

#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <barrier>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace manyforge::core;

ContentDigest Digest(char value) {
  return ContentDigest("sha256:" + std::string(64U, value));
}

class Observations final : public IManagerLivenessObservationPort {
 public:
  Result<void> Publish(const ManagerLivenessObservationDTO& observation) override {
    std::lock_guard lock(mutex);
    order.push_back("publish");
    values.push_back(observation);
    return Result<void>::Ok();
  }

  std::vector<std::string> order;
  std::vector<ManagerLivenessObservationDTO> values;
  std::mutex mutex;
};

class FailSafes final : public IManagerLivenessFailSafePort {
 public:
  void LatchAndContain(
      const ManagerLivenessLossIntentDTO& intent) noexcept override {
    order.push_back("contain");
    if (external_order != nullptr) {
      external_order->push_back("contain");
    }
    values.push_back(intent);
  }

  std::vector<std::string>* external_order{};
  std::vector<std::string> order;
  std::vector<ManagerLivenessLossIntentDTO> values;
};

class ThrowingScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(TimePoint, std::function<void()>,
                          SchedulerPriority) override {
    throw std::runtime_error("schedule failed");
  }
  void cancel(ScheduleHandle) override {}
};

class CancelThrowingScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(TimePoint, std::function<void()>,
                          SchedulerPriority) override {
    return 1U;
  }
  void cancel(ScheduleHandle) override {
    throw std::runtime_error("cancel failed");
  }
};

class FailingObservations final : public IManagerLivenessObservationPort {
 public:
  Result<void> Publish(
      const ManagerLivenessObservationDTO& observation) override {
    values.push_back(observation);
    if (fail_next) {
      fail_next = false;
      return Result<void>::Err(
          ErrorDTO{"internal_error", "injected observation failure", {}});
    }
    return Result<void>::Ok();
  }

  bool fail_next{};
  std::vector<ManagerLivenessObservationDTO> values;
};

class BlockingReconciliationObservations final
    : public IManagerLivenessObservationPort {
 public:
  Result<void> Publish(
      const ManagerLivenessObservationDTO& observation) override {
    std::unique_lock lock(mutex);
    values.push_back(observation);
    if (observation.status.state == ManagerLivenessStateDTO::kCurrent &&
        !observation.status.command_fenced) {
      candidate_entered = true;
      condition.notify_all();
      condition.wait(lock, [&] { return release_candidate; });
    }
    return Result<void>::Ok();
  }

  void WaitForCandidate() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return candidate_entered; });
  }

  void ReleaseCandidate() {
    std::lock_guard lock(mutex);
    release_candidate = true;
    condition.notify_all();
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool candidate_entered{};
  bool release_candidate{};
  std::vector<ManagerLivenessObservationDTO> values;
};

ManagerLivenessReceiverConfig Config() {
  return {CellId("cell.1"),
          RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime.epoch.1"),
          RuntimeUnitId("manager"),
          RuntimeEpoch("manager.epoch.1"),
          RequestOwnerEpoch("manager.owner.1"),
          ManagerLivenessPolicyDTO{RuntimePolicyId("policy.1"),
                                   RuntimePolicyRevision("1"), 10, 30,
                                   ProductContractId("safe.1"),
                                   ProductContractRevision("1")},
          RetainedBundleId("bundle.1"),
          Digest('a')};
}

ManagerHeartbeatDTO Heartbeat(std::uint64_t sequence = 1U) {
  return {CellId("cell.1"), RuntimeUnitId("manager"),
          RuntimeEpoch("manager.epoch.1"),
          RequestOwnerEpoch("manager.owner.1"), sequence,
          RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")};
}

ManagerLivenessReconciliationAck Ack(
    RuntimeEpoch epoch = RuntimeEpoch("manager.epoch.1"),
    RequestOwnerEpoch owner = RequestOwnerEpoch("manager.owner.1")) {
  return {CellId("cell.1"), RuntimeUnitId("manager"), std::move(epoch),
          std::move(owner), RetainedBundleId("bundle.1"), Digest('a'),
          Digest('c')};
}

TEST(ManagerLivenessReceiver, ObservationMayDetachReentrantly) {
  class DetachingObservations final : public IManagerLivenessObservationPort {
   public:
    ManagerLivenessReceiver* receiver{};
    bool detached{};
    Result<void> Publish(const ManagerLivenessObservationDTO&) override {
      if (receiver) detached = receiver->DetachSchedulerUntil(
          std::chrono::steady_clock::now() + std::chrono::seconds(1));
      return Result<void>::Ok();
    }
  } observations;
  FakeClock clock;
  ManualScheduler scheduler;
  FailSafes fail_safe;
  ManagerLivenessReceiver receiver(Config(), clock, scheduler,
                                    [] { return Digest('b'); },
                                    fail_safe, observations);
  observations.receiver = &receiver;
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  EXPECT_TRUE(observations.detached);
}

TEST(ManagerLivenessReceiver, DetachedReceiverMayOutliveSchedulerAndClock) {
  auto clock = std::make_unique<FakeClock>();
  auto scheduler = std::make_unique<ManualScheduler>();
  Observations observations;
  FailSafes fail_safe;
  auto receiver = std::make_unique<ManagerLivenessReceiver>(
      Config(), *clock, *scheduler, [] { return Digest('b'); },
      fail_safe, observations);
  ASSERT_TRUE(receiver->Renew(Heartbeat()).ok());
  receiver->DetachScheduler();
  scheduler.reset();
  clock.reset();
  EXPECT_FALSE(receiver->Renew(Heartbeat(2U)).ok());
  EXPECT_NO_THROW(static_cast<void>(receiver->observation()));
  EXPECT_NO_THROW(receiver.reset());
}

TEST(ManagerLivenessReceiver, StrictRenewalAndExactDeadlineAreLocalAndLatched) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  fail_safe.external_order = &observations.order;
  unsigned safe_calls = 0U;
  ManagerLivenessReceiver receiver(
      Config(), clock, scheduler,
      [&] {
        ++safe_calls;
        observations.order.push_back("safe");
        return Digest('b');
      },
      fail_safe, observations);

  ASSERT_EQ(observations.values.size(), 1U);
  EXPECT_EQ(observations.values.back().status.state,
            ManagerLivenessStateDTO::kUnreconciled);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kCurrent);
  EXPECT_TRUE(receiver.status().command_fenced);
  EXPECT_EQ(scheduler.last_priority(), SchedulerPriority::RuntimeSafety);

  EXPECT_FALSE(receiver.Renew(Heartbeat()).ok());
  auto regressed = Heartbeat(0U);
  EXPECT_FALSE(receiver.Renew(regressed).ok());
  auto foreign = Heartbeat(2U);
  foreign.cell_id = CellId("foreign");
  EXPECT_FALSE(receiver.Renew(foreign).ok());
  foreign = Heartbeat(2U);
  foreign.runtime_policy_revision = RuntimePolicyRevision("2");
  EXPECT_FALSE(receiver.Renew(foreign).ok());

  clock.set(29);
  scheduler.advance_to(29);
  EXPECT_EQ(safe_calls, 0U);
  clock.set(30);
  scheduler.advance_to(30);
  EXPECT_EQ(safe_calls, 1U);
  ASSERT_EQ(fail_safe.values.size(), 1U);
  EXPECT_EQ(fail_safe.values.front().kind,
            ManagerLivenessLossKindDTO::kExpiry);
  EXPECT_EQ(fail_safe.values.front().receiver_status_sequence,
            receiver.status().receiver_status_sequence);
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kExpired);
  EXPECT_TRUE(receiver.status().command_fenced);
  ASSERT_GE(observations.order.size(), 3U);
  EXPECT_EQ(observations.order[observations.order.size() - 3U], "safe");
  EXPECT_EQ(observations.order[observations.order.size() - 2U], "contain");
  EXPECT_EQ(observations.order.back(), "publish");
  EXPECT_FALSE(receiver.Renew(Heartbeat(2U)).ok());
  scheduler.advance_to(100);
  EXPECT_EQ(safe_calls, 1U);
}

TEST(ManagerLivenessReceiver, RearmInvalidatesStaleDeadline) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  unsigned safe_calls = 0U;
  ManagerLivenessReceiver receiver(
      Config(), clock, scheduler,
      [&] {
        ++safe_calls;
        return Digest('b');
      },
      fail_safe, observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat(1U)).ok());
  clock.set(20);
  ASSERT_TRUE(receiver.Renew(Heartbeat(2U)).ok());
  clock.set(30);
  scheduler.advance_to(30);
  EXPECT_EQ(safe_calls, 0U);
  clock.set(50);
  scheduler.advance_to(50);
  EXPECT_EQ(safe_calls, 1U);
}

TEST(ManagerLivenessReceiver, CallbackFailureStillPublishesExpiredOnce) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  ManagerLivenessReceiver receiver(
      Config(), clock, scheduler,
      []() -> ContentDigest { throw std::runtime_error("safe failed"); },
      fail_safe, observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  clock.set(30);
  EXPECT_NO_THROW(scheduler.advance_to(30));
  ASSERT_EQ(observations.values.back().status.state,
            ManagerLivenessStateDTO::kExpired);
  ASSERT_EQ(fail_safe.values.size(), 1U);
  EXPECT_TRUE(
      observations.values.back().status.fault_or_effect_reference.has_value());
}

TEST(ManagerLivenessReceiver,
     ScheduleFailureFencesSafesPublishesAndReturnsStableError) {
  FakeClock clock;
  ThrowingScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  unsigned safe_calls = 0U;
  ManagerLivenessReceiver receiver(
      Config(), clock, scheduler,
      [&] {
        ++safe_calls;
        observations.order.push_back("safe");
        return Digest('b');
      },
      fail_safe, observations);

  const auto renewed = receiver.Renew(Heartbeat());
  ASSERT_FALSE(renewed.ok());
  EXPECT_EQ(renewed.error().code, "internal_error");
  EXPECT_EQ(renewed.error().details.at("reason"),
            "watchdog_schedule_failed");
  EXPECT_EQ(safe_calls, 1U);
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kExpired);
  EXPECT_TRUE(receiver.status().command_fenced);
  ASSERT_GE(observations.order.size(), 2U);
  EXPECT_EQ(observations.order[observations.order.size() - 2U], "safe");
  EXPECT_EQ(observations.order.back(), "publish");
  EXPECT_FALSE(receiver.Renew(Heartbeat(2U)).ok());
  EXPECT_EQ(safe_calls, 1U);
}

TEST(ManagerLivenessReceiver, ReplacementRequiresExactRenewalAndAcknowledgement) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  unsigned safe_calls = 0U;
  ManagerLivenessReceiver receiver(Config(), clock, scheduler,
                                   [&] {
                                     ++safe_calls;
                                     return Digest('b');
                                   },
                                   fail_safe,
                                   observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  ASSERT_TRUE(receiver.CompleteReconciliation(Ack()).ok());
  EXPECT_FALSE(receiver.status().command_fenced);

  ASSERT_TRUE(receiver.BeginManagerEpochReplacement(
      RuntimeUnitId("manager"), RuntimeEpoch("manager.epoch.2"),
      RequestOwnerEpoch("manager.owner.2")).ok());
  ASSERT_EQ(fail_safe.values.size(), 1U);
  EXPECT_EQ(safe_calls, 1U);
  EXPECT_EQ(fail_safe.values.front().kind,
            ManagerLivenessLossKindDTO::kManagerEpochReplacement);
  EXPECT_EQ(fail_safe.values.front().lost_manager_runtime_epoch,
            RuntimeEpoch("manager.epoch.1"));
  EXPECT_EQ(fail_safe.values.front().lost_manager_owner_epoch,
            RequestOwnerEpoch("manager.owner.1"));
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kUnreconciled);
  EXPECT_TRUE(receiver.status().command_fenced);
  EXPECT_FALSE(receiver.Renew(Heartbeat(2U)).ok());
  auto replacement = Heartbeat(1U);
  replacement.manager_runtime_epoch = RuntimeEpoch("manager.epoch.2");
  replacement.manager_owner_epoch = RequestOwnerEpoch("manager.owner.2");
  ASSERT_TRUE(receiver.Renew(replacement).ok());
  EXPECT_TRUE(receiver.status().command_fenced);
  EXPECT_FALSE(receiver.CompleteReconciliation(Ack()).ok());
  ASSERT_TRUE(receiver.CompleteReconciliation(
      Ack(RuntimeEpoch("manager.epoch.2"),
          RequestOwnerEpoch("manager.owner.2"))).ok());
  EXPECT_FALSE(receiver.status().command_fenced);
}

TEST(ManagerLivenessReceiver,
     RequiredSetRebindCancelsReceiptAndRequiresFreshExactReconciliation) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  unsigned safe_calls = 0U;
  ManagerLivenessReceiver receiver(
      Config(), clock, scheduler,
      [&] {
        ++safe_calls;
        return Digest('b');
      },
      fail_safe, observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat(8U)).ok());
  ASSERT_TRUE(receiver.CompleteReconciliation(Ack()).ok());
  const auto predecessor_sequence = receiver.status().receiver_status_sequence;

  ASSERT_TRUE(
      receiver.RebindRequiredSet(RetainedBundleId("bundle.2"), Digest('d'))
          .ok());
  const auto rebound = receiver.observation();
  EXPECT_EQ(rebound.active_bundle_id, RetainedBundleId("bundle.2"));
  EXPECT_EQ(rebound.required_runtime_set_digest, Digest('d'));
  EXPECT_EQ(rebound.status.state, ManagerLivenessStateDTO::kUnreconciled);
  EXPECT_TRUE(rebound.status.command_fenced);
  EXPECT_EQ(rebound.status.last_accepted_heartbeat_sequence, 0U);
  EXPECT_GT(rebound.status.receiver_status_sequence, predecessor_sequence);
  EXPECT_EQ(safe_calls, 0U);
  EXPECT_TRUE(fail_safe.values.empty());

  clock.set(30);
  scheduler.advance_to(30);
  EXPECT_EQ(safe_calls, 0U);
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kUnreconciled);
  EXPECT_FALSE(receiver.CompleteReconciliation(Ack()).ok());

  ASSERT_TRUE(receiver.Renew(Heartbeat(1U)).ok());
  auto successor_ack = Ack();
  successor_ack.active_bundle_id = RetainedBundleId("bundle.2");
  successor_ack.required_runtime_set_digest = Digest('d');
  ASSERT_TRUE(receiver.CompleteReconciliation(successor_ack).ok());
  EXPECT_FALSE(receiver.status().command_fenced);
}

TEST(ManagerLivenessReceiver,
     ExpiredManagerEpochCannotBeResurrectedByRequiredSetRebind) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  ManagerLivenessReceiver receiver(Config(), clock, scheduler,
                                   [] { return Digest('b'); }, fail_safe,
                                   observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  clock.set(30);
  scheduler.advance_to(30);
  ASSERT_EQ(receiver.status().state, ManagerLivenessStateDTO::kExpired);

  const auto rebound =
      receiver.RebindRequiredSet(RetainedBundleId("bundle.2"), Digest('d'));
  ASSERT_FALSE(rebound.ok());
  EXPECT_EQ(rebound.error().details.at("reason"), "manager_epoch_expired");
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kExpired);
  EXPECT_FALSE(receiver.Renew(Heartbeat(2U)).ok());
}

TEST(ManagerLivenessReceiver,
     ReplacementCancelFailureStaysFencedAndReturnsStableError) {
  FakeClock clock;
  CancelThrowingScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  ManagerLivenessReceiver receiver(Config(), clock, scheduler,
                                   [] { return Digest('b'); }, fail_safe,
                                   observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());

  const auto replaced = receiver.BeginManagerEpochReplacement(
      RuntimeUnitId("manager"), RuntimeEpoch("manager.epoch.2"),
      RequestOwnerEpoch("manager.owner.2"));
  ASSERT_FALSE(replaced.ok());
  EXPECT_EQ(replaced.error().code, "internal_error");
  EXPECT_EQ(replaced.error().details.at("reason"),
            "watchdog_schedule_failed");
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kUnreconciled);
  EXPECT_TRUE(receiver.status().command_fenced);
  EXPECT_EQ(observations.values.back().status.state,
            ManagerLivenessStateDTO::kUnreconciled);
  EXPECT_TRUE(observations.values.back().status.command_fenced);
}

TEST(ManagerLivenessReceiver,
     FailedReconciliationPublicationStaysFencedAndExactRetrySucceeds) {
  FakeClock clock;
  ManualScheduler scheduler;
  FailingObservations observations;
  FailSafes fail_safe;
  ManagerLivenessReceiver receiver(Config(), clock, scheduler,
                                   [] { return Digest('b'); }, fail_safe,
                                   observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());

  observations.fail_next = true;
  const auto failed = receiver.CompleteReconciliation(Ack());
  ASSERT_FALSE(failed.ok());
  EXPECT_TRUE(receiver.status().command_fenced);
  const auto failed_sequence = receiver.status().receiver_status_sequence;

  ASSERT_TRUE(receiver.CompleteReconciliation(Ack()).ok());
  EXPECT_FALSE(receiver.status().command_fenced);
  EXPECT_GT(receiver.status().receiver_status_sequence, failed_sequence);
}

TEST(ManagerLivenessReceiver,
     ExpiryWinningBlockedReconciliationPublicationNeverUnfencesLocally) {
  FakeClock clock;
  ManualScheduler scheduler;
  BlockingReconciliationObservations observations;
  FailSafes fail_safe;
  unsigned safe_calls = 0U;
  ManagerLivenessReceiver receiver(
      Config(), clock, scheduler,
      [&] {
        ++safe_calls;
        return Digest('b');
      },
      fail_safe, observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());

  auto reconciliation = std::async(
      std::launch::async, [&] { return receiver.CompleteReconciliation(Ack()); });
  observations.WaitForCandidate();
  clock.set(30);
  scheduler.advance_to(30);
  EXPECT_EQ(safe_calls, 1U);
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kExpired);
  EXPECT_TRUE(receiver.status().command_fenced);

  observations.ReleaseCandidate();
  const auto reconciled = reconciliation.get();
  ASSERT_FALSE(reconciled.ok());
  EXPECT_EQ(reconciled.error().code, "invalid_state_transition");
  EXPECT_EQ(reconciled.error().details.at("reason"),
            "manager_liveness_reconciliation_superseded");
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kExpired);
  EXPECT_TRUE(receiver.status().command_fenced);
}

TEST(ManagerLivenessReceiver, DestructionInvalidatesScheduledGeneration) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  unsigned safe_calls = 0U;
  {
    ManagerLivenessReceiver receiver(
        Config(), clock, scheduler,
        [&] {
          ++safe_calls;
          return Digest('b');
        },
        fail_safe, observations);
    ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  }
  const auto publications = observations.values.size();
  clock.set(30);
  scheduler.advance_to(30);
  EXPECT_EQ(safe_calls, 0U);
  EXPECT_EQ(observations.values.size(), publications);
}

TEST(ManagerLivenessReceiver, ConcurrentRenewAndExpiryLinearizeSafely) {
  FakeClock clock;
  ManualScheduler scheduler;
  Observations observations;
  FailSafes fail_safe;
  unsigned safe_calls = 0U;
  ManagerLivenessReceiver receiver(
      Config(), clock, scheduler,
      [&] {
        ++safe_calls;
        return Digest('b');
      },
      fail_safe, observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  clock.set(30);
  std::barrier start(3);
  Result<void> renewal = Result<void>::Err(ErrorCode::kInternalError);
  std::thread renewer([&] {
    start.arrive_and_wait();
    renewal = receiver.Renew(Heartbeat(2U));
  });
  std::thread expiry([&] {
    start.arrive_and_wait();
    scheduler.advance_to(30);
  });
  start.arrive_and_wait();
  renewer.join();
  expiry.join();

  const auto current = receiver.status();
  EXPECT_TRUE(current.state == ManagerLivenessStateDTO::kCurrent ||
              current.state == ManagerLivenessStateDTO::kExpired);
  EXPECT_LE(safe_calls, 1U);
  EXPECT_EQ(renewal.ok(),
            current.state == ManagerLivenessStateDTO::kCurrent);
}

}  // namespace
