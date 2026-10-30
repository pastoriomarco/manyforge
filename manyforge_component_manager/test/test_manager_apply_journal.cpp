

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "manyforge_component_manager/manager_apply_journal.hpp"
#include "manyforge_component_manager/manager_runtime_convergence_coordinator.hpp"
#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include "lite6_controllable_retention_fixture.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace manyforge::component_manager {

// Defined by the journal implementation; deliberately omitted from the public API.
core::Result<PersistedManagerApplyStatus> ReadManagerApplyStatusReadOnlyForTesting(
    const std::filesystem::path &, const core::CellId &,
    const core::NamedApplyPublicationTransactionDTO &, std::uint64_t *);

namespace {

using core::ActiveBundlePublicationGeneration;
using core::ApplyConfigurationFenceIntentDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::CellId;
using core::ConfigurationFenceAcceptedDTO;
using core::ConfigurationFenceGeneration;
using core::ConfigurationFenceId;
using core::ConfigurationFenceScopeDTO;
using core::ContentDigest;
using core::ControlBindingId;
using core::ControlBindingRevision;
using core::ControlDomainId;
using core::ControlDomainRevision;
using core::ControllableComponentId;
using core::EndpointRoleId;
using core::GraphRevision;
using core::ProductContractId;
using core::ProductContractRevision;
using core::ProductId;
using core::ProjectionHealthDTO;
using core::ProjectionHealthState;
using core::ProjectionNativeRuntimeBindingDTO;
using core::ProjectionRevisionDTO;
using core::PublicRequestContextDTO;
using core::RequestId;
using core::RequestOwnerEpoch;
using core::ResourceId;
using core::RetainedBundleId;
using core::RuntimeEpoch;
using core::RuntimeProfileId;
using core::RuntimeProfileRevision;
using core::RuntimeRoleId;
using core::RuntimeUnitId;
using core::SceneId;
using core::SceneRevision;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-apply-journal-test-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kDigestThree =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr std::string_view kSceneIdOne =
    "sha256:abababababababababababababababababababababababababababababababab";
constexpr std::string_view kSceneIdTwo =
    "sha256:cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";

ManagerApplyIntentDTO Intent(std::string apply_id = "apply-1",
                             const std::uint64_t generation = 1U,
                             std::string request_id = "request-1",
                             std::string owner_epoch = "owner-epoch-1",
                             std::string candidate = "candidate-bundle-1") {
  return {
      PublicRequestContextDTO{
          RequestId(std::move(request_id)),
          RequestOwnerEpoch(std::move(owner_epoch)),
          "component-manager-test",
          std::optional<std::string>("correlation-1"),
      },
      ApplyId(std::move(apply_id)),
      ApplyGeneration(generation),
      CellId("cell-1"),
      ProjectionRevisionDTO{
          RetainedBundleId("predecessor-bundle"),
          GraphRevision("predecessor-graph"),
          SceneRevision{
              SceneId(std::string(kSceneIdOne)),
              7U,
              ContentDigest(std::string(kDigestOne)),
          },
      },
      RetainedBundleId(std::move(candidate)),
      GraphRevision("candidate-graph"),
      ManagerApplyPreparedActivationDTO{
          ContentDigest("sha256:1414141414141414141414141414141414141414141414141414141414141414"),
          core::DeploymentArtifactId("deployment-artifact-1"),
          core::DeploymentRevision(
              "sha256:1515151515151515151515151515151515151515151515151515151515151515"),
          core::ProgramBindingId(std::string(kDigestTwo)),
          ContentDigest("sha256:1616161616161616161616161616161616161616161616161616161616161616"),
          R"({"schema_version":"test.prepared_activation_receipt.v1"})",
      },
      core::AdmissionGeneration(7U),
      std::nullopt,
  };
}

ManagerApplyIntentDTO ParentClaimIntent() {
  auto intent = Intent();
  intent.expected_admission_generation.reset();
  intent.parent_authoring_claim = core::CellAdmissionClaimDTO{
      {
          core::CellAdmissionClaimKindDTO::kAuthoringMutation,
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          RequestOwnerEpoch("composer-owner-1"),
          "composer-transaction-1",
          RequestId("composer-request-1"),
          ContentDigest(std::string(kDigestTwo)),
          core::AdmissionGeneration(7U),
          {},
          std::nullopt,
          std::nullopt,
      },
      core::CellAdmissionClaimGeneration(3U),
      core::AdmissionGeneration(7U),
      42,
  };
  return intent;
}

ApplyConfigurationFenceIntentDTO FenceIntent() {
  return {
      Intent().request_context,
      core::ApplyFenceOwnerDTO{ApplyId("apply-1"), ApplyGeneration(1U)},
      ConfigurationFenceScopeDTO{
          {ControllableComponentId("robot-1")},
          {{ControlDomainId("arm"), ControlDomainRevision("domain-r1"),
            ControlDomainRevision("domain-r2")}},
          {{ControlBindingId("arm-jtc"), ControlBindingRevision("binding-r1"),
            ControlBindingRevision("binding-r2")}},
          {ResourceId("joint-1"), ResourceId("joint-2")},
      },
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      RetainedBundleId("predecessor-bundle"),
      GraphRevision("predecessor-graph"),
      11U,
      ConfigurationFenceGeneration(4U),
      {{ControlDomainId("arm"),
        core::ActiveControlConfigurationId("trajectory-mode"),
        core::ActiveControlConfigurationGeneration(3U)}},
      "apply configuration transition",
  };
}

std::string FenceIdentifier(const char prefix, const std::size_t index) {
  auto ordinal = std::to_string(index);
  std::string value(1U, prefix);
  value.append(6U - ordinal.size(), '0');
  value.append(ordinal);
  value.append(128U - value.size(), 'x');
  return value;
}

ApplyConfigurationFenceIntentDTO MaximumFenceIntent() {
  auto intent = FenceIntent();
  intent.scope = {};
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceComponents;
       ++index) {
    intent.scope.controllable_component_ids.emplace_back(
        FenceIdentifier('c', index));
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceDomains;
       ++index) {
    intent.scope.domains.push_back({
        ControlDomainId(FenceIdentifier('d', index)),
        index % 2U == 0U ? std::optional<ControlDomainRevision>(
                               ControlDomainRevision(std::string(kDigestOne)))
                         : std::nullopt,
        index % 2U == 1U ? std::optional<ControlDomainRevision>(
                               ControlDomainRevision(std::string(kDigestTwo)))
                         : std::nullopt,
    });
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceBindings;
       ++index) {
    intent.scope.bindings.push_back({
        ControlBindingId(FenceIdentifier('b', index)),
        index % 2U == 0U ? std::optional<ControlBindingRevision>(
                               ControlBindingRevision(std::string(kDigestOne)))
                         : std::nullopt,
        index % 2U == 1U ? std::optional<ControlBindingRevision>(
                               ControlBindingRevision(std::string(kDigestTwo)))
                         : std::nullopt,
    });
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceResources;
       ++index) {
    intent.scope.resources.emplace_back(FenceIdentifier('r', index));
  }
  intent.expected_active_control_configurations.clear();
  for (std::size_t index = 0U; index < intent.scope.domains.size();
       index += 2U) {
    intent.expected_active_control_configurations.push_back(
        {intent.scope.domains[index].control_domain_id,
         core::ActiveControlConfigurationId(FenceIdentifier('a', index)),
         core::ActiveControlConfigurationGeneration(3U)});
  }
  return intent;
}

ManagerApplyAffectedScopeDTO AffectedScope(
    const ApplyConfigurationFenceIntentDTO &fence_intent = FenceIntent()) {
  return {
      fence_intent.owner_transaction.apply_id,
      fence_intent.owner_transaction.apply_generation,
      fence_intent.scope,
      fence_intent.busy_disposition,
      manyforge::deployment::ApplyComponentDeltaDTO{},
  };
}

// A mixed decision inside FenceIntent()'s scope: robot-1 is rebuilt because
// its bindings changed, tool-1 owns no domain and keeps its runtime.
manyforge::deployment::ApplyComponentDeltaDTO ReplacingDelta() {
  const manyforge::deployment::ApplyComponentChangeDTO robot{
      ControllableComponentId("robot-1"),
      manyforge::deployment::ApplyComponentDisposition::kReplace,
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestTwo)),
      {ControlDomainId("arm")},
      {ControlDomainId("arm")},
      {std::string(manyforge::deployment::kApplyComponentReasonBindings)}};
  const manyforge::deployment::ApplyComponentChangeDTO tool{
      ControllableComponentId("tool-1"),
      manyforge::deployment::ApplyComponentDisposition::kRetain,
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestOne)),
      {},
      {},
      {}};
  return {{robot, tool}, true, {ControllableComponentId("tool-1")}};
}

ManagerApplyFenceAttemptWriteResult EnsureAttempt(
    SqliteManagerApplyJournal &journal, const ContentDigest &intent_digest,
    const ApplyConfigurationFenceIntentDTO &fence_intent = FenceIntent()) {
  const auto affected =
      journal.EnsureAffectedScope(intent_digest, AffectedScope(fence_intent));
  return journal.EnsureFenceAttempt(
      intent_digest, affected.record.affected_scope_digest, fence_intent);
}

ConfigurationFenceAcceptedDTO
AcceptedFence(core::RequestReplayStatusDTO replay_status =
                  core::RequestReplayStatusDTO::kAcceptedNew) {
  return {
      ConfigurationFenceId("fence-1"),
      ConfigurationFenceGeneration(5U),
      FenceIntent().scope,
      123456789LL,
      replay_status,
  };
}

RuntimeUnitConvergencePlanRowDTO RuntimePlanRow() {
  const auto requirement = core::MakeRuntimeContractRequirement(
      core::RuntimeImplementationFamilyId("runtime-family"), {}, {});
  if (!requirement.ok()) {
    throw std::runtime_error("test runtime requirement is invalid");
  }
  return {
      RuntimeUnitId("runtime-1"),
      RuntimeProfileId("profile-1"),
      RuntimeProfileRevision("profile-r1"),
      core::RuntimeLifecycleControl::kManagerManaged,
      RuntimeUnitCandidateDisposition::kAdded,
      ProductId("runtime-product-1"),
      ProductContractId("manyforge.reference-runtime-product.v1"),
      ProductContractRevision("1"),
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestTwo)),
      {},
      {RuntimeRoleId("runtime.role.reference")},
      EndpointRoleId("runtime.status"),
      "manyforge.reference-runtime-status.v2",
      "1",
      "stdout_line",
      "manyforge.process-adapter.posix-shell.v1",
      "1",
      "manyforge.retained-artifact-retrieval.v1",
      "1",
      ContentDigest(std::string(kDigestOne)),
      128U,
      "linux",
      "posix-sh",
      requirement.value(),
      core::RuntimeContractOfferDigest(std::string(kDigestOne)),
  };
}

ManagerApplyRuntimeConvergenceIntentDTO
RuntimeIntent(const ContentDigest &acceptance_digest,
              const bool empty = false) {
  return {
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      RetainedBundleId("candidate-bundle-1"),
      GraphRevision("candidate-graph"),
      acceptance_digest,
      ActiveBundlePublicationGeneration(3U),
      ContentDigest(std::string(kDigestOne)),
      empty ? std::vector<RuntimeUnitConvergencePlanRowDTO>{}
            : std::vector<RuntimeUnitConvergencePlanRowDTO>{RuntimePlanRow()},
  };
}

ManagerApplyRuntimeStagedReadyDTO
RuntimeStagedReady(const ContentDigest &convergence_digest,
                   const std::uint64_t generation, const bool empty = false) {
  std::vector<RuntimeUnitStagedReadyEvidenceDTO> runtime_units;
  if (!empty) {
    runtime_units.push_back({
        RuntimeUnitId("runtime-1"),
        core::RuntimeLifecycleControl::kManagerManaged,
        RuntimeUnitCandidateDisposition::kAdded,
        RuntimeEpoch("runtime-epoch-1"),
        "observer-1",
        generation,
        generation,
        generation,
        ContentDigest(std::string(kDigestOne)),
        ContentDigest(std::string(kDigestTwo)),
        RuntimeSourcePhase::kServing,
        RuntimeSourceHealth::kHealthy,
        {RuntimeRoleId("runtime.role.reference")},
        ManagedCommandSurfaceState::kClosedNoActiveEffect,
        {},
        generation,
        generation + 1U,
        generation + 2U,
        core::RuntimeContractOfferDigest(std::string(kDigestOne)),
    });
  }
  return {
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      generation,
      convergence_digest,
      ActiveBundlePublicationGeneration(3U),
      ContentDigest(std::string(kDigestOne)),
      100U + generation,
      std::move(runtime_units),
  };
}

ProjectionRevisionDTO CandidateProjection() {
  return {
      RetainedBundleId("candidate-bundle-1"),
      GraphRevision("candidate-graph"),
      SceneRevision{
          SceneId(std::string(kSceneIdTwo)),
          8U,
          ContentDigest(std::string(kDigestTwo)),
      },
  };
}

ManagerApplyPrivateMaterializationDTO
PrivateMaterialization() {
  const auto candidate = CandidateProjection();
  return {
      candidate.scene_revision.content_hash,
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      candidate,
      ContentDigest(std::string(kDigestTwo)),
      512U,
  };
}
ManagerApplyBringUpPlanDTO
BringUpPlan(const ManagerApplyIntentDTO &parent = Intent(),
            core::ComponentLifecycleIntentKindDTO kind = core::ComponentLifecycleIntentKindDTO::kPrepare) {
  const ProjectionRevisionDTO candidate{
      parent.candidate_retained_bundle_id, parent.candidate_graph_revision,
      parent.expected_active_projection_revision.scene_revision};
  const core::ExpectedActiveControlConfigurationDTO expected_configuration{
      core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
      std::nullopt, std::nullopt};
  const core::ExpectedControlBindingRevisionDTO expected_binding{
      ControlBindingId("arm-jtc"), ControlBindingRevision("binding-r2"),
      core::ControlRouteId("route-1")};
  const core::ExpectedControlDomainRevisionDTO expected_domain{
      ControlDomainId("arm"), ControlDomainRevision("domain-r2"),
      core::ControllerTopologyId("topology-1"),
      core::TopologyRevision("topology-revision-1"), expected_configuration,
      {expected_binding}};
  core::ComponentLifecycleIntentDTO intent{
      {RequestId("bring-up-robot-1"), RequestOwnerEpoch("child-owner-1"),
       "test", std::nullopt},
      ControllableComponentId("robot-1"),
      kind,
      parent.expected_active_projection_revision.graph_revision,
      core::ComponentLifecycleGeneration(1U),
      {expected_domain},
      std::nullopt,
      {},
      core::AdmissionGeneration(7U),
      std::nullopt};
  ManagerApplyBringUpChildRequestDTO child{
      intent,
      candidate,
      ContentDigest(std::string(kDigestTwo)),
      512U,
      RuntimeUnitId("kernel-runtime"),
      RuntimeEpoch("kernel-epoch-1"),
      RuntimeEpoch("manager-epoch-1")};
  const ManagerApplyPrivateMaterializationDTO materialization{
      candidate.scene_revision.content_hash,
      parent.apply_id,
      parent.apply_generation,
      candidate,
      child.candidate_retained_bundle_digest,
      child.candidate_retained_bundle_bytes};
  const auto digest =
      ComputeManagerApplyPrivateMaterializationDigest(materialization);
  return {
      parent.apply_id,
      parent.apply_generation,
      ComputeManagerApplyIntentDigest(parent),
      {ControllableComponentId("robot-1"), ControllableComponentId("tool-1")},
      {child},
      {core::NamedApplyPublicationTransactionDTO{parent.apply_id,
                                                 parent.apply_generation},
       candidate, candidate.retained_bundle_id, candidate.graph_revision,
       ActiveBundlePublicationGeneration(3U), digest},
      ContentDigest(std::string(kDigestOne))};
}

core::ComponentLifecycleStatusDTO
BringUpChildStatus(const ManagerApplyBringUpIntentWriteResult &parent,
                   const PersistedManagerApplyFenceAcceptance &fence,
                   ManagerApplyBringUpChildPhase phase) {
  const auto &child = parent.plan.plan.children.front();
  const auto &intent = child.intent;
  const core::ComponentLifecycleAcceptedDTO acceptance{
      intent.request_context.request_id,
      core::RequestReplayStatusDTO::kAcceptedNew,
      core::LifecycleTransactionId("apply-component-1"),
      core::LifecycleTransactionGeneration(1U),
      core::ComponentLifecycleGeneration(
          intent.expected_component_lifecycle_generation.value() + 1U),
      42LL,
      1000LL};
  const core::ComponentLifecycleRequestedStateDTO requested{
      core::ComponentLifecycleRequestedStatePresenceDTO::kResolved,
      intent.intent == core::ComponentLifecycleIntentKindDTO::kDisable
          ? core::ComponentLifecycleStateDTO::kDisabled
          : (intent.intent == core::ComponentLifecycleIntentKindDTO::kPrepare
              ? core::ComponentLifecycleStateDTO::kPrepared
              : core::ComponentLifecycleStateDTO::kActive)};
  const bool accepted = phase == ManagerApplyBringUpChildPhase::kAccepted;
  const bool terminal = phase == ManagerApplyBringUpChildPhase::kTerminal;
  const core::ResolvedLifecycleControlDomainScopeDTO scope{
      ControlDomainId("arm"),
      ControlDomainRevision(accepted ? "domain-r1" : "domain-r2"),
      core::ControllerTopologyId("topology-1"),
      core::TopologyRevision("topology-revision-1"),
      std::nullopt,
      {{ControlBindingId("arm-jtc"),
        ControlBindingRevision(accepted ? "binding-r1" : "binding-r2"),
        core::ControlRouteId("route-1")}},
      {ResourceId("joint-1"), ResourceId("joint-2")}};
  const core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
      scope.control_domain_id,
      scope.control_domain_revision,
      core::ProviderInstanceId("provider-1"),
      core::ProviderManifestId("manifest-1"),
      core::ProviderManifestRevision("manifest-r1"),
      child.runtime_unit_id,
      child.runtime_epoch,
      ContentDigest(std::string(kDigestThree))};
  const core::CellAdmissionClaimDTO claim{
      {core::CellAdmissionClaimKindDTO::kManagerApply,
       core::CellAdmissionOwnerRoleDTO::kManager,
       parent.intent.intent.request_context.request_owner_epoch,
       parent.intent.intent.apply_id.value(),
       parent.intent.intent.request_context.request_id,
       parent.plan.plan_digest,
       *parent.intent.intent.expected_admission_generation,
       {},
       std::nullopt,
       std::nullopt},
      core::CellAdmissionClaimGeneration(8U),
      *parent.intent.intent.expected_admission_generation,
      42LL};
  const core::ApplyComponentBringUpReferenceDTO reference{
      {parent.intent.intent.apply_id, parent.intent.intent.apply_generation},
      parent.plan.plan_digest};
  const core::ApplyComponentBringUpAuthorityDTO authority{
      reference,
      claim,
      {reference.owner, fence.accepted_fence.configuration_fence_id,
       fence.accepted_fence.configuration_fence_generation,
       fence.accepted_fence.captured_scope, child.manager_runtime_epoch}};
  core::ComponentLifecycleStatusDTO status{
      acceptance,
      intent,
      accepted   ? 1U
      : terminal ? 3U
                 : 2U,
      intent.request_context.request_owner_epoch,
      core::ComponentLifecycleStateDTO::kDeclared,
      accepted ? core::ComponentLifecycleStateDTO::kDeclared : *requested.state,
      requested,
      accepted   ? core::ComponentLifecycleTransactionPhaseDTO::kAccepted
      : terminal ? core::ComponentLifecycleTransactionPhaseDTO::kSucceeded
                 : core::ComponentLifecycleTransactionPhaseDTO::kVerifying,
      {{scope.control_domain_id,
        intent.expected_control_domains.front(),
        scope,
        occurrence,
        core::ComponentLifecycleStateDTO::kDeclared,
        accepted ? core::ComponentLifecycleStateDTO::kDeclared
                 : *requested.state,
        requested,
        accepted ? core::ComponentLifecycleDomainActionStateDTO::kReserved
                 : core::ComponentLifecycleDomainActionStateDTO::kSucceeded,
        {},
        std::nullopt}},
      false,
      terminal ? std::optional<core::ComponentLifecycleTerminalResultDTO>(
                     core::ComponentLifecycleTerminalResultDTO::kSucceeded)
               : std::nullopt,
      std::nullopt,
      42LL,
      accepted   ? 42LL
      : terminal ? 44LL
                 : 43LL,
      terminal ? 0LL : 900LL};
  status.apply_bring_up = core::ApplyComponentBringUpEvidenceDTO{
      authority,
      authority.held_fence_query.expected_scope,
      core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
          intent, child.candidate_projection_revision,
          child.candidate_retained_bundle_digest,
          child.candidate_retained_bundle_bytes, child.runtime_unit_id,
          child.runtime_epoch, child.manager_runtime_epoch, reference),
      child.candidate_projection_revision,
      child.candidate_retained_bundle_digest,
      child.candidate_retained_bundle_bytes,
      {child.runtime_unit_id, child.runtime_epoch},
      std::nullopt,
      std::nullopt,
      std::nullopt};
  return status;
}

ManagerApplyBringUpChildProofDTO
BringUpChildProof(const ManagerApplyBringUpIntentWriteResult &parent,
                  const PersistedManagerApplyFenceAcceptance &fence,
                  ManagerApplyBringUpChildPhase phase) {
  const auto status = BringUpChildStatus(parent, fence, phase);
  return {parent.plan.plan.apply_id,
          parent.plan.plan.apply_generation,
          parent.plan.plan_digest,
          parent.plan.plan.children.front().intent.controllable_component_id,
          phase,
          {core::RequestReplayStatusDTO::kAcceptedNew, status.acceptance,
           status, std::nullopt, std::nullopt}};
}

ManagerApplyPublicationReceiptDTO PublicationReceipt(
    const PersistedManagerApplyPrivateMaterialization &materialization) {
  const auto &record = materialization.private_materialization;
  core::ActiveBundlePublicationRequestDTO request{
      core::NamedApplyPublicationTransactionDTO{
          record.apply_id,
          record.apply_generation,
      },
      record.candidate_projection_revision,
      record.candidate_projection_revision.retained_bundle_id,
      record.candidate_projection_revision.graph_revision,
      ActiveBundlePublicationGeneration(3U),
      materialization.private_materialization_content_digest,
  };
  return {
      materialization.private_materialization_content_digest,
      core::ActiveBundlePublicationReceiptDTO{
          std::move(request),
          ActiveBundlePublicationGeneration(4U),
          ContentDigest(std::string(kDigestOne)),
      },
  };
}

struct SelectedBundleTransitionRecords final {
  PersistedManagerApplyPrivateMaterialization materialization;
  PersistedManagerApplyPublicationReceipt publication;
};

ManagerApplyFenceReleaseIntentDTO
FenceReleaseIntent(const SelectedBundleTransitionRecords &records,
                   const PersistedManagerApplyFenceAcceptance &acceptance) {
  const auto &candidate =
      records.materialization.private_materialization.candidate_projection_revision;
  core::ConfigurationFenceReconciliationEvidenceDTO evidence{
      core::ExecutionHostEpochDTO{
          RuntimeUnitId("kernel-runtime"),
          RuntimeEpoch("kernel-epoch-1"),
          RequestOwnerEpoch("kernel-owner-epoch-1"),
          ContentDigest(std::string(kDigestTwo)),
          ActiveBundlePublicationGeneration(4U),
      },
      core::ApplyFenceOwnerDTO{ApplyId("apply-1"), ApplyGeneration(1U)},
      acceptance.accepted_fence.configuration_fence_id,
      acceptance.accepted_fence.configuration_fence_generation,
      acceptance.accepted_fence.captured_scope,
      12U,
      candidate.graph_revision,
      candidate,
      records.publication.publication.publication_receipt
          .publication_generation,
      records.publication.publication.publication_receipt
          .durable_public_pointer_content_digest,
      {{ControlDomainId("arm"),
        core::ActiveControlConfigurationId("trajectory-mode-v2"),
        core::ActiveControlConfigurationGeneration(4U)}},
      0U,
      0U,
      ContentDigest(std::string(kDigestOne)),
  };
  return {
      records.publication.publication_receipt_digest,
      acceptance.acceptance_digest,
      core::ConfigurationFenceReleaseDTO{std::move(evidence),
                                         RuntimeEpoch("manager-epoch-1")},
  };
}

