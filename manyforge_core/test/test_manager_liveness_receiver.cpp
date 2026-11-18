#include <gtest/gtest.h>

#include <manyforge_core/manager_liveness_receiver.hpp>
#include <condition_variable>
#include <future>
#include <mutex>
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
    values.push_back(observation);
    return Result<void>::Ok();
  }

  std::vector<ManagerLivenessObservationDTO> values;
  std::mutex mutex;
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
          ManagerLivenessPolicyDTO{RuntimePolicyId("policy.1"),
                                   RuntimePolicyRevision("1"), 10},
          RetainedBundleId("bundle.1"),
          Digest('a')};
}

ManagerHeartbeatDTO Heartbeat(std::uint64_t sequence = 1U) {
  return {CellId("cell.1"), RuntimeUnitId("manager"), sequence,
          RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")};
}

ManagerLivenessReconciliationAck Ack(
    RuntimeUnitId manager = RuntimeUnitId("manager")) {
  return {CellId("cell.1"), std::move(manager), RetainedBundleId("bundle.1"), Digest('a'), Digest('c')};
}

TEST(ManagerLivenessReceiver, StrictRenewalNeverExpires) {
  Observations observations;
  ManagerLivenessReceiver receiver(Config(), observations);

  ASSERT_EQ(observations.values.size(), 1U);
  EXPECT_EQ(observations.values.back().status.state,
            ManagerLivenessStateDTO::kUnreconciled);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kCurrent);
  EXPECT_TRUE(receiver.status().command_fenced);

  auto regressed = Heartbeat(0U);
  EXPECT_FALSE(receiver.Renew(regressed).ok());
  ASSERT_TRUE(receiver.CompleteReconciliation(Ack()).ok());

  const auto publications = observations.values.size();
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kCurrent);
  EXPECT_FALSE(receiver.status().command_fenced);
  EXPECT_EQ(observations.values.size(), publications);
  ASSERT_TRUE(receiver.Renew(Heartbeat(2U)).ok());
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kCurrent);
  EXPECT_FALSE(receiver.status().command_fenced);
}

TEST(ManagerLivenessReceiver,
     RequiredSetRebindCancelsReceiptAndRequiresFreshExactReconciliation) {
  Observations observations;
  ManagerLivenessReceiver receiver(Config(), observations);
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
  EXPECT_FALSE(receiver.CompleteReconciliation(Ack()).ok());

  ASSERT_TRUE(receiver.Renew(Heartbeat(1U)).ok());
  auto successor_ack = Ack();
  successor_ack.active_bundle_id = RetainedBundleId("bundle.2");
  successor_ack.required_runtime_set_digest = Digest('d');
  ASSERT_TRUE(receiver.CompleteReconciliation(successor_ack).ok());
  EXPECT_FALSE(receiver.status().command_fenced);
}

TEST(ManagerLivenessReceiver,
     FailedReconciliationPublicationStaysFencedAndExactRetrySucceeds) {
  FailingObservations observations;
  ManagerLivenessReceiver receiver(Config(), observations);
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
     RebindWinningBlockedReconciliationPublicationNeverUnfencesLocally) {
  BlockingReconciliationObservations observations;
  ManagerLivenessReceiver receiver(Config(), observations);
  ASSERT_TRUE(receiver.Renew(Heartbeat()).ok());

  auto reconciliation = std::async(
      std::launch::async, [&] { return receiver.CompleteReconciliation(Ack()); });
  observations.WaitForCandidate();
  ASSERT_TRUE(
      receiver.RebindRequiredSet(RetainedBundleId("bundle.2"), Digest('d'))
          .ok());
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kUnreconciled);
  EXPECT_TRUE(receiver.status().command_fenced);

  observations.ReleaseCandidate();
  const auto reconciled = reconciliation.get();
  ASSERT_FALSE(reconciled.ok());
  EXPECT_EQ(reconciled.error().code, "invalid_state_transition");
  EXPECT_EQ(reconciled.error().details.at("reason"),
            "manager_liveness_reconciliation_superseded");
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kUnreconciled);
  EXPECT_TRUE(receiver.status().command_fenced);
}

}  // namespace
