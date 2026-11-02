#include <gtest/gtest.h>

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_deployment/apply_fence_qualification.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kernel_apply_fence_rpc_internal.hpp"

namespace manyforge::component_manager::detail {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kDigestThree =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";

static_assert(
    kMaximumManagerApplyFenceComponents ==
    deployment::kMaximumQualifiedApplyFenceControllableComponentsPerTransition);
static_assert(
    kMaximumManagerApplyFenceDomains ==
    deployment::kMaximumQualifiedApplyFenceControlDomainsPerTransition);
static_assert(
    kMaximumManagerApplyFenceBindings ==
    deployment::kMaximumQualifiedApplyFenceControlBindingsPerTransition);
static_assert(kMaximumManagerApplyFenceResources ==
              deployment::kMaximumQualifiedApplyFenceResourcesPerTransition);

[[nodiscard]] std::string Identifier(const char prefix, const std::size_t index,
                                     const std::size_t bytes) {
  auto ordinal = std::to_string(index);
  std::string value(1U, prefix);
  value.append(6U - ordinal.size(), '0');
  value.append(ordinal);
  value.append(bytes - value.size(), 'x');
  return value;
}

[[nodiscard]] std::string Text(const char value, const std::size_t bytes) {
  return std::string(bytes, value);
}

[[nodiscard]] core::ConfigurationFenceScopeDTO MaximumTransitionScope() {
  core::ConfigurationFenceScopeDTO scope;
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceComponents;
       ++index) {
    scope.controllable_component_ids.emplace_back(Identifier('c', index, 128U));
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceDomains;
       ++index) {
    scope.domains.push_back({
        core::ControlDomainId(Identifier('d', index, 128U)),
        index % 2U == 0U
            ? std::optional<core::ControlDomainRevision>(
                  core::ControlDomainRevision(std::string(kDigestOne)))
            : std::nullopt,
        index % 2U == 1U
            ? std::optional<core::ControlDomainRevision>(
                  core::ControlDomainRevision(std::string(kDigestTwo)))
            : std::nullopt,
    });
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceBindings;
       ++index) {
    scope.bindings.push_back({
        core::ControlBindingId(Identifier('b', index, 128U)),
        index % 2U == 0U
            ? std::optional<core::ControlBindingRevision>(
                  core::ControlBindingRevision(std::string(kDigestOne)))
            : std::nullopt,
        index % 2U == 1U
            ? std::optional<core::ControlBindingRevision>(
                  core::ControlBindingRevision(std::string(kDigestTwo)))
            : std::nullopt,
    });
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceResources;
       ++index) {
    scope.resources.emplace_back(Identifier('r', index, 128U));
  }
  EXPECT_TRUE(scope.valid());
  return scope;
}

