#include <gtest/gtest.h>
#include <sqlite3.h>

// Storage revision 11 and 12 objects, absent from every older journal shape.
#define MANYFORGE_DROP_NATIVE_APPLY_TABLES                                              \
  "DROP TABLE cclc_native_apply_adoptions; DROP TABLE cclc_native_apply_plans;"         \
  "DROP TABLE cclc_native_apply_plan_components; DROP TABLE cclc_native_apply_plan_routes;" \
  "DROP TABLE cclc_native_apply_plan_removals; DROP TABLE cclc_native_apply_reservations;"
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../src/component_lifecycle_journal_codec_internal.hpp"

namespace manyforge::core {
namespace {

[[nodiscard]] ActiveBundlePublicationReceiptDTO NativeApplyPublication(
    const std::string& apply_id, std::uint64_t apply_generation,
    const std::string& bundle, std::uint64_t publication_generation);
[[nodiscard]] NativeApplyPlanRequestDTO NativeApplyPlanRequest(
    const std::string& apply_id, std::uint64_t apply_generation,
    const std::string& bundle, const ActiveControlConfigurationDTO& route);

[[nodiscard]] Result<CCLCJournalRecoveryDTO> BeginTestOwnerEpoch(SqliteCCLCJournal& journal,
                                                                 RequestOwnerEpoch& seed_then_owner,
                                                                 std::int64_t started_at_ns,
                                                                 std::size_t capacity) {
  auto result = journal.BeginOwnerEpoch(seed_then_owner, started_at_ns, capacity);
  if (result.ok()) seed_then_owner = result.value().request_owner_epoch;
  return result;
}

[[nodiscard]] Result<CellAdmissionClaimDTO> SeedLifecycleClaim(
    SqliteCCLCJournal& journal, const RequestOwnerEpoch& owner,
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAcceptedDTO& acceptance) {
  const CellAdmissionClaimIntentDTO claim_intent{
      CellAdmissionClaimKindDTO::kComponentLifecycle,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      owner,
      acceptance.lifecycle_transaction_id.value(),
      intent.request_context.request_id,
      ContentDigest(detail::ComponentLifecycleSemanticIntentDigest(intent)),
      intent.expected_admission_generation,
      {},
      std::nullopt,
      std::nullopt,
      intent.program_node_evidence};
  auto recorded = journal.RecordCellClaimAttempt(owner, claim_intent);
  if (!recorded.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(recorded.error());
  }
  CellAdmissionClaimDTO claim{claim_intent, CellAdmissionClaimGeneration(1U),
                              AdmissionGeneration(1U), 0};
  auto acquired = journal.RecordCellClaimAcquired(owner, claim);
  return acquired.ok() ? Result<CellAdmissionClaimDTO>::Ok(std::move(claim))
                       : Result<CellAdmissionClaimDTO>::Err(acquired.error());
}

[[nodiscard]] Result<void> ReleaseUnacceptedTestClaim(SqliteCCLCJournal& journal,
                                                      const RequestOwnerEpoch& owner,
                                                      const CellAdmissionClaimDTO& claim) {
  const CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {CellAdmissionSettlementKindDTO::kProvenNoEffect, claim.intent.request_payload_digest}};
  const auto recorded = journal.RecordUnacceptedCellClaimRelease(owner, release);
  if (!recorded.ok()) return recorded;
  return journal.RecordCellClaimReleaseReceipt(
      owner, release,
      {CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact,
       RequestOwnerEpoch("kernel.1"),
       AdmissionGeneration(2U),
       claim,
       release,
       std::nullopt,
       {},
       {}});
}

[[nodiscard]] CellAdmissionClaimIntentDTO RouteClaimIntent(
    const RequestOwnerEpoch& owner, const PreparedRouteSelectionAdmission& prepared,
    bool recovery) {
  const auto& intent = prepared.initial_status.accepted_intent;
  CellAdmissionClaimIntentDTO claim_intent{
      CellAdmissionClaimKindDTO::kComponentLifecycle,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      owner,
      prepared.acceptance.route_selection_transaction_id.value(),
      intent.request_context.request_id,
      ContentDigest(detail::RouteSelectionSemanticIntentDigest(intent)),
      *intent.expected_admission_generation,
      {},
      {},
      {},
      intent.program_node_evidence};
  if (recovery) {
    claim_intent.claim_kind = CellAdmissionClaimKindDTO::kInterventionOrRecovery;
    claim_intent.scope.configurations = {
        {intent.desired_configuration.control_domain_id,
         *intent.expected_active_control_configuration.active_control_configuration_id,
         *intent.expected_active_control_configuration.active_control_configuration_generation}};
    claim_intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
        claim_intent.owner_role, owner, claim_intent.owner_transaction_id,
        claim_intent.request_payload_digest, std::nullopt};
  }
  return claim_intent;
}

[[nodiscard]] Result<CellAdmissionClaimDTO> SeedRouteClaim(
    SqliteCCLCJournal& journal, const RequestOwnerEpoch& owner,
    const PreparedRouteSelectionAdmission& prepared, bool recovery = false) {
  const auto claim_intent = RouteClaimIntent(owner, prepared, recovery);
  const auto recorded = journal.RecordCellClaimAttempt(owner, claim_intent);
  if (!recorded.ok()) return Result<CellAdmissionClaimDTO>::Err(recorded.error());
  CellAdmissionClaimDTO claim{claim_intent, CellAdmissionClaimGeneration(1U),
                              AdmissionGeneration(1U), 0};
  const auto acquired = journal.RecordCellClaimAcquired(owner, claim);
  return acquired.ok() ? Result<CellAdmissionClaimDTO>::Ok(claim)
                       : Result<CellAdmissionClaimDTO>::Err(acquired.error());
}

[[nodiscard]] CellAdmissionClaimReleaseDTO RouteClaimRelease(
    const CellAdmissionClaimDTO& claim, const RouteSelectionStatusDTO& status) {
  return {claim.intent.owner_role,
          claim.intent.owner_epoch,
          claim.intent.owner_transaction_id,
          claim.intent.request_id,
          claim.intent.request_payload_digest,
          claim.acquired_admission_generation,
          claim.claim_generation,
          {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
           ContentDigest(detail::RouteSelectionStatusReleasePreconditionDigest(status))}};
}

class ScopedTemporaryDirectory final {
 public:
  ScopedTemporaryDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    path_ =
        std::filesystem::temp_directory_path() /
        ("manyforge-cclc-journal-test-" + std::to_string(static_cast<std::uint64_t>(::getpid())) +
         "-" + std::to_string(next.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create CCLC journal test root");
    }
  }

  ~ScopedTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] ExpectedControlBindingRevisionDTO Binding() {
  return {ControlBindingId("binding.1"), ControlBindingRevision("binding-revision.1"),
          ControlRouteId("route.1")};
}