ManagerApplyFenceReleasedDTO
FenceReleased(const PersistedManagerApplyFenceReleaseIntent &intent) {
  const auto &request = intent.fence_release_intent.release_request;
  return {
      intent.fence_release_intent_digest,
      core::ConfigurationFenceReleasedDTO{
          request,
          request.reconciliation_evidence.execution_host_epoch,
          987654321LL,
          core::RequestReplayStatusDTO::kAcceptedNew,
          ContentDigest(std::string(kDigestTwo)),
      },
  };
}

ManagerApplyTerminalOutcomeDTO
TerminalOutcome(const SelectedBundleTransitionRecords &records,
                const PersistedManagerApplyFenceReleased &released) {
  const auto &result = records.materialization.private_materialization;
  return {
      result.apply_id,
      result.apply_generation,
      result.candidate_projection_revision,
      records.publication.publication_receipt_digest,
      records.publication.publication_receipt_digest,
      released.fence_released_digest,
      ManagerApplyTerminalOutcomeState::kSucceeded,
      std::nullopt,
  };
}

struct ApplyCompletionRecords final {
  PersistedManagerApplyFenceReleaseIntent release_intent;
  PersistedManagerApplyFenceReleased released;
  PersistedManagerApplyTerminalOutcome terminal;
};

ApplyCompletionRecords
EnsureApplyCompletion(SqliteManagerApplyJournal &journal,
                      const SelectedBundleTransitionRecords &records) {
  const auto acceptance =
      journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  if (!acceptance.has_value()) {
    throw std::runtime_error("test fixture lacks fence acceptance");
  }
  const auto release_intent = journal.EnsureFenceReleaseIntent(
      FenceReleaseIntent(records, *acceptance));
  const auto released =
      journal.EnsureFenceReleased(FenceReleased(release_intent.record));
  const auto terminal =
      journal.EnsureTerminalOutcome(TerminalOutcome(records, released.record));
  return {release_intent.record, released.record, terminal.record};
}

struct AcceptedApplyRecords final {
  PersistedManagerApplyIntent intent;
  PersistedManagerApplyFenceAcceptance acceptance;
};

AcceptedApplyRecords EnsureAcceptedApply(SqliteManagerApplyJournal &journal) {
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;
  const auto acceptance = journal.EnsureFenceAcceptance(
      ApplyId("apply-1"), ApplyGeneration(1U), attempt.fence_attempt_digest,
      AcceptedFence());
  return {intent, acceptance.record};
}

struct SceneParentRecords final {
  PersistedManagerApplyRuntimeStagedReady staged_ready;
};

SceneParentRecords EnsureSceneParents(SqliteManagerApplyJournal &journal,
                                      const bool empty_runtime = false) {
  const auto accepted = EnsureAcceptedApply(journal);
  const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
      accepted.intent.intent_digest,
      RuntimeIntent(accepted.acceptance.acceptance_digest, empty_runtime));
  const auto staged_ready = journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
      runtime_intent.record.runtime_convergence_intent_digest, 1U,
      empty_runtime));
  return {staged_ready.record};
}

SelectedBundleTransitionRecords
EnsureSelectedBundleTransition(SqliteManagerApplyJournal &journal) {
  static_cast<void>(EnsureSceneParents(journal, true));
  const auto materialization =
      journal.EnsurePrivateMaterialization(PrivateMaterialization());
  const auto publication = journal.EnsurePublicationReceipt(
      PublicationReceipt(materialization.record));
  return {materialization.record, publication.record};
}

bool SameFenceIntent(const ApplyConfigurationFenceIntentDTO &lhs,
                     const ApplyConfigurationFenceIntentDTO &rhs) {
  return lhs.owner_request_context == rhs.owner_request_context &&
         lhs.owner_transaction == rhs.owner_transaction &&
         lhs.scope == rhs.scope &&
         lhs.busy_disposition == rhs.busy_disposition &&
         lhs.expected_retained_bundle_id == rhs.expected_retained_bundle_id &&
         lhs.expected_graph_revision == rhs.expected_graph_revision &&
         lhs.expected_operation_resource_generation ==
             rhs.expected_operation_resource_generation &&
         lhs.expected_fence_generation == rhs.expected_fence_generation &&
         lhs.expected_active_control_configurations ==
             rhs.expected_active_control_configurations &&
         lhs.reason == rhs.reason;
}

bool SameAcceptedFence(const ConfigurationFenceAcceptedDTO &lhs,
                       const ConfigurationFenceAcceptedDTO &rhs) {
  return lhs.configuration_fence_id == rhs.configuration_fence_id &&
         lhs.configuration_fence_generation ==
             rhs.configuration_fence_generation &&
         lhs.captured_scope == rhs.captured_scope &&
         lhs.accepted_at_ns == rhs.accepted_at_ns &&
         lhs.replay_status == rhs.replay_status;
}

void ExecSql(const std::filesystem::path &path, const std::string &sql) {
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr),
            SQLITE_OK);
  char *error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  const std::string detail =
      error == nullptr ? std::string{} : std::string(error);
  sqlite3_free(error);
  EXPECT_EQ(result, SQLITE_OK) << detail;
  EXPECT_EQ(sqlite3_close(database), SQLITE_OK);
}

std::string ReadFileBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open test file");
  }
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

void ExpectNoSqliteSidecars(const std::filesystem::path &path) {
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-journal"));
}

std::shared_ptr<const deployment::RetainedBundle>
NativeParentCandidate(const std::filesystem::path &root,
                      const bool components) {
  deployment::FilesystemBuildManifestRepository manifests(root);
  deployment::FilesystemCatalogArtifactStore catalogs(root);
  deployment::FilesystemRawArtifactStore raw_artifacts(root);
  deployment::FilesystemRetainedBundleRepository bundles(root);
  if (components) {
    auto fixture =
        deployment::test::make_lite6_ros2_control_shared_consumer_retention_fixture(
            "runtime_only_cell");
    const deployment::test::Lite6ControllableCompilerInputArtifactSource source(
        fixture);
    return deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            fixture.qualified, manifests, source,
            fixture.structural_preparations(), catalogs, raw_artifacts,
            bundles);
  }
  auto fixture = deployment::test::make_runtime_only_retention_fixture();
  const deployment::test::RuntimeOnlyCompilerInputArtifactSource source(
      fixture.runtime_profile);
  return deployment::detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(fixture.qualified, manifests,
                                              source, {}, catalogs,
                                              raw_artifacts, bundles);
}

struct NativeParentReadFixture final {
  core::NativeApplyPlanDTO plan;
  core::CellAdmissionClaimDTO claim;
  core::CellAdmissionRecoveryReceiptQueryDTO query;
  core::CellAdmissionRecoveryReceiptDTO receipt;
};

core::CellAdmissionRecoveryReceiptDTO
HeldNativeParent(const core::CellAdmissionClaimDTO &claim) {
  return {core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact,
          RequestOwnerEpoch("kernel-original-parent"),
          core::AdmissionGeneration(9U),
          claim,
          std::nullopt,
          std::nullopt,
          {claim},
          {{claim.intent.owner_role,
            RequestOwnerEpoch("replacement-owner-observer")}}};
}

// These fixtures exercise the storage joins. The named coordinator's separate
// test obtains the same early/later codes from the actual Prepare/Finalize
// catch.
NativeParentReadFixture WriteNativeParent(
    const std::filesystem::path &path,
    const std::filesystem::path &package_root,
    const deployment::RetainedBundle &candidate, const bool authoring = false,
    const std::string &failure_code = "runtime_composition_preparation_failed",
    const ManagerApplyPhase phase = ManagerApplyPhase::kRuntimeConverged,
    const bool materialized = false,
    const deployment::RetainedBundle *predecessor = nullptr,
    const bool linked = false, const bool holding = false,
    // A known `failed` terminal with this cause code instead of unknown_effect.
    const std::optional<std::string> &known_cause_code = std::nullopt) {
  const auto &original = predecessor == nullptr ? candidate : *predecessor;
  auto intent = authoring ? ParentClaimIntent() : Intent();
  intent.cell_id = candidate.value().cell_graph.cell_id;
  intent.candidate_retained_bundle_id = candidate.retained_bundle_id();
  intent.candidate_graph_revision = candidate.value().cell_graph.graph_revision;
  intent.expected_active_projection_revision.retained_bundle_id =
      original.retained_bundle_id();
  intent.expected_active_projection_revision.graph_revision =
      original.value().cell_graph.graph_revision;
  intent.prepared_activation.deployment_artifact_id =
      candidate.value().deployment_artifact_id;
  intent.prepared_activation.deployment_revision =
      candidate.value().deployment_revision;
  SqliteManagerApplyJournal journal(path, intent.cell_id);
  std::optional<ManagerApplyBringUpIntentWriteResult> linked_parent;
  if (linked) {
    auto plan = BringUpPlan(intent);
    plan.configured_components.clear();
    for (const auto &component :
         candidate.value().cell_graph.controllable_components)
      plan.configured_components.push_back(component.controllable_component_id);
    auto &child = plan.children.front();
    child.intent.controllable_component_id = plan.configured_components.front();
    child.intent.expected_control_domains.clear();
    child.runtime_unit_id = candidate.value()
                                .cell_graph.control_bindings.front()
                                .provider_runtime_unit_id;
    child.candidate_retained_bundle_digest =
        ContentDigest(deployment::sha256_digest(candidate.canonical_bytes()));
    child.candidate_retained_bundle_bytes = candidate.canonical_bytes().size();
    for (const auto &desired :
         candidate.value().desired_control_configurations) {
      std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
      for (const auto &route : desired.binding_routes)
        bindings.push_back({route.control_binding_id,
                            route.control_binding_revision,
                            route.control_route_id});
      const core::ExpectedActiveControlConfigurationDTO expected_configuration{
          core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
          std::nullopt, std::nullopt};
      const core::ExpectedControlDomainRevisionDTO expected_domain{
          desired.control_domain_id, desired.control_domain_revision,
          desired.controller_topology_id, desired.topology_revision,
          expected_configuration, bindings};
      child.intent.expected_control_domains.push_back(expected_domain);
    }
    plan.expected_publication.private_materialization_content_digest =
        ComputeManagerApplyPrivateMaterializationDigest(
            {child.candidate_projection_revision.scene_revision.content_hash,
             intent.apply_id, intent.apply_generation,
             child.candidate_projection_revision,
             child.candidate_retained_bundle_digest,
             child.candidate_retained_bundle_bytes});
    linked_parent = journal.EnsureBringUpIntent(intent, plan);
  }
  const auto persisted = linked_parent ? linked_parent->intent
                                       : journal.EnsureIntent(intent).record;
  auto fence = FenceIntent();
  fence.owner_request_context = intent.request_context;
  fence.scope =
      deployment::derive_apply_configuration_fence_scope(original, candidate);
  fence.expected_retained_bundle_id = original.retained_bundle_id();
  fence.expected_graph_revision = original.value().cell_graph.graph_revision;
  fence.expected_active_control_configurations.clear();
  for (const auto &domain : fence.scope.domains)
    if (domain.predecessor_control_domain_revision)
      fence.expected_active_control_configurations.push_back(
          {domain.control_domain_id,
           core::ActiveControlConfigurationId("test-original-configuration"),
           core::ActiveControlConfigurationGeneration(4U)});
  const auto delta =
      deployment::derive_apply_component_delta(original, candidate);
  const auto affected =
      journal
          .EnsureAffectedScope(persisted.intent_digest,
                               {intent.apply_id, intent.apply_generation,
                                fence.scope, fence.busy_disposition, delta})
          .record;
  const auto attempt =
      journal
          .EnsureFenceAttempt(persisted.intent_digest,
                              affected.affected_scope_digest, fence)
          .record;
  auto accepted = AcceptedFence();
  accepted.captured_scope = fence.scope;
  const auto acceptance =
      journal
          .EnsureFenceAcceptance(intent.apply_id, intent.apply_generation,
                                 attempt.fence_attempt_digest, accepted)
          .record;
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root);
  auto convergence = RuntimeIntent(acceptance.acceptance_digest, true);
  convergence.candidate_retained_bundle_id =
      intent.candidate_retained_bundle_id;
  convergence.candidate_graph_revision = intent.candidate_graph_revision;
  convergence.runtime_units = BuildVerifiedRuntimeConvergencePlan(
      candidate, artifacts, "linux",
      {"posix.1-2017",
       std::string(deployment::ros2_control_execution_runtime_contract::kLaunchEnvironmentContract)});
  const auto runtime =
      journal
          .EnsureRuntimeConvergenceIntent(persisted.intent_digest, convergence)
          .record;
  auto ready =
      RuntimeStagedReady(runtime.runtime_convergence_intent_digest, 1U, true);
  for (const auto &row : convergence.runtime_units) {
    auto evidence =
        RuntimeStagedReady(ContentDigest(std::string(kDigestOne)), 1U)
            .runtime_units.front();
    evidence.runtime_unit_id = row.runtime_unit_id;
    evidence.lifecycle_control = row.lifecycle_control;
    evidence.candidate_disposition = row.candidate_disposition;
    evidence.observed_product_identity = row.product_identity;
    evidence.observed_configuration_identity = row.configuration_identity;
    evidence.observed_runtime_contract_offer_digest =
        row.runtime_contract_offer_digest;
    evidence.ready_runtime_role_ids = row.required_runtime_role_ids;
    for (const auto &dependency : row.dependency_runtime_unit_ids)
      evidence.dependency_runtime_epochs.push_back(
          {dependency, RuntimeEpoch("actual-test-dependency-epoch")});
    ready.runtime_units.push_back(std::move(evidence));
  }
  static_cast<void>(journal.EnsureRuntimeStagedReady(ready));
  if (linked_parent) {
    for (const auto proof_phase : {ManagerApplyBringUpChildPhase::kAccepted,
                                   ManagerApplyBringUpChildPhase::kHolding}) {
      if (proof_phase == ManagerApplyBringUpChildPhase::kHolding && !holding)
        break;
      auto proof = BringUpChildProof(*linked_parent, acceptance, proof_phase);
      auto &status = *proof.result.status;
      status.previous_state.reset();
      const bool installed =
          proof_phase == ManagerApplyBringUpChildPhase::kHolding;
      if (!installed)
        status.current_state.reset();
      status.domains.clear();
      for (const auto &expected : linked_parent->plan.plan.children.front()
                                      .intent.expected_control_domains) {
        std::optional<core::ResolvedLifecycleControlDomainScopeDTO> scope;
        std::optional<core::ResolvedLifecycleProviderOccurrenceDTO> provider;
        if (installed) {
          std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
          std::set<ResourceId> resources;
          const core::ControlBindingDTO *first_binding = nullptr;
          for (const auto &binding :
               candidate.value().cell_graph.control_bindings)
            if (binding.control_domain_id == expected.control_domain_id) {
              if (first_binding == nullptr)
                first_binding = &binding;
              const auto route = std::find_if(
                  expected.controller().expected_control_bindings.begin(),
                  expected.controller().expected_control_bindings.end(), [&](const auto& value) {
                    return value.control_binding_id == binding.control_binding_id;
                  });
              bindings.push_back({binding.control_binding_id,
                                  binding.control_binding_revision,
                                  route->control_route_id});
              for (const auto &resource : binding.resource_bindings)
                resources.insert(resource.resource_id);
            }
          scope = core::ResolvedLifecycleControlDomainScopeDTO{
              expected.control_domain_id,
              expected.control_domain_revision,
              expected.controller().controller_topology_id,
              expected.controller().topology_revision,
              std::nullopt,
              bindings,
              {resources.begin(), resources.end()}};
          provider = core::ResolvedLifecycleProviderOccurrenceDTO{
              expected.control_domain_id,
              expected.control_domain_revision,
              first_binding->provider_instance_id,
              first_binding->provider_manifest_id,
              first_binding->provider_manifest_revision,
              linked_parent->plan.plan.children.front().runtime_unit_id,
              linked_parent->plan.plan.children.front().runtime_epoch,
              ContentDigest(std::string(kDigestThree))};
        }
        const auto observed =
            installed ? status.requested_state.state : std::nullopt;
        status.domains.push_back(
            {expected.control_domain_id,
             expected,
             scope,
             provider,
             observed,
             observed,
             status.requested_state,
             installed
                 ? core::ComponentLifecycleDomainActionStateDTO::kSucceeded
                 : core::ComponentLifecycleDomainActionStateDTO::kReserved,
             {},
             std::nullopt});
      }
      if (!proof.result.locally_valid())
        throw std::runtime_error(
            "native parent delegated proof fixture is invalid");
      static_cast<void>(journal.EnsureBringUpChildProof(proof));
    }
  }
  if (materialized) {
    auto materialization = PrivateMaterialization();
    materialization.candidate_projection_revision.retained_bundle_id =
        candidate.retained_bundle_id();
    materialization.candidate_projection_revision.graph_revision =
        intent.candidate_graph_revision;
    static_cast<void>(journal.EnsurePrivateMaterialization(materialization));
  }
  if (!failure_code.empty())
    static_cast<void>(journal.EnsureTerminalOutcome(
        {intent.apply_id, intent.apply_generation, std::nullopt, std::nullopt,
         std::nullopt, std::nullopt,
         known_cause_code ? ManagerApplyTerminalOutcomeState::kFailed
                          : ManagerApplyTerminalOutcomeState::kUnknownEffect,
         ManagerApplyFailureEvidenceDTO{
             persisted.intent_digest, phase, failure_code,
             "original retained preparation exception",
             known_cause_code ? ManagerApplyEffectDisposition::kKnown
                              : ManagerApplyEffectDisposition::kUnknown,
             known_cause_code.value_or("typed_cause_code"),
             {},
             "component_manager",
             ManagerApplyEffectBoundary::kUnknown}}));
  core::NativeApplyPlanDTO plan{{intent.apply_id, intent.apply_generation},
                                candidate.retained_bundle_id(),
                                intent.candidate_graph_revision,
                                {},
                                {},
                                {},
                                {},
                                {},
                                {}};
  for (const auto &row : delta.components) {
    if (row.candidate_runtime_identity)
      plan.components.push_back({row.controllable_component_id,
                                 core::ComponentLifecycleStateDTO::kDeclared,
                                 core::ComponentLifecycleGeneration(3U)});
    if (row.disposition == deployment::ApplyComponentDisposition::kRetain)
      plan.retained_components.push_back(row.controllable_component_id);
    if (row.disposition == deployment::ApplyComponentDisposition::kRemove)
      plan.removed_components.push_back(row.controllable_component_id);
  }
  for (const auto &desired : candidate.value().desired_control_configurations) {
    plan.routes.push_back({desired.control_domain_id,
                           core::ActiveControlConfigurationGeneration(4U)});
    if (std::any_of(
            delta.components.begin(), delta.components.end(),
            [&](const auto &row) {
              return row.disposition ==
                         deployment::ApplyComponentDisposition::kRetain &&
                     std::binary_search(row.candidate_domains.begin(),
                                        row.candidate_domains.end(),
                                        desired.control_domain_id);
            }))
      plan.retained_domains.push_back(desired.control_domain_id);
  }
  for (const auto &desired : original.value().desired_control_configurations)
    if (std::none_of(candidate.value().desired_control_configurations.begin(),
                     candidate.value().desired_control_configurations.end(),
                     [&](const auto &value) {
                       return value.control_domain_id ==
                              desired.control_domain_id;
                     }))
      plan.removed_domains.push_back(desired.control_domain_id);
  const core::CellAdmissionClaimDTO claim =
      intent.parent_authoring_claim.value_or(core::CellAdmissionClaimDTO{
          {core::CellAdmissionClaimKindDTO::kManagerApply,
           core::CellAdmissionOwnerRoleDTO::kManager,
           intent.request_context.request_owner_epoch,
           intent.apply_id.value(),
           intent.request_context.request_id,
           linked_parent ? linked_parent->plan.plan_digest
                         : persisted.intent_digest,
           core::AdmissionGeneration(7U),
           {},
           std::nullopt,
           std::nullopt,
           std::nullopt},
          core::CellAdmissionClaimGeneration(linked_parent ? 8U : 3U),
          core::AdmissionGeneration(7U),
          42LL});
  return {std::move(plan),
          claim,
          {claim.intent, claim, std::nullopt},
          HeldNativeParent(claim)};
}

TEST(ManagerApplyJournalIntegrity,
     RejectsSchemaEmptyDatabaseWithForeignSqliteIdentity) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  ExecSql(path, "PRAGMA application_id=424242;"
                "PRAGMA user_version=77;");
  const auto before = ReadFileBytes(path);
  ExpectNoSqliteSidecars(path);

  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), before);
  ExpectNoSqliteSidecars(path);
}

TEST(ManagerApplyJournalIntegrity,
     RejectsForeignSchemaAndCellWithoutChangingAtRestSource) {
  TemporaryDirectory directory;
  const auto schema_path = directory.path() / "foreign-schema.sqlite3";
  ExecSql(schema_path, "CREATE TABLE foreign_table(value TEXT);");
  const auto schema_before = ReadFileBytes(schema_path);
  EXPECT_THROW(SqliteManagerApplyJournal(schema_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(schema_path), schema_before);
  ExpectNoSqliteSidecars(schema_path);

  const auto cell_path = directory.path() / "foreign-cell.sqlite3";
  { SqliteManagerApplyJournal owner(cell_path, CellId("foreign-cell")); }
  ExpectNoSqliteSidecars(cell_path);
  const auto cell_before = ReadFileBytes(cell_path);
  EXPECT_THROW(SqliteManagerApplyJournal(cell_path, CellId("cell-1")),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(ReadFileBytes(cell_path), cell_before);
  ExpectNoSqliteSidecars(cell_path);
}

TEST(ManagerApplyJournalReadOnly,
     NeverCreatesAndPreservesExactRecordDatabaseAndWal) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "read-only.sqlite3";
  const auto intent = Intent();
  const core::NamedApplyPublicationTransactionDTO apply{
      intent.apply_id, intent.apply_generation};
  EXPECT_FALSE(
      ReadManagerApplyStatusReadOnly(path, intent.cell_id, apply).ok());
  EXPECT_FALSE(std::filesystem::exists(path));
  ExpectNoSqliteSidecars(path);
  SqliteManagerApplyJournal owner(path, intent.cell_id);
  const auto accepted = owner.EnsureIntent(intent);
  const auto wal = std::filesystem::path(path.string() + "-wal");
  ASSERT_TRUE(std::filesystem::exists(wal));
  const auto database_before = ReadFileBytes(path);
  const auto wal_before = ReadFileBytes(wal);
  const auto actual =
      ReadManagerApplyStatusReadOnly(path, intent.cell_id, apply);
  ASSERT_TRUE(actual.ok()) << actual.error().message;
  EXPECT_EQ(actual.value().intent, accepted.record);
  EXPECT_FALSE(
      ReadManagerApplyStatusReadOnly(path, CellId("another-cell"), apply).ok());
  EXPECT_FALSE(ReadManagerApplyStatusReadOnly(
                   path, intent.cell_id, {apply.apply_id, ApplyGeneration(2U)})
                   .ok());
  EXPECT_EQ(ReadFileBytes(path), database_before);
  EXPECT_EQ(ReadFileBytes(wal), wal_before);
  ExecSql(
      path,
      "DROP TRIGGER manager_apply_intents_no_update;"
      "UPDATE manager_apply_intents SET source_label='tampered';"
      "CREATE TRIGGER manager_apply_intents_no_update BEFORE UPDATE ON "
      "manager_apply_intents "
      "BEGIN SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); END;");
  const auto corrupt_database = ReadFileBytes(path);
  const auto corrupt_wal = ReadFileBytes(wal);
  EXPECT_FALSE(
      ReadManagerApplyStatusReadOnly(path, intent.cell_id, apply).ok());
  EXPECT_EQ(ReadFileBytes(path), corrupt_database);
  EXPECT_EQ(ReadFileBytes(wal), corrupt_wal);
}

TEST(ManagerApplyJournalIntegrity, RejectsPreUa4JournalVersion) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "pre-ua4.sqlite3";
  { SqliteManagerApplyJournal initialized(path, CellId("cell-1")); }
  ExecSql(path, "PRAGMA user_version=12;");
  const auto before = ReadFileBytes(path);
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), before);
  ExpectNoSqliteSidecars(path);
}

TEST(ManagerApplyJournalIntegrity,
     RejectsOrphanShmWithoutWalWithoutChangingEitherFile) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "orphan-shm.sqlite3";
  {
    std::ofstream main(path, std::ios::binary);
    ASSERT_TRUE(main);
  }
  const auto shm_path = std::filesystem::path(path.string() + "-shm");
  {
    std::ofstream shm(shm_path, std::ios::binary);
    ASSERT_TRUE(shm);
    shm << "orphan-shm";
  }
  const auto database_before = ReadFileBytes(path);
  const auto shm_before = ReadFileBytes(shm_path);

  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), database_before);
  EXPECT_EQ(ReadFileBytes(shm_path), shm_before);
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
}