[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
ActiveConfigurations(const core::ConfigurationFenceScopeDTO &scope,
                     const bool candidate) {
  std::vector<core::DomainActiveControlConfigurationDTO> result;
  for (std::size_t index = 0U; index < scope.domains.size(); ++index) {
    const auto &domain = scope.domains[index];
    const bool required =
        candidate ? domain.candidate_control_domain_revision.has_value()
                  : domain.predecessor_control_domain_revision.has_value();
    if (required) {
      result.push_back(
          {domain.control_domain_id,
           core::ActiveControlConfigurationId(Identifier('a', index, 256U)),
           core::ActiveControlConfigurationGeneration(1U)});
    }
  }
  return result;
}

[[nodiscard]] core::ApplyConfigurationFenceIntentDTO
MaximumIntent(const core::ConfigurationFenceScopeDTO &scope) {
  return {
      {core::RequestId(Identifier('q', 1U, 256U)),
       core::RequestOwnerEpoch(Identifier('o', 1U, 256U)),
       Text('s', kMaximumManagerApplySourceLabelBytes),
       Text('k', kMaximumManagerApplyCorrelationIdBytes)},
      {core::ApplyId(Identifier('p', 1U, 256U)), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      core::RetainedBundleId(std::string(kDigestOne)),
      core::GraphRevision(std::string(kDigestOne)),
      6U,
      core::ConfigurationFenceGeneration(7U),
      ActiveConfigurations(scope, false),
      Text('f', kMaximumManagerApplyFenceReasonBytes),
  };
}

[[nodiscard]] core::ExecutionHostEpochDTO ExecutionHostEpoch() {
  return {core::RuntimeUnitId(Identifier('u', 1U, 128U)),
          core::RuntimeEpoch(Identifier('e', 1U, 256U)),
          core::RequestOwnerEpoch(Identifier('w', 1U, 256U)),
          core::ContentDigest(std::string(kDigestThree)),
          core::ActiveBundlePublicationGeneration(9U)};
}

[[nodiscard]] core::ProjectionRevisionDTO ActiveProjection() {
  return {core::RetainedBundleId(std::string(kDigestTwo)),
          core::GraphRevision(std::string(kDigestTwo)),
          {core::SceneId(std::string(kDigestTwo)), 1U,
           core::ContentDigest(std::string(kDigestThree))}};
}

[[nodiscard]] core::ConfigurationFenceReconciliationEvidenceDTO
Evidence(const core::ConfigurationFenceScopeDTO &scope,
         const core::ApplyFenceOwnerDTO &owner) {
  return {ExecutionHostEpoch(),
          owner,
          core::ConfigurationFenceId(Identifier('f', 1U, 256U)),
          core::ConfigurationFenceGeneration(8U),
          scope,
          7U,
          core::GraphRevision(std::string(kDigestTwo)),
          ActiveProjection(),
          core::ActiveBundlePublicationGeneration(9U),
          core::ContentDigest(std::string(kDigestOne)),
          ActiveConfigurations(scope, true),
          0U,
          0U,
          core::ContentDigest(std::string(kDigestTwo))};
}

template <typename Value>
void ExpectRequestRoundTripWithinPacket(const Value &value) {
  const auto encoded =
      EncodeKernelApplyFenceRpcRequest(KernelApplyFenceRpcRequest(value));
  EXPECT_LE(encoded.size(), kMaximumKernelApplyFenceRpcPacketBytes);
  const auto decoded = DecodeKernelApplyFenceRpcRequest(encoded);
  const auto *round_trip = std::get_if<Value>(&decoded);
  ASSERT_NE(round_trip, nullptr);
  EXPECT_EQ(*round_trip, value);
}

TEST(KernelApplyFenceRpcInternal,
     LargestCompilerQualifiedRequestAndReleaseFitAndRoundTrip) {
  const auto scope = MaximumTransitionScope();
  const auto intent = MaximumIntent(scope);
  ASSERT_TRUE(intent.valid());
  const core::ConfigurationFenceAdmissionQueryDTO admission_query{
      scope, core::RuntimeEpoch(Identifier('m', 1U, 256U))};
  const core::ApplyFenceAcquireAttemptQueryDTO attempt_query{
      intent, core::ContentDigest(std::string(kDigestThree)),
      admission_query.manager_runtime_epoch};
  const auto evidence = Evidence(scope, intent.owner_transaction);
  ASSERT_TRUE(evidence.valid());
  const core::ConfigurationFenceReleaseDTO release{
      evidence, admission_query.manager_runtime_epoch};
  ASSERT_TRUE(release.valid());

  ExpectRequestRoundTripWithinPacket(admission_query);
  ExpectRequestRoundTripWithinPacket(intent);
  ExpectRequestRoundTripWithinPacket(attempt_query);
  ExpectRequestRoundTripWithinPacket(core::ConfigurationFenceStatusQueryDTO{
      intent.owner_transaction, evidence.configuration_fence_id,
      evidence.configuration_fence_generation, scope,
      admission_query.manager_runtime_epoch});
  ExpectRequestRoundTripWithinPacket(release);
}

TEST(KernelApplyFenceRpcInternal,
     LargestCompilerQualifiedStatusAndAttemptFitWithLosslessSharedScope) {
  const auto scope = MaximumTransitionScope();
  const auto intent = MaximumIntent(scope);
  const auto evidence = Evidence(scope, intent.owner_transaction);
  const core::ConfigurationFenceStatusQueryDTO status_query{
      intent.owner_transaction, evidence.configuration_fence_id,
      evidence.configuration_fence_generation, scope,
      core::RuntimeEpoch(Identifier('m', 1U, 256U))};
  const core::ConfigurationFenceReleaseDTO release{
      evidence, status_query.manager_runtime_epoch};
  const core::ConfigurationFenceReleasedDTO released{
      release, evidence.execution_host_epoch, 10U,
      core::RequestReplayStatusDTO::kAcceptedNew,
      core::ContentDigest(std::string(kDigestThree))};
  ASSERT_TRUE(released.valid());
  const core::ConfigurationFenceStatusDTO status{
      status_query,
      evidence,
      core::ConfigurationFenceState::kReleased,
      false,
      released,
      {},
      core::ContentDigest(std::string(kDigestOne)),
      std::nullopt};
  ASSERT_TRUE(status.presence_valid());

  const auto encoded_status =
      EncodeKernelApplyFenceRpcResponse(KernelApplyFenceRpcResponse(status));
  EXPECT_LE(encoded_status.size(), kMaximumKernelApplyFenceRpcPacketBytes);
  const auto decoded_status = DecodeKernelApplyFenceRpcResponse(encoded_status);
  const auto *status_round_trip =
      std::get_if<core::ConfigurationFenceStatusDTO>(&decoded_status);
  ASSERT_NE(status_round_trip, nullptr);
  EXPECT_TRUE(status_round_trip->matches(status_query));
  EXPECT_EQ(status_round_trip->reconciliation_evidence, evidence);
  ASSERT_TRUE(status_round_trip->release_receipt.has_value());
  EXPECT_EQ(*status_round_trip->release_receipt, released);

  const core::ConfigurationFenceAcceptedDTO accepted{
      evidence.configuration_fence_id, evidence.configuration_fence_generation,
      scope, 9U, core::RequestReplayStatusDTO::kAcceptedNew};
  ASSERT_TRUE(accepted.matches(intent));
  const core::ApplyFenceAcquireAttemptReconcileResultDTO attempt_result{
      evidence.execution_host_epoch,
      {intent, core::ContentDigest(std::string(kDigestTwo)),
       status_query.manager_runtime_epoch},
      core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact,
      accepted,
      true,
      {},
      core::ContentDigest(std::string(kDigestThree)),
      std::nullopt};
  ASSERT_TRUE(attempt_result.presence_valid());
  const auto encoded_attempt = EncodeKernelApplyFenceRpcResponse(
      KernelApplyFenceRpcResponse(attempt_result));
  EXPECT_LE(encoded_attempt.size(), kMaximumKernelApplyFenceRpcPacketBytes);
  const auto decoded_attempt =
      DecodeKernelApplyFenceRpcResponse(encoded_attempt);
  const auto *attempt_round_trip =
      std::get_if<core::ApplyFenceAcquireAttemptReconcileResultDTO>(
          &decoded_attempt);
  ASSERT_NE(attempt_round_trip, nullptr);
  EXPECT_TRUE(attempt_round_trip->matches(attempt_result.echoed_query));
  EXPECT_EQ(attempt_round_trip->accepted_fence, accepted);
}

} // namespace
TEST(KernelApplyFenceRpcInternal,
     ProviderScopesUseExplicitEnvelopeAndRetainExactProof) {
  const core::ProviderConfigurationFenceOccurrenceDTO occurrence{
      core::ControllableComponentId("io"),
      core::ProviderInstanceId("provider.io"),
      core::ContentDigest(std::string(kDigestOne)),
      {core::ResourceId("io.output")}};
  const core::ConfigurationFenceScopeDTO scope{
      {occurrence.controllable_component_id},
      {{core::ControlDomainId("io.domain"), core::ControlDomainRevision("old"),
        core::ControlDomainRevision("new"),
        core::ProviderConfigurationFenceDomainDTO{occurrence, occurrence}}},
      {},
      occurrence.output_resources};
  ASSERT_TRUE(scope.valid());
  const core::ConfigurationFenceAdmissionQueryDTO query{
      scope, core::RuntimeEpoch("manager")};
  const auto request = EncodeKernelApplyFenceRpcRequest(query);
  EXPECT_EQ(request.substr(0U, 8U), "MFNKAFR5");
  EXPECT_TRUE(IsKernelApplyFenceRpcPacket(request));
  EXPECT_EQ(std::get<core::ConfigurationFenceAdmissionQueryDTO>(
                DecodeKernelApplyFenceRpcRequest(request)),
            query);
  auto alias = request;
  alias[7] = '2';
  EXPECT_THROW((void)DecodeKernelApplyFenceRpcRequest(alias),
               std::invalid_argument);
  const core::ConfigurationFenceAcceptedDTO accepted{
      core::ConfigurationFenceId("fence.io"),
      core::ConfigurationFenceGeneration(1U), scope, 1,
      core::RequestReplayStatusDTO::kAcceptedNew};
  const auto reply = EncodeKernelApplyFenceRpcResponse(accepted);
  EXPECT_EQ(reply.substr(0U, 8U), "MFNKAFR5");
  EXPECT_EQ(std::get<core::ConfigurationFenceAcceptedDTO>(
                DecodeKernelApplyFenceRpcResponse(reply)),
            accepted);
  EXPECT_EQ(EncodeKernelApplyFenceRpcResponse(
                DecodeKernelApplyFenceRpcResponse(reply)),
            reply);
  auto physical = MaximumIntent(MaximumTransitionScope());
  const auto legacy = EncodeKernelApplyFenceRpcRequest(physical);
  EXPECT_EQ(legacy.substr(0U, 8U), "MFNKAFR2");
  alias = legacy;
  alias[7] = '3';
  EXPECT_THROW((void)DecodeKernelApplyFenceRpcRequest(alias),
               std::invalid_argument);
}

TEST(KernelApplyFenceRpcInternal,
     ProviderAdmissionRoundTripsExplicitV5WithoutLineage) {
  const core::ProviderConfigurationFenceOccurrenceDTO occurrence{
      core::ControllableComponentId("io"),
      core::ProviderInstanceId("provider.io"),
      core::ContentDigest(std::string(kDigestOne)),
      {}};
  const core::ConfigurationFenceScopeDTO scope{
      {occurrence.controllable_component_id},
      {{core::ControlDomainId("io.domain"), core::ControlDomainRevision("old"),
        core::ControlDomainRevision("new"),
        core::ProviderConfigurationFenceDomainDTO{occurrence, occurrence}}},
      {},
      {}};
  core::ConfigurationFenceAdmissionSnapshotDTO snapshot{
      {scope, core::RuntimeEpoch("manager.io")},
      ExecutionHostEpoch(),
      ActiveProjection(),
      core::ContentDigest(std::string(kDigestOne)),
      1U,
      core::ConfigurationFenceGeneration(2U),
      {},
      0U,
      0U,
      core::ContentDigest(std::string(kDigestTwo))};
  ASSERT_TRUE(snapshot.presence_valid());
  const auto bytes = EncodeKernelApplyFenceRpcResponse(snapshot);
  EXPECT_EQ(bytes.substr(0U, 8U), "MFNKAFR5");
  EXPECT_EQ(std::get<core::ConfigurationFenceAdmissionSnapshotDTO>(
                DecodeKernelApplyFenceRpcResponse(bytes)),
            snapshot);
  auto intent = MaximumIntent(scope);
  intent.expected_active_control_configurations.clear();
  ASSERT_TRUE(intent.valid());
  const auto intent_bytes = EncodeKernelApplyFenceRpcRequest(intent);
  EXPECT_EQ(intent_bytes.substr(0U, 8U), "MFNKAFR5");
  EXPECT_EQ(std::get<core::ApplyConfigurationFenceIntentDTO>(
                DecodeKernelApplyFenceRpcRequest(intent_bytes)),
            intent);
  // The lineage-carrying v4 packet is no longer accepted.
  auto old = bytes;
  old[7] = '4';
  EXPECT_THROW((void)DecodeKernelApplyFenceRpcResponse(old),
               std::invalid_argument);
  old = intent_bytes;
  old[7] = '4';
  EXPECT_THROW((void)DecodeKernelApplyFenceRpcRequest(old),
               std::invalid_argument);
}

TEST(KernelApplyFenceRpcInternal,
     ScopeRecordRoundTripsProviderAndControllerScopesExactly) {
  const core::ProviderConfigurationFenceOccurrenceDTO before{
      core::ControllableComponentId("io"),
      core::ProviderInstanceId("provider.io"),
      core::ContentDigest(std::string(kDigestOne)),
      {core::ResourceId("io.output")}};
  auto after = before;
  after.signal_configuration_digest = core::ContentDigest(std::string(kDigestTwo));
  const core::ConfigurationFenceScopeDTO provider{
      {before.controllable_component_id},
      {{core::ControlDomainId("io.domain"), core::ControlDomainRevision("old"),
        core::ControlDomainRevision("new"),
        core::ProviderConfigurationFenceDomainDTO{before, after}}},
      {},
      before.output_resources};
  ASSERT_TRUE(provider.valid());
  const auto provider_record = EncodeConfigurationFenceScopeRecord(provider);
  EXPECT_EQ(DecodeConfigurationFenceScopeRecord(provider_record), provider);
  const auto controller = MaximumTransitionScope();
  const auto controller_record = EncodeConfigurationFenceScopeRecord(controller);
  EXPECT_EQ(DecodeConfigurationFenceScopeRecord(controller_record), controller);
  EXPECT_THROW((void)DecodeConfigurationFenceScopeRecord(provider_record + "x"),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeConfigurationFenceScopeRecord(
                   provider_record.substr(0U, provider_record.size() - 1U)),
               std::invalid_argument);
  // A record is never a packet of the base version.
  auto alias = provider_record;
  alias[7] = '2';
  EXPECT_THROW((void)DecodeConfigurationFenceScopeRecord(alias),
               std::invalid_argument);
}

} // namespace manyforge::component_manager::detail