[[nodiscard]] ExpectedControlDomainRevisionDTO ExpectedDomain() {
  return {ControlDomainId("domain.1"),
          ControlDomainRevision("domain-revision.1"),
          ControllerTopologyId("topology.1"),
          TopologyRevision("topology-revision.1"),
          {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt},
          {Binding()}};
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO CurrentScope() {
  const auto expected = ExpectedDomain();
  return {expected.control_domain_id,
          expected.control_domain_revision,
          expected.controller_topology_id,
          expected.topology_revision,
          std::nullopt,
          expected.expected_control_bindings,
          {ResourceId("resource.1")}};
}

[[nodiscard]] ExpectedControlDomainRevisionDTO ExpectedDisableDomain() {
  auto expected = ExpectedDomain();
  expected.expected_active_control_configuration = {
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      ActiveControlConfigurationId("configuration.1"), ActiveControlConfigurationGeneration(7U)};
  return expected;
}

[[nodiscard]] ActiveControlConfigurationDTO ActiveConfiguration(
    const ExpectedControlDomainRevisionDTO& expected) {
  return {ActiveControlConfigurationId("configuration.1"),
          expected.control_domain_id,
          expected.control_domain_revision,
          {{expected.expected_control_bindings.front().control_binding_id,
            expected.expected_control_bindings.front().control_binding_revision,
            expected.expected_control_bindings.front().control_route_id}},
          ActivationSetId("activation-set.1"),
          expected.controller_topology_id,
          expected.topology_revision,
          RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime-epoch.1"),
          1U,
          {},
          ActiveControlConfigurationGeneration(7U),
          ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO CurrentDisableScope() {
  const auto expected = ExpectedDisableDomain();
  return {expected.control_domain_id,      expected.control_domain_revision,
          expected.controller_topology_id, expected.topology_revision,
          ActiveConfiguration(expected),   expected.expected_control_bindings,
          {ResourceId("resource.1")}};
}

[[nodiscard]] ResolvedLifecycleProviderOccurrenceDTO CurrentProviderOccurrence() {
  return {ControlDomainId("domain.1"),
          ControlDomainRevision("domain-revision.1"),
          ProviderInstanceId("provider.1"),
          ProviderManifestId("manifest.1"),
          ProviderManifestRevision("manifest-revision.1"),
          RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime-epoch.1"),
          ContentDigest("catalog-digest.1")};
}

[[nodiscard]] ConfigurationFenceScopeDTO DisableFenceScope() {
  const auto expected = ExpectedDisableDomain();
  return {{ControllableComponentId("component.1")},
          {{expected.control_domain_id, expected.control_domain_revision, std::nullopt}},
          {{expected.expected_control_bindings.front().control_binding_id,
            expected.expected_control_bindings.front().control_binding_revision, std::nullopt}},
          {ResourceId("resource.1")}};
}

[[nodiscard]] ConfigurationFenceScopeDTO FullDisableFenceScope() {
  auto scope = DisableFenceScope();
  scope.controllable_component_ids.push_back(ControllableComponentId("component.2"));
  scope.domains.push_back(
      {ControlDomainId("domain.2"), ControlDomainRevision("domain-revision.2"), std::nullopt});
  scope.bindings.push_back(
      {ControlBindingId("binding.2"), ControlBindingRevision("binding-revision.2"), std::nullopt});
  scope.resources.push_back(ResourceId("resource.2"));
  return scope;
}

[[nodiscard]] ComponentLifecycleRequestedStateDTO RequestedPrepared() {
  return {ComponentLifecycleRequestedStatePresenceDTO::kResolved,
          ComponentLifecycleStateDTO::kPrepared};
}

[[nodiscard]] ComponentLifecycleIntentDTO Intent(std::string request_id,
                                                 const RequestOwnerEpoch& owner_epoch,
                                                 std::uint64_t expected_component_generation,
                                                 std::string reason = "prepare") {
  return {{RequestId(std::move(request_id)), owner_epoch, "journal-test", std::nullopt},
          ControllableComponentId("component.1"),
          ComponentLifecycleIntentKindDTO::kPrepare,
          GraphRevision("graph-revision.1"),
          ComponentLifecycleGeneration(expected_component_generation),
          {ExpectedDomain()},
          std::move(reason),
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt}};
}

[[nodiscard]] ComponentLifecycleIntentDTO DisableIntent(
    std::string request_id, const RequestOwnerEpoch& owner_epoch,
    std::uint64_t expected_component_generation) {
  auto result =
      Intent(std::move(request_id), owner_epoch, expected_component_generation, "disable");
  result.intent = ComponentLifecycleIntentKindDTO::kDisable;
  result.expected_control_domains = {ExpectedDisableDomain()};
  return result;
}

[[nodiscard]] ComponentLifecycleAcceptedDTO Acceptance(const ComponentLifecycleIntentDTO& intent,
                                                       std::uint64_t transaction_generation,
                                                       std::string transaction_id) {
  return {intent.request_context.request_id,
          RequestReplayStatusDTO::kAcceptedNew,
          LifecycleTransactionId(std::move(transaction_id)),
          LifecycleTransactionGeneration(transaction_generation),
          ComponentLifecycleGeneration(intent.expected_component_lifecycle_generation.value() + 1U),
          100,
          1'000'000};
}

[[nodiscard]] ComponentLifecycleDomainStatusDTO InitialDomain() {
  const auto expected = ExpectedDomain();
  return {expected.control_domain_id,
          expected,
          CurrentScope(),
          CurrentProviderOccurrence(),
          ComponentLifecycleStateDTO::kDeclared,
          ComponentLifecycleStateDTO::kDeclared,
          RequestedPrepared(),
          ComponentLifecycleDomainActionStateDTO::kReserved,
          ComponentLifecycleDomainEffectDTO::kNone,
          {},
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] ComponentLifecycleStatusDTO InitialStatus(
    const ComponentLifecycleIntentDTO& intent, const ComponentLifecycleAcceptedDTO& acceptance) {
  return {acceptance,
          intent,
          1U,
          intent.request_context.request_owner_epoch,
          ComponentLifecycleStateDTO::kDeclared,
          ComponentLifecycleStateDTO::kDeclared,
          RequestedPrepared(),
          ComponentLifecycleTransactionPhaseDTO::kAccepted,
          {InitialDomain()},
          std::nullopt,
          std::nullopt,
          std::nullopt,
          false,
          std::nullopt,
          std::nullopt,
          acceptance.accepted_at_ns,
          acceptance.accepted_at_ns,
          acceptance.timeout_duration_ns};
}

[[nodiscard]] ComponentLifecycleStatusDTO InitialDisableStatus(
    const ComponentLifecycleIntentDTO& intent, const ComponentLifecycleAcceptedDTO& acceptance) {
  const auto& expected = intent.expected_control_domains.front();
  const ComponentLifecycleRequestedStateDTO requested{
      ComponentLifecycleRequestedStatePresenceDTO::kResolved,
      ComponentLifecycleStateDTO::kDisabled};
  return {acceptance,
          intent,
          1U,
          intent.request_context.request_owner_epoch,
          ComponentLifecycleStateDTO::kActive,
          ComponentLifecycleStateDTO::kDisabling,
          requested,
          ComponentLifecycleTransactionPhaseDTO::kAccepted,
          {{expected.control_domain_id,
            expected,
            CurrentDisableScope(),
            CurrentProviderOccurrence(),
            ComponentLifecycleStateDTO::kActive,
            ComponentLifecycleStateDTO::kActive,
            requested,
            ComponentLifecycleDomainActionStateDTO::kReserved,
            ComponentLifecycleDomainEffectDTO::kNone,
            {},
            std::nullopt,
            std::nullopt}},
          std::nullopt,
          std::nullopt,
          std::nullopt,
          false,
          std::nullopt,
          std::nullopt,
          acceptance.accepted_at_ns,
          acceptance.accepted_at_ns,
          acceptance.timeout_duration_ns};
}

[[nodiscard]] ComponentLifecycleStatusDTO InitialUnresolvedDisableStatus(
    const ComponentLifecycleIntentDTO& intent, const ComponentLifecycleAcceptedDTO& acceptance) {
  auto status = InitialDisableStatus(intent, acceptance);
  status.previous_state.reset();
  status.current_state.reset();
  status.domains.front().current_scope.reset();
  status.domains.front().resolved_provider_occurrence.reset();
  status.domains.front().previous_state.reset();
  status.domains.front().current_state.reset();
  return status;
}

[[nodiscard]] DisableHandoffRequestDTO DisableRequest(
    const ComponentLifecycleStatusDTO& status,
    RequestOwnerEpoch kernel_owner_epoch = RequestOwnerEpoch("kernel-owner.1"),
    std::string kernel_request_id = "disable-handoff.1") {
  const auto scope = DisableFenceScope();
  const auto& configuration = *status.domains.front().current_scope->active_control_configuration;
  return {{RequestId(std::move(kernel_request_id)), std::move(kernel_owner_epoch),
           "CCLC Disable handoff", status.accepted_intent.request_context.request_id.value()},
          status.accepted_intent.request_context.request_id,
          status.acceptance.lifecycle_transaction_id,
          status.acceptance.lifecycle_transaction_generation,
          status.accepted_intent.controllable_component_id,
          DisableHandoffScopeResolutionDTO::kExact,
          scope,
          ContentDigest("disable-admission-snapshot.1"),
          ActiveBundlePublicationGeneration(3U),
          ContentDigest("public-pointer.1"),
          status.accepted_intent.expected_graph_revision,
          RetainedBundleId("retained-bundle.1"),
          status.acceptance.component_lifecycle_generation,
          9U,
          ConfigurationFenceGeneration(11U),
          {{configuration.control_domain_id,
            {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
             configuration.active_control_configuration_id,
             configuration.active_control_configuration_generation}}}};
}

[[nodiscard]] DisableHandoffComponentScopeExpectationDTO DisableComponentExpectation(
    const DisableHandoffRequestDTO& request) {
  return {ContentDigest("component-snapshot.1"),
          request.expected_active_publication_generation,
          request.expected_active_public_pointer_digest,
          request.expected_retained_bundle_id,
          request.expected_graph_revision,
          request.expected_component_lifecycle_generation,
          request.affected_scope,
          request.expected_active_control_configurations};
}

[[nodiscard]] DisableHandoffAdmissionSnapshotDTO DisableAdmissionSnapshot(
    const DisableHandoffRequestDTO& request,
    const DisableHandoffComponentScopeExpectationDTO& expectation,
    const RequestOwnerEpoch& kernel_owner_epoch) {
  return {{request.controllable_component_id,
           DisableHandoffScopeRequestKindDTO::kResolvedComponentScope, expectation},
          kernel_owner_epoch,
          request.scope_resolution,
          request.affected_scope,
          request.expected_active_publication_generation,
          request.expected_active_public_pointer_digest,
          request.expected_retained_bundle_id,
          request.expected_graph_revision,
          request.expected_operation_resource_generation,
          request.expected_fence_generation,
          request.expected_active_control_configurations,
          ContentDigest("disable-release-admission.1")};
}

[[nodiscard]] DisableHandoffRequestDTO PromotedFullDisableRequest(
    const ComponentLifecycleStatusDTO& status) {
  auto request = DisableRequest(status);
  request.scope_resolution = DisableHandoffScopeResolutionDTO::kPromotedToFull;
  request.affected_scope = FullDisableFenceScope();
  request.expected_active_control_configurations.push_back(
      {ControlDomainId("domain.2"),
       {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt}});
  return request;
}

[[nodiscard]] DisableHandoffAcceptedDTO DisableAcceptance(const DisableHandoffRequestDTO& request) {
  return {request.owner_request_context.request_id,
          RequestReplayStatusDTO::kAcceptedNew,
          request.lifecycle_transaction_id,
          request.lifecycle_transaction_generation,
          request.controllable_component_id,
          request.scope_resolution,
          ContentDigest("scope-digest.1"),
          ConfigurationFenceId("configuration-fence.1"),
          ConfigurationFenceGeneration(12U),
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] ConfigurationFenceHandleDTO DisableFenceHandle(
    const DisableHandoffRequestDTO& request, const DisableHandoffAcceptedDTO& acceptance) {
  return {acceptance.configuration_fence_id,
          acceptance.configuration_fence_generation,
          request.lifecycle_transaction_id,
          request.lifecycle_transaction_generation,
          request.controllable_component_id,
          request.affected_scope,
          "disable containment",
          ConfigurationFenceState::kHeld};
}

[[nodiscard]] DisableHandoffStatusDTO DisableReady(const DisableHandoffRequestDTO& request,
                                                   const DisableHandoffAcceptedDTO& acceptance) {
  return {request.lifecycle_transaction_id,
          request.lifecycle_transaction_generation,
          request.controllable_component_id,
          acceptance.scope_resolution,
          acceptance.component_scope_digest,
          acceptance.configuration_fence_id,
          acceptance.configuration_fence_generation,
          DisableHandoffStateDTO::kReady,
          request.affected_scope,
          DisableFenceHandle(request, acceptance),
          std::nullopt,
          {},
          std::nullopt};
}

[[nodiscard]] DisableHandoffStatusDTO DisableReadyWithScope(
    const DisableHandoffRequestDTO& request, const DisableHandoffAcceptedDTO& acceptance,
    ConfigurationFenceScopeDTO scope) {
  auto status = DisableReady(request, acceptance);
  status.scope = std::move(scope);
  status.configuration_fence_handle.scope = status.scope;
  return status;
}

[[nodiscard]] CCLCEffectAttemptDTO PreparedDisableEffect(const DisableHandoffRequestDTO& request,
                                                         std::uint64_t effect_sequence = 1U) {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          request.lifecycle_transaction_id,
          request.lifecycle_transaction_generation,
          effect_sequence,
          CCLCEffectKindDTO::kKernelDisableHandoff,
          CCLCEffectAttemptStateDTO::kPrepared,
          std::nullopt,
          std::nullopt,
          request,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] CCLCEffectAttemptDTO CompletedDisableEffect(
    const DisableHandoffRequestDTO& request, const DisableHandoffAcceptedDTO& acceptance,
    std::uint64_t effect_sequence = 1U) {
  auto attempt = PreparedDisableEffect(request, effect_sequence);
  attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
  attempt.disable_handoff_result = DisableHandoffEffectResultDTO{
      request, DisableHandoffEffectDispositionDTO::kAccepted, acceptance};
  return attempt;
}

[[nodiscard]] ComponentLifecycleStatusDTO PreparedDisableStatus(
    ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains;
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns -= 10;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO CompletedDisableStatus(
    ComponentLifecycleStatusDTO status, const DisableHandoffStatusDTO& handoff) {
  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kContaining;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kExecuting;
  status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kEffectObserved;
  if (status.domains.front().reconciliation_evidence == std::nullopt) {
    status.domains.front().reconciliation_evidence =
        LifecycleReconciliationEvidenceDTO{ProviderInstanceId("provider.1"),
                                           ProviderManifestId("manifest.1"),
                                           ProviderManifestRevision("manifest-revision.1"),
                                           RuntimeUnitId("runtime.1"),
                                           RuntimeEpoch("runtime-epoch.1"),
                                           1U,
                                           1U,
                                           ComponentLifecycleStateDTO::kDisabling,
                                           LifecyclePostconditionResultDTO::kNotSatisfied,
                                           510};
  }
  status.domains.front().error.reset();
  status.configuration_fence = handoff.configuration_fence_handle;
  status.disable_handoff = handoff;
  status.reconciliation_required = false;
  status.error.reset();
  status.latest_updated_at_ns += 10;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO CompletedNoSnapshotDisableStatus(
    ComponentLifecycleStatusDTO status, const DisableHandoffStatusDTO& handoff) {
  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kContaining;
  status.configuration_fence = handoff.configuration_fence_handle;
  status.disable_handoff = handoff;
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns -= 10;
  return status;
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO SecondCurrentScope() {
  return {ControlDomainId("domain.2"),
          ControlDomainRevision("domain-revision.2"),
          ControllerTopologyId("topology.2"),
          TopologyRevision("topology-revision.2"),
          std::nullopt,
          {{ControlBindingId("binding.2"), ControlBindingRevision("binding-revision.2"),
            ControlRouteId("route.2")}},
          {ResourceId("resource.2")}};
}

[[nodiscard]] ResolvedLifecycleProviderOccurrenceDTO SecondProviderOccurrence() {
  return {ControlDomainId("domain.2"),
          ControlDomainRevision("domain-revision.2"),
          ProviderInstanceId("provider.2"),
          ProviderManifestId("manifest.2"),
          ProviderManifestRevision("manifest-revision.2"),
          RuntimeUnitId("runtime.2"),
          RuntimeEpoch("runtime-epoch.2"),
          ContentDigest("catalog-digest.2")};
}

[[nodiscard]] ComponentLifecycleStatusDTO AugmentedPromotedFullDisableStatus(
    ComponentLifecycleStatusDTO status) {
  const ErrorDTO outcome{"component_not_disabled_without_vendor_mutation",
                         "containment completed but lifecycle state remains active",
                         {}};
  const auto requested = status.requested_state;
  status.status_sequence += 1U;
  status.previous_state = ComponentLifecycleStateDTO::kActive;
  status.current_state = ComponentLifecycleStateDTO::kActive;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;

  auto& expected = status.domains.front();
  expected.current_scope = CurrentDisableScope();
  expected.resolved_provider_occurrence = CurrentProviderOccurrence();
  expected.previous_state = ComponentLifecycleStateDTO::kActive;
  expected.current_state = ComponentLifecycleStateDTO::kActive;
  expected.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
  expected.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  expected.error = outcome;

  status.domains.push_back({ControlDomainId("domain.2"),
                            std::nullopt,
                            SecondCurrentScope(),
                            SecondProviderOccurrence(),
                            ComponentLifecycleStateDTO::kActive,
                            ComponentLifecycleStateDTO::kActive,
                            requested,
                            ComponentLifecycleDomainActionStateDTO::kFailed,
                            ComponentLifecycleDomainEffectDTO::kKnownNoEffect,
                            {},
                            std::nullopt,
                            outcome});
  status.reconciliation_required = false;
  status.error = outcome;
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns -= 10;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO PersistPromotedFullNoSnapshotHandoff(
    SqliteCCLCJournal& journal, const RequestOwnerEpoch& owner) {
  const auto intent = DisableIntent("request.1", owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  auto status = InitialUnresolvedDisableStatus(intent, acceptance);
  if (!SeedLifecycleClaim(journal, owner, intent, acceptance).ok()) {
    throw std::runtime_error("failed to seed lifecycle claim");
  }
  const auto accepted = journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status);
  if (!accepted.ok()) {
    throw std::runtime_error(accepted.error().code + ": " + accepted.error().message);
  }

  const auto request = PromotedFullDisableRequest(status);
  const auto handoff_acceptance = DisableAcceptance(request);
  const auto handoff = DisableReady(request, handoff_acceptance);
  status = PreparedDisableStatus(status);
  const auto prepared = journal.RecordEffectPrepared(owner, PreparedDisableEffect(request), status);
  if (!prepared.ok()) {
    throw std::runtime_error(prepared.error().code + ": " + prepared.error().message);
  }
  status = CompletedNoSnapshotDisableStatus(status, handoff);
  const auto completed = journal.RecordEffectCompleted(
      owner, CompletedDisableEffect(request, handoff_acceptance), status);
  if (!completed.ok()) {
    throw std::runtime_error(completed.error().code + ": " + completed.error().message);
  }
  return status;
}

[[nodiscard]] DisableFenceReleaseRequestDTO DisableReleaseRequest(
    const ComponentLifecycleStatusDTO& status,
    RequestOwnerEpoch kernel_owner_epoch = RequestOwnerEpoch("kernel-owner.1"),
    std::string kernel_request_id = "disable-release.1") {
  const auto handoff_request =
      DisableRequest(status, kernel_owner_epoch, "release-scope-reference.1");
  const auto expectation = DisableComponentExpectation(handoff_request);
  auto admission = DisableAdmissionSnapshot(handoff_request, expectation, kernel_owner_epoch);
  admission.configuration_fence_generation =
      status.configuration_fence->configuration_fence_generation;
  return {{RequestId(std::move(kernel_request_id)), std::move(kernel_owner_epoch),
           "CCLC Disable fence release", status.accepted_intent.request_context.request_id.value()},
          status.accepted_intent.request_context.request_id,
          status.acceptance.lifecycle_transaction_id,
          status.acceptance.lifecycle_transaction_generation,
          status.accepted_intent.controllable_component_id,
          *status.disable_handoff,
          status.status_sequence,
          ContentDigest(detail::ComponentLifecycleStatusReleasePreconditionDigest(status)),
          expectation,
          admission};
}

[[nodiscard]] DisableFenceReleasedDTO DisableReleaseReceipt(
    const DisableFenceReleaseRequestDTO& request) {
  return {request,
          RequestReplayStatusDTO::kAcceptedNew,
          700,
          request.ready_handoff.configuration_fence_id,
          request.ready_handoff.configuration_fence_generation,
          request.ready_handoff.component_scope_digest,
          ContentDigest("disable-release-receipt.1")};
}

[[nodiscard]] CCLCEffectAttemptDTO PreparedReleaseEffect(
    const DisableFenceReleaseRequestDTO& request, std::uint64_t effect_sequence = 2U) {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          request.lifecycle_transaction_id,
          request.lifecycle_transaction_generation,
          effect_sequence,
          CCLCEffectKindDTO::kKernelDisableFenceRelease,
          CCLCEffectAttemptStateDTO::kPrepared,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          request,
          std::nullopt};
}

[[nodiscard]] CCLCEffectAttemptDTO CompletedReleaseEffect(
    const DisableFenceReleaseRequestDTO& request,
    DisableFenceReleaseEffectDispositionDTO disposition,
    std::optional<DisableFenceReleasedDTO> receipt, std::uint64_t effect_sequence = 2U) {
  auto attempt = PreparedReleaseEffect(request, effect_sequence);
  attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
  attempt.disable_fence_release_result =
      DisableFenceReleaseEffectResultDTO{request, disposition, std::move(receipt)};
  return attempt;
}

[[nodiscard]] ComponentLifecycleStatusDTO PreparedReleaseStatus(
    ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns -= 10;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO ReleasePendingStatus(ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.configuration_fence->state = ConfigurationFenceState::kReleasePending;
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns -= 10;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO ResolvedDisableStatus(
    ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  status.domains.front().current_state = ComponentLifecycleStateDTO::kDisabled;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
  status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kEffectObserved;
  if (status.domains.front().reconciliation_evidence.has_value()) {
    auto& evidence = *status.domains.front().reconciliation_evidence;
    evidence.source_sequence += 1U;
    evidence.observation_generation += 1U;
    evidence.observed_state = ComponentLifecycleStateDTO::kDisabled;
    evidence.postcondition_result = LifecyclePostconditionResultDTO::kSatisfied;
    evidence.observed_at_ns += 10;
  }
  status.domains.front().error.reset();
  status.reconciliation_required = false;
  status.error.reset();
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns -= 10;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO CompletedReleaseStatus(
    ComponentLifecycleStatusDTO status, const DisableFenceReleasedDTO& receipt) {
  status.status_sequence += 1U;
  status.current_state = ComponentLifecycleStateDTO::kDisabled;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
  status.domains.front().current_state = ComponentLifecycleStateDTO::kDisabled;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
  status.configuration_fence->state = ConfigurationFenceState::kReleased;
  status.disable_fence_released = receipt;
  status.reconciliation_required = false;
  status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
  status.error.reset();
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns = 0;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO UncertainDisableStatus(
    ComponentLifecycleStatusDTO status, const RequestOwnerEpoch& replacement_owner) {
  const ErrorDTO error{
      "disable_handoff_outcome_unknown", "prepared Kernel handoff requires reconciliation", {}};
  status.status_sequence += 1U;
  status.status_owner_epoch = replacement_owner;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown;
  status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kEffectUncertain;
  status.domains.front().error = error;
  status.reconciliation_required = true;
  status.error = error;
  status.latest_updated_at_ns = 500;
  status.remaining_duration_ns = 0;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO ReplacementTerminalKnownNoEffect(
    ComponentLifecycleStatusDTO status, const RequestOwnerEpoch& replacement_owner) {
  const ErrorDTO error{"lifecycle_owner_replaced_before_effect",
                       "the prior owner ended before preparing an external effect",
                       {}};
  status.status_sequence += 1U;
  status.status_owner_epoch = replacement_owner;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kFailed;
  for (auto& domain : status.domains) {
    if (domain.action_state != ComponentLifecycleDomainActionStateDTO::kSucceeded) {
      domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
      if (domain.effect == ComponentLifecycleDomainEffectDTO::kNone) {
        domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
      }
      domain.error = error;
    }
  }
  status.reconciliation_required = false;
  status.terminal_result = ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect;
  status.error = error;
  status.latest_updated_at_ns = 500;
  status.remaining_duration_ns = 0;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO TerminalSuccess(ComponentLifecycleStatusDTO status,
                                                          std::uint64_t sequence) {
  status.status_sequence = sequence;
  status.current_state = ComponentLifecycleStateDTO::kPrepared;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
  status.domains.front().current_state = ComponentLifecycleStateDTO::kPrepared;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
  status.domains.front().effect = status.domains.front().primitive_attempts.empty()
                                      ? ComponentLifecycleDomainEffectDTO::kKnownNoEffect
                                      : ComponentLifecycleDomainEffectDTO::kEffectObserved;
  status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
  status.error.reset();
  status.latest_updated_at_ns += 10;
  status.remaining_duration_ns = 0;
  return status;
}

// History tests model settled work: a terminal result alone cannot release the
// Kernel claim or make its recovery evidence eligible for compaction.
[[nodiscard]] Result<void> RecordSettledTerminal(SqliteCCLCJournal& journal,
                                                 const RequestOwnerEpoch& owner,
                                                 const ComponentLifecycleStatusDTO& terminal) {
  const auto& intent = terminal.accepted_intent;
  const CellAdmissionClaimDTO claim{
      {CellAdmissionClaimKindDTO::kComponentLifecycle,
       CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
       owner,
       terminal.acceptance.lifecycle_transaction_id.value(),
       intent.request_context.request_id,
       ContentDigest(detail::ComponentLifecycleSemanticIntentDigest(intent)),
       intent.expected_admission_generation,
       {},
       std::nullopt,
       std::nullopt,
       intent.program_node_evidence},
      CellAdmissionClaimGeneration(1U),
      AdmissionGeneration(1U),
      0};
  const CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      owner,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
       ContentDigest(detail::ComponentLifecycleStatusReleasePreconditionDigest(terminal))}};
  const auto recorded = journal.RecordLifecycleTerminalAndClaimRelease(owner, terminal, release);
  if (!recorded.ok()) {
    return Result<void>::Err(recorded.error());
  }
  return journal.RecordCellClaimReleaseReceipt(
      owner, release,
      {CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact,
       RequestOwnerEpoch("kernel.1"),
       AdmissionGeneration(2U),
       claim,
       release,
       std::nullopt,
       {},
       {}});
}

[[nodiscard]] BrandLifecyclePrimitiveIntentDTO PrimitiveIntent(
    const ComponentLifecycleStatusDTO& status, std::string attempt_id = "attempt.1") {
  const auto& expected = status.accepted_intent.expected_control_domains.front();
  return {status.acceptance.lifecycle_transaction_id,
          status.acceptance.lifecycle_transaction_generation,
          status.accepted_intent.controllable_component_id,
          expected.control_domain_id,
          expected.control_domain_revision,
          ProviderInstanceId("provider.1"),
          ProviderManifestId("manifest.1"),
          ProviderManifestRevision("manifest-revision.1"),
          RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime-epoch.1"),
          LifecyclePrimitiveId("prepare"),
          LifecyclePrimitiveAttemptId(std::move(attempt_id)),
          1U,
          500'000};
}

[[nodiscard]] BrandLifecyclePrimitiveResultDTO PrimitiveSuccess(
    const BrandLifecyclePrimitiveIntentDTO& intent) {
  return {intent.lifecycle_transaction_id,
          intent.lifecycle_transaction_generation,
          intent.controllable_component_id,
          intent.control_domain_id,
          intent.control_domain_revision,
          intent.provider_instance_id,
          intent.provider_manifest_id,
          intent.provider_manifest_revision,
          intent.provider_runtime_unit_id,
          intent.provider_runtime_epoch,
          intent.lifecycle_primitive_id,
          intent.lifecycle_primitive_attempt_id,
          intent.expected_observation_generation,
          LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown,
          LifecyclePrimitiveEffectDTO::kObserved,
          LifecyclePrimitiveKnownResultDTO::kSucceeded,
          std::nullopt};
}

[[nodiscard]] CCLCEffectAttemptDTO PreparedPrimitiveEffect(
    const BrandLifecyclePrimitiveIntentDTO& intent, std::uint64_t effect_sequence = 1U) {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          intent.lifecycle_transaction_id,
          intent.lifecycle_transaction_generation,
          effect_sequence,
          CCLCEffectKindDTO::kBrandLifecyclePrimitive,
          CCLCEffectAttemptStateDTO::kPrepared,
          intent,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] CCLCEffectAttemptDTO CompletedPrimitiveEffect(
    const BrandLifecyclePrimitiveIntentDTO& intent, const BrandLifecyclePrimitiveResultDTO& result,
    std::uint64_t effect_sequence = 1U) {
  auto attempt = PreparedPrimitiveEffect(intent, effect_sequence);
  attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
  attempt.lifecycle_primitive_result = result;
  return attempt;
}

[[nodiscard]] ComponentLifecycleStatusDTO PreparedEffectStatus(ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kExecuting;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kExecuting;
  status.latest_updated_at_ns = 200;
  status.remaining_duration_ns = 999'900;
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO CompletedEffectStatus(
    ComponentLifecycleStatusDTO status, const BrandLifecyclePrimitiveResultDTO& result) {
  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kVerifying;
  status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kEffectObserved;
  status.domains.front().primitive_attempts.push_back(result);
  status.latest_updated_at_ns = 300;
  status.remaining_duration_ns = 999'800;
  return status;
}

[[nodiscard]] std::string Sha(char value) {
  return std::string("sha256:") + std::string(64U, value);
}

[[nodiscard]] ControllerTopologyDTO RouteTopology() {
  const ResourceInterfaceClaimDTO position{ResourceRoleId("arm_joints"), "position"};
  return {ControllerTopologyId("topology.arm"),
          TopologyRevision(Sha('1')),
          {{ControllerRoleId("hardware"), {}, {position}, {}}},
          {},
          {{ActivationSetId("activation.direct"), {ControllerRoleId("hardware")}, {}},
           {ActivationSetId("activation.legacy"), {ControllerRoleId("hardware")}, {}}},
          {{ControlRouteId("route.direct"),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {ControllerRoleId("hardware")},
            {ActivationSetId("activation.direct")}},
           {ControlRouteId("route.legacy"),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {ControllerRoleId("hardware")},
            {ActivationSetId("activation.legacy")}}}};
}

[[nodiscard]] DesiredControlConfigurationDTO RouteDesired() {
  return {ControlDomainId("route-domain.1"),
          ControlDomainRevision(Sha('3')),
          ControllerTopologyId("topology.arm"),
          TopologyRevision(Sha('1')),
          ActivationSetId("activation.direct"),
          {{ControlBindingId("route-binding.a"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.direct")},
           {ControlBindingId("route-binding.b"), ControlBindingRevision(Sha('5')),
            ControlRouteId("route.direct")}}};
}

[[nodiscard]] ControllerLifecycleExpectedDomainDTO RouteExpectedDomain() {
  const auto desired = RouteDesired();
  return {desired.control_domain_id,
          desired.control_domain_revision,
          desired.controller_topology_id,
          desired.topology_revision,
          ProductId("product.control.arm"),
          ProductContractId("manyforge.control-domain-control.v2"),
          ProductContractRevision(Sha('6')),
          ContentDigest(Sha('7')),
          ContentDigest(Sha('8')),
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("runtime-epoch.1"),
          RouteTopology(),
          desired,
          {{ControlBindingId("route-binding.a"),
            ControlBindingRevision(Sha('4')),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {{ResourceId("joint.a"), "position"}},
            {{ResourceId("joint.a"), "position"}}},
           {ControlBindingId("route-binding.b"),
            ControlBindingRevision(Sha('5')),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {{ResourceId("joint.b"), "position"}},
            {{ResourceId("joint.b"), "position"}}}},
          {{ControllerRoleId("hardware"),
            ControllerImplementationId("implementation.hardware"),
            ControllerImplementationRevision(Sha('9')),
            "controller_interface::ControllerInterface",
            ContentDigest(Sha('a')),
            {},
            {},
            {},
            {}}},
          {{ControlBindingId("route-binding.a"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.direct"), EndpointRoleId("endpoint.command.a"), "/cell/arm/a",
            true, false},
           {ControlBindingId("route-binding.b"), ControlBindingRevision(Sha('5')),
            ControlRouteId("route.direct"), EndpointRoleId("endpoint.command.b"), "/cell/arm/b",
            true, false}}};
}

[[nodiscard]] std::vector<RouteSelectionComponentExpectationDTO> RouteComponents() {
  return {{ControllableComponentId("component.a"), ComponentLifecycleGeneration(4U),
           ComponentLifecycleStateDTO::kPrepared},
          {ControllableComponentId("component.b"), ComponentLifecycleGeneration(7U),
           ComponentLifecycleStateDTO::kActive}};
}

[[nodiscard]] ActiveControlConfigurationDTO RouteCurrentConfiguration() {
  const auto desired = RouteDesired();
  return {ActiveControlConfigurationId("configuration.current"),
          desired.control_domain_id,
          desired.control_domain_revision,
          {{ControlBindingId("route-binding.a"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.legacy")},
           {ControlBindingId("route-binding.b"), ControlBindingRevision(Sha('5')),
            ControlRouteId("route.legacy")}},
          ActivationSetId("activation.legacy"),
          desired.controller_topology_id,
          desired.topology_revision,
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("runtime-epoch.1"),
          8U,
          {},
          ActiveControlConfigurationGeneration(5U),
          ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] ExpectedActiveControlConfigurationDTO RouteExpectedCurrent() {
  return {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
          ActiveControlConfigurationId("configuration.current"),
          ActiveControlConfigurationGeneration(5U)};
}

[[nodiscard]] ConfigurationFenceScopeDTO RouteScope() {
  const auto desired = RouteDesired();
  return {{ControllableComponentId("component.a"), ControllableComponentId("component.b")},
          {{desired.control_domain_id, desired.control_domain_revision,
            desired.control_domain_revision}},
          {{ControlBindingId("route-binding.a"), ControlBindingRevision(Sha('4')),
            ControlBindingRevision(Sha('4'))},
           {ControlBindingId("route-binding.b"), ControlBindingRevision(Sha('5')),
            ControlBindingRevision(Sha('5'))}},
          {ResourceId("joint.a"), ResourceId("joint.b")}};
}

[[nodiscard]] RouteSelectionIntentDTO RouteIntent(const RequestOwnerEpoch& owner,
                                                  std::string request_id = "request.route.1") {
  return {{RequestId(std::move(request_id)), owner, "journal-route-test", std::nullopt},
          RetainedBundleId("bundle.1"),
          GraphRevision("graph.1"),
          RouteDesired(),
          RouteExpectedCurrent(),
          RouteComponents(),
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt},
          AdmissionGeneration(1U)};
}

[[nodiscard]] RouteConfigurationFenceAdmissionSnapshotDTO RouteFenceSnapshot() {
  const RouteConfigurationFenceAdmissionQueryDTO query{
      RouteSelectionTransactionId("route-selection.1"),
      RouteSelectionTransactionGeneration(1U),
      RouteScope(),
      RetainedBundleId("bundle.1"),
      GraphRevision("graph.1"),
      RouteExpectedCurrent()};
  return {query,
          {RuntimeUnitId("runtime.kernel"), RuntimeEpoch("kernel-runtime-epoch.1"),
           RequestOwnerEpoch("kernel-owner.1"), ContentDigest(Sha('b')),
           ActiveBundlePublicationGeneration(3U)},
          {RetainedBundleId("bundle.1"),
           GraphRevision("graph.1"),
           {SceneId(Sha('c')), 2U, ContentDigest(Sha('d'))}},
          ActiveBundlePublicationGeneration(3U),
          ContentDigest(Sha('e')),
          11U,
          ConfigurationFenceGeneration(4U),
          RouteExpectedCurrent(),
          0U,
          0U,
          ContentDigest(Sha('f'))};
}

[[nodiscard]] PreparedRouteSelectionAdmission PreparedRoute(const RequestOwnerEpoch& owner) {
  const auto intent = RouteIntent(owner);
  const RouteSelectionAdmissionCandidate candidate{
      RouteSelectionTransactionId("route-selection.1"),
      RouteSelectionTransactionGeneration(1U),
      ActiveControlConfigurationId("configuration.candidate"),
      ActiveControlConfigurationGeneration(6U),
      ActiveControlConfigurationGeneration(5U),
      100,
      10'000,
      RetainedBundleId("bundle.1"),
      GraphRevision("graph.1"),
      RouteComponents(),
      RouteCurrentConfiguration(),
      RouteExpectedDomain(),
      RouteScope(),
      RouteFenceSnapshot()};
  const auto prepared = PrepareRouteSelectionAdmission(intent, candidate);
  if (!prepared.ok()) {
    throw std::runtime_error(prepared.error().message);
  }
  return prepared.value();
}

[[nodiscard]] PreparedRouteSelectionAdmission PreparedRouteSuccessor(
    const RequestOwnerEpoch& owner) {
  const auto intent = RouteIntent(owner, "request.route.2");
  auto fence_snapshot = RouteFenceSnapshot();
  fence_snapshot.query.route_selection_transaction_id =
      RouteSelectionTransactionId("route-selection.2");
  fence_snapshot.query.route_selection_transaction_generation =
      RouteSelectionTransactionGeneration(2U);
  const RouteSelectionAdmissionCandidate candidate{
      RouteSelectionTransactionId("route-selection.2"),
      RouteSelectionTransactionGeneration(2U),
      ActiveControlConfigurationId("configuration.candidate.2"),
      ActiveControlConfigurationGeneration(7U),
      ActiveControlConfigurationGeneration(6U),
      200,
      10'000,
      RetainedBundleId("bundle.1"),
      GraphRevision("graph.1"),
      RouteComponents(),
      RouteCurrentConfiguration(),
      RouteExpectedDomain(),
      RouteScope(),
      std::move(fence_snapshot)};
  const auto prepared = PrepareRouteSelectionAdmission(intent, candidate);
  if (!prepared.ok()) {
    throw std::runtime_error(prepared.error().message);
  }
  return prepared.value();
}

[[nodiscard]] ControllerLifecycleObservationDTO RouteDesiredObservation() {
  const auto expected = RouteExpectedDomain();
  std::vector<ControllerLifecycleObservedControllerDTO> controllers;
  for (const auto& controller : expected.expected_controllers) {
    controllers.push_back({controller, ControllerLifecycleActivityDTO::kActive});
  }
  std::vector<ControllerLifecycleObservedEndpointDTO> endpoints;
  for (const auto& endpoint : expected.expected_endpoints) {
    endpoints.push_back({endpoint, true});
  }
  std::vector<ActiveControlConfigurationBindingRouteDTO> active_routes;
  for (const auto& route : expected.desired_configuration.binding_routes) {
    active_routes.push_back(
        {route.control_binding_id, route.control_binding_revision, route.control_route_id});
  }
  return {MakeControllerLifecycleObservationQuery(expected),
          1U,
          121,
          122,
          1'000,
          ControllerLifecycleObservedConfigurationDTO{
              expected.desired_configuration.activation_set_id, std::move(active_routes)},
          std::move(controllers),
          {},
          std::move(endpoints)};
}

[[nodiscard]] ControllerLifecycleReconciliationDTO RouteDesiredReconciliation() {
  const auto expected = RouteExpectedDomain();
  return {MakeControllerLifecycleObservationQuery(expected),
          1U,
          ControllerLifecycleReconciliationDispositionDTO::kReconciled,
          expected.desired_configuration.activation_set_id,
          expected.desired_configuration.binding_routes,
          {}};
}

[[nodiscard]] ActiveControlConfigurationDTO RouteCandidateConfiguration() {
  const auto desired = RouteDesired();
  return {ActiveControlConfigurationId("configuration.candidate"),
          desired.control_domain_id,
          desired.control_domain_revision,
          {{ControlBindingId("route-binding.a"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.direct")},
           {ControlBindingId("route-binding.b"), ControlBindingRevision(Sha('5')),
            ControlRouteId("route.direct")}},
          desired.activation_set_id,
          desired.controller_topology_id,
          desired.topology_revision,
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("runtime-epoch.1"),
          1U,
          {},
          ActiveControlConfigurationGeneration(6U),
          ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] RouteConfigurationFenceIntentDTO RouteFenceIntent() {
  const auto snapshot = RouteFenceSnapshot();
  return {{RequestId("kernel.acquire.1"), snapshot.execution_host_epoch.request_owner_epoch,
           "cclc-route-selection", std::nullopt},
          RequestId("request.route.1"),
          snapshot.query.route_selection_transaction_id,
          snapshot.query.route_selection_transaction_generation,
          snapshot.query.scope,
          snapshot.snapshot_digest,
          RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy,
          snapshot.query.expected_retained_bundle_id,
          snapshot.query.expected_graph_revision,
          snapshot.operation_resource_generation,
          snapshot.configuration_fence_generation,
          snapshot.current_active_control_configuration,
          ActiveControlConfigurationId("configuration.candidate"),
          ActiveControlConfigurationGeneration(6U),
          "complete-domain route selection"};
}

[[nodiscard]] RouteConfigurationFenceAcceptedDTO RouteAcceptedFence() {
  const auto intent = RouteFenceIntent();
  return {intent.owner_request_context.request_id,
          RequestReplayStatusDTO::kAcceptedNew,
          intent.route_selection_transaction_id,
          intent.route_selection_transaction_generation,
          ConfigurationFenceId("route-fence.1"),
          ConfigurationFenceGeneration(5U),
          intent.scope,
          intent.candidate_active_control_configuration_id,
          intent.candidate_active_control_configuration_generation,
          120,
          ContentDigest(Sha('1'))};
}

[[nodiscard]] CCLCRouteEffectAttemptDTO PreparedRouteFenceAcquire() {
  return {RouteSelectionTransactionId("route-selection.1"),
          RouteSelectionTransactionGeneration(1U),
          1U,
          CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire,
          CCLCRouteEffectAttemptStateDTO::kPrepared,
          RouteFenceIntent(),
          std::nullopt};
}

[[nodiscard]] RouteSelectionStatusDTO RouteAcquirePreparedStatus(RouteSelectionStatusDTO status) {
  status.status_sequence += 1U;
  status.phase = RouteSelectionTransactionPhaseDTO::kAcquiringFence;
  status.latest_updated_at_ns = 110;
  status.remaining_duration_ns = 9'990;
  return status;
}

[[nodiscard]] CCLCRouteEffectAttemptDTO CompletedRouteFenceAcquire() {
  auto attempt = PreparedRouteFenceAcquire();
  attempt.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
  attempt.result = RouteConfigurationFenceAcquireEffectResultDTO{
      RouteFenceIntent(), RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted,
      RouteAcceptedFence()};
  return attempt;
}

[[nodiscard]] RouteSelectionStatusDTO RouteAcquireCompletedStatus(RouteSelectionStatusDTO status) {
  status.status_sequence += 1U;
  status.phase = RouteSelectionTransactionPhaseDTO::kObserving;
  status.accepted_fence = RouteAcceptedFence();
  status.latest_updated_at_ns = 120;
  status.remaining_duration_ns = 9'980;
  return status;
}

[[nodiscard]] std::filesystem::path JournalPath(const ScopedTemporaryDirectory& temporary) {
  return temporary.path() / std::string(kCCLCJournalFilename);
}

TEST(SqliteCCLCJournal, RouteSelectionSharesMembershipAndReplaysBeforeCurrentChecks) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 4U);
  RequestOwnerEpoch owner("owner.route.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());

  const auto intent = RouteIntent(owner);
  const auto prepared = PreparedRoute(owner);
  ASSERT_TRUE(SeedRouteClaim(journal, owner, prepared).ok());
  const auto accepted = journal.AcceptOrReplayRouteSelection(owner, intent, prepared.acceptance,
                                                             prepared.initial_status);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(accepted.value().locally_valid());
  EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);

  auto attributed = intent;
  attributed.attribution = {RequestTransportDTO::kHttp,
                            RequestAssuranceDTO::kExternallyAuthenticated, std::string("operator")};
  const auto replay = journal.AcceptOrReplayRouteSelection(owner, attributed, prepared.acceptance,
                                                           prepared.initial_status);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().acceptance->route_selection_transaction_generation,
            RouteSelectionTransactionGeneration(1U));

  const auto lifecycle_collision = Intent("request.route.1", owner, 1U);
  const auto conflict = journal.InspectLifecycleRequest(owner, lifecycle_collision);
  ASSERT_TRUE(conflict.ok()) << conflict.error().message;
  ASSERT_TRUE(conflict.value().has_value());
  EXPECT_EQ(conflict.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);

  const auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("owner.route.2"), 2, 4U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_TRUE(recovery.value().locally_valid());
  EXPECT_EQ(recovery.value().highest_route_selection_transaction_generation, 1U);
  ASSERT_EQ(recovery.value().active_configuration_watermarks.size(), 1U);
  EXPECT_EQ(recovery.value().active_configuration_watermarks.front().control_domain_id,
            RouteDesired().control_domain_id);
  EXPECT_EQ(recovery.value()
                .active_configuration_watermarks.front()
                .active_control_configuration_generation,
            ActiveControlConfigurationGeneration(6U));
  ASSERT_EQ(recovery.value().interrupted_route_selections.size(), 1U);
  EXPECT_EQ(recovery.value().interrupted_route_selections.front(), prepared.initial_status);
  ASSERT_EQ(recovery.value().route_effect_sequence_watermarks.size(), 1U);
  EXPECT_EQ(recovery.value().route_effect_sequence_watermarks.front().highest_effect_sequence, 0U);
}

TEST(SqliteCCLCJournal, RouteOwnerReplacementCanCloseKnownNoEffectWithoutAnAttempt) {
  for (const bool recovery_claim : {false, true}) {
    SCOPED_TRACE(recovery_claim);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U);
    RequestOwnerEpoch owner1("owner.route.1");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner1, 1, 4U).ok());
    const auto prepared = PreparedRoute(owner1);
    const auto claim = SeedRouteClaim(journal, owner1, prepared, recovery_claim);
    ASSERT_TRUE(claim.ok());
    ASSERT_TRUE(journal
                    .AcceptOrReplayRouteSelection(owner1, RouteIntent(owner1), prepared.acceptance,
                                                  prepared.initial_status)
                    .ok());

    RequestOwnerEpoch owner2("owner.route.2");
    const auto recovered = BeginTestOwnerEpoch(journal, owner2, 2, 4U);
    ASSERT_TRUE(recovered.ok());
    ASSERT_EQ(recovered.value().cell_claims.size(), 1U);
    EXPECT_EQ(recovered.value().cell_claims.front().intent, claim.value().intent);
    EXPECT_EQ(recovered.value().cell_claims.front().claim, claim.value());
    auto terminal = prepared.initial_status;
    terminal.status_sequence = 2U;
    terminal.status_owner_epoch = owner2;
    terminal.phase = RouteSelectionTransactionPhaseDTO::kFailed;
    terminal.effect = RouteSelectionEffectDTO::kKnownNoEffect;
    terminal.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
    terminal.error = ErrorDTO{"route_owner_replaced", "owner replaced before effect", {}};
    terminal.latest_updated_at_ns = 0;
    terminal.remaining_duration_ns = 0;
    ASSERT_TRUE(terminal.locally_valid());

    // No terminal publication can become visible without its exact release
    // proof. A pending claim remains active evidence, not evictable history.
    EXPECT_FALSE(journal.RecordRouteSelectionStatus(owner2, terminal).ok());

    const auto recorded = journal.RecordRouteTerminalAndClaimRelease(
        owner2, terminal, RouteClaimRelease(claim.value(), terminal));
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;
    const auto retained = journal.LoadRetainedRouteSelectionStatus(
        prepared.acceptance.route_selection_transaction_id);
    ASSERT_TRUE(retained.ok());
    ASSERT_TRUE(retained.value().has_value());
    EXPECT_EQ(*retained.value(), terminal);
    const auto release = RouteClaimRelease(claim.value(), terminal);
    ASSERT_TRUE(ValidateCCLCCellAdmissionSettlementReadOnly(
                    JournalPath(temporary), CellId("cell.1"), claim.value(), release)
                    .ok());
    auto foreign = claim.value();
    foreign.intent.expected_admission_generation = AdmissionGeneration(2U);
    EXPECT_FALSE(ValidateCCLCCellAdmissionSettlementReadOnly(JournalPath(temporary),
                                                             CellId("cell.1"), foreign, release)
                     .ok());
    if (recovery_claim) {
      foreign = claim.value();
      foreign.intent.scope.configurations.front().active_configuration_id =
          ActiveControlConfigurationId("foreign.configuration");
      EXPECT_FALSE(ValidateCCLCCellAdmissionSettlementReadOnly(JournalPath(temporary),
                                                               CellId("cell.1"), foreign, release)
                       .ok());
      foreign = claim.value();
      foreign.intent.claim_kind = CellAdmissionClaimKindDTO::kComponentLifecycle;
      foreign.intent.scope.configurations.clear();
      foreign.intent.intervention_evidence.reset();
      EXPECT_FALSE(ValidateCCLCCellAdmissionSettlementReadOnly(JournalPath(temporary),
                                                               CellId("cell.1"), foreign, release)
                       .ok());
    }
    for (const auto* id : {"pressure.1", "pressure.2"}) {
      ASSERT_TRUE(journal
                      .RecordRouteSelectionRejection(
                          owner2, RouteIntent(owner2, id),
                          {"invalid_state_transition", "unstarted", {{"effect", "not_started"}}},
                          {})
                      .ok());
    }
    EXPECT_TRUE(
        journal.LoadRetainedRouteSelectionStatus(prepared.acceptance.route_selection_transaction_id)
            .value());
    ASSERT_TRUE(
        journal
            .RecordCellClaimReleaseReceipt(owner2, release,
                                           {CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact,
                                            RequestOwnerEpoch("kernel.1"),
                                            AdmissionGeneration(2U),
                                            claim.value(),
                                            release,
                                            {},
                                            {},
                                            {}})
            .ok());
    EXPECT_FALSE(
        journal.LoadRetainedRouteSelectionStatus(prepared.acceptance.route_selection_transaction_id)
            .value());
  }
}

TEST(SqliteCCLCJournal, OriginalRouteResultRemainsReadOnlyAcrossOwnerReplacement) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 4U);
  RequestOwnerEpoch owner("route.seed");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());
  const auto intent = RouteIntent(owner);
  const auto absent = journal.ReadRouteSelectionRequestResult(intent);
  ASSERT_TRUE(absent.ok());
  EXPECT_FALSE(absent.value());
  const auto prepared = PreparedRoute(owner);
  ASSERT_TRUE(SeedRouteClaim(journal, owner, prepared).ok());
  ASSERT_TRUE(
      journal
          .AcceptOrReplayRouteSelection(owner, intent, prepared.acceptance, prepared.initial_status)
          .ok());
  const auto replacement = journal.BeginOwnerEpoch(RequestOwnerEpoch("replacement.seed"), 2, 4U);
  ASSERT_TRUE(replacement.ok());
  ASSERT_EQ(replacement.value().interrupted_route_selections.size(), 1U);
  for (unsigned repeat = 0; repeat != 2; ++repeat) {
    const auto result = journal.ReadRouteSelectionRequestResult(intent);
    ASSERT_TRUE(result.ok() && result.value());
    EXPECT_EQ(result.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(result.value()->status, prepared.initial_status);
    EXPECT_EQ(result.value()->acceptance->route_selection_transaction_id,
              prepared.acceptance.route_selection_transaction_id);
  }
  auto conflict = intent;
  conflict.desired_configuration.binding_routes.front().control_route_id = ControlRouteId("other");
  const auto changed = journal.ReadRouteSelectionRequestResult(conflict);
  ASSERT_TRUE(changed.ok() && changed.value());
  EXPECT_EQ(changed.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);
  const auto submit =
      journal.InspectRouteSelectionRequest(replacement.value().request_owner_epoch, intent);
  ASSERT_TRUE(submit.ok() && submit.value());
  EXPECT_EQ(submit.value()->replay_status, RequestReplayStatusDTO::kStaleRequestOwnerEpoch);
  const auto status =
      journal.LoadRetainedRouteSelectionStatus(prepared.acceptance.route_selection_transaction_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.value(), prepared.initial_status);
}

TEST(SqliteCCLCJournal, UnacceptedRouteRecoveryRetainsNoEffectBeforeRetiringTheOldNamespace) {
  for (const bool recovery_claim : {false, true}) {
    for (bool acquired : {false, true}) {
      SCOPED_TRACE(recovery_claim);
      SCOPED_TRACE(acquired);
      ScopedTemporaryDirectory temporary;
      const auto path = JournalPath(temporary);
      SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
      RequestOwnerEpoch owner("old.seed");
      ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());
      const auto intent = RouteIntent(owner);
      const auto attempt = RouteClaimIntent(owner, PreparedRoute(owner), recovery_claim);
      ASSERT_TRUE(journal.RecordCellClaimAttempt(owner, attempt).ok());
      const CellAdmissionClaimDTO claim{attempt, CellAdmissionClaimGeneration(1U),
                                        AdmissionGeneration(1U), 0};
      if (acquired) {
        ASSERT_TRUE(journal.RecordCellClaimAcquired(owner, claim).ok());
      }
      RequestOwnerEpoch replacement("new.seed");
      ASSERT_TRUE(BeginTestOwnerEpoch(journal, replacement, 2, 4U).ok());
      if (acquired) {
        const CellAdmissionClaimReleaseDTO release{
            attempt.owner_role,
            owner,
            attempt.owner_transaction_id,
            attempt.request_id,
            attempt.request_payload_digest,
            claim.acquired_admission_generation,
            claim.claim_generation,
            {CellAdmissionSettlementKindDTO::kProvenNoEffect, attempt.request_payload_digest}};
        ASSERT_TRUE(journal.RecordUnacceptedCellClaimRelease(replacement, release).ok());
        // Crash/lost reply repeats exact recovery, not the physical command.
        ASSERT_TRUE(journal.RecordUnacceptedCellClaimRelease(replacement, release).ok());
        ASSERT_TRUE(
            ValidateCCLCCellAdmissionSettlementReadOnly(path, CellId("cell.1"), claim, release)
                .ok());
        ASSERT_TRUE(journal
                        .RecordCellClaimReleaseReceipt(
                            replacement, release,
                            {CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact,
                             RequestOwnerEpoch("kernel.1"),
                             AdmissionGeneration(2U),
                             claim,
                             release,
                             {},
                             {},
                             {}})
                        .ok());
      } else {
        const auto retired = journal.RecordCellClaimNotAcquired(replacement, attempt);
        ASSERT_TRUE(retired.ok()) << retired.error().message;
      }
      const auto result = journal.ReadRouteSelectionRequestResult(intent);
      ASSERT_TRUE(result.ok() && result.value() && result.value()->rejection);
      EXPECT_EQ(result.value()->rejection->details.at("effect"), "not_started");
      EXPECT_FALSE(result.value()->acceptance);
      journal.Close();
      SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U);
      const auto recovery = reopened.BeginOwnerEpoch(RequestOwnerEpoch("third.seed"), 3, 4U);
      ASSERT_TRUE(recovery.ok());
      EXPECT_TRUE(recovery.value().cell_claims.empty());
    }
  }
}

TEST(SqliteCCLCJournal, RouteRecoveryClaimCannotChangeKindOrConfigurationAcrossRestart) {
  for (const bool wrong_scope : {false, true}) {
    SCOPED_TRACE(wrong_scope);
    ScopedTemporaryDirectory temporary;
    const auto path = JournalPath(temporary);
    SqliteCCLCJournal journal(path, CellId("cell.1"), 4U);
    RequestOwnerEpoch owner("recovery.seed");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());
    const auto prepared = PreparedRoute(owner);
    auto intent = RouteClaimIntent(owner, prepared, true);
    if (wrong_scope)
      intent.scope.configurations.front().active_configuration_id =
          ActiveControlConfigurationId("foreign.configuration");
    ASSERT_TRUE(journal.RecordCellClaimAttempt(owner, intent).ok());

    auto ordinary = intent;
    ordinary.claim_kind = CellAdmissionClaimKindDTO::kComponentLifecycle;
    ordinary.scope.configurations.clear();
    ordinary.intervention_evidence.reset();
    EXPECT_FALSE(journal.RecordCellClaimNotAcquired(owner, ordinary).ok());
    EXPECT_FALSE(journal
                     .RecordRouteSelectionRejection(
                         owner, RouteIntent(owner),
                         {"invalid_state_transition", "unstarted", {{"effect", "not_started"}}},
                         ordinary)
                     .ok());
    CellAdmissionClaimDTO claim{intent, CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U),
                                0};
    auto changed = claim;
    changed.intent.scope.configurations.front().active_configuration_generation =
        ActiveControlConfigurationGeneration(99U);
    EXPECT_FALSE(journal.RecordCellClaimAcquired(owner, changed).ok());
    ASSERT_TRUE(journal.RecordCellClaimAcquired(owner, claim).ok());
    const auto accepted = journal.AcceptOrReplayRouteSelection(
        owner, RouteIntent(owner), prepared.acceptance, prepared.initial_status);
    EXPECT_EQ(accepted.ok(), !wrong_scope);
    journal.Close();

    SqliteCCLCJournal reopened(path, CellId("cell.1"), 4U);
    RequestOwnerEpoch successor("successor.seed");
    const auto recovered = BeginTestOwnerEpoch(reopened, successor, 2, 4U);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().cell_claims.size(), 1U);
    EXPECT_EQ(recovered.value().cell_claims.front().intent, intent);
    EXPECT_EQ(recovered.value().cell_claims.front().claim, claim);
    EXPECT_EQ(recovered.value().cell_claims.front().transaction_accepted, !wrong_scope);
  }
}

TEST(SqliteCCLCJournal, VersionNineMigrationDoesNotInventRecoveryForAnOrdinaryClaim) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"), 4U);
  RequestOwnerEpoch owner("old.seed");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());
  const auto prepared = PreparedRoute(owner);
  const auto claim = SeedRouteClaim(journal, owner, prepared);
  ASSERT_TRUE(claim.ok());
  journal.Close();
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(raw,
                   "ALTER TABLE cclc_lifecycle_cell_claims DROP COLUMN recovery_configuration;"
                   MANYFORGE_DROP_NATIVE_APPLY_TABLES
                   "UPDATE cclc_journal_metadata SET schema_version=9; PRAGMA user_version=9;",
                   nullptr, nullptr, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 4U);
  RequestOwnerEpoch successor("new.seed");
  const auto recovered = BeginTestOwnerEpoch(reopened, successor, 2, 4U);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().cell_claims.size(), 1U);
  EXPECT_EQ(recovered.value().cell_claims.front().claim, claim.value());
  EXPECT_FALSE(recovered.value().cell_claims.front().intent.intervention_evidence);
}

TEST(SqliteCCLCJournal, RouteCodecAndVersionEightMigrationPreserveLegacyClaimlessEvidence) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("old.seed");
  SqliteCCLCJournal journal(path, CellId("cell.1"), 4U);
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());
  auto prepared = PreparedRoute(owner);
  auto& intent = prepared.initial_status.accepted_intent;
  intent.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch("runner.1"),
                                                                 ProgramRunId("run.1"),
                                                                 CellAdmissionClaimGeneration(7U),
                                                                 "route",
                                                                 "route.1",
                                                                 "cclc.control_route"};
  const auto current_bytes = detail::EncodeRouteSelectionStatus(prepared.initial_status);
  EXPECT_EQ(detail::DecodeRouteSelectionStatus(current_bytes), prepared.initial_status);
  EXPECT_NE(current_bytes.find("MF-CCLC-ROUTE-STATUS-V3"), std::string::npos);
  auto changed = intent;
  changed.program_node_evidence->node_attempt_id = "route.2";
  EXPECT_NE(detail::RouteSelectionSemanticIntentDigest(changed),
            detail::RouteSelectionSemanticIntentDigest(intent));
  changed.expected_admission_generation.reset();
  EXPECT_FALSE(changed.locally_valid());
  // The codec still reads legacy Program-node evidence, but route selection
  // is no longer a Program node: the claimed request carries none.
  intent.program_node_evidence.reset();
  ASSERT_TRUE(SeedRouteClaim(journal, owner, prepared).ok());
  ASSERT_TRUE(
      journal
          .AcceptOrReplayRouteSelection(owner, intent, prepared.acceptance, prepared.initial_status)
          .ok());
  journal.Close();

  // Materialize the actual pre-enrollment record shape: old bytes/digest and
  // no claim. Recovery must preserve it, not manufacture a retrospective claim.
  intent.program_node_evidence.reset();
  intent.expected_admission_generation.reset();
  const auto bytes = detail::EncodeRouteSelectionStatus(prepared.initial_status);
  const auto digest = detail::RouteSelectionSemanticIntentDigest(intent);
  EXPECT_NE(bytes.find("MF-CCLC-ROUTE-STATUS-V2"), std::string::npos);
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  sqlite3_stmt* update = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(raw, "UPDATE route_selection_transactions SET status_blob=?", -1,
                               &update, nullptr),
            SQLITE_OK);
  ASSERT_EQ(
      sqlite3_bind_blob(update, 1, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_step(update), SQLITE_DONE);
  sqlite3_finalize(update);
  ASSERT_EQ(sqlite3_prepare_v2(raw, "UPDATE cclc_request_memberships SET semantic_request_digest=?",
                               -1, &update, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_bind_text(update, 1, digest.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(update), SQLITE_DONE);
  sqlite3_finalize(update);
  ASSERT_EQ(
      sqlite3_exec(raw,
                   "DELETE FROM cclc_lifecycle_cell_claims;"
                   "ALTER TABLE cclc_lifecycle_cell_claims DROP COLUMN recovery_configuration;"
                   MANYFORGE_DROP_NATIVE_APPLY_TABLES
                   "UPDATE cclc_journal_metadata SET schema_version=8; PRAGMA user_version=8;",
                   nullptr, nullptr, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 4U);
  const auto recovery = reopened.BeginOwnerEpoch(RequestOwnerEpoch("new.seed"), 2, 4U);
  ASSERT_TRUE(recovery.ok());
  EXPECT_TRUE(recovery.value().cell_claims.empty());
  ASSERT_EQ(recovery.value().interrupted_route_selections.size(), 1U);
  EXPECT_EQ(detail::EncodeRouteSelectionStatus(recovery.value().interrupted_route_selections[0]),
            bytes);
  const auto result = reopened.ReadRouteSelectionRequestResult(intent);
  ASSERT_TRUE(result.ok() && result.value() && result.value()->status);
  EXPECT_EQ(detail::RouteSelectionSemanticIntentDigest(result.value()->status->accepted_intent),
            digest);
}

TEST(SqliteCCLCJournal, FailedCandidateBurnsGenerationWithoutReplacingCurrentPublication) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 4U);
  RequestOwnerEpoch owner("owner.route.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());

  const auto first = PreparedRoute(owner);
  const auto claim = SeedRouteClaim(journal, owner, first);
  ASSERT_TRUE(claim.ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayRouteSelection(owner, RouteIntent(owner), first.acceptance,
                                                first.initial_status)
                  .ok());
  auto failed = first.initial_status;
  failed.status_sequence = 2U;
  failed.phase = RouteSelectionTransactionPhaseDTO::kFailed;
  failed.effect = RouteSelectionEffectDTO::kKnownNoEffect;
  failed.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
  failed.error = ErrorDTO{"route_fence_not_acquired", "Kernel proved no Acquire effect", {}};
  failed.latest_updated_at_ns = 110;
  failed.remaining_duration_ns = 0;
  ASSERT_TRUE(failed.locally_valid());
  ASSERT_TRUE(journal
                  .RecordRouteTerminalAndClaimRelease(owner, failed,
                                                      RouteClaimRelease(claim.value(), failed))
                  .ok());

  const auto current =
      journal.LoadCurrentActiveControlConfiguration(RouteDesired().control_domain_id);
  ASSERT_TRUE(current.ok());
  ASSERT_TRUE(current.value().has_value());
  EXPECT_EQ(*current.value(), RouteCurrentConfiguration());

  const auto successor = PreparedRouteSuccessor(owner);
  ASSERT_TRUE(SeedRouteClaim(journal, owner, successor).ok());
  const auto accepted = journal.AcceptOrReplayRouteSelection(
      owner, RouteIntent(owner, "request.route.2"), successor.acceptance, successor.initial_status);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().acceptance->candidate_active_control_configuration_generation,
            ActiveControlConfigurationGeneration(7U));
}

TEST(SqliteCCLCJournal, CurrentConfigurationPublicationIsAtomicAndRecoveredExactly) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner1("owner.route.1");
  std::optional<RouteSelectionStatusDTO> published;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 4U);
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner1, 1, 4U).ok());
    const auto prepared = PreparedRoute(owner1);
    const auto claim = SeedRouteClaim(journal, owner1, prepared);
    ASSERT_TRUE(claim.ok());
    ASSERT_TRUE(journal
                    .AcceptOrReplayRouteSelection(owner1, RouteIntent(owner1), prepared.acceptance,
                                                  prepared.initial_status)
                    .ok());

    const auto bootstrap =
        journal.LoadCurrentActiveControlConfiguration(RouteDesired().control_domain_id);
    ASSERT_TRUE(bootstrap.ok());
    ASSERT_EQ(bootstrap.value(), std::optional(RouteCurrentConfiguration()));

    const auto acquiring = RouteAcquirePreparedStatus(prepared.initial_status);
    ASSERT_TRUE(
        journal.RecordRouteEffectPrepared(owner1, PreparedRouteFenceAcquire(), acquiring).ok());
    auto observing = RouteAcquireCompletedStatus(acquiring);
    ASSERT_TRUE(
        journal.RecordRouteEffectCompleted(owner1, CompletedRouteFenceAcquire(), observing).ok());

    published = observing;
    published->status_sequence += 1U;
    published->phase = RouteSelectionTransactionPhaseDTO::kPublishing;
    published->effect = RouteSelectionEffectDTO::kEffectObserved;
    published->controller_observation = RouteDesiredObservation();
    published->controller_reconciliation = RouteDesiredReconciliation();
    published->affected_control_binding_ids.clear();
    published->affected_controller_role_ids.clear();
    published->resulting_active_control_configuration = RouteCandidateConfiguration();
    published->latest_updated_at_ns = 130;
    published->remaining_duration_ns = 9'970;
    ASSERT_TRUE(published->locally_valid());
    const auto recorded = journal.RecordRouteSelectionStatus(owner1, *published);
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;

    const auto current =
        journal.LoadCurrentActiveControlConfiguration(RouteDesired().control_domain_id);
    ASSERT_TRUE(current.ok());
    ASSERT_EQ(current.value(), std::optional(RouteCandidateConfiguration()));

    const auto coherent = journal.LoadCurrentSnapshotFacts({ControllableComponentId("component.1")},
                                                           {RouteDesired().control_domain_id});
    ASSERT_TRUE(coherent.ok()) << coherent.error().message;
    ASSERT_EQ(coherent.value().components.size(), 1U);
    EXPECT_FALSE(coherent.value().components.front().durable_generation.has_value());
    ASSERT_EQ(coherent.value().domains.size(), 1U);
    EXPECT_EQ(coherent.value().domains.front().current_active_control_configuration,
              std::optional(RouteCandidateConfiguration()));

    auto expanded = *published;
    expanded.status_sequence += 1U;
    expanded.affected_control_binding_ids = {ControlBindingId("route-binding.a"),
                                             ControlBindingId("route-binding.b")};
    expanded.affected_controller_role_ids = {ControllerRoleId("hardware")};
    expanded.latest_updated_at_ns += 1;
    expanded.remaining_duration_ns -= 1;
    ASSERT_TRUE(expanded.locally_valid());
    EXPECT_FALSE(journal.RecordRouteSelectionStatus(owner1, expanded).ok());
  }

  SqliteCCLCJournal reopened(path, CellId("cell.1"), 4U);
  const auto recovery = reopened.BeginOwnerEpoch(RequestOwnerEpoch("owner.route.2"), 2, 4U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().current_active_control_configurations.size(), 1U);
  EXPECT_EQ(recovery.value().current_active_control_configurations.front(),
            RouteCandidateConfiguration());
  ASSERT_EQ(recovery.value().interrupted_route_selections.size(), 1U);
  ASSERT_TRUE(published.has_value());
  EXPECT_EQ(recovery.value().interrupted_route_selections.front(), *published);
}

