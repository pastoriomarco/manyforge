#include <gtest/gtest.h>

#include "manager_liveness_rpc_internal.hpp"
#include <array>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

namespace manyforge::component_manager::detail {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigest2 =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

ManagerLivenessTransportRequestDTO Request() {
  const ManagerLivenessPolicyDTO policy{
      RuntimePolicyId("policy.native"),
      RuntimePolicyRevision(std::string(kDigest)),
      250,
      1000,
      ProductContractId("safe.native"),
      ProductContractRevision(std::string(kDigest2))};
  std::vector<RequiredManagerLivenessReceiverDTO> receivers{
      {RuntimeUnitId("native.a"), RuntimeEpoch("epoch.a"),
       policy.runtime_policy_id, policy.runtime_policy_revision},
      {RuntimeUnitId("native.b"), RuntimeEpoch("epoch.b"),
       policy.runtime_policy_id, policy.runtime_policy_revision}};
  const auto set_digest = ComputeRequiredRuntimeSetDigest(receivers);
  ManagerLivenessRequiredSetDTO set{CellId("cell.1"),
                                    RetainedBundleId("bundle.1"),
                                    set_digest,
                                    RuntimeUnitId("manager.1"),
                                    RuntimeEpoch("manager-epoch.1"),
                                    RequestOwnerEpoch("manager-owner.1"),
                                    std::move(receivers)};
  return {set.required_receivers.front().runtime_unit_id,
          ActiveBundlePublicationGeneration(4U),
          GraphRevision("graph.1"),
          ContentDigest(std::string(kDigest)),
          set,
          policy,
          ManagerHeartbeatDTO{
              set.cell_id, set.manager_runtime_unit_id,
              set.manager_runtime_epoch, set.manager_owner_epoch, 7U,
              policy.runtime_policy_id, policy.runtime_policy_revision},
          ManagerLivenessReconciliationAck{
              set.cell_id, set.manager_runtime_unit_id,
              set.manager_runtime_epoch, set.manager_owner_epoch,
              set.active_bundle_id, set.required_runtime_set_digest,
              ContentDigest(std::string(kDigest2))}};
}

ManagerLivenessLossIntentDTO LossIntent(std::uint64_t sequence = 3U) {
  const auto request = Request();
  const auto &receiver = request.required_set.required_receivers.front();
  return {request.required_set.cell_id,
          request.required_set.active_bundle_id,
          request.required_set.required_runtime_set_digest,
          receiver.runtime_unit_id,
          receiver.runtime_epoch,
          receiver.runtime_policy_id,
          receiver.runtime_policy_revision,
          request.required_set.manager_runtime_unit_id,
          request.required_set.manager_runtime_epoch,
          request.required_set.manager_owner_epoch,
          sequence,
          ManagerLivenessLossKindDTO::kExpiry};
}

class LivenessObservations final : public IManagerLivenessObservationPort {
public:
  Result<void>
  Publish(const ManagerLivenessObservationDTO &observation) override {
    values.push_back(observation);
    return Result<void>::Ok();
  }

  std::vector<ManagerLivenessObservationDTO> values;
};

class CountingContainment final : public IManagerLivenessFailSafePort {
public:
  void LatchAndContain(
      const ManagerLivenessLossIntentDTO &intent) noexcept override {
    intents.push_back(intent);
  }