TEST(ManagerApplyJournalIntegrity,
     RejectsForeignCellFromWalCrashResidueWithoutChangingSource) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "wal-residue.sqlite3";
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      auto *owner = new SqliteManagerApplyJournal(path, CellId("foreign-cell"));
      static_cast<void>(owner);
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  const auto wal_path = std::filesystem::path(path.string() + "-wal");
  const auto shm_path = std::filesystem::path(path.string() + "-shm");
  ASSERT_TRUE(std::filesystem::exists(wal_path));
  std::filesystem::remove(shm_path);
  const auto database_before = ReadFileBytes(path);
  const auto wal_before = ReadFileBytes(wal_path);

  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(ReadFileBytes(path), database_before);
  EXPECT_EQ(ReadFileBytes(wal_path), wal_before);
  EXPECT_FALSE(std::filesystem::exists(shm_path));

  EXPECT_NO_THROW({
    SqliteManagerApplyJournal recovered(path, CellId("foreign-cell"));
    EXPECT_EQ(recovered.cell_id(), CellId("foreign-cell"));
  });
}

TEST(ManagerApplyIntent, ValidityRejectsMissingRequiredIdentity) {
  auto intent = Intent();
  EXPECT_TRUE(intent.valid());

  intent.request_context.request_id = RequestId("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.request_context.source_label.clear();
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.request_context.correlation_id = "";
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.apply_generation = ApplyGeneration(0U);
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.expected_active_projection_revision.scene_revision.revision_id = 0U;
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.candidate_retained_bundle_id = RetainedBundleId("");
  EXPECT_FALSE(intent.valid());

  // The exact UA2A prepared activation is part of the operation identity: an
  // intent without it cannot name the input it accepted.
  intent = Intent();
  intent.prepared_activation.captured_input_digest = ContentDigest("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.deployment_artifact_id =
      core::DeploymentArtifactId("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.deployment_revision =
      core::DeploymentRevision("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.program_binding_id = core::ProgramBindingId("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.prepared_activation_id = ContentDigest("");
  EXPECT_FALSE(intent.valid());
}

TEST(ManagerApplyJournal,
     DerivesOneBoundedOperationStatusFromDurableMilestones) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "status.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;

  // An unknown durable identity is not an operation.
  EXPECT_FALSE(
      journal.ReadStatus(ApplyId("apply-unknown"), ApplyGeneration(1U))
          .has_value());

  const auto accepted =
      journal.ReadStatus(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(accepted->intent.intent_digest, intent.intent_digest);
  EXPECT_FALSE(accepted->terminal_outcome.has_value());
  const auto accepted_progress = DeriveManagerApplyProgress(*accepted);
  EXPECT_EQ(accepted_progress.phase, ManagerApplyPhase::kAccepted);
  EXPECT_EQ(accepted_progress.record_digest, intent.intent_digest);
  EXPECT_EQ(std::string(ToString(accepted_progress.phase)), "accepted");

  // The phase advances only with the exact parent-linked durable record; a
  // heartbeat or any elapsed time changes nothing.
  const auto scope =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope()).record;
  const auto fenced =
      journal.ReadStatus(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(fenced.has_value());
  const auto fenced_progress = DeriveManagerApplyProgress(*fenced);
  EXPECT_EQ(fenced_progress.phase, ManagerApplyPhase::kFenceAcquired);
  EXPECT_EQ(fenced_progress.record_digest, scope.affected_scope_digest);
  EXPECT_EQ(std::string(ToString(fenced_progress.phase)), "fence_acquired");
}

TEST(ManagerApplyTerminalOutcome, RequiresExactlyOneCompleteDisposition) {
  const auto projection = ProjectionRevisionDTO{
      RetainedBundleId("bundle.candidate"),
      GraphRevision("graph.candidate"),
      SceneRevision{
          SceneId(std::string(kSceneIdOne)),
          3U,
          ContentDigest(std::string(kDigestOne)),
      },
  };
  const auto failure = ManagerApplyFailureEvidenceDTO{
      ContentDigest(std::string(kDigestThree)),
      ManagerApplyPhase::kRuntimeConverged,
      "runtime_initialization_failed",
      "replacement initialization failed after teardown",
      ManagerApplyEffectDisposition::kKnown,
      "typed_cause_code",
      {},
      "component_manager",
      ManagerApplyEffectBoundary::kUnknown,
  };
  EXPECT_TRUE(failure.valid());

  const ManagerApplyTerminalOutcomeDTO full_success{
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      projection,
      ContentDigest(std::string(kDigestTwo)),
      ContentDigest(std::string(kDigestThree)),
      ContentDigest(std::string(kDigestOne)),
      ManagerApplyTerminalOutcomeState::kSucceeded,
      std::nullopt,
  };
  EXPECT_TRUE(full_success.valid());
  EXPECT_TRUE(full_success.succeeded());

  // A success carries the complete publication evidence and no failure.
  auto partial = full_success;
  partial.fence_released_digest.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.scene_completion_digest.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.publication_receipt_digest.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.candidate_projection_revision.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.publication_receipt_digest = ContentDigest(std::string());
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.failure = failure;
  EXPECT_FALSE(partial.valid());

  const ManagerApplyTerminalOutcomeDTO failed{
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kFailed,
      failure,
  };
  EXPECT_TRUE(failed.valid());
  EXPECT_FALSE(failed.succeeded());

  // A failure carries no publication evidence at all: one stray success field
  // is rejected even though the complete success set is obviously absent.
  auto stray = failed;
  stray.fence_released_digest = ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(stray.valid());
  stray = failed;
  stray.scene_completion_digest = ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(stray.valid());
  stray = failed;
  stray.publication_receipt_digest = ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(stray.valid());
  stray = failed;
  stray.candidate_projection_revision = projection;
  EXPECT_FALSE(stray.valid());

  // A known failure and an unknown effect are distinct dispositions.
  auto mismatched = failed;
  mismatched.failure->effect_disposition =
      ManagerApplyEffectDisposition::kUnknown;
  EXPECT_FALSE(mismatched.valid());
  auto uncertain = failed;
  uncertain.state = ManagerApplyTerminalOutcomeState::kUnknownEffect;
  EXPECT_FALSE(uncertain.valid());
  uncertain.failure->effect_disposition =
      ManagerApplyEffectDisposition::kUnknown;
  EXPECT_TRUE(uncertain.valid());
  EXPECT_FALSE(uncertain.succeeded());
  uncertain.state = ManagerApplyTerminalOutcomeState::kSucceeded;
  EXPECT_FALSE(uncertain.valid());

  // A terminal failure retains exactly one valid, bounded, intent-bound failure.
  auto missing = failed;
  missing.failure.reset();
  EXPECT_FALSE(missing.valid());
  auto unbound = failed;
  unbound.failure->intent_digest = ContentDigest(std::string());
  EXPECT_FALSE(unbound.valid());
  auto empty_code = failed;
  empty_code.failure->failure_code.clear();
  EXPECT_FALSE(empty_code.valid());
  auto empty_message = failed;
  empty_message.failure->failure_message.clear();
  EXPECT_FALSE(empty_message.valid());
  auto oversized_code = failed;
  oversized_code.failure->failure_code =
      std::string(kMaximumManagerApplyFailureCodeBytes + 1U, 'c');
  EXPECT_FALSE(oversized_code.valid());
  auto oversized_message = failed;
  oversized_message.failure->failure_message =
      std::string(kMaximumManagerApplyFailureMessageBytes + 1U, 'm');
  EXPECT_FALSE(oversized_message.valid());

  // The persisted vocabulary never spells an unknown-effect failure as a
  // success or as clean absence.
  EXPECT_EQ(std::string(ToString(ManagerApplyEffectDisposition::kKnown)),
            "known");
  EXPECT_EQ(std::string(ToString(ManagerApplyEffectDisposition::kUnknown)),
            "unknown");
  EXPECT_EQ(std::string(ToString(ManagerApplyTerminalOutcomeState::kFailed)),
            "failed");
  EXPECT_EQ(
      std::string(ToString(ManagerApplyTerminalOutcomeState::kUnknownEffect)),
      "unknown_effect");
}

TEST(ManagerApplyJournal, RetainsTheTypedCauseOfATerminalFailure) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "terminal-typed-cause.sqlite3";
  std::optional<ManagerApplyTerminalOutcomeDTO> expected;
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    // The phase code names the failed Apply step; the cause keeps the Kernel's
    // own code, details (separator bytes included) and producer.
    const ManagerApplyTerminalOutcomeDTO outcome{
        ApplyId("apply-1"),
        ApplyGeneration(1U),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        ManagerApplyTerminalOutcomeState::kFailed,
        ManagerApplyFailureEvidenceDTO{
            intent.intent_digest,
            ManagerApplyPhase::kAccepted,
            "apply_admission_failed",
            "Kernel rejected Manager Apply admission: stale_generation: moved",
            ManagerApplyEffectDisposition::kKnown,
            "stale_generation",
            {{"reason", "expected_generation_stale"},
             {"odd=key:", "value\nwith=separators:9"}},
            "kernel",
            ManagerApplyEffectBoundary::kBeforeEffect},
    };
    EXPECT_TRUE(journal.EnsureTerminalOutcome(outcome).inserted);
    expected = outcome;
    auto other_cause = outcome;
    other_cause.failure->cause_details.erase("reason");
    EXPECT_THROW(static_cast<void>(journal.EnsureTerminalOutcome(other_cause)),
                 ManagerApplyJournalConflictError);
  }
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto stored =
      reopened.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(stored.has_value() && expected.has_value());
  EXPECT_EQ(stored->terminal_outcome, *expected);
  EXPECT_EQ(stored->terminal_outcome.failure->cause_code, "stale_generation");
  EXPECT_EQ(stored->terminal_outcome.failure->producer, "kernel");
  EXPECT_EQ(stored->terminal_outcome.failure->effect_boundary,
            ManagerApplyEffectBoundary::kBeforeEffect);
  EXPECT_EQ(stored->terminal_outcome.failure->cause_details.at("odd=key:"),
            "value\nwith=separators:9");
}

TEST(ManagerApplyTerminalOutcome, TypedCauseIsBounded) {
  const auto valid = ManagerApplyFailureEvidenceDTO{
      ContentDigest(std::string(kDigestThree)),
      ManagerApplyPhase::kAccepted,
      "apply_admission_failed",
      "refused",
      ManagerApplyEffectDisposition::kKnown,
      "stale_generation",
      {{"reason", "expected_generation_stale"}},
      "kernel",
      ManagerApplyEffectBoundary::kUnknown,
  };
  EXPECT_TRUE(valid.valid());
  auto invalid = valid;
  invalid.cause_code.clear();
  EXPECT_FALSE(invalid.valid());
  invalid = valid;
  invalid.producer.clear();
  EXPECT_FALSE(invalid.valid());
  invalid = valid;
  invalid.producer = std::string(kMaximumManagerApplyFailureProducerBytes + 1U, 'p');
  EXPECT_FALSE(invalid.valid());
  invalid = valid;
  invalid.cause_details.emplace("", "empty key");
  EXPECT_FALSE(invalid.valid());
  invalid = valid;
  invalid.cause_details["long"] =
      std::string(kMaximumManagerApplyFailureCauseDetailValueBytes + 1U, 'v');
  EXPECT_FALSE(invalid.valid());
  invalid = valid;
  for (std::size_t index = 0; index < kMaximumManagerApplyFailureCauseDetails; ++index)
    invalid.cause_details["key" + std::to_string(index)] = "value";
  EXPECT_FALSE(invalid.valid());
  // A failure before any effect is a known one; an unknown effect cannot be.
  invalid = valid;
  invalid.effect_boundary = ManagerApplyEffectBoundary::kBeforeEffect;
  EXPECT_TRUE(invalid.valid());
  invalid.effect_disposition = ManagerApplyEffectDisposition::kUnknown;
  EXPECT_FALSE(invalid.valid());
  EXPECT_STREQ(ToString(ManagerApplyEffectBoundary::kBeforeEffect), "before_effect");
  EXPECT_STREQ(ToString(ManagerApplyEffectBoundary::kUnknown), "unknown");
}

TEST(ManagerApplyJournal, RetainsOneImmutableTerminalFailurePerOperation) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "terminal-failure.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;

  // Initialization failed after destructive teardown, so the effects are not
  // established and the operation must not claim clean absence.
  const auto failure = ManagerApplyFailureEvidenceDTO{
      intent.intent_digest,
      ManagerApplyPhase::kRuntimeConverged,
      "runtime_initialization_failed",
      "replacement initialization failed after teardown",
      ManagerApplyEffectDisposition::kUnknown,
      "typed_cause_code",
      {},
      "component_manager",
      ManagerApplyEffectBoundary::kUnknown,
  };
  const auto retained = journal.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kUnknownEffect,
      failure,
  });
  EXPECT_TRUE(retained.inserted);

  // Exact replay is a read of the same immutable terminal outcome.
  const auto replay = journal.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kUnknownEffect,
      failure,
  });
  EXPECT_FALSE(replay.inserted);
  EXPECT_EQ(replay.record, retained.record);

  // A different failure for the same operation identity is rejected: the
  // terminal result is retained exactly once and is never rewritten.
  auto conflicting = failure;
  conflicting.failure_message = "another failure";
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kUnknownEffect,
                   conflicting,
               }),
               ManagerApplyJournalConflictError);

  const auto status =
      journal.ReadStatus(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->terminal_outcome.has_value());
  const auto &terminal = status->terminal_outcome->terminal_outcome;
  EXPECT_FALSE(terminal.succeeded());
  EXPECT_EQ(terminal.state, ManagerApplyTerminalOutcomeState::kUnknownEffect);
  ASSERT_TRUE(terminal.failure.has_value());
  EXPECT_EQ(terminal.failure->failure_code, "runtime_initialization_failed");
  EXPECT_EQ(terminal.failure->effect_disposition,
            ManagerApplyEffectDisposition::kUnknown);
  EXPECT_EQ(terminal.failure->intent_digest, intent.intent_digest);
  EXPECT_FALSE(terminal.candidate_projection_revision.has_value());
  // A terminal failure retains the phase that actually failed.
  EXPECT_EQ(DeriveManagerApplyProgress(*status).phase,
            ManagerApplyPhase::kRuntimeConverged);
  EXPECT_EQ(DeriveManagerApplyProgress(*status).record_digest,
            status->terminal_outcome->terminal_outcome_digest);

  journal.Close();

  // The retained first failure survives a Manager restart exactly.
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto stored = reopened.ReadTerminalOutcome(ApplyId("apply-1"),
                                                   ApplyGeneration(1U));
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->terminal_outcome, retained.record.terminal_outcome);
  EXPECT_EQ(stored->terminal_outcome_digest,
            retained.record.terminal_outcome_digest);
  EXPECT_EQ(reopened.VerifyIntegrity().terminal_outcome_count, 1);
}

// UA7W.5: a direct Manager claim settles only after a known refusal at the
// accepted phase; any later or uncertain failure keeps it held until the
// cold-restart recovery (UA7W.R).
TEST(ManagerApplyJournal, SettlesADirectClaimOnlyForAKnownAcceptedPhaseRefusal) {
  const auto settlement = [](const ManagerApplyPhase phase,
                             const ManagerApplyEffectDisposition disposition,
                             const bool record_terminal) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "settlement.sqlite3";
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    ContentDigest evidence{std::string(kDigestOne)};
    if (record_terminal) {
      evidence = journal
                     .EnsureTerminalOutcome({
                         ApplyId("apply-1"),
                         ApplyGeneration(1U),
                         std::nullopt,
                         std::nullopt,
                         std::nullopt,
                         std::nullopt,
                         disposition == ManagerApplyEffectDisposition::kKnown
                             ? ManagerApplyTerminalOutcomeState::kFailed
                             : ManagerApplyTerminalOutcomeState::kUnknownEffect,
                         ManagerApplyFailureEvidenceDTO{
                             intent.intent_digest, phase, "refused",
                             "refused by the test", disposition,
                             "typed_cause_code",
                             {},
                             "component_manager",
                             ManagerApplyEffectBoundary::kUnknown},
                     })
                     .record.terminal_outcome_digest;
    }
    const core::CellAdmissionClaimDTO claim{
        {
            core::CellAdmissionClaimKindDTO::kManagerApply,
            core::CellAdmissionOwnerRoleDTO::kManager,
            intent.intent.request_context.request_owner_epoch,
            intent.intent.apply_id.value(),
            intent.intent.request_context.request_id,
            intent.intent_digest,
            *intent.intent.expected_admission_generation,
            {},
            std::nullopt,
            std::nullopt,
        },
        core::CellAdmissionClaimGeneration(3U),
        *intent.intent.expected_admission_generation,
        42,
    };
    const core::CellAdmissionClaimReleaseDTO release{
        claim.intent.owner_role,
        claim.intent.owner_epoch,
        claim.intent.owner_transaction_id,
        claim.intent.request_id,
        claim.intent.request_payload_digest,
        claim.acquired_admission_generation,
        claim.claim_generation,
        {core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
         evidence},
    };
    return ValidateManagerApplyCellAdmissionSettlementReadOnly(
               path, CellId("cell-1"), claim, release)
        .ok();
  };

  EXPECT_TRUE(settlement(ManagerApplyPhase::kAccepted,
                         ManagerApplyEffectDisposition::kKnown, true));
  EXPECT_FALSE(settlement(ManagerApplyPhase::kFenceAcquired,
                          ManagerApplyEffectDisposition::kKnown, true));
  EXPECT_FALSE(settlement(ManagerApplyPhase::kFenceAccepted,
                          ManagerApplyEffectDisposition::kKnown, true));
  EXPECT_FALSE(settlement(ManagerApplyPhase::kAccepted,
                          ManagerApplyEffectDisposition::kUnknown, true));
  EXPECT_FALSE(settlement(ManagerApplyPhase::kAccepted,
                          ManagerApplyEffectDisposition::kKnown, false));
}

// Q118: exact terminal proof is passive; a LIVE holder is never inferred dead.
TEST(ManagerApplyJournal,
     ConfigurationSuccessorRequiresExactTerminalOldClaims) {
  for (const auto disposition : {ManagerApplyEffectDisposition::kKnown,
                                 ManagerApplyEffectDisposition::kUnknown}) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "successor-proof.sqlite3";
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    const core::CellAdmissionClaimDTO claim{
        {core::CellAdmissionClaimKindDTO::kManagerApply,
         core::CellAdmissionOwnerRoleDTO::kManager,
         intent.intent.request_context.request_owner_epoch,
         intent.intent.apply_id.value(),
         intent.intent.request_context.request_id,
         intent.intent_digest,
         *intent.intent.expected_admission_generation,
         {},
         std::nullopt,
         std::nullopt,
         std::nullopt},
        core::CellAdmissionClaimGeneration(3U),
        *intent.intent.expected_admission_generation,
        42};
    const std::array claims{claim};
    EXPECT_FALSE(ValidateTerminalManagerApplyClaimsReadOnly(
                     path, CellId("cell-1"), claims)
                     .ok());
    const auto terminal =
        journal
            .EnsureTerminalOutcome(
                {ApplyId("apply-1"), ApplyGeneration(1U), std::nullopt,
                 std::nullopt, std::nullopt, std::nullopt,
                 disposition == ManagerApplyEffectDisposition::kKnown
                     ? ManagerApplyTerminalOutcomeState::kFailed
                     : ManagerApplyTerminalOutcomeState::kUnknownEffect,
                 ManagerApplyFailureEvidenceDTO{
                     intent.intent_digest, ManagerApplyPhase::kAccepted,
                     "refused", "test refusal", disposition,
                     "typed_cause_code",
                     {},
                     "component_manager",
                     ManagerApplyEffectBoundary::kUnknown}})
            .record;
    EXPECT_TRUE(ValidateTerminalManagerApplyClaimsReadOnly(
                    path, CellId("cell-1"), claims)
                    .ok());
    auto foreign = claim;
    foreign.intent.request_payload_digest =
        ContentDigest(std::string(kDigestTwo));
    EXPECT_FALSE(ValidateTerminalManagerApplyClaimsReadOnly(
                     path, CellId("cell-1"), std::array{foreign})
                     .ok());
    EXPECT_FALSE(ValidateTerminalManagerApplyClaimsReadOnly(
                     path, CellId("cell-other"), claims)
                     .ok());
    EXPECT_FALSE(ValidateTerminalManagerApplyClaimsReadOnly(
                     path, CellId("cell-1"), std::array{claim, claim})
                     .ok());
    EXPECT_EQ(
        journal.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U)),
        terminal);
    EXPECT_EQ(journal.VerifyIntegrity().terminal_outcome_count, 1);
  }
}

TEST(ManagerApplyJournal,
     ConfigurationSuccessorRequiresExactFreshSucceededChain) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "fresh-success-proof.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto transition = EnsureSelectedBundleTransition(journal);
  const auto status =
      journal.ReadStatus(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(status.has_value());
  const auto &intent = status->intent;
  const core::CellAdmissionClaimDTO claim{
      {core::CellAdmissionClaimKindDTO::kManagerApply,
       core::CellAdmissionOwnerRoleDTO::kManager,
       intent.intent.request_context.request_owner_epoch,
       intent.intent.apply_id.value(),
       intent.intent.request_context.request_id,
       intent.intent_digest,
       *intent.intent.expected_admission_generation,
       {},
       std::nullopt,
       std::nullopt,
       std::nullopt},
      core::CellAdmissionClaimGeneration(3U),
      *intent.intent.expected_admission_generation,
      42};
  core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
       ContentDigest(std::string(kDigestOne))}};
  EXPECT_FALSE(ValidateFreshManagerApplySuccessReadOnly(path, CellId("cell-1"),
                                                        claim, release)
                   .ok());
  const auto completed = EnsureApplyCompletion(journal, transition);
  release.settlement.evidence_digest =
      completed.terminal.terminal_outcome_digest;
  EXPECT_TRUE(ValidateFreshManagerApplySuccessReadOnly(path, CellId("cell-1"),
                                                       claim, release)
                  .ok());
  EXPECT_FALSE(ValidateTerminalManagerApplyClaimsReadOnly(
                   path, CellId("cell-1"), std::array{claim})
                   .ok());
  auto foreign = release;
  foreign.settlement.evidence_digest = ContentDigest(std::string(kDigestTwo));
  EXPECT_FALSE(ValidateFreshManagerApplySuccessReadOnly(path, CellId("cell-1"),
                                                        claim, foreign)
                   .ok());
  foreign = release;
  foreign.claim_generation = core::CellAdmissionClaimGeneration(4U);
  EXPECT_FALSE(ValidateFreshManagerApplySuccessReadOnly(path, CellId("cell-1"),
                                                        claim, foreign)
                   .ok());
  EXPECT_EQ(
      journal.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U)),
      completed.terminal);
  EXPECT_EQ(journal.VerifyIntegrity().terminal_outcome_count, 1);
}

TEST(ManagerApplyJournal, RejectsATerminalFailureThatIsNotBoundToItsIntent) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "failure-binding.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;

  // An unknown operation is not a durable identity.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-unknown"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       "preflight_refused",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                       "typed_cause_code",
                       {},
                       "component_manager",
                       ManagerApplyEffectBoundary::kUnknown,
                   },
               }),
               ManagerApplyJournalConflictError);

  // A failure must name the exact accepted intent of the same generation.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       ContentDigest(std::string(kDigestThree)),
                       ManagerApplyPhase::kAccepted,
                       "preflight_refused",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                       "typed_cause_code",
                       {},
                       "component_manager",
                       ManagerApplyEffectBoundary::kUnknown,
                   },
               }),
               ManagerApplyJournalConflictError);

  // A mismatched state/disposition pair is rejected before any write.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       "preflight_refused",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kUnknown,
                       "typed_cause_code",
                       {},
                       "component_manager",
                       ManagerApplyEffectBoundary::kUnknown,
                   },
               }),
               std::invalid_argument);

  // Empty and oversized retained failure text is rejected before any write.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       "",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                       "typed_cause_code",
                       {},
                       "component_manager",
                       ManagerApplyEffectBoundary::kUnknown,
                   },
               }),
               std::invalid_argument);
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       std::string(kMaximumManagerApplyFailureCodeBytes + 1U, 'c'),
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                       "typed_cause_code",
                       {},
                       "component_manager",
                       ManagerApplyEffectBoundary::kUnknown,
                   },
               }),
               std::invalid_argument);

  // None of the refusals left a terminal record behind.
  EXPECT_FALSE(journal.ReadTerminalOutcome(ApplyId("apply-1"),
                                           ApplyGeneration(1U))
                   .has_value());
}