TEST(SqliteCCLCJournal, RouteFenceEffectCompletesAtomicallyAndRecoversExactStatus) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 4U);
  RequestOwnerEpoch owner1("owner.route.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner1, 1, 4U).ok());
  const auto prepared_route = PreparedRoute(owner1);
  ASSERT_TRUE(SeedRouteClaim(journal, owner1, prepared_route).ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayRouteSelection(owner1, RouteIntent(owner1),
                                                prepared_route.acceptance,
                                                prepared_route.initial_status)
                  .ok());

  const auto acquiring = RouteAcquirePreparedStatus(prepared_route.initial_status);
  const auto prepared_effect = PreparedRouteFenceAcquire();
  const auto prepared_row = journal.RecordRouteEffectPrepared(owner1, prepared_effect, acquiring);
  ASSERT_TRUE(prepared_row.ok()) << prepared_row.error().message;

  const auto observing = RouteAcquireCompletedStatus(acquiring);
  const auto completed_effect = CompletedRouteFenceAcquire();
  const auto completed_row =
      journal.RecordRouteEffectCompleted(owner1, completed_effect, observing);
  ASSERT_TRUE(completed_row.ok()) << completed_row.error().message;
  EXPECT_EQ(completed_row.value(), prepared_row.value());

  const auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("owner.route.2"), 2, 4U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_TRUE(recovery.value().locally_valid());
  ASSERT_EQ(recovery.value().interrupted_route_selections.size(), 1U);
  EXPECT_EQ(recovery.value().interrupted_route_selections.front(), observing);
  ASSERT_EQ(recovery.value().route_effect_sequence_watermarks.size(), 1U);
  EXPECT_EQ(recovery.value().route_effect_sequence_watermarks.front().highest_effect_sequence, 1U);
  EXPECT_TRUE(recovery.value().unresolved_route_effect_attempts.empty());
}