  std::vector<ManagerLivenessLossIntentDTO> intents;
};

ManagerLivenessReceiverConfig ReceiverConfig() {
  const auto request = Request();
  const auto &receiver = request.required_set.required_receivers.front();
  return {request.required_set.cell_id,
          receiver.runtime_unit_id,
          receiver.runtime_epoch,
          request.required_set.manager_runtime_unit_id,
          request.required_set.manager_runtime_epoch,
          request.required_set.manager_owner_epoch,
          request.policy,
          request.required_set.active_bundle_id,
          request.required_set.required_runtime_set_digest};
}

TEST(ManagerLivenessCommandAdmission,
     IsInitiallyClosedAndStaleReconciliationCannotReopenAfterLoss) {
  ManagerLivenessCommandAdmission admission;
  EXPECT_FALSE(admission.open());
  EXPECT_FALSE(admission.Allows(ManagerLivenessCommandClass::kEffectful));
  EXPECT_TRUE(admission.Allows(ManagerLivenessCommandClass::kStop));
  EXPECT_TRUE(admission.Allows(ManagerLivenessCommandClass::kReadOnly));
  const auto first_generation = admission.generation();
  admission.OpenIfUnchanged(first_generation);
  ASSERT_TRUE(admission.open());

  const auto evidence = admission.CloseAndAttest(LossIntent());
  EXPECT_FALSE(admission.open());
  EXPECT_TRUE(manyforge::content_identity::is_sha256_digest(evidence.value()));
  admission.OpenIfUnchanged(first_generation);
  EXPECT_FALSE(admission.open());

  const auto current_generation = admission.generation();
  admission.OpenIfUnchanged(current_generation);
  EXPECT_TRUE(admission.open());
  EXPECT_NE(admission.CloseAndAttest(LossIntent(4U)), evidence);
  EXPECT_FALSE(admission.open());
}

TEST(ManagerLivenessCommandAdmission,
     RequiredSetRebindInvalidatesPredecessorOpenGeneration) {
  ManagerLivenessCommandAdmission admission;
  const auto predecessor = admission.generation();
  admission.OpenIfUnchanged(predecessor);
  ASSERT_TRUE(admission.open());

  admission.CloseForRequiredSetRebind();
  EXPECT_FALSE(admission.open());
  admission.OpenIfUnchanged(predecessor);
  EXPECT_FALSE(admission.open());
}

TEST(ManagerLivenessCommandAdmission,
     ExactLeaseOpensThenExpiryClosesBeforeExactlyOneContainment) {
  auto clock = std::make_shared<FakeClock>();
  auto scheduler = std::make_unique<ManualScheduler>();
  auto *const scheduler_observer = scheduler.get();
  LivenessObservations observations;
  CountingContainment containment;
  ManagerLivenessCommandAdmission admission;
  ChildOwnedManagerLivenessReceiver child_lifetime(clock, std::move(scheduler));
  child_lifetime.Install(
      ReceiverConfig(),
      [&](const ManagerLivenessLossIntentDTO &intent) {
        EXPECT_TRUE(admission.open());
        return admission.CloseAndAttest(intent);
      },
      containment, observations);
  auto *const receiver = child_lifetime.get();
  ASSERT_NE(receiver, nullptr);

  const auto request = Request();
  const auto admission_generation = admission.generation();
  ASSERT_TRUE(receiver->Renew(request.heartbeat).ok());
  ASSERT_TRUE(receiver->CompleteReconciliation(*request.reconciliation).ok());
  admission.OpenIfUnchanged(admission_generation);
  ASSERT_TRUE(admission.Allows(ManagerLivenessCommandClass::kEffectful));

  clock->set(request.policy.expiry_timeout_ns);
  scheduler_observer->advance_to(request.policy.expiry_timeout_ns);
  EXPECT_FALSE(admission.Allows(ManagerLivenessCommandClass::kEffectful));
  ASSERT_EQ(containment.intents.size(), 1U);
  EXPECT_EQ(containment.intents.front().kind,
            ManagerLivenessLossKindDTO::kExpiry);
  scheduler_observer->advance_to(request.policy.expiry_timeout_ns * 2);
  EXPECT_EQ(containment.intents.size(), 1U);
  admission.OpenIfUnchanged(admission_generation);
  EXPECT_FALSE(admission.open());
}

TEST(ManagerLivenessCommandAdmission,
     ChildOwnedWatchdogSurvivesCompositionReplacementAndRejectsStaleManager) {
  auto clock = std::make_shared<FakeClock>();
  auto scheduler = std::make_unique<ManualScheduler>();
  auto *const scheduler_observer = scheduler.get();
  LivenessObservations observations;
  CountingContainment containment;
  ManagerLivenessCommandAdmission admission;
  ChildOwnedManagerLivenessReceiver child_lifetime(clock, std::move(scheduler));
  child_lifetime.Install(
      ReceiverConfig(),
      [&](const ManagerLivenessLossIntentDTO &intent) {
        return admission.CloseAndAttest(intent);
      },
      containment, observations);
  auto *const receiver = child_lifetime.get();
  const auto request = Request();
  const auto generation = admission.generation();
  ASSERT_TRUE(receiver->Renew(request.heartbeat).ok());
  ASSERT_TRUE(receiver->CompleteReconciliation(*request.reconciliation).ok());
  admission.OpenIfUnchanged(generation);
  ASSERT_TRUE(admission.open());

  auto composition = std::make_unique<int>(1);
  composition.reset();
  ASSERT_TRUE(
      receiver
          ->BeginManagerEpochReplacement(RuntimeUnitId("manager.1"),
                                         RuntimeEpoch("manager-epoch.2"),
                                         RequestOwnerEpoch("manager-owner.2"))
          .ok());
  EXPECT_FALSE(admission.open());
  ASSERT_EQ(containment.intents.size(), 1U);
  EXPECT_FALSE(receiver->Renew(request.heartbeat).ok());
  EXPECT_FALSE(receiver->CompleteReconciliation(*request.reconciliation).ok());
  admission.OpenIfUnchanged(generation);
  EXPECT_FALSE(admission.open());

  clock->set(request.policy.expiry_timeout_ns * 2);
  scheduler_observer->advance_to(request.policy.expiry_timeout_ns * 2);
  EXPECT_EQ(containment.intents.size(), 1U);
}

TEST(ManagerLivenessBatchPolicy,
     NativeSubsetCarriesFullCrossTransportSetAndFailureWithholdsFullExpiry) {
  std::array requests{Request(), Request()};
  const auto &native_policy = requests[0].policy;
  for (auto &request : requests) {
    request.required_set.required_receivers.push_back(
        {RuntimeUnitId("program-runner.external"),
         RuntimeEpoch("program-runner.epoch"),
         RuntimePolicyId("policy.program-runner"), RuntimePolicyRevision("1")});
    request.required_set.required_runtime_set_digest =
        ComputeRequiredRuntimeSetDigest(
            request.required_set.required_receivers);
    request.reconciliation->required_runtime_set_digest =
        request.required_set.required_runtime_set_digest;
  }
  requests[1].receiver_runtime_unit_id =
      requests[1].required_set.required_receivers[1].runtime_unit_id;
  EXPECT_NO_THROW(EncodeManagerLivenessRpcRequest(requests[0]));
  EXPECT_NO_THROW(EncodeManagerLivenessRpcRequest(requests[1]));
  EXPECT_EQ(requests[0].required_set, requests[1].required_set);
  ASSERT_EQ(requests[0].required_set.required_receivers.size(), 3U);
  for (const auto &request : requests) {
    EXPECT_EQ(request.policy, native_policy);
    EXPECT_EQ(request.required_set.required_runtime_set_digest,
              ComputeRequiredRuntimeSetDigest(
                  request.required_set.required_receivers));
    EXPECT_EQ(request.required_set.required_receivers.back().runtime_unit_id,
              RuntimeUnitId("program-runner.external"));
  }

  const auto started = ManagerLivenessBatchDeadline::Clock::time_point(
      std::chrono::nanoseconds(100));
  const ManagerLivenessBatchDeadline deadline(started,
                                              std::chrono::nanoseconds(750));
  const std::array receiver_deadlines{deadline.value(), deadline.value()};
  EXPECT_EQ(receiver_deadlines[0], started + std::chrono::nanoseconds(750));
  EXPECT_EQ(receiver_deadlines[1], receiver_deadlines[0]);

  ManagerLivenessRetryGate retry_gate;
  ASSERT_TRUE(retry_gate.attempt_allowed(100U));
  retry_gate.RecordFailure(100U, 1000U);
  for (std::size_t receiver = 0U; receiver < 2U; ++receiver) {
    SCOPED_TRACE(receiver);
    EXPECT_FALSE(retry_gate.attempt_allowed(100U));
    EXPECT_FALSE(retry_gate.attempt_allowed(1099U));
  }
  EXPECT_TRUE(retry_gate.attempt_allowed(1100U));
  retry_gate.RecordSuccess();
  EXPECT_FALSE(retry_gate.failed());
  EXPECT_TRUE(retry_gate.attempt_allowed(1100U));
}

TEST(ManagerLivenessRpcInternal,
     RetryPolicyUsesNativeBatchCertaintyNotUnrelatedReceiverFailure) {
  constexpr std::uint64_t origin = 31ULL * 365ULL * 86400ULL * 1'000'000'000ULL;
  for (const auto progress : {ManagerLivenessBatchProgress::kNotStarted,
                              ManagerLivenessBatchProgress::kInFlight,
                              ManagerLivenessBatchProgress::kValidated}) {
    ManagerLivenessRetryGate gate;
    gate.RecordFailure(origin, 1000);
    EXPECT_FALSE(gate.attempt_allowed(origin + 999));
    EXPECT_TRUE(gate.attempt_allowed(origin + 1000));
    gate.RecordAttemptFailure(progress, origin + 1000, 1000);
    EXPECT_EQ(gate.failed(),
              progress == ManagerLivenessBatchProgress::kInFlight);
    EXPECT_EQ(gate.attempt_allowed(origin + 1001),
              progress != ManagerLivenessBatchProgress::kInFlight);
    EXPECT_TRUE(gate.attempt_allowed(origin + 2000));
    gate.RecordSuccess(); // the same reset used for a successor publication
    EXPECT_FALSE(gate.failed());
    EXPECT_TRUE(gate.attempt_allowed(origin + 1001));
  }
  ManagerLivenessRetryGate gate;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  gate.RecordAttemptFailure(ManagerLivenessBatchProgress::kInFlight,
                            maximum - 2, 1000);
  EXPECT_FALSE(gate.attempt_allowed(maximum - 1));
  EXPECT_TRUE(gate.attempt_allowed(maximum));
}

TEST(ManagerLivenessRpcInternal,
     RoundTripsExactPolicyRequiredSetHeartbeatAndReconciliation) {
  const auto request = Request();
  const auto bytes = EncodeManagerLivenessRpcRequest(request);
  EXPECT_TRUE(IsManagerLivenessRpcPacket(bytes));
  const auto decoded = DecodeManagerLivenessRpcRequest(bytes);
  EXPECT_EQ(decoded.receiver_runtime_unit_id, request.receiver_runtime_unit_id);
  EXPECT_EQ(decoded.required_set, request.required_set);
  EXPECT_EQ(decoded.policy, request.policy);
  EXPECT_EQ(decoded.heartbeat, request.heartbeat);
  ASSERT_EQ(decoded.reconciliation.has_value(),
            request.reconciliation.has_value());
  ASSERT_TRUE(decoded.reconciliation.has_value());
  const auto &decoded_ack = *decoded.reconciliation;
  const auto &request_ack = *request.reconciliation;
  EXPECT_EQ(decoded_ack.cell_id, request_ack.cell_id);
  EXPECT_EQ(decoded_ack.manager_runtime_unit_id,
            request_ack.manager_runtime_unit_id);
  EXPECT_EQ(decoded_ack.manager_runtime_epoch,
            request_ack.manager_runtime_epoch);
  EXPECT_EQ(decoded_ack.manager_owner_epoch, request_ack.manager_owner_epoch);
  EXPECT_EQ(decoded_ack.active_bundle_id, request_ack.active_bundle_id);
  EXPECT_EQ(decoded_ack.required_runtime_set_digest,
            request_ack.required_runtime_set_digest);
  EXPECT_EQ(decoded_ack.reconciliation_evidence_digest,
            request_ack.reconciliation_evidence_digest);

  const auto &receiver = request.required_set.required_receivers.front();
  ManagerLivenessStatusDTO status{request.required_set.cell_id,
                                  receiver.runtime_unit_id,
                                  receiver.runtime_epoch,
                                  request.required_set.manager_runtime_unit_id,
                                  request.required_set.manager_runtime_epoch,
                                  request.required_set.manager_owner_epoch,
                                  request.policy.runtime_policy_id,
                                  request.policy.runtime_policy_revision,
                                  request.heartbeat.heartbeat_sequence,
                                  3U,
                                  0,
                                  ManagerLivenessStateDTO::kCurrent,
                                  false,
                                  std::nullopt};
  ManagerLivenessObservationDTO observation{
      status, request.required_set.active_bundle_id,
      request.required_set.required_runtime_set_digest,
      ComputeManagerLivenessStatusDigest(status)};
  EXPECT_EQ(DecodeManagerLivenessRpcResponse(
                EncodeManagerLivenessRpcResponse(observation)),
            observation);
}

TEST(ManagerLivenessRpcInternal,
     HeterogeneousRequiredSetUsesOnlyNamedReceiverPolicy) {
  auto request = Request();
  auto &other_receiver = request.required_set.required_receivers.back();
  other_receiver.runtime_policy_id = RuntimePolicyId("policy.program-runner");
  other_receiver.runtime_policy_revision = RuntimePolicyRevision("1");
  request.required_set.required_runtime_set_digest =
      ComputeRequiredRuntimeSetDigest(request.required_set.required_receivers);
  request.reconciliation->required_runtime_set_digest =
      request.required_set.required_runtime_set_digest;

  const auto decoded =
      DecodeManagerLivenessRpcRequest(EncodeManagerLivenessRpcRequest(request));
  EXPECT_EQ(EncodeManagerLivenessRpcRequest(decoded),
            EncodeManagerLivenessRpcRequest(request));

  request.policy.runtime_policy_id = other_receiver.runtime_policy_id;
  request.policy.runtime_policy_revision =
      other_receiver.runtime_policy_revision;
  request.heartbeat.runtime_policy_id = request.policy.runtime_policy_id;
  request.heartbeat.runtime_policy_revision =
      request.policy.runtime_policy_revision;
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);
}