TEST(ManagerApplyJournalValues,
     EnforcesEveryTextBoundaryAndSignedCounterBoundary) {
  TemporaryDirectory directory;
  using Setter =
      std::function<void(ManagerApplyIntentDTO &, const std::string &)>;
  const std::vector<std::pair<std::string, Setter>> identities{
      {"request_id",
       [](auto &value, const auto &text) {
         value.request_context.request_id = RequestId(text);
       }},
      {"request_owner_epoch",
       [](auto &value, const auto &text) {
         value.request_context.request_owner_epoch = RequestOwnerEpoch(text);
       }},
      {"apply_id",
       [](auto &value, const auto &text) { value.apply_id = ApplyId(text); }},
      {"predecessor_bundle",
       [](auto &value, const auto &text) {
         value.expected_active_projection_revision.retained_bundle_id =
             RetainedBundleId(text);
       }},
      {"predecessor_graph",
       [](auto &value, const auto &text) {
         value.expected_active_projection_revision.graph_revision =
             GraphRevision(text);
       }},
      {"candidate_bundle",
       [](auto &value, const auto &text) {
         value.candidate_retained_bundle_id = RetainedBundleId(text);
       }},
      {"candidate_graph",
       [](auto &value, const auto &text) {
         value.candidate_graph_revision = GraphRevision(text);
       }},
  };

  for (const auto &[label, set_value] : identities) {
    SCOPED_TRACE(label);
    auto exact = Intent();
    set_value(exact,
              std::string(kMaximumManagerApplyJournalIdentifierBytes, 'a'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-exact.sqlite3"), CellId("cell-1"));
      EXPECT_TRUE(journal.EnsureIntent(exact).inserted);
    }

    auto excessive = Intent();
    set_value(
        excessive,
        std::string(kMaximumManagerApplyJournalIdentifierBytes + 1U, 'a'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-excessive.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(excessive), std::invalid_argument);
    }

    auto control = Intent();
    set_value(control, std::string("a\0b", 3U));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-control.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(control), std::invalid_argument);
    }
  }

  {
    auto exact_scene = Intent();
    exact_scene.expected_active_projection_revision.scene_revision.scene_id =
        SceneId(std::string(kSceneIdTwo));
    SqliteManagerApplyJournal journal(
        directory.path() / "predecessor-scene-exact.sqlite3", CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(exact_scene).inserted);
  }
  for (const auto &invalid_scene : {
           std::string(kMaximumManagerApplyJournalIdentifierBytes, 'a'),
           std::string("sha256:a\0b", 10U),
       }) {
    auto intent = Intent();
    intent.expected_active_projection_revision.scene_revision.scene_id =
        SceneId(invalid_scene);
    SqliteManagerApplyJournal journal(
        directory.path() / (invalid_scene.find('\0') == std::string::npos
                                ? "predecessor-scene-malformed.sqlite3"
                                : "predecessor-scene-control.sqlite3"),
        CellId("cell-1"));
    EXPECT_THROW(journal.EnsureIntent(intent), std::invalid_argument);
  }

  const auto maximum_identity =
      std::string(kMaximumManagerApplyJournalIdentifierBytes, 'c');
  EXPECT_NO_THROW({
    SqliteManagerApplyJournal journal(directory.path() / "cell-exact.sqlite3",
                                      CellId(maximum_identity));
    auto intent = Intent();
    intent.cell_id = CellId(maximum_identity);
    EXPECT_TRUE(journal.EnsureIntent(intent).inserted);
  });
  EXPECT_THROW(SqliteManagerApplyJournal(
                   directory.path() / "cell-excessive.sqlite3",
                   CellId(std::string(
                       kMaximumManagerApplyJournalIdentifierBytes + 1U, 'c'))),
               std::invalid_argument);
  EXPECT_THROW(
      SqliteManagerApplyJournal(directory.path() / "cell-control.sqlite3",
                                CellId(std::string("c\0d", 3U))),
      std::invalid_argument);

  for (const auto &[label, maximum, set_value] :
       std::vector<std::tuple<std::string, std::size_t, Setter>>{
           {"source_label", kMaximumManagerApplySourceLabelBytes,
            [](auto &value, const auto &text) {
              value.request_context.source_label = text;
            }},
           {"correlation_id", kMaximumManagerApplyCorrelationIdBytes,
            [](auto &value, const auto &text) {
              value.request_context.correlation_id = text;
            }},
       }) {
    SCOPED_TRACE(label);
    auto exact = Intent();
    set_value(exact, std::string(maximum, 't'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-exact.sqlite3"), CellId("cell-1"));
      EXPECT_TRUE(journal.EnsureIntent(exact).inserted);
    }
    auto excessive = Intent();
    set_value(excessive, std::string(maximum + 1U, 't'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-excessive.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(excessive), std::invalid_argument);
    }
    auto control = Intent();
    set_value(control, std::string("t\nx"));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-control.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(control), std::invalid_argument);
    }
  }

  auto excessive_generation = Intent();
  excessive_generation.apply_generation = ApplyGeneration(
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
      1U);
  {
    SqliteManagerApplyJournal journal(
        directory.path() / "generation-excessive.sqlite3", CellId("cell-1"));
    EXPECT_THROW(journal.EnsureIntent(excessive_generation),
                 std::invalid_argument);
  }
  auto maximum_scene_revision = Intent();
  maximum_scene_revision.expected_active_projection_revision.scene_revision
      .revision_id =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  {
    SqliteManagerApplyJournal journal(
        directory.path() / "scene-revision-exact.sqlite3", CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(maximum_scene_revision).inserted);
  }
  auto excessive_scene_revision = Intent();
  excessive_scene_revision.expected_active_projection_revision.scene_revision
      .revision_id =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
  {
    SqliteManagerApplyJournal journal(directory.path() /
                                          "scene-revision-excessive.sqlite3",
                                      CellId("cell-1"));
    EXPECT_THROW(journal.EnsureIntent(excessive_scene_revision),
                 std::invalid_argument);
  }
}

TEST(ManagerApplyJournal,
     SceneInitializationPurposeIsDurableAndChangesIntentDigest) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "ordinary-digest.sqlite3";
  const auto intent = Intent();
  auto initializing = intent;
  initializing.scene_initialization = ManagerSceneInitialization::kInitialize;
  const auto original_digest = ComputeManagerApplyIntentDigest(intent);
  EXPECT_NE(ComputeManagerApplyIntentDigest(initializing), original_digest);
  EXPECT_EQ(ComputeManagerApplyIntentDigest(intent), original_digest);
  {
    SqliteManagerApplyJournal journal(path, intent.cell_id);
    const auto accepted = journal.EnsureIntent(intent);
    EXPECT_TRUE(accepted.inserted);
    EXPECT_EQ(accepted.record.intent_digest, original_digest);
    EXPECT_FALSE(journal.EnsureIntent(intent).inserted);
    EXPECT_THROW(journal.EnsureIntent(initializing),
                 ManagerApplyJournalConflictError);
  }
  SqliteManagerApplyJournal reopened(path, intent.cell_id);
  const auto stored = reopened.ReadIntent(intent.apply_id, intent.apply_generation);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->intent, intent);
  EXPECT_EQ(stored->intent_digest, original_digest);
  EXPECT_EQ(reopened.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournalIntegrity,
     IntentDigestIsSensitiveToEveryPersistedField) {
  TemporaryDirectory directory;
  const auto base = Intent();
  std::vector<ManagerApplyIntentDTO> variants{base};
  const auto add = [&](const auto &mutate) {
    auto value = base;
    mutate(value);
    variants.push_back(std::move(value));
  };
  add([](auto &value) {
    value.request_context.request_id = RequestId("request-2");
  });
  add([](auto &value) {
    value.request_context.request_owner_epoch =
        RequestOwnerEpoch("owner-epoch-2");
  });
  add([](auto &value) {
    value.request_context.source_label = "another-source";
  });
  add([](auto &value) { value.request_context.correlation_id.reset(); });
  add([](auto &value) {
    value.request_context.correlation_id = "correlation-2";
  });
  add([](auto &value) { value.apply_id = ApplyId("apply-2"); });
  add([](auto &value) { value.apply_generation = ApplyGeneration(2U); });
  add([](auto &value) { value.cell_id = CellId("cell-2"); });
  add([](auto &value) {
    value.expected_active_projection_revision.retained_bundle_id =
        RetainedBundleId("predecessor-bundle-2");
  });
  add([](auto &value) {
    value.expected_active_projection_revision.graph_revision =
        GraphRevision("predecessor-graph-2");
  });
  add([](auto &value) {
    value.expected_active_projection_revision.scene_revision.scene_id =
        SceneId(std::string(kSceneIdTwo));
  });
  add([](auto &value) {
    value.expected_active_projection_revision.scene_revision.revision_id = 8U;
  });
  add([](auto &value) {
    value.expected_active_projection_revision.scene_revision.content_hash =
        ContentDigest(std::string(kDigestTwo));
  });
  add([](auto &value) {
    value.candidate_retained_bundle_id = RetainedBundleId("candidate-bundle-2");
  });
  add([](auto &value) {
    value.candidate_graph_revision = GraphRevision("candidate-graph-2");
  });
  add([](auto &value) {
    value.prepared_activation.captured_input_digest =
        ContentDigest(std::string(kDigestTwo));
  });
  add([](auto &value) {
    value.prepared_activation.deployment_artifact_id =
        core::DeploymentArtifactId("deployment-artifact-2");
  });
  add([](auto &value) {
    value.prepared_activation.deployment_revision =
        core::DeploymentRevision(std::string(kDigestTwo));
  });
  add([](auto &value) {
    value.prepared_activation.program_binding_id =
        core::ProgramBindingId(std::string(kDigestThree));
  });
  // A non-executable Program outcome is its own durable identity (H1a).
  add([](auto &value) {
    value.prepared_activation.program_binding_id = std::nullopt;
  });
  add([](auto &value) {
    value.prepared_activation.prepared_activation_id =
        ContentDigest(std::string(kDigestTwo));
  });

  std::set<std::string> digests;
  for (std::size_t index = 0U; index < variants.size(); ++index) {
    const auto &intent = variants[index];
    SqliteManagerApplyJournal journal(
        directory.path() / ("digest-" + std::to_string(index) + ".sqlite3"),
        intent.cell_id);
    if (intent.apply_generation.value() == 2U) {
      EXPECT_TRUE(journal
                      .EnsureIntent(Intent(intent.apply_id.value(), 1U,
                                           "generation-predecessor-request",
                                           "generation-predecessor-owner"))
                      .inserted);
    }
    const auto result = journal.EnsureIntent(intent);
    ASSERT_TRUE(result.inserted);
    EXPECT_TRUE(digests.insert(result.record.intent_digest.value()).second);
  }
  EXPECT_EQ(digests.size(), variants.size());
}

TEST(ManagerApplyJournal,
     PersistsANonExecutableProgramIntentWithoutABindingAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  auto intent = Intent();
  intent.prepared_activation.program_binding_id = std::nullopt;
  ASSERT_TRUE(intent.valid());
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto inserted = journal.EnsureIntent(intent);
    EXPECT_TRUE(inserted.inserted);
    EXPECT_EQ(inserted.record.intent, intent);
    // The same Apply with a bound Program is a different, conflicting intent.
    EXPECT_THROW(static_cast<void>(journal.EnsureIntent(Intent())),
                 ManagerApplyJournalConflictError);
  }
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto read = reopened.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(read.has_value());
  EXPECT_FALSE(read->intent.prepared_activation.program_binding_id.has_value());
  EXPECT_EQ(read->intent, intent);
}

TEST(ManagerApplyJournal, PersistsExactIntentAndReplaysAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  const auto intent = Intent();
  ContentDigest digest("unset");
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto inserted = journal.EnsureIntent(intent);
    EXPECT_TRUE(inserted.inserted);
    EXPECT_EQ(inserted.record.intent, intent);
    EXPECT_TRUE(inserted.record.intent_digest.value().starts_with("sha256:"));
    digest = inserted.record.intent_digest;

    const auto replay = journal.EnsureIntent(intent);
    EXPECT_FALSE(replay.inserted);
    EXPECT_EQ(replay.record, inserted.record);
    ASSERT_TRUE(journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U))
                    .has_value());
    EXPECT_EQ(journal
                  .ReadRequest(RequestOwnerEpoch("owner-epoch-1"),
                               RequestId("request-1"))
                  ->intent,
              intent);
    EXPECT_EQ(journal.ReadApplyGenerationHighWater(ApplyId("apply-1")),
              ApplyGeneration(1U));
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto record =
      reopened.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->intent, intent);
  EXPECT_EQ(record->intent_digest, digest);
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.cell_id, CellId("cell-1"));
  EXPECT_EQ(report.apply_intent_count, 1);
  EXPECT_EQ(report.distinct_apply_id_count, 1);
  EXPECT_EQ(report.distinct_request_identity_count, 1);
}

TEST(ManagerApplyJournal, PersistsExactParentAuthoringClaimAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "parent-claim-apply.sqlite3";
  const auto intent = ParentClaimIntent();
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto inserted = journal.EnsureIntent(intent);
    ASSERT_TRUE(inserted.inserted);
    EXPECT_EQ(inserted.record.intent, intent);
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto record =
      reopened.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->intent, intent);
  EXPECT_EQ(reopened.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal,
     AuthoringOwnerRotationSettlesOnlyClaimsWithoutPendingManagerEffects) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "authoring-owner-rotation.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto claim = *ParentClaimIntent().parent_authoring_claim;
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> current{
      {core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
       RequestOwnerEpoch("composer-owner-1")}};
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> successor{
      {core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
       RequestOwnerEpoch("composer-owner-2")}};
  const auto evidence =
      core::CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          RequestOwnerEpoch("composer-owner-2"));
  ASSERT_TRUE(evidence.ok());
  const core::CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel-owner-1"), {claim}, successor,
      evidence.value()};

  const auto local = ValidateManagerAuthoringOwnerReconciliationReadOnly(
      path, CellId("cell-1"), {claim}, current, reconcile);
  ASSERT_TRUE(local.ok());
  ASSERT_EQ(local.value().settlements.size(), 1U);
  EXPECT_EQ(local.value().settlements[0].predecessor_claim, claim);
  EXPECT_EQ(local.value().settlements[0].successor_owner_epoch,
            RequestOwnerEpoch("composer-owner-2"));

  static_cast<void>(journal.EnsureIntent(ParentClaimIntent()));
  const auto pending = ValidateManagerAuthoringOwnerReconciliationReadOnly(
      path, CellId("cell-1"), {claim}, current, reconcile);
  EXPECT_FALSE(pending.ok());

  const auto durable_intent =
      journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(durable_intent.has_value());
  static_cast<void>(journal.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kFailed,
      ManagerApplyFailureEvidenceDTO{
          durable_intent->intent_digest,
          ManagerApplyPhase::kAccepted,
          "maintenance_preflight_refused",
          "maintenance was refused before any runtime effect",
          ManagerApplyEffectDisposition::kKnown,
          "typed_cause_code",
          {},
          "component_manager",
          ManagerApplyEffectBoundary::kUnknown},
  }));
  const auto refused_before_effect =
      ValidateManagerAuthoringOwnerReconciliationReadOnly(
          path, CellId("cell-1"), {claim}, current, reconcile);
  EXPECT_TRUE(refused_before_effect.ok());

  const auto destructive_path =
      directory.path() / "authoring-owner-destructive-failure.sqlite3";
  SqliteManagerApplyJournal destructive(destructive_path, CellId("cell-1"));
  const auto destructive_intent =
      destructive.EnsureIntent(ParentClaimIntent()).record;
  static_cast<void>(destructive.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kFailed,
      ManagerApplyFailureEvidenceDTO{
          destructive_intent.intent_digest,
          ManagerApplyPhase::kRuntimeConverged,
          "runtime_initialization_failed",
          "replacement initialization failed after teardown",
          ManagerApplyEffectDisposition::kKnown,
          "typed_cause_code",
          {},
          "component_manager",
          ManagerApplyEffectBoundary::kUnknown},
  }));
  const auto destructive_failure =
      ValidateManagerAuthoringOwnerReconciliationReadOnly(
          destructive_path, CellId("cell-1"), {claim}, current, reconcile);
  EXPECT_FALSE(destructive_failure.ok());
}

TEST(ManagerApplyJournal,
     ReadsOneTransactionallyConsistentStatusForEveryDurableIntent) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  EXPECT_TRUE(journal.ReadStatusInventory().empty());

  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto scope =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope()).record;
  const auto records = journal.ReadStatusInventory();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].intent, intent);
  ASSERT_TRUE(records[0].affected_scope.has_value());
  EXPECT_EQ(records[0].affected_scope->affected_scope_digest,
            scope.affected_scope_digest);
  EXPECT_FALSE(records[0].fence_attempt.has_value());
  EXPECT_FALSE(records[0].fence_acceptance.has_value());
  EXPECT_FALSE(records[0].runtime_convergence_intent.has_value());
  EXPECT_FALSE(records[0].latest_runtime_staged_ready.has_value());
  EXPECT_FALSE(records[0].private_materialization.has_value());
  EXPECT_FALSE(records[0].publication_receipt.has_value());
  EXPECT_FALSE(records[0].fence_release_intent.has_value());
  EXPECT_FALSE(records[0].fence_released.has_value());
  EXPECT_FALSE(records[0].terminal_outcome.has_value());
}

TEST(ManagerApplyJournal,
     ReadsAreReverifiedOnlyAfterAnOwnOrForeignCommit) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto first = journal.ReadStatusInventory();
  ASSERT_EQ(first.size(), 1U);
  const auto verified = journal.record_verification_count();

  // Unchanged database: the verified inventory is reused, not re-verified.
  for (int read = 0; read < 2; ++read) {
    const auto reused = journal.ReadStatusInventory();
    ASSERT_EQ(reused.size(), 1U);
    EXPECT_EQ(reused[0].intent, intent);
    EXPECT_FALSE(reused[0].affected_scope.has_value());
  }
  EXPECT_EQ(journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U)),
            intent);
  EXPECT_FALSE(
      journal.ReadAffectedScope(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(journal.record_verification_count(), verified);

  // An own write is visible and re-verified on the very next read.
  const auto scope =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope()).record;
  const auto with_scope = journal.ReadStatusInventory();
  EXPECT_GT(journal.record_verification_count(), verified);
  ASSERT_EQ(with_scope.size(), 1U);
  ASSERT_TRUE(with_scope[0].affected_scope.has_value());
  EXPECT_EQ(with_scope[0].affected_scope->affected_scope_digest,
            scope.affected_scope_digest);

  // A foreign commit that corrupts a verified record is detected on the very
  // next read, and no stale verified inventory survives the failure.
  const auto rewrite_source = [&](const std::string &source) {
    ExecSql(
        path,
        "DROP TRIGGER manager_apply_intents_no_update;"
        "UPDATE manager_apply_intents SET source_label='" +
            source +
            "';"
            "CREATE TRIGGER manager_apply_intents_no_update "
            "BEFORE UPDATE ON manager_apply_intents BEGIN "
            "SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); END;");
  };
  rewrite_source("tampered-source");
  EXPECT_THROW(static_cast<void>(journal.ReadStatusInventory()),
               ManagerApplyJournalIntegrityError);
  EXPECT_THROW(static_cast<void>(journal.ReadStatusInventory()),
               ManagerApplyJournalIntegrityError);
  rewrite_source(Intent().request_context.source_label);
  const auto restored = journal.ReadStatusInventory();
  ASSERT_EQ(restored.size(), 1U);
  EXPECT_EQ(restored[0].intent, intent);
  ASSERT_TRUE(restored[0].affected_scope.has_value());
  EXPECT_EQ(restored[0].affected_scope->affected_scope_digest,
            scope.affected_scope_digest);
}

TEST(ManagerApplyJournal, CommittedChildWriteSurvivesProcessExit) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  { SqliteManagerApplyJournal initialized(path, CellId("cell-1")); }
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      SqliteManagerApplyJournal journal(path, CellId("cell-1"));
      static_cast<void>(journal.EnsureIntent(Intent()));
      _exit(0);
    } catch (...) {
      _exit(71);
    }
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto durable =
      reopened.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->intent, Intent());
}

TEST(ManagerApplyJournal, UncommittedChildWriteDisappearsOnRecovery) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  { SqliteManagerApplyJournal initialized(path, CellId("cell-1")); }
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    sqlite3 *database = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      _exit(72);
    }
    const std::string sql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO manager_apply_intents ("
        "apply_id,apply_generation,request_id,request_owner_epoch,"
        "source_label,correlation_id,predecessor_retained_bundle_id,"
        "predecessor_graph_revision,predecessor_scene_id,"
        "predecessor_scene_revision_id,predecessor_scene_content_hash,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "captured_input_digest,deployment_artifact_id,deployment_revision,"
        "program_binding_id,prepared_activation_id,"
        "prepared_activation_receipt_json,"
        "expected_admission_generation,parent_authoring_claim,scene_"
        "initialization,intent_digest"
        ") VALUES ("
        "'apply-uncommitted',1,'request-uncommitted','owner-epoch-1',"
        "'child',NULL,'predecessor-bundle','predecessor-graph','" +
        std::string(kSceneIdOne) +
        "',"
        "7,'" +
        std::string(kDigestOne) +
        "','candidate-bundle','candidate-graph',"
        "'sha256:14141414141414141414141414141414141414141414141414141414"
        "14141414','deployment-artifact-1',"
        "'sha256:15151515151515151515151515151515151515151515151515151515"
        "15151515','sha256:"
        "17171717171717171717171717171717171717171717171717171717"
        "17171717',"
        "'sha256:16161616161616161616161616161616161616161616161616161616"
        "16161616','{\"schema_version\":\"test.prepared_activation_receipt."
        "v1\"}',"
        "7,NULL,'preserve','sha256:"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'"
        ");";
    if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      sqlite3_close(database);
      _exit(73);
    }
    _exit(0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_FALSE(
      reopened.ReadIntent(ApplyId("apply-uncommitted"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(reopened.VerifyIntegrity().apply_intent_count, 0);
}

TEST(ManagerApplyJournal, RequiresContiguousGenerationPerApplyIdentity) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));

  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-1", 2U, "request-2", "owner-epoch-1")),
      ManagerApplyJournalConflictError);
  EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);
  EXPECT_TRUE(journal
                  .EnsureIntent(Intent("apply-1", 2U, "request-2",
                                       "owner-epoch-1", "candidate-bundle-2"))
                  .inserted);
  EXPECT_EQ(journal.ReadApplyGenerationHighWater(ApplyId("apply-1")),
            ApplyGeneration(2U));
  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-1", 4U, "request-4", "owner-epoch-1")),
      ManagerApplyJournalConflictError);

  EXPECT_TRUE(journal
                  .EnsureIntent(Intent("apply-2", 1U, "request-3",
                                       "owner-epoch-1", "candidate-bundle-3"))
                  .inserted);
  const auto report = journal.VerifyIntegrity();
  EXPECT_EQ(report.apply_intent_count, 3);
  EXPECT_EQ(report.distinct_apply_id_count, 2);
}

TEST(ManagerApplyJournal, PublicRequestIdentityCannotNameAnotherIntent) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);

  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-2", 1U, "request-1", "owner-epoch-1",
                                  "candidate-bundle-2")),
      ManagerApplyJournalConflictError);
  EXPECT_FALSE(
      journal.ReadIntent(ApplyId("apply-2"), ApplyGeneration(1U)).has_value());

  // The same request ID in a new request-owner epoch is a new identity.
  EXPECT_TRUE(journal
                  .EnsureIntent(Intent("apply-2", 1U, "request-1",
                                       "owner-epoch-2", "candidate-bundle-2"))
                  .inserted);
}

