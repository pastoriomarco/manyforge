#include <gtest/gtest.h>

#include "manager_liveness_rpc_internal.hpp"
#include <array>

namespace manyforge::component_manager::detail {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigest2 =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
// A Scene id is an exact sha256 digest (SceneRevision::valid).
constexpr std::string_view kSceneId =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr std::string_view kEmptySceneId =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";

ActiveBundlePublicationReceiptDTO ApplyPublication() {
  const ProjectionRevisionDTO projection{
      RetainedBundleId("bundle.1"), GraphRevision("graph.1"),
      {SceneId(std::string(kSceneId)), 2U, ContentDigest(std::string(kDigest2))}};
  return {{NamedApplyPublicationTransactionDTO{ApplyId(5U), ApplyGeneration(1U)},
           projection, projection.retained_bundle_id, projection.graph_revision,
           ActiveBundlePublicationGeneration(3U), ContentDigest(std::string(kDigest2))},
          ActiveBundlePublicationGeneration(4U),
          ContentDigest(std::string(kDigest))};
}

ActiveBundlePublicationReceiptDTO BootstrapPublication() {
  const ProjectionRevisionDTO projection{
      RetainedBundleId("bundle.empty"), GraphRevision("graph.empty"),
      {SceneId(std::string(kEmptySceneId)), 1U, ContentDigest(std::string(kDigest2))}};
  return {{BootstrapPublicationTransactionDTO{BootstrapId("bootstrap.1"),
                                              BootstrapGeneration(1U)},
           projection, projection.retained_bundle_id, projection.graph_revision,
           std::nullopt, ContentDigest(std::string(kDigest2))},
          ActiveBundlePublicationGeneration(1U),
          ContentDigest(std::string(kDigest))};
}

ManagerLivenessTransportRequestDTO Request() {
  const ManagerLivenessPolicyDTO policy{
      RuntimePolicyId("policy.native"),
      RuntimePolicyRevision(std::string(kDigest)),
      250};
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
                                    std::move(receivers)};
  return {set.required_receivers.front().runtime_unit_id,
          ApplyPublication(),
          set,
          policy,
          ManagerHeartbeatDTO{
              set.cell_id, set.manager_runtime_unit_id, 7U,
              policy.runtime_policy_id, policy.runtime_policy_revision},
          ManagerLivenessReconciliationAck{
              set.cell_id, set.manager_runtime_unit_id,
              set.active_bundle_id, set.required_runtime_set_digest,
              ContentDigest(std::string(kDigest2))}};
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

ManagerLivenessReceiverConfig ReceiverConfig() {
  const auto request = Request();
  const auto &receiver = request.required_set.required_receivers.front();
  return {request.required_set.cell_id,
          receiver.runtime_unit_id,
          receiver.runtime_epoch,
          request.required_set.manager_runtime_unit_id,
          request.policy,
          request.required_set.active_bundle_id,
          request.required_set.required_runtime_set_digest};
}

TEST(ManagerLivenessCommandAdmission, IsInitiallyClosedAndOpens) {
  ManagerLivenessCommandAdmission admission;
  EXPECT_FALSE(admission.open());
  EXPECT_FALSE(admission.Allows(ManagerLivenessCommandClass::kEffectful));
  EXPECT_TRUE(admission.Allows(ManagerLivenessCommandClass::kStop));
  EXPECT_TRUE(admission.Allows(ManagerLivenessCommandClass::kReadOnly));
  admission.Open();
  ASSERT_TRUE(admission.open());
  EXPECT_TRUE(admission.Allows(ManagerLivenessCommandClass::kEffectful));
}

TEST(ManagerLivenessCommandAdmission, RequiredSetRebindCloses) {
  ManagerLivenessCommandAdmission admission;
  admission.Open();
  ASSERT_TRUE(admission.open());

  admission.CloseForRequiredSetRebind();
  EXPECT_FALSE(admission.open());
}

TEST(ManagerLivenessCommandAdmission,
     ExactReportOpensAndAQuietManagerNeverClosesOrContains) {
  LivenessObservations observations;
  ManagerLivenessCommandAdmission admission;
  ChildOwnedManagerLivenessReceiver child_lifetime;
  child_lifetime.Install(ReceiverConfig(), observations);
  auto *const receiver = child_lifetime.get();
  ASSERT_NE(receiver, nullptr);

  const auto request = Request();
  ASSERT_TRUE(receiver->Renew(request.heartbeat).ok());
  ASSERT_TRUE(receiver->CompleteReconciliation(*request.reconciliation).ok());
  admission.Open();
  ASSERT_TRUE(admission.Allows(ManagerLivenessCommandClass::kEffectful));
  auto next = request.heartbeat;
  ++next.heartbeat_sequence;
  ASSERT_TRUE(receiver->Renew(next).ok());
  EXPECT_TRUE(admission.Allows(ManagerLivenessCommandClass::kEffectful));
}

TEST(ManagerLivenessBatchPolicy,
     NativeSubsetCarriesFullCrossTransportSetAndOneBatchDeadline) {
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
                                  request.policy.runtime_policy_id,
                                  request.policy.runtime_policy_revision,
                                  request.heartbeat.heartbeat_sequence,
                                  3U,
                                  ManagerLivenessStateDTO::kCurrent,
                                  false,
                                  std::nullopt};
  ManagerLivenessObservationDTO observation{
      status, request.required_set.active_bundle_id,
      request.required_set.required_runtime_set_digest};
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
  request.active_publication.publication_generation = ActiveBundlePublicationGeneration(0U);
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);
  request = Request();
  request.active_publication.durable_public_pointer_content_digest = ContentDigest("foreign");
  EXPECT_THROW(EncodeManagerLivenessRpcRequest(request), std::invalid_argument);

  EXPECT_FALSE(IsManagerLivenessRpcPacket(
      "manyforge.manager-liveness.response.v1 foreign"));
  // Requests of the earlier formats (v1 with the removed expiry field, v2
  // with the removed Manager owner epoch) are refused.
  for (const char *earlier_tag : {"manyforge.manager-liveness.request.v1",
                                  "manyforge.manager-liveness.request.v2"}) {
    auto earlier = EncodeManagerLivenessRpcRequest(Request());
    earlier.replace(0U, kManagerLivenessRequestTag.size(), earlier_tag);
    EXPECT_FALSE(IsManagerLivenessRpcPacket(earlier));
    EXPECT_THROW(DecodeManagerLivenessRpcRequest(earlier),
                 std::invalid_argument);
  }
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