TEST(ManagerLivenessRpcInternal,
     RejectsMissingDuplicateForeignDigestAndNonCanonicalSequence) {
  auto request = Request();
  auto malformed = EncodeManagerLivenessRpcRequest(request);
  malformed.erase(malformed.find_last_of(' '));
  EXPECT_THROW(DecodeManagerLivenessRpcRequest(malformed),
               std::invalid_argument);

  request.required_set.required_receivers.push_back(
      request.required_set.required_receivers.front());
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);

  request = Request();
  request.required_set.required_runtime_set_digest =
      ContentDigest(std::string(kDigest2));
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);

  auto sequence = EncodeManagerLivenessRpcRequest(Request());
  const auto position = sequence.find(" 7 ");
  ASSERT_NE(position, std::string::npos);
  sequence.replace(position, 3U, " 07 ");
  EXPECT_THROW(DecodeManagerLivenessRpcRequest(sequence),
               std::invalid_argument);

  request = Request();
  request.receiver_runtime_unit_id = RuntimeUnitId("foreign.receiver");
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);
  request = Request();
  request.active_publication_generation = ActiveBundlePublicationGeneration(0U);
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);
  request = Request();
  request.active_public_pointer_digest = ContentDigest("foreign");
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);

  EXPECT_FALSE(IsManagerLivenessRpcPacket(
      "manyforge.manager-liveness.response.v1 foreign"));
  EXPECT_THROW(DecodeManagerLivenessRpcResponse(
                   "manyforge.manager-liveness.response.v1 foreign"),
               std::invalid_argument);
  try {
    static_cast<void>(DecodeManagerLivenessRpcResponse(
        EncodeManagerLivenessRpcRejection("superseded_active_publication")));
    FAIL() << "expected typed Manager-liveness rejection";
  } catch (const ManagerLivenessRpcRejection &rejection) {
    EXPECT_EQ(rejection.reason(), "superseded_active_publication");
  }
}

} // namespace
TEST(NativeShutdownRpc, ExactEpochClosedGrammarAndActualMilestonesRoundTrip) {
  NativeShutdownProgress actual{"host.epoch",
                                NativeShutdownPhase::kGroupRetirement,
                                0U,
                                4U,
                                2U,
                                4U,
                                1U,
                                2U,
                                1U,
                                1U,
                                42};
  EXPECT_EQ(DecodeNativeShutdownRpcRequest(
                EncodeNativeShutdownRpcRequest(actual.runtime_epoch)),
            actual.runtime_epoch);
  EXPECT_EQ(
      DecodeNativeShutdownRpcResponse(EncodeNativeShutdownRpcResponse(actual)),
      actual);
  EXPECT_THROW(DecodeNativeShutdownRpcRequest(
                   "manyforge.native-shutdown.request.v1 foreign extra"),
               std::invalid_argument);
  EXPECT_THROW(
      DecodeNativeShutdownRpcResponse(
          "manyforge.native-shutdown.response.v1 epoch 99 0 0 0 0 0 0 0 0 -1"),
      std::invalid_argument);
  actual.drained_groups = 3U;
  EXPECT_THROW(EncodeNativeShutdownRpcResponse(actual), std::invalid_argument);
}