TEST(ManagerApplyJournal, ApplyIdentityCannotAcquireConflictingPayload) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);

  auto conflicting = Intent();
  conflicting.candidate_retained_bundle_id =
      RetainedBundleId("different-candidate");
  EXPECT_THROW(journal.EnsureIntent(conflicting),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal, RejectsForeignCellAndMalformedPersistedValues) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));

  auto foreign = Intent();
  foreign.cell_id = CellId("cell-2");
  EXPECT_THROW(journal.EnsureIntent(foreign), ManagerApplyJournalConflictError);

  auto malformed = Intent();
  malformed.expected_active_projection_revision.scene_revision.content_hash =
      ContentDigest("not-a-digest");
  EXPECT_THROW(journal.EnsureIntent(malformed), std::invalid_argument);
  journal.Close();
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-2")),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyJournal, ExactConcurrentDuplicateHasOneDurableEffect) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal first(path, CellId("cell-1"));
  SqliteManagerApplyJournal second(path, CellId("cell-1"));
  const auto intent = Intent();
  std::barrier start(2);
  auto write = [&](SqliteManagerApplyJournal &journal) {
    start.arrive_and_wait();
    return journal.EnsureIntent(intent);
  };
  auto first_result = std::async(std::launch::async, write, std::ref(first));
  auto second_result = std::async(std::launch::async, write, std::ref(second));
  const auto one = first_result.get();
  const auto two = second_result.get();
  EXPECT_NE(one.inserted, two.inserted);
  EXPECT_EQ(one.record, two.record);
  EXPECT_EQ(first.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal, ConcurrentFirstOpenConvergesOneExactSchema) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  std::barrier open_together(2);
  const auto open = [&, start](const bool explicit_close) {
    start.get();
    open_together.arrive_and_wait();
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto report = journal.VerifyIntegrity();
    if (explicit_close) {
      journal.Close();
    }
    return report;
  };
  auto first = std::async(std::launch::async, open, true);
  std::future<ManagerApplyJournalIntegrityReport> second;
  try {
    second = std::async(std::launch::async, open, false);
  } catch (...) {
    const auto launch_error = std::current_exception();
    start_promise.set_exception(launch_error);
    try {
      static_cast<void>(first.get());
    } catch (...) {
    }
    std::rethrow_exception(launch_error);
  }
  start_promise.set_value();
  EXPECT_EQ(first.get().apply_intent_count, 0);
  EXPECT_EQ(second.get().apply_intent_count, 0);
  ExpectNoSqliteSidecars(path);

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(reopened.VerifyIntegrity().apply_intent_count, 0);
}

TEST(ManagerApplyJournal, ConcurrentConflictsDoNotCreateMixedIntent) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal first(path, CellId("cell-1"));
  SqliteManagerApplyJournal second(path, CellId("cell-1"));
  auto one = Intent();
  auto two = Intent();
  two.candidate_retained_bundle_id = RetainedBundleId("candidate-bundle-2");
  std::barrier start(2);
  auto write = [&](SqliteManagerApplyJournal &journal,
                   const ManagerApplyIntentDTO &intent) {
    start.arrive_and_wait();
    try {
      return std::optional<ManagerApplyIntentDTO>(
          journal.EnsureIntent(intent).record.intent);
    } catch (const ManagerApplyJournalConflictError &) {
      return std::optional<ManagerApplyIntentDTO>{};
    }
  };
  auto first_result =
      std::async(std::launch::async, write, std::ref(first), std::cref(one));
  auto second_result =
      std::async(std::launch::async, write, std::ref(second), std::cref(two));
  const auto stored_one = first_result.get();
  const auto stored_two = second_result.get();
  EXPECT_NE(stored_one.has_value(), stored_two.has_value());
  const auto durable =
      first.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(durable.has_value());
  EXPECT_TRUE(durable->intent == one || durable->intent == two);
  EXPECT_EQ(first.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal, DetectsPayloadTamperingWithRestoredSchemaShape) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);
  }
  ExecSql(path,
          "DROP TRIGGER manager_apply_intents_no_update;"
          "UPDATE manager_apply_intents "
          "SET predecessor_scene_content_hash='" +
              std::string(kDigestTwo) +
              "';"
              "CREATE TRIGGER manager_apply_intents_no_update "
              "BEFORE UPDATE ON manager_apply_intents "
              "BEGIN "
              "SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); "
              "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyJournal, DetectsUnexpectedSchemaObjects) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  { SqliteManagerApplyJournal journal(path, CellId("cell-1")); }
  ExecSql(path, "CREATE TABLE unexpected(value TEXT) STRICT;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyJournalIntegrity, DetectsRawGenerationDeletionOnReopen) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);
    EXPECT_TRUE(journal
                    .EnsureIntent(Intent("apply-1", 2U, "request-2",
                                         "owner-epoch-1", "candidate-bundle-2"))
                    .inserted);
  }
  ExecSql(path, "DROP TRIGGER manager_apply_intents_no_delete;"
                "DELETE FROM manager_apply_intents "
                "WHERE apply_id='apply-1' AND apply_generation=1;"
                "CREATE TRIGGER manager_apply_intents_no_delete "
                "BEFORE DELETE ON manager_apply_intents "
                "BEGIN "
                "SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); "
                "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyJournalIntegrity,
     RejectsOversizedPreexistingDatabaseBeforeOpeningIt) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "oversized.sqlite3";
  {
    std::ofstream stream(path);
    ASSERT_TRUE(stream);
  }
  std::filesystem::resize_file(
      path, static_cast<std::uintmax_t>(
                kMaximumManagerApplyJournalDatabasePageImageBytes) +
                1U);
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalLimitError);
}

TEST(ManagerApplyJournal, RejectsSymlinkAndClosedUse) {
  TemporaryDirectory directory;
  const auto real = directory.path() / "real.sqlite3";
  const auto link = directory.path() / "link.sqlite3";
  {
    SqliteManagerApplyJournal journal(real, CellId("cell-1"));
    journal.Close();
    EXPECT_THROW(journal.EnsureIntent(Intent()),
                 ManagerApplyJournalClosedError);
    EXPECT_THROW(journal.VerifyIntegrity(), ManagerApplyJournalClosedError);
  }
  std::filesystem::create_symlink(real, link);
  EXPECT_THROW(SqliteManagerApplyJournal(link, CellId("cell-1")),
               std::invalid_argument);
}

TEST(ManagerApplyJournal, IntentInventoryIsExhaustionBounded) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  std::optional<PersistedManagerApplyIntent> first_record;
  for (std::size_t index = 0U; index < kMaximumManagerApplyIntents; ++index) {
    const auto suffix = std::to_string(index);
    const auto result =
        journal.EnsureIntent(Intent("apply-" + suffix, 1U, "request-" + suffix,
                                    "owner-epoch-1", "candidate-" + suffix));
    EXPECT_TRUE(result.inserted);
    if (index == 0U) {
      first_record = result.record;
    }
  }
  EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count,
            static_cast<std::int64_t>(kMaximumManagerApplyIntents));
  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-overflow", 1U, "request-overflow",
                                  "owner-epoch-1", "candidate-overflow")),
      ManagerApplyJournalLimitError);
  ASSERT_TRUE(first_record.has_value());
  const auto replay = journal.EnsureIntent(
      Intent("apply-0", 1U, "request-0", "owner-epoch-1", "candidate-0"));
  EXPECT_FALSE(replay.inserted);
  EXPECT_EQ(replay.record, *first_record);
  EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count,
            static_cast<std::int64_t>(kMaximumManagerApplyIntents));
}

TEST(ManagerApplyFenceJournal,
     PersistsAttemptAndAcceptanceExactlyAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  ContentDigest intent_digest("unset");
  ContentDigest attempt_digest("unset");
  ContentDigest acceptance_digest("unset");
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    intent_digest = journal.EnsureIntent(Intent()).record.intent_digest;
    const auto attempt = EnsureAttempt(journal, intent_digest);
    EXPECT_TRUE(attempt.inserted);
    EXPECT_EQ(attempt.record.intent_digest, intent_digest);
    EXPECT_TRUE(SameFenceIntent(attempt.record.fence_intent, FenceIntent()));
    attempt_digest = attempt.record.fence_attempt_digest;
    const auto attempt_replay = EnsureAttempt(journal, intent_digest);
    EXPECT_FALSE(attempt_replay.inserted);
    EXPECT_EQ(attempt_replay.record.fence_attempt_digest, attempt_digest);

    const auto accepted =
        journal.EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                      attempt_digest, AcceptedFence());
    EXPECT_TRUE(accepted.inserted);
    EXPECT_TRUE(
        SameAcceptedFence(accepted.record.accepted_fence, AcceptedFence()));
    acceptance_digest = accepted.record.acceptance_digest;
    const auto accepted_replay =
        journal.EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                      attempt_digest, AcceptedFence());
    EXPECT_FALSE(accepted_replay.inserted);
    EXPECT_EQ(accepted_replay.record.acceptance_digest, acceptance_digest);
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto attempt =
      reopened.ReadFenceAttempt(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(attempt.has_value());
  EXPECT_EQ(attempt->intent_digest, intent_digest);
  EXPECT_EQ(attempt->fence_attempt_digest, attempt_digest);
  EXPECT_TRUE(SameFenceIntent(attempt->fence_intent, FenceIntent()));
  const auto accepted =
      reopened.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(accepted->fence_attempt_digest, attempt_digest);
  EXPECT_EQ(accepted->acceptance_digest, acceptance_digest);
  EXPECT_TRUE(SameAcceptedFence(accepted->accepted_fence, AcceptedFence()));
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.affected_scope_count, 1);
  EXPECT_EQ(report.fence_attempt_count, 1);
  EXPECT_EQ(report.fence_acceptance_count, 1);
}

TEST(ManagerApplyJournalReadOnly, ProofLookupsDoNotGrowWithUnrelatedHistory) {
  const auto measure = [](const int unrelated_count) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "apply.sqlite3";
    const auto target = Intent();
    SqliteManagerApplyJournal journal(path, target.cell_id);
    const auto accepted = journal.EnsureIntent(target).record;
    for (int i = 0; i < unrelated_count; ++i) {
      const auto suffix = std::to_string(i);
      static_cast<void>(journal.EnsureIntent(
          Intent("other-" + suffix, 1U, "other-request-" + suffix)));
    }
    const core::NamedApplyPublicationTransactionDTO apply{
        target.apply_id, target.apply_generation};
    std::uint64_t lookups = 0U;
    const auto proof = ReadManagerApplyStatusReadOnlyForTesting(
        path, target.cell_id, apply, &lookups);
    EXPECT_TRUE(proof.ok());
    if (proof.ok()) {
      EXPECT_EQ(proof.value().intent, accepted);
    }
    return lookups;
  };
  const auto short_history = measure(0);
  const auto long_history = measure(32);
  EXPECT_GT(short_history, 0U);
  EXPECT_EQ(long_history, short_history);
}

TEST(ManagerApplyJournal, TouchedWriteAndReadDecodeBoundedHistory) {
  const auto measure = [](const int unrelated_count) {
    TemporaryDirectory directory;
    SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                      CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    for (int i = 0; i < unrelated_count; ++i) {
      const auto suffix = std::to_string(i);
      static_cast<void>(journal.EnsureIntent(
          Intent("other-" + suffix, 1U, "other-request-" + suffix)));
    }
    const auto before_write = journal.record_verification_count();
    const auto scope = journal.EnsureAffectedScope(intent.intent_digest,
                                                    AffectedScope());
    EXPECT_TRUE(scope.inserted);
    const auto after_write = journal.record_verification_count();
    const auto status = journal.ReadStatus(intent.intent.apply_id,
                                           intent.intent.apply_generation);
    EXPECT_TRUE(status.has_value());
    if (status.has_value()) {
      EXPECT_TRUE(status->affected_scope.has_value());
    }
    const auto after_read = journal.record_verification_count();
    return std::pair{after_write - before_write, after_read - after_write};
  };
  const auto short_history = measure(0);
  const auto long_history = measure(32);
  EXPECT_GT(short_history.first, 0U);
  EXPECT_GT(short_history.second, 0U);
  EXPECT_EQ(long_history.first, short_history.first);
  EXPECT_EQ(long_history.second, short_history.second);
}

TEST(ManagerApplyJournal, TamperedTouchedIntentRefusesScopeAndRollsBack) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto rewrite = [&](const std::string &source) {
    ExecSql(path,
            "DROP TRIGGER manager_apply_intents_no_update;"
            "UPDATE manager_apply_intents SET source_label='" + source +
                "' WHERE apply_id='apply-1';"
                "CREATE TRIGGER manager_apply_intents_no_update BEFORE UPDATE ON "
                "manager_apply_intents BEGIN SELECT RAISE(ABORT, "
                "'manager_apply_intents is immutable'); END;");
  };
  rewrite("tampered-source");
  EXPECT_THROW(static_cast<void>(journal.EnsureAffectedScope(
                   intent.intent_digest, AffectedScope())),
               ManagerApplyJournalIntegrityError);
  rewrite(Intent().request_context.source_label);
  EXPECT_EQ(journal.VerifyIntegrity().affected_scope_count, 0U);
  EXPECT_TRUE(journal.EnsureAffectedScope(intent.intent_digest, AffectedScope())
                  .inserted);
}

TEST(ManagerApplyJournal, CorruptPreviousGenerationRefusesChildWrite) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  static_cast<void>(journal.EnsureIntent(Intent()));
  const auto current = journal.EnsureIntent(
      Intent("apply-1", 2U, "request-2", "owner-epoch-1",
             "candidate-bundle-2")).record;
  auto scope = AffectedScope();
  scope.apply_generation = ApplyGeneration(2U);
  const auto rewrite = [&](const std::string &source) {
    ExecSql(path,
            "DROP TRIGGER manager_apply_intents_no_update;"
            "UPDATE manager_apply_intents SET source_label='" + source +
                "' WHERE apply_id='apply-1' AND apply_generation=1;"
                "CREATE TRIGGER manager_apply_intents_no_update BEFORE UPDATE ON "
                "manager_apply_intents BEGIN SELECT RAISE(ABORT, "
                "'manager_apply_intents is immutable'); END;");
  };
  rewrite("tampered-source");
  // Ordinary reads keep their existing exact-record behavior.
  EXPECT_EQ(journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(2U)),
            current);
  EXPECT_THROW(static_cast<void>(journal.EnsureAffectedScope(
                   current.intent_digest, scope)),
               ManagerApplyJournalIntegrityError);
  rewrite(Intent().request_context.source_label);
  EXPECT_EQ(journal.VerifyIntegrity().affected_scope_count, 0U);
  EXPECT_TRUE(journal.EnsureAffectedScope(current.intent_digest, scope).inserted);
}

TEST(ManagerApplyJournal, ChildWriteChecksOnlyImmediateApplyPredecessor) {
  const auto measure = [](const std::uint64_t generations) {
    TemporaryDirectory directory;
    SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                      CellId("cell-1"));
    for (std::uint64_t generation = 1; generation < generations;
         ++generation) {
      const auto suffix = std::to_string(generation);
      static_cast<void>(journal.EnsureIntent(
          Intent("apply-1", generation, "request-" + suffix)));
    }
    const auto suffix = std::to_string(generations);
    const auto current = journal.EnsureIntent(
        Intent("apply-1", generations, "request-" + suffix)).record;
    auto scope = AffectedScope();
    scope.apply_generation = ApplyGeneration(generations);
    const auto before = journal.record_verification_count();
    EXPECT_TRUE(journal.EnsureAffectedScope(current.intent_digest, scope).inserted);
    return journal.record_verification_count() - before;
  };
  const auto short_history = measure(2U);
  EXPECT_GT(short_history, 0U);
  EXPECT_EQ(measure(32U), short_history);
}

TEST(ManagerApplyJournal, UnrelatedCorruptionIsDetectedOnReopen) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  const auto target = Intent();
  const core::NamedApplyPublicationTransactionDTO apply{
      target.apply_id, target.apply_generation};
  {
    SqliteManagerApplyJournal journal(path, target.cell_id);
    const auto intent = journal.EnsureIntent(target).record;
    static_cast<void>(journal.EnsureIntent(
        Intent("other", 1U, "other-request")));
    ExecSql(path,
            "DROP TRIGGER manager_apply_intents_no_update;"
            "UPDATE manager_apply_intents SET source_label='tampered' "
            "WHERE apply_id='other';"
            "CREATE TRIGGER manager_apply_intents_no_update BEFORE UPDATE ON "
            "manager_apply_intents BEGIN SELECT RAISE(ABORT, "
            "'manager_apply_intents is immutable'); END;");
    EXPECT_TRUE(journal.EnsureAffectedScope(intent.intent_digest,
                                            AffectedScope()).inserted);
    EXPECT_TRUE(ReadManagerApplyStatusReadOnly(path, target.cell_id, apply).ok());
  }
  EXPECT_THROW((SqliteManagerApplyJournal(path, target.cell_id)),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyFenceJournal,
     AffectedScopeIsImmutableAndRequiredBeforeFenceAttempt) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto affected =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope());
  EXPECT_TRUE(affected.inserted);
  const auto replay =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope());
  EXPECT_FALSE(replay.inserted);
  EXPECT_EQ(replay.record.affected_scope_digest,
            affected.record.affected_scope_digest);

  auto different = AffectedScope();
  different.scope.resources.push_back(ResourceId("joint-3"));
  EXPECT_THROW(journal.EnsureAffectedScope(intent.intent_digest, different),
               ManagerApplyJournalConflictError);
  EXPECT_THROW(journal.EnsureFenceAttempt(
                   intent.intent_digest, ContentDigest(std::string(kDigestTwo)),
                   FenceIntent()),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().affected_scope_count, 1);
  EXPECT_EQ(journal.VerifyIntegrity().fence_attempt_count, 0);
}

TEST(ManagerApplyFenceJournal,
     PersistsTheExactComponentDeltaWithTheScopeAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  auto affected_scope = AffectedScope();
  affected_scope.component_delta = ReplacingDelta();
  ASSERT_TRUE(affected_scope.valid());
  ContentDigest intent_digest{std::string(kDigestOne)};
  ContentDigest scope_digest{std::string(kDigestOne)};
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    intent_digest = journal.EnsureIntent(Intent()).record.intent_digest;
    const auto written =
        journal.EnsureAffectedScope(intent_digest, affected_scope);
    ASSERT_TRUE(written.inserted);
    scope_digest = written.record.affected_scope_digest;
    // The same scope with another decision is a conflicting record.
    auto retained_only = affected_scope;
    retained_only.component_delta = {};
    EXPECT_THROW(journal.EnsureAffectedScope(intent_digest, retained_only),
                 ManagerApplyJournalConflictError);
  }
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto read =
      reopened.ReadAffectedScope(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->affected_scope, affected_scope);
  EXPECT_EQ(read->affected_scope.component_delta, ReplacingDelta());
  EXPECT_EQ(read->affected_scope_digest, scope_digest);
  EXPECT_FALSE(reopened.EnsureAffectedScope(intent_digest, affected_scope)
                   .inserted);
}

TEST(ManagerApplyFenceJournal, RefusesAComponentDeltaOutsideTheFenceScope) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  auto affected_scope = AffectedScope();
  affected_scope.component_delta = ReplacingDelta();
  // robot-1 is rebuilt, so it must be fenced.
  affected_scope.scope.controllable_component_ids.clear();
  EXPECT_FALSE(affected_scope.valid());
  EXPECT_THROW(journal.EnsureAffectedScope(intent.intent_digest, affected_scope),
               std::invalid_argument);
  EXPECT_EQ(journal.VerifyIntegrity().affected_scope_count, 0);
}

TEST(ManagerApplyFenceJournal, DetectsComponentDeltaCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "affected-delta.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    auto affected_scope = AffectedScope();
    affected_scope.component_delta = ReplacingDelta();
    static_cast<void>(
        journal.EnsureAffectedScope(intent.intent_digest, affected_scope));
  }
  ExecSql(path,
          "DROP TRIGGER manager_apply_affected_scopes_no_update;"
          // Still a valid delta, but a different decision than was sealed.
          "UPDATE manager_apply_affected_scopes SET component_delta_encoding="
          "replace(component_delta_encoding,"
          "'\"shared_scene_projection_changed\":true',"
          "'\"shared_scene_projection_changed\":false');"
          "CREATE TRIGGER manager_apply_affected_scopes_no_update "
          "BEFORE UPDATE ON manager_apply_affected_scopes BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_affected_scopes is immutable'); "
          "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyFenceJournal, DetectsAffectedScopePayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "affected-scope.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    static_cast<void>(
        journal.EnsureAffectedScope(intent.intent_digest, AffectedScope()));
  }
  ExecSql(path,
          "DROP TRIGGER manager_apply_affected_scopes_no_update;"
          "UPDATE manager_apply_affected_scopes SET resources_encoding='0;';"
          "CREATE TRIGGER manager_apply_affected_scopes_no_update "
          "BEFORE UPDATE ON manager_apply_affected_scopes BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_affected_scopes is immutable'); "
          "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyFenceJournal,
     RequiresExactParentAndRejectsDifferingReplayEvidence) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  EXPECT_THROW(EnsureAttempt(journal, ContentDigest(std::string(kDigestTwo)),
                             FenceIntent()),
               ManagerApplyJournalConflictError);
  const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;

  auto differing_attempt = FenceIntent();
  differing_attempt.reason = "different exact evidence";
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, differing_attempt),
               ManagerApplyJournalConflictError);
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-1"), ApplyGeneration(1U),
                   ContentDigest(std::string(kDigestTwo)), AcceptedFence()),
               ManagerApplyJournalConflictError);

  EXPECT_TRUE(
      journal
          .EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                 attempt.fence_attempt_digest, AcceptedFence())
          .inserted);
  auto differing_acceptance = AcceptedFence();
  differing_acceptance.replay_status =
      core::RequestReplayStatusDTO::kReplayedExact;
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-1"), ApplyGeneration(1U),
                   attempt.fence_attempt_digest, differing_acceptance),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyFenceJournal, RejectsAcceptanceWithDifferentCapturedScope) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;
  auto accepted = AcceptedFence();
  accepted.captured_scope.resources.push_back(ResourceId("joint-3"));
  EXPECT_THROW(
      journal.EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                    attempt.fence_attempt_digest, accepted),
      ManagerApplyJournalConflictError);
  EXPECT_FALSE(
      journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  auto wrong_generation = AcceptedFence();
  wrong_generation.configuration_fence_generation =
      ConfigurationFenceGeneration(6U);
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-1"), ApplyGeneration(1U),
                   attempt.fence_attempt_digest, wrong_generation),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyFenceJournal,
     OneAcceptedFenceIdentityCannotBindTwoApplyGenerations) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto first_intent = journal.EnsureIntent(Intent()).record;
  const auto first_attempt =
      EnsureAttempt(journal, first_intent.intent_digest).record;
  EXPECT_TRUE(journal
                  .EnsureFenceAcceptance(
                      ApplyId("apply-1"), ApplyGeneration(1U),
                      first_attempt.fence_attempt_digest, AcceptedFence())
                  .inserted);

  const auto second_input =
      Intent("apply-2", 1U, "request-2", "owner-epoch-1", "candidate-bundle-2");
  const auto second_intent = journal.EnsureIntent(second_input).record;
  auto second_fence_intent = FenceIntent();
  second_fence_intent.owner_request_context = second_input.request_context;
  second_fence_intent.owner_transaction =
      core::ApplyFenceOwnerDTO{ApplyId("apply-2"), ApplyGeneration(1U)};
  const auto second_attempt =
      EnsureAttempt(journal, second_intent.intent_digest, second_fence_intent)
          .record;
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-2"), ApplyGeneration(1U),
                   second_attempt.fence_attempt_digest, AcceptedFence()),
               ManagerApplyJournalConflictError);
  EXPECT_FALSE(
      journal.ReadFenceAcceptance(ApplyId("apply-2"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(journal.VerifyIntegrity().fence_acceptance_count, 1);
}

TEST(ManagerApplyFenceJournal, EnforcesCardinalityAndTextLimits) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  auto too_many_resources = FenceIntent();
  too_many_resources.scope.resources.clear();
  for (std::size_t index = 0U; index <= kMaximumManagerApplyFenceResources;
       ++index) {
    too_many_resources.scope.resources.emplace_back("resource-" +
                                                    std::to_string(index));
  }
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, too_many_resources),
               ManagerApplyJournalLimitError);
  auto long_reason = FenceIntent();
  long_reason.reason.assign(kMaximumManagerApplyFenceReasonBytes + 1U, 'x');
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, long_reason),
               std::invalid_argument);
  auto invalid_identifier = FenceIntent();
  invalid_identifier.scope.resources = {ResourceId("bad\nresource")};
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, invalid_identifier),
               std::invalid_argument);
  EXPECT_EQ(journal.VerifyIntegrity().fence_attempt_count, 0);
}