TEST(SqliteCCLCJournal, RoutePreparedEffectSurvivesReplacementAndClosesAdmission) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 4U);
  RequestOwnerEpoch owner1("owner.route.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner1, 1, 4U).ok());
  const auto prepared_route = PreparedRoute(owner1);
  ASSERT_TRUE(SeedRouteClaim(journal, owner1, prepared_route).ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayRouteSelection(owner1, RouteIntent(owner1),
                                                prepared_route.acceptance,
                                                prepared_route.initial_status)
                  .ok());
  const auto acquiring = RouteAcquirePreparedStatus(prepared_route.initial_status);
  const auto prepared_effect = PreparedRouteFenceAcquire();
  ASSERT_TRUE(journal.RecordRouteEffectPrepared(owner1, prepared_effect, acquiring).ok());

  RequestOwnerEpoch owner2("owner.route.2");
  const auto first_recovery = BeginTestOwnerEpoch(journal, owner2, 2, 4U);
  ASSERT_TRUE(first_recovery.ok()) << first_recovery.error().message;
  ASSERT_TRUE(first_recovery.value().locally_valid());
  ASSERT_EQ(first_recovery.value().unresolved_route_effect_attempts.size(), 1U);
  EXPECT_EQ(first_recovery.value().unresolved_route_effect_attempts.front(), prepared_effect);

  auto reconciling = acquiring;
  reconciling.status_sequence = 3U;
  reconciling.status_owner_epoch = owner2;
  reconciling.phase = RouteSelectionTransactionPhaseDTO::kOutcomeUnknown;
  reconciling.effect = RouteSelectionEffectDTO::kEffectUncertain;
  reconciling.reconciliation_required = true;
  reconciling.error = ErrorDTO{"route_effect_unresolved", "route effect outcome unknown", {}};
  reconciling.latest_updated_at_ns = 0;
  reconciling.remaining_duration_ns = 0;
  ASSERT_TRUE(reconciling.locally_valid());
  const auto replaced = journal.RecordRouteSelectionStatus(owner2, reconciling);
  ASSERT_TRUE(replaced.ok()) << replaced.error().message;

  const auto second_recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("owner.route.3"), 3, 4U);
  ASSERT_TRUE(second_recovery.ok()) << second_recovery.error().message;
  ASSERT_TRUE(second_recovery.value().locally_valid());
  ASSERT_EQ(second_recovery.value().interrupted_route_selections.size(), 1U);
  EXPECT_EQ(second_recovery.value().interrupted_route_selections.front(), reconciling);
  ASSERT_EQ(second_recovery.value().unresolved_route_effect_attempts.size(), 1U);
  EXPECT_EQ(second_recovery.value().unresolved_route_effect_attempts.front(), prepared_effect);
}

TEST(SqliteCCLCJournal, CounterMintedNamespacesAndSettledGuardsStayBoundedAcrossRotations) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch first_epoch("");
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
    RequestOwnerEpoch owner("");
    for (std::uint64_t generation = 1; generation <= 8; ++generation) {
      if (generation % 2U == 1U) {
        const auto begun =
            journal.BeginOwnerEpoch(RequestOwnerEpoch("startup-unique-seed"), generation, 2U);
        ASSERT_TRUE(begun.ok()) << begun.error().message;
        EXPECT_NE(owner, begun.value().request_owner_epoch);
        owner = begun.value().request_owner_epoch;
        if (generation == 1U) first_epoch = owner;
      }
      const auto intent = Intent("request." + std::to_string(generation), owner, generation + 2U);
      const auto accepted =
          Acceptance(intent, generation, "lifecycle." + std::to_string(generation));
      const auto initial = InitialStatus(intent, accepted);
      ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, accepted).ok());
      ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, accepted, initial).ok());
      ASSERT_TRUE(RecordSettledTerminal(journal, owner, TerminalSuccess(initial, 2U)).ok());
    }
  }
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
  sqlite3_stmt* query = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db,
                               "SELECT (SELECT count(*) FROM cclc_owner_epochs),"
                               "(SELECT count(*) FROM cclc_request_memberships),"
                               "(SELECT count(*) FROM cclc_lifecycle_transaction_identity_guards),"
                               "(SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs')",
                               -1, &query, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(query), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int64(query, 0), 2);
  EXPECT_EQ(sqlite3_column_int64(query, 1), 4);  // Current plus one complete full predecessor.
  EXPECT_EQ(sqlite3_column_int64(query, 2), 1);
  EXPECT_EQ(sqlite3_column_int64(query, 3), 4);
  sqlite3_finalize(query);
  sqlite3_close(db);
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U);
  const auto next = reopened.BeginOwnerEpoch(RequestOwnerEpoch("startup-unique-seed"), 9, 2U);
  ASSERT_TRUE(next.ok()) << next.error().message;
  EXPECT_NE(next.value().request_owner_epoch, first_epoch);
  EXPECT_EQ(next.value().highest_lifecycle_transaction_generation, 8U);
}

TEST(SqliteCCLCJournal, CounterGuardRejectsNoncanonicalNewIdentityBeforeAcceptance) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto intent = Intent("request.1", owner, 3U);
  const auto wrong = Acceptance(intent, 1U, "lifecycle.42");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, wrong).ok());
  const auto refused =
      journal.AcceptOrReplayLifecycle(owner, intent, wrong, InitialStatus(intent, wrong));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_journal_transaction_identity_reused");
  const auto result = journal.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value());
}

TEST(SqliteCCLCJournal, MissingOrRegressedNamespaceCounterCannotBePrunedAway) {
  for (const auto* corruption :
       {"DELETE FROM sqlite_sequence WHERE name='cclc_owner_epochs'",
        "UPDATE sqlite_sequence SET seq=0 WHERE name='cclc_owner_epochs'"}) {
    ScopedTemporaryDirectory temporary;
    const auto path = JournalPath(temporary);
    SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
    const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U);
    ASSERT_TRUE(begun.ok());
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, corruption, nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    const auto refused = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 2, 2U,
                                                 begun.value().request_owner_epoch);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "component_lifecycle_journal_corrupt");
    journal.Close();
    EXPECT_THROW((SqliteCCLCJournal(path, CellId("cell.1"), 1U)), std::runtime_error);
  }
}

TEST(SqliteCCLCJournal, OversizedRejectionExpiresWithoutCorruptingTheByteBudget) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"), 1U, 1U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto intent = Intent("request.1", owner, 3U);
  const ErrorDTO error{"invalid_state_transition", "not admitted", {{"effect", "not_started"}}};
  const auto recorded = journal.RecordLifecycleRejection(owner, intent, error, std::nullopt);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  EXPECT_EQ(recorded.value().rejection, error);
  journal.Close();
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U, 1U);
  const auto read = reopened.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(read.ok());
  ASSERT_TRUE(read.value());
  EXPECT_EQ(read.value()->replay_status, RequestReplayStatusDTO::kRequestResultExpired);
}

TEST(SqliteCCLCJournal, LiveCheckpointCannotCrossAnUnresolvedClaimOrAdvanceItsCounter) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto intent = Intent("request.1", owner, 3U);
  const auto accepted = Acceptance(intent, 1U, "lifecycle.1");
  const auto claim = SeedLifecycleClaim(journal, owner, intent, accepted);
  ASSERT_TRUE(claim.ok());
  const auto refused = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 2, 2U, owner);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_checkpoint_not_quiescent");
  ASSERT_TRUE(ReleaseUnacceptedTestClaim(journal, owner, claim.value()).ok());
  const auto checkpoint = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 3, 2U, owner);
  ASSERT_TRUE(checkpoint.ok()) << checkpoint.error().message;
  EXPECT_NE(checkpoint.value().request_owner_epoch, owner);
  journal.Close();
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(JournalPath(temporary).c_str(), &raw), SQLITE_OK);
  sqlite3_stmt* counter = nullptr;
  ASSERT_EQ(
      sqlite3_prepare_v2(raw, "SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs'", -1,
                         &counter, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_step(counter), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int64(counter, 0), 2);
  sqlite3_finalize(counter);
  sqlite3_close(raw);
}

TEST(SqliteCCLCJournal, UnsupportedCompactedLegacyIdentityRollsBackVersionSevenMigration) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U).ok());
  }
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(
          raw,
          "ALTER TABLE cclc_lifecycle_cell_claims DROP COLUMN recovery_configuration;"
          "INSERT INTO cclc_lifecycle_transaction_identity_guards VALUES('lifecycle.42',1);"
          "UPDATE cclc_journal_metadata SET highest_transaction_generation=1,schema_version=7;"
          "PRAGMA user_version=7;",
          nullptr, nullptr, nullptr),
      SQLITE_OK);
  sqlite3_close(raw);
  EXPECT_THROW((SqliteCCLCJournal(path, CellId("cell.1"), 1U)), std::runtime_error);
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  sqlite3_stmt* version = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &version, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(version, 0), 7);
  sqlite3_finalize(version);
  sqlite3_close(raw);
}

TEST(SqliteCCLCJournal, FullMembershipIsRejectedDuringInspectionBeforeAnyNewClaim) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 1U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto first = Intent("request.1", owner, 3U);
  const ErrorDTO error{"invalid_state_transition", "not admitted", {{"effect", "not_started"}}};
  ASSERT_TRUE(journal.RecordLifecycleRejection(owner, first, error, std::nullopt).ok());
  const auto lifecycle = journal.InspectLifecycleRequest(owner, Intent("request.2", owner, 3U));
  ASSERT_TRUE(lifecycle.ok());
  ASSERT_TRUE(lifecycle.value());
  EXPECT_EQ(lifecycle.value()->replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
  const auto route = journal.InspectRouteSelectionRequest(owner, RouteIntent(owner));
  ASSERT_TRUE(route.ok());
  ASSERT_TRUE(route.value());
  EXPECT_EQ(route.value()->replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
  const auto duplicate = journal.InspectLifecycleRequest(owner, first);
  ASSERT_TRUE(duplicate.ok());
  ASSERT_TRUE(duplicate.value());
  EXPECT_EQ(duplicate.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
}

TEST(SqliteCCLCJournal, UnstartedAbsenceSurvivesOtherCallerCutoverUntilNextFullRetirement) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const ErrorDTO rejected{"invalid_state_transition", "unstarted", {{"effect", "not_started"}}};
  SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
  const auto first = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U);
  ASSERT_TRUE(first.ok());
  const auto old = first.value().request_owner_epoch;
  for (const auto* id : {"request.1", "request.2"}) {
    ASSERT_TRUE(
        journal.RecordLifecycleRejection(old, Intent(id, old, 3U), rejected, std::nullopt).ok());
  }
  const auto unseen = Intent("lost.capacity.reply", old, 3U);
  const auto before = journal.ReadLifecycleRequestResult(unseen);
  ASSERT_TRUE(before.ok() && before.value());
  EXPECT_EQ(before.value()->replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
  const auto second = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 2, 2U, old);
  ASSERT_TRUE(second.ok());
  journal.Close();
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U);
  const auto proof = reopened.ReadLifecycleRequestResult(unseen);
  ASSERT_TRUE(proof.ok() && proof.value());
  EXPECT_EQ(proof.value()->replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
  const auto route_proof =
      reopened.ReadRouteSelectionRequestResult(RouteIntent(old, "unseen.route"));
  ASSERT_TRUE(route_proof.ok() && route_proof.value());
  EXPECT_EQ(route_proof.value()->replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
  const auto wrong_kind = reopened.ReadRouteSelectionRequestResult(RouteIntent(old, "request.1"));
  ASSERT_TRUE(wrong_kind.ok() && wrong_kind.value());
  EXPECT_EQ(wrong_kind.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);
  const auto expired = reopened.ReadLifecycleRequestResult(Intent("request.1", old, 3U));
  ASSERT_TRUE(expired.ok() && expired.value());
  EXPECT_EQ(expired.value()->replay_status, RequestReplayStatusDTO::kRequestResultExpired);
  const auto current = second.value().request_owner_epoch;
  for (const auto* id : {"request.3", "request.4"}) {
    ASSERT_TRUE(
        reopened.RecordLifecycleRejection(current, Intent(id, current, 3U), rejected, std::nullopt)
            .ok());
  }
  ASSERT_TRUE(reopened.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 3, 2U, current).ok());
  const auto retired = reopened.ReadLifecycleRequestResult(unseen);
  ASSERT_TRUE(retired.ok());
  EXPECT_FALSE(
      retired.value());  // Beyond the explicit bounded recovery window: unknown, not retryable.
  const auto retired_route =
      reopened.ReadRouteSelectionRequestResult(RouteIntent(old, "unseen.route"));
  ASSERT_TRUE(retired_route.ok());
  EXPECT_FALSE(retired_route.value());
}

TEST(SqliteCCLCJournal, VersionSixUpgradePreservesExistingRequestAndStatusBytes) {
  ScopedTemporaryDirectory temporary;
  RequestOwnerEpoch owner("owner.1");
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto intent = Intent("request.1", owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  const auto initial = InitialStatus(intent, acceptance);
  const auto digest = detail::ComponentLifecycleSemanticIntentDigest(intent);
  const auto bytes = detail::EncodeComponentLifecycleStatus(initial);
  {
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, acceptance, initial).ok());
  }
  journal.Close();
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(JournalPath(temporary).c_str(), &raw), SQLITE_OK);
  const auto result =
      sqlite3_exec(raw,
                   "ALTER TABLE cclc_lifecycle_cell_claims DROP COLUMN recovery_configuration;"
                   "ALTER TABLE cclc_lifecycle_cell_claims DROP COLUMN program_node_evidence;"
                   "ALTER TABLE cclc_request_memberships DROP COLUMN rejection_blob;"
                   "ALTER TABLE cclc_request_memberships DROP COLUMN rejection_sequence;"
                   MANYFORGE_DROP_NATIVE_APPLY_TABLES
                   "UPDATE cclc_journal_metadata SET schema_version=6; PRAGMA user_version=6;",
                   nullptr, nullptr, nullptr);
  const std::string detail = sqlite3_errmsg(raw);
  sqlite3_close(raw);
  ASSERT_EQ(result, SQLITE_OK) << detail;
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"), 8U);
  const auto read = reopened.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(read.ok());
  ASSERT_TRUE(read.value());
  ASSERT_TRUE(read.value()->status);
  EXPECT_EQ(detail::EncodeComponentLifecycleStatus(*read.value()->status), bytes);
  EXPECT_EQ(detail::ComponentLifecycleSemanticIntentDigest(read.value()->status->accepted_intent),
            digest);
}

TEST(SqliteCCLCJournal, RejectedRequestIsExactBoundedHistoryAndSurvivesOwnerReplacement) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  SqliteCCLCJournal first_journal(path, CellId("cell.1"), 1U);
  ASSERT_TRUE(BeginTestOwnerEpoch(first_journal, owner, 1, 8U).ok());
  const auto first = Intent("request.rejected.1", owner, 3U);
  const auto second = Intent("request.rejected.2", owner, 3U);
  const ErrorDTO rejected{"invalid_state_transition",
                          "Held by Stop",
                          {{"reason", "program_node_stop_fence_active"},
                           {"selection_revision", "4"},
                           {"effect", "not_started"}}};
  {
    const auto result =
        first_journal.RecordLifecycleRejection(owner, first, rejected, std::nullopt);
    ASSERT_TRUE(result.ok()) << result.error().message;
    ASSERT_TRUE(result.value().locally_valid());
    EXPECT_EQ(result.value().rejection, rejected);
    ASSERT_TRUE(first_journal.RecordLifecycleRejection(owner, second, rejected, std::nullopt).ok());
    const auto expired = first_journal.ReadLifecycleRequestResult(first);
    ASSERT_TRUE(expired.ok());
    ASSERT_TRUE(expired.value());
    EXPECT_EQ(expired.value()->replay_status, RequestReplayStatusDTO::kRequestResultExpired);
    EXPECT_FALSE(expired.value()->rejection);
  }
  first_journal.Close();
  SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
  const auto recovered = journal.BeginOwnerEpoch(RequestOwnerEpoch("owner.2"), 2, 8U);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().highest_lifecycle_transaction_generation, 0U);
  EXPECT_TRUE(recovered.value().cell_claims.empty());
  const auto result = journal.ReadLifecycleRequestResult(second);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value());
  EXPECT_EQ(result.value()->rejection, rejected);
}

TEST(SqliteCCLCJournal, TerminalChildProofSurvivesConcurrentHistoryAndOwnerReplacement) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto first = Intent("request.1", owner, 3U);
  const auto accepted = Acceptance(first, 1U, "lifecycle.1");
  const auto initial = InitialStatus(first, accepted);
  const auto terminal = TerminalSuccess(initial, 2U);
  std::optional<CellAdmissionClaimDTO> claim;
  std::optional<CellAdmissionClaimReleaseDTO> release;
  {
    auto seeded = SeedLifecycleClaim(journal, owner, first, accepted);
    ASSERT_TRUE(seeded.ok());
    claim = seeded.value();
    ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, first, accepted, initial).ok());
    release = CellAdmissionClaimReleaseDTO{
        claim->intent.owner_role,
        owner,
        claim->intent.owner_transaction_id,
        claim->intent.request_id,
        claim->intent.request_payload_digest,
        claim->acquired_admission_generation,
        claim->claim_generation,
        {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
         ContentDigest(detail::ComponentLifecycleStatusReleasePreconditionDigest(terminal))}};
    ASSERT_TRUE(journal.RecordLifecycleTerminalAndClaimRelease(owner, terminal, *release).ok());
    // Another child/result fills the one-entry history while A's release is delayed.
    const ErrorDTO rejection{"stale_generation", "Changed", {{"effect", "not_started"}}};
    ASSERT_TRUE(journal
                    .RecordLifecycleRejection(owner, Intent("request.2", owner, 4U), rejection,
                                              std::nullopt)
                    .ok());
    ASSERT_TRUE(journal
                    .RecordLifecycleRejection(owner, Intent("request.3", owner, 4U), rejection,
                                              std::nullopt)
                    .ok());
    EXPECT_TRUE(
        ValidateCCLCCellAdmissionSettlementReadOnly(path, CellId("cell.1"), *claim, *release).ok());
  }
  journal.Close();
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U);
  RequestOwnerEpoch successor("owner.2");
  const auto recovered = BeginTestOwnerEpoch(reopened, successor, 2, 8U);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().cell_claims.size(), 1U);
  EXPECT_EQ(recovered.value().cell_claims[0].release_intent, release);
  EXPECT_TRUE(
      ValidateCCLCCellAdmissionSettlementReadOnly(path, CellId("cell.1"), *claim, *release).ok());
  const CellAdmissionRecoveryReceiptDTO receipt{
      CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact,
      RequestOwnerEpoch("kernel.1"),
      AdmissionGeneration(2U),
      claim,
      release,
      std::nullopt,
      {},
      {}};
  EXPECT_TRUE(reopened.RecordCellClaimReleaseReceipt(successor, *release, receipt).ok());
}

TEST(SqliteCCLCJournal, ReplaysBeforeCapacityAndIgnoresOnlyAttribution) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 1U).ok());

  const auto first_intent = Intent("request.1", owner, 3U);
  const auto first_acceptance = Acceptance(first_intent, 1U, "lifecycle.1");
  const auto first_status = InitialStatus(first_intent, first_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, first_intent, first_acceptance).ok());
  const auto accepted =
      journal.AcceptOrReplayLifecycle(owner, first_intent, first_acceptance, first_status);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);

  auto attributed = first_intent;
  attributed.attribution = {RequestTransportDTO::kHttp,
                            RequestAssuranceDTO::kExternallyAuthenticated, std::string("operator")};
  const auto replay_candidate = Acceptance(attributed, 2U, "discarded.2");
  const auto replayed = journal.AcceptOrReplayLifecycle(
      owner, attributed, replay_candidate, InitialStatus(attributed, replay_candidate));
  ASSERT_TRUE(replayed.ok()) << replayed.error().message;
  EXPECT_EQ(replayed.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  ASSERT_TRUE(replayed.value().acceptance.has_value());
  EXPECT_EQ(replayed.value().acceptance->lifecycle_transaction_id,
            first_acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(replayed.value().status.has_value());
  EXPECT_EQ(replayed.value().status->accepted_intent.attribution, first_intent.attribution);

  auto unusable_acceptance = replay_candidate;
  unusable_acceptance.replay_status = RequestReplayStatusDTO::kRequestIdConflict;
  auto unusable_status = InitialStatus(attributed, replay_candidate);
  unusable_status.acceptance = unusable_acceptance;
  const auto replay_with_unusable_candidate =
      journal.AcceptOrReplayLifecycle(owner, attributed, unusable_acceptance, unusable_status);
  ASSERT_TRUE(replay_with_unusable_candidate.ok());
  EXPECT_EQ(replay_with_unusable_candidate.value().replay_status,
            RequestReplayStatusDTO::kReplayedExact);

  auto conflict = first_intent;
  conflict.reason = "different";
  const auto conflict_result = journal.AcceptOrReplayLifecycle(
      owner, conflict, replay_candidate, InitialStatus(conflict, replay_candidate));
  ASSERT_TRUE(conflict_result.ok());
  EXPECT_EQ(conflict_result.value().replay_status, RequestReplayStatusDTO::kRequestIdConflict);

  const auto second_intent = Intent("request.2", owner, 4U);
  const auto second_acceptance = Acceptance(second_intent, 2U, "lifecycle.2");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, second_intent, second_acceptance).ok());
  const auto full = journal.AcceptOrReplayLifecycle(
      owner, second_intent, second_acceptance, InitialStatus(second_intent, second_acceptance));
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(full.value().replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
}

TEST(SqliteCCLCJournal, EnforcesDurableTransactionAndComponentWatermarks) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto first = Intent("request.1", owner, 3U);
  const auto first_acceptance = Acceptance(first, 1U, "lifecycle.1");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, first, first_acceptance).ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayLifecycle(owner, first, first_acceptance,
                                           InitialStatus(first, first_acceptance))
                  .ok());

  const auto stale = Intent("request.2", owner, 3U);
  const auto stale_acceptance = Acceptance(stale, 2U, "lifecycle.2");
  const auto stale_claim = SeedLifecycleClaim(journal, owner, stale, stale_acceptance);
  ASSERT_TRUE(stale_claim.ok());
  const auto stale_result = journal.AcceptOrReplayLifecycle(owner, stale, stale_acceptance,
                                                            InitialStatus(stale, stale_acceptance));
  ASSERT_FALSE(stale_result.ok());
  EXPECT_EQ(stale_result.error().code, "component_lifecycle_journal_generation_conflict");
  ASSERT_TRUE(ReleaseUnacceptedTestClaim(journal, owner, stale_claim.value()).ok());

  const auto current = Intent("request.3", owner, 4U);
  const auto skipped = Acceptance(current, 3U, "lifecycle.3");
  const auto skipped_claim = SeedLifecycleClaim(journal, owner, current, skipped);
  ASSERT_TRUE(skipped_claim.ok());
  const auto skipped_result =
      journal.AcceptOrReplayLifecycle(owner, current, skipped, InitialStatus(current, skipped));
  ASSERT_FALSE(skipped_result.ok());
  EXPECT_EQ(skipped_result.error().code, "component_lifecycle_journal_generation_conflict");
  ASSERT_TRUE(ReleaseUnacceptedTestClaim(journal, owner, skipped_claim.value()).ok());

  const auto accepted_current = Intent("request.4", owner, 4U);
  const auto next = Acceptance(accepted_current, 2U, "lifecycle.2");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, accepted_current, next).ok());
  ASSERT_TRUE(
      journal
          .AcceptOrReplayLifecycle(owner, accepted_current, next,
                                   InitialStatus(accepted_current, next))
          .ok());
}

TEST(SqliteCCLCJournal, CurrentComponentFactSurvivesTerminalCompactionAndReopen) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

    const auto unseen =
        journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
    ASSERT_TRUE(unseen.ok());
    EXPECT_FALSE(unseen.value().durable_generation.has_value());
    EXPECT_FALSE(unseen.value().current_state.has_value());

    const auto first = Intent("request.1", owner, 3U);
    const auto first_acceptance = Acceptance(first, 1U, "lifecycle.1");
    auto first_status = InitialStatus(first, first_acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, first, first_acceptance).ok());
    ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, first, first_acceptance, first_status).ok());
    auto current = journal.LoadCurrentComponentLifecycleFact(first.controllable_component_id);
    ASSERT_TRUE(current.ok());
    ASSERT_TRUE(current.value().durable_generation.has_value());
    EXPECT_EQ(*current.value().durable_generation, ComponentLifecycleGeneration(4U));
    EXPECT_EQ(current.value().current_state, ComponentLifecycleStateDTO::kDeclared);

    first_status = TerminalSuccess(first_status, 2U);
    ASSERT_TRUE(RecordSettledTerminal(journal, owner, first_status).ok());

    const auto second = Intent("request.2", owner, 4U);
    const auto second_acceptance = Acceptance(second, 2U, "lifecycle.2");
    auto second_status = InitialStatus(second, second_acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, second, second_acceptance).ok());
    second_status.previous_state = ComponentLifecycleStateDTO::kPrepared;
    second_status.current_state = ComponentLifecycleStateDTO::kPrepared;
    second_status.domains.front().previous_state = ComponentLifecycleStateDTO::kPrepared;
    second_status.domains.front().current_state = ComponentLifecycleStateDTO::kPrepared;
    ASSERT_TRUE(second_status.locally_valid());
    ASSERT_TRUE(
        journal.AcceptOrReplayLifecycle(owner, second, second_acceptance, second_status).ok());
    second_status = TerminalSuccess(second_status, 2U);
    ASSERT_TRUE(RecordSettledTerminal(journal, owner, second_status).ok());

    const auto compacted =
        journal.LoadRetainedLifecycleStatus(first_acceptance.lifecycle_transaction_id);
    ASSERT_TRUE(compacted.ok());
    EXPECT_FALSE(compacted.value().has_value());
    current = journal.LoadCurrentComponentLifecycleFact(second.controllable_component_id);
    ASSERT_TRUE(current.ok());
    EXPECT_EQ(current.value().durable_generation, ComponentLifecycleGeneration(5U));
    EXPECT_EQ(current.value().current_state, ComponentLifecycleStateDTO::kPrepared);
    const auto coherent = journal.LoadCurrentSnapshotFacts({second.controllable_component_id},
                                                           {ControlDomainId("domain.1")});
    ASSERT_TRUE(coherent.ok()) << coherent.error().message;
    ASSERT_EQ(coherent.value().components.size(), 1U);
    EXPECT_EQ(coherent.value().components.front(), current.value());
    ASSERT_EQ(coherent.value().domains.size(), 1U);
    EXPECT_FALSE(coherent.value().domains.front().current_active_control_configuration.has_value());
    journal.Close();
  }

  SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U);
  const auto current =
      reopened.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().durable_generation, ComponentLifecycleGeneration(5U));
  EXPECT_EQ(current.value().current_state, ComponentLifecycleStateDTO::kPrepared);
}

