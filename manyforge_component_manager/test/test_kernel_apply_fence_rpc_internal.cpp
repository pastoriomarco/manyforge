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
TEST(
    KernelApplyFenceRpcInternal,
    SelectedOwnerClaimStatusReleaseAndReconciliationHaveClosedDistinctRecords) {
  const core::ConfigurationFenceScopeDTO scope{
      {core::ControllableComponentId("robot.selected")},
      {{core::ControlDomainId("domain.arm"),
        core::ControlDomainRevision("arm.v1"),
        core::ControlDomainRevision("arm.v1")},
       {core::ControlDomainId("domain.gripper"),
        core::ControlDomainRevision("gripper.v1"),
        core::ControlDomainRevision("gripper.v1")}},
      {{core::ControlBindingId("binding.arm"),
        core::ControlBindingRevision("arm.v1"),
        core::ControlBindingRevision("arm.v1")},
       {core::ControlBindingId("binding.gripper"),
        core::ControlBindingRevision("gripper.v1"),
        core::ControlBindingRevision("gripper.v1")}},
      {core::ResourceId("resource.arm"), core::ResourceId("resource.gripper")}};
  const core::PublicRequestContextDTO request{
      core::RequestId("selected.request"),
      core::RequestOwnerEpoch("cclc.owner"), "selected-test", std::nullopt};
  const core::LifecycleTransactionId transaction("selected-lifecycle.1");
  const std::vector<core::DomainActiveControlConfigurationDTO> active{
      {core::ControlDomainId("domain.arm"),
       core::ActiveControlConfigurationId("active.arm"),
       core::ActiveControlConfigurationGeneration(1U)},
      {core::ControlDomainId("domain.gripper"),
       core::ActiveControlConfigurationId("active.gripper"),
       core::ActiveControlConfigurationGeneration(1U)}};
  core::CellAdmissionScopeDTO claim_scope{scope.resources, {}};
  for (const auto &row : active)
    claim_scope.configurations.push_back(
        {row.control_domain_id, row.active_control_configuration_id,
         row.active_control_configuration_generation});
  core::CellAdmissionClaimIntentDTO claim_intent{
      core::CellAdmissionClaimKindDTO::kInterventionOrRecovery,
      core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      request.request_owner_epoch,
      transaction.value(),
      request.request_id,
      core::ContentDigest(std::string(kDigestOne)),
      core::AdmissionGeneration(1U),
      claim_scope,
      std::nullopt,
      core::CellAdmissionInterventionEvidenceDTO{
          core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
          request.request_owner_epoch, transaction.value(),
          core::ContentDigest(std::string(kDigestOne)), std::nullopt}};
  const core::RuntimeEpoch manager_epoch("manager.selected");
  const core::ConfigurationFenceAdmissionSnapshotDTO admission{
      {scope, manager_epoch},
      ExecutionHostEpoch(),
      ActiveProjection(),
      core::ContentDigest(std::string(kDigestOne)),
      7U,
      core::ConfigurationFenceGeneration(7U),
      active,
      0U,
      0U,
      core::ContentDigest(std::string(kDigestTwo))};
  const core::SelectedBringUpConfigurationFenceIntentDTO intent{
      request,
      transaction,
      core::LifecycleTransactionGeneration(1U),
      {claim_intent, core::CellAdmissionClaimGeneration(2U),
       core::AdmissionGeneration(2U), 3},
      admission,
      admission.execution_host_epoch.publication_generation,
      scope,
      ActiveProjection()};
  ASSERT_TRUE(intent.valid());
  ExpectRequestRoundTripWithinPacket(intent);
  const core::SelectedBringUpFenceAcquireAttemptQueryDTO acquire_query{
      intent, core::ContentDigest(std::string(kDigestThree)), manager_epoch};
  ExpectRequestRoundTripWithinPacket(acquire_query);
  const core::ConfigurationFenceAcceptedDTO accepted{
      core::ConfigurationFenceId("fence.selected"),
      core::ConfigurationFenceGeneration(8U), scope, 9,
      core::RequestReplayStatusDTO::kAcceptedNew};
  ASSERT_TRUE(intent.matches(accepted));
  const core::SelectedBringUpConfigurationFenceStatusQueryDTO status_query{
      intent.owner(), accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope, manager_epoch};
  ExpectRequestRoundTripWithinPacket(status_query);
  const core::SelectedBringUpConfigurationFenceReconciliationEvidenceDTO
      evidence{admission.execution_host_epoch,
               intent.owner(),
               accepted.configuration_fence_id,
               accepted.configuration_fence_generation,
               scope,
               7U,
               ActiveProjection().graph_revision,
               ActiveProjection(),
               admission.execution_host_epoch.publication_generation,
               admission.active_public_pointer_digest,
               active,
               0U,
               0U,
               core::ContentDigest(std::string(kDigestTwo))};
  const core::SelectedBringUpConfigurationFenceReleaseDTO release{
      evidence, manager_epoch};
  ExpectRequestRoundTripWithinPacket(release);
  ExpectRequestRoundTripWithinPacket(
      core::SelectedBringUpFenceReleaseAttemptQueryDTO{release});
  const core::SelectedBringUpConfigurationFenceReleasedDTO released{
      release, admission.execution_host_epoch, 10,
      core::RequestReplayStatusDTO::kAcceptedNew,
      core::ContentDigest(std::string(kDigestThree))};
  const auto decoded_released = DecodeKernelApplyFenceRpcResponse(
      EncodeKernelApplyFenceRpcResponse(released));
  EXPECT_EQ(std::get<core::SelectedBringUpConfigurationFenceReleasedDTO>(
                decoded_released),
            released);
  const core::SelectedBringUpConfigurationFenceStatusDTO status{
      status_query,
      evidence,
      core::ConfigurationFenceState::kReleased,
      false,
      released,
      {},
      core::ContentDigest(std::string(kDigestOne)),
      std::nullopt};
  const auto decoded_status = DecodeKernelApplyFenceRpcResponse(
      EncodeKernelApplyFenceRpcResponse(status));
  const auto &round_trip_status =
      std::get<core::SelectedBringUpConfigurationFenceStatusDTO>(
          decoded_status);
  EXPECT_TRUE(round_trip_status.matches(status_query));
  EXPECT_EQ(round_trip_status.reconciliation_evidence, evidence);
  EXPECT_EQ(round_trip_status.release_receipt, released);
  const core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO
      acquire_result{
          admission.execution_host_epoch,
          acquire_query,
          core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact,
          accepted,
          true,
          {},
          core::ContentDigest(std::string(kDigestOne)),
          std::nullopt};
  const auto decoded_acquire = DecodeKernelApplyFenceRpcResponse(
      EncodeKernelApplyFenceRpcResponse(acquire_result));
  EXPECT_TRUE(
      std::get<core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO>(
          decoded_acquire)
          .matches(acquire_query));
  const core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO
      release_result{
          {release},
          core::SelectedBringUpFenceReleaseAttemptState::kReleasedExact,
          released,
          status,
          std::nullopt};
  const auto decoded_release = DecodeKernelApplyFenceRpcResponse(
      EncodeKernelApplyFenceRpcResponse(release_result));
  EXPECT_TRUE(
      std::get<core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO>(
          decoded_release)
          .presence_valid());

  auto foreign_pairing = release_result;
  auto &foreign_status = *foreign_pairing.observed_status;
  const core::ConfigurationFenceId foreign_fence("fence.foreign.selected");
  foreign_status.echoed_query.configuration_fence_id = foreign_fence;
  foreign_status.reconciliation_evidence.configuration_fence_id = foreign_fence;
  foreign_status.release_receipt->request.reconciliation_evidence
      .configuration_fence_id = foreign_fence;
  ASSERT_TRUE(foreign_status.presence_valid());
  EXPECT_FALSE(foreign_pairing.presence_valid());
  EXPECT_THROW(EncodeKernelApplyFenceRpcResponse(foreign_pairing),
               std::invalid_argument);

  auto foreign_receipt = release_result;
  foreign_receipt.observed_status->release_receipt->released_at_ns += 1;
  ASSERT_TRUE(foreign_receipt.observed_status->presence_valid());
  EXPECT_FALSE(foreign_receipt.presence_valid());
  EXPECT_THROW(EncodeKernelApplyFenceRpcResponse(foreign_receipt),
               std::invalid_argument);

  auto foreign_epoch = release_result;
  foreign_epoch.observed_status->echoed_query.manager_runtime_epoch =
      core::RuntimeEpoch("manager.foreign");
  ASSERT_TRUE(foreign_epoch.observed_status->presence_valid());
  EXPECT_FALSE(foreign_epoch.presence_valid());
  EXPECT_THROW(EncodeKernelApplyFenceRpcResponse(foreign_epoch),
               std::invalid_argument);

  auto held_status = status;
  held_status.state = core::ConfigurationFenceState::kHeld;
  held_status.admission_fenced = true;
  held_status.release_receipt.reset();
  const core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO held_pairing{
      {release},
      core::SelectedBringUpFenceReleaseAttemptState::kNotReleasedHeld,
      std::nullopt,
      held_status,
      std::nullopt};
  ASSERT_TRUE(held_pairing.presence_valid());
  auto held_foreign_epoch = held_pairing;
  held_foreign_epoch.observed_status->echoed_query.manager_runtime_epoch =
      core::RuntimeEpoch("manager.foreign");
  ASSERT_TRUE(held_foreign_epoch.observed_status->presence_valid());
  EXPECT_FALSE(held_foreign_epoch.presence_valid());
  EXPECT_THROW(EncodeKernelApplyFenceRpcResponse(held_foreign_epoch),
               std::invalid_argument);

  auto foreign = intent;
  foreign.selected_claim.intent.owner_transaction_id = "route-selection.1";
  EXPECT_FALSE(foreign.valid());
  EXPECT_THROW(EncodeKernelApplyFenceRpcRequest(foreign),
               std::invalid_argument);
  auto partial = intent;
  partial.selected_claim.intent.scope.resources.pop_back();
  EXPECT_FALSE(partial.valid());
  EXPECT_THROW(EncodeKernelApplyFenceRpcRequest(partial),
               std::invalid_argument);
  // The closed new ownership is never interpreted as Apply; its old tags stay
  // fixed.
  const auto encoded = EncodeKernelApplyFenceRpcRequest(intent);
  EXPECT_EQ(static_cast<unsigned char>(encoded.at(8)), 7U);
  const auto old_encoded =
      EncodeKernelApplyFenceRpcRequest(MaximumIntent(scope));
  EXPECT_EQ(static_cast<unsigned char>(old_encoded.at(8)), 2U);
  auto malformed = encoded;
  malformed.push_back('x');
  EXPECT_THROW(DecodeKernelApplyFenceRpcRequest(malformed),
               std::invalid_argument);
}

} // namespace manyforge::component_manager::detail