TEST(ManagerApplyFenceJournal,
     PersistsAndReloadsTheExactQualifiedTransitionEnvelope) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "maximum-fence.sqlite3";
  const auto fence_intent = MaximumFenceIntent();
  ASSERT_TRUE(fence_intent.valid());
  std::optional<PersistedManagerApplyAffectedScope> expected_scope;
  std::optional<PersistedManagerApplyFenceAttempt> expected_attempt;
  std::optional<PersistedManagerApplyFenceAcceptance> expected_acceptance;
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    expected_scope = journal
                         .EnsureAffectedScope(intent.intent_digest,
                                              AffectedScope(fence_intent))
                         .record;
    expected_attempt =
        journal
            .EnsureFenceAttempt(intent.intent_digest,
                                expected_scope->affected_scope_digest,
                                fence_intent)
            .record;
    expected_acceptance =
        journal
            .EnsureFenceAcceptance(
                ApplyId("apply-1"), ApplyGeneration(1U),
                expected_attempt->fence_attempt_digest,
                ConfigurationFenceAcceptedDTO{
                    ConfigurationFenceId("fence-maximum"),
                    ConfigurationFenceGeneration(5U), fence_intent.scope,
                    123456789LL, core::RequestReplayStatusDTO::kAcceptedNew})
            .record;
    const auto report = journal.VerifyIntegrity();
    EXPECT_EQ(report.affected_scope_count, 1);
    EXPECT_EQ(report.fence_attempt_count, 1);
    EXPECT_EQ(report.fence_acceptance_count, 1);
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto stored_scope =
      reopened.ReadAffectedScope(ApplyId("apply-1"), ApplyGeneration(1U));
  const auto stored_attempt =
      reopened.ReadFenceAttempt(ApplyId("apply-1"), ApplyGeneration(1U));
  const auto stored_acceptance =
      reopened.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(stored_scope.has_value());
  ASSERT_TRUE(stored_attempt.has_value());
  ASSERT_TRUE(stored_acceptance.has_value());
  EXPECT_EQ(stored_scope->affected_scope.scope, fence_intent.scope);
  EXPECT_EQ(stored_scope->affected_scope_digest,
            expected_scope->affected_scope_digest);
  EXPECT_EQ(stored_attempt->fence_intent, fence_intent);
  EXPECT_EQ(stored_attempt->fence_attempt_digest,
            expected_attempt->fence_attempt_digest);
  EXPECT_EQ(stored_acceptance->accepted_fence.captured_scope,
            fence_intent.scope);
  EXPECT_EQ(stored_acceptance->acceptance_digest,
            expected_acceptance->acceptance_digest);
}

TEST(ManagerApplyFenceJournal,
     DetectsAttemptAndAcceptancePayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto attempt_path = directory.path() / "attempt.sqlite3";
  {
    SqliteManagerApplyJournal journal(attempt_path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    static_cast<void>(EnsureAttempt(journal, intent.intent_digest));
  }
  ExecSql(
      attempt_path,
      "DROP TRIGGER manager_apply_fence_attempts_no_update;"
      "UPDATE manager_apply_fence_attempts SET reason='tampered';"
      "CREATE TRIGGER manager_apply_fence_attempts_no_update "
      "BEFORE UPDATE ON manager_apply_fence_attempts BEGIN "
      "SELECT RAISE(ABORT, 'manager_apply_fence_attempts is immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(attempt_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);

  const auto acceptance_path = directory.path() / "acceptance.sqlite3";
  {
    SqliteManagerApplyJournal journal(acceptance_path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;
    static_cast<void>(journal.EnsureFenceAcceptance(
        ApplyId("apply-1"), ApplyGeneration(1U), attempt.fence_attempt_digest,
        AcceptedFence()));
  }
  ExecSql(
      acceptance_path,
      "DROP TRIGGER manager_apply_fence_acceptances_no_update;"
      "UPDATE manager_apply_fence_acceptances SET accepted_at_ns=1;"
      "CREATE TRIGGER manager_apply_fence_acceptances_no_update "
      "BEFORE UPDATE ON manager_apply_fence_acceptances BEGIN "
      "SELECT RAISE(ABORT, 'manager_apply_fence_acceptances is immutable'); "
      "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(acceptance_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyRuntimeJournal,
     PersistsIntentAndStagedReadyExactlyAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "runtime.sqlite3";
  PersistedManagerApplyRuntimeConvergenceIntent expected_intent{
      RuntimeIntent(ContentDigest(std::string(kDigestOne))),
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestOne)),
  };
  PersistedManagerApplyRuntimeStagedReady expected_staged{
      RuntimeStagedReady(ContentDigest(std::string(kDigestOne)), 1U),
      ContentDigest(std::string(kDigestOne)),
  };
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto accepted = EnsureAcceptedApply(journal);
    const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest));
    EXPECT_TRUE(runtime_intent.inserted);
    EXPECT_TRUE(runtime_intent.record.runtime_convergence_intent.valid());
    const auto runtime_replay = journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest));
    EXPECT_FALSE(runtime_replay.inserted);
    EXPECT_EQ(runtime_replay.record, runtime_intent.record);
    expected_intent = runtime_intent.record;

    const auto staged = journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
        runtime_intent.record.runtime_convergence_intent_digest, 1U));
    EXPECT_TRUE(staged.inserted);
    EXPECT_TRUE(staged.record.staged_ready.valid());
    const auto staged_replay =
        journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
            runtime_intent.record.runtime_convergence_intent_digest, 1U));
    EXPECT_FALSE(staged_replay.inserted);
    EXPECT_EQ(staged_replay.record, staged.record);
    expected_staged = staged.record;
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(reopened.ReadRuntimeConvergenceIntent(ApplyId("apply-1"),
                                                  ApplyGeneration(1U)),
            expected_intent);
  EXPECT_EQ(reopened.ReadLatestRuntimeStagedReady(ApplyId("apply-1"),
                                                  ApplyGeneration(1U)),
            expected_staged);
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.runtime_convergence_intent_count, 1);
  EXPECT_EQ(report.runtime_staged_ready_count, 1);
}

TEST(ManagerApplyRuntimeJournal,
     RequiresAcceptedParentAndRejectsConflictingReplay) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "runtime.sqlite3",
                                    CellId("cell-1"));
  const auto parent = journal.EnsureIntent(Intent()).record;
  EXPECT_THROW(journal.EnsureRuntimeConvergenceIntent(
                   parent.intent_digest,
                   RuntimeIntent(ContentDigest(std::string(kDigestOne)))),
               ManagerApplyJournalConflictError);

  const auto attempt = EnsureAttempt(journal, parent.intent_digest).record;
  const auto acceptance = journal.EnsureFenceAcceptance(
      ApplyId("apply-1"), ApplyGeneration(1U), attempt.fence_attempt_digest,
      AcceptedFence());
  const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
      parent.intent_digest, RuntimeIntent(acceptance.record.acceptance_digest));

  auto conflicting_intent = RuntimeIntent(acceptance.record.acceptance_digest);
  conflicting_intent.runtime_units.front().product_identity =
      ContentDigest(std::string(kDigestTwo));
  EXPECT_THROW(journal.EnsureRuntimeConvergenceIntent(parent.intent_digest,
                                                      conflicting_intent),
               ManagerApplyJournalConflictError);

  EXPECT_THROW(journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
                   ContentDigest(std::string(kDigestTwo)), 1U)),
               ManagerApplyJournalConflictError);
  EXPECT_THROW(
      journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
          runtime_intent.record.runtime_convergence_intent_digest, 2U)),
      ManagerApplyJournalConflictError);

  const auto first = journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
      runtime_intent.record.runtime_convergence_intent_digest, 1U));
  auto conflicting_staged = first.record.staged_ready;
  conflicting_staged.runtime_units.front().process_observation_sequence = 2U;
  EXPECT_THROW(journal.EnsureRuntimeStagedReady(conflicting_staged),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().runtime_staged_ready_count, 1);
}

TEST(ManagerApplyRuntimeJournal,
     EmptyRuntimeSetIsExplicitAndStagedGenerationsAreBounded) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "runtime.sqlite3",
                                    CellId("cell-1"));
  const auto accepted = EnsureAcceptedApply(journal);
  const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
      accepted.intent.intent_digest,
      RuntimeIntent(accepted.acceptance.acceptance_digest, true));
  ASSERT_TRUE(
      runtime_intent.record.runtime_convergence_intent.runtime_units.empty());

  for (std::uint64_t generation = 1U;
       generation <= kMaximumRuntimeStagedReadySetsPerApply; ++generation) {
    EXPECT_TRUE(journal
                    .EnsureRuntimeStagedReady(RuntimeStagedReady(
                        runtime_intent.record.runtime_convergence_intent_digest,
                        generation, true))
                    .inserted);
  }
  EXPECT_THROW(journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
                   runtime_intent.record.runtime_convergence_intent_digest,
                   kMaximumRuntimeStagedReadySetsPerApply + 1U, true)),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().runtime_staged_ready_count,
            static_cast<std::int64_t>(kMaximumRuntimeStagedReadySetsPerApply));
}

TEST(ManagerApplyRuntimeJournal, CorruptStagedPredecessorRefusesLaterWrite) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "runtime.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto accepted = EnsureAcceptedApply(journal);
  const auto runtime = journal.EnsureRuntimeConvergenceIntent(
      accepted.intent.intent_digest,
      RuntimeIntent(accepted.acceptance.acceptance_digest));
  const auto digest = runtime.record.runtime_convergence_intent_digest;
  ASSERT_TRUE(journal.EnsureRuntimeStagedReady(RuntimeStagedReady(digest, 1U))
                  .inserted);
  ExecSql(path,
          "DROP TRIGGER manager_apply_runtime_staged_ready_sets_no_update;"
          "UPDATE manager_apply_runtime_staged_ready_sets "
          "SET runtime_evidence_encoding='0;' WHERE staged_ready_generation=1;"
          "CREATE TRIGGER manager_apply_runtime_staged_ready_sets_no_update "
          "BEFORE UPDATE ON manager_apply_runtime_staged_ready_sets BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_runtime_staged_ready_sets is "
          "immutable'); END;");
  EXPECT_THROW(static_cast<void>(journal.EnsureRuntimeStagedReady(
                   RuntimeStagedReady(digest, 2U))),
               ManagerApplyJournalIntegrityError);
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &database,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr), SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(database,
      "SELECT COUNT(*) FROM manager_apply_runtime_staged_ready_sets "
      "WHERE staged_ready_generation=2", -1, &statement, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int64(statement, 0), 0);
  EXPECT_EQ(sqlite3_finalize(statement), SQLITE_OK);
  EXPECT_EQ(sqlite3_close(database), SQLITE_OK);
}

TEST(ManagerApplyRuntimeJournal,
     DetectsConvergenceAndStagedReadyPayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto intent_path = directory.path() / "runtime-intent.sqlite3";
  {
    SqliteManagerApplyJournal journal(intent_path, CellId("cell-1"));
    const auto accepted = EnsureAcceptedApply(journal);
    static_cast<void>(journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest)));
  }
  ExecSql(intent_path,
          "DROP TRIGGER manager_apply_runtime_convergence_intents_no_update;"
          "UPDATE manager_apply_runtime_convergence_intents "
          "SET runtime_units_encoding='0;';"
          "CREATE TRIGGER manager_apply_runtime_convergence_intents_no_update "
          "BEFORE UPDATE ON manager_apply_runtime_convergence_intents BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_runtime_convergence_intents is "
          "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(intent_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);

  const auto staged_path = directory.path() / "runtime-staged.sqlite3";
  {
    SqliteManagerApplyJournal journal(staged_path, CellId("cell-1"));
    const auto accepted = EnsureAcceptedApply(journal);
    const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest));
    static_cast<void>(journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
        runtime_intent.record.runtime_convergence_intent_digest, 1U)));
  }
  ExecSql(staged_path,
          "DROP TRIGGER manager_apply_runtime_staged_ready_sets_no_update;"
          "UPDATE manager_apply_runtime_staged_ready_sets "
          "SET runtime_evidence_encoding='0;';"
          "CREATE TRIGGER manager_apply_runtime_staged_ready_sets_no_update "
          "BEFORE UPDATE ON manager_apply_runtime_staged_ready_sets BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_runtime_staged_ready_sets is "
          "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(staged_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplySelectedBundleJournal,
     PersistsCompleteForwardChainAndExactReplayAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "selected-bundle.sqlite3";
  std::optional<SelectedBundleTransitionRecords> expected;
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    expected = EnsureSelectedBundleTransition(journal);

    const auto materialization =
        journal.EnsurePrivateMaterialization(PrivateMaterialization());
    EXPECT_FALSE(materialization.inserted);
    EXPECT_EQ(materialization.record, expected->materialization);
    const auto publication = journal.EnsurePublicationReceipt(
        PublicationReceipt(expected->materialization));
    EXPECT_FALSE(publication.inserted);
    EXPECT_EQ(publication.record, expected->publication);
    const auto report = journal.VerifyIntegrity();
    EXPECT_EQ(report.private_materialization_count, 1);
    EXPECT_EQ(report.publication_receipt_count, 1);
  }

  ASSERT_TRUE(expected.has_value());
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(reopened.ReadPrivateMaterialization(ApplyId("apply-1"),
                                                ApplyGeneration(1U)),
            expected->materialization);
  EXPECT_EQ(
      reopened.ReadPublicationReceipt(ApplyId("apply-1"), ApplyGeneration(1U)),
      expected->publication);
}

TEST(ManagerApplySelectedBundleJournal,
     RejectsConflictingReplayAtEveryForwardBoundary) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(
      directory.path() / "selected-bundle-conflict.sqlite3", CellId("cell-1"));
  const auto records = EnsureSelectedBundleTransition(journal);

  auto materialization = PrivateMaterialization();
  materialization.retained_bundle_canonical_byte_size += 1U;
  EXPECT_THROW(journal.EnsurePrivateMaterialization(materialization),
               ManagerApplyJournalConflictError);

  auto publication = PublicationReceipt(records.materialization);
  publication.publication_receipt.durable_public_pointer_content_digest =
      ContentDigest(std::string(kDigestTwo));
  EXPECT_THROW(journal.EnsurePublicationReceipt(publication),
               ManagerApplyJournalConflictError);

}

TEST(ManagerApplySelectedBundleJournal,
     DetectsForwardChainPayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "selected-bundle-corrupt.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    static_cast<void>(EnsureSelectedBundleTransition(journal));
  }
  ExecSql(path,
          "DROP TRIGGER manager_apply_private_materializations_no_update;"
          "UPDATE manager_apply_private_materializations "
          "SET retained_bundle_canonical_byte_size=513;"
          "CREATE TRIGGER manager_apply_private_materializations_no_update "
          "BEFORE UPDATE ON manager_apply_private_materializations BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_private_materializations is "
          "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyCompletionJournal,
     PersistsReleaseAndTerminalChainWithExactRestartReplay) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply-completion.sqlite3";
  std::optional<SelectedBundleTransitionRecords> transition;
  std::optional<ApplyCompletionRecords> expected;
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    transition = EnsureSelectedBundleTransition(journal);
    expected = EnsureApplyCompletion(journal, *transition);

    const auto acceptance =
        journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
    ASSERT_TRUE(acceptance.has_value());
    const auto release_intent = journal.EnsureFenceReleaseIntent(
        FenceReleaseIntent(*transition, *acceptance));
    EXPECT_FALSE(release_intent.inserted);
    EXPECT_EQ(release_intent.record, expected->release_intent);
    const auto released =
        journal.EnsureFenceReleased(FenceReleased(expected->release_intent));
    EXPECT_FALSE(released.inserted);
    EXPECT_EQ(released.record, expected->released);
    const auto terminal = journal.EnsureTerminalOutcome(
        TerminalOutcome(*transition, expected->released));
    EXPECT_FALSE(terminal.inserted);
    EXPECT_EQ(terminal.record, expected->terminal);
    const auto status = journal.ReadStatus(ApplyId("apply-1"),
                                           ApplyGeneration(1U));
    ASSERT_TRUE(status.has_value());
    const auto progress = DeriveManagerApplyProgress(*status);
    EXPECT_EQ(progress.phase, ManagerApplyPhase::kSettled);
    EXPECT_EQ(progress.record_digest,
              expected->terminal.terminal_outcome_digest);

    const auto report = journal.VerifyIntegrity();
    EXPECT_EQ(report.fence_release_intent_count, 1);
    EXPECT_EQ(report.fence_released_count, 1);
    EXPECT_EQ(report.terminal_outcome_count, 1);
  }

  ASSERT_TRUE(expected.has_value());
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(
      reopened.ReadFenceReleaseIntent(ApplyId("apply-1"), ApplyGeneration(1U)),
      expected->release_intent);
  EXPECT_EQ(reopened.ReadFenceReleased(ApplyId("apply-1"), ApplyGeneration(1U)),
            expected->released);
  EXPECT_EQ(
      reopened.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U)),
      expected->terminal);
}

TEST(ManagerApplyCompletionJournal,
     DependencyReadsDoNotRetainAbsenceOrCorruptionAcrossCalls) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "read-scope.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto transition = EnsureSelectedBundleTransition(journal);
  const auto read = [&] {
    return journal.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U));
  };
  EXPECT_FALSE(read().has_value());
  const auto completed = EnsureApplyCompletion(journal, transition);
  EXPECT_EQ(read(), completed.terminal);

  // A separate writer changes a deep predecessor without changing schema
  // shape. A completed earlier read must not hide it, and an exception must
  // not leave a stale read context behind after the database is restored.
  const auto rewrite_source = [&](const std::string &source) {
    ExecSql(
        path,
        "DROP TRIGGER manager_apply_intents_no_update;"
        "UPDATE manager_apply_intents SET source_label='" +
            source +
            "';"
            "CREATE TRIGGER manager_apply_intents_no_update "
            "BEFORE UPDATE ON manager_apply_intents BEGIN "
            "SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); END;");
  };
  rewrite_source("tampered-source");
  EXPECT_THROW(static_cast<void>(read()), ManagerApplyJournalIntegrityError);
  EXPECT_THROW(static_cast<void>(journal.VerifyIntegrity()),
               ManagerApplyJournalIntegrityError);
  rewrite_source(Intent().request_context.source_label);
  EXPECT_EQ(read(), completed.terminal);
  EXPECT_EQ(journal.VerifyIntegrity().terminal_outcome_count, 1);
}

TEST(ManagerApplyCompletionJournal,
     RejectsPrematureTerminalAndConflictingCompletionRecords) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(
      directory.path() / "apply-completion-conflict.sqlite3", CellId("cell-1"));
  const auto transition = EnsureSelectedBundleTransition(journal);
  const auto acceptance =
      journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(acceptance.has_value());
  const auto release_intent = journal.EnsureFenceReleaseIntent(
      FenceReleaseIntent(transition, *acceptance));
  EXPECT_FALSE(
      journal.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  auto impossible_terminal = ManagerApplyTerminalOutcomeDTO{
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      transition.materialization.private_materialization
          .candidate_projection_revision,
      transition.publication.publication_receipt_digest,
      transition.publication.publication_receipt_digest,
      ContentDigest(std::string(kDigestOne)),
      ManagerApplyTerminalOutcomeState::kSucceeded,
      std::nullopt,
  };
  EXPECT_THROW(journal.EnsureTerminalOutcome(impossible_terminal),
               ManagerApplyJournalConflictError);

  auto conflicting_intent = FenceReleaseIntent(transition, *acceptance);
  conflicting_intent.release_request.manager_runtime_epoch =
      RuntimeEpoch("manager-epoch-2");
  EXPECT_THROW(journal.EnsureFenceReleaseIntent(conflicting_intent),
               ManagerApplyJournalConflictError);

  const auto released =
      journal.EnsureFenceReleased(FenceReleased(release_intent.record));
  auto conflicting_release = FenceReleased(release_intent.record);
  conflicting_release.released_fence.released_at_ns += 1;
  EXPECT_THROW(journal.EnsureFenceReleased(conflicting_release),
               ManagerApplyJournalConflictError);

  static_cast<void>(journal.EnsureTerminalOutcome(
      TerminalOutcome(transition, released.record)));
  auto conflicting_terminal = TerminalOutcome(transition, released.record);
  conflicting_terminal.publication_receipt_digest =
      ContentDigest(std::string(kDigestTwo));
  EXPECT_THROW(journal.EnsureTerminalOutcome(conflicting_terminal),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyCompletionJournal,
     DetectsReleaseRequestPayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply-completion-corrupt.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto transition = EnsureSelectedBundleTransition(journal);
    static_cast<void>(EnsureApplyCompletion(journal, transition));
  }
  ExecSql(path, "DROP TRIGGER manager_apply_fence_release_intents_no_update;"
                "UPDATE manager_apply_fence_release_intents "
                "SET release_request_encoding='0;';"
                "CREATE TRIGGER manager_apply_fence_release_intents_no_update "
                "BEFORE UPDATE ON manager_apply_fence_release_intents BEGIN "
                "SELECT RAISE(ABORT, 'manager_apply_fence_release_intents is "
                "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

} // namespace
TEST(ManagerNativeApplyParentRead,
     ReopensExactPreparationFailureWithOriginalHeldExclusion) {
  TemporaryDirectory package;
  const auto candidate = NativeParentCandidate(package.path(), false);
  ASSERT_TRUE(candidate);
  for (const bool authoring : {false, true}) {
    TemporaryDirectory state;
    const auto path = state.path() / "early-parent.sqlite3";
    const auto fixture =
        WriteNativeParent(path, package.path(), *candidate, authoring);
    ASSERT_TRUE(
        core::ValidateCellAdmissionRecoveryReceiptQuery(fixture.query).ok());
    ASSERT_TRUE(
        core::ValidateCellAdmissionRecoveryReceipt(fixture.receipt).ok());
    const auto cell = candidate->value().cell_graph.cell_id;
    const auto before = ReadFileBytes(path);
    const auto general = ReadNativeApplyParentStatusReadOnly(
        path, cell, fixture.plan, *candidate);
    ASSERT_TRUE(general.ok()) << general.error().message;
    const auto early = ReadNativeApplyPreparationFailureReadOnly(
        path, cell, fixture.plan, *candidate, fixture.claim, fixture.query,
        fixture.receipt);
    ASSERT_TRUE(early.ok()) << early.error().message;
    ASSERT_TRUE(early.value());
    EXPECT_EQ(early.value()->intent, general.value().intent);
    EXPECT_EQ(early.value()->terminal_outcome,
              general.value().terminal_outcome);
    ASSERT_TRUE(early.value()->terminal_outcome);
    EXPECT_EQ(early.value()->terminal_outcome->terminal_outcome.state,
              ManagerApplyTerminalOutcomeState::kUnknownEffect);
    EXPECT_FALSE(early.value()->private_materialization);
    EXPECT_FALSE(early.value()->publication_receipt);
    EXPECT_FALSE(early.value()->fence_release_intent);
    EXPECT_FALSE(early.value()->fence_released);
    EXPECT_EQ(fixture.receipt.held_claims, std::vector{fixture.claim});
    EXPECT_EQ(ReadFileBytes(path), before);
    SqliteManagerApplyJournal reopened(path, cell);
    EXPECT_EQ(reopened.ReadTerminalOutcome(fixture.plan.apply.apply_id,
                                           fixture.plan.apply.apply_generation),
              early.value()->terminal_outcome);
    EXPECT_EQ(reopened.VerifyIntegrity().fence_released_count, 0);
    const auto absent = state.path() / "missing-parent.sqlite3";
    EXPECT_FALSE(ReadNativeApplyParentStatusReadOnly(absent, cell, fixture.plan,
                                                     *candidate)
                     .ok());
    EXPECT_FALSE(std::filesystem::exists(absent));
  }
}

TEST(ManagerNativeApplyParentRead,
     LaterCodeWrongPhaseInstallationAndPendingRemainUnclassified) {
  TemporaryDirectory package;
  const auto candidate = NativeParentCandidate(package.path(), false);
  ASSERT_TRUE(candidate);
  for (const auto &[code, phase, materialized] :
       std::vector<std::tuple<std::string, ManagerApplyPhase, bool>>{
           {"runtime_composition_finalization_failed",
            ManagerApplyPhase::kRuntimeConverged, false},
           {"initial_scene_installation_failed",
            ManagerApplyPhase::kRuntimeConverged, false},
           {"publication_failed", ManagerApplyPhase::kRuntimeConverged, false},
           {"runtime_composition_preparation_failed",
            ManagerApplyPhase::kFenceAccepted, false},
           {"runtime_composition_preparation_failed",
            ManagerApplyPhase::kRuntimeConverged, true},
           {"", ManagerApplyPhase::kRuntimeConverged, false}}) {
    SCOPED_TRACE(code);
    TemporaryDirectory state;
    const auto path = state.path() / "later-parent.sqlite3";
    const auto fixture = WriteNativeParent(path, package.path(), *candidate,
                                           false, code, phase, materialized);
    const auto cell = candidate->value().cell_graph.cell_id;
    const auto read = ReadNativeApplyParentStatusReadOnly(
        path, cell, fixture.plan, *candidate);
    ASSERT_TRUE(read.ok()) << read.error().message;
    EXPECT_EQ(read.value().terminal_outcome.has_value(), !code.empty());
    const auto early = ReadNativeApplyPreparationFailureReadOnly(
        path, cell, fixture.plan, *candidate, fixture.claim, fixture.query,
        fixture.receipt);
    ASSERT_TRUE(early.ok()) << early.error().message;
    EXPECT_FALSE(early.value());
  }
}

TEST(ManagerNativeApplyParentRead,
     ScenarioCompositionRefusalIsTheSameEarlyPreparationFailure) {
  TemporaryDirectory package;
  const auto candidate = NativeParentCandidate(package.path(), false);
  ASSERT_TRUE(candidate);
  // Only the host's typed refusal makes a known `failed` Prepare early.
  for (const auto &[cause, classified] :
       std::vector<std::pair<std::string, bool>>{
           {std::string(kScenarioCompositionRefusedCode), true},
           {"typed_cause_code", false}}) {
    SCOPED_TRACE(cause);
    TemporaryDirectory state;
    const auto path = state.path() / "refused-parent.sqlite3";
    const auto fixture = WriteNativeParent(
        path, package.path(), *candidate, false,
        "runtime_composition_preparation_failed",
        ManagerApplyPhase::kRuntimeConverged, false, nullptr, false, false,
        cause);
    const auto cell = candidate->value().cell_graph.cell_id;
    const auto early = ReadNativeApplyPreparationFailureReadOnly(
        path, cell, fixture.plan, *candidate, fixture.claim, fixture.query,
        fixture.receipt);
    ASSERT_TRUE(early.ok()) << early.error().message;
    ASSERT_EQ(early.value().has_value(), classified);
    if (classified) {
      EXPECT_EQ(early.value()->terminal_outcome->terminal_outcome.state,
                ManagerApplyTerminalOutcomeState::kFailed);
    }
  }
}

TEST(ManagerNativeApplyParentRead,
     RejectsForeignPlanClaimAndNonExactRecoveryEvidence) {
  TemporaryDirectory package;
  TemporaryDirectory state;
  const auto candidate = NativeParentCandidate(package.path(), false);
  ASSERT_TRUE(candidate);
  const auto path = state.path() / "foreign-parent.sqlite3";
  const auto fixture = WriteNativeParent(path, package.path(), *candidate);
  const auto cell = candidate->value().cell_graph.cell_id;
  const auto baseline = ReadNativeApplyPreparationFailureReadOnly(
      path, cell, fixture.plan, *candidate, fixture.claim, fixture.query,
      fixture.receipt);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(baseline.value());
  const auto reject_plan = [&](const auto &mutate) {
    auto plan = fixture.plan;
    mutate(plan);
    EXPECT_FALSE(
        ReadNativeApplyParentStatusReadOnly(path, cell, plan, *candidate).ok());
  };
  reject_plan([](auto &p) { p.apply.apply_id = ApplyId("foreign-apply"); });
  reject_plan([](auto &p) { p.apply.apply_generation = ApplyGeneration(2U); });
  reject_plan([](auto &p) {
    p.candidate_retained_bundle_id = RetainedBundleId("foreign-candidate");
  });
  reject_plan([](auto &p) {
    p.candidate_graph_revision = GraphRevision("foreign-graph");
  });
  reject_plan([](auto &p) {
    p.removed_components.push_back(ControllableComponentId("invented-removal"));
  });
  reject_plan([](auto &p) {
    p.removed_domains.push_back(ControlDomainId("invented-removal"));
  });
  EXPECT_FALSE(ReadNativeApplyParentStatusReadOnly(path, CellId("foreign-cell"),
                                                   fixture.plan, *candidate)
                   .ok());
  const auto reject_claim = [&](const auto &mutate) {
    auto claim = fixture.claim;
    mutate(claim.intent);
    const core::CellAdmissionRecoveryReceiptQueryDTO query{claim.intent, claim,
                                                           std::nullopt};
    EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                     path, cell, fixture.plan, *candidate, claim, query,
                     HeldNativeParent(claim))
                     .ok());
  };
  reject_claim(
      [](auto &i) { i.owner_epoch = RequestOwnerEpoch("foreign-owner"); });
  reject_claim([](auto &i) { i.request_id = RequestId("foreign-request"); });
  reject_claim([](auto &i) { i.owner_transaction_id = "foreign-transaction"; });
  reject_claim([](auto &i) {
    i.request_payload_digest = ContentDigest(std::string(kDigestTwo));
  });
  reject_claim([](auto &i) {
    i.expected_admission_generation = core::AdmissionGeneration(9U);
  });
  auto query = fixture.query;
  query.expected_claim->claim_generation =
      core::CellAdmissionClaimGeneration(4U);
  EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                   path, cell, fixture.plan, *candidate, fixture.claim, query,
                   fixture.receipt)
                   .ok());
  auto receipt = fixture.receipt;
  receipt.matched_claim->acquired_admission_generation =
      core::AdmissionGeneration(10U);
  receipt.held_claims = {*receipt.matched_claim};
  EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                   path, cell, fixture.plan, *candidate, fixture.claim,
                   fixture.query, receipt)
                   .ok());
  receipt = fixture.receipt;
  receipt.status = core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown;
  receipt.matched_claim.reset();
  EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                   path, cell, fixture.plan, *candidate, fixture.claim,
                   fixture.query, receipt)
                   .ok());
  // Authoring transfer retains the complete stored claim, not just its intent.
  const auto authoring_path = state.path() / "authoring-parent.sqlite3";
  auto authoring =
      WriteNativeParent(authoring_path, package.path(), *candidate, true);
  authoring.claim.claim_generation = core::CellAdmissionClaimGeneration(99U);
  authoring.query = {authoring.claim.intent, authoring.claim, std::nullopt};
  authoring.receipt = HeldNativeParent(authoring.claim);
  EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                   authoring_path, cell, authoring.plan, *candidate,
                   authoring.claim, authoring.query, authoring.receipt)
                   .ok());
}