// The request carries the Manager's whole publication receipt, for a named
// Apply and for a bootstrap.
TEST(ManagerLivenessRpcInternal, RoundTripsNamedApplyAndBootstrapPublication) {
  for (const auto &publication : {ApplyPublication(), BootstrapPublication()}) {
    ASSERT_TRUE(publication.valid());
    auto request = Request();
    request.active_publication = publication;
    const auto bytes = EncodeManagerLivenessRpcRequest(request);
    const auto decoded = DecodeManagerLivenessRpcRequest(bytes);
    EXPECT_EQ(decoded, request);
    EXPECT_EQ(EncodeManagerLivenessRpcRequest(decoded), bytes);
  }
}

// The earlier layout with three publication tokens (generation, graph,
// pointer digest) is refused by its field count.
TEST(ManagerLivenessRpcInternal, RefusesTheEarlierThreeTokenPublicationLayout) {
  const auto bytes = EncodeManagerLivenessRpcRequest(Request());
  std::vector<std::string> tokens(1U);
  for (const char c : bytes) {
    if (c == ' ')
      tokens.emplace_back();
    else
      tokens.back().push_back(c);
  }
  ASSERT_EQ(tokens.size(), 24U + 4U * Request().required_set.required_receivers.size());
  std::string earlier = tokens[0] + ' ' + tokens[1] + ' ' + tokens[7] + ' ' + tokens[2];
  for (std::size_t index = 13U; index < tokens.size(); ++index)
    earlier += ' ' + tokens[index];
  EXPECT_TRUE(IsManagerLivenessRpcPacket(earlier));
  EXPECT_THROW(DecodeManagerLivenessRpcRequest(earlier), std::invalid_argument);
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
          "manyforge.native-shutdown.response.v3 epoch 99 0 0 0 0 0 0 0 0 -1"),
      std::invalid_argument);
  // The previous version is refused: no compatibility reading.
  EXPECT_THROW(
      DecodeNativeShutdownRpcResponse(
          "manyforge.native-shutdown.response.v2 epoch 1 0 0 0 0 0 0 0 0 -1"),
      std::invalid_argument);
  // OD-187 (3): the planned Halt is a phase of its own.
  NativeShutdownProgress halt{"host.epoch", NativeShutdownPhase::kPlannedHalt};
  EXPECT_EQ(DecodeNativeShutdownRpcResponse(EncodeNativeShutdownRpcResponse(halt)),
            halt);
  EXPECT_EQ(EncodeNativeShutdownRpcResponse(halt).rfind(
                "manyforge.native-shutdown.response.v3 host.epoch 14 ", 0),
            0U);
  EXPECT_EQ(NativeShutdownPhaseName(NativeShutdownPhase::kPlannedHalt),
            "planned-halt");
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