TEST(SqliteCCLCJournal,
     SettledPartialEffectDoesNotRestorePredecessorActiveStateOnReopen) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.partial.1");
  const ControllableComponentId component_id("component.1");
  ComponentLifecycleGeneration accepted_component_generation(0U);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
    const auto route = ActiveConfiguration(ExpectedDomain());
    const auto baseline = journal.PlanNativeApply(
        NativeApplyPlanRequest("apply.partial.baseline", 1U, "bundle.partial.baseline", route));
    ASSERT_TRUE(baseline.ok()) << baseline.error().code << ": "
                               << baseline.error().message;
    ASSERT_EQ(baseline.value().components.size(), 1U);
    ASSERT_EQ(baseline.value().components.front().state,
              ComponentLifecycleStateDTO::kActive);
    const auto adopted = journal.AdoptNativeApplyFacts(
        {NativeApplyPublication("apply.partial.baseline", 1U,
                                "bundle.partial.baseline", 2U),
         baseline.value().components, {route}});
    ASSERT_TRUE(adopted.ok()) << adopted.error().code << ": "
                              << adopted.error().message;

    const auto active_before_disable =
        journal.LoadCurrentComponentLifecycleFact(component_id);
    ASSERT_TRUE(active_before_disable.ok());
    EXPECT_EQ(active_before_disable.value().current_state,
              ComponentLifecycleStateDTO::kActive);

    const auto intent = DisableIntent(
        "request.partial.1", owner,
        baseline.value().components.front().generation.value());
    const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
    accepted_component_generation = acceptance.component_lifecycle_generation;
    auto status = InitialDisableStatus(intent, acceptance);
    ASSERT_TRUE(status.locally_valid());
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    const auto accepted_lifecycle =
        journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status);
    ASSERT_TRUE(accepted_lifecycle.ok()) << accepted_lifecycle.error().code << ": "
                                         << accepted_lifecycle.error().message;
    const auto disabling = journal.LoadCurrentComponentLifecycleFact(
        intent.controllable_component_id);
    ASSERT_TRUE(disabling.ok());
    EXPECT_EQ(disabling.value().current_state,
              ComponentLifecycleStateDTO::kDisabling);

    const ErrorDTO cause{"provider_partial_effect",
                         "one lifecycle domain changed incompletely", {}};
    status.status_sequence = 2U;
    status.phase = ComponentLifecycleTransactionPhaseDTO::kFailed;
    status.domains.front().action_state =
        ComponentLifecycleDomainActionStateDTO::kFailed;
    status.domains.front().effect =
        ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
    status.domains.front().error = cause;
    status.terminal_result =
        ComponentLifecycleTerminalResultDTO::kFailedPartialEffect;
    status.error = cause;
    status.latest_updated_at_ns += 10;
    status.remaining_duration_ns = 0;
    ASSERT_TRUE(status.locally_valid());
    const auto settled = RecordSettledTerminal(journal, owner, status);
    ASSERT_TRUE(settled.ok()) << settled.error().code << ": "
                              << settled.error().message;
    const auto after_failure = journal.LoadCurrentComponentLifecycleFact(
        intent.controllable_component_id);
    ASSERT_TRUE(after_failure.ok());
    EXPECT_EQ(after_failure.value().durable_generation,
              acceptance.component_lifecycle_generation);
    EXPECT_FALSE(after_failure.value().current_state.has_value());
    journal.Close();
  }

  SqliteCCLCJournal reopened(path, CellId("cell.1"), 8U);
  const auto recovered = reopened.LoadCurrentComponentLifecycleFact(component_id);
  ASSERT_TRUE(recovered.ok());
  EXPECT_EQ(recovered.value().durable_generation, accepted_component_generation);
  EXPECT_FALSE(recovered.value().current_state.has_value());
  const auto coherent = reopened.LoadCurrentSnapshotFacts(
      {component_id}, {ControlDomainId("domain.1")});
  ASSERT_TRUE(coherent.ok()) << coherent.error().message;
  ASSERT_EQ(coherent.value().components.size(), 1U);
  EXPECT_FALSE(coherent.value().components.front().current_state.has_value());
}

TEST(SqliteCCLCJournal, UnresolvedContainmentRetainsGenerationWithoutInventingState) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto intent = DisableIntent("request.1", owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  const auto status = InitialUnresolvedDisableStatus(intent, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status).ok());

  const auto current = journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id);
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().durable_generation, ComponentLifecycleGeneration(4U));
  EXPECT_FALSE(current.value().current_state.has_value());
}

TEST(SqliteCCLCJournal, StaleDisableUsesTheImmediateDurableSuccessorAndReplaysFirst) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto first = Intent("request.1", owner, 3U);
  const auto first_acceptance = Acceptance(first, 1U, "lifecycle.1");
  const auto first_status = InitialStatus(first, first_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, first, first_acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, first, first_acceptance, first_status).ok());

  const auto stale_disable = DisableIntent("request.2", owner, 3U);
  auto stale_disable_acceptance = Acceptance(stale_disable, 2U, "lifecycle.2");
  stale_disable_acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(5U);
  const auto stale_disable_status = InitialDisableStatus(stale_disable, stale_disable_acceptance);
  ASSERT_TRUE(stale_disable_status.locally_valid());
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, stale_disable, stale_disable_acceptance).ok());
  const auto accepted = journal.AcceptOrReplayLifecycle(
      owner, stale_disable, stale_disable_acceptance, stale_disable_status);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  const auto replay = journal.AcceptOrReplayLifecycle(owner, first, first_acceptance, first_status);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  ASSERT_TRUE(replay.value().acceptance.has_value());
  EXPECT_EQ(replay.value().acceptance->component_lifecycle_generation,
            first_acceptance.component_lifecycle_generation);

  const auto stale_again = DisableIntent("request.3", owner, 3U);
  auto regressed = Acceptance(stale_again, 3U, "lifecycle.3");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, stale_again, regressed).ok());
  const auto regressed_result = journal.AcceptOrReplayLifecycle(
      owner, stale_again, regressed, InitialDisableStatus(stale_again, regressed));
  ASSERT_FALSE(regressed_result.ok());
  EXPECT_EQ(regressed_result.error().code, "component_lifecycle_journal_generation_conflict");

  regressed.component_lifecycle_generation = ComponentLifecycleGeneration(7U);
  const auto gap_result = journal.AcceptOrReplayLifecycle(
      owner, stale_again, regressed, InitialDisableStatus(stale_again, regressed));
  ASSERT_FALSE(gap_result.ok());
  EXPECT_EQ(gap_result.error().code, "component_lifecycle_journal_generation_conflict");

  const auto ahead = DisableIntent("request.4", owner, 6U);
  auto ahead_acceptance = Acceptance(ahead, 3U, "lifecycle.4");
  ahead_acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(7U);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, ahead, ahead_acceptance).ok());
  const auto ahead_result = journal.AcceptOrReplayLifecycle(
      owner, ahead, ahead_acceptance, InitialDisableStatus(ahead, ahead_acceptance));
  ASSERT_FALSE(ahead_result.ok());
  EXPECT_EQ(ahead_result.error().code, "component_lifecycle_journal_generation_conflict");

  regressed.component_lifecycle_generation = ComponentLifecycleGeneration(6U);
  ASSERT_TRUE(journal
                  .AcceptOrReplayLifecycle(owner, stale_again, regressed,
                                           InitialDisableStatus(stale_again, regressed))
                  .ok());
}

TEST(SqliteCCLCJournal, ResolvedDisableSeedsAnAbsentWatermarkFromItsSelectedPredecessor) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto stale_disable = DisableIntent("request.1", owner, 3U);
  auto acceptance = Acceptance(stale_disable, 1U, "lifecycle.1");
  acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(8U);
  const auto status = InitialDisableStatus(stale_disable, acceptance);
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, stale_disable, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, stale_disable, acceptance, status).ok());

  const auto current = Intent("request.2", owner, 8U);
  const auto current_acceptance = Acceptance(current, 2U, "lifecycle.2");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, current, current_acceptance).ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplayLifecycle(owner, current, current_acceptance,
                                           InitialStatus(current, current_acceptance))
                  .ok());
}

TEST(SqliteCCLCJournal, UnresolvedFullStopSeedsAnAbsentWatermarkOnlyFromExpectation) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto unresolved = DisableIntent("request.1", owner, 3U);
  auto gap = Acceptance(unresolved, 1U, "lifecycle.1");
  gap.component_lifecycle_generation = ComponentLifecycleGeneration(5U);
  const auto gap_status = InitialUnresolvedDisableStatus(unresolved, gap);
  ASSERT_TRUE(gap_status.locally_valid());
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, unresolved, gap).ok());
  const auto gap_result = journal.AcceptOrReplayLifecycle(owner, unresolved, gap, gap_status);
  ASSERT_FALSE(gap_result.ok());
  EXPECT_EQ(gap_result.error().code, "component_lifecycle_journal_generation_conflict");

  const auto acceptance = Acceptance(unresolved, 1U, "lifecycle.1");
  const auto status = InitialUnresolvedDisableStatus(unresolved, acceptance);
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, unresolved, acceptance, status).ok());
}

TEST(SqliteCCLCJournal, PersistsAttemptBeforeEffectAndProjectsOnlyItsExactResult) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto lifecycle_intent = Intent("request.1", owner, 3U);
  const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  auto status = InitialStatus(lifecycle_intent, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, acceptance, status).ok());

  const auto primitive = PrimitiveIntent(status);
  const auto prepared_attempt = PreparedPrimitiveEffect(primitive);
  status = PreparedEffectStatus(status);
  const auto prepared = journal.RecordEffectPrepared(owner, prepared_attempt, status);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  const auto prepared_replay = journal.RecordEffectPrepared(owner, prepared_attempt, status);
  ASSERT_TRUE(prepared_replay.ok());
  EXPECT_EQ(prepared_replay.value(), prepared.value());
  auto conflicting_prepared = prepared_attempt;
  conflicting_prepared.effect_sequence = 2U;
  const auto prepared_conflict = journal.RecordEffectPrepared(owner, conflicting_prepared, status);
  ASSERT_FALSE(prepared_conflict.ok());
  EXPECT_EQ(prepared_conflict.error().code, "component_lifecycle_journal_attempt_conflict");

  const auto result = PrimitiveSuccess(primitive);
  const auto completed_attempt = CompletedPrimitiveEffect(primitive, result);
  status = CompletedEffectStatus(status, result);
  const auto recorded = journal.RecordEffectCompleted(owner, completed_attempt, status);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  EXPECT_EQ(recorded.value(), prepared.value());
  EXPECT_TRUE(journal.RecordEffectCompleted(owner, completed_attempt, status).ok());
  auto conflicting_result = result;
  conflicting_result.known_result = LifecyclePrimitiveKnownResultDTO::kFailed;
  conflicting_result.error = ErrorDTO{"failed", "conflict", {}};
  auto conflicting_status = status;
  conflicting_status.domains.front().primitive_attempts = {conflicting_result};
  EXPECT_FALSE(journal
                   .RecordEffectCompleted(owner,
                                          CompletedPrimitiveEffect(primitive, conflicting_result),
                                          conflicting_status)
                   .ok());

  auto advanced = status;
  advanced.status_sequence += 1U;
  advanced.latest_updated_at_ns += 10;
  advanced.remaining_duration_ns -= 10;
  ASSERT_TRUE(advanced.locally_valid());
  ASSERT_TRUE(journal.RecordLifecycleStatus(owner, advanced).ok());
  EXPECT_TRUE(journal.RecordEffectCompleted(owner, completed_attempt, status).ok());

  const auto terminal = TerminalSuccess(advanced, 5U);
  ASSERT_TRUE(terminal.locally_valid());
  EXPECT_TRUE(journal.RecordLifecycleStatus(owner, terminal).ok());
}

TEST(SqliteCCLCJournal, ReplacementRecoversPreparedAttemptAsPossibleEffectWithoutRedispatch) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch first_owner("owner.1");
  ComponentLifecycleStatusDTO status = [&]() {
    SqliteCCLCJournal first(path, CellId("cell.1"), 8U);
    EXPECT_TRUE(BeginTestOwnerEpoch(first, first_owner, 1, 8U).ok());
    const auto lifecycle_intent = Intent("request.1", first_owner, 3U);
    const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
    auto initial = InitialStatus(lifecycle_intent, acceptance);
    EXPECT_TRUE(SeedLifecycleClaim(first, first_owner, lifecycle_intent, acceptance).ok());
    EXPECT_TRUE(
        first.AcceptOrReplayLifecycle(first_owner, lifecycle_intent, acceptance, initial).ok());
    initial = PreparedEffectStatus(initial);
    EXPECT_TRUE(first
                    .RecordEffectPrepared(
                        first_owner, PreparedPrimitiveEffect(PrimitiveIntent(initial)), initial)
                    .ok());
    first.Close();
    return initial;
  }();

  SqliteCCLCJournal replacement(path, CellId("cell.1"), 8U);
  RequestOwnerEpoch second_owner("owner.2");
  const auto recovered = BeginTestOwnerEpoch(replacement, second_owner, 500, 8U);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().interrupted_transactions.size(), 1U);
  ASSERT_EQ(recovered.value().effect_sequence_watermarks.size(), 1U);
  EXPECT_EQ(recovered.value().effect_sequence_watermarks.front().highest_effect_sequence, 1U);
  ASSERT_EQ(recovered.value().unresolved_effect_attempts.size(), 1U);
  const auto& unresolved = recovered.value().unresolved_effect_attempts.front();
  EXPECT_EQ(unresolved.state, CCLCEffectAttemptStateDTO::kPrepared);
  ASSERT_TRUE(unresolved.lifecycle_primitive_intent.has_value());
  EXPECT_EQ(unresolved.lifecycle_primitive_intent->lifecycle_primitive_attempt_id,
            LifecyclePrimitiveAttemptId("attempt.1"));

  const auto stale_replay =
      replacement.AcceptOrReplayLifecycle(first_owner, status.accepted_intent, status.acceptance,
                                          InitialStatus(status.accepted_intent, status.acceptance));
  ASSERT_TRUE(stale_replay.ok());
  EXPECT_EQ(stale_replay.value().replay_status, RequestReplayStatusDTO::kStaleRequestOwnerEpoch);

  auto uncertain = PrimitiveSuccess(*unresolved.lifecycle_primitive_intent);
  uncertain.dispatch_state = LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain;
  uncertain.effect = LifecyclePrimitiveEffectDTO::kPossible;
  uncertain.known_result.reset();
  uncertain.error =
      ErrorDTO{"owner_replaced_after_prepare", "dispatch outcome requires observation", {}};
  status.status_sequence = 3U;
  status.status_owner_epoch = second_owner;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown;
  status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kEffectUncertain;
  status.domains.front().primitive_attempts = {uncertain};
  status.domains.front().error = uncertain.error;
  status.reconciliation_required = true;
  status.error = uncertain.error;
  status.latest_updated_at_ns = 500;
  status.remaining_duration_ns = 0;
  ASSERT_TRUE(status.locally_valid());
  auto completed = unresolved;
  completed.state = CCLCEffectAttemptStateDTO::kCompleted;
  completed.lifecycle_primitive_result = uncertain;
  ASSERT_TRUE(replacement.RecordEffectCompleted(second_owner, completed, status).ok());
}

TEST(SqliteCCLCJournal, DisableHandoffPreparedAndCompletedStateAdvanceAtomically) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto lifecycle_intent = DisableIntent("request.1", owner, 3U);
  const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  auto status = InitialDisableStatus(lifecycle_intent, acceptance);
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, acceptance, status).ok());

  const auto request = DisableRequest(status);
  ASSERT_TRUE(request.locally_valid());
  const auto prepared_attempt = PreparedDisableEffect(request);
  status = PreparedDisableStatus(status);
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(journal.RecordEffectPrepared(owner, prepared_attempt, status).ok());

  auto recovered_owner = RequestOwnerEpoch("owner.2");
  journal.Close();
  SqliteCCLCJournal replacement(JournalPath(temporary), CellId("cell.1"), 8U);
  const auto recovery = BeginTestOwnerEpoch(replacement, recovered_owner, 500, 8U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().unresolved_effect_attempts.size(), 1U);
  EXPECT_EQ(recovery.value().unresolved_effect_attempts.front().effect_kind,
            CCLCEffectKindDTO::kKernelDisableHandoff);
  ASSERT_TRUE(
      recovery.value().unresolved_effect_attempts.front().disable_handoff_request.has_value());
  EXPECT_EQ(*recovery.value().unresolved_effect_attempts.front().disable_handoff_request, request);

  const auto accepted = DisableAcceptance(request);
  const auto handoff = DisableReady(request, accepted);
  ASSERT_TRUE(accepted.locally_valid());
  ASSERT_TRUE(handoff.locally_valid());
  status = UncertainDisableStatus(status, recovered_owner);
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(replacement.RecordLifecycleStatus(recovered_owner, status).ok());
  status = CompletedDisableStatus(status, handoff);
  ASSERT_TRUE(status.locally_valid());
  const auto completed = replacement.RecordEffectCompleted(
      recovered_owner, CompletedDisableEffect(request, accepted), status);
  ASSERT_TRUE(completed.ok()) << completed.error().code << ": " << completed.error().message;

  const auto retained =
      replacement.LoadRetainedLifecycleStatus(acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value().has_value());
  EXPECT_EQ(*retained.value(), status);

  auto evidence_regression = status;
  evidence_regression.status_sequence += 1U;
  evidence_regression.phase = ComponentLifecycleTransactionPhaseDTO::kAccepted;
  evidence_regression.current_state = ComponentLifecycleStateDTO::kActive;
  evidence_regression.domains.front().current_state = ComponentLifecycleStateDTO::kActive;
  evidence_regression.domains.front().action_state =
      ComponentLifecycleDomainActionStateDTO::kReserved;
  evidence_regression.domains.front().effect = ComponentLifecycleDomainEffectDTO::kNone;
  evidence_regression.configuration_fence.reset();
  evidence_regression.disable_handoff.reset();
  evidence_regression.latest_updated_at_ns += 10;
  ASSERT_TRUE(evidence_regression.locally_valid());
  const auto regressed = replacement.RecordLifecycleStatus(recovered_owner, evidence_regression);
  ASSERT_FALSE(regressed.ok());
  EXPECT_EQ(regressed.error().code, "component_lifecycle_journal_status_regressed");

  auto terminal_with_held_fence = status;
  terminal_with_held_fence.status_sequence += 1U;
  terminal_with_held_fence.current_state = ComponentLifecycleStateDTO::kDisabled;
  terminal_with_held_fence.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
  terminal_with_held_fence.domains.front().current_state = ComponentLifecycleStateDTO::kDisabled;
  terminal_with_held_fence.domains.front().action_state =
      ComponentLifecycleDomainActionStateDTO::kSucceeded;
  terminal_with_held_fence.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
  terminal_with_held_fence.latest_updated_at_ns += 10;
  terminal_with_held_fence.remaining_duration_ns = 0;
  EXPECT_FALSE(terminal_with_held_fence.locally_valid());
}

TEST(SqliteCCLCJournal, CompletedDisableHandoffRejectsOmissionsAndAcceptsFullWidening) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto lifecycle_intent = DisableIntent("request.1", owner, 3U);
  const auto lifecycle_acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  auto status = InitialDisableStatus(lifecycle_intent, lifecycle_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, lifecycle_acceptance).ok());
  ASSERT_TRUE(
      journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, lifecycle_acceptance, status).ok());

  const auto request = DisableRequest(status);
  auto handoff_acceptance = DisableAcceptance(request);
  handoff_acceptance.scope_resolution = DisableHandoffScopeResolutionDTO::kPromotedToFull;
  status = PreparedDisableStatus(status);
  ASSERT_TRUE(journal.RecordEffectPrepared(owner, PreparedDisableEffect(request), status).ok());

  const auto completion = CompletedDisableEffect(request, handoff_acceptance);
  const auto expect_omission_rejected = [&](ConfigurationFenceScopeDTO narrowed_scope) {
    const auto handoff =
        DisableReadyWithScope(request, handoff_acceptance, std::move(narrowed_scope));
    const auto successor = CompletedDisableStatus(status, handoff);
    ASSERT_TRUE(handoff.locally_valid());
    ASSERT_TRUE(successor.locally_valid());
    const auto result = journal.RecordEffectCompleted(owner, completion, successor);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "component_lifecycle_journal_attempt_conflict");
  };

  auto missing_domain = FullDisableFenceScope();
  missing_domain.domains.erase(missing_domain.domains.begin());
  expect_omission_rejected(std::move(missing_domain));

  auto missing_binding = FullDisableFenceScope();
  missing_binding.bindings.erase(missing_binding.bindings.begin());
  expect_omission_rejected(std::move(missing_binding));

  auto missing_resource = FullDisableFenceScope();
  missing_resource.resources.erase(missing_resource.resources.begin());
  expect_omission_rejected(std::move(missing_resource));

  const auto widened = DisableReadyWithScope(request, handoff_acceptance, FullDisableFenceScope());
  const auto widened_successor = CompletedDisableStatus(status, widened);
  ASSERT_TRUE(widened_successor.locally_valid());
  const auto accepted = journal.RecordEffectCompleted(owner, completion, widened_successor);
  ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
}

TEST(SqliteCCLCJournal, PromotedFullDisableHandoffCannotNarrowItsPreparedScope) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto lifecycle_intent = DisableIntent("request.1", owner, 3U);
  const auto lifecycle_acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  auto status = InitialDisableStatus(lifecycle_intent, lifecycle_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, lifecycle_acceptance).ok());
  ASSERT_TRUE(
      journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, lifecycle_acceptance, status).ok());

  const auto request = PromotedFullDisableRequest(status);
  const auto handoff_acceptance = DisableAcceptance(request);
  ASSERT_TRUE(request.locally_valid());
  status = PreparedDisableStatus(status);
  ASSERT_TRUE(journal.RecordEffectPrepared(owner, PreparedDisableEffect(request), status).ok());

  const auto completion = CompletedDisableEffect(request, handoff_acceptance);
  const auto narrowed = DisableReadyWithScope(request, handoff_acceptance, DisableFenceScope());
  const auto narrowed_successor = CompletedDisableStatus(status, narrowed);
  ASSERT_TRUE(narrowed_successor.locally_valid());
  const auto rejected = journal.RecordEffectCompleted(owner, completion, narrowed_successor);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "component_lifecycle_journal_attempt_conflict");

  const auto full = DisableReadyWithScope(request, handoff_acceptance, FullDisableFenceScope());
  const auto full_successor = CompletedDisableStatus(status, full);
  ASSERT_TRUE(full_successor.locally_valid());
  const auto accepted = journal.RecordEffectCompleted(owner, completion, full_successor);
  ASSERT_TRUE(accepted.ok()) << accepted.error().code << ": " << accepted.error().message;
}

TEST(SqliteCCLCJournal, PromotedFullNoSnapshotDisableAbsorbsOneSnapshotAndRetainsItOnRestart) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  ComponentLifecycleStatusDTO augmented = [&] {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    const auto begun = BeginTestOwnerEpoch(journal, owner, 1, 8U);
    if (!begun.ok()) {
      throw std::runtime_error(begun.error().code + ": " + begun.error().message);
    }
    const auto ready = PersistPromotedFullNoSnapshotHandoff(journal, owner);
    auto snapshot = AugmentedPromotedFullDisableStatus(ready);
    if (!snapshot.locally_valid()) {
      throw std::runtime_error("the promoted-Full snapshot fixture is invalid");
    }
    const auto recorded = journal.RecordLifecycleStatus(owner, snapshot);
    if (!recorded.ok()) {
      throw std::runtime_error(recorded.error().code + ": " + recorded.error().message);
    }
    journal.Close();
    return snapshot;
  }();

  SqliteCCLCJournal reopened(path, CellId("cell.1"), 8U);
  const auto retained =
      reopened.LoadRetainedLifecycleStatus(augmented.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  ASSERT_TRUE(retained.value().has_value());
  EXPECT_EQ(*retained.value(), augmented);

  RequestOwnerEpoch replacement_owner("owner.2");
  const auto recovery = BeginTestOwnerEpoch(reopened, replacement_owner, 500, 8U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 1U);
  EXPECT_EQ(recovery.value().interrupted_transactions.front(), augmented);
}

TEST(SqliteCCLCJournal, PromotedFullNoSnapshotDisableRejectsMutationAndMalformedSnapshot) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto ready = PersistPromotedFullNoSnapshotHandoff(journal, owner);

  const auto primitive = PrimitiveIntent(ready);
  const auto prepared_vendor_status = PreparedEffectStatus(ready);
  ASSERT_TRUE(prepared_vendor_status.locally_valid());
  const auto vendor = journal.RecordEffectPrepared(owner, PreparedPrimitiveEffect(primitive, 2U),
                                                   prepared_vendor_status);
  ASSERT_FALSE(vendor.ok());
  EXPECT_EQ(vendor.error().code, "component_lifecycle_journal_attempt_invalid");

  auto malformed = AugmentedPromotedFullDisableStatus(ready);
  auto& current_only = malformed.domains.back();
  current_only.current_scope->control_domain_revision =
      ControlDomainRevision("outside-full-fence-revision");
  current_only.resolved_provider_occurrence->control_domain_revision =
      current_only.current_scope->control_domain_revision;
  ASSERT_TRUE(malformed.locally_valid());
  const auto rejected = journal.RecordLifecycleStatus(owner, malformed);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "component_lifecycle_journal_status_regressed");
}