TEST(ManagerNativeApplyParentRead,
     JoinsHealthyRetainedComponentsAndRoutesOutsideEmptyFence) {
  TemporaryDirectory package;
  TemporaryDirectory state;
  const auto candidate = NativeParentCandidate(package.path(), true);
  ASSERT_TRUE(candidate);
  const auto path = state.path() / "retained-closure.sqlite3";
  const auto fixture = WriteNativeParent(path, package.path(), *candidate);
  ASSERT_FALSE(fixture.plan.components.empty());
  ASSERT_FALSE(fixture.plan.routes.empty());
  const auto cell = candidate->value().cell_graph.cell_id;
  const auto read =
      ReadNativeApplyParentStatusReadOnly(path, cell, fixture.plan, *candidate);
  ASSERT_TRUE(read.ok()) << read.error().message;
  ASSERT_TRUE(read.value().affected_scope);
  EXPECT_TRUE(read.value()
                  .affected_scope->affected_scope.scope
                  .controllable_component_ids.empty());
  EXPECT_EQ(fixture.plan.retained_components.size(),
            candidate->value().cell_graph.controllable_components.size());
  EXPECT_EQ(fixture.plan.retained_domains.size(),
            candidate->value().desired_control_configurations.size());
  const auto reject_plan = [&](const auto &mutate) {
    auto plan = fixture.plan;
    mutate(plan);
    EXPECT_FALSE(
        ReadNativeApplyParentStatusReadOnly(path, cell, plan, *candidate).ok());
  };
  reject_plan([](auto &p) { p.components.clear(); });
  reject_plan([](auto &p) { p.components.push_back(p.components.front()); });
  reject_plan([](auto &p) { p.routes.clear(); });
  reject_plan([](auto &p) { p.retained_components.clear(); });
  reject_plan([](auto &p) { p.retained_domains.clear(); });
  reject_plan([](auto &p) {
    p.components.front().generation = core::ComponentLifecycleGeneration(0U);
  });
  reject_plan([](auto &p) {
    p.routes.front().generation = core::ActiveControlConfigurationGeneration(
        std::numeric_limits<std::uint64_t>::max());
  });
  TemporaryDirectory foreign_package;
  const auto foreign_candidate =
      NativeParentCandidate(foreign_package.path(), false);
  ASSERT_TRUE(foreign_candidate);
  EXPECT_FALSE(ReadNativeApplyParentStatusReadOnly(path, cell, fixture.plan,
                                                   *foreign_candidate)
                   .ok());
}

TEST(ManagerNativeApplyParentRead,
     JoinsActualRemovedMembershipFromSameCellSealedDelta) {
  TemporaryDirectory previous_package;
  TemporaryDirectory candidate_package;
  TemporaryDirectory state;
  const auto previous = NativeParentCandidate(previous_package.path(), true);
  const auto candidate = NativeParentCandidate(candidate_package.path(), false);
  ASSERT_TRUE(previous);
  ASSERT_TRUE(candidate);
  ASSERT_EQ(previous->value().cell_graph.cell_id,
            candidate->value().cell_graph.cell_id);
  const auto path = state.path() / "removed-closure.sqlite3";
  const auto fixture = WriteNativeParent(
      path, candidate_package.path(), *candidate, false,
      "runtime_composition_preparation_failed",
      ManagerApplyPhase::kRuntimeConverged, false, previous.get());
  ASSERT_FALSE(fixture.plan.removed_components.empty());
  ASSERT_FALSE(fixture.plan.removed_domains.empty());
  EXPECT_TRUE(fixture.plan.components.empty());
  EXPECT_TRUE(fixture.plan.routes.empty());
  const auto cell = candidate->value().cell_graph.cell_id;
  const auto read = ReadNativeApplyPreparationFailureReadOnly(
      path, cell, fixture.plan, *candidate, fixture.claim, fixture.query,
      fixture.receipt);
  ASSERT_TRUE(read.ok()) << read.error().message;
  ASSERT_TRUE(read.value());
  auto foreign = fixture.plan;
  foreign.removed_components.clear();
  EXPECT_FALSE(
      ReadNativeApplyParentStatusReadOnly(path, cell, foreign, *candidate)
          .ok());
  foreign = fixture.plan;
  foreign.removed_domains.clear();
  EXPECT_FALSE(
      ReadNativeApplyParentStatusReadOnly(path, cell, foreign, *candidate)
          .ok());
}

TEST(ManagerNativeApplyParentRead,
     LinkedParentUsesSealedClaimAndRefusesInstalledChildProof) {
  TemporaryDirectory previous_package;
  TemporaryDirectory candidate_package;
  const auto previous = NativeParentCandidate(previous_package.path(), false);
  const auto candidate = NativeParentCandidate(candidate_package.path(), true);
  ASSERT_TRUE(previous);
  ASSERT_TRUE(candidate);
  ASSERT_EQ(previous->value().cell_graph.cell_id,
            candidate->value().cell_graph.cell_id);
  for (const bool holding : {false, true}) {
    TemporaryDirectory state;
    const auto path = state.path() / "linked-prepare.sqlite3";
    const auto fixture =
        WriteNativeParent(path, candidate_package.path(), *candidate, false,
                          "runtime_composition_preparation_failed",
                          ManagerApplyPhase::kRuntimeConverged, false,
                          previous.get(), true, holding);
    const auto cell = candidate->value().cell_graph.cell_id;
    const auto retained = ReadNativeApplyParentStatusReadOnly(
        path, cell, fixture.plan, *candidate);
    ASSERT_TRUE(retained.ok()) << retained.error().message;
    ASSERT_TRUE(retained.value().bring_up_plan);
    EXPECT_EQ(fixture.claim.intent.request_payload_digest,
              retained.value().bring_up_plan->plan_digest);
    EXPECT_NE(fixture.claim.intent.request_payload_digest,
              retained.value().intent.intent_digest);
    // Actual Q91 acquisition keeps the Manager claim's ordinary empty scope.
    // Its nonempty aggregate is carried separately by the held Apply fence.
    EXPECT_TRUE(fixture.claim.intent.scope.resources.empty());
    EXPECT_TRUE(fixture.claim.intent.scope.configurations.empty());
    ASSERT_TRUE(retained.value().fence_acceptance);
    ASSERT_FALSE(retained.value().fence_acceptance->accepted_fence.captured_scope
                     .resources.empty());
    EXPECT_EQ(retained.value().bring_up_child_proofs.size(), holding ? 2U : 1U);
    const auto read = ReadNativeApplyPreparationFailureReadOnly(
        path, cell, fixture.plan, *candidate, fixture.claim, fixture.query,
        fixture.receipt);
    ASSERT_TRUE(read.ok()) << read.error().message;
    EXPECT_EQ(read.value().has_value(), !holding);
    auto scoped = fixture.claim;
    scoped.intent.scope.resources = retained.value().fence_acceptance
                                        ->accepted_fence.captured_scope.resources;
    EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                     path, cell, fixture.plan, *candidate, scoped,
                     {scoped.intent, scoped, std::nullopt},
                     HeldNativeParent(scoped))
                     .ok());
    auto ordinary = fixture.claim;
    ordinary.intent.request_payload_digest =
        retained.value().intent.intent_digest;
    EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                     path, cell, fixture.plan, *candidate, ordinary,
                     {ordinary.intent, ordinary, std::nullopt},
                     HeldNativeParent(ordinary))
                     .ok());
    auto foreign = fixture.claim;
    foreign.claim_generation = core::CellAdmissionClaimGeneration(100U);
    EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                     path, cell, fixture.plan, *candidate, foreign,
                     {foreign.intent, foreign, std::nullopt},
                     HeldNativeParent(foreign))
                     .ok());
  }
}

TEST(ManagerNativeApplyParentRead,
     VerifiesTouchedChainAndFailureIntentDigestInFreshSnapshot) {
  TemporaryDirectory package;
  TemporaryDirectory state;
  const auto candidate = NativeParentCandidate(package.path(), false);
  ASSERT_TRUE(candidate);
  const auto path = state.path() / "corrupt-inventory.sqlite3";
  const auto fixture = WriteNativeParent(path, package.path(), *candidate);
  const auto cell = candidate->value().cell_graph.cell_id;
  {
    SqliteManagerApplyJournal journal(path, cell);
    auto foreign_row = Intent("other-apply", 1U, "other-request");
    foreign_row.cell_id = cell;
    static_cast<void>(journal.EnsureIntent(foreign_row));
  }
  ASSERT_TRUE(
      ReadNativeApplyParentStatusReadOnly(path, cell, fixture.plan, *candidate)
          .ok());
  ExecSql(
      path,
      "DROP TRIGGER manager_apply_intents_no_update;"
      "UPDATE manager_apply_intents SET source_label='tampered' WHERE "
      "apply_id='other-apply';"
      "CREATE TRIGGER manager_apply_intents_no_update BEFORE UPDATE ON "
      "manager_apply_intents "
      "BEGIN SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); END;");
  EXPECT_TRUE(
      ReadNativeApplyParentStatusReadOnly(path, cell, fixture.plan, *candidate)
          .ok());
  ExecSql(
      path,
      "DROP TRIGGER manager_apply_intents_no_update;"
      "UPDATE manager_apply_intents SET source_label='component-manager-test' "
      "WHERE apply_id='other-apply';"
      "CREATE TRIGGER manager_apply_intents_no_update BEFORE UPDATE ON "
      "manager_apply_intents "
      "BEGIN SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); END;");
  ASSERT_TRUE(
      ReadNativeApplyParentStatusReadOnly(path, cell, fixture.plan, *candidate)
          .ok());
  ExecSql(path, "DROP TRIGGER manager_apply_terminal_outcomes_no_update;"
                "UPDATE manager_apply_terminal_outcomes SET intent_digest='" +
                    std::string(kDigestTwo) +
                    "';"
                    "CREATE TRIGGER manager_apply_terminal_outcomes_no_update "
                    "BEFORE UPDATE ON manager_apply_terminal_outcomes "
                    "BEGIN SELECT RAISE(ABORT, "
                    "'manager_apply_terminal_outcomes is immutable'); END;");
  EXPECT_FALSE(ReadNativeApplyPreparationFailureReadOnly(
                   path, cell, fixture.plan, *candidate, fixture.claim,
                   fixture.query, fixture.receipt)
                   .ok());
}
TEST(ManagerApplyBringUpJournal,
     CapturesCompletePlanAtomicallyAndReplaysWithoutReminting) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "bring-up.sqlite3";
  const auto parent = Intent();
  auto plan = BringUpPlan(parent);
  ASSERT_TRUE(plan.valid());
  // There is no one-child restriction: the immutable configured subset may
  // contain several distinct actual original component requests.
  auto second = plan.children.front();
  second.intent.controllable_component_id = ControllableComponentId("tool-1");
  second.intent.request_context.request_id = RequestId("bring-up-tool-1");
  second.intent.intent = core::ComponentLifecycleIntentKindDTO::kEnable;
  plan.children.push_back(second);
  ASSERT_TRUE(plan.valid());
  std::optional<PersistedManagerApplyBringUpPlan> expected;
  {
    SqliteManagerApplyJournal journal(path, parent.cell_id);
    const auto inserted = journal.EnsureBringUpIntent(parent, plan);
    EXPECT_TRUE(inserted.inserted);
    expected = inserted.plan;
    EXPECT_EQ(inserted.intent.intent_digest,
              ComputeManagerApplyIntentDigest(parent));
    EXPECT_EQ(inserted.plan.plan_digest,
              ComputeManagerApplyBringUpPlanDigest(plan));
    EXPECT_FALSE(journal.EnsureBringUpIntent(parent, plan).inserted);
    EXPECT_EQ(journal.ReadBringUpPlanByRequest(
                  parent.request_context.request_owner_epoch,
                  parent.request_context.request_id),
              expected);
    const auto inventory = journal.ReadStatusInventory();
    ASSERT_EQ(inventory.size(), 1U);
    EXPECT_EQ(inventory.front().bring_up_plan, expected);
    EXPECT_TRUE(inventory.front().bring_up_child_proofs.empty());
    EXPECT_EQ(journal.VerifyIntegrity().bring_up_plan_count, 1);
    // An ordinary identity observation stays byte-identical. The coordinator
    // must classify the linked parent before any ordinary continuation.
    EXPECT_EQ(journal.EnsureIntent(parent).record, inserted.intent);
  }
  SqliteManagerApplyJournal restored(path, parent.cell_id);
  EXPECT_EQ(restored.ReadBringUpPlan(parent.apply_id, parent.apply_generation),
            expected);
}

TEST(ManagerApplyBringUpJournal, ReadOnlyClaimIntentResolvesOnlyExactRetainedParent) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "parent-claim-intent-read.sqlite3";
  const auto parent = Intent();
  const auto claim = [&] {
    SqliteManagerApplyJournal journal(path, parent.cell_id);
    const auto accepted_plan = journal.EnsureBringUpIntent(parent, BringUpPlan(parent));
    return core::CellAdmissionClaimIntentDTO{
        core::CellAdmissionClaimKindDTO::kManagerApply,
        core::CellAdmissionOwnerRoleDTO::kManager,
        parent.request_context.request_owner_epoch, parent.apply_id.value(),
        parent.request_context.request_id, accepted_plan.plan.plan_digest,
        *parent.expected_admission_generation, {}, std::nullopt, std::nullopt};
  }();
  const auto before = ReadFileBytes(path);
  const auto observed = ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, claim);
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  EXPECT_EQ(observed.value().intent.intent, parent);
  ASSERT_TRUE(observed.value().bring_up_plan);
  EXPECT_EQ(observed.value().bring_up_plan->plan_digest, claim.request_payload_digest);
  EXPECT_FALSE(observed.value().fence_attempt);
  EXPECT_FALSE(observed.value().terminal_outcome);
  const auto reject = [&](const auto &mutate) {
    auto foreign = claim;
    mutate(foreign);
    EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, foreign).ok());
  };
  reject([](auto &value) { value.owner_epoch = RequestOwnerEpoch("foreign-owner"); });
  reject([](auto &value) { value.request_id = RequestId("foreign-request"); });
  reject([](auto &value) { value.owner_transaction_id = "foreign-apply"; });
  reject([](auto &value) { value.request_payload_digest = ContentDigest(std::string(kDigestTwo)); });
  reject([](auto &value) { value.expected_admission_generation = core::AdmissionGeneration(8U); });
  reject([](auto &value) { value.owner_role = core::CellAdmissionOwnerRoleDTO::kAuthoringOwner; });
  EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, CellId("foreign-cell"), claim).ok());
  EXPECT_EQ(ReadFileBytes(path), before);
  // A live-WAL read may create SQLite bookkeeping; retained main bytes stay exact.
  const auto absent = directory.path() / "absent-claim-parent.sqlite3";
  EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(absent, parent.cell_id, claim).ok());
  EXPECT_FALSE(std::filesystem::exists(absent));
}

TEST(ManagerApplyBringUpJournal, ReadOnlyClaimIntentPositivelyDistinguishesOrdinaryParent) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "ordinary-parent-claim-read.sqlite3";
  const auto parent = Intent();
  const auto durable = [&] {
    SqliteManagerApplyJournal journal(path, parent.cell_id);
    return journal.EnsureIntent(parent).record;
  }();
  const core::CellAdmissionClaimIntentDTO claim{
      core::CellAdmissionClaimKindDTO::kManagerApply,
      core::CellAdmissionOwnerRoleDTO::kManager,
      parent.request_context.request_owner_epoch, parent.apply_id.value(),
      parent.request_context.request_id, durable.intent_digest,
      *parent.expected_admission_generation, {}, std::nullopt, std::nullopt};
  const auto before = ReadFileBytes(path);
  const auto ordinary = ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, claim);
  ASSERT_TRUE(ordinary.ok()) << ordinary.error().message;
  EXPECT_EQ(ordinary.value().intent, durable);
  EXPECT_FALSE(ordinary.value().bring_up_plan);
  auto foreign = claim;
  foreign.request_payload_digest = ContentDigest(std::string(kDigestTwo));
  EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, foreign).ok());
  foreign = claim;
  foreign.expected_admission_generation = core::AdmissionGeneration(8U);
  EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, foreign).ok());
  // A linked-only reference cannot turn this positive ordinary observation
  // into a configured child plan or an acquired claim.
  EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id,
      core::ApplyComponentBringUpReferenceDTO{{parent.apply_id, parent.apply_generation},
          durable.intent_digest}).ok());
  EXPECT_EQ(ReadFileBytes(path), before);
}

TEST(ManagerApplyJournal, ReadOnlyConfigurationParentKeepsDirectAndAuthoringGrammars) {
  TemporaryDirectory directory;
  for (const bool authoring : {false, true}) {
    const auto path = directory.path() / (authoring ? "authoring-config.sqlite3" : "direct-config.sqlite3");
    const auto parent = authoring ? ParentClaimIntent() : Intent();
    const auto durable = [&] {
      SqliteManagerApplyJournal journal(path, parent.cell_id);
      return journal.EnsureIntent(parent).record;
    }();
    const auto claim = parent.parent_authoring_claim.value_or(core::CellAdmissionClaimDTO{
        {core::CellAdmissionClaimKindDTO::kManagerApply, core::CellAdmissionOwnerRoleDTO::kManager,
         parent.request_context.request_owner_epoch, parent.apply_id.value(), parent.request_context.request_id,
         durable.intent_digest, core::AdmissionGeneration(7U), {}, std::nullopt, std::nullopt},
        core::CellAdmissionClaimGeneration(3U), core::AdmissionGeneration(7U), 42});
    const core::NamedApplyPublicationTransactionDTO apply{parent.apply_id, parent.apply_generation};
    std::optional<core::CellAdmissionClaimDTO> other_authoring_claim;
    if (authoring) {
      auto other = ParentClaimIntent();
      other.apply_id = ApplyId("apply-2");
      other.request_context.request_id = RequestId("request-2");
      other.parent_authoring_claim->intent.owner_transaction_id = "composer-transaction-2";
      other.parent_authoring_claim->intent.request_id = RequestId("composer-request-2");
      other.parent_authoring_claim->intent.request_payload_digest = ContentDigest(std::string(kDigestThree));
      other.parent_authoring_claim->claim_generation = core::CellAdmissionClaimGeneration(4U);
      SqliteManagerApplyJournal journal(path, parent.cell_id);
      EXPECT_EQ(journal.EnsureIntent(other).record.intent, other);
      other_authoring_claim = other.parent_authoring_claim;
    }
    const auto before = ReadFileBytes(path);
    const auto observed = ReadManagerApplyConfigurationStatusReadOnly(path, parent.cell_id, apply, claim);
    ASSERT_TRUE(observed.ok()) << observed.error().message;
    EXPECT_EQ(observed.value().intent, durable);
    EXPECT_FALSE(observed.value().bring_up_plan);
    auto foreign = claim;
    if (authoring) foreign.claim_generation = core::CellAdmissionClaimGeneration(4U);
    else foreign.intent.request_payload_digest = ContentDigest(std::string(kDigestTwo));
    const auto refused = ReadManagerApplyConfigurationStatusReadOnly(path, parent.cell_id, apply, foreign);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("integrity_detail"), authoring
        ? "configuration authoring parent claim mismatch" : "configuration direct parent intent mismatch");
    const auto missing = ReadManagerApplyConfigurationStatusReadOnly(path, parent.cell_id,
        {parent.apply_id, ApplyGeneration(2U)}, claim);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().details.at("integrity_detail"), "ordinary configuration parent missing");
    if (other_authoring_claim) {
      const auto foreign_apply = ReadManagerApplyConfigurationStatusReadOnly(
          path, parent.cell_id, apply, *other_authoring_claim);
      ASSERT_FALSE(foreign_apply.ok());
      EXPECT_EQ(foreign_apply.error().details.at("integrity_detail"),
                "configuration authoring parent claim mismatch");
    }
    EXPECT_EQ(ReadFileBytes(path), before);
  }
}