TEST(NativeShutdownRpc,
     PollingIsNotProgressAndAbsentOwnersHaveNoDestructorMilestone) {
  NativeShutdownProjection projection("host.epoch");
  projection.Enter(NativeShutdownPhase::kExecutionDrain);
  const auto waiting = projection.Read();
  EXPECT_EQ(projection.Read(), waiting);
  std::unique_ptr<int> absent;
  {
    NativeShutdownOwnerPhase guard{
        projection, NativeShutdownPhase::kKernelDestruction, absent};
  }
  EXPECT_EQ(projection.Read(), waiting);
  projection.WorkerJoined();
  EXPECT_EQ(projection.Read().completed_workers, 1U);
  NativeShutdownPhase seen_by_destructor = NativeShutdownPhase::kRunning;
  struct ActualOwner {
    NativeShutdownProjection &projection;
    NativeShutdownPhase &seen;
    ~ActualOwner() { seen = projection.Read().phase; }
  };
  {
    ActualOwner actual{projection, seen_by_destructor};
    NativeShutdownOwnerPhase guard{
        projection, NativeShutdownPhase::kKernelDestruction, actual};
  }
  EXPECT_EQ(seen_by_destructor, NativeShutdownPhase::kKernelDestruction);
}

} // namespace manyforge::component_manager::detail