TEST(SqliteCCLCJournal, PromotedFullSnapshotClosureFreezesAfterFirstAugmentation) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto ready = PersistPromotedFullNoSnapshotHandoff(journal, owner);
  const auto augmented = AugmentedPromotedFullDisableStatus(ready);
  ASSERT_TRUE(augmented.locally_valid());
  ASSERT_TRUE(journal.RecordLifecycleStatus(owner, augmented).ok());

  auto narrowed = augmented;
  narrowed.status_sequence += 1U;
  narrowed.domains.pop_back();
  narrowed.latest_updated_at_ns += 10;
  narrowed.remaining_duration_ns -= 10;
  ASSERT_TRUE(narrowed.locally_valid());
  const auto narrowed_result = journal.RecordLifecycleStatus(owner, narrowed);
  ASSERT_FALSE(narrowed_result.ok());
  EXPECT_EQ(narrowed_result.error().code, "component_lifecycle_journal_status_regressed");

  auto changed_provider = augmented;
  changed_provider.status_sequence += 1U;
  changed_provider.domains.front().resolved_provider_occurrence->lifecycle_plan_catalog_digest =
      ContentDigest("different-catalog-digest");
  changed_provider.latest_updated_at_ns += 10;
  changed_provider.remaining_duration_ns -= 10;
  ASSERT_TRUE(changed_provider.locally_valid());
  const auto provider_result = journal.RecordLifecycleStatus(owner, changed_provider);
  ASSERT_FALSE(provider_result.ok());
  EXPECT_EQ(provider_result.error().code, "component_lifecycle_journal_status_regressed");
}

TEST(SqliteCCLCJournal, DisableFenceReleaseAndTerminalStatusCommitAtomicallyAndSurviveReopen) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  std::optional<ComponentLifecycleStatusDTO> terminal_status;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

    const auto lifecycle_intent = DisableIntent("request.1", owner, 3U);
    const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
    auto status = InitialDisableStatus(lifecycle_intent, acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, acceptance).ok());
    ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, acceptance, status).ok());

    const auto handoff_request = DisableRequest(status);
    const auto handoff_acceptance = DisableAcceptance(handoff_request);
    const auto handoff_status = DisableReady(handoff_request, handoff_acceptance);
    status = PreparedDisableStatus(status);
    ASSERT_TRUE(
        journal.RecordEffectPrepared(owner, PreparedDisableEffect(handoff_request), status).ok());
    status = CompletedDisableStatus(status, handoff_status);
    ASSERT_TRUE(journal
                    .RecordEffectCompleted(
                        owner, CompletedDisableEffect(handoff_request, handoff_acceptance), status)
                    .ok());

    const auto premature_release = DisableReleaseRequest(status);
    const auto premature_status = PreparedReleaseStatus(status);
    ASSERT_TRUE(premature_release.locally_valid());
    ASSERT_TRUE(premature_status.locally_valid());
    const auto premature = journal.RecordEffectPrepared(
        owner, PreparedReleaseEffect(premature_release), premature_status);
    ASSERT_FALSE(premature.ok());
    EXPECT_EQ(premature.error().code, "component_lifecycle_journal_attempt_invalid");

    status = ResolvedDisableStatus(status);
    ASSERT_TRUE(status.domains.front().reconciliation_evidence->locally_valid());
    EXPECT_EQ(status.domains.front().requested_state.presence,
              ComponentLifecycleRequestedStatePresenceDTO::kResolved);
    ASSERT_TRUE(status.domains.front().requested_state.state.has_value());
    EXPECT_EQ(status.domains.front().current_state, *status.domains.front().requested_state.state);
    EXPECT_EQ(status.domains.front().reconciliation_evidence->observed_state,
              status.domains.front().current_state);
    EXPECT_EQ(status.domains.front().reconciliation_evidence->postcondition_result,
              LifecyclePostconditionResultDTO::kSatisfied);
    ASSERT_TRUE(status.domains.front().locally_valid());
    ASSERT_TRUE(status.disable_handoff->locally_valid());
    ASSERT_TRUE(status.configuration_fence->locally_valid());
    ASSERT_TRUE(status.locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(owner, status).ok());

    const auto unprepared_pending = ReleasePendingStatus(status);
    ASSERT_TRUE(unprepared_pending.locally_valid());
    const auto unprepared_pending_result = journal.RecordLifecycleStatus(owner, unprepared_pending);
    ASSERT_FALSE(unprepared_pending_result.ok());
    EXPECT_EQ(unprepared_pending_result.error().code,
              "component_lifecycle_journal_attempt_conflict");

    const auto release_request = DisableReleaseRequest(status);
    ASSERT_TRUE(release_request.locally_valid());
    auto wrong_fence_generation = release_request;
    wrong_fence_generation.current_admission_snapshot.configuration_fence_generation =
        ConfigurationFenceGeneration(
            release_request.ready_handoff.configuration_fence_generation.value() + 1U);
    EXPECT_FALSE(wrong_fence_generation.locally_valid());
    auto wrong_active_configuration = release_request;
    auto& current_configuration =
        wrong_active_configuration.current_admission_snapshot.current_active_control_configurations
            .front()
            .active_control_configuration;
    current_configuration.active_control_configuration_generation =
        ActiveControlConfigurationGeneration(
            current_configuration.active_control_configuration_generation->value() + 1U);
    EXPECT_FALSE(wrong_active_configuration.locally_valid());
    auto prepared_status = PreparedReleaseStatus(status);
    ASSERT_TRUE(prepared_status.locally_valid());

    auto stale_precondition = release_request;
    stale_precondition.release_precondition_status_digest = ContentDigest("stale-status-digest");
    const auto stale_prepare = journal.RecordEffectPrepared(
        owner, PreparedReleaseEffect(stale_precondition), prepared_status);
    ASSERT_FALSE(stale_prepare.ok());
    EXPECT_EQ(stale_prepare.error().code, "component_lifecycle_journal_attempt_invalid");

    ASSERT_TRUE(
        journal.RecordEffectPrepared(owner, PreparedReleaseEffect(release_request), prepared_status)
            .ok());
    prepared_status = ReleasePendingStatus(prepared_status);
    ASSERT_TRUE(prepared_status.locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(owner, prepared_status).ok());
    const auto missing_receipt = journal.RecordEffectCompleted(
        owner,
        CompletedReleaseEffect(release_request, DisableFenceReleaseEffectDispositionDTO::kReleased,
                               std::nullopt),
        prepared_status);
    ASSERT_FALSE(missing_receipt.ok());
    EXPECT_EQ(missing_receipt.error().code, "component_lifecycle_journal_attempt_invalid");

    const auto receipt = DisableReleaseReceipt(release_request);
    ASSERT_TRUE(receipt.locally_valid());
    terminal_status = CompletedReleaseStatus(prepared_status, receipt);
    ASSERT_TRUE(terminal_status->locally_valid());
    const auto completed = journal.RecordEffectCompleted(
        owner,
        CompletedReleaseEffect(release_request, DisableFenceReleaseEffectDispositionDTO::kReleased,
                               receipt),
        *terminal_status);
    ASSERT_TRUE(completed.ok()) << completed.error().code << ": " << completed.error().message;

    const auto retained = journal.LoadRetainedLifecycleStatus(acceptance.lifecycle_transaction_id);
    ASSERT_TRUE(retained.ok());
    ASSERT_TRUE(retained.value().has_value());
    EXPECT_EQ(*retained.value(), *terminal_status);
    journal.Close();
  }

  SqliteCCLCJournal reopened(path, CellId("cell.1"), 8U);
  const auto retained =
      reopened.LoadRetainedLifecycleStatus(terminal_status->acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value().has_value());
  EXPECT_EQ(*retained.value(), *terminal_status);
}

TEST(SqliteCCLCJournal, ReplacementRecoversPreparedFenceReleaseAndCommitsExactLateReceipt) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch first_owner("owner.1");
  std::optional<DisableFenceReleaseRequestDTO> release_request;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, first_owner, 1, 8U).ok());
    const auto lifecycle_intent = DisableIntent("request.1", first_owner, 3U);
    const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
    auto status = InitialDisableStatus(lifecycle_intent, acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(journal, first_owner, lifecycle_intent, acceptance).ok());
    ASSERT_TRUE(
        journal.AcceptOrReplayLifecycle(first_owner, lifecycle_intent, acceptance, status).ok());
    const auto handoff_request = DisableRequest(status);
    const auto handoff_acceptance = DisableAcceptance(handoff_request);
    status = PreparedDisableStatus(status);
    ASSERT_TRUE(
        journal.RecordEffectPrepared(first_owner, PreparedDisableEffect(handoff_request), status)
            .ok());
    status = CompletedDisableStatus(status, DisableReady(handoff_request, handoff_acceptance));
    ASSERT_TRUE(
        journal
            .RecordEffectCompleted(
                first_owner, CompletedDisableEffect(handoff_request, handoff_acceptance), status)
            .ok());
    status = ResolvedDisableStatus(status);
    ASSERT_TRUE(status.locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(first_owner, status).ok());
    release_request = DisableReleaseRequest(status);
    status = PreparedReleaseStatus(status);
    ASSERT_TRUE(
        journal.RecordEffectPrepared(first_owner, PreparedReleaseEffect(*release_request), status)
            .ok());
    journal.Close();
  }

  ASSERT_TRUE(release_request.has_value());
  SqliteCCLCJournal replacement(path, CellId("cell.1"), 8U);
  RequestOwnerEpoch replacement_owner("owner.2");
  const auto recovery = BeginTestOwnerEpoch(replacement, replacement_owner, 500, 8U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 1U);
  ASSERT_EQ(recovery.value().unresolved_effect_attempts.size(), 1U);
  const auto& unresolved = recovery.value().unresolved_effect_attempts.front();
  EXPECT_EQ(unresolved.effect_kind, CCLCEffectKindDTO::kKernelDisableFenceRelease);
  ASSERT_TRUE(unresolved.disable_fence_release_request.has_value());
  EXPECT_EQ(*unresolved.disable_fence_release_request, *release_request);
  auto status = recovery.value().interrupted_transactions.front();
  EXPECT_FALSE(status.terminal_result.has_value());
  EXPECT_EQ(status.configuration_fence->state, ConfigurationFenceState::kHeld);
  EXPECT_FALSE(status.disable_fence_released.has_value());

  status.status_sequence += 1U;
  status.status_owner_epoch = replacement_owner;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown;
  status.reconciliation_required = true;
  status.error =
      ErrorDTO{"disable_release_response_unknown", "reconciling the prepared release", {}};
  status.latest_updated_at_ns = 500;
  status.remaining_duration_ns = 0;
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(replacement.RecordLifecycleStatus(replacement_owner, status).ok());

  const auto receipt = DisableReleaseReceipt(*release_request);
  auto terminal = CompletedReleaseStatus(status, receipt);
  ASSERT_TRUE(terminal.locally_valid());
  auto completed = unresolved;
  completed.state = CCLCEffectAttemptStateDTO::kCompleted;
  completed.disable_fence_release_result = DisableFenceReleaseEffectResultDTO{
      *release_request, DisableFenceReleaseEffectDispositionDTO::kReleased, receipt};
  const auto result = replacement.RecordEffectCompleted(replacement_owner, completed, terminal);
  ASSERT_TRUE(result.ok()) << result.error().message;

  const auto retained =
      replacement.LoadRetainedLifecycleStatus(terminal.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value().has_value());
  EXPECT_EQ(*retained.value(), terminal);
  const auto next_epoch = replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.3"), 900, 8U);
  ASSERT_TRUE(next_epoch.ok());
  EXPECT_TRUE(next_epoch.value().interrupted_transactions.empty());
  EXPECT_TRUE(next_epoch.value().unresolved_effect_attempts.empty());
}

TEST(SqliteCCLCJournal, ProvenNoEffectHandoffUsesAFreshKernelEpochSuccessorWithoutRedispatch) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto lifecycle_intent = DisableIntent("request.1", owner, 3U);
  const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  auto status = InitialDisableStatus(lifecycle_intent, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, acceptance, status).ok());

  const auto first_request =
      DisableRequest(status, RequestOwnerEpoch("kernel-owner.1"), "disable-handoff.1");
  status = PreparedDisableStatus(status);
  ASSERT_TRUE(
      journal.RecordEffectPrepared(owner, PreparedDisableEffect(first_request), status).ok());

  const auto overlapping_request =
      DisableRequest(status, RequestOwnerEpoch("kernel-owner.2"), "disable-handoff.2");
  const auto overlapping_status = PreparedDisableStatus(status);
  const auto overlapping = journal.RecordEffectPrepared(
      owner, PreparedDisableEffect(overlapping_request, 2U), overlapping_status);
  ASSERT_FALSE(overlapping.ok());
  EXPECT_EQ(overlapping.error().code, "component_lifecycle_journal_attempt_unresolved");

  auto proven_not_seen_status = status;
  proven_not_seen_status.status_sequence += 1U;
  proven_not_seen_status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  proven_not_seen_status.latest_updated_at_ns += 10;
  proven_not_seen_status.remaining_duration_ns -= 10;
  ASSERT_TRUE(proven_not_seen_status.locally_valid());
  auto proven_not_seen = PreparedDisableEffect(first_request);
  proven_not_seen.state = CCLCEffectAttemptStateDTO::kCompleted;
  proven_not_seen.disable_handoff_result = DisableHandoffEffectResultDTO{
      first_request, DisableHandoffEffectDispositionDTO::kProvenNotSeen, std::nullopt};
  ASSERT_TRUE(journal.RecordEffectCompleted(owner, proven_not_seen, proven_not_seen_status).ok());

  const auto successor_request = DisableRequest(
      proven_not_seen_status, RequestOwnerEpoch("kernel-owner.2"), "disable-handoff.2");
  EXPECT_NE(first_request.owner_request_context.request_owner_epoch,
            successor_request.owner_request_context.request_owner_epoch);
  EXPECT_NE(first_request.owner_request_context.request_id,
            successor_request.owner_request_context.request_id);
  const auto successor_status = PreparedDisableStatus(proven_not_seen_status);
  const auto successor = journal.RecordEffectPrepared(
      owner, PreparedDisableEffect(successor_request, 2U), successor_status);
  ASSERT_TRUE(successor.ok()) << successor.error().message;
}

TEST(SqliteCCLCJournal, ProvenHeldFenceUsesAFreshKernelEpochSuccessorWithoutClaimingRelease) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto lifecycle_intent = DisableIntent("request.1", owner, 3U);
  const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  auto status = InitialDisableStatus(lifecycle_intent, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, acceptance, status).ok());
  const auto handoff_request = DisableRequest(status);
  const auto handoff_acceptance = DisableAcceptance(handoff_request);
  status = PreparedDisableStatus(status);
  ASSERT_TRUE(
      journal.RecordEffectPrepared(owner, PreparedDisableEffect(handoff_request), status).ok());
  status = CompletedDisableStatus(status, DisableReady(handoff_request, handoff_acceptance));
  ASSERT_TRUE(journal
                  .RecordEffectCompleted(
                      owner, CompletedDisableEffect(handoff_request, handoff_acceptance), status)
                  .ok());
  status = ResolvedDisableStatus(status);
  ASSERT_TRUE(status.domains.front().reconciliation_evidence->locally_valid());
  EXPECT_EQ(status.domains.front().requested_state.presence,
            ComponentLifecycleRequestedStatePresenceDTO::kResolved);
  ASSERT_TRUE(status.domains.front().requested_state.state.has_value());
  EXPECT_EQ(status.domains.front().current_state, *status.domains.front().requested_state.state);
  EXPECT_EQ(status.domains.front().reconciliation_evidence->observed_state,
            status.domains.front().current_state);
  EXPECT_EQ(status.domains.front().reconciliation_evidence->postcondition_result,
            LifecyclePostconditionResultDTO::kSatisfied);
  ASSERT_TRUE(status.domains.front().locally_valid());
  ASSERT_TRUE(status.disable_handoff->locally_valid());
  ASSERT_TRUE(status.configuration_fence->locally_valid());
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(journal.RecordLifecycleStatus(owner, status).ok());

  const auto first_release =
      DisableReleaseRequest(status, RequestOwnerEpoch("kernel-owner.1"), "disable-release.1");
  status = PreparedReleaseStatus(status);
  ASSERT_TRUE(
      journal.RecordEffectPrepared(owner, PreparedReleaseEffect(first_release), status).ok());
  const auto overlapping_release =
      DisableReleaseRequest(status, RequestOwnerEpoch("kernel-owner.2"), "disable-release.2");
  const auto overlapping_status = PreparedReleaseStatus(status);
  const auto overlapping = journal.RecordEffectPrepared(
      owner, PreparedReleaseEffect(overlapping_release, 3U), overlapping_status);
  ASSERT_FALSE(overlapping.ok());
  EXPECT_EQ(overlapping.error().code, "component_lifecycle_journal_attempt_unresolved");
  auto proven_held_status = status;
  proven_held_status.status_sequence += 1U;
  proven_held_status.latest_updated_at_ns += 10;
  proven_held_status.remaining_duration_ns -= 10;
  ASSERT_TRUE(proven_held_status.locally_valid());
  ASSERT_TRUE(
      journal
          .RecordEffectCompleted(
              owner,
              CompletedReleaseEffect(
                  first_release, DisableFenceReleaseEffectDispositionDTO::kProvenNotReleasedHeld,
                  std::nullopt),
              proven_held_status)
          .ok());
  EXPECT_EQ(proven_held_status.configuration_fence->state, ConfigurationFenceState::kHeld);
  EXPECT_FALSE(proven_held_status.disable_fence_released.has_value());

  const auto successor_release = DisableReleaseRequest(
      proven_held_status, RequestOwnerEpoch("kernel-owner.2"), "disable-release.2");
  EXPECT_NE(first_release.owner_request_context.request_owner_epoch,
            successor_release.owner_request_context.request_owner_epoch);
  const auto successor_status = PreparedReleaseStatus(proven_held_status);
  const auto successor = journal.RecordEffectPrepared(
      owner, PreparedReleaseEffect(successor_release, 3U), successor_status);
  ASSERT_TRUE(successor.ok()) << successor.error().message;
}

TEST(SqliteCCLCJournal, RejectsDomainEffectRegressionAndDroppedReconciliationEvidence) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto lifecycle_intent = Intent("request.1", owner, 3U);
  const auto acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  auto status = InitialStatus(lifecycle_intent, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, lifecycle_intent, acceptance, status).ok());

  status.status_sequence += 1U;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kReconciling;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kReconciling;
  status.domains.front().effect = ComponentLifecycleDomainEffectDTO::kEffectUncertain;
  status.domains.front().reconciliation_evidence =
      LifecycleReconciliationEvidenceDTO{ProviderInstanceId("provider.1"),
                                         ProviderManifestId("manifest.1"),
                                         ProviderManifestRevision("manifest-revision.1"),
                                         RuntimeUnitId("runtime.1"),
                                         RuntimeEpoch("runtime-epoch.1"),
                                         1U,
                                         1U,
                                         ComponentLifecycleStateDTO::kDeclared,
                                         LifecyclePostconditionResultDTO::kNotSatisfied,
                                         200};
  status.reconciliation_required = true;
  status.latest_updated_at_ns = 200;
  status.remaining_duration_ns -= 100;
  ASSERT_TRUE(status.locally_valid());
  ASSERT_TRUE(journal.RecordLifecycleStatus(owner, status).ok());

  auto changed_provider = status;
  changed_provider.status_sequence += 1U;
  changed_provider.domains.front().resolved_provider_occurrence->lifecycle_plan_catalog_digest =
      ContentDigest("different-catalog-digest");
  changed_provider.latest_updated_at_ns += 10;
  changed_provider.remaining_duration_ns -= 10;
  ASSERT_TRUE(changed_provider.locally_valid());
  const auto changed_provider_result = journal.RecordLifecycleStatus(owner, changed_provider);
  ASSERT_FALSE(changed_provider_result.ok());
  EXPECT_EQ(changed_provider_result.error().code, "component_lifecycle_journal_status_regressed");

  auto dropped = status;
  dropped.status_sequence += 1U;
  dropped.phase = ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown;
  dropped.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown;
  dropped.domains.front().reconciliation_evidence.reset();
  const ErrorDTO unresolved{"reconciliation_evidence_lost", "fresh evidence is required", {}};
  dropped.domains.front().error = unresolved;
  dropped.error = unresolved;
  dropped.latest_updated_at_ns += 10;
  ASSERT_TRUE(dropped.locally_valid());
  const auto drop_result = journal.RecordLifecycleStatus(owner, dropped);
  ASSERT_FALSE(drop_result.ok());
  EXPECT_EQ(drop_result.error().code, "component_lifecycle_journal_status_regressed");

  auto unresolved_without_fresh_evidence = status;
  unresolved_without_fresh_evidence.status_sequence += 1U;
  unresolved_without_fresh_evidence.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  unresolved_without_fresh_evidence.domains.front().action_state =
      ComponentLifecycleDomainActionStateDTO::kVerifying;
  unresolved_without_fresh_evidence.domains.front().effect =
      ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  unresolved_without_fresh_evidence.reconciliation_required = false;
  unresolved_without_fresh_evidence.latest_updated_at_ns += 10;
  ASSERT_TRUE(unresolved_without_fresh_evidence.locally_valid());
  const auto unresolved_result =
      journal.RecordLifecycleStatus(owner, unresolved_without_fresh_evidence);
  ASSERT_FALSE(unresolved_result.ok());
  EXPECT_EQ(unresolved_result.error().code, "component_lifecycle_journal_status_regressed");

  auto same_generation_mutation = unresolved_without_fresh_evidence;
  same_generation_mutation.domains.front().reconciliation_evidence->observed_state =
      ComponentLifecycleStateDTO::kPrepared;
  same_generation_mutation.domains.front().reconciliation_evidence->postcondition_result =
      LifecyclePostconditionResultDTO::kSatisfied;
  ASSERT_TRUE(same_generation_mutation.locally_valid());
  const auto semantic_mutation = journal.RecordLifecycleStatus(owner, same_generation_mutation);
  ASSERT_FALSE(semantic_mutation.ok());
  EXPECT_EQ(semantic_mutation.error().code, "component_lifecycle_journal_status_regressed");

  auto timestamp_only = unresolved_without_fresh_evidence;
  timestamp_only.domains.front().reconciliation_evidence->source_sequence += 1U;
  timestamp_only.domains.front().reconciliation_evidence->observed_at_ns += 10;
  ASSERT_TRUE(timestamp_only.locally_valid());
  const auto timestamp_result = journal.RecordLifecycleStatus(owner, timestamp_only);
  ASSERT_FALSE(timestamp_result.ok());
  EXPECT_EQ(timestamp_result.error().code, "component_lifecycle_journal_status_regressed");
}

TEST(SqliteCCLCJournal, ExactReplayPrecedesTheBoundedActiveTransactionCapacityCheck) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 128U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 128U).ok());

  std::optional<ComponentLifecycleIntentDTO> first_intent;
  std::optional<ComponentLifecycleAcceptedDTO> first_acceptance;
  for (std::size_t index = 0U; index < kMaximumCCLCActiveTransactions; ++index) {
    const auto lifecycle_intent =
        Intent("request." + std::to_string(index + 1U), owner, 3U + index);
    const auto acceptance =
        Acceptance(lifecycle_intent, index + 1U, "lifecycle." + std::to_string(index + 1U));
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, lifecycle_intent, acceptance).ok());
    const auto accepted = journal.AcceptOrReplayLifecycle(
        owner, lifecycle_intent, acceptance, InitialStatus(lifecycle_intent, acceptance));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
    if (index == 0U) {
      first_intent = lifecycle_intent;
      first_acceptance = acceptance;
    }
  }

  const auto overflow_intent =
      Intent("request.overflow", owner, 3U + kMaximumCCLCActiveTransactions);
  const auto overflow_acceptance =
      Acceptance(overflow_intent, kMaximumCCLCActiveTransactions + 1U, "lifecycle.overflow");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, overflow_intent, overflow_acceptance).ok());
  const auto overflow =
      journal.AcceptOrReplayLifecycle(owner, overflow_intent, overflow_acceptance,
                                      InitialStatus(overflow_intent, overflow_acceptance));
  ASSERT_FALSE(overflow.ok());
  EXPECT_EQ(overflow.error().code, "request_queue_full");

  ASSERT_TRUE(first_intent.has_value());
  ASSERT_TRUE(first_acceptance.has_value());
  const auto replay = journal.AcceptOrReplayLifecycle(
      owner, *first_intent, *first_acceptance, InitialStatus(*first_intent, *first_acceptance));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  ASSERT_TRUE(replay.value().acceptance.has_value());
  EXPECT_EQ(replay.value().acceptance->lifecycle_transaction_id,
            first_acceptance->lifecycle_transaction_id);
}

TEST(SqliteCCLCJournal, CompactsTerminalResultToNonExecutingMembershipTombstone) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto first = Intent("request.1", owner, 3U);
  const auto first_acceptance = Acceptance(first, 1U, "lifecycle.1");
  auto first_status = InitialStatus(first, first_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, first, first_acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, first, first_acceptance, first_status).ok());
  first_status = TerminalSuccess(first_status, 2U);
  ASSERT_TRUE(RecordSettledTerminal(journal, owner, first_status).ok());

  const auto second = Intent("request.2", owner, 4U);
  const auto second_acceptance = Acceptance(second, 2U, "lifecycle.2");
  auto second_status = InitialStatus(second, second_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, second, second_acceptance).ok());
  ASSERT_TRUE(
      journal.AcceptOrReplayLifecycle(owner, second, second_acceptance, second_status).ok());
  second_status = TerminalSuccess(second_status, 2U);
  ASSERT_TRUE(RecordSettledTerminal(journal, owner, second_status).ok());

  const auto expired = journal.AcceptOrReplayLifecycle(
      owner, first,
      [&] {
        auto unusable = first_acceptance;
        unusable.replay_status = RequestReplayStatusDTO::kRequestIdConflict;
        return unusable;
      }(),
      InitialStatus(first, first_acceptance));
  ASSERT_TRUE(expired.ok());
  EXPECT_EQ(expired.value().replay_status, RequestReplayStatusDTO::kRequestResultExpired);
  const auto retained =
      journal.LoadRetainedLifecycleStatus(second_acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value().has_value());
  EXPECT_EQ(retained.value()->terminal_result, ComponentLifecycleTerminalResultDTO::kSucceeded);
}