TEST(ManagerApplyJournal, ReadOnlyConfigurationParentRefusesLinkedPlanWithoutChangingBytes) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "linked-is-not-config.sqlite3";
  const auto parent = Intent();
  const auto linked = [&] {
    SqliteManagerApplyJournal journal(path, parent.cell_id);
    return journal.EnsureBringUpIntent(parent, BringUpPlan(parent));
  }();
  const core::CellAdmissionClaimDTO claim{
      {core::CellAdmissionClaimKindDTO::kManagerApply, core::CellAdmissionOwnerRoleDTO::kManager,
       parent.request_context.request_owner_epoch, parent.apply_id.value(), parent.request_context.request_id,
       linked.plan.plan_digest, *parent.expected_admission_generation, {}, std::nullopt, std::nullopt},
      core::CellAdmissionClaimGeneration(3U), core::AdmissionGeneration(7U), 42};
  const auto before = ReadFileBytes(path);
  const auto refused = ReadManagerApplyConfigurationStatusReadOnly(path, parent.cell_id,
      {parent.apply_id, parent.apply_generation}, claim);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("integrity_detail"), "configuration parent has linked bring-up plan");
  EXPECT_EQ(ReadFileBytes(path), before);
}

TEST(ManagerApplyBringUpJournal, ReadOnlyParentJoinsExactPlanWithoutCreatingAuthority) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "parent-read.sqlite3";
  const auto parent = Intent();
  const core::ApplyFenceOwnerDTO owner{parent.apply_id, parent.apply_generation};
  core::ApplyComponentBringUpReferenceDTO reference{
      owner,
      core::ContentDigest(std::string(kDigestOne))};
  {
    SqliteManagerApplyJournal journal(path, parent.cell_id);
    const auto accepted = journal.EnsureBringUpIntent(parent, BringUpPlan(parent));
    reference.plan_digest = accepted.plan.plan_digest;
    const auto current = journal.ReadStatus(parent.apply_id, parent.apply_generation);
    ASSERT_TRUE(current);
    const auto observed = ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, reference);
    ASSERT_TRUE(observed.ok()) << observed.error().message;
    EXPECT_EQ(observed.value().intent, current->intent);
    EXPECT_FALSE(observed.value().affected_scope);
    EXPECT_FALSE(current->affected_scope);
    EXPECT_FALSE(observed.value().fence_attempt);
    EXPECT_FALSE(current->fence_attempt);
    EXPECT_FALSE(observed.value().fence_acceptance);
    EXPECT_FALSE(current->fence_acceptance);
    EXPECT_FALSE(observed.value().runtime_convergence_intent);
    EXPECT_FALSE(current->runtime_convergence_intent);
    EXPECT_FALSE(observed.value().latest_runtime_staged_ready);
    EXPECT_FALSE(current->latest_runtime_staged_ready);
    EXPECT_FALSE(observed.value().private_materialization);
    EXPECT_FALSE(current->private_materialization);
    EXPECT_FALSE(observed.value().publication_receipt);
    EXPECT_FALSE(current->publication_receipt);
    EXPECT_FALSE(observed.value().fence_release_intent);
    EXPECT_FALSE(current->fence_release_intent);
    EXPECT_FALSE(observed.value().fence_released);
    EXPECT_FALSE(current->fence_released);
    EXPECT_FALSE(observed.value().terminal_outcome);
    EXPECT_FALSE(current->terminal_outcome);
    EXPECT_EQ(observed.value().bring_up_plan, current->bring_up_plan);
    EXPECT_TRUE(observed.value().bring_up_child_proofs.empty());
    EXPECT_TRUE(current->bring_up_child_proofs.empty());
    EXPECT_FALSE(observed.value().fence_acceptance);
    EXPECT_FALSE(observed.value().terminal_outcome);
    auto foreign = reference;
    foreign.plan_digest = core::ContentDigest(std::string(kDigestTwo));
    EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, foreign).ok());
    foreign = reference;
    foreign.owner.apply_generation = core::ApplyGeneration(parent.apply_generation.value() + 1U);
    EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, foreign).ok());
    EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(path, CellId("foreign-cell"), reference).ok());
    EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count, 1);
  }
  const auto before = ReadFileBytes(path);
  const auto reopened = ReadManagerApplyBringUpStatusReadOnly(path, parent.cell_id, reference);
  ASSERT_TRUE(reopened.ok());
  EXPECT_EQ(reopened.value().bring_up_plan->plan_digest, reference.plan_digest);
  EXPECT_EQ(ReadFileBytes(path), before);
  // A live-WAL read may create SQLite bookkeeping; retained main bytes stay exact.
  const auto absent_path = directory.path() / "never-created.sqlite3";
  EXPECT_FALSE(ReadManagerApplyBringUpStatusReadOnly(absent_path, parent.cell_id, reference).ok());
  EXPECT_FALSE(std::filesystem::exists(absent_path));
}

TEST(ManagerApplyBringUpJournal,
     ConflictingOrInvalidPlanLeavesNoUnlinkedParent) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "atomic.sqlite3",
                                    CellId("cell-1"));
  const auto parent = Intent();
  auto plan = BringUpPlan(parent);
  plan.expected_publication.candidate_projection_revision.scene_revision
      .revision_id += 1U;
  plan.children.front().candidate_projection_revision =
      plan.expected_publication.candidate_projection_revision;
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpIntent(parent, plan)),
               ManagerApplyJournalIntegrityError);
  EXPECT_FALSE(
      journal.ReadIntent(parent.apply_id, parent.apply_generation).has_value());
  EXPECT_FALSE(journal.ReadBringUpPlan(parent.apply_id, parent.apply_generation)
                   .has_value());
  EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count, 0);
  plan = BringUpPlan(parent);
  const auto original = journal.EnsureBringUpIntent(parent, plan);
  plan.children.front().runtime_epoch = RuntimeEpoch("foreign-host-epoch");
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpIntent(parent, plan)),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.ReadBringUpPlan(parent.apply_id, parent.apply_generation),
            original.plan);
}

TEST(ManagerApplyBringUpJournal,
     DoesNotRetrofitOrRepurposeOrdinaryAuthoringAuthority) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "ordinary.sqlite3",
                                    CellId("cell-1"));
  const auto parent = Intent();
  const auto ordinary = journal.EnsureIntent(parent);
  EXPECT_THROW(static_cast<void>(
                   journal.EnsureBringUpIntent(parent, BringUpPlan(parent))),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.ReadIntent(parent.apply_id, parent.apply_generation),
            ordinary.record);
  EXPECT_FALSE(journal.ReadBringUpPlan(parent.apply_id, parent.apply_generation)
                   .has_value());
  const auto authoring = ParentClaimIntent();
  auto plan = BringUpPlan(authoring);
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpIntent(authoring, plan)),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyBringUpJournal,
     LinkedMaterializationRequiresActualEveryChildHoldingProof) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "holding.sqlite3",
                                    CellId("cell-1"));
  const auto plan = BringUpPlan();
  static_cast<void>(journal.EnsureBringUpIntent(Intent(), plan));
  const auto &child = plan.children.front();
  const ManagerApplyPrivateMaterializationDTO materialization{
      child.candidate_projection_revision.scene_revision.content_hash,
      plan.apply_id,
      plan.apply_generation,
      child.candidate_projection_revision,
      child.candidate_retained_bundle_digest,
      child.candidate_retained_bundle_bytes};
  EXPECT_THROW(
      static_cast<void>(journal.EnsurePrivateMaterialization(materialization)),
      ManagerApplyJournalIntegrityError);
  EXPECT_FALSE(
      journal.ReadPrivateMaterialization(plan.apply_id, plan.apply_generation)
          .has_value());
  EXPECT_EQ(journal.VerifyIntegrity().private_materialization_count, 0);
}

TEST(ManagerApplyBringUpJournal,
     RetiredDomainHoldingRequiresNoAttemptOrError) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "retired.sqlite3",
                                    CellId("cell-1"));
  const auto parent = journal.EnsureBringUpIntent(Intent(), BringUpPlan());
  auto intent = FenceIntent();
  intent.scope.domains.push_back({ControlDomainId("retired"),
                                  ControlDomainRevision("retired-r1"), std::nullopt});
  intent.scope.bindings.push_back({ControlBindingId("retired-jtc"),
                                   ControlBindingRevision("retired-binding-r1"), std::nullopt});
  intent.scope.resources.push_back(ResourceId("joint-3"));
  // A controller domain with a predecessor names its active configuration.
  intent.expected_active_control_configurations.push_back(
      {ControlDomainId("retired"), core::ActiveControlConfigurationId("retired-mode"),
       core::ActiveControlConfigurationGeneration(1U)});
  const auto attempt = EnsureAttempt(journal, parent.intent.intent_digest, intent);
  auto accepted_fence = AcceptedFence();
  accepted_fence.captured_scope = intent.scope;
  const auto fence = journal.EnsureFenceAcceptance(
      parent.intent.intent.apply_id, parent.intent.intent.apply_generation,
      attempt.record.fence_attempt_digest, accepted_fence).record;
  const auto proof_with_retired = [&](ManagerApplyBringUpChildPhase phase) {
    auto proof = BringUpChildProof(parent, fence, phase);
    auto &status = *proof.result.status;
    const core::ResolvedLifecycleControlDomainScopeDTO scope{
        ControlDomainId("retired"), ControlDomainRevision("retired-r1"),
        core::ControllerTopologyId("topology-1"), core::TopologyRevision("topology-revision-1"),
        std::nullopt,
        {{ControlBindingId("retired-jtc"), ControlBindingRevision("retired-binding-r1"),
          core::ControlRouteId("route-1")}},
        {ResourceId("joint-3")}};
    const auto &child = parent.plan.plan.children.front();
    const core::ResolvedLifecycleProviderOccurrenceDTO provider{
        scope.control_domain_id, scope.control_domain_revision,
        core::ProviderInstanceId("provider-1"), core::ProviderManifestId("manifest-1"),
        core::ProviderManifestRevision("manifest-r1"), child.runtime_unit_id,
        child.runtime_epoch, ContentDigest(std::string(kDigestThree))};
    status.domains.push_back({scope.control_domain_id, std::nullopt, scope, provider,
        core::ComponentLifecycleStateDTO::kDeclared,
        core::ComponentLifecycleStateDTO::kDeclared, status.requested_state,
        core::ComponentLifecycleDomainActionStateDTO::kNotStarted, {}, std::nullopt});
    return proof;
  };
  const auto accepted = proof_with_retired(ManagerApplyBringUpChildPhase::kAccepted);
  const auto holding = proof_with_retired(ManagerApplyBringUpChildPhase::kHolding);
  ASSERT_TRUE(accepted.result.locally_valid());
  ASSERT_TRUE(holding.result.locally_valid());
  static_cast<void>(journal.EnsureBringUpChildProof(accepted));

  auto attempted = holding;
  auto &attempted_status = *attempted.result.status;
  auto &retired = attempted_status.domains.back();
  const auto &provider = *retired.resolved_provider_occurrence;
  retired.primitive_attempts.push_back({
      attempted_status.acceptance.lifecycle_transaction_id,
      attempted_status.acceptance.lifecycle_transaction_generation,
      attempted_status.accepted_intent.controllable_component_id,
      retired.control_domain_id, provider.control_domain_revision,
      provider.provider_instance_id, provider.provider_manifest_id,
      provider.provider_manifest_revision, provider.provider_runtime_unit_id,
      provider.provider_runtime_epoch, core::LifecyclePrimitiveId("transition.prepare"),
      core::LifecyclePrimitiveAttemptId("retired-attempt"), 1U,
      core::LifecycleTransitionOutcomeDTO::kSucceeded, std::nullopt});
  ASSERT_TRUE(attempted.result.locally_valid());
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpChildProof(attempted)),
               ManagerApplyJournalIntegrityError);

  auto failed = holding;
  failed.result.status->domains.back().action_state = core::ComponentLifecycleDomainActionStateDTO::kFailed;
  failed.result.status->domains.back().error = core::ErrorDTO{"retired_error", "retired domain failed", {}};
  ASSERT_TRUE(failed.result.locally_valid());
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpChildProof(failed)),
               ManagerApplyJournalIntegrityError);
  EXPECT_TRUE(journal.EnsureBringUpChildProof(holding).inserted);
}

TEST(ManagerApplyBringUpJournal,
     RetainsExactNoAdmissionRejectionAndRefusesForeignChild) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "rejection.sqlite3",
                                    CellId("cell-1"));
  const auto accepted = journal.EnsureBringUpIntent(Intent(), BringUpPlan());
  ManagerApplyBringUpChildProofDTO proof{
      accepted.plan.plan.apply_id,
      accepted.plan.plan.apply_generation,
      accepted.plan.plan_digest,
      ControllableComponentId("robot-1"),
      ManagerApplyBringUpChildPhase::kAccepted,
      {core::RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt,
       core::ErrorDTO{"not_qualified", "actual owner refused bring-up", {{"effect", "not_started"}}},
       std::nullopt}};
  ASSERT_TRUE(proof.result.locally_valid());
  const auto retained = journal.EnsureBringUpChildProof(proof);
  EXPECT_TRUE(retained.inserted);
  EXPECT_FALSE(journal.EnsureBringUpChildProof(proof).inserted);
  const auto read = journal.ReadBringUpChildProof(
      proof.apply_id, proof.apply_generation, proof.controllable_component_id,
      proof.phase);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->proof_digest, retained.record.proof_digest);
  EXPECT_EQ(read->proof.result.rejection, proof.result.rejection);
  proof.controllable_component_id = ControllableComponentId("tool-1");
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpChildProof(proof)),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(journal.VerifyIntegrity().bring_up_child_proof_count, 1);
}

TEST(ManagerApplyBringUpJournal, ChangedPlanCannotHideBehindVerifiedReadCache) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "tamper-plan.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto accepted = journal.EnsureBringUpIntent(Intent(), BringUpPlan());
  ASSERT_TRUE(journal
                  .ReadBringUpPlan(accepted.plan.plan.apply_id,
                                   accepted.plan.plan.apply_generation)
                  .has_value());
  ExecSql(path, "DROP TRIGGER manager_apply_bring_up_plans_no_update;"
                "UPDATE manager_apply_bring_up_plans SET plan_encoding=X'00';"
                "CREATE TRIGGER manager_apply_bring_up_plans_no_update BEFORE "
                "UPDATE ON manager_apply_bring_up_plans "
                "BEGIN SELECT RAISE(ABORT, 'manager_apply_bring_up_plans is "
                "immutable'); END;");
  EXPECT_THROW(
      static_cast<void>(journal.ReadBringUpPlan(
          accepted.plan.plan.apply_id, accepted.plan.plan.apply_generation)),
      ManagerApplyJournalIntegrityError);
  EXPECT_THROW(static_cast<void>(journal.ReadStatusInventory()),
               ManagerApplyJournalIntegrityError);
  EXPECT_THROW(static_cast<void>(journal.VerifyIntegrity()),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyBringUpJournal,
     JoinsActualHoldingPublicationTerminalFenceAndParentClaimForRequestedTargets) {
  for (const auto kind : {core::ComponentLifecycleIntentKindDTO::kPrepare,
                          core::ComponentLifecycleIntentKindDTO::kEnable,
                          core::ComponentLifecycleIntentKindDTO::kDisable}) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "settlement.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto parent = journal.EnsureBringUpIntent(Intent(), BringUpPlan(Intent(), kind));
  const auto attempt = EnsureAttempt(journal, parent.intent.intent_digest);
  const auto fence =
      journal
          .EnsureFenceAcceptance(parent.intent.intent.apply_id,
                                 parent.intent.intent.apply_generation,
                                 attempt.record.fence_attempt_digest,
                                 AcceptedFence())
          .record;
  const auto accepted = BringUpChildProof(
      parent, fence, ManagerApplyBringUpChildPhase::kAccepted);
  const auto holding =
      BringUpChildProof(parent, fence, ManagerApplyBringUpChildPhase::kHolding);
  const auto terminal = BringUpChildProof(
      parent, fence, ManagerApplyBringUpChildPhase::kTerminal);
  ASSERT_TRUE(accepted.result.locally_valid());
  ASSERT_TRUE(holding.result.locally_valid());
  ASSERT_TRUE(terminal.result.locally_valid());
  static_cast<void>(journal.EnsureBringUpChildProof(accepted));
  auto foreign = holding;
  foreign.result.status->apply_bring_up->parent_authority.parent_claim
      .claim_generation = core::CellAdmissionClaimGeneration(9U);
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpChildProof(foreign)),
               ManagerApplyJournalIntegrityError);
  static_cast<void>(journal.EnsureBringUpChildProof(holding));
  EXPECT_THROW(static_cast<void>(journal.EnsureBringUpChildProof(terminal)),
               ManagerApplyJournalIntegrityError);
  const auto &child = parent.plan.plan.children.front();
  const auto materialization =
      journal
          .EnsurePrivateMaterialization(
              {child.candidate_projection_revision.scene_revision.content_hash,
               parent.intent.intent.apply_id,
               parent.intent.intent.apply_generation,
               child.candidate_projection_revision,
               child.candidate_retained_bundle_digest,
               child.candidate_retained_bundle_bytes})
          .record;
  // Selected proof is counted only for this exact linked parent. It does not
  // fabricate an ordinary runtime staging record to satisfy cardinality.
  EXPECT_EQ(journal.VerifyIntegrity().runtime_staged_ready_count, 0);
  EXPECT_EQ(journal.VerifyIntegrity().private_materialization_count, 1);
  const auto publication =
      journal
          .EnsurePublicationReceipt(
              {materialization.private_materialization_content_digest,
               {parent.plan.plan.expected_publication,
                ActiveBundlePublicationGeneration(4U),
                ContentDigest(std::string(kDigestOne))}})
          .record;
  const SelectedBundleTransitionRecords transition{materialization,
                                                   publication};
  EXPECT_THROW(static_cast<void>(journal.EnsureFenceReleaseIntent(
                   FenceReleaseIntent(transition, fence))),
               ManagerApplyJournalIntegrityError);
  static_cast<void>(journal.EnsureBringUpChildProof(terminal));
  const auto completed = EnsureApplyCompletion(journal, transition);
  const auto claim =
      accepted.result.status->apply_bring_up->parent_authority.parent_claim;
  core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
       completed.terminal.terminal_outcome_digest}};
  EXPECT_FALSE(ValidateManagerApplyCellAdmissionSettlementReadOnly(
                   path, CellId("cell-1"), claim, release)
                   .ok());
  // Even an ordinary-looking intent-digest claim cannot classify this linked
  // parent through the ordinary settlement reader.
  auto ordinary_claim = claim;
  ordinary_claim.intent.request_payload_digest = parent.intent.intent_digest;
  auto ordinary_release = release;
  ordinary_release.request_payload_digest = parent.intent.intent_digest;
  EXPECT_FALSE(ValidateManagerApplyCellAdmissionSettlementReadOnly(
                   path, CellId("cell-1"), ordinary_claim, ordinary_release).ok());
  EXPECT_TRUE(ValidateManagerApplyBringUpCellAdmissionSettlementReadOnly(
                  path, CellId("cell-1"), claim, release)
                  .ok());
  release.claim_generation = core::CellAdmissionClaimGeneration(9U);
  EXPECT_FALSE(ValidateManagerApplyBringUpCellAdmissionSettlementReadOnly(
                   path, CellId("cell-1"), claim, release)
                   .ok());
  const auto inventory = journal.ReadStatusInventory();
  ASSERT_EQ(inventory.size(), 1U);
  EXPECT_EQ(inventory.front().bring_up_child_proofs.size(), 3U);
  EXPECT_EQ(journal.VerifyIntegrity().bring_up_child_proof_count, 3);
  }
}

TEST(ManagerApplyJournalIntegrity,
     RefusesV19InterruptedIntentWithActionablePreservationMessage) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "interrupted-v19.sqlite3";
  {
    SqliteManagerApplyJournal owner(path, CellId("cell-1"));
    static_cast<void>(owner.EnsureIntent(Intent()));
  }
  ExecSql(path, "PRAGMA user_version=19;");
  const auto before = ReadFileBytes(path);
  try {
    SqliteManagerApplyJournal refused(path, CellId("cell-1"));
    FAIL() << "older interrupted journal must be refused";
  } catch (const ManagerApplyJournalIntegrityError &error) {
    const std::string message(error.what());
    EXPECT_NE(message.find(path.string()), std::string::npos);
    EXPECT_NE(message.find("version 19"), std::string::npos);
    EXPECT_NE(message.find("v20"), std::string::npos);
    EXPECT_NE(message.find("Preserve"), std::string::npos);
    EXPECT_NE(message.find("interrupted"), std::string::npos);
    EXPECT_NE(message.find("reconcile"), std::string::npos);
  }
  EXPECT_EQ(ReadFileBytes(path), before);
  ExpectNoSqliteSidecars(path);
}

TEST(ManagerApplyJournal, InitializingPurposeSurvivesJournalReopen) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "initializing.sqlite3";
  auto intent = Intent();
  intent.scene_initialization = ManagerSceneInitialization::kInitialize;
  {
    SqliteManagerApplyJournal journal(path, intent.cell_id);
    EXPECT_TRUE(journal.EnsureIntent(intent).inserted);
  }
  SqliteManagerApplyJournal reopened(path, intent.cell_id);
  const auto stored =
      reopened.ReadIntent(intent.apply_id, intent.apply_generation);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->intent, intent);
  EXPECT_EQ(stored->intent.scene_initialization,
            ManagerSceneInitialization::kInitialize);
  EXPECT_FALSE(reopened.EnsureIntent(intent).inserted);
}

TEST(ManagerApplyJournal,
     ProviderFenceProofSurvivesDurableScopeAndAttemptReopen) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "provider-fence.sqlite3";
  auto fence = FenceIntent();
  const core::ProviderConfigurationFenceOccurrenceDTO occurrence{
      ControllableComponentId("robot-1"),
      core::ProviderInstanceId("provider.io"),
      ContentDigest(std::string(kDigestOne)),
      {ResourceId("io.output")}};
  fence.scope = {
      {occurrence.controllable_component_id},
      {{ControlDomainId("io.domain"), ControlDomainRevision("old"),
        ControlDomainRevision("new"),
        core::ProviderConfigurationFenceDomainDTO{occurrence, occurrence}}},
      {},
      occurrence.output_resources};
  fence.expected_active_control_configurations.clear();
  fence.expected_provider_lineages = {
      {ControlDomainId("io.domain"),
       core::RuntimeEpoch("actual.provider.epoch"),
       core::ComponentLifecycleGeneration(7U)}};
  ASSERT_TRUE(fence.valid());
  {
    SqliteManagerApplyJournal journal(database, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    const auto affected =
        journal.EnsureAffectedScope(intent.intent_digest, AffectedScope(fence))
            .record;
    const auto attempt =
        journal
            .EnsureFenceAttempt(intent.intent_digest,
                                affected.affected_scope_digest, fence)
            .record;
    EXPECT_EQ(attempt.fence_intent.scope, fence.scope);
  }
  SqliteManagerApplyJournal reopened(database, CellId("cell-1"));
  const auto scope =
      reopened.ReadAffectedScope(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(scope);
  EXPECT_EQ(scope->affected_scope.scope, fence.scope);
  const auto attempt =
      reopened.ReadFenceAttempt(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(attempt);
  EXPECT_EQ(attempt->fence_intent.scope, fence.scope);
  EXPECT_EQ(attempt->fence_intent.expected_provider_lineages,
            fence.expected_provider_lineages);
  EXPECT_TRUE(
      attempt->fence_intent.expected_active_control_configurations.empty());
}

} // namespace manyforge::component_manager