TEST(SqliteCCLCJournal, ReplacementRecoversZeroAndCompletedEffectSequenceWatermarks) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch first_owner("owner.1");
  {
    SqliteCCLCJournal first(path, CellId("cell.1"), 8U);
    ASSERT_TRUE(BeginTestOwnerEpoch(first, first_owner, 1, 8U).ok());

    const auto without_effect = Intent("request.1", first_owner, 3U);
    const auto without_effect_acceptance = Acceptance(without_effect, 1U, "lifecycle.1");
    ASSERT_TRUE(
        SeedLifecycleClaim(first, first_owner, without_effect, without_effect_acceptance).ok());
    ASSERT_TRUE(
        first
            .AcceptOrReplayLifecycle(first_owner, without_effect, without_effect_acceptance,
                                     InitialStatus(without_effect, without_effect_acceptance))
            .ok());

    const auto with_effect = Intent("request.2", first_owner, 4U);
    const auto with_effect_acceptance = Acceptance(with_effect, 2U, "lifecycle.2");
    auto status = InitialStatus(with_effect, with_effect_acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(first, first_owner, with_effect, with_effect_acceptance).ok());
    ASSERT_TRUE(
        first.AcceptOrReplayLifecycle(first_owner, with_effect, with_effect_acceptance, status)
            .ok());
    const auto primitive = PrimitiveIntent(status);
    status = PreparedEffectStatus(status);
    ASSERT_TRUE(
        first.RecordEffectPrepared(first_owner, PreparedPrimitiveEffect(primitive), status).ok());
    const auto result = PrimitiveSuccess(primitive);
    status = CompletedEffectStatus(status, result);
    ASSERT_TRUE(
        first
            .RecordEffectCompleted(first_owner, CompletedPrimitiveEffect(primitive, result), status)
            .ok());
  }

  SqliteCCLCJournal replacement(path, CellId("cell.1"), 8U);
  const auto recovery = replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.2"), 500, 8U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 2U);
  ASSERT_EQ(recovery.value().effect_sequence_watermarks.size(), 2U);
  EXPECT_EQ(recovery.value().effect_sequence_watermarks[0].highest_effect_sequence, 0U);
  EXPECT_EQ(recovery.value().effect_sequence_watermarks[1].highest_effect_sequence, 1U);
  EXPECT_TRUE(recovery.value().unresolved_effect_attempts.empty());
}

TEST(SqliteCCLCJournal, ReplacementTerminalizesNoPreparedEffectAsKnownNoEffect) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch first_owner("owner.1");
  SqliteCCLCJournal first(path, CellId("cell.1"), 8U);
  ASSERT_TRUE(BeginTestOwnerEpoch(first, first_owner, 1, 8U).ok());
  const auto intent = Intent("request.1", first_owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  {
    ASSERT_TRUE(SeedLifecycleClaim(first, first_owner, intent, acceptance).ok());
    ASSERT_TRUE(first
                    .AcceptOrReplayLifecycle(first_owner, intent, acceptance,
                                             InitialStatus(intent, acceptance))
                    .ok());
  }
  first.Close();
  SqliteCCLCJournal replacement(path, CellId("cell.1"), 8U);
  RequestOwnerEpoch replacement_owner("owner.2");
  const auto recovery = BeginTestOwnerEpoch(replacement, replacement_owner, 500, 8U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 1U);
  ASSERT_TRUE(recovery.value().unresolved_effect_attempts.empty());

  auto manufactured_success =
      TerminalSuccess(recovery.value().interrupted_transactions.front(),
                      recovery.value().interrupted_transactions.front().status_sequence + 1U);
  manufactured_success.status_owner_epoch = replacement_owner;
  manufactured_success.latest_updated_at_ns = 500;
  ASSERT_TRUE(manufactured_success.locally_valid());
  const auto unsupported =
      replacement.RecordLifecycleStatus(replacement_owner, manufactured_success);
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().code, "component_lifecycle_journal_status_regressed");

  const auto terminal = ReplacementTerminalKnownNoEffect(
      recovery.value().interrupted_transactions.front(), replacement_owner);
  ASSERT_TRUE(terminal.locally_valid());
  const auto recorded = replacement.RecordLifecycleStatus(replacement_owner, terminal);
  ASSERT_TRUE(recorded.ok()) << recorded.error().code << ": " << recorded.error().message;

  const auto retained =
      replacement.LoadRetainedLifecycleStatus(acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value().has_value());
  EXPECT_EQ(*retained.value(), terminal);
}

TEST(SqliteCCLCJournal, ReplacementCannotTerminalizeUnresolvedPreparedEffect) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch first_owner("owner.1");
  SqliteCCLCJournal first(path, CellId("cell.1"), 8U);
  ASSERT_TRUE(BeginTestOwnerEpoch(first, first_owner, 1, 8U).ok());
  const auto intent = Intent("request.1", first_owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  {
    auto status = InitialStatus(intent, acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(first, first_owner, intent, acceptance).ok());
    ASSERT_TRUE(first.AcceptOrReplayLifecycle(first_owner, intent, acceptance, status).ok());
    const auto primitive = PrimitiveIntent(status);
    status = PreparedEffectStatus(status);
    ASSERT_TRUE(
        first.RecordEffectPrepared(first_owner, PreparedPrimitiveEffect(primitive), status).ok());
  }
  first.Close();
  SqliteCCLCJournal replacement(path, CellId("cell.1"), 8U);
  RequestOwnerEpoch replacement_owner("owner.2");
  const auto recovery = BeginTestOwnerEpoch(replacement, replacement_owner, 500, 8U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 1U);
  ASSERT_EQ(recovery.value().unresolved_effect_attempts.size(), 1U);

  const auto false_terminal = ReplacementTerminalKnownNoEffect(
      recovery.value().interrupted_transactions.front(), replacement_owner);
  ASSERT_TRUE(false_terminal.locally_valid());
  const auto rejected = replacement.RecordLifecycleStatus(replacement_owner, false_terminal);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "component_lifecycle_journal_status_regressed");
}

TEST(SqliteCCLCJournal, EffectIdentityIsScopedByTransactionAcrossOwnerEpochs) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch first_owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, first_owner, 1, 8U).ok());

  const auto first = Intent("request.shared", first_owner, 3U);
  const auto first_acceptance = Acceptance(first, 1U, "lifecycle.1");
  auto first_status = InitialStatus(first, first_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, first_owner, first, first_acceptance).ok());
  ASSERT_TRUE(
      journal.AcceptOrReplayLifecycle(first_owner, first, first_acceptance, first_status).ok());
  const auto first_primitive = PrimitiveIntent(first_status, "attempt.shared");
  first_status = PreparedEffectStatus(first_status);
  ASSERT_TRUE(
      journal
          .RecordEffectPrepared(first_owner, PreparedPrimitiveEffect(first_primitive), first_status)
          .ok());
  const auto first_result = PrimitiveSuccess(first_primitive);
  first_status = CompletedEffectStatus(first_status, first_result);
  ASSERT_TRUE(journal
                  .RecordEffectCompleted(first_owner,
                                         CompletedPrimitiveEffect(first_primitive, first_result),
                                         first_status)
                  .ok());
  ASSERT_TRUE(journal.RecordLifecycleStatus(first_owner, TerminalSuccess(first_status, 4U)).ok());

  const auto keeps_namespace = Intent("request.pending", first_owner, 4U);
  const auto keeps_namespace_acceptance = Acceptance(keeps_namespace, 2U, "lifecycle.2");
  ASSERT_TRUE(
      SeedLifecycleClaim(journal, first_owner, keeps_namespace, keeps_namespace_acceptance).ok());
  ASSERT_TRUE(
      journal
          .AcceptOrReplayLifecycle(first_owner, keeps_namespace, keeps_namespace_acceptance,
                                   InitialStatus(keeps_namespace, keeps_namespace_acceptance))
          .ok());

  RequestOwnerEpoch second_owner("owner.2");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, second_owner, 500, 8U).ok());
  const auto second = Intent("request.shared", second_owner, 5U);
  const auto second_acceptance = Acceptance(second, 3U, "lifecycle.3");
  auto second_status = InitialStatus(second, second_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, second_owner, second, second_acceptance).ok());
  ASSERT_TRUE(
      journal.AcceptOrReplayLifecycle(second_owner, second, second_acceptance, second_status).ok());
  const auto second_primitive = PrimitiveIntent(second_status, "attempt.shared");
  second_status = PreparedEffectStatus(second_status);
  const auto prepared = journal.RecordEffectPrepared(
      second_owner, PreparedPrimitiveEffect(second_primitive), second_status);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
}

TEST(SqliteCCLCJournal, CompactsByDurableTerminalOrderNotAcceptanceOrder) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());

  const auto first = Intent("request.1", owner, 3U);
  const auto first_acceptance = Acceptance(first, 1U, "lifecycle.1");
  auto first_status = InitialStatus(first, first_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, first, first_acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, first, first_acceptance, first_status).ok());
  const auto second = Intent("request.2", owner, 4U);
  const auto second_acceptance = Acceptance(second, 2U, "lifecycle.2");
  auto second_status = InitialStatus(second, second_acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, second, second_acceptance).ok());
  ASSERT_TRUE(
      journal.AcceptOrReplayLifecycle(owner, second, second_acceptance, second_status).ok());

  ASSERT_TRUE(RecordSettledTerminal(journal, owner, TerminalSuccess(second_status, 2U)).ok());
  ASSERT_TRUE(RecordSettledTerminal(journal, owner, TerminalSuccess(first_status, 2U)).ok());

  const auto first_retained =
      journal.LoadRetainedLifecycleStatus(first_acceptance.lifecycle_transaction_id);
  const auto second_retained =
      journal.LoadRetainedLifecycleStatus(second_acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(first_retained.ok());
  ASSERT_TRUE(second_retained.ok());
  EXPECT_TRUE(first_retained.value().has_value());
  EXPECT_FALSE(second_retained.value().has_value());
}

TEST(SqliteCCLCJournal, CompactsTerminalResultAtConfiguredBytePressure) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U, 1U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto intent = Intent("request.1", owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  auto status = InitialStatus(intent, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status).ok());
  ASSERT_TRUE(RecordSettledTerminal(journal, owner, TerminalSuccess(status, 2U)).ok());
  const auto retained = journal.LoadRetainedLifecycleStatus(acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  EXPECT_FALSE(retained.value().has_value());
  const auto replay =
      journal.AcceptOrReplayLifecycle(owner, intent, acceptance, InitialStatus(intent, acceptance));
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kRequestResultExpired);
}

TEST(SqliteCCLCJournal, RejectsSemanticColumnCorruptionOnReopen) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    RequestOwnerEpoch owner("owner.1");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
    const auto intent = Intent("request.1", owner, 3U);
    const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    ASSERT_TRUE(
        journal
            .AcceptOrReplayLifecycle(owner, intent, acceptance, InitialStatus(intent, acceptance))
            .ok());
    journal.Close();
  }

  sqlite3* database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr),
            SQLITE_OK);
  ASSERT_NE(database, nullptr);
  ASSERT_EQ(sqlite3_exec(database,
                         "UPDATE lifecycle_transactions SET terminal=1,"
                         "terminal_sequence=1",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  EXPECT_THROW((SqliteCCLCJournal(path, CellId("cell.1"), 8U)), std::runtime_error);
}

TEST(SqliteCCLCJournal, RejectsStaleCurrentComponentFactOnReopen) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    RequestOwnerEpoch owner("owner.1");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
    const auto intent = Intent("request.1", owner, 3U);
    const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    ASSERT_TRUE(
        journal
            .AcceptOrReplayLifecycle(owner, intent, acceptance, InitialStatus(intent, acceptance))
            .ok());
    journal.Close();
  }

  sqlite3* database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr),
            SQLITE_OK);
  ASSERT_NE(database, nullptr);
  ASSERT_EQ(sqlite3_exec(database,
                         "UPDATE lifecycle_component_current_facts SET "
                         "component_lifecycle_generation=3",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  EXPECT_THROW((SqliteCCLCJournal(path, CellId("cell.1"), 8U)), std::runtime_error);
}

TEST(SqliteCCLCJournal, RejectsStatusThatDropsCompletedEffectEvidence) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  std::optional<ComponentLifecycleStatusDTO> prepared_status;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    RequestOwnerEpoch owner("owner.1");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
    const auto intent = Intent("request.1", owner, 3U);
    const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
    auto status = InitialStatus(intent, acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status).ok());
    const auto primitive = PrimitiveIntent(status);
    prepared_status = PreparedEffectStatus(status);
    ASSERT_TRUE(
        journal.RecordEffectPrepared(owner, PreparedPrimitiveEffect(primitive), *prepared_status)
            .ok());
    const auto result = PrimitiveSuccess(primitive);
    status = CompletedEffectStatus(*prepared_status, result);
    ASSERT_TRUE(
        journal.RecordEffectCompleted(owner, CompletedPrimitiveEffect(primitive, result), status)
            .ok());
    journal.Close();
  }

  ASSERT_TRUE(prepared_status.has_value());
  const auto prepared_blob = detail::EncodeComponentLifecycleStatus(*prepared_status);
  sqlite3* database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READWRITE, nullptr),
            SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(database,
                               "UPDATE lifecycle_transactions SET status_sequence=?,"
                               "latest_updated_at_ns=?,status_blob=?",
                               -1, &statement, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_bind_int64(statement, 1,
                               static_cast<sqlite3_int64>(prepared_status->status_sequence)),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_bind_int64(statement, 2, prepared_status->latest_updated_at_ns), SQLITE_OK);
  ASSERT_EQ(sqlite3_bind_blob64(statement, 3, prepared_blob.data(), prepared_blob.size(),
                                SQLITE_TRANSIENT),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
  ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);

  EXPECT_THROW((SqliteCCLCJournal(path, CellId("cell.1"), 8U)), std::runtime_error);
}

TEST(SqliteCCLCJournal, RejectsASymlinkedDatabaseAncestor) {
  ScopedTemporaryDirectory temporary;
  const auto real_parent = temporary.path() / "real" / "nested";
  ASSERT_TRUE(std::filesystem::create_directories(real_parent));
  const auto linked_parent = temporary.path() / "linked";
  ASSERT_NO_THROW(
      std::filesystem::create_directory_symlink(temporary.path() / "real", linked_parent));
  EXPECT_THROW((SqliteCCLCJournal(linked_parent / "nested" / std::string(kCCLCJournalFilename),
                                  CellId("cell.1"), 8U)),
               std::runtime_error);
}

TEST(SqliteCCLCJournal, RejectsIncoherentOwnerAndResultCapacities) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  const auto result = journal.BeginOwnerEpoch(RequestOwnerEpoch("owner.1"), 1, 7U);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "component_lifecycle_journal_owner_epoch_invalid");
}

TEST(SqliteCCLCJournal, QuiescentRotationKeepsRetainedResultAndEpochReuseGuard) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch first_owner("owner.1");
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, first_owner, 1, 8U).ok());
    const auto first = Intent("request.shared", first_owner, 3U);
    const auto first_acceptance = Acceptance(first, 1U, "lifecycle.1");
    auto first_status = InitialStatus(first, first_acceptance);
    const auto first_claim =
        SeedLifecycleClaim(journal, first_owner, first, first_acceptance);
    ASSERT_TRUE(first_claim.ok());
    ASSERT_TRUE(
        journal.AcceptOrReplayLifecycle(first_owner, first, first_acceptance, first_status).ok());
    const CellAdmissionClaimReleaseDTO release{
        first_claim.value().intent.owner_role,
        first_claim.value().intent.owner_epoch,
        first_claim.value().intent.owner_transaction_id,
        first_claim.value().intent.request_id,
        first_claim.value().intent.request_payload_digest,
        first_claim.value().acquired_admission_generation,
        first_claim.value().claim_generation,
        {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
         ContentDigest(detail::ComponentLifecycleStatusReleasePreconditionDigest(
             TerminalSuccess(first_status, 2U)))}};
    ASSERT_TRUE(journal
                    .RecordLifecycleTerminalAndClaimRelease(
                        first_owner, TerminalSuccess(first_status, 2U), release)
                    .ok());
    const CellAdmissionRecoveryReceiptDTO receipt{
        CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact,
        RequestOwnerEpoch("kernel.1"),
        AdmissionGeneration(2U),
        first_claim.value(),
        release,
        std::nullopt,
        {},
        {}};
    ASSERT_TRUE(journal.RecordCellClaimReleaseReceipt(first_owner, release, receipt).ok());

    RequestOwnerEpoch second_owner("owner.2");
    const auto recovery = BeginTestOwnerEpoch(journal, second_owner, 500, 8U);
    ASSERT_TRUE(recovery.ok());
    EXPECT_TRUE(recovery.value().interrupted_transactions.empty());
    const auto original_result = journal.ReadLifecycleRequestResult(first);
    ASSERT_TRUE(original_result.ok());
    ASSERT_TRUE(original_result.value());
    ASSERT_TRUE(original_result.value()->status);
    EXPECT_EQ(original_result.value()->status->terminal_result,
              ComponentLifecycleTerminalResultDTO::kSucceeded);
    const auto second = Intent("request.shared", second_owner, 4U);
    const auto reused_identity = Acceptance(second, 2U, "lifecycle.1");
    ASSERT_TRUE(SeedLifecycleClaim(journal, second_owner, second, reused_identity).ok());
    const auto rejected = journal.AcceptOrReplayLifecycle(second_owner, second, reused_identity,
                                                          InitialStatus(second, reused_identity));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "component_lifecycle_journal_transaction_identity_reused");
    const auto fresh = Intent("request.fresh", second_owner, 4U);
    const auto second_acceptance = Acceptance(fresh, 2U, "lifecycle.2");
    ASSERT_TRUE(SeedLifecycleClaim(journal, second_owner, fresh, second_acceptance).ok());
    ASSERT_TRUE(journal
                    .AcceptOrReplayLifecycle(second_owner, fresh, second_acceptance,
                                             InitialStatus(fresh, second_acceptance))
                    .ok());
    journal.Close();
  }

  sqlite3* database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr),
            SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(database,
                               "SELECT (SELECT count(*) FROM cclc_request_memberships "
                               "WHERE owner_epoch=?1),"
                               "(SELECT count(*) FROM cclc_owner_epochs "
                               "WHERE owner_epoch=?1)",
                               -1, &statement, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_bind_text(statement, 1, first_owner.value().c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int64(statement, 0), 1);
  EXPECT_EQ(sqlite3_column_int64(statement, 1), 1);
  EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
  ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);

  SqliteCCLCJournal reopened(path, CellId("cell.1"), 8U);
  const auto prior_epoch = first_owner;
  const auto reused = BeginTestOwnerEpoch(reopened, first_owner, 1'000, 8U);
  ASSERT_TRUE(reused.ok());
  EXPECT_NE(first_owner, prior_epoch);  // Even reuse of the seed cannot reissue the namespace.
}

TEST(SqliteCCLCJournal, BoundsTheDurableComponentWatermarkSet) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 128U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 128U).ok());
  for (std::size_t index = 0U; index < kMaximumCCLCComponentWatermarks; ++index) {
    auto intent = Intent("request." + std::to_string(index + 1U), owner, 3U);
    intent.controllable_component_id =
        ControllableComponentId("component." + std::to_string(index + 1U));
    const auto acceptance =
        Acceptance(intent, index + 1U, "lifecycle." + std::to_string(index + 1U));
    auto status = InitialStatus(intent, acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status).ok());
    ASSERT_TRUE(RecordSettledTerminal(journal, owner, TerminalSuccess(status, 2U)).ok());
  }

  auto overflow = Intent("request.overflow", owner, 3U);
  overflow.controllable_component_id = ControllableComponentId("component.overflow");
  const auto overflow_acceptance =
      Acceptance(overflow, kMaximumCCLCComponentWatermarks + 1U, "lifecycle.overflow");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, overflow, overflow_acceptance).ok());
  const auto result = journal.AcceptOrReplayLifecycle(owner, overflow, overflow_acceptance,
                                                      InitialStatus(overflow, overflow_acceptance));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "component_lifecycle_journal_capacity_exhausted");
}

TEST(SqliteCCLCJournal, NeverReusesOwnerEpochOrChangesJournalIdentity) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
    const auto prior_epoch = owner;
    const auto reused = BeginTestOwnerEpoch(journal, owner, 2, 8U);
    ASSERT_TRUE(reused.ok());
    EXPECT_NE(owner, prior_epoch);
  }
  EXPECT_THROW((SqliteCCLCJournal(path, CellId("other-cell"), 8U)), std::runtime_error);
  EXPECT_THROW((SqliteCCLCJournal(path, CellId("cell.1"), 9U)), std::runtime_error);
}

TEST(SqliteCCLCJournal,
     ReadOnlyReconciliationAcceptsExactEmptyLifecycleOwnerRotationAndReplay) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto evidence =
      CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner);
  ASSERT_TRUE(evidence.ok());
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> predecessor_owners{
      {CellAdmissionOwnerRoleDTO::kProgramRunner,
       RequestOwnerEpoch("program.owner.1")}};
  const CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.1"),
      {},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner,
        RequestOwnerEpoch("program.owner.1")},
       {CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner}},
      evidence.value()};

  const auto first = ValidateCCLCCellAdmissionReconciliationReadOnly(path, CellId("cell.1"), {},
                                                                     predecessor_owners, reconcile);
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first.value().current_owner_epochs,
            reconcile.current_owner_epochs);
  EXPECT_TRUE(first.value().settlements.empty());
  const auto replay = ValidateCCLCCellAdmissionReconciliationReadOnly(
      path, CellId("cell.1"), {}, reconcile.current_owner_epochs, reconcile);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value(), first.value());
  auto foreign_membership = reconcile;
  foreign_membership.current_owner_epochs.front().owner_epoch =
      RequestOwnerEpoch("program.owner.2");
  EXPECT_FALSE(ValidateCCLCCellAdmissionReconciliationReadOnly(
                   path, CellId("cell.1"), {}, predecessor_owners, foreign_membership)
                   .ok());
}

TEST(SqliteCCLCJournal,
     ReadOnlyReconciliationRejectsMissingCorruptAndChangedEvidence) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.1");
  SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  const auto evidence =
      CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner);
  ASSERT_TRUE(evidence.ok());
  CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.1"),
      {},
      {{CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner}},
      evidence.value()};

  EXPECT_FALSE(ValidateCCLCCellAdmissionReconciliationReadOnly(temporary.path() / "missing.sqlite3",
                                                               CellId("cell.1"), {}, {}, reconcile)
                   .ok());
  auto changed = reconcile;
  changed.reconciliation_evidence_digest =
      ContentDigest(
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  EXPECT_FALSE(
      ValidateCCLCCellAdmissionReconciliationReadOnly(path, CellId("cell.1"), {}, {}, changed)
          .ok());
  EXPECT_FALSE(
      ValidateCCLCCellAdmissionReconciliationReadOnly(path, CellId("other-cell"), {}, {}, reconcile)
          .ok());
}

TEST(SqliteCCLCJournal,
     ReadOnlyReconciliationRejectsHeldLifecycleClaimWithUnresolvedEffect) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch predecessor("owner.1");
  std::optional<CellAdmissionClaimDTO> claim;
  {
    SqliteCCLCJournal first(path, CellId("cell.1"), 8U);
    ASSERT_TRUE(BeginTestOwnerEpoch(first, predecessor, 1, 8U).ok());
    const auto intent = Intent("request.1", predecessor, 3U);
    const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
    auto status = InitialStatus(intent, acceptance);
    const auto seeded =
        SeedLifecycleClaim(first, predecessor, intent, acceptance);
    ASSERT_TRUE(seeded.ok());
    claim = seeded.value();
    ASSERT_TRUE(
        first.AcceptOrReplayLifecycle(predecessor, intent, acceptance, status)
            .ok());
    const auto primitive = PrimitiveIntent(status);
    status = PreparedEffectStatus(status);
    ASSERT_TRUE(first
                    .RecordEffectPrepared(
                        predecessor, PreparedPrimitiveEffect(primitive), status)
                    .ok());
  }
  RequestOwnerEpoch successor("owner.2");
  SqliteCCLCJournal replacement(path, CellId("cell.1"), 8U);
  ASSERT_TRUE(BeginTestOwnerEpoch(replacement, successor, 500, 8U).ok());
  const auto evidence =
      CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, successor);
  ASSERT_TRUE(evidence.ok());
  const CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.1"),
      {*claim},
      {{CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, successor}},
      evidence.value()};
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> current_owners{
      {CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, predecessor}};
  EXPECT_FALSE(ValidateCCLCCellAdmissionReconciliationReadOnly(path, CellId("cell.1"), {*claim},
                                                               current_owners, reconcile)
                   .ok());
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO NativeApplyPublication(
    const std::string& apply_id, std::uint64_t apply_generation,
    const std::string& bundle, std::uint64_t publication_generation) {
  const ProjectionRevisionDTO projection{
      RetainedBundleId(bundle), GraphRevision("graph.1"),
      {SceneId(Sha('a')), 1U, ContentDigest(Sha('b'))}};
  return {{NamedApplyPublicationTransactionDTO{ApplyId(apply_id),
                                               ApplyGeneration(apply_generation)},
           projection, projection.retained_bundle_id, projection.graph_revision,
           ActiveBundlePublicationGeneration(publication_generation - 1U),
           ContentDigest(Sha('c'))},
          ActiveBundlePublicationGeneration(publication_generation), ContentDigest(Sha('d'))};
}

[[nodiscard]] NativeApplyPlanRequestDTO NativeApplyPlanRequest(
    const std::string& apply_id, std::uint64_t apply_generation, const std::string& bundle,
    const ActiveControlConfigurationDTO& route) {
  return {{ApplyId(apply_id), ApplyGeneration(apply_generation)},
          RetainedBundleId(bundle),
          GraphRevision("graph.1"),
          {ControllableComponentId("component.1")},
          {{route.control_domain_id, route.active_control_configuration_generation}},
          {},
          {},
          {},
          {}};
}

[[nodiscard]] NativeApplyPlanRequestDTO NativeApplyPlanRequestWithDelta(
    const std::string& apply_id, const std::string& bundle,
    std::vector<ControllableComponentId> components,
    std::vector<NativeApplyRouteGenerationDTO> compiled_routes,
    std::vector<ControllableComponentId> retained_components,
    std::vector<ControllableComponentId> removed_components,
    std::vector<ActiveControlConfigurationDTO> retained_routes,
    std::vector<ControlDomainId> removed_domains) {
  return {{ApplyId(apply_id), ApplyGeneration(1U)},
          RetainedBundleId(bundle),
          GraphRevision("graph.1"),
          std::move(components),
          std::move(compiled_routes),
          std::move(retained_components),
          std::move(removed_components),
          std::move(retained_routes),
          std::move(removed_domains)};
}

[[nodiscard]] ActiveControlConfigurationDTO SecondDomainRoute() {
  auto expected = ExpectedDomain();
  expected.control_domain_id = ControlDomainId("domain.2");
  expected.expected_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  auto route = ActiveConfiguration(expected);
  route.active_control_configuration_id = ActiveControlConfigurationId("configuration.2");
  return route;
}

TEST(SqliteCCLCJournal, NativeApplyRetainsExactFactsAndNeverRevivesARemovedGeneration) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto first_route = ActiveConfiguration(ExpectedDomain());
  const auto second_route = SecondDomainRoute();
  const ControllableComponentId first("component.1");
  const ControllableComponentId second("component.2");

  const auto installed =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", first_route));
  ASSERT_TRUE(installed.ok());
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                          installed.value().components,
                                          {first_route}})
                  .ok());

  // A retains its runtime; C is added. A keeps its exact fact and route.
  const auto add_request = NativeApplyPlanRequestWithDelta(
      "apply.2", "bundle.b", {first, second},
      {{first_route.control_domain_id,
        first_route.active_control_configuration_generation},
       {second_route.control_domain_id,
        second_route.active_control_configuration_generation}},
      {first}, {}, {first_route}, {});
  const auto added = journal.PlanNativeApply(add_request);
  ASSERT_TRUE(added.ok()) << added.error().message;
  ASSERT_EQ(added.value().components.size(), 2U);
  EXPECT_EQ(added.value().components[0].generation, ComponentLifecycleGeneration(1U));
  EXPECT_EQ(added.value().components[1].generation, ComponentLifecycleGeneration(1U));
  EXPECT_EQ(added.value().retained_components, std::vector<ControllableComponentId>{first});
  EXPECT_EQ(added.value().routes[0].generation, first_route.active_control_configuration_generation);
  EXPECT_EQ(added.value().retained_domains,
            std::vector<ControlDomainId>{first_route.control_domain_id});
  // Replay must name the same retained scope, not merely the same candidate.
  auto conflicting_replay = add_request;
  conflicting_replay.retained_components.clear();
  conflicting_replay.retained_routes.clear();
  const auto conflict = journal.PlanNativeApply(conflicting_replay);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "component_lifecycle_native_apply_conflict");
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.2", 1U, "bundle.b", 3U),
                                          added.value().components,
                                          {first_route, second_route}})
                  .ok());
  const auto retained_fact = journal.LoadCurrentComponentLifecycleFact(first);
  ASSERT_TRUE(retained_fact.ok());
  EXPECT_EQ(retained_fact.value().durable_generation, ComponentLifecycleGeneration(1U));
  EXPECT_EQ(retained_fact.value().current_state, ComponentLifecycleStateDTO::kActive);

  // A is removed while C is retained: A's current facts go, its history stays.
  const auto remove_request = NativeApplyPlanRequestWithDelta(
      "apply.3", "bundle.c", {second},
      {{second_route.control_domain_id,
        second_route.active_control_configuration_generation}},
      {second}, {first}, {second_route}, {first_route.control_domain_id});
  const auto removed = journal.PlanNativeApply(remove_request);
  ASSERT_TRUE(removed.ok()) << removed.error().message;
  EXPECT_EQ(removed.value().removed_components, std::vector<ControllableComponentId>{first});
  EXPECT_EQ(removed.value().removed_domains,
            std::vector<ControlDomainId>{first_route.control_domain_id});
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.3", 1U, "bundle.c", 4U),
                                          removed.value().components,
                                          {second_route}})
                  .ok());
  const auto removed_fact = journal.LoadCurrentComponentLifecycleFact(first);
  ASSERT_TRUE(removed_fact.ok());
  EXPECT_FALSE(removed_fact.value().current_state.has_value());
  const auto removed_route = journal.LoadCurrentActiveControlConfiguration(first_route.control_domain_id);
  ASSERT_TRUE(removed_route.ok());
  EXPECT_FALSE(removed_route.value().has_value());

  // Re-adding A mints newer generations than any it held before removal.
  const auto readded =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.4", 1U, "bundle.a", first_route));
  ASSERT_TRUE(readded.ok()) << readded.error().message;
  ASSERT_EQ(readded.value().components.size(), 1U);
  EXPECT_EQ(readded.value().components.front().generation, ComponentLifecycleGeneration(2U));
  EXPECT_EQ(readded.value().routes.front().generation.value(),
            first_route.active_control_configuration_generation.value() + 1U);
}

TEST(SqliteCCLCJournal, NativeApplyBoundsHistoricalAndReservedIdentityNamespaces) {
  for (const bool route_namespace : {false, true}) {
    for (const bool adopt : {false, true}) {
      SCOPED_TRACE(route_namespace);
      SCOPED_TRACE(adopt);
      ScopedTemporaryDirectory temporary;
      const auto capacity = route_namespace ? kMaximumCCLCActiveConfigurationWatermarks
                                            : kMaximumCCLCComponentWatermarks;
      const auto request_for = [&](const std::size_t index) {
        auto route = ActiveConfiguration(ExpectedDomain());
        route.control_domain_id = ControlDomainId("domain." + std::to_string(index));
        auto request = NativeApplyPlanRequest(
            "apply.capacity." + std::to_string(index), 1U, "bundle.capacity", route);
        if (route_namespace) {
          request.components.clear();
        } else {
          request.components = {ControllableComponentId("component." + std::to_string(index))};
          request.compiled_routes.clear();
        }
        return request;
      };
      {
        SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
        for (std::size_t index = 0U; index < capacity; ++index) {
          const auto request = request_for(index);
          const auto plan = journal.PlanNativeApply(request);
          ASSERT_TRUE(plan.ok()) << plan.error().message;
          if (adopt) {
            std::vector<ActiveControlConfigurationDTO> routes;
            if (route_namespace) {
              auto route = ActiveConfiguration(ExpectedDomain());
              route.control_domain_id = request.compiled_routes.front().control_domain_id;
              route.active_control_configuration_generation = plan.value().routes.front().generation;
              routes.push_back(std::move(route));
            }
            const auto result = journal.AdoptNativeApplyFacts(
                {NativeApplyPublication(request.apply.apply_id.value(), 1U,
                                        "bundle.capacity", index + 2U),
                 plan.value().components, std::move(routes)});
            ASSERT_TRUE(result.ok()) << result.error().message;
          }
        }
        const auto refused = journal.PlanNativeApply(request_for(capacity));
        ASSERT_FALSE(refused.ok());
        EXPECT_EQ(refused.error().code, "component_lifecycle_journal_capacity_exhausted");
        // A refused successor cannot overwrite the last accepted plan, and
        // exact replay remains available even at capacity.
        ASSERT_TRUE(journal.PlanNativeApply(request_for(capacity - 1U)).ok());
      }
      SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
      ASSERT_TRUE(reopened.PlanNativeApply(request_for(capacity - 1U)).ok());
      const auto refused = reopened.PlanNativeApply(request_for(capacity));
      ASSERT_FALSE(refused.ok());
      EXPECT_EQ(refused.error().code, "component_lifecycle_journal_capacity_exhausted");
    }
  }
}

TEST(SqliteCCLCJournal, NativeApplyRetentionRequiresTheExactCurrentFact) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  const ControllableComponentId component("component.1");
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    // Nothing is current yet: a runtime cannot be retained without a fact.
    auto unfounded = NativeApplyPlanRequest("apply.0", 1U, "bundle.a", route);
    unfounded.retained_components = {component};
    const auto missing = journal.PlanNativeApply(unfounded);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, "component_lifecycle_native_apply_retained_fact_missing");

    const auto installed =
        journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
    ASSERT_TRUE(installed.ok());
    ASSERT_TRUE(journal
                    .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                            installed.value().components,
                                            {route}})
                    .ok());

    // A retained route must be the journal's exact current configuration.
    auto drifted = route;
    drifted.source_sequence = route.source_sequence + 1U;
    auto mismatched = NativeApplyPlanRequest("apply.2", 1U, "bundle.b", route);
    mismatched.retained_components = {component};
    mismatched.retained_routes = {drifted};
    const auto mismatch = journal.PlanNativeApply(mismatched);
    ASSERT_FALSE(mismatch.ok());
    EXPECT_EQ(mismatch.error().code, "component_lifecycle_native_apply_retained_route_mismatch");

    auto retaining = NativeApplyPlanRequest("apply.3", 1U, "bundle.b", route);
    retaining.retained_components = {component};
    retaining.retained_routes = {route};
    ASSERT_TRUE(journal.PlanNativeApply(retaining).ok());
    journal.Close();
  }
  {
    // The retained fact changes between planning and adoption.
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(JournalPath(temporary).c_str(), &database), SQLITE_OK);
    const auto update = "UPDATE lifecycle_component_current_facts SET component_lifecycle_state=" +
                        std::to_string(static_cast<int>(ComponentLifecycleStateDTO::kDisabled));
    ASSERT_EQ(sqlite3_exec(database, update.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  }
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto plan = journal.LoadNativeApplyPlan({ApplyId("apply.3"), ApplyGeneration(1U)});
  ASSERT_TRUE(plan.ok());
  ASSERT_TRUE(plan.value().has_value());
  const auto adopted = journal.AdoptNativeApplyFacts(
      {NativeApplyPublication("apply.3", 1U, "bundle.b", 3U), plan.value()->components, {route}});
  ASSERT_FALSE(adopted.ok());
  EXPECT_EQ(adopted.error().code, "component_lifecycle_native_apply_retained_fact_changed");
}

TEST(SqliteCCLCJournal, VersionElevenMigrationKeepsAnAdoptedNativeApply) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const auto route = ActiveConfiguration(ExpectedDomain());
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    const auto plan =
        journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
    ASSERT_TRUE(plan.ok());
    ASSERT_TRUE(journal
                    .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                            plan.value().components,
                                            {route}})
                    .ok());
    journal.Close();
  }
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw,
                         "DROP TABLE cclc_native_apply_plan_removals;"
                         "ALTER TABLE cclc_native_apply_plan_components DROP COLUMN retained;"
                         "ALTER TABLE cclc_native_apply_plan_routes DROP COLUMN retained;"
                         "UPDATE cclc_journal_metadata SET schema_version=11;"
                         "PRAGMA user_version=11;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  SqliteCCLCJournal migrated(path, CellId("cell.1"));
  const auto plan = migrated.LoadNativeApplyPlan({ApplyId("apply.1"), ApplyGeneration(1U)});
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_TRUE(plan.value().has_value());
  EXPECT_TRUE(plan.value()->retained_components.empty());
  EXPECT_TRUE(plan.value()->removed_components.empty());
  const auto fact = migrated.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok());
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kActive);
}

TEST(SqliteCCLCJournal, AnUnsettledPlanBlocksFreshWorkAndItsReservationsAreNeverReused) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto route = ActiveConfiguration(ExpectedDomain());
  ASSERT_TRUE(route.locally_valid());

  // Apply 1 plans and then never settles (for example a known failure).
  const auto first =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.1", route));
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_EQ(first.value().components.size(), 1U);
  ASSERT_EQ(first.value().routes.size(), 1U);

  // Fresh lifecycle work is refused while it is unsettled (H1a A-3).
  RequestOwnerEpoch owner("owner.unsettled-plan");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto intent = DisableIntent("request.unsettled", owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.unsettled");
  const auto denied = journal.AcceptOrReplayLifecycle(
      owner, intent, acceptance, InitialUnresolvedDisableStatus(intent, acceptance));
  ASSERT_FALSE(denied.ok());
  EXPECT_EQ(denied.error().code, "component_lifecycle_native_apply_unsettled");

  // A later Apply may replace the abandoned plan, but never reuses its
  // reserved generations: the host may already have used them.
  const auto second =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.2", 1U, "bundle.2", route));
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_EQ(second.value().components.size(), 1U);
  ASSERT_EQ(second.value().routes.size(), 1U);
  EXPECT_GT(second.value().components.front().generation.value(),
            first.value().components.front().generation.value());
  EXPECT_GT(second.value().routes.front().generation.value(),
            first.value().routes.front().generation.value());
  const auto pending = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(pending.ok());
  ASSERT_TRUE(pending.value().has_value());
  EXPECT_EQ(pending.value()->apply_id, ApplyId("apply.2"));
  // The abandoned plan cannot be adopted any more.
  EXPECT_FALSE(journal
                   .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.1", 2U),
                                           first.value().components,
                                           {route}})
                   .ok());
}

TEST(SqliteCCLCJournal, NativeApplyFactsRequireARecordedPlanAndExactReplay) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto initially_unsettled = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(initially_unsettled.ok());
  EXPECT_FALSE(initially_unsettled.value().has_value());
  const auto publication = NativeApplyPublication("apply.1", 1U, "bundle.1", 2U);
  ASSERT_TRUE(publication.valid());
  const auto route = ActiveConfiguration(ExpectedDomain());
  ASSERT_TRUE(route.locally_valid());

  // Nothing planned: facts observed after publication cannot be adopted.
  const NativeApplyFactAdoptionDTO unplanned{
      publication,
      {{ControllableComponentId("component.1"), ComponentLifecycleStateDTO::kActive,
        ComponentLifecycleGeneration(1U)}},
      {route}};
  EXPECT_FALSE(journal.AdoptNativeApplyFacts(unplanned).ok());

  const auto plan = journal.PlanNativeApply(
      NativeApplyPlanRequest("apply.1", 1U, "bundle.1", route));
  ASSERT_TRUE(plan.ok());
  const auto pending = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(pending.ok());
  ASSERT_TRUE(pending.value().has_value());
  EXPECT_EQ(*pending.value(), plan.value().apply);
  ASSERT_EQ(plan.value().components.size(), 1U);
  EXPECT_EQ(plan.value().components.front().state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(plan.value().components.front().generation, ComponentLifecycleGeneration(1U));
  // The first publication keeps the compiled route generation.
  ASSERT_EQ(plan.value().routes.size(), 1U);
  EXPECT_EQ(plan.value().routes.front().generation,
            route.active_control_configuration_generation);

  // A pending parent Apply reserves those generations. Fresh CCLC work
  // cannot mint a colliding successor, even if it reaches the journal.
  RequestOwnerEpoch owner("owner.native-apply-guard");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto lifecycle_intent = DisableIntent("request.lifecycle", owner, 3U);
  const auto lifecycle_acceptance = Acceptance(lifecycle_intent, 1U, "lifecycle.1");
  const auto lifecycle_denied = journal.AcceptOrReplayLifecycle(
      owner, lifecycle_intent, lifecycle_acceptance,
      InitialUnresolvedDisableStatus(lifecycle_intent, lifecycle_acceptance));
  ASSERT_FALSE(lifecycle_denied.ok());
  EXPECT_EQ(lifecycle_denied.error().code, "component_lifecycle_native_apply_unsettled");
  const auto route_intent = RouteIntent(owner);
  const auto route_prepared = PreparedRoute(owner);
  const auto route_denied = journal.AcceptOrReplayRouteSelection(
      owner, route_intent, route_prepared.acceptance, route_prepared.initial_status);
  ASSERT_FALSE(route_denied.ok());
  EXPECT_EQ(route_denied.error().code, "component_lifecycle_native_apply_unsettled");

  const NativeApplyFactAdoptionDTO adoption{publication, plan.value().components, {route}};
  const auto before = journal.HasNativeApplyFacts(publication);
  ASSERT_TRUE(before.ok());
  EXPECT_FALSE(before.value());
  ASSERT_TRUE(journal.AdoptNativeApplyFacts(adoption).ok());
  ASSERT_TRUE(journal.AdoptNativeApplyFacts(adoption).ok());
  const auto settled = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(settled.ok());
  EXPECT_FALSE(settled.value().has_value());
  const auto after = journal.HasNativeApplyFacts(publication);
  ASSERT_TRUE(after.ok());
  EXPECT_TRUE(after.value());
  const auto current = journal.LoadCurrentActiveControlConfiguration(route.control_domain_id);
  ASSERT_TRUE(current.ok());
  ASSERT_TRUE(current.value().has_value());
  EXPECT_EQ(*current.value(), route);
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok());
  EXPECT_EQ(fact.value().durable_generation, ComponentLifecycleGeneration(1U));
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kActive);

  auto changed = adoption;
  changed.components.front().state = ComponentLifecycleStateDTO::kDisabled;
  EXPECT_FALSE(journal.AdoptNativeApplyFacts(changed).ok());
  auto wrong_publication = publication;
  wrong_publication.publication_generation = ActiveBundlePublicationGeneration(3U);
  EXPECT_FALSE(journal.HasNativeApplyFacts(wrong_publication).ok());
}

TEST(SqliteCCLCJournal, AFailedRouteReadbackAfterPublicationAdoptsNothingAndStaysContained) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  ASSERT_TRUE(route.locally_valid());
  const auto publication = NativeApplyPublication("apply.1", 1U, "bundle.1", 2U);
  std::vector<NativeApplyComponentFactDTO> planned_components;
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const auto plan =
        journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.1", route));
    ASSERT_TRUE(plan.ok()) << plan.error().message;
    planned_components = plan.value().components;
    ASSERT_EQ(planned_components.size(), 1U);

    // The candidate routes are now published; each injected readback below
    // is what the host observed instead of the planned facts.
    const auto expect_nothing_adopted = [&](const NativeApplyFactAdoptionDTO& readback,
                                            std::string_view expected_code) {
      const auto adopted = journal.AdoptNativeApplyFacts(readback);
      ASSERT_FALSE(adopted.ok());
      if (!expected_code.empty()) {
        EXPECT_EQ(adopted.error().code, expected_code);
      }
      // Nothing partial: no current route or component fact, no adoption
      // record, and the plan still blocks fresh lifecycle work.
      const auto current = journal.LoadCurrentActiveControlConfiguration(route.control_domain_id);
      ASSERT_TRUE(current.ok());
      EXPECT_FALSE(current.value().has_value());
      const auto has_facts = journal.HasNativeApplyFacts(publication);
      ASSERT_TRUE(has_facts.ok());
      EXPECT_FALSE(has_facts.value());
      const auto pending = journal.ReadUnsettledNativeApplyPlan();
      ASSERT_TRUE(pending.ok());
      ASSERT_TRUE(pending.value().has_value());
      EXPECT_EQ(pending.value()->apply_id, ApplyId("apply.1"));
    };

    // The route did not reconcile (admission must stay closed).
    auto unreconciled = route;
    unreconciled.state = ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
    expect_nothing_adopted({publication, planned_components, {unreconciled}}, {});
    // The host still reads back a predecessor or foreign generation.
    auto stale_generation = route;
    stale_generation.active_control_configuration_generation =
        ActiveControlConfigurationGeneration(
            route.active_control_configuration_generation.value() + 1U);
    expect_nothing_adopted({publication, planned_components, {stale_generation}},
                           "component_lifecycle_native_apply_unplanned");
    // A planned domain is missing from the readback.
    expect_nothing_adopted({publication, planned_components, {}},
                           "component_lifecycle_native_apply_unplanned");
    // The component's live state differs from the plan.
    auto changed_components = planned_components;
    changed_components.front().state = ComponentLifecycleStateDTO::kDisabled;
    expect_nothing_adopted({publication, changed_components, {route}},
                           "component_lifecycle_native_apply_unplanned");

    RequestOwnerEpoch owner("owner.failed-readback");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    const auto prepared = PreparedRoute(owner);
    const auto denied = journal.AcceptOrReplayRouteSelection(
        owner, RouteIntent(owner), prepared.acceptance, prepared.initial_status);
    ASSERT_FALSE(denied.ok());
    EXPECT_EQ(denied.error().code, "component_lifecycle_native_apply_unsettled");
    journal.Close();
  }
  // After a restart the same Apply adopts its exact readback, once.
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const NativeApplyFactAdoptionDTO exact{publication, planned_components, {route}};
  ASSERT_TRUE(reopened.AdoptNativeApplyFacts(exact).ok());
  const auto current = reopened.LoadCurrentActiveControlConfiguration(route.control_domain_id);
  ASSERT_TRUE(current.ok());
  ASSERT_TRUE(current.value().has_value());
  EXPECT_EQ(*current.value(), route);
  const auto settled = reopened.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(settled.ok());
  EXPECT_FALSE(settled.value().has_value());
}

TEST(SqliteCCLCJournal, UnadoptedNativeApplyPlanStaysContainedAcrossRestart) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(
        journal.PlanNativeApply(NativeApplyPlanRequest("apply.lost", 4U, "bundle.a", route)).ok());
    journal.Close();  // The process ends before the Apply adopts or fails cleanly.
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const auto pending = reopened.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(pending.ok());
  ASSERT_TRUE(pending.value().has_value());
  EXPECT_EQ(pending.value()->apply_id, ApplyId("apply.lost"));
  EXPECT_EQ(pending.value()->apply_generation, ApplyGeneration(4U));
  // Restart route reconciliation uses ordinary route selection: it stays
  // refused and names the Apply, so no reserved generation can be minted.
  RequestOwnerEpoch owner("owner.after-restart");
  ASSERT_TRUE(BeginTestOwnerEpoch(reopened, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto prepared = PreparedRoute(owner);
  const auto denied = reopened.AcceptOrReplayRouteSelection(
      owner, RouteIntent(owner), prepared.acceptance, prepared.initial_status);
  ASSERT_FALSE(denied.ok());
  EXPECT_EQ(denied.error().code, "component_lifecycle_native_apply_unsettled");
  EXPECT_NE(denied.error().message.find("apply.lost/4"), std::string::npos);
  const auto still = reopened.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(still.ok());
  EXPECT_TRUE(still.value().has_value());
}

TEST(SqliteCCLCJournal, NativeApplyPlanNeverReusesAGenerationAndKeepsAnExplicitDisable) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  const auto compiled = route.active_control_configuration_generation.value();
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const auto plan =
        journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
    ASSERT_TRUE(plan.ok());
    ASSERT_TRUE(journal
                    .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                            plan.value().components,
                                            {route}})
                    .ok());
    journal.Close();
  }
  {
    // The operator explicitly disabled the component after the first Apply.
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(JournalPath(temporary).c_str(), &database), SQLITE_OK);
    const auto update = "UPDATE lifecycle_component_current_facts SET component_lifecycle_state=" +
                        std::to_string(static_cast<int>(ComponentLifecycleStateDTO::kDisabled));
    ASSERT_EQ(sqlite3_exec(database, update.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  }
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  // A later Apply (another bundle, then the first one again) keeps the
  // Disable and continues every generation past the previous Apply.
  const auto second =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.2", 1U, "bundle.b", route));
  ASSERT_TRUE(second.ok());
  const auto pending_second = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(pending_second.ok());
  ASSERT_TRUE(pending_second.value().has_value());
  EXPECT_EQ(*pending_second.value(), second.value().apply);
  EXPECT_EQ(second.value().components.front().state, ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(second.value().components.front().generation, ComponentLifecycleGeneration(2U));
  EXPECT_EQ(second.value().routes.front().generation.value(), compiled + 1U);
  // Exact replay returns the recorded plan without reserving again.
  const auto replay =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.2", 1U, "bundle.b", route));
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().components.front().generation, ComponentLifecycleGeneration(2U));
  EXPECT_EQ(replay.value().routes.front().generation.value(), compiled + 1U);
  EXPECT_FALSE(
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.2", 1U, "bundle.c", route)).ok());
  // apply.2 is abandoned before adoption: its generations are never reused.
  const auto third =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.3", 1U, "bundle.a", route));
  ASSERT_TRUE(third.ok());
  const auto pending_third = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(pending_third.ok());
  ASSERT_TRUE(pending_third.value().has_value());
  EXPECT_EQ(*pending_third.value(), third.value().apply);
  EXPECT_EQ(third.value().components.front().state, ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(third.value().components.front().generation, ComponentLifecycleGeneration(3U));
  EXPECT_EQ(third.value().routes.front().generation.value(), compiled + 2U);
  // The superseded plan can no longer be adopted.
  auto stale_route = route;
  stale_route.active_control_configuration_generation =
      ActiveControlConfigurationGeneration(compiled + 1U);
  EXPECT_FALSE(journal
                   .AdoptNativeApplyFacts({NativeApplyPublication("apply.2", 1U, "bundle.b", 3U),
                                           second.value().components,
                                           {stale_route}})
                   .ok());
  auto current_route = route;
  current_route.active_control_configuration_generation =
      ActiveControlConfigurationGeneration(compiled + 2U);
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.3", 1U, "bundle.a", 3U),
                                          third.value().components,
                                          {current_route}})
                  .ok());
  const auto settled_third = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(settled_third.ok());
  EXPECT_FALSE(settled_third.value().has_value());
  const auto fact =
      journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
  ASSERT_TRUE(fact.ok());
  EXPECT_EQ(fact.value().durable_generation, ComponentLifecycleGeneration(3U));
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDisabled);
}

}  // namespace
}  // namespace manyforge::core
