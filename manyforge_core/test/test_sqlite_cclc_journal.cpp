#include <gtest/gtest.h>
#include "close_retirement_sqlite_faults.hpp"
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../src/component_lifecycle_journal_codec_internal.hpp"

namespace manyforge::core {
namespace {

// Actual retained UR5e launch identity from the qualified Close fixture.
const std::string kUr5eLaunchIdentity257 = "mock\nur5e_robot\nur5e_robotiq_2f_85\n/manyforge/ur5e_robot\njoint_trajectory_controller\nrobotiq_gripper_controller\n\nsha256:02e2b9e7733647dc75eea1a518dda0289aa07603d668d07e16dc87c79ab970fc\nsha256:d5e2658bd13c5f16c8f23c699c70fc200a4e4b904e995571c3eda1718faffec6\n";

[[nodiscard]] content_identity::CanonicalValue WithObjectMember(
    const content_identity::CanonicalValue& object, std::string_view key,
    content_identity::CanonicalValue value) {
  auto fields = object.as_object();
  const auto found = std::find_if(fields.begin(), fields.end(),
                                  [&](const auto& field) { return field.first == key; });
  if (found == fields.end())
    fields.emplace_back(std::string(key), std::move(value));
  else
    found->second = std::move(value);
  return content_identity::CanonicalValue(std::move(fields));
}

void ExecRaw(const std::filesystem::path& path, const std::string& sql);

[[nodiscard]] ActiveBundlePublicationReceiptDTO NativeApplyPublication(
    const std::string& apply_id, std::uint64_t apply_generation, const std::string& bundle,
    std::uint64_t publication_generation);
[[nodiscard]] NativeApplyPlanRequestDTO NativeApplyPlanRequest(
    const std::string& apply_id, std::uint64_t apply_generation, const std::string& bundle,
    const ActiveControlConfigurationDTO& route);

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
          expected.controller().controller_topology_id,
          expected.controller().topology_revision,
          std::nullopt,
          expected.controller().expected_control_bindings,
          {ResourceId("resource.1")}};
}

[[nodiscard]] ExpectedControlDomainRevisionDTO ExpectedDisableDomain() {
  auto expected = ExpectedDomain();
  expected.controller().expected_active_control_configuration = {
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      ActiveControlConfigurationId("configuration.1"), ActiveControlConfigurationGeneration(7U)};
  return expected;
}

[[nodiscard]] ActiveControlConfigurationDTO ActiveConfiguration(
    const ExpectedControlDomainRevisionDTO& expected) {
  return {ActiveControlConfigurationId("configuration.1"),
          expected.control_domain_id,
          expected.control_domain_revision,
          {{expected.controller().expected_control_bindings.front().control_binding_id,
            expected.controller().expected_control_bindings.front().control_binding_revision,
            expected.controller().expected_control_bindings.front().control_route_id}},
          ActivationSetId("activation-set.1"),
          expected.controller().controller_topology_id,
          expected.controller().topology_revision,
          RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime-epoch.1"),
          1U,
          {},
          ActiveControlConfigurationGeneration(7U),
          ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO CurrentDisableScope() {
  const auto expected = ExpectedDisableDomain();
  return {expected.control_domain_id,
          expected.control_domain_revision,
          expected.controller().controller_topology_id,
          expected.controller().topology_revision,
          ActiveConfiguration(expected),
          expected.controller().expected_control_bindings,
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

[[nodiscard]] std::shared_ptr<const content_identity::CanonicalValue>
TestLaunchDiagnosticSnapshot() {
  return std::make_shared<
      const content_identity::CanonicalValue>(content_identity::parse_canonical_json(
      R"snapshot({"context":{"controllable_component_id":"component.arm","generator_identity":null,"graph_revision":"graph.1","launch_identity":"sha256:0000000000000000000000000000000000000000000000000000000000000000","request_context":"Apply=apply.7","retained_bundle_id":"bundle.1","runtime_epoch":"epoch.1","runtime_unit_id":"robot_cell.ros2_control_execution"},"core_dump_link":null,"exit":{"core_dumped":false,"observed_utc":"2026-10-11T18:00:00Z","signal":11,"status":null},"generated":{"controllers_sha256":"sha256:2222222222222222222222222222222222222222222222222222222222222222","robot_description_sha256":"sha256:1111111111111111111111111111111111111111111111111111111111111111"},"launch_readiness":{"arm_controller_active":null,"binding_epoch":null,"captured_utc":null,"controller_bound":null,"controller_loaded":null,"controller_manager_present":null,"controller_observed":null,"hardware_ready":null,"joint_sample_available":null,"known":null,"sample_age_ns":null},"output":{"dropped_forward_bytes":0,"dropped_forward_lines":0,"invalid_text_bytes":0,"line_count":1,"tail":"controller crashed\n","truncated":false},"settings":{"argv":["ros2","launch"],"arm_controller_name":"arm","controller_profile":"ur5","controllers_yaml":"arm: effective settings","frame_prefix":"","gripper_controller_name":null,"retained_configuration_json":"{}","runtime_namespace":"/robot"}})snapshot"));
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

TEST(SqliteCCLCJournal, PneumaticNoneControllerQueryRoundTripsWithoutChangingRequiredEncoding) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 4U);
  RequestOwnerEpoch owner("none.codec.owner");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 4U).ok());
  const auto prepared = PreparedRoute(owner);
  const auto legacy = detail::EncodeRouteSelectionStatus(prepared.initial_status);
  EXPECT_EQ(detail::DecodeRouteSelectionStatus(legacy), prepared.initial_status);
  auto none = prepared.initial_status;
  none.acceptance.controller_lifecycle_observation_query.physical_expectation =
      ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone;
  ASSERT_TRUE(none.locally_valid());
  const auto encoded = detail::EncodeRouteSelectionStatus(none);
  EXPECT_EQ(encoded.size(), legacy.size() + 5U); // only this typed query has the escape
  EXPECT_EQ(detail::DecodeRouteSelectionStatus(encoded), none);
  EXPECT_EQ(detail::EncodeRouteSelectionStatus(prepared.initial_status), legacy);
  const auto domain = none.acceptance.controller_lifecycle_observation_query.control_domain_id.value();
  std::string prefix(4U, '\0');
  prefix.push_back(static_cast<char>(ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone));
  for (std::size_t i = 0; i < 4U; ++i)
    prefix.push_back(static_cast<char>((domain.size() >> (i * 8U)) & 0xffU));
  prefix += domain;
  const auto offset = encoded.find(prefix);
  ASSERT_NE(offset, std::string::npos);
  auto invalid = encoded;
  invalid[offset + 4U] = static_cast<char>(255U);
  EXPECT_THROW(detail::DecodeRouteSelectionStatus(invalid), std::invalid_argument);
  auto foreign = none;
  foreign.acceptance.controller_lifecycle_observation_query.source_runtime_epoch = RuntimeEpoch("");
  EXPECT_FALSE(foreign.locally_valid());
  EXPECT_THROW(detail::EncodeRouteSelectionStatus(foreign), std::invalid_argument);
  ASSERT_TRUE(SeedRouteClaim(journal, owner, prepared).ok());
  ASSERT_TRUE(journal.AcceptOrReplayRouteSelection(owner, none.accepted_intent,
      none.acceptance, none).ok());
  journal.Close();
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"), 4U);
  const auto retained = reopened.LoadRetainedRouteSelectionStatus(none.acceptance.route_selection_transaction_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  ASSERT_TRUE(retained.value());
  EXPECT_EQ(*retained.value(), none);
}

TEST(SqliteCCLCJournal, RouteCodecPreservesProgramNodeEvidenceAndRequiresAdmissionGeneration) {
  RequestOwnerEpoch owner("old.seed");
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
  ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  const auto intent = Intent("request.1", owner, 3U);
  const auto accepted = Acceptance(intent, 1U, "lifecycle.1");
  const auto claim = SeedLifecycleClaim(journal, owner, intent, accepted);
  ASSERT_TRUE(claim.ok());
  const auto unsettled = journal.ReadQuiescentOwner(owner);
  ASSERT_FALSE(unsettled.ok());
  EXPECT_EQ(unsettled.error().code, "component_lifecycle_checkpoint_not_quiescent");
  const auto refused = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 2, 2U, owner);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_checkpoint_not_quiescent");
  ASSERT_TRUE(ReleaseUnacceptedTestClaim(journal, owner, claim.value()).ok());
  ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  const auto checkpoint = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 3, 2U, owner);
  ASSERT_TRUE(checkpoint.ok()) << checkpoint.error().message;
  EXPECT_NE(checkpoint.value().request_owner_epoch, owner);
  const auto stale = journal.ReadQuiescentOwner(owner);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "component_lifecycle_journal_owner_epoch_stale");
  ASSERT_TRUE(journal.ReadQuiescentOwner(checkpoint.value().request_owner_epoch).ok());
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

TEST(SqliteCCLCJournal, EarlierSchemaVersionIsRefusedUnchangedWithAFreshStartInstruction) {
  // Version 13 removed the Disable Stop-handoff effect kinds and status
  // fields. The development journal is current-only: an older journal, which
  // may hold those records, is refused before any write and left unchanged.
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U).ok());
  }
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw,
                         "UPDATE cclc_journal_metadata SET schema_version=12;"
                         "PRAGMA user_version=12;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  sqlite3_close(raw);
  try {
    SqliteCCLCJournal refused(path, CellId("cell.1"), 1U);
    FAIL() << "an earlier CCLC journal version was opened";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("schema version 12 is unsupported"), std::string::npos) << message;
    EXPECT_NE(message.find("compose-dev.sh stop"), std::string::npos) << message;
  }
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  sqlite3_stmt* version = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &version, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(version, 0), 12);
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

TEST(SqliteCCLCJournal, RetainedComponentHistoryRequiresCompletedSettledRowsAndSurvivesReopen) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const ControllableComponentId component("component.1");
  std::optional<ComponentLifecycleStatusDTO> completed;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"), 8U);
    RequestOwnerEpoch owner("owner.1");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
    const auto intent = Intent("history.1", owner, 3U);
    const auto accepted = Acceptance(intent, 1U, "lifecycle.1");
    const auto initial = InitialStatus(intent, accepted);
    const auto acquired = SeedLifecycleClaim(journal, owner, intent, accepted);
    ASSERT_TRUE(acquired.ok());
    const auto admitted = journal.AcceptOrReplayLifecycle(owner, intent, accepted, initial);
    ASSERT_TRUE(admitted.ok()) << admitted.error().code << ": " << admitted.error().message;
    const auto pending = journal.LoadRetainedComponentLifecycleHistory(component);
    ASSERT_TRUE(pending.ok());
    EXPECT_TRUE(pending.value().empty());
    completed = TerminalSuccess(initial, 2U);
    const auto &claim = acquired.value();
    const CellAdmissionClaimReleaseDTO release{
        claim.intent.owner_role,
        claim.intent.owner_epoch,
        claim.intent.owner_transaction_id,
        claim.intent.request_id,
        claim.intent.request_payload_digest,
        claim.acquired_admission_generation,
        claim.claim_generation,
        {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
         ContentDigest(
             detail::ComponentLifecycleStatusReleasePreconditionDigest(
                 *completed))}};
    ASSERT_TRUE(
        journal
            .RecordLifecycleTerminalAndClaimRelease(owner, *completed, release)
            .ok());
    const auto before_ack =
        journal.LoadRetainedComponentLifecycleHistory(component);
    ASSERT_TRUE(before_ack.ok());
    EXPECT_TRUE(before_ack.value().empty());
    ASSERT_TRUE(journal
                    .RecordCellClaimReleaseReceipt(
                        owner, release,
                        {CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact,
                         RequestOwnerEpoch("kernel.1"),
                         AdmissionGeneration(2U),
                         claim,
                         release,
                         std::nullopt,
                         {},
                         {}})
                    .ok());
    const auto history = journal.LoadRetainedComponentLifecycleHistory(component);
    ASSERT_TRUE(history.ok());
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(history.value().front(), *completed);
    const auto foreign =
        journal.LoadRetainedComponentLifecycleHistory(ControllableComponentId("component.foreign"));
    ASSERT_TRUE(foreign.ok());
    EXPECT_TRUE(foreign.value().empty());
    EXPECT_FALSE(journal.LoadRetainedComponentLifecycleHistory(ControllableComponentId("")).ok());
    journal.Close();
  }
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 8U);
  const auto history = reopened.LoadRetainedComponentLifecycleHistory(component);
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history.value().size(), 1U);
  EXPECT_EQ(history.value().front(), *completed);
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
  expected.controller().expected_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
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

TEST(SqliteCCLCJournal, RecordedEmptyNativeApplyAdoptsOnSameOwnerAndPreservesRemovalWatermarks) {
  ScopedTemporaryDirectory temporary;
  RequestOwnerEpoch owner("owner.empty-apply");
  const ControllableComponentId component("component.1");
  const auto route = ActiveConfiguration(ExpectedDomain());
  auto empty_request = NativeApplyPlanRequestWithDelta(
      "apply.empty", "bundle.empty", {}, {}, {}, {component}, {}, {route.control_domain_id});
  empty_request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  const auto empty_publication = NativeApplyPublication("apply.empty", 1U, "bundle.empty", 3U);
  ComponentLifecycleGeneration predecessor_generation(0U);
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    auto present_request = NativeApplyPlanRequest("apply.present", 1U, "bundle.present", route);
    present_request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
    const auto present = journal.PlanNativeApply(present_request);
    ASSERT_TRUE(present.ok()) << present.error().message;
    ASSERT_EQ(present.value().components.size(), 1U);
    predecessor_generation = present.value().components.front().generation;
    ASSERT_TRUE(journal.AdoptNativeApplyFacts({
        NativeApplyPublication("apply.present", 1U, "bundle.present", 2U),
        present.value().components, {route}}).ok());
    ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                           LifecycleTargetOriginDTO::kOperator).ok());
    const auto planned = journal.PlanNativeApply(empty_request);
    ASSERT_TRUE(planned.ok()) << planned.error().message;
    EXPECT_TRUE(planned.value().components.empty());
    EXPECT_TRUE(planned.value().routes.empty());
    EXPECT_FALSE(planned.value().component_bring_up);
    EXPECT_EQ(planned.value().removed_components, std::vector<ControllableComponentId>{component});
    EXPECT_EQ(planned.value().removed_domains, std::vector<ControlDomainId>{route.control_domain_id});
    const auto recorded = journal.LoadNativeApplyPlan(empty_request.apply);
    ASSERT_TRUE(recorded.ok() && recorded.value());
    EXPECT_EQ(*recorded.value(), planned.value());
    EXPECT_EQ(journal.ReadUnsettledNativeApplyPlan().value(), empty_request.apply);
    // Planning alone cannot turn a missing physical product into empty facts.
    EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().current_state,
              ComponentLifecycleStateDTO::kDeclared);
    EXPECT_EQ(journal.LoadCurrentActiveControlConfiguration(route.control_domain_id).value(), route);
    EXPECT_FALSE(journal.HasNativeApplyFacts(empty_publication).value());
    ASSERT_TRUE(journal.AdoptNativeApplyFacts({empty_publication, {}, {}}).ok());
    ASSERT_TRUE(journal.HasNativeApplyFacts(empty_publication).value());
    EXPECT_FALSE(journal.ReadUnsettledNativeApplyPlan().value());
    const auto removed = journal.LoadCurrentComponentLifecycleFact(component);
    ASSERT_TRUE(removed.ok());
    EXPECT_FALSE(removed.value().current_state);
    EXPECT_EQ(removed.value().durable_generation, predecessor_generation);
    EXPECT_FALSE(journal.LoadCurrentActiveControlConfiguration(route.control_domain_id).value());
    EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());
    EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
    ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
    EXPECT_FALSE(journal.ReadQuiescentOwner(RequestOwnerEpoch("owner.foreign")).ok());
    ASSERT_TRUE(journal.AdoptNativeApplyFacts({empty_publication, {}, {}}).ok());
    EXPECT_EQ(journal.LoadNativeApplyPlan(empty_request.apply).value(), recorded.value());
    EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().durable_generation,
              predecessor_generation);
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  ASSERT_TRUE(reopened.ReadQuiescentOwner(owner).ok());
  ASSERT_TRUE(reopened.HasNativeApplyFacts(empty_publication).value());
  const auto recorded = reopened.LoadNativeApplyPlan(empty_request.apply);
  ASSERT_TRUE(recorded.ok() && recorded.value());
  EXPECT_TRUE(recorded.value()->components.empty());
  EXPECT_TRUE(recorded.value()->routes.empty());
  EXPECT_FALSE(recorded.value()->component_bring_up);
  EXPECT_FALSE(reopened.LoadCurrentComponentLifecycleFact(component).value().current_state);
  EXPECT_FALSE(reopened.LoadCurrentActiveControlConfiguration(route.control_domain_id).value());
  const auto selection = reopened.ReadApplyComponentGenerationSelection(owner, component);
  ASSERT_TRUE(selection.ok());
  EXPECT_EQ(selection.value().predecessor_generation, predecessor_generation);
  EXPECT_EQ(selection.value().candidate_generation.value(), predecessor_generation.value() + 1U);
  auto readd = NativeApplyPlanRequest("apply.readd", 1U, "bundle.present", route);
  readd.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  const auto readded = reopened.PlanNativeApply(readd);
  ASSERT_TRUE(readded.ok()) << readded.error().message;
  ASSERT_EQ(readded.value().components.size(), 1U);
  ASSERT_EQ(readded.value().routes.size(), 1U);
  EXPECT_EQ(readded.value().components.front().generation, selection.value().candidate_generation);
  EXPECT_EQ(readded.value().routes.front().generation.value(),
            route.active_control_configuration_generation.value() + 1U);
}

TEST(SqliteCCLCJournal, EmptyNativeApplyFactsRequireTheActualMatchingRecordedEmptyPlan) {
  for (const std::string_view kind : {"missing", "foreign_apply", "foreign_generation",
                                      "foreign_bundle", "foreign_graph", "nonempty_plan"}) {
    SCOPED_TRACE(kind);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    RequestOwnerEpoch owner("owner.empty-apply");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    auto request = NativeApplyPlanRequestWithDelta(
        "apply.empty", "bundle.empty", {}, {}, {}, {}, {}, {});
    request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
    if (kind == "nonempty_plan") {
      request = NativeApplyPlanRequest("apply.empty", 1U, "bundle.empty", ActiveConfiguration(ExpectedDomain()));
      request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
    }
    if (kind != "missing") {
      ASSERT_TRUE(journal.PlanNativeApply(request).ok());
    }
    const auto before = journal.LoadNativeApplyPlan(request.apply);
    ASSERT_TRUE(before.ok());
    auto publication = NativeApplyPublication("apply.empty", 1U, "bundle.empty", 2U);
    auto& transaction = std::get<NamedApplyPublicationTransactionDTO>(publication.request.transaction_ref);
    if (kind == "foreign_apply") transaction.apply_id = ApplyId("apply.foreign");
    if (kind == "foreign_generation") transaction.apply_generation = ApplyGeneration(2U);
    if (kind == "foreign_bundle") {
      publication.request.candidate_retained_bundle_id = RetainedBundleId("bundle.foreign");
      publication.request.candidate_projection_revision.retained_bundle_id = RetainedBundleId("bundle.foreign");
    }
    if (kind == "foreign_graph") {
      publication.request.candidate_graph_revision = GraphRevision("graph.foreign");
      publication.request.candidate_projection_revision.graph_revision = GraphRevision("graph.foreign");
    }
    ASSERT_TRUE(publication.valid());
    const auto rejected = journal.AdoptNativeApplyFacts({publication, {}, {}});
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "component_lifecycle_native_apply_unplanned");
    EXPECT_EQ(journal.LoadNativeApplyPlan(request.apply).value(), before.value());
    EXPECT_FALSE(journal.HasNativeApplyFacts(publication).value());
    EXPECT_FALSE(journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"))
                     .value().current_state);
    EXPECT_FALSE(journal.LoadCurrentActiveControlConfiguration(ControlDomainId("domain.1")).value());
    ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
    if (kind == "missing") {
      EXPECT_FALSE(journal.ReadUnsettledNativeApplyPlan().value());
    } else {
      EXPECT_EQ(journal.ReadUnsettledNativeApplyPlan().value(), request.apply);
    }
  }
}

TEST(SqliteCCLCJournal, EmptyNativeApplyReplayRefusesForeignHeadAndUnexpectedFacts) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.empty-apply");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  auto request = NativeApplyPlanRequestWithDelta(
      "apply.empty", "bundle.empty", {}, {}, {}, {}, {}, {});
  request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  const auto plan = journal.PlanNativeApply(request);
  ASSERT_TRUE(plan.ok());
  ASSERT_TRUE(plan.value().components.empty() && plan.value().routes.empty());
  const auto publication = NativeApplyPublication("apply.empty", 1U, "bundle.empty", 2U);
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({publication, {}, {}}).ok());
  for (const std::string_view kind : {"generation", "pointer_digest", "components", "routes"}) {
    SCOPED_TRACE(kind);
    NativeApplyFactAdoptionDTO foreign{publication, {}, {}};
    if (kind == "generation") foreign.publication.publication_generation = ActiveBundlePublicationGeneration(3U);
    if (kind == "pointer_digest") foreign.publication.durable_public_pointer_content_digest = ContentDigest(Sha('e'));
    if (kind == "components") {
      foreign.components.push_back({ControllableComponentId("component.1"),
          ComponentLifecycleStateDTO::kDeclared, ComponentLifecycleGeneration(1U)});
    }
    if (kind == "routes") foreign.current_routes.push_back(ActiveConfiguration(ExpectedDomain()));
    ASSERT_TRUE(foreign.publication.valid());
    const auto rejected = journal.AdoptNativeApplyFacts(foreign);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "component_lifecycle_native_apply_conflict");
    EXPECT_EQ(journal.LoadNativeApplyPlan(request.apply).value(), plan.value());
    EXPECT_TRUE(journal.HasNativeApplyFacts(publication).value());
    EXPECT_FALSE(journal.ReadUnsettledNativeApplyPlan().value());
    EXPECT_FALSE(journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"))
                     .value().current_state);
    EXPECT_FALSE(journal.LoadCurrentActiveControlConfiguration(ControlDomainId("domain.1")).value());
    EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
    ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  }
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

TEST(SqliteCCLCJournal, EarlyParentFailureClassificationKeepsFactsAndReservedGenerations) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  std::optional<NativeApplyPlanDTO> observed_plan;
  bool terminal_observed = false;
  SqliteCCLCJournal* live = nullptr;
  const NativeApplyParentPreparationFailureSource source =
      [&](const NativeApplyPlanDTO& captured) -> Result<bool> {
        // This ordinary journal read would deadlock if the callback retained
        // the journal mutex or a SQLite transaction.
        const auto retained = live->LoadNativeApplyPlan(captured.apply);
        if (!retained.ok()) return Result<bool>::Err(retained.error());
        if (!retained.value() || *retained.value() != captured)
          return Result<bool>::Ok(false);
        observed_plan = captured;
        return Result<bool>::Ok(terminal_observed);
      };
  const NamedApplyPublicationTransactionDTO failed_apply{
      ApplyId("apply.preparation-failed"), ApplyGeneration(1U)};
  NativeApplyPlanDTO failed_plan{failed_apply, RetainedBundleId("bundle.b"),
      GraphRevision("graph.1"), {}, {}, {}, {}, {}, {}};
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"),
        kDefaultCCLCExactResultHistory, kDefaultCCLCTerminalResultBytes, {}, source);
    live = &journal;
    const auto first = journal.PlanNativeApply(
        NativeApplyPlanRequest("apply.initial", 1U, "bundle.a", route));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(journal.AdoptNativeApplyFacts(
        {NativeApplyPublication("apply.initial", 1U, "bundle.a", 2U),
         first.value().components, {route}}).ok());
    const auto before = journal.LoadCurrentComponentLifecycleFact(
        first.value().components.front().component_id);
    ASSERT_TRUE(before.ok());
    ASSERT_TRUE(before.value().current_state.has_value());
    const auto failed = journal.PlanNativeApply(
        NativeApplyPlanRequest("apply.preparation-failed", 1U, "bundle.b", route));
    ASSERT_TRUE(failed.ok());
    failed_plan = failed.value();
    const auto pending = journal.ReadUnsettledNativeApplyPlan();
    ASSERT_TRUE(pending.ok());
    ASSERT_TRUE(pending.value().has_value());
    terminal_observed = true;
    const auto ended = journal.ReadUnsettledNativeApplyPlan();
    ASSERT_TRUE(ended.ok());
    EXPECT_FALSE(ended.value().has_value());
    EXPECT_EQ(observed_plan, std::optional(failed_plan));
    const auto exemption = journal.ComponentInUnadoptedApplyPlan(
        failed_plan.components.front().component_id);
    ASSERT_TRUE(exemption.ok());
    EXPECT_FALSE(exemption.value());
    const auto after = journal.LoadCurrentComponentLifecycleFact(
        first.value().components.front().component_id);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value(), before.value());
    const auto adopted = journal.HasNativeApplyFacts(
        NativeApplyPublication("apply.preparation-failed", 1U, "bundle.b", 3U));
    ASSERT_TRUE(adopted.ok());
    EXPECT_FALSE(adopted.value());
    journal.Close();
  }
  // The classification was never written as adoption or a new journal row.
  {
    SqliteCCLCJournal without_source(JournalPath(temporary), CellId("cell.1"));
    const auto pending = without_source.ReadUnsettledNativeApplyPlan();
    ASSERT_TRUE(pending.ok());
    EXPECT_EQ(pending.value(), std::optional(failed_plan.apply));
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"),
      kDefaultCCLCExactResultHistory, kDefaultCCLCTerminalResultBytes, {}, source);
  live = &reopened;
  const auto ended = reopened.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(ended.ok());
  EXPECT_FALSE(ended.value().has_value());
  const auto next = reopened.PlanNativeApply(
      NativeApplyPlanRequest("apply.next", 1U, "bundle.c", route));
  ASSERT_TRUE(next.ok());
  EXPECT_GT(next.value().components.front().generation.value(),
            failed_plan.components.front().generation.value());
  EXPECT_GT(next.value().routes.front().generation.value(),
            failed_plan.routes.front().generation.value());
}

TEST(SqliteCCLCJournal, ParentFailureObservationCannotEndAChangedPlan) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  SqliteCCLCJournal* live = nullptr;
  bool replaced = false;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"),
      kDefaultCCLCExactResultHistory, kDefaultCCLCTerminalResultBytes, {},
      [&](const NativeApplyPlanDTO& captured) -> Result<bool> {
        if (captured.apply.apply_id != ApplyId("apply.old"))
          return Result<bool>::Err(ErrorDTO{"parent_unavailable", "retained parent unavailable", {}});
        const auto replacement = live->PlanNativeApply(
            NativeApplyPlanRequest("apply.new", 1U, "bundle.b", route));
        if (!replacement.ok()) return Result<bool>::Err(replacement.error());
        replaced = true;
        return Result<bool>::Ok(true);
      });
  live = &journal;
  ASSERT_TRUE(journal.PlanNativeApply(
      NativeApplyPlanRequest("apply.old", 1U, "bundle.a", route)).ok());
  const auto pending = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(pending.ok());
  EXPECT_TRUE(replaced);
  ASSERT_TRUE(pending.value().has_value());
  EXPECT_EQ(pending.value()->apply_id, ApplyId("apply.new"));
  const auto still_pending = journal.ReadUnsettledNativeApplyPlan();
  ASSERT_TRUE(still_pending.ok());
  EXPECT_EQ(still_pending.value(), pending.value());
  const auto exemption = journal.ComponentInUnadoptedApplyPlan(
      ControllableComponentId("component.1"));
  ASSERT_TRUE(exemption.ok());
  EXPECT_TRUE(exemption.value());
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

TEST(SqliteCCLCJournal, ObservedControllerApplyAdoptsDeclaredComponentsAndPublishedRoutes) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  const auto compiled = route.active_control_configuration_generation.value();
  const auto declared_request = [&](const std::string& apply_id, const std::string& bundle) {
    auto request = NativeApplyPlanRequest(apply_id, 1U, bundle, route);
    request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
    return request;
  };
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    // Only Active (native) or Declared (observed controller) may be asked for.
    auto prepared = declared_request("apply.invalid", "bundle.a");
    prepared.changed_component_state = ComponentLifecycleStateDTO::kPrepared;
    const auto invalid = journal.PlanNativeApply(prepared);
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.error().code, "component_lifecycle_native_apply_invalid");

    const auto plan = journal.PlanNativeApply(declared_request("apply.1", "bundle.a"));
    ASSERT_TRUE(plan.ok()) << plan.error().message;
    ASSERT_EQ(plan.value().components.size(), 1U);
    EXPECT_EQ(plan.value().components.front().state, ComponentLifecycleStateDTO::kDeclared);
    EXPECT_EQ(plan.value().components.front().generation, ComponentLifecycleGeneration(1U));
    ASSERT_EQ(plan.value().routes.size(), 1U);
    EXPECT_EQ(plan.value().routes.front().generation.value(), compiled);
    // The same delegation cannot be replayed as an enabling Apply.
    const auto enabling =
        journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
    ASSERT_FALSE(enabling.ok());
    EXPECT_EQ(enabling.error().code, "component_lifecycle_native_apply_conflict");
    const auto replay = journal.PlanNativeApply(declared_request("apply.1", "bundle.a"));
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    EXPECT_EQ(replay.value().components.front().state, ComponentLifecycleStateDTO::kDeclared);
    EXPECT_EQ(replay.value().components.front().generation, ComponentLifecycleGeneration(1U));

    // Readiness the host never established cannot be adopted.
    const auto publication = NativeApplyPublication("apply.1", 1U, "bundle.a", 2U);
    auto active = plan.value().components;
    active.front().state = ComponentLifecycleStateDTO::kActive;
    const auto claimed_active = journal.AdoptNativeApplyFacts({publication, active, {route}});
    ASSERT_FALSE(claimed_active.ok());
    EXPECT_EQ(claimed_active.error().code, "component_lifecycle_native_apply_unplanned");
    auto other_route = route;
    other_route.active_control_configuration_generation =
        ActiveControlConfigurationGeneration(compiled + 1U);
    EXPECT_FALSE(
        journal.AdoptNativeApplyFacts({publication, plan.value().components, {other_route}}).ok());

    const NativeApplyFactAdoptionDTO exact{publication, plan.value().components, {route}};
    ASSERT_TRUE(journal.AdoptNativeApplyFacts(exact).ok());
    ASSERT_TRUE(journal.AdoptNativeApplyFacts(exact).ok());
    const auto settled = journal.ReadUnsettledNativeApplyPlan();
    ASSERT_TRUE(settled.ok());
    EXPECT_FALSE(settled.value().has_value());
    const auto fact =
        journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.1"));
    ASSERT_TRUE(fact.ok());
    EXPECT_EQ(fact.value().durable_generation, ComponentLifecycleGeneration(1U));
    EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);
    const auto current = journal.LoadCurrentActiveControlConfiguration(route.control_domain_id);
    ASSERT_TRUE(current.ok());
    ASSERT_TRUE(current.value().has_value());
    EXPECT_EQ(*current.value(), route);
    journal.Close();
  }
  {
    // A later Load keeps the component Declared and continues past every
    // generation the first one used.
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const auto second = journal.PlanNativeApply(declared_request("apply.2", "bundle.b"));
    ASSERT_TRUE(second.ok()) << second.error().message;
    EXPECT_EQ(second.value().components.front().state, ComponentLifecycleStateDTO::kDeclared);
    EXPECT_EQ(second.value().components.front().generation, ComponentLifecycleGeneration(2U));
    EXPECT_EQ(second.value().routes.front().generation.value(), compiled + 1U);
    auto second_route = route;
    second_route.active_control_configuration_generation =
        ActiveControlConfigurationGeneration(compiled + 1U);
    ASSERT_TRUE(journal
                    .AdoptNativeApplyFacts({NativeApplyPublication("apply.2", 1U, "bundle.b", 3U),
                                            second.value().components,
                                            {second_route}})
                    .ok());
    journal.Close();
  }
  {
    // The operator explicitly disabled the component.
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(JournalPath(temporary).c_str(), &database), SQLITE_OK);
    const auto update = "UPDATE lifecycle_component_current_facts SET component_lifecycle_state=" +
                        std::to_string(static_cast<int>(ComponentLifecycleStateDTO::kDisabled));
    ASSERT_EQ(sqlite3_exec(database, update.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  }
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto third = journal.PlanNativeApply(declared_request("apply.3", "bundle.a"));
  ASSERT_TRUE(third.ok()) << third.error().message;
  EXPECT_EQ(third.value().components.front().state, ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(third.value().components.front().generation, ComponentLifecycleGeneration(3U));
  EXPECT_EQ(third.value().routes.front().generation.value(), compiled + 2U);
  // A Disabled plan replays under either changed-state request: only the
  // non-Disabled changed components are bound to it.
  EXPECT_TRUE(journal.PlanNativeApply(declared_request("apply.3", "bundle.a")).ok());
}

// --- Requested lifecycle targets (ADR-0026 §7 amendment, 2026-10-08) ---

TEST(SqliteCCLCJournal, ConfigurationAdoptionLeavesNewComponentsWithoutFollowedTargets) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto route = ActiveConfiguration(ExpectedDomain());
  const ControllableComponentId component("component.1");
  auto request = NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route);
  request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  const auto plan = journal.PlanNativeApply(request);
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  // Nothing is targeted before the facts are adopted.
  ASSERT_TRUE(journal.LoadLifecycleTargets().ok());
  EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
  EXPECT_FALSE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                          LifecycleTargetOriginDTO::kOperator)
                   .ok());
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                          plan.value().components,
                                          {route}})
                  .ok());
  const auto targets = journal.LoadLifecycleTargets();
  ASSERT_TRUE(targets.ok());
  EXPECT_TRUE(targets.value().empty());
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());
  const auto fact = journal.LoadCurrentComponentLifecycleFact(component);
  ASSERT_TRUE(fact.ok());
  EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(fact.value().durable_generation, plan.value().components.front().generation);
  // Exact adoption replay is read-only: the generation does not advance.
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                          plan.value().components,
                                          {route}})
                  .ok());
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());

  // Only an explicit operator request creates the first followed target.
  const auto disabled = journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kDisabled,
                                                   LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(disabled.ok()) << disabled.error().message;
  EXPECT_EQ(disabled.value().target_generation, 1U);
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), disabled.value());
}

TEST(SqliteCCLCJournal, ChangedConfigurationRetiresIntentAndRemovalKeepsItsExistingCancellation) {
  ScopedTemporaryDirectory temporary;
  const auto route = ActiveConfiguration(ExpectedDomain());
  const ControllableComponentId component("component.1");
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
    ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kDisabled,
                                           LifecycleTargetOriginDTO::kOperator)
                    .ok());
    journal.Close();
  }
  {
    // The operator's Disable completed.
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(JournalPath(temporary).c_str(), &database), SQLITE_OK);
    const auto update = "UPDATE lifecycle_component_current_facts SET component_lifecycle_state=" +
                        std::to_string(static_cast<int>(ComponentLifecycleStateDTO::kDisabled));
    ASSERT_EQ(sqlite3_exec(database, update.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  }
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  auto next_route = route;
  next_route.active_control_configuration_generation = ActiveControlConfigurationGeneration(
      route.active_control_configuration_generation.value() + 1U);
  const auto second =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.2", 1U, "bundle.b", route));
  ASSERT_TRUE(second.ok());
  ASSERT_EQ(second.value().components.front().state, ComponentLifecycleStateDTO::kDisabled);
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.2", 1U, "bundle.b", 3U),
                                          second.value().components,
                                          {next_route}})
                  .ok());
  // The observed Disabled state remains truthful, but a changed occurrence
  // has no followed target until a new explicit operator command.
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().current_state,
            ComponentLifecycleStateDTO::kDisabled);
  const auto disabled = journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kDisabled,
                                                    LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(disabled.ok());
  EXPECT_EQ(disabled.value().target_generation, 2U);
  ASSERT_TRUE(journal
                  .RecordInterventionCause(component, ControlDomainId("domain.1"),
                                           LifecycleBlockingCauseClassDTO::kFault, 10, 0U)
                  .ok());

  // An Apply that removes the component cancels its target and causes.
  const auto removal = NativeApplyPlanRequestWithDelta(
      "apply.3", "bundle.c", {}, {}, {}, {component}, {}, {route.control_domain_id});
  const auto removed = journal.PlanNativeApply(removal);
  ASSERT_TRUE(removed.ok()) << removed.error().message;
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.3", 1U, "bundle.c", 4U),
                                          removed.value().components,
                                          {}})
                  .ok());
  EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value().has_value());
  EXPECT_TRUE(journal.LoadInterventionCauses(component).value().empty());
  // The cancellation fences a driver pass that read the facts before it.
  const auto cancelled = journal.LoadLifecycleTargetFacts(component);
  ASSERT_TRUE(cancelled.ok()) << cancelled.error().message;
  EXPECT_FALSE(cancelled.value().target.has_value());
  EXPECT_EQ(cancelled.value().recovery_generation, 1U);
  const auto stale =
      journal.RecordInterventionCause(component, ControlDomainId("domain.1"),
                                      LifecycleBlockingCauseClassDTO::kFault, 20, 0U);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, kLifecycleFactsStaleErrorCode);
  EXPECT_TRUE(journal.LoadInterventionCauses(component).value().empty());

  // Re-adding it configures only; the tombstone preserves target generation.
  auto readd_route = route;
  readd_route.active_control_configuration_generation = ActiveControlConfigurationGeneration(
      route.active_control_configuration_generation.value() + 2U);
  const auto readded =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.4", 1U, "bundle.a", route));
  ASSERT_TRUE(readded.ok()) << readded.error().message;
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.4", 1U, "bundle.a", 5U),
                                          readded.value().components,
                                          {readd_route}})
                  .ok())
      << "re-add adoption";
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());
  const auto explicit_target = journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                                         LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(explicit_target.ok());
  EXPECT_EQ(explicit_target.value().target_generation, 3U);
}

TEST(SqliteCCLCJournal, InterventionCauseSurvivesReopenUntilFreshStatusClearsIt) {
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("component.1");
  const ControlDomainId domain("domain.1");
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const auto recorded = journal.RecordInterventionCause(
        component, domain, LifecycleBlockingCauseClassDTO::kEmergencyStop, 100, 0U);
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;
    EXPECT_TRUE(recorded.value());
    // The same cause is not rewritten.
    EXPECT_FALSE(journal
                     .RecordInterventionCause(component, domain,
                                              LifecycleBlockingCauseClassDTO::kEmergencyStop, 200,
                                              0U)
                     .value());
    // Only intervention-requiring classes are retained.
    EXPECT_FALSE(journal
                     .RecordInterventionCause(component, domain,
                                              LifecycleBlockingCauseClassDTO::kAvailability, 300,
                                              0U)
                     .ok());
    journal.Close();
  }
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto causes = journal.LoadInterventionCauses(component);
  ASSERT_TRUE(causes.ok());
  ASSERT_EQ(causes.value().size(), 1U);
  EXPECT_EQ(causes.value().front(),
            (ComponentLifecycleInterventionCauseDTO{
                component, domain, LifecycleBlockingCauseClassDTO::kEmergencyStop, 100}));
  EXPECT_TRUE(journal.RecordInterventionCause(component, domain, std::nullopt, 400, 0U).value());
  EXPECT_TRUE(journal.LoadInterventionCauses(component).value().empty());
  EXPECT_FALSE(journal.RecordInterventionCause(component, domain, std::nullopt, 500, 0U).value());
}

// --- Component incidents (addendum 2, 2026-10-08) ---

[[nodiscard]] ComponentIncidentRecordDTO TestIncident(const std::string& id,
                                                      const ControllableComponentId& component,
                                                      const LifecycleBlockingCauseClassDTO cause,
                                                      const std::int64_t detected_at_ns) {
  ComponentLifecycleIncidentDTO incident{id,
                                         component,
                                         cause,
                                         {"C2", "mode=0"},
                                         detected_at_ns,
                                         "Component lost its controller.",
                                         std::nullopt,
                                         {},
                                         ComponentIncidentContainmentStateDTO::kPending,
                                         std::nullopt,
                                         std::nullopt,
                                         false,
                                         std::nullopt,
                                         std::nullopt,
                                         std::nullopt};
  return {incident, ComponentIncidentEventPayload(
                        incident, LifecycleIncidentRecoveryPolicyFor(
                                      DefaultLifecycleIncidentRecoveryPolicy(), cause),
                        kComponentIncidentEventKind)};
}

[[nodiscard]] ComponentLifecycleStatusDTO SelectedStatus(const RequestOwnerEpoch& owner,
                                                         std::uint64_t predecessor = 1U) {
  auto intent = Intent("selected.request.1", owner, predecessor, "selected bring-up");
  intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  intent.expected_control_domains = {ExpectedDisableDomain()};
  auto status = InitialStatus(intent, Acceptance(intent, 1U, "selected-lifecycle.1"));
  status.previous_state = ComponentLifecycleStateDTO::kActive;
  status.current_state = ComponentLifecycleStateDTO::kActive;
  status.requested_state = {ComponentLifecycleRequestedStatePresenceDTO::kResolved,
                            ComponentLifecycleStateDTO::kActive};
  auto& domain = status.domains.front();
  domain.expected_domain = intent.expected_control_domains.front();
  domain.current_scope = CurrentDisableScope();
  domain.previous_state = ComponentLifecycleStateDTO::kActive;
  domain.current_state = ComponentLifecycleStateDTO::kActive;
  domain.requested_state = status.requested_state;
  const ContentDigest digest("sha256:" + std::string(64U, 'a'));
  const CellAdmissionClaimIntentDTO claim{
      CellAdmissionClaimKindDTO::kInterventionOrRecovery,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      owner,
      status.acceptance.lifecycle_transaction_id.value(),
      intent.request_context.request_id,
      digest,
      intent.expected_admission_generation,
      {{ResourceId("resource.1")},
       {{ControlDomainId("domain.1"), ActiveControlConfigurationId("configuration.1"),
         ActiveControlConfigurationGeneration(7U)}}},
      std::nullopt,
      CellAdmissionInterventionEvidenceDTO{
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner,
          status.acceptance.lifecycle_transaction_id.value(), digest, std::nullopt},
      std::nullopt};
  status.selected_bring_up = SelectedComponentBringUpEvidenceDTO{
      digest,
      digest,
      "incident.selected",
      StopId("stop.selected"),
      StopGeneration(7U),
      {RetainedBundleId("bundle.selected"),
       GraphRevision("graph.selected"),
       {SceneId(digest.value()), 1U, digest}},
      digest,
      1024U,
      {ExecutionBindingEpoch(12U), {{ControlDomainId("domain.1"), 30U}}},
      {ExecutionBindingEpoch(13U), {{ControlDomainId("domain.1"), 31U}}},
      {claim, CellAdmissionClaimGeneration(15U), AdmissionGeneration(1U), 80},
      {ConfigurationFenceId("fence.selected"),
       ConfigurationFenceGeneration(22U),
       {{ControllableComponentId("component.1")},
        {{ControlDomainId("domain.1"), ControlDomainRevision("domain-revision.1"),
          ControlDomainRevision("domain-revision.1")}},
        {{ControlBindingId("binding.1"), ControlBindingRevision("binding-revision.1"),
          ControlBindingRevision("binding-revision.1")}},
        {ResourceId("resource.1")}},
       90,
       RequestReplayStatusDTO::kAcceptedNew},
      ContentDigest("sha256:" + std::string(64U, 'c'))};
  return status;
}

[[nodiscard]] Result<void> SeedSelectedClaim(SqliteCCLCJournal& journal,
                                             const ComponentLifecycleStatusDTO& status) {
  const auto& claim = status.selected_bring_up->selected_claim;
  const auto attempted = journal.RecordSelectedCellClaimAttempt(
      claim.intent.owner_epoch, claim.intent,
      status.selected_bring_up->original_request_payload_digest);
  if (!attempted.ok()) return attempted;
  return journal.RecordCellClaimAcquired(claim.intent.owner_epoch, claim);
}

[[nodiscard]] Result<void> SeedApplyPredecessorFacts(SqliteCCLCJournal& journal) {
  const auto route = ActiveConfiguration(ExpectedDomain());
  const auto planned = journal.PlanNativeApply(
      NativeApplyPlanRequest("apply.predecessor", 1U, "bundle.a", route));
  if (!planned.ok()) return Result<void>::Err(planned.error());
  return journal.AdoptNativeApplyFacts({
      NativeApplyPublication("apply.predecessor", 1U, "bundle.a", 2U),
      planned.value().components, {route}});
}

[[nodiscard]] ComponentLifecycleStatusDTO ApplyBringUpStatus(SqliteCCLCJournal& journal, const RequestOwnerEpoch& owner,
                                                            bool recovery = false) {
  const auto selection = journal.ReadApplyComponentGenerationSelection(owner, ControllableComponentId("component.1"));
  if (!selection.ok()) throw std::runtime_error(selection.error().message);
  auto status = SelectedStatus(owner);
  status.accepted_intent.apply_generation_selection = selection.value();
  status.accepted_intent.expected_component_lifecycle_generation =
      selection.value().predecessor_generation.value_or(ComponentLifecycleGeneration(0U));
  status.acceptance.component_lifecycle_generation = selection.value().candidate_generation;
  const auto selected = *status.selected_bring_up;
  status.selected_bring_up.reset();
  status.acceptance.lifecycle_transaction_id = LifecycleTransactionId("apply-lifecycle.1");
  const ContentDigest plan_digest("sha256:" + std::string(64U, 'd'));
  const ApplyFenceOwnerDTO parent_owner{ApplyId("apply.parent"), ApplyGeneration(4U)};
  const ApplyComponentBringUpReferenceDTO parent{parent_owner, plan_digest};
  const CellAdmissionClaimIntentDTO claim_intent{
      CellAdmissionClaimKindDTO::kManagerApply, CellAdmissionOwnerRoleDTO::kManager,
      RequestOwnerEpoch("manager.original"), parent.owner.apply_id.value(),
      RequestId("manager.request"), plan_digest, status.accepted_intent.expected_admission_generation,
      {{}, {}}, std::nullopt, std::nullopt, std::nullopt};
  status.apply_bring_up = ApplyComponentBringUpEvidenceDTO{
      {parent, {claim_intent, CellAdmissionClaimGeneration(17U),
                claim_intent.expected_admission_generation, 70},
       {parent.owner, ConfigurationFenceId("parent.fence"), ConfigurationFenceGeneration(22U),
        selected.accepted_fence.captured_scope, RuntimeEpoch("manager.runtime")}},
      selected.accepted_fence.captured_scope,
      selected.original_request_payload_digest, selected.candidate_projection_revision,
      selected.candidate_retained_bundle_digest, selected.candidate_retained_bundle_bytes,
      {RuntimeUnitId("host.unit"), RuntimeEpoch("host.original")},
      std::nullopt, std::nullopt, std::nullopt};
  auto& evidence = *status.apply_bring_up;
  if (recovery) {
    evidence.recovery = ApplyComponentBringUpRecoveryEvidenceDTO{
        selected.predecessor_snapshot_digest, selected.incident_id, selected.containment_stop_id,
        selected.containment_stop_generation};
    evidence.predecessor_occurrence = selected.predecessor_occurrence;
    evidence.successor_occurrence_floor = selected.successor_occurrence;
  }
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id, evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch, parent);
  return status;
}

[[nodiscard]] Result<bool> ContainSelectedIncident(SqliteCCLCJournal& journal,
                                                   const ComponentLifecycleStatusDTO& status) {
  const auto& evidence = *status.selected_bring_up;
  const auto initial =
      TestIncident(evidence.incident_id, status.accepted_intent.controllable_component_id,
                   LifecycleBlockingCauseClassDTO::kProviderLost, 20);
  const auto opened = journal.OpenComponentIncident(initial, 0U);
  if (!opened.ok()) return Result<bool>::Err(opened.error());
  const ComponentIncidentContainmentReceiptDTO receipt{
      evidence.containment_stop_id, evidence.containment_stop_generation, {}, std::nullopt};
  const auto record = ComponentIncidentRecordWithContainment(
      opened.value().record_json, ComponentIncidentContainmentStateDTO::kRequested, receipt,
      std::nullopt);
  return journal.RecordComponentIncidentContainment(
      evidence.incident_id, ComponentIncidentContainmentStateDTO::kRequested, record, 30);
}

TEST(SqliteCCLCJournal, SelectedAcceptanceKeepsIncidentAndTargetAndReplaysExactEvidence) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.selected");
  ComponentLifecycleStatusDTO saved = SelectedStatus(owner);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    const auto route = ActiveConfiguration(ExpectedDomain());
    const auto plan =
        journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
    ASSERT_TRUE(plan.ok()) << plan.error().message;
    ASSERT_TRUE(journal
                    .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                            plan.value().components,
                                            {route}})
                    .ok());
    ASSERT_TRUE(journal
                    .SetLifecycleTarget(ControllableComponentId("component.1"),
                                        LifecycleTargetStateDTO::kDisabled,
                                        LifecycleTargetOriginDTO::kOperator)
                    .ok());
    const auto target = journal.LoadLifecycleTarget(ControllableComponentId("component.1"));
    ASSERT_TRUE(target.ok());
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    saved = SelectedStatus(owner, plan.value().components.front().generation.value());
    ASSERT_TRUE(saved.locally_valid());
    ASSERT_TRUE(SeedSelectedClaim(journal, saved).ok());
    ASSERT_TRUE(ContainSelectedIncident(journal, saved).value());
    const auto initial_incident =
        journal.LoadOpenComponentIncident(saved.accepted_intent.controllable_component_id).value();
    ASSERT_TRUE(initial_incident);
    EXPECT_FALSE(
        journal.AcceptOrReplayLifecycle(owner, saved.accepted_intent, saved.acceptance, saved)
            .ok());
    auto accepted = journal.AcceptOrReplaySelectedComponentBringUp(
        owner, saved.accepted_intent, saved.acceptance, saved,
        saved.selected_bring_up->original_request_payload_digest);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    EXPECT_EQ(accepted.value().status, saved);
    // Acceptance reserves a CCLC generation while retaining the last-known
    // predecessor state. It does not establish the successor controller.
    const auto fact =
        journal.LoadCurrentComponentLifecycleFact(saved.accepted_intent.controllable_component_id);
    ASSERT_TRUE(fact.ok());
    EXPECT_EQ(fact.value().durable_generation, saved.acceptance.component_lifecycle_generation);
    EXPECT_EQ(fact.value().current_state, ComponentLifecycleStateDTO::kActive);
    ASSERT_TRUE(accepted.value().status);
    EXPECT_EQ(accepted.value().status->selected_bring_up->predecessor_occurrence,
              saved.selected_bring_up->predecessor_occurrence);
    const auto unsettled = journal.LoadComponentsWithUnsettledLifecycle();
    ASSERT_TRUE(unsettled.ok());
    EXPECT_EQ(unsettled.value(), std::vector<ControllableComponentId>{
                                     saved.accepted_intent.controllable_component_id});
    EXPECT_EQ(journal.LoadLifecycleTarget(saved.accepted_intent.controllable_component_id).value(),
              target.value());
    EXPECT_EQ(
        journal.LoadOpenComponentIncident(saved.accepted_intent.controllable_component_id).value(),
        initial_incident);
    auto proposed = saved;
    proposed.selected_bring_up->successor_occurrence.domains.front().observation_generation = 999U;
    auto replay = journal.AcceptOrReplaySelectedComponentBringUp(
        owner, saved.accepted_intent, saved.acceptance, proposed,
        saved.selected_bring_up->original_request_payload_digest);
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(replay.value().status, saved);
    auto foreign_intent = saved.accepted_intent;
    foreign_intent.reason = "different selected request";
    const auto conflict = journal.InspectSelectedComponentBringUpRequest(
        owner, foreign_intent, saved.selected_bring_up->original_request_payload_digest);
    ASSERT_TRUE(conflict.ok());
    ASSERT_TRUE(conflict.value());
    EXPECT_EQ(conflict.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);
    auto changed_digest = ContentDigest("sha256:" + std::string(64U, 'b'));
    EXPECT_EQ(
        journal.InspectSelectedComponentBringUpRequest(owner, saved.accepted_intent, changed_digest)
            .value()
            ->replay_status,
        RequestReplayStatusDTO::kRequestIdConflict);
    auto changed_status = saved;
    changed_status.status_sequence = 2U;
    changed_status.latest_updated_at_ns += 1;
    changed_status.selected_bring_up->candidate_retained_bundle_bytes += 1;
    EXPECT_FALSE(journal.RecordLifecycleStatus(owner, changed_status).ok());
    journal.Close();
  }
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  const auto read = reopened.ReadSelectedComponentBringUpRequestResult(
      saved.accepted_intent, saved.selected_bring_up->original_request_payload_digest);
  ASSERT_TRUE(read.ok());
  ASSERT_TRUE(read.value());
  EXPECT_EQ(read.value()->status, saved);
  RequestOwnerEpoch replacement("owner.replacement");
  const auto recovery =
      BeginTestOwnerEpoch(reopened, replacement, 200, kDefaultCCLCExactResultHistory);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 1U);
  EXPECT_EQ(recovery.value().interrupted_transactions.front(), saved);
  ASSERT_EQ(recovery.value().cell_claims.size(), 1U);
  EXPECT_EQ(recovery.value().cell_claims.front().claim, saved.selected_bring_up->selected_claim);
  EXPECT_TRUE(recovery.value().cell_claims.front().transaction_accepted);
  const auto restored_fact =
      reopened.LoadCurrentComponentLifecycleFact(saved.accepted_intent.controllable_component_id);
  ASSERT_TRUE(restored_fact.ok());
  EXPECT_EQ(restored_fact.value().durable_generation,
            saved.acceptance.component_lifecycle_generation);
  EXPECT_EQ(restored_fact.value().current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(reopened.LoadComponentsWithUnsettledLifecycle().value(),
            std::vector<ControllableComponentId>{saved.accepted_intent.controllable_component_id});
  EXPECT_EQ(reopened
                .ReadSelectedComponentBringUpRequestResult(
                    saved.accepted_intent, saved.selected_bring_up->original_request_payload_digest)
                .value()
                ->status,
            saved);
}

TEST(SqliteCCLCJournal, ApplyGenerationSelectionIsAnExactOwnerObservationWithoutSeeding) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.apply.selection");
  const ControllableComponentId component("component.1");
  const auto absent_owner = journal.ReadApplyComponentGenerationSelection(owner, component);
  ASSERT_FALSE(absent_owner.ok());
  EXPECT_EQ(absent_owner.error().code, "component_lifecycle_journal_owner_epoch_stale");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto selected = journal.ReadApplyComponentGenerationSelection(owner, component);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_EQ(selected.value(), (ApplyComponentGenerationSelectionDTO{
      std::nullopt, ComponentLifecycleGeneration(1U), 0U}));
  const auto fact = journal.LoadCurrentComponentLifecycleFact(component);
  ASSERT_TRUE(fact.ok());
  EXPECT_FALSE(fact.value().durable_generation);
  EXPECT_FALSE(fact.value().current_state);
  EXPECT_TRUE(journal.LoadComponentsWithUnsettledLifecycle().value().empty());
  EXPECT_FALSE(journal.ReadApplyComponentGenerationSelection(
      RequestOwnerEpoch("owner.foreign"), component).ok());
  EXPECT_EQ(journal.ReadApplyComponentGenerationSelection(owner, component).value(), selected.value());
  journal.Close();
  const auto closed = journal.ReadApplyComponentGenerationSelection(owner, component);
  ASSERT_FALSE(closed.ok());
  EXPECT_EQ(closed.error().code, "component_lifecycle_journal_closed");
}

TEST(SqliteCCLCJournal, ApplyGenerationSelectionHonorsWatermarksAndAbandonedReservationsOnReadd) {
  for (const auto prior_watermark : {0U, 7U}) {
    SCOPED_TRACE(prior_watermark);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    auto route = ActiveConfiguration(ExpectedDomain());
    for (unsigned index = 1U; index <= prior_watermark; ++index) {
      const auto apply_id = "apply.history." + std::to_string(index);
      const auto plan = journal.PlanNativeApply(
          NativeApplyPlanRequest(apply_id, 1U, "bundle.a", route));
      ASSERT_TRUE(plan.ok()) << plan.error().message;
      route.active_control_configuration_generation = plan.value().routes.front().generation;
      ASSERT_TRUE(journal.AdoptNativeApplyFacts({
          NativeApplyPublication(apply_id, 1U, "bundle.a", index + 1U),
          plan.value().components, {route}}).ok());
    }
    RequestOwnerEpoch owner("owner.apply.selection");
    const ControllableComponentId component("component.1");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    const auto before = journal.ReadApplyComponentGenerationSelection(owner, component);
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before.value().candidate_generation, ComponentLifecycleGeneration(prior_watermark + 1U));
    EXPECT_EQ(before.value().prior_reservation_generation, 0U);
    const auto reservation = prior_watermark == 0U ? 8U : 9U;
    for (unsigned index = prior_watermark + 1U; index <= reservation; ++index) {
      const auto plan = journal.PlanNativeApply(NativeApplyPlanRequest(
          "apply.abandoned." + std::to_string(index), 1U, "bundle.a", route));
      ASSERT_TRUE(plan.ok()) << plan.error().message;
      EXPECT_EQ(plan.value().components.front().generation, ComponentLifecycleGeneration(index));
    }
    // An actual removal adoption removes current facts while keeping both
    // historical watermark and earlier unconsumed reservations.
    const auto removal = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
        "apply.removal", "bundle.removal", {}, {}, {}, {component}, {}, {route.control_domain_id}));
    ASSERT_TRUE(removal.ok()) << removal.error().message;
    ASSERT_TRUE(journal.AdoptNativeApplyFacts({
        NativeApplyPublication("apply.removal", 1U, "bundle.removal", reservation + 2U),
        {}, {}}).ok());
    const auto selected = journal.ReadApplyComponentGenerationSelection(owner, component);
    ASSERT_TRUE(selected.ok()) << selected.error().message;
    EXPECT_EQ(selected.value().predecessor_generation,
              prior_watermark ? std::optional<ComponentLifecycleGeneration>(ComponentLifecycleGeneration(prior_watermark))
                              : std::nullopt);
    EXPECT_EQ(selected.value().prior_reservation_generation, reservation);
    EXPECT_EQ(selected.value().candidate_generation, ComponentLifecycleGeneration(reservation + 1U));
    const auto current = journal.LoadCurrentComponentLifecycleFact(component);
    ASSERT_TRUE(current.ok());
    EXPECT_EQ(current.value().durable_generation, selected.value().predecessor_generation);
    EXPECT_FALSE(current.value().current_state);
  }
}

TEST(SqliteCCLCJournal, ApplyGenerationSelectionRaceIsRefusedBeforeAcceptance) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
  RequestOwnerEpoch owner("owner.apply.race");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto predicted = ApplyBringUpStatus(journal, owner);
  ASSERT_TRUE(predicted.locally_valid());
  const auto reservation = journal.PlanNativeApply(NativeApplyPlanRequest(
      "apply.parent", 4U, "bundle.a", ActiveConfiguration(ExpectedDomain())));
  ASSERT_TRUE(reservation.ok()) << reservation.error().message;
  const auto refused = journal.AcceptOrReplayApplyComponentBringUp(
      owner, predicted.accepted_intent, predicted.acceptance, predicted,
      predicted.apply_bring_up->original_request_payload_digest);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_journal_generation_conflict");
  EXPECT_FALSE(journal.LoadRetainedLifecycleStatus(predicted.acceptance.lifecycle_transaction_id).value());
  EXPECT_TRUE(journal.LoadComponentsWithUnsettledLifecycle().value().empty());
  const auto actual = journal.ReadApplyComponentGenerationSelection(
      owner, predicted.accepted_intent.controllable_component_id);
  ASSERT_TRUE(actual.ok());
  EXPECT_EQ(actual.value().predecessor_generation,
            predicted.accepted_intent.apply_generation_selection->predecessor_generation);
  EXPECT_EQ(actual.value().candidate_generation, ComponentLifecycleGeneration(3U));
  EXPECT_EQ(actual.value().prior_reservation_generation, 2U);
}

TEST(SqliteCCLCJournal, FreshApplyGenerationRequiresClosedSelectionAndRoundTripsExplicitAbsence) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
  RequestOwnerEpoch owner("owner.apply.closed");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto selected = ApplyBringUpStatus(journal, owner);
  const auto selected_bytes = EncodeComponentLifecycleIntent(selected.accepted_intent);
  EXPECT_EQ(DecodeComponentLifecycleIntent(selected_bytes), selected.accepted_intent);
  auto legacy = selected;
  legacy.accepted_intent.apply_generation_selection.reset();
  auto& evidence = *legacy.apply_bring_up;
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      legacy.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id, evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch, evidence.parent_authority.parent);
  ASSERT_TRUE(legacy.locally_valid());
  EXPECT_FALSE(journal.AcceptOrReplayApplyComponentBringUp(owner, legacy.accepted_intent,
      legacy.acceptance, legacy, evidence.original_request_payload_digest).ok());
  EXPECT_FALSE(journal.AcceptOrReplayLifecycle(owner, selected.accepted_intent,
      selected.acceptance, selected).ok());
  auto initial = selected.accepted_intent;
  initial.expected_component_lifecycle_generation = ComponentLifecycleGeneration(0U);
  initial.apply_generation_selection = ApplyComponentGenerationSelectionDTO{
      std::nullopt, ComponentLifecycleGeneration(1U), 0U};
  ASSERT_TRUE(initial.locally_valid());
  EXPECT_EQ(DecodeComponentLifecycleIntent(EncodeComponentLifecycleIntent(initial)), initial);
  auto foreign = initial;
  foreign.apply_generation_selection->prior_reservation_generation = 2U;
  EXPECT_FALSE(foreign.locally_valid());
  initial.apply_generation_selection.reset();
  EXPECT_FALSE(initial.locally_valid());
  EXPECT_FALSE(journal.LoadRetainedLifecycleStatus(selected.acceptance.lifecycle_transaction_id).value());
}

TEST(SqliteCCLCJournal, ApplyBringUpKeepsParentAuthoritySeparateAndReopensObservationOnly) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.apply.child");
  std::optional<ComponentLifecycleStatusDTO> saved;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    saved = ApplyBringUpStatus(journal, owner);
    ASSERT_TRUE(saved->locally_valid());
    const auto& digest = saved->apply_bring_up->original_request_payload_digest;
    // Ordinary submission cannot acquire delegated authority from evidence.
    EXPECT_FALSE(journal.AcceptOrReplayLifecycle(owner, saved->accepted_intent,
                                                 saved->acceptance, *saved).ok());
    const auto accepted = journal.AcceptOrReplayApplyComponentBringUp(
        owner, saved->accepted_intent, saved->acceptance, *saved, digest);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    EXPECT_EQ(accepted.value().status, saved);
    EXPECT_EQ(journal.LoadComponentsWithUnsettledLifecycle().value(),
              std::vector<ControllableComponentId>{saved->accepted_intent.controllable_component_id});
    auto changed = *saved;
    changed.apply_bring_up->parent_authority.parent_claim.claim_generation =
        CellAdmissionClaimGeneration(18U);
    EXPECT_EQ(journal.AcceptOrReplayApplyComponentBringUp(owner, changed.accepted_intent,
        changed.acceptance, changed, digest).value().status, saved);
    const auto conflict = journal.ReadApplyComponentBringUpRequestResult(
        saved->accepted_intent, ContentDigest("sha256:" + std::string(64U, 'f')));
    ASSERT_TRUE(conflict.ok());
    ASSERT_TRUE(conflict.value());
    EXPECT_EQ(conflict.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);
    EXPECT_EQ(journal.ReadLifecycleRequestResult(saved->accepted_intent).value()->replay_status,
              RequestReplayStatusDTO::kRequestIdConflict);
  }
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  RequestOwnerEpoch replacement("replacement.apply.child");
  const auto recovery = BeginTestOwnerEpoch(reopened, replacement, 200,
                                           kDefaultCCLCExactResultHistory);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 1U);
  EXPECT_EQ(recovery.value().interrupted_transactions.front(), *saved);
  EXPECT_TRUE(recovery.value().cell_claims.empty());
  EXPECT_TRUE(recovery.value().unresolved_effect_attempts.empty());
  EXPECT_EQ(reopened.ReadApplyComponentBringUpRequestResult(saved->accepted_intent,
      saved->apply_bring_up->original_request_payload_digest).value()->status, saved);
  EXPECT_EQ(reopened.LoadComponentsWithUnsettledLifecycle().value(),
            std::vector<ControllableComponentId>{saved->accepted_intent.controllable_component_id});
}

TEST(SqliteCCLCJournal, ApplyInitialInstallationRecordsActualDomainsOnceWithoutPredecessorAggregate) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.apply.initial");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  auto accepted = ApplyBringUpStatus(journal, owner);
  auto& evidence = *accepted.apply_bring_up;
  accepted.previous_state.reset();
  accepted.current_state.reset();
  auto& expected = accepted.accepted_intent.expected_control_domains.front();
  expected.controller().expected_active_control_configuration = {
      ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
  auto second_expected = expected;
  second_expected.control_domain_id = ControlDomainId("domain.2");
  second_expected.controller().expected_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  accepted.accepted_intent.expected_control_domains.push_back(second_expected);
  accepted.domains.push_back(accepted.domains.front());
  for (std::size_t index = 0U; index < accepted.domains.size(); ++index) {
    auto& domain = accepted.domains[index];
    domain.control_domain_id = accepted.accepted_intent.expected_control_domains[index].control_domain_id;
    domain.expected_domain = accepted.accepted_intent.expected_control_domains[index];
    domain.current_scope.reset();
    domain.resolved_provider_occurrence.reset();
    domain.previous_state.reset();
    domain.current_state.reset();
  }
  evidence.selected_scope.domains.front().predecessor_control_domain_revision.reset();
  evidence.selected_scope.bindings.front().predecessor_control_binding_revision.reset();
  evidence.selected_scope.domains.push_back({second_expected.control_domain_id, std::nullopt,
                                           second_expected.control_domain_revision});
  evidence.selected_scope.bindings.push_back({ControlBindingId("binding.2"), std::nullopt,
      second_expected.controller().expected_control_bindings.front().control_binding_revision});
  evidence.selected_scope.resources.push_back(ResourceId("resource.2"));
  evidence.parent_authority.held_fence_query.expected_scope = evidence.selected_scope;
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      accepted.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id, evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch, evidence.parent_authority.parent);
  ASSERT_TRUE(accepted.locally_valid());
  ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, accepted.accepted_intent,
      accepted.acceptance, accepted, evidence.original_request_payload_digest).ok());
  auto installed = accepted;
  installed.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  installed.status_sequence += 1U;
  installed.latest_updated_at_ns += 1;
  for (std::size_t index = 0U; index < installed.domains.size(); ++index) {
    auto& domain = installed.domains[index];
    const auto& wanted = *domain.expected_domain;
    auto scope = CurrentDisableScope();
    scope.control_domain_id = wanted.control_domain_id;
    scope.controller().current_control_bindings = wanted.controller().expected_control_bindings;
    scope.controller().active_control_configuration.reset();
    scope.controller().current_resources = {ResourceId(index == 0U ? "resource.1" : "resource.2")};
    domain.current_scope = scope;
    auto provider = CurrentProviderOccurrence();
    provider.control_domain_id = wanted.control_domain_id;
    provider.provider_instance_id = ProviderInstanceId(index == 0U ? "installed.1" : "installed.2");
    domain.resolved_provider_occurrence = provider;
    domain.previous_state = index == 0U ? ComponentLifecycleStateDTO::kDeclared
                                     : ComponentLifecycleStateDTO::kPrepared;
    domain.current_state = domain.previous_state;
  }
  ASSERT_TRUE(installed.locally_valid());
  auto foreign = installed;
  foreign.accepted_intent.reason = "foreign original delegated child";
  auto& foreign_evidence = *foreign.apply_bring_up;
  foreign_evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      foreign.accepted_intent, foreign_evidence.candidate_projection_revision,
      foreign_evidence.candidate_retained_bundle_digest, foreign_evidence.candidate_retained_bundle_bytes,
      foreign_evidence.original_execution_host.runtime_unit_id,
      foreign_evidence.original_execution_host.runtime_epoch,
      foreign_evidence.parent_authority.held_fence_query.manager_runtime_epoch,
      foreign_evidence.parent_authority.parent);
  ASSERT_NE(foreign_evidence.original_request_payload_digest,
            installed.apply_bring_up->original_request_payload_digest);
  ASSERT_TRUE(foreign.locally_valid());
  const auto foreign_recorded = journal.RecordLifecycleStatus(owner, foreign);
  ASSERT_FALSE(foreign_recorded.ok());
  EXPECT_EQ(foreign_recorded.error().code, "component_lifecycle_journal_status_regressed");
  auto invented = installed;
  invented.previous_state = ComponentLifecycleStateDTO::kDeclared;
  EXPECT_FALSE(journal.RecordLifecycleStatus(owner, invented).ok());
  const auto recorded = journal.RecordLifecycleStatus(owner, installed);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  const auto retained = journal.LoadRetainedLifecycleStatus(installed.acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  EXPECT_EQ(retained.value(), installed);
  EXPECT_FALSE(retained.value()->previous_state);
  EXPECT_FALSE(retained.value()->current_state);
  auto second_installation = installed;
  second_installation.status_sequence += 1U;
  second_installation.latest_updated_at_ns += 1;
  second_installation.domains.front().resolved_provider_occurrence->provider_instance_id =
      ProviderInstanceId("another.installation");
  ASSERT_TRUE(second_installation.locally_valid());
  EXPECT_FALSE(journal.RecordLifecycleStatus(owner, second_installation).ok());
  // Later ordinary observation updates retain the installed occurrences.
  installed.status_sequence += 1U;
  installed.latest_updated_at_ns += 1;
  for (auto& domain : installed.domains) {
    domain.current_state = ComponentLifecycleStateDTO::kActive;
    domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
    domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  }
  installed.current_state = ComponentLifecycleStateDTO::kActive;
  ASSERT_TRUE(installed.locally_valid());
  ASSERT_TRUE(journal.RecordLifecycleStatus(owner, installed).ok());
  EXPECT_FALSE(installed.previous_state);
}

TEST(SqliteCCLCJournal, ApplyRecoveryInstallationPreservesHistoryAndRejectsLaterProviderChange) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.apply.recovery");
  ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  ASSERT_TRUE(ContainSelectedIncident(journal, SelectedStatus(owner)).value());
  const auto accepted = ApplyBringUpStatus(journal, owner, true);
  ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, accepted.accepted_intent,
      accepted.acceptance, accepted, accepted.apply_bring_up->original_request_payload_digest).ok());
  auto installed = accepted;
  installed.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  installed.status_sequence += 1U;
  installed.latest_updated_at_ns += 1;
  installed.domains.front().resolved_provider_occurrence->provider_instance_id = ProviderInstanceId("installed.new");
  installed.domains.front().current_state = ComponentLifecycleStateDTO::kDeclared;
  installed.domains.front().current_scope->controller().active_control_configuration.reset();
  ASSERT_TRUE(installed.locally_valid());
  ASSERT_TRUE(journal.RecordLifecycleStatus(owner, installed).ok());
  EXPECT_EQ(installed.previous_state, accepted.previous_state);
  EXPECT_EQ(installed.domains.front().previous_state, accepted.domains.front().previous_state);
  installed.status_sequence += 1U;
  installed.latest_updated_at_ns += 1;
  installed.domains.front().resolved_provider_occurrence->provider_instance_id = ProviderInstanceId("installed.again");
  ASSERT_TRUE(installed.locally_valid());
  EXPECT_FALSE(journal.RecordLifecycleStatus(owner, installed).ok());
}

TEST(SqliteCCLCJournal, ApplyInstallationCannotBeReconstructedByReplacementOwner) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.apply.original");
  ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto accepted = ApplyBringUpStatus(journal, owner);
  ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, accepted.accepted_intent,
      accepted.acceptance, accepted, accepted.apply_bring_up->original_request_payload_digest).ok());
  RequestOwnerEpoch replacement("owner.apply.replacement");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, replacement, 200, kDefaultCCLCExactResultHistory).ok());
  auto installed = accepted;
  installed.status_owner_epoch = replacement;
  installed.remaining_duration_ns = 0;
  installed.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  installed.status_sequence += 1U;
  installed.latest_updated_at_ns = 201;
  installed.domains.front().resolved_provider_occurrence->provider_instance_id = ProviderInstanceId("installed.new");
  ASSERT_TRUE(installed.locally_valid());
  EXPECT_FALSE(journal.RecordLifecycleStatus(replacement, installed).ok());
  EXPECT_EQ(journal.LoadRetainedLifecycleStatus(accepted.acceptance.lifecycle_transaction_id).value(), accepted);
}

TEST(SqliteCCLCJournal, ApplyBringUpRequiresCurrentIncidentAndOriginalParentExpectation) {
  for (unsigned scenario = 0U; scenario != 5U; ++scenario) {
    SCOPED_TRACE(scenario);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    RequestOwnerEpoch owner("owner.apply.child");
    ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    auto status = ApplyBringUpStatus(journal, owner, scenario != 1U);
    const auto predecessor = SelectedStatus(owner);
    if (scenario != 0U) {
      ASSERT_TRUE(ContainSelectedIncident(journal, predecessor).value());
    }
    if (scenario == 2U) status.apply_bring_up->recovery->incident_id = "foreign.incident";
    if (scenario == 3U) status.apply_bring_up->recovery->containment_stop_generation =
        StopGeneration(8U);
    if (scenario == 4U) {
      auto& parent_claim = status.apply_bring_up->parent_authority.parent_claim;
      parent_claim.intent.expected_admission_generation = AdmissionGeneration(2U);
      parent_claim.acquired_admission_generation = AdmissionGeneration(2U);
    }
    ASSERT_TRUE(status.locally_valid());
    EXPECT_FALSE(journal.AcceptOrReplayApplyComponentBringUp(owner, status.accepted_intent,
        status.acceptance, status, status.apply_bring_up->original_request_payload_digest).ok());
    EXPECT_FALSE(journal.LoadRetainedLifecycleStatus(status.acceptance.lifecycle_transaction_id)
                     .value());
    EXPECT_FALSE(journal.ReadApplyComponentBringUpRequestResult(status.accepted_intent,
        status.apply_bring_up->original_request_payload_digest).value());
  }
}

TEST(SqliteCCLCJournal, ApplyBringUpAcceptsExactContainedRecoveryWithoutChildClaim) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.apply.child");
  ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto predecessor = SelectedStatus(owner);
  ASSERT_TRUE(ContainSelectedIncident(journal, predecessor).value());
  const auto status = ApplyBringUpStatus(journal, owner, true);
  ASSERT_TRUE(status.locally_valid());
  const auto accepted = journal.AcceptOrReplayApplyComponentBringUp(owner, status.accepted_intent,
      status.acceptance, status, status.apply_bring_up->original_request_payload_digest);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().status, status);
  EXPECT_EQ(journal.LoadOpenComponentIncident(status.accepted_intent.controllable_component_id)
                .value()->incident.incident_id,
            predecessor.selected_bring_up->incident_id);
}

TEST(SqliteCCLCJournal, SelectedAcceptanceRequiresCurrentIncidentAndExactUnreleasedClaim) {
  for (unsigned scenario = 0; scenario != 5; ++scenario) {
    SCOPED_TRACE(scenario);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    RequestOwnerEpoch owner("owner.selected");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    auto status = SelectedStatus(owner);
    ASSERT_TRUE(status.locally_valid());
    ASSERT_TRUE(SeedSelectedClaim(journal, status).ok());
    if (scenario != 0U) {
      ASSERT_TRUE(ContainSelectedIncident(journal, status).value());
    }
    if (scenario == 1U) status.selected_bring_up->incident_id = "incident.foreign";
    if (scenario == 2U) status.selected_bring_up->containment_stop_generation = StopGeneration(8U);
    if (scenario == 3U)
      status.selected_bring_up->selected_claim.claim_generation = CellAdmissionClaimGeneration(16U);
    if (scenario == 4U) {
      const auto released =
          ReleaseUnacceptedTestClaim(journal, owner, status.selected_bring_up->selected_claim);
      ASSERT_TRUE(released.ok()) << released.error().message;
      const auto replay = journal.ReadSelectedComponentBringUpRequestResult(
          status.accepted_intent, status.selected_bring_up->original_request_payload_digest);
      ASSERT_TRUE(replay.ok());
      ASSERT_TRUE(replay.value());
      EXPECT_TRUE(replay.value()->rejection);
      EXPECT_EQ(journal
                    .ReadSelectedComponentBringUpRequestResult(
                        status.accepted_intent, status.selected_bring_up->offer_digest)
                    .value()
                    ->replay_status,
                RequestReplayStatusDTO::kRequestIdConflict);
    }
    const auto accepted = journal.AcceptOrReplaySelectedComponentBringUp(
        owner, status.accepted_intent, status.acceptance, status,
        status.selected_bring_up->original_request_payload_digest);
    if (scenario == 4U) {
      ASSERT_TRUE(accepted.ok());
      EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
      ASSERT_TRUE(accepted.value().rejection);
      EXPECT_EQ(accepted.value().rejection->code, "cclc_request_recovered_unstarted");
      EXPECT_FALSE(accepted.value().acceptance);
      EXPECT_FALSE(accepted.value().status);
    } else {
      EXPECT_FALSE(accepted.ok());
    }
    EXPECT_FALSE(
        journal.LoadRetainedLifecycleStatus(status.acceptance.lifecycle_transaction_id).value());
  }
}

TEST(SqliteCCLCJournal, SelectedTerminalSettlementRequiresItsExactFullClaim) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  RequestOwnerEpoch owner("owner.selected");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto status = SelectedStatus(owner);
  ASSERT_TRUE(SeedSelectedClaim(journal, status).ok());
  ASSERT_TRUE(ContainSelectedIncident(journal, status).value());
  ASSERT_TRUE(journal
                  .AcceptOrReplaySelectedComponentBringUp(
                      owner, status.accepted_intent, status.acceptance, status,
                      status.selected_bring_up->original_request_payload_digest)
                  .ok());
  const auto terminal = ReplacementTerminalKnownNoEffect(status, owner);
  ASSERT_TRUE(terminal.locally_valid());
  const auto& claim = status.selected_bring_up->selected_claim;
  const CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
       ContentDigest(detail::ComponentLifecycleStatusReleasePreconditionDigest(terminal))}};
  const auto recorded = journal.RecordLifecycleTerminalAndClaimRelease(owner, terminal, release);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(
      ValidateCCLCCellAdmissionSettlementReadOnly(path, CellId("cell.1"), claim, release).ok());
  auto foreign = claim;
  foreign.intent.scope.configurations.front().active_configuration_id =
      ActiveControlConfigurationId("foreign.configuration");
  EXPECT_FALSE(
      ValidateCCLCCellAdmissionSettlementReadOnly(path, CellId("cell.1"), foreign, release).ok());
  journal.Close();
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  ASSERT_EQ(
      reopened.LoadRetainedLifecycleStatus(status.acceptance.lifecycle_transaction_id).value(),
      terminal);
}

TEST(SqliteCCLCJournal, SelectedAttemptRecoveryRetainsOriginalRequestIdentityBeforeClaimDeletion) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.selected");
  ComponentLifecycleStatusDTO status = SelectedStatus(owner);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    status = SelectedStatus(owner);
    const auto& evidence = *status.selected_bring_up;
    ASSERT_NE(evidence.offer_digest, evidence.original_request_payload_digest);
    EXPECT_FALSE(journal.RecordCellClaimAttempt(owner, evidence.selected_claim.intent).ok());
    ASSERT_TRUE(journal
                    .RecordSelectedCellClaimAttempt(owner, evidence.selected_claim.intent,
                                                    evidence.original_request_payload_digest)
                    .ok());
    auto foreign_claim = evidence.selected_claim;
    foreign_claim.intent.request_payload_digest = ContentDigest("sha256:" + std::string(64U, 'd'));
    EXPECT_FALSE(journal.RecordCellClaimAcquired(owner, foreign_claim).ok());
    journal.Close();
  }
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  RequestOwnerEpoch replacement("owner.replacement");
  const auto recovered =
      BeginTestOwnerEpoch(reopened, replacement, 200, kDefaultCCLCExactResultHistory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().cell_claims.size(), 1U);
  const auto& attempt = recovered.value().cell_claims.front();
  EXPECT_FALSE(attempt.claim);
  EXPECT_EQ(attempt.intent, status.selected_bring_up->selected_claim.intent);
  EXPECT_EQ(attempt.original_request_payload_digest,
            status.selected_bring_up->original_request_payload_digest);
  ASSERT_TRUE(reopened.RecordCellClaimNotAcquired(replacement, attempt.intent).ok());
  const auto result = reopened.ReadSelectedComponentBringUpRequestResult(
      status.accepted_intent, status.selected_bring_up->original_request_payload_digest);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value());
  ASSERT_TRUE(result.value()->rejection);
  EXPECT_EQ(result.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(result.value()->rejection->details.at("effect"), "not_started");
  EXPECT_FALSE(result.value()->acceptance);
  EXPECT_FALSE(result.value()->status);
  EXPECT_FALSE(
      reopened.LoadRetainedLifecycleStatus(status.acceptance.lifecycle_transaction_id).value());
  EXPECT_EQ(reopened
                .ReadSelectedComponentBringUpRequestResult(status.accepted_intent,
                                                           status.selected_bring_up->offer_digest)
                .value()
                ->replay_status,
            RequestReplayStatusDTO::kRequestIdConflict);
}

TEST(SqliteCCLCJournal, SelectedRejectionReplayAndExpirationUseCallerDigestNotOfferProof) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
  RequestOwnerEpoch owner("owner.selected");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto status = SelectedStatus(owner);
  const auto& stable = status.selected_bring_up->original_request_payload_digest;
  const ErrorDTO refused{
      "selected_test_refusal", "candidate does not qualify", {{"effect", "not_started"}}};
  const auto result = journal.RecordSelectedComponentBringUpRejection(
      owner, status.accepted_intent, stable, refused, std::nullopt);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().rejection, refused);
  EXPECT_FALSE(result.value().acceptance);
  EXPECT_FALSE(result.value().status);
  ASSERT_EQ(journal.ReadSelectedComponentBringUpRequestResult(status.accepted_intent, stable)
                .value()
                ->replay_status,
            RequestReplayStatusDTO::kReplayedExact);
  auto next = status.accepted_intent;
  next.request_context.request_id = RequestId("selected.request.2");
  ASSERT_TRUE(
      journal.RecordSelectedComponentBringUpRejection(owner, next, stable, refused, std::nullopt)
          .ok());
  EXPECT_EQ(journal.ReadSelectedComponentBringUpRequestResult(status.accepted_intent, stable)
                .value()
                ->replay_status,
            RequestReplayStatusDTO::kRequestResultExpired);
  EXPECT_EQ(journal
                .ReadSelectedComponentBringUpRequestResult(status.accepted_intent,
                                                           status.selected_bring_up->offer_digest)
                .value()
                ->replay_status,
            RequestReplayStatusDTO::kRequestIdConflict);
  journal.Close();
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U);
  EXPECT_EQ(reopened.ReadSelectedComponentBringUpRequestResult(status.accepted_intent, stable)
                .value()
                ->replay_status,
            RequestReplayStatusDTO::kRequestResultExpired);
  const auto replay = reopened.ReadSelectedComponentBringUpRequestResult(next, stable);
  ASSERT_TRUE(replay.ok());
  ASSERT_TRUE(replay.value());
  EXPECT_EQ(replay.value()->rejection, refused);
}

TEST(SqliteCCLCJournal, SelectedAcceptedCallerDigestCannotCrossStoredOwnerProof) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  RequestOwnerEpoch owner("owner.selected");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto status = SelectedStatus(owner);
  ASSERT_TRUE(SeedSelectedClaim(journal, status).ok());
  ASSERT_TRUE(ContainSelectedIncident(journal, status).value());
  const auto& evidence = *status.selected_bring_up;
  EXPECT_FALSE(journal
                   .AcceptOrReplaySelectedComponentBringUp(owner, status.accepted_intent,
                                                           status.acceptance, status,
                                                           evidence.offer_digest)
                   .ok());
  auto foreign = status;
  foreign.selected_bring_up->original_request_payload_digest =
      ContentDigest("sha256:" + std::string(64U, 'd'));
  ASSERT_TRUE(foreign.locally_valid());
  EXPECT_FALSE(journal
                   .AcceptOrReplaySelectedComponentBringUp(
                       owner, foreign.accepted_intent, foreign.acceptance, foreign,
                       foreign.selected_bring_up->original_request_payload_digest)
                   .ok());
  ASSERT_TRUE(journal
                  .AcceptOrReplaySelectedComponentBringUp(owner, status.accepted_intent,
                                                          status.acceptance, status,
                                                          evidence.original_request_payload_digest)
                  .ok());
  journal.Close();
  ExecRaw(path, "UPDATE cclc_request_memberships SET semantic_request_digest='" +
                    evidence.offer_digest.value() + "' WHERE request_id='" +
                    status.accepted_intent.request_context.request_id.value() + "'");
  EXPECT_THROW((SqliteCCLCJournal(path, CellId("cell.1"))), std::runtime_error);
}

TEST(SqliteCCLCJournal, SelectedPendingRejectionMustMatchBothCallerAndOwnerIntent) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.selected");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto status = SelectedStatus(owner);
  const auto& evidence = *status.selected_bring_up;
  ASSERT_TRUE(journal
                  .RecordSelectedCellClaimAttempt(owner, evidence.selected_claim.intent,
                                                  evidence.original_request_payload_digest)
                  .ok());
  const ErrorDTO error{
      "selected_test_refusal", "candidate does not qualify", {{"effect", "not_started"}}};
  EXPECT_FALSE(journal
                   .RecordSelectedComponentBringUpRejection(owner, status.accepted_intent,
                                                            evidence.offer_digest, error,
                                                            evidence.selected_claim.intent)
                   .ok());
  const auto rejected = journal.RecordSelectedComponentBringUpRejection(
      owner, status.accepted_intent, evidence.original_request_payload_digest, error,
      evidence.selected_claim.intent);
  ASSERT_TRUE(rejected.ok()) << rejected.error().message;
  EXPECT_EQ(rejected.value().rejection, error);
  EXPECT_FALSE(rejected.value().acceptance);
  EXPECT_FALSE(rejected.value().status);
  EXPECT_EQ(journal
                .ReadSelectedComponentBringUpRequestResult(status.accepted_intent,
                                                           evidence.original_request_payload_digest)
                .value()
                ->rejection,
            error);
}

TEST(SqliteCCLCJournal, SelectedUnacceptedReleaseRefusesCrossingClaimBlobBeforeRejection) {
  for (unsigned scenario = 0U; scenario < 3U; ++scenario) {
    SCOPED_TRACE(scenario);
    ScopedTemporaryDirectory temporary;
    const auto path = JournalPath(temporary);
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    RequestOwnerEpoch owner("owner.selected");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    const auto status = SelectedStatus(owner);
    ASSERT_TRUE(SeedSelectedClaim(journal, status).ok());
    const auto& evidence = *status.selected_bring_up;
    auto changed = evidence.selected_claim.intent;
    if (scenario == 0U) {
      changed.owner_epoch = RequestOwnerEpoch("owner.foreign");
      changed.intervention_evidence->offering_owner_epoch = changed.owner_epoch;
    } else if (scenario == 1U) {
      changed.request_id = RequestId("foreign.request");
    } else {
      changed.expected_admission_generation = AdmissionGeneration(2U);
    }
    const auto blob = detail::EncodeCCLCSelectedBringUpClaimIntent(
        {changed, ContentDigest("sha256:" + std::string(64U, 'd'))});
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* update = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(raw, "UPDATE cclc_lifecycle_cell_claims SET recovery_configuration=?",
                           -1, &update, nullptr),
        SQLITE_OK);
    ASSERT_EQ(
        sqlite3_bind_blob(update, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(update), SQLITE_DONE);
    ASSERT_EQ(sqlite3_finalize(update), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    const auto rejected = ReleaseUnacceptedTestClaim(journal, owner, evidence.selected_claim);
    EXPECT_FALSE(rejected.ok());
    if (!rejected.ok()) {
      EXPECT_EQ(rejected.error().code, "component_lifecycle_journal_corrupt");
    }
    const auto result = journal.ReadSelectedComponentBringUpRequestResult(
        status.accepted_intent, evidence.original_request_payload_digest);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value());
    EXPECT_FALSE(
        journal.LoadRetainedLifecycleStatus(status.acceptance.lifecycle_transaction_id).value());
  }
}

TEST(SqliteCCLCJournal, SelectedClaimBlobCannotOverrideSqlIdentityOnRecovery) {
  for (const auto& column :
       {"owner_epoch", "request_id", "request_payload_digest", "expected_admission_generation"}) {
    SCOPED_TRACE(column);
    ScopedTemporaryDirectory temporary;
    const auto path = JournalPath(temporary);
    RequestOwnerEpoch owner("owner.selected");
    RequestOwnerEpoch foreign_owner("owner.foreign");
    {
      SqliteCCLCJournal journal(path, CellId("cell.1"));
      ASSERT_TRUE(
          BeginTestOwnerEpoch(journal, foreign_owner, 0, kDefaultCCLCExactResultHistory).ok());
      ASSERT_TRUE(journal.RecordLifecycleRejection(
                         foreign_owner, Intent("foreign.request", foreign_owner, 1U),
                         ErrorDTO{"test_rejection", "retain the foreign owner namespace",
                                  {{"effect", "not_started"}}},
                         std::nullopt).ok());
      ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
      const auto status = SelectedStatus(owner);
      ASSERT_TRUE(SeedSelectedClaim(journal, status).ok());
      journal.Close();
    }
    const std::string value =
        std::string(column) == "expected_admission_generation"
            ? "2"
            : (std::string(column) == "owner_epoch" ? "'" + foreign_owner.value() + "'"
                                                    : "'foreign'");
    ExecRaw(path, "UPDATE cclc_lifecycle_cell_claims SET " + std::string(column) + "=" + value);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* foreign_keys = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "PRAGMA foreign_key_check", -1, &foreign_keys, nullptr),
              SQLITE_OK);
    EXPECT_EQ(sqlite3_step(foreign_keys), SQLITE_DONE);
    ASSERT_EQ(sqlite3_finalize(foreign_keys), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    SqliteCCLCJournal reopened(path, CellId("cell.1"));
    const auto recovery = reopened.BeginOwnerEpoch(RequestOwnerEpoch("replacement"), 200,
                                                   kDefaultCCLCExactResultHistory);
    EXPECT_FALSE(recovery.ok());
    if (!recovery.ok()) {
      EXPECT_EQ(recovery.error().code, "component_lifecycle_journal_corrupt");
    }
  }
}

/// The operator's Recover of component.1 (an ordinary lifecycle intent).
[[nodiscard]] ComponentLifecycleIntentDTO RecoverIntent(std::string request_id,
                                                        const RequestOwnerEpoch& owner_epoch) {
  auto result = Intent(std::move(request_id), owner_epoch, 1U, "recover");
  result.intent = ComponentLifecycleIntentKindDTO::kRecover;
  result.request_context.source_label = "composer";
  return result;
}

/// Runs one statement on a second connection (fault injection only).
void ExecRaw(const std::filesystem::path& path, const std::string& sql) {
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << sql;
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

[[nodiscard]] std::int64_t RawScalar(const std::filesystem::path& path, const char* sql) {
  sqlite3* database{};
  if (sqlite3_open(path.c_str(), &database) != SQLITE_OK)
    throw std::runtime_error("raw open failed");
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK ||
      sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_close(database);
    throw std::runtime_error("raw scalar failed");
  }
  const auto value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return value;
}

TEST(SqliteCCLCJournal, QuiescenceObservationRequiresTheExactActiveOwnerWithoutMintingOrWriting) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
  const auto absent = journal.ReadQuiescentOwner(RequestOwnerEpoch("absent.owner"));
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().code, "component_lifecycle_journal_owner_epoch_stale");
  EXPECT_EQ(RawScalar(path, "SELECT count(*) FROM cclc_owner_epochs"), 0);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U);
  ASSERT_TRUE(begun.ok());
  const auto owner = begun.value().request_owner_epoch;
  const auto sequence = RawScalar(path, "SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs'");
  const auto foreign = journal.ReadQuiescentOwner(RequestOwnerEpoch("foreign.owner"));
  ASSERT_FALSE(foreign.ok());
  EXPECT_EQ(foreign.error().code, "component_lifecycle_journal_owner_epoch_stale");
  ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  EXPECT_EQ(RawScalar(path, "SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs'"), sequence);
  EXPECT_EQ(RawScalar(path, "SELECT count(*) FROM cclc_owner_epochs WHERE active=1"), 1);
  EXPECT_EQ(RawScalar(path, "SELECT count(*) FROM cclc_request_memberships"), 0);
  const auto invalid = journal.ReadQuiescentOwner(RequestOwnerEpoch(""));
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code, "component_lifecycle_journal_query_invalid");
  journal.Close();
  const auto closed = journal.ReadQuiescentOwner(owner);
  ASSERT_FALSE(closed.ok());
  EXPECT_EQ(closed.error().code, "component_lifecycle_journal_closed");
  SqliteCCLCJournal reopened(path, CellId("cell.1"), 1U);
  ASSERT_TRUE(reopened.ReadQuiescentOwner(owner).ok());
  EXPECT_EQ(RawScalar(path, "SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs'"), sequence);
}

TEST(SqliteCCLCJournal, QuiescenceObservationWaitsForActualLifecycleAndEffectSettlement) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 8U);
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 8U).ok());
  ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  const auto intent = Intent("request.1", owner, 3U);
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  auto status = InitialStatus(intent, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
  ASSERT_TRUE(journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status).ok());
  const auto expect_unsettled = [&] {
    const auto read = journal.ReadQuiescentOwner(owner);
    ASSERT_FALSE(read.ok());
    EXPECT_EQ(read.error().code, "component_lifecycle_checkpoint_not_quiescent");
  };
  expect_unsettled();
  const auto primitive = PrimitiveIntent(status);
  status = PreparedEffectStatus(status);
  ASSERT_TRUE(journal.RecordEffectPrepared(owner, PreparedPrimitiveEffect(primitive), status).ok());
  expect_unsettled();
  const auto result = PrimitiveSuccess(primitive);
  status = CompletedEffectStatus(status, result);
  ASSERT_TRUE(journal.RecordEffectCompleted(owner, CompletedPrimitiveEffect(primitive, result), status).ok());
  expect_unsettled();
  const auto terminal = TerminalSuccess(status, 5U);
  ASSERT_TRUE(terminal.locally_valid());
  ASSERT_TRUE(RecordSettledTerminal(journal, owner, terminal).ok());
  ASSERT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  const auto retained = journal.LoadRetainedLifecycleStatus(acceptance.lifecycle_transaction_id);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value());
  EXPECT_EQ(*retained.value(), terminal);
}

TEST(SqliteCCLCJournal, QuiescenceObservationPreservesStorageReadFailure) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"), 1U);
  const auto begun = journal.BeginOwnerEpoch(RequestOwnerEpoch("seed"), 1, 2U);
  ASSERT_TRUE(begun.ok());
  ASSERT_TRUE(journal.ReadQuiescentOwner(begun.value().request_owner_epoch).ok());
  ExecRaw(path, "DROP TABLE cclc_route_effect_attempts");
  const auto failed = journal.ReadQuiescentOwner(begun.value().request_owner_epoch);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "component_lifecycle_journal_storage_failure");
  EXPECT_EQ(RawScalar(path, "SELECT count(*) FROM cclc_owner_epochs WHERE active=1"), 1);
}

TEST(SqliteCCLCJournal, OpenComponentIncidentQueryIncludesNoncurrentOwnersAndRequiresAllRecoveries) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  // BeginTestOwnerEpoch replaces the seed with the actual minted owner.
  RequestOwnerEpoch owner("owner.1");
  const ControllableComponentId first("component.1");
  const ControllableComponentId second("component.removed");
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    ASSERT_TRUE(journal.HasOpenIncident().ok());
    EXPECT_FALSE(journal.HasOpenIncident().value());
    ASSERT_TRUE(journal.OpenComponentIncident(
        TestIncident("incident.1", first, LifecycleBlockingCauseClassDTO::kFault, 100), 0U).ok());
    ASSERT_TRUE(journal.OpenComponentIncident(
        TestIncident("incident.removed", second, LifecycleBlockingCauseClassDTO::kFault, 100), 0U).ok());
    // Neither owner is in current Apply inventory; durable incidents still close admission.
    EXPECT_EQ(RawScalar(path, "SELECT count(*) FROM lifecycle_component_current_facts"), 0);
    // Pending incident rows do not emit history until containment is recorded.
    const auto events_before = RawScalar(path, "SELECT count(*) FROM cclc_component_incident_events");
    EXPECT_EQ(events_before, 0);
    EXPECT_TRUE(journal.HasOpenIncident().value());
    EXPECT_EQ(RawScalar(path, "SELECT count(*) FROM cclc_component_incident_events"), events_before);
    const auto recovered = journal.RecoverComponentIncident(
        owner, RecoverIntent("recover.first", owner), "operator", 200);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_TRUE(recovered.value());
    EXPECT_TRUE(journal.HasOpenIncident().value());
  }
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  EXPECT_TRUE(reopened.HasOpenIncident().value());
  auto recover = RecoverIntent("recover.removed", owner);
  recover.controllable_component_id = second;
  const auto recovered = reopened.RecoverComponentIncident(owner, recover, "operator", 300);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_TRUE(recovered.value());
  EXPECT_FALSE(reopened.HasOpenIncident().value());
  reopened.Close();
  const auto closed = reopened.HasOpenIncident();
  ASSERT_FALSE(closed.ok());
  EXPECT_EQ(closed.error().code, "component_lifecycle_journal_closed");
}

TEST(SqliteCCLCJournal, OpenComponentIncidentQueryPreservesStorageFailure) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  ExecRaw(path, "DROP TABLE cclc_component_incidents");
  const auto failed = journal.HasOpenIncident();
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "component_lifecycle_journal_storage_failure");
}

TEST(SqliteCCLCJournal, RefusesPreDiagnosticIncidentFormatWithoutChangingStoredBytes) {
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("component.1");
  // Literal v1 bytes from the pre-diagnostic codec: no diagnostic key and the
  // original containment representation. Do not regenerate with today's codec.
  const std::string original_record =
      "{\"acknowledged\":false,\"acknowledged_at_ns\":null,\"acknowledged_by\":null,"
      "\"acknowledgement_request_id\":null,\"active_operation_ids\":[\"operation.1\"],"
      "\"cause_class\":\"fault\",\"containment_error\":null,\"containment_state\":\"requested\","
      "\"controllable_component_id\":\"component.1\",\"detected_at_ns\":100,"
      "\"event_kind\":\"component_incident\",\"incident_id\":\"incident.legacy\","
      "\"last_good_observation\":null,\"program_run_id\":\"program.1\",\"providers\":[],"
      "\"recovery_policy\":{\"acknowledgement_required\":true,\"physical_action_required\":true,"
      "\"provider_restart_after_acknowledgement\":false,"
      "\"recovery_steps\":\"physical_action_if_required,clear_error,ready\"},"
      "\"schema_version\":\"manyforge.component_incident_event.v1\","
      "\"stop\":{\"stop_generation\":7,\"stop_id\":\"stop.legacy\"},"
      "\"summary\":\"Component lost its controller.\","
      "\"timeline\":[{\"at_ns\":100,\"cause_class\":\"fault\",\"vendor_codes\":[\"C2\",\"mode=0\"]}"
      "],"
      "\"vendor_codes\":[\"C2\",\"mode=0\"]}";

  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    auto incident =
        TestIncident("incident.legacy", component, LifecycleBlockingCauseClassDTO::kFault, 100);
    auto old_format = incident;
    old_format.record_json = original_record;
    const auto rejected = journal.OpenComponentIncident(old_format, 0U);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "component_lifecycle_incident_invalid");
    EXPECT_EQ(RawScalar(JournalPath(temporary), "SELECT count(*) FROM cclc_component_incidents"), 0);
    ASSERT_TRUE(journal.OpenComponentIncident(incident, 0U).ok());
    const auto before_events = journal.LoadComponentIncidentEvents(0U, 8U);
    ASSERT_TRUE(before_events.ok()) << before_events.error().message;
    EXPECT_TRUE(before_events.value().empty());
    const auto rejected_containment = journal.RecordComponentIncidentContainment(
        "incident.legacy", ComponentIncidentContainmentStateDTO::kRequested, original_record, 110);
    ASSERT_FALSE(rejected_containment.ok());
    EXPECT_EQ(rejected_containment.error().code, "component_lifecycle_incident_invalid");
    const auto rejected_update = journal.UpdateOpenComponentIncident(
        "incident.legacy", LifecycleBlockingCauseClassDTO::kFault, {}, original_record);
    ASSERT_FALSE(rejected_update.ok());
    EXPECT_EQ(rejected_update.error().code, "component_lifecycle_incident_invalid");
    EXPECT_EQ(journal.LoadOpenComponentIncident(component).value()->record_json, incident.record_json);
    const auto unchanged_events = journal.LoadComponentIncidentEvents(0U, 8U);
    ASSERT_TRUE(unchanged_events.ok()) << unchanged_events.error().message;
    EXPECT_EQ(unchanged_events.value(), before_events.value());
    // Obtain real current-format history through the journal's ordinary
    // containment producer before exercising copied obsolete history bytes.
    const auto contained = ComponentIncidentRecordWithContainment(
        incident.record_json, ComponentIncidentContainmentStateDTO::kFailed,
        std::nullopt, std::string("fixture containment unavailable"));
    const auto recorded = journal.RecordComponentIncidentContainment(
        "incident.legacy", ComponentIncidentContainmentStateDTO::kFailed, contained, 110);
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;
    ASSERT_TRUE(recorded.value());
    const auto emitted = journal.LoadComponentIncidentEvents(0U, 8U);
    ASSERT_TRUE(emitted.ok()) << emitted.error().message;
    ASSERT_EQ(emitted.value().size(), 1U);
    EXPECT_EQ(emitted.value().front().incident_id, "incident.legacy");
    EXPECT_EQ(emitted.value().front().payload_json, contained);
    ExecRaw(JournalPath(temporary),
            "UPDATE cclc_component_incidents SET containment_state=" +
                std::to_string(static_cast<int>(ComponentIncidentContainmentStateDTO::kRequested)) +
                ",record_json='" + original_record + "' WHERE incident_id='incident.legacy'");
    // A current candidate cannot silently rewrite an obsolete stored predecessor.
    const auto predecessor_update = journal.UpdateOpenComponentIncident(
        "incident.legacy", LifecycleBlockingCauseClassDTO::kFault, {}, incident.record_json);
    ASSERT_FALSE(predecessor_update.ok());
    EXPECT_EQ(predecessor_update.error().code, "component_lifecycle_journal_corrupt");
    ExecRaw(JournalPath(temporary),
            "UPDATE cclc_component_incident_events SET payload_json='" + original_record +
                "' WHERE incident_id='incident.legacy'");
    journal.Close();
  }
  const auto read_bytes = [&] {
    std::ifstream input(JournalPath(temporary), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto before = read_bytes();
  ASSERT_FALSE(before.empty());
  {
    SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
    const auto loaded = reopened.LoadOpenComponentIncident(component);
    ASSERT_FALSE(loaded.ok());
    EXPECT_EQ(loaded.error().code, "component_lifecycle_journal_corrupt");
    // The routine driver path must refuse the same old format without decoding diagnostics.
    EXPECT_FALSE(reopened.LoadLifecycleTargetFacts(component).ok());
    const auto history = reopened.LoadComponentIncidentEvents(0U, 8U);
    ASSERT_FALSE(history.ok());
    EXPECT_EQ(history.error().code, "component_lifecycle_journal_corrupt");
    reopened.Close();
  }
  EXPECT_EQ(read_bytes(), before);
}

TEST(SqliteCCLCJournal, PrunesOnlyOldestClosedIncidentsAfterCommitAndUnlock) {
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("open.component");
  std::vector<ComponentIncidentPruneNotice> notices;
  SqliteCCLCJournal* live{};
  SqliteCCLCJournal journal(
      JournalPath(temporary), CellId("cell.1"), kDefaultCCLCExactResultHistory,
      kDefaultCCLCTerminalResultBytes, [&](const ComponentIncidentPruneNotice& notice) {
        // Re-entry needs the journal mutex released; the second connection
        // sees the committed deletion and newly recorded open incident.
        ASSERT_TRUE(
            live->LoadOpenComponentIncident(ControllableComponentId("new.component")).value());
        EXPECT_EQ(RawScalar(JournalPath(temporary),
                            "SELECT count(*) FROM cclc_component_incidents WHERE acknowledged=1"),
                  200);
        notices.push_back(notice);
      });
  live = &journal;
  ASSERT_TRUE(
      journal
          .OpenComponentIncident(
              TestIncident("open.oldest", component, LifecycleBlockingCauseClassDTO::kFault, 1), 0U)
          .ok());
  EXPECT_TRUE(notices.empty());
  // Seed past the bound without invoking production pruning. The oldest
  // row remains OPEN; only the 205 deliberately closed rows are eligible.
  ExecRaw(JournalPath(temporary),
          "WITH RECURSIVE n(x) AS (SELECT 2 UNION ALL SELECT x+1 FROM n WHERE x<206) "
          "INSERT INTO "
          "cclc_component_incidents(incident_id,incident_sequence,controllable_component_id,"
          "cause_class,vendor_codes_json,detected_at_ns,summary,containment_state,record_json,"
          "acknowledged,acknowledged_by,acknowledged_at_ns,acknowledgement_request_id,"
          "acknowledgement_owner_epoch) "
          "SELECT 'closed.'||x,x,'closed.component.'||x,cause_class,vendor_codes_json,x,summary,"
          "containment_state,record_json,1,'test',x,'recover.'||x,'owner.test' FROM "
          "n,cclc_component_incidents WHERE incident_id='open.oldest'");
  ASSERT_TRUE(
      journal
          .OpenComponentIncident(TestIncident("new.open", ControllableComponentId("new.component"),
                                              LifecycleBlockingCauseClassDTO::kFault, 300),
                                 0U)
          .ok());
  ASSERT_EQ(notices.size(), 1U);
  EXPECT_EQ(notices.front().count, 5U);
  EXPECT_EQ(notices.front().oldest_sequence, 2U);
  EXPECT_EQ(notices.front().newest_sequence, 6U);
  EXPECT_EQ(
      RawScalar(JournalPath(temporary),
                "SELECT min(incident_sequence) FROM cclc_component_incidents WHERE acknowledged=1"),
      7);
  EXPECT_EQ(RawScalar(JournalPath(temporary),
                      "SELECT count(*) FROM cclc_component_incidents WHERE acknowledged=0"),
            2);
  EXPECT_EQ(journal.LoadOpenComponentIncident(component).value()->incident.incident_id,
            "open.oldest");
  ASSERT_TRUE(journal
                  .OpenComponentIncident(
                      TestIncident("new.ignored", ControllableComponentId("new.component"),
                                   LifecycleBlockingCauseClassDTO::kFault, 400),
                      0U)
                  .ok());
  EXPECT_EQ(notices.size(), 1U);
}

TEST(SqliteCCLCJournal, PruneNoticeFailureCannotUndoCommittedPruningOrNewIncident) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(
      JournalPath(temporary), CellId("cell.1"), kDefaultCCLCExactResultHistory,
      kDefaultCCLCTerminalResultBytes,
      [](const ComponentIncidentPruneNotice&) { throw std::runtime_error("sink failure"); });
  ASSERT_TRUE(
      journal
          .OpenComponentIncident(TestIncident("seed", ControllableComponentId("seed.component"),
                                              LifecycleBlockingCauseClassDTO::kFault, 1),
                                 0U)
          .ok());
  ExecRaw(JournalPath(temporary),
          "WITH RECURSIVE n(x) AS (SELECT 2 UNION ALL SELECT x+1 FROM n WHERE x<202) "
          "INSERT INTO "
          "cclc_component_incidents(incident_id,incident_sequence,controllable_component_id,"
          "cause_class,vendor_codes_json,detected_at_ns,summary,containment_state,record_json,"
          "acknowledged,acknowledged_by,acknowledged_at_ns,acknowledgement_request_id,"
          "acknowledgement_owner_epoch) "
          "SELECT 'closed.'||x,x,'closed.component.'||x,cause_class,vendor_codes_json,x,summary,"
          "containment_state,record_json,1,'test',x,'recover.'||x,'owner.test' FROM "
          "n,cclc_component_incidents WHERE incident_id='seed'");
  const auto recorded =
      journal.OpenComponentIncident(TestIncident("new", ControllableComponentId("new.component"),
                                                 LifecycleBlockingCauseClassDTO::kFault, 300),
                                    0U);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  EXPECT_EQ(recorded.value().incident.incident_id, "new");
  EXPECT_EQ(RawScalar(JournalPath(temporary),
                      "SELECT count(*) FROM cclc_component_incidents WHERE acknowledged=1"),
            200);
  EXPECT_EQ(RawScalar(JournalPath(temporary),
                      "SELECT count(*) FROM cclc_component_incidents WHERE acknowledged=0"),
            2);
}

namespace {
[[nodiscard]] std::shared_ptr<const content_identity::CanonicalValue>
IdentityAloneExceedsDiagnosticBudget() {
  using content_identity::CanonicalValue;
  auto snapshot = *TestLaunchDiagnosticSnapshot();
  snapshot = WithObjectMember(snapshot, "context", WithObjectMember(
      *snapshot.find_object_member("context"), "launch_identity",
      CanonicalValue(std::string(kMaximumCellAdmissionPrivateBinaryBytes, '\x01'))));
  auto settings = snapshot.find_object_member("settings")->as_object();
  for (auto& [key, value] : settings)
    if (key != "argv") value = CanonicalValue(nullptr);
  snapshot = WithObjectMember(snapshot, "settings", CanonicalValue(std::move(settings)));
  return std::make_shared<const CanonicalValue>(std::move(snapshot));
}
}  // namespace

TEST(SqliteCCLCJournal, ImpossibleIdentityCaptureRecordsNamedAbsentSnapshotAndPreservesFirstError) {
  const auto snapshot = IdentityAloneExceedsDiagnosticBudget();
  ASSERT_TRUE(ComponentIncidentDiagnosticSnapshotShapeIsValid(*snapshot));
  ASSERT_GT(content_identity::canonical_json(*snapshot).size(), kMaxComponentIncidentRecordBytes);
  for (const bool short_first : {false, true}) {
    SCOPED_TRACE(short_first);
    ScopedTemporaryDirectory temporary;
    const ControllableComponentId component("ur5e_robot");
    const auto initial = TestIncident("incident.ur5e", component,
                                      LifecycleBlockingCauseClassDTO::kFault, 100);
    const ErrorDTO first{"controller_launch_exited", "original signal=11", {{"original", "fact"}}};
    const ErrorDTO incoming{"controller_launch_exited", "incoming signal=11", {{"incoming", "fact"}}};
    {
      SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
      ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
      const auto contained = ComponentIncidentRecordWithContainment(
          initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
          std::string("fixture containment unavailable"));
      ASSERT_TRUE(journal.RecordComponentIncidentContainment(
          "incident.ur5e", ComponentIncidentContainmentStateDTO::kFailed, contained, 110).value());
      if (short_first) {
        ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.ur5e", 115, first).value());
      }
      const auto appended = journal.AppendComponentIncidentDiagnostic(
          "incident.ur5e", 120, incoming, snapshot);
      ASSERT_TRUE(appended.ok()) << appended.error().message;
      ASSERT_TRUE(appended.value());
      const auto current = journal.LoadOpenComponentIncident(component).value();
      ASSERT_TRUE(current && current->diagnostic);
      const auto& original_error = short_first ? first : incoming;
      EXPECT_EQ(current->diagnostic->code, original_error.code);
      EXPECT_EQ(current->diagnostic->message, original_error.message);
      for (const auto& [key, value] : original_error.details)
        EXPECT_EQ(current->diagnostic->details.at(key), value);
      EXPECT_EQ(current->diagnostic->details.at("snapshot_omitted_fields"), "diagnostic_too_large");
      EXPECT_FALSE(current->diagnostic_snapshot);
      const auto events = journal.LoadComponentIncidentEvents(0U, 16U).value();
      std::shared_ptr<const content_identity::CanonicalValue> event_snapshot;
      EXPECT_EQ(ComponentIncidentDiagnosticFromRecord(events.back().payload_json, &event_snapshot),
                current->diagnostic);
      EXPECT_FALSE(event_snapshot);
      EXPECT_FALSE(journal.AppendComponentIncidentDiagnostic(
          "incident.ur5e", 130, incoming, snapshot).value());
      EXPECT_EQ(journal.LoadComponentIncidentEvents(0U, 16U).value(), events);
      EXPECT_EQ(snapshot->find_object_member("context")->find_object_member("launch_identity")->as_string(),
                std::string(kMaximumCellAdmissionPrivateBinaryBytes, '\x01'));
    }
    SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
    const auto retained = reopened.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(retained && retained->diagnostic);
    EXPECT_EQ(retained->diagnostic->message, (short_first ? first : incoming).message);
    EXPECT_EQ(retained->diagnostic->details.at("snapshot_omitted_fields"), "diagnostic_too_large");
    EXPECT_FALSE(retained->diagnostic_snapshot);
  }
}

TEST(SqliteCCLCJournal, ImpossibleIdentityCaptureReusesEighthMetadataSlot) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const ControllableComponentId component("ur5e_robot");
  const auto initial = TestIncident("incident.ur5e", component,
                                    LifecycleBlockingCauseClassDTO::kFault, 100);
  ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
  const auto contained = ComponentIncidentRecordWithContainment(
      initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
      std::string("fixture containment unavailable"));
  ASSERT_TRUE(journal.RecordComponentIncidentContainment(
      "incident.ur5e", ComponentIncidentContainmentStateDTO::kFailed, contained, 110).value());
  ErrorDTO first{"controller_launch_exited", "original signal=11", {{"snapshot_omitted_fields", ""}}};
  for (std::size_t i = 1; i < kMaxLifecycleErrorDetails; ++i)
    first.details.emplace("original." + std::to_string(i), "fact");
  ASSERT_EQ(first.details.size(), kMaxLifecycleErrorDetails);
  ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.ur5e", 115, first).value());
  const auto appended = journal.AppendComponentIncidentDiagnostic("incident.ur5e", 120,
      ErrorDTO{"controller_launch_exited", "replacement signal", {}}, IdentityAloneExceedsDiagnosticBudget());
  ASSERT_TRUE(appended.ok()) << appended.error().message;
  ASSERT_TRUE(appended.value());
  const auto current = journal.LoadOpenComponentIncident(component).value();
  ASSERT_TRUE(current && current->diagnostic);
  EXPECT_EQ(current->diagnostic->code, first.code);
  EXPECT_EQ(current->diagnostic->message, first.message);
  EXPECT_EQ(current->diagnostic->details.size(), kMaxLifecycleErrorDetails);
  for (const auto& [key, value] : first.details)
    if (key != "snapshot_omitted_fields") {
      EXPECT_EQ(current->diagnostic->details.at(key), value);
    }
  EXPECT_EQ(current->diagnostic->details.at("snapshot_omitted_fields"), "diagnostic_too_large");
  EXPECT_FALSE(current->diagnostic_snapshot);
}

TEST(SqliteCCLCJournal, ImpossibleIdentityCaptureDoesNotInventOverflowMetadataCapacity) {
  for (const bool full_metadata : {false, true}) {
    SCOPED_TRACE(full_metadata);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const ControllableComponentId component("ur5e_robot");
    const auto initial = TestIncident("incident.ur5e", component,
                                      LifecycleBlockingCauseClassDTO::kFault, 100);
    ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
    const auto contained = ComponentIncidentRecordWithContainment(
        initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
        std::string("fixture containment unavailable"));
    ASSERT_TRUE(journal.RecordComponentIncidentContainment(
        "incident.ur5e", ComponentIncidentContainmentStateDTO::kFailed, contained, 110).value());
    ErrorDTO incoming{"controller_launch_exited", "original signal=11", {}};
    if (full_metadata)
      incoming.details.emplace("snapshot_omitted_fields", std::string(kMaxLifecycleErrorDetailValueBytes, 'x'));
    for (std::size_t i = incoming.details.size(); i < kMaxLifecycleErrorDetails; ++i)
      incoming.details.emplace("original." + std::to_string(i), "fact");
    ASSERT_TRUE(ComponentIncidentDiagnosticIsValid(incoming));
    if (!full_metadata) {
      ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.ur5e", 115, incoming).value());
    }
    const auto before = journal.LoadOpenComponentIncident(component).value();
    const auto events = journal.LoadComponentIncidentEvents(0U, 16U).value();
    const auto appended = journal.AppendComponentIncidentDiagnostic(
        "incident.ur5e", 120, incoming, IdentityAloneExceedsDiagnosticBudget());
    ASSERT_FALSE(appended.ok());
    EXPECT_EQ(appended.error().code, "component_lifecycle_incident_invalid");
    EXPECT_NE(appended.error().message.find("diagnostic_too_large"), std::string::npos);
    EXPECT_EQ(journal.LoadOpenComponentIncident(component).value()->record_json, before->record_json);
    EXPECT_EQ(journal.LoadComponentIncidentEvents(0U, 16U).value(), events);
  }
}

TEST(SqliteCCLCJournal, CeilingIdentityKeepsLatestMarkedTailWithinExistingJournalBudget) {
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("ur5e_robot");
  const auto initial = TestIncident("incident.ur5e", component,
                                    LifecycleBlockingCauseClassDTO::kFault, 100);
  const std::string identity(kMaximumCellAdmissionPrivateBinaryBytes, 'i');
  const std::string tail = std::string(128U * 1024U - 128U, 't') + "latest crash line\n";
  auto snapshot = *TestLaunchDiagnosticSnapshot();
  snapshot = WithObjectMember(snapshot, "context", WithObjectMember(
      *snapshot.find_object_member("context"), "launch_identity", content_identity::CanonicalValue(identity)));
  auto output = WithObjectMember(*snapshot.find_object_member("output"), "tail",
                                  content_identity::CanonicalValue(tail));
  output = WithObjectMember(output, "truncated", content_identity::CanonicalValue(false));
  snapshot = WithObjectMember(snapshot, "output", output);
  ASSERT_TRUE(ComponentIncidentDiagnosticSnapshotShapeIsValid(snapshot));
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
    const auto contained = ComponentIncidentRecordWithContainment(
        initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
        std::string("fixture containment unavailable"));
    ASSERT_TRUE(journal.RecordComponentIncidentContainment(
        "incident.ur5e", ComponentIncidentContainmentStateDTO::kFailed, contained, 110).value());
    ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.ur5e", 120,
        ErrorDTO{"controller_launch_exited", "signal=11", {}},
        std::make_shared<const content_identity::CanonicalValue>(snapshot)).value());
    const auto current = journal.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(current && current->diagnostic_snapshot);
    EXPECT_LE(current->record_json.size(), kMaxComponentIncidentRecordBytes);
    const auto& captured = *current->diagnostic_snapshot;
    EXPECT_EQ(captured.find_object_member("context")->find_object_member("launch_identity")->as_string(), identity);
    const auto& captured_output = *captured.find_object_member("output");
    EXPECT_TRUE(captured_output.find_object_member("truncated")->as_bool());
    const auto& retained_tail = captured_output.find_object_member("tail")->as_string();
    EXPECT_TRUE(retained_tail.starts_with("[truncated]"));
    EXPECT_TRUE(retained_tail.ends_with("latest crash line\n"));
    EXPECT_LT(retained_tail.size(), tail.size());
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const auto retained = reopened.LoadOpenComponentIncident(component).value();
  ASSERT_TRUE(retained && retained->diagnostic_snapshot);
  EXPECT_EQ(retained->diagnostic_snapshot->find_object_member("context")
                ->find_object_member("launch_identity")->as_string(), identity);
}

TEST(SqliteCCLCJournal, Real257ByteLaunchIdentitySurvivesIncidentReadbackAndReopen) {
  ASSERT_EQ(kUr5eLaunchIdentity257.size(), 257U);
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("ur5e_robot");
  const auto initial = TestIncident("incident.ur5e", component,
                                    LifecycleBlockingCauseClassDTO::kFault, 100);
  const auto context = WithObjectMember(
      *TestLaunchDiagnosticSnapshot()->find_object_member("context"), "launch_identity",
      content_identity::CanonicalValue(kUr5eLaunchIdentity257));
  const auto snapshot = std::make_shared<const content_identity::CanonicalValue>(
      WithObjectMember(*TestLaunchDiagnosticSnapshot(), "context", context));
  ASSERT_TRUE(ComponentIncidentDiagnosticSnapshotShapeIsValid(*snapshot));
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
    const auto contained = ComponentIncidentRecordWithContainment(
        initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
        std::string("fixture containment unavailable"));
    ASSERT_TRUE(journal.RecordComponentIncidentContainment(
        "incident.ur5e", ComponentIncidentContainmentStateDTO::kFailed, contained, 110).value());
    ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic(
        "incident.ur5e", 120, ErrorDTO{"controller_launch_exited", "signal=11", {}}, snapshot).value());
    const auto current = journal.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(current && current->diagnostic_snapshot);
    EXPECT_EQ(*current->diagnostic_snapshot, *snapshot);
    std::shared_ptr<const content_identity::CanonicalValue> event_snapshot;
    (void)ComponentIncidentDiagnosticFromRecord(
        journal.LoadComponentIncidentEvents(0U, 16U).value().back().payload_json, &event_snapshot);
    ASSERT_TRUE(event_snapshot);
    EXPECT_EQ(*event_snapshot, *snapshot);
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const auto retained = reopened.LoadOpenComponentIncident(component).value();
  ASSERT_TRUE(retained && retained->diagnostic_snapshot);
  EXPECT_EQ(retained->diagnostic_snapshot->find_object_member("context")
                ->find_object_member("launch_identity")->as_string(), kUr5eLaunchIdentity257);
}

TEST(SqliteCCLCJournal, ExitDiagnosticIsSetOnceAndSurvivesGlobalEventTrimAndReopen) {
  const auto named_snapshot = TestLaunchDiagnosticSnapshot();
  ASSERT_NE(named_snapshot->find_object_member("launch_readiness"), nullptr);
  EXPECT_EQ(named_snapshot->find_object_member("readiness"), nullptr);
  const auto& named_context = *named_snapshot->find_object_member("context");
  EXPECT_NE(named_context.find_object_member("controllable_component_id"), nullptr);
  EXPECT_NE(named_context.find_object_member("retained_bundle_id"), nullptr);
  EXPECT_EQ(named_context.find_object_member("component_id"), nullptr);
  EXPECT_EQ(named_context.find_object_member("bundle_id"), nullptr);
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("component.1");
  const auto initial =
      TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kFault, 100);
  ErrorDTO diagnostic{"controller_launch_exited",
                      "signal=11 Apply=apply.7 log=/controller_launch.log tail=crash",
                      {}};
  diagnostic.details["retained_configuration_sha256"] = content_identity::sha256_digest("{}");
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
    const auto contained = ComponentIncidentRecordWithContainment(
        initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
        std::string("test containment unavailable"));
    ASSERT_TRUE(journal
                    .RecordComponentIncidentContainment(
                        "incident.1", ComponentIncidentContainmentStateDTO::kFailed, contained, 110)
                    .value());
    const auto initial_events = journal.LoadComponentIncidentEvents(0U, 16U);
    ASSERT_TRUE(initial_events.ok()) << initial_events.error().message;
    ASSERT_EQ(initial_events.value().size(), 1U);
    const auto original_event = initial_events.value().front();
    ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.1", 115, diagnostic).value());
    EXPECT_FALSE(
        journal
            .AppendComponentIncidentDiagnostic(
                "incident.1", 116, ErrorDTO{"controller_launch_exited", "later short error", {}})
            .value());
    const auto short_record = journal.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(short_record && short_record->diagnostic);
    EXPECT_EQ(short_record->diagnostic, diagnostic);
    EXPECT_FALSE(short_record->diagnostic_snapshot);
    const auto short_event = journal.LoadComponentIncidentEvents(0U, 16U).value().back();
    EXPECT_TRUE(content_identity::parse_canonical_json(short_event.payload_json)
                    .find_object_member("diagnostic")
                    ->find_object_member("snapshot")
                    ->is_null());
    ASSERT_TRUE(journal
                    .AppendComponentIncidentDiagnostic(
                        "incident.1", 120,
                        ErrorDTO{"controller_launch_exited", "later rich incoming error", {}},
                        TestLaunchDiagnosticSnapshot())
                    .value());
    EXPECT_FALSE(journal
                     .AppendComponentIncidentDiagnostic("incident.1", 130, diagnostic,
                                                        TestLaunchDiagnosticSnapshot())
                     .value());
    auto current = journal.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(current);
    EXPECT_EQ(current->incident.cause_class, initial.incident.cause_class);
    EXPECT_EQ(current->incident.summary, initial.incident.summary);
    EXPECT_EQ(current->diagnostic, diagnostic);
    auto events = journal.LoadComponentIncidentEvents(0U, 16U).value();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1], short_event);
    EXPECT_EQ(events.front(), original_event);
    EXPECT_EQ(ComponentIncidentDiagnosticFromRecord(events.back().payload_json), diagnostic);
    // A driver that read the old row cannot erase the set-once evidence when
    // writing an ordinary current-cause update.
    ASSERT_TRUE(journal
                    .UpdateOpenComponentIncident("incident.1", initial.incident.cause_class,
                                                 initial.incident.vendor_codes, contained)
                    .value());
    EXPECT_EQ(journal.LoadOpenComponentIncident(component).value()->diagnostic, diagnostic);
    // Fill history with bytes from an actual current-format emitted event,
    // then append an unrelated incident through the normal global trim path.
    // The row, not history retention, owns the durable diagnostic fact.
    ExecRaw(JournalPath(temporary),
            "WITH RECURSIVE n(x) AS (SELECT 4 UNION ALL SELECT x+1 FROM n WHERE x<1050) "
            "INSERT INTO cclc_component_incident_events SELECT "
            "x,incident_id,event_kind,x,payload_json FROM n,cclc_component_incident_events "
            "WHERE event_sequence=" + std::to_string(original_event.sequence));
    const auto other = TestIncident("incident.other", ControllableComponentId("component.other"),
                                    LifecycleBlockingCauseClassDTO::kFault, 200);
    ASSERT_TRUE(journal.OpenComponentIncident(other, 0U).ok());
    const auto other_contained = ComponentIncidentRecordWithContainment(
        other.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
        std::string("test"));
    ASSERT_TRUE(journal
                    .RecordComponentIncidentContainment(
                        "incident.other", ComponentIncidentContainmentStateDTO::kFailed,
                        other_contained, 210)
                    .value());
    const auto trimmed = journal.LoadComponentIncidentEvents(0U, 64U);
    ASSERT_TRUE(trimmed.ok()) << trimmed.error().code << ": " << trimmed.error().message;
    events = trimmed.value();
    ASSERT_FALSE(events.empty());
    EXPECT_GT(events.front().sequence, 3U);  // Earlier events were trimmed.
    EXPECT_FALSE(journal
                     .AppendComponentIncidentDiagnostic(
                         "incident.1", 220,
                         ErrorDTO{"controller_launch_exited", "later signal=9", {}},
                         TestLaunchDiagnosticSnapshot())
                     .value());
    journal.Close();
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const auto current = reopened.LoadOpenComponentIncident(component).value();
  ASSERT_TRUE(current);
  EXPECT_EQ(current->diagnostic, diagnostic);
  EXPECT_EQ(current->incident.summary, initial.incident.summary);
  EXPECT_EQ(current->incident.cause_class, initial.incident.cause_class);
  EXPECT_FALSE(reopened
                   .AppendComponentIncidentDiagnostic("incident.1", 230, diagnostic,
                                                      TestLaunchDiagnosticSnapshot())
                   .value());
}

[[nodiscard]] content_identity::CanonicalValue WithMember(
    const content_identity::CanonicalValue& value, const std::string_view key,
    content_identity::CanonicalValue replacement) {
  auto fields = value.as_object();
  for (auto& [name, field] : fields)
    if (name == key) field = std::move(replacement);
  return content_identity::CanonicalValue(std::move(fields));
}

TEST(SqliteCCLCJournal,
     WorstCaseMetadataReservePreservesBoundedTailAndAllowsFullHistoryThenRecover) {
  using content_identity::CanonicalValue;
  for (const std::size_t settings_bytes : {124000U, 300000U, 500U * 1024U}) {
    SCOPED_TRACE(settings_bytes);
    const bool settings_dominate = settings_bytes == 300000U;
    const bool aggregate_overflow = settings_bytes == 500U * 1024U;
    const std::string settings_key =
        aggregate_overflow ? "retained_configuration_json" : "controllers_yaml";
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const ControllableComponentId component("component.1");
    auto initial =
        TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kFault, 100);
    auto primary = content_identity::parse_canonical_json(initial.record_json).as_object();
    primary.emplace_back("primary_fixture_evidence", CanonicalValue(std::string(64000U, 'p')));
    initial.record_json = content_identity::canonical_json(CanonicalValue(std::move(primary)));
    ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
    // Pending containment refuses enrichment without an event or row mutation.
    const ErrorDTO pending_error{"controller_launch_exited", "signal=11", {}};
    const auto pending = journal.AppendComponentIncidentDiagnostic("incident.1", 101, pending_error,
                                                                   TestLaunchDiagnosticSnapshot());
    ASSERT_TRUE(pending.ok());
    EXPECT_FALSE(pending.value());
    EXPECT_EQ(journal.LoadOpenComponentIncident(component).value()->record_json,
              initial.record_json);
    EXPECT_TRUE(journal.LoadComponentIncidentEvents(0U, 10U).value().empty());
    const ComponentIncidentContainmentReceiptDTO receipt{
        StopId("stop.real"),
        StopGeneration(std::numeric_limits<std::uint64_t>::max()),
        {OperationId("operation.1"), OperationId("operation.2")},
        ProgramRunId("program.1")};
    initial.record_json = ComponentIncidentRecordWithContainment(
        initial.record_json, ComponentIncidentContainmentStateDTO::kRequested, receipt,
        std::nullopt);
    ASSERT_TRUE(journal
                    .RecordComponentIncidentContainment(
                        "incident.1", ComponentIncidentContainmentStateDTO::kRequested,
                        initial.record_json, 102)
                    .value());
    const auto hydrated = journal.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(hydrated);
    EXPECT_EQ(hydrated->incident.containment, receipt);
    EXPECT_TRUE(hydrated->incident.locally_valid());
    auto snapshot = *TestLaunchDiagnosticSnapshot();
    auto settings = *snapshot.find_object_member("settings");
    const std::string settings_text(settings_bytes, 's');
    settings = WithMember(settings, settings_key, CanonicalValue(settings_text));
    snapshot = WithMember(snapshot, "settings", std::move(settings));
    auto output = *snapshot.find_object_member("output");
    const std::string tail = settings_dominate ? "" : std::string(65535U, '"');
    output = WithMember(output, "tail", CanonicalValue(tail));
    output = WithMember(output, "line_count", CanonicalValue(tail.empty() ? 0.0 : 1.0));
    snapshot = WithMember(snapshot, "output", std::move(output));
    ASSERT_TRUE(ComponentIncidentDiagnosticSnapshotShapeIsValid(snapshot));
    EXPECT_EQ(ComponentIncidentDiagnosticSnapshotIsValid(snapshot), !aggregate_overflow);
    if (!settings_dominate) {
      EXPECT_EQ(content_identity::canonical_json(CanonicalValue(tail)).size(), 128U * 1024U);
    }
    const ErrorDTO first_error{"controller_launch_exited",
                               "initial short signal=11",
                               {{"request_context", "first Apply"}}};
    ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.1", 105, first_error).value());
    const ErrorDTO error{"controller_launch_exited", "later signal=11", {}};
    const auto appended = journal.AppendComponentIncidentDiagnostic(
        "incident.1", 110, error, std::make_shared<const CanonicalValue>(snapshot));
    ASSERT_TRUE(appended.ok()) << appended.error().message;
    ASSERT_TRUE(appended.value());
    const auto row = journal.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(row && row->diagnostic_snapshot);
    EXPECT_LE(row->record_json.size(), kMaxComponentIncidentRecordBytes);
    EXPECT_EQ(row->incident.cause_class, initial.incident.cause_class);
    EXPECT_EQ(row->incident.summary, initial.incident.summary);
    const auto recorded = content_identity::parse_canonical_json(row->record_json);
    EXPECT_EQ(recorded.find_object_member("primary_fixture_evidence")->as_string(),
              std::string(64000U, 'p'));
    const auto& captured = *row->diagnostic_snapshot;
    EXPECT_FALSE(captured.find_object_member("output")->find_object_member("truncated")->as_bool());
    EXPECT_EQ(captured.find_object_member("output")->find_object_member("tail")->as_string(), tail);
    EXPECT_TRUE(
        captured.find_object_member("settings")->find_object_member(settings_key)->is_null());
    EXPECT_EQ(row->diagnostic->details.at("snapshot_omitted_fields"), settings_key);
    EXPECT_EQ(row->diagnostic->details.at("retained_configuration_sha256"),
              content_identity::sha256_digest(aggregate_overflow ? settings_text : "{}"));
    EXPECT_EQ(row->diagnostic->code, first_error.code);
    EXPECT_EQ(row->diagnostic->message, first_error.message);
    EXPECT_EQ(row->diagnostic->details.at("request_context"), "first Apply");
    ASSERT_TRUE(ComponentIncidentDiagnosticSnapshotIsValid(captured));
    const auto duplicate = journal.AppendComponentIncidentDiagnostic(
        "incident.1", 111, error, std::make_shared<const CanonicalValue>(snapshot));
    ASSERT_TRUE(duplicate.ok());
    EXPECT_FALSE(duplicate.value());
    const auto event = journal.LoadComponentIncidentEvents(0U, 10U).value().back();
    EXPECT_LE(event.payload_json.size(), kMaxComponentIncidentRecordBytes);
    // Exact reserve formula: encoded maximum future primary metadata minus
    // the encoded current primary row. Six-byte control escapes are valid
    // vendor/attribution text; all existing count/identity/time bounds apply.
    const std::vector<std::string> maximal_codes(
        kMaxComponentIncidentVendorCodes,
        std::string(kMaxComponentIncidentVendorCodeBytes, '\x01'));
    auto expected_future = initial.record_json;
    auto latest = row->record_json;
    const auto immutable = content_identity::canonical_json(captured);
    for (std::size_t change = 0; change < kMaxComponentIncidentTimelineEntries; ++change) {
      expected_future = ComponentIncidentRecordWithChange(
          expected_future, std::numeric_limits<std::int64_t>::max(),
          LifecycleBlockingCauseClassDTO::kExternalControl, maximal_codes);
      latest = ComponentIncidentRecordWithChange(latest, std::numeric_limits<std::int64_t>::max(),
                                                 LifecycleBlockingCauseClassDTO::kExternalControl,
                                                 maximal_codes);
      ASSERT_TRUE(journal
                      .UpdateOpenComponentIncident("incident.1",
                                                   LifecycleBlockingCauseClassDTO::kExternalControl,
                                                   maximal_codes, latest)
                      .value());
      EXPECT_LE(latest.size(), kMaxComponentIncidentRecordBytes);
      EXPECT_EQ(content_identity::canonical_json(
                    *journal.LoadOpenComponentIncident(component).value()->diagnostic_snapshot),
                immutable);
    }
    auto acknowledged = initial.incident;
    acknowledged.acknowledged = true;
    acknowledged.acknowledged_by = std::string(kMaxLifecycleRequestLabelBytes, '\x01');
    acknowledged.acknowledgement_request_id =
        RequestId(std::string(kMaxLifecycleIdentityBytes, '\x01'));
    acknowledged.acknowledged_at_ns = std::numeric_limits<std::int64_t>::max();
    expected_future = ComponentIncidentAcknowledgedPayload(expected_future, acknowledged);
    const auto reserved_growth = expected_future.size() - initial.record_json.size();
    EXPECT_LE(row->record_json.size() + reserved_growth, kMaxComponentIncidentRecordBytes);
    RequestOwnerEpoch owner("owner.1");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    auto recover = RecoverIntent(std::string(kMaxLifecycleIdentityBytes, '\x01'), owner);
    recover.attribution = {RequestTransportDTO::kHttp,
                           RequestAssuranceDTO::kExternallyAuthenticated,
                           acknowledged.acknowledged_by};
    ASSERT_TRUE(recover.locally_valid());
    const auto recovered = journal.RecoverComponentIncident(
        owner, recover, *acknowledged.acknowledged_by, *acknowledged.acknowledged_at_ns);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_TRUE(recovered.value());
    const auto final_event = journal.LoadComponentIncidentEvents(0U, 10U).value().back();
    EXPECT_LE(final_event.payload_json.size(), kMaxComponentIncidentRecordBytes);
    std::shared_ptr<const CanonicalValue> final_snapshot;
    EXPECT_TRUE(ComponentIncidentDiagnosticFromRecord(final_event.payload_json, &final_snapshot));
    ASSERT_TRUE(final_snapshot);
    EXPECT_EQ(content_identity::canonical_json(*final_snapshot), immutable);
  }
}

TEST(SqliteCCLCJournal, ConflictingRichMetadataKeepsTheFirstShortDiagnostic) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const ControllableComponentId component("component.1");
  const auto initial =
      TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kFault, 100);
  ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
  const auto contained = ComponentIncidentRecordWithContainment(
      initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
      std::string("containment failed"));
  ASSERT_TRUE(journal
                  .RecordComponentIncidentContainment(
                      "incident.1", ComponentIncidentContainmentStateDTO::kFailed, contained, 101)
                  .value());
  const ErrorDTO first{"controller_launch_exited",
                       "original signal=11",
                       {{"retained_configuration_sha256", "original known identity"}}};
  ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.1", 102, first).value());
  const auto original = journal.LoadOpenComponentIncident(component).value();
  const auto events = journal.LoadComponentIncidentEvents(0U, 16U).value();
  EXPECT_FALSE(journal
                   .AppendComponentIncidentDiagnostic(
                       "incident.1", 103,
                       ErrorDTO{"controller_launch_exited", "replacement error", {}},
                       TestLaunchDiagnosticSnapshot())
                   .ok());
  const auto after = journal.LoadOpenComponentIncident(component).value();
  ASSERT_TRUE(after && original);
  EXPECT_EQ(after->record_json, original->record_json);
  EXPECT_EQ(after->diagnostic, first);
  EXPECT_FALSE(after->diagnostic_snapshot);
  EXPECT_EQ(after->incident.containment_error, "containment failed");
  EXPECT_EQ(journal.LoadComponentIncidentEvents(0U, 16U).value(), events);
}

TEST(SqliteCCLCJournal, OriginalOmissionFactsMustMatchTheCompleteCapture) {
  using content_identity::CanonicalValue;
  for (const bool requires_omission : {false, true}) {
    SCOPED_TRACE(requires_omission);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const ControllableComponentId component("component.1");
    const auto initial =
        TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kFault, 100);
    ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
    const auto contained = ComponentIncidentRecordWithContainment(
        initial.record_json, ComponentIncidentContainmentStateDTO::kFailed, std::nullopt,
        std::string("containment failed"));
    ASSERT_TRUE(journal
                    .RecordComponentIncidentContainment(
                        "incident.1", ComponentIncidentContainmentStateDTO::kFailed, contained, 101)
                    .value());
    const std::string omission = "controllers_yaml,retained_configuration_json";
    const ErrorDTO first{
        "controller_launch_exited", "original signal=11", {{"snapshot_omitted_fields", omission}}};
    ASSERT_TRUE(journal.AppendComponentIncidentDiagnostic("incident.1", 102, first).value());
    const auto original = journal.LoadOpenComponentIncident(component).value();
    const auto events = journal.LoadComponentIncidentEvents(0U, 16U).value();
    auto snapshot = *TestLaunchDiagnosticSnapshot();
    if (requires_omission) {
      auto settings = *snapshot.find_object_member("settings");
      settings =
          WithMember(settings, "controllers_yaml", CanonicalValue(std::string(501U * 1024U, 'y')));
      settings = WithMember(settings, "retained_configuration_json",
                            CanonicalValue(std::string(500U * 1024U, 'j')));
      snapshot = WithMember(snapshot, "settings", std::move(settings));
    }
    const auto captured = journal.AppendComponentIncidentDiagnostic(
        "incident.1", 103, ErrorDTO{"controller_launch_exited", "replacement error", {}},
        std::make_shared<const CanonicalValue>(snapshot));
    EXPECT_EQ(captured.ok(), requires_omission);
    const auto after = journal.LoadOpenComponentIncident(component).value();
    ASSERT_TRUE(after && original);
    EXPECT_EQ(after->diagnostic->message, first.message);
    EXPECT_EQ(after->diagnostic->details.at("snapshot_omitted_fields"), omission);
    if (requires_omission) {
      ASSERT_TRUE(captured.value());
      ASSERT_TRUE(after->diagnostic_snapshot);
      const auto* settings = after->diagnostic_snapshot->find_object_member("settings");
      EXPECT_TRUE(settings->find_object_member("controllers_yaml")->is_null());
      EXPECT_TRUE(settings->find_object_member("retained_configuration_json")->is_null());
    } else {
      EXPECT_EQ(after->record_json, original->record_json);
      EXPECT_EQ(journal.LoadComponentIncidentEvents(0U, 16U).value(), events);
    }
  }
}

TEST(SqliteCCLCJournal, EncodedTailOverflowCannotAlterAnOpenPrimaryIncident) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const ControllableComponentId component("component.1");
  const auto initial =
      TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kFault, 100);
  ASSERT_TRUE(journal.OpenComponentIncident(initial, 0U).ok());
  auto snapshot = *TestLaunchDiagnosticSnapshot();
  auto output = *snapshot.find_object_member("output");
  // Raw bytes fit 128KiB, but JSON quoting pushes this over the encoded cap.
  output = WithMember(output, "tail", content_identity::CanonicalValue(std::string(65536U, '"')));
  snapshot = WithMember(snapshot, "output", std::move(output));
  EXPECT_FALSE(ComponentIncidentDiagnosticSnapshotIsValid(snapshot));
  EXPECT_FALSE(journal
                   .AppendComponentIncidentDiagnostic(
                       "incident.1", 110, ErrorDTO{"controller_launch_exited", "signal=11", {}},
                       std::make_shared<const content_identity::CanonicalValue>(snapshot))
                   .ok());
  const auto row = journal.LoadOpenComponentIncident(component).value();
  ASSERT_TRUE(row);
  EXPECT_EQ(row->record_json, initial.record_json);
  EXPECT_FALSE(row->diagnostic);
  EXPECT_TRUE(journal.LoadComponentIncidentEvents(0U, 10U).value().empty());
}

TEST(SqliteCCLCJournal, OpenIncidentSurvivesReopenAndOnlyTheOperatorsRecoverClosesIt) {
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("component.1");
  const auto opened =
      TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kProviderLost, 100);
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    const auto stored = journal.OpenComponentIncident(opened, 0U);
    ASSERT_TRUE(stored.ok()) << stored.error().message;
    EXPECT_EQ(stored.value(), opened);
    // One open incident per component: a second detection returns it.
    const auto again = journal.OpenComponentIncident(
        TestIncident("incident.2", component, LifecycleBlockingCauseClassDTO::kFault, 200), 0U);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().incident.incident_id, "incident.1");
    ASSERT_TRUE(journal
                    .RecordInterventionCause(component, ControlDomainId("domain.1"),
                                             LifecycleBlockingCauseClassDTO::kEmergencyStop, 150,
                                             0U)
                    .ok());
    journal.Close();
  }
  // In-session host replacement: a new journal owner sees the same incident.
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto open = journal.LoadOpenComponentIncident(component);
  ASSERT_TRUE(open.ok()) << open.error().message;
  ASSERT_TRUE(open.value().has_value());
  EXPECT_EQ(*open.value(), opened);
  EXPECT_TRUE(journal.LoadComponentIncidentEvents(0U, 16U).value().empty());

  // Containment is recorded once, with its history event.
  const auto contained = ComponentIncidentRecordWithContainment(
      opened.record_json, ComponentIncidentContainmentStateDTO::kRequested,
      ComponentIncidentContainmentReceiptDTO{StopId("stop.7"), StopGeneration(3U), {}, std::nullopt},
      std::nullopt);
  ASSERT_TRUE(journal
                  .RecordComponentIncidentContainment(
                      "incident.1", ComponentIncidentContainmentStateDTO::kRequested, contained, 110)
                  .value());
  EXPECT_FALSE(journal
                   .RecordComponentIncidentContainment(
                       "incident.1", ComponentIncidentContainmentStateDTO::kRequested, contained,
                       120)
                   .value());
  // A further change while open: the most recent cause wins.
  const auto changed = ComponentIncidentRecordWithChange(
      contained, 130, LifecycleBlockingCauseClassDTO::kEmergencyStop, {"C2", "mode=0", "C1"});
  ASSERT_TRUE(journal
                  .UpdateOpenComponentIncident("incident.1",
                                               LifecycleBlockingCauseClassDTO::kEmergencyStop,
                                               {"C2", "mode=0", "C1"}, changed)
                  .value());
  auto current = journal.LoadOpenComponentIncident(component).value();
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(current->incident.cause_class, LifecycleBlockingCauseClassDTO::kEmergencyStop);
  EXPECT_EQ(current->incident.vendor_codes, (std::vector<std::string>{"C2", "mode=0", "C1"}));
  EXPECT_EQ(current->incident.containment_state, ComponentIncidentContainmentStateDTO::kRequested);
  EXPECT_EQ(current->record_json, changed);

  const auto recover = RecoverIntent("operator.recover.1", owner);
  const ComponentIncidentRecoveryDTO expected_recovery{
      "incident.1", component, LifecycleBlockingCauseClassDTO::kEmergencyStop, "operator.ada",
      140};
  const auto recovered = journal.RecoverComponentIncident(owner, recover, "operator.ada", 140);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_TRUE(recovered.value().has_value());
  EXPECT_EQ(recovered.value()->replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_TRUE(recovered.value()->locally_valid());
  EXPECT_EQ(recovered.value()->incident_recovery, expected_recovery);
  // Its retransmission is no second acknowledgement; the ledger replays it.
  const auto again = journal.RecoverComponentIncident(owner, recover, "operator.ada", 150);
  ASSERT_TRUE(again.ok()) << again.error().message;
  EXPECT_FALSE(again.value().has_value());
  EXPECT_FALSE(journal.LoadOpenComponentIncident(component).value().has_value());
  EXPECT_TRUE(journal.LoadInterventionCauses(component).value().empty());
  const auto recovery = journal.ReadLifecycleRequestResult(recover);
  ASSERT_TRUE(recovery.ok() && recovery.value().has_value());
  EXPECT_EQ(recovery.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(recovery.value()->incident_recovery, expected_recovery);
  EXPECT_FALSE(journal.ReadLifecycleRequestResult(RecoverIntent("other", owner))
                   .value()
                   .has_value());

  const auto events = journal.LoadComponentIncidentEvents(0U, 16U);
  ASSERT_TRUE(events.ok()) << events.error().message;
  ASSERT_EQ(events.value().size(), 2U);
  EXPECT_EQ(events.value()[0].sequence, 1U);
  EXPECT_EQ(events.value()[0].event_kind, kComponentIncidentEventKind);
  EXPECT_EQ(events.value()[0].payload_json, contained);
  EXPECT_EQ(events.value()[1].sequence, 2U);
  EXPECT_EQ(events.value()[1].event_kind, kComponentIncidentAcknowledgedEventKind);
  EXPECT_NE(events.value()[1].payload_json.find(R"("acknowledged_by":"operator.ada")"),
            std::string::npos);
  EXPECT_NE(events.value()[1].payload_json.find(R"("timeline":[{"at_ns":100,)"),
            std::string::npos);
  EXPECT_EQ(journal.LoadComponentIncidentEvents(1U, 16U).value().size(), 1U);
  EXPECT_FALSE(journal.LoadComponentIncidentEvents(0U, 0U).ok());

  // After the Recover a new detection opens a new incident.
  const auto next = journal.OpenComponentIncident(
      TestIncident("incident.3", component, LifecycleBlockingCauseClassDTO::kCollision, 300), 1U);
  ASSERT_TRUE(next.ok());
  EXPECT_EQ(next.value().incident.incident_id, "incident.3");
}

TEST(SqliteCCLCJournal, CloseRecoveryMatchesActualIncidentAndRetainsTypedReplayAcrossReopen) {
  ScopedTemporaryDirectory temporary;
  RequestOwnerEpoch owner("owner.1");
  const ControllableComponentId component("component.1");
  auto close = RecoverIntent("close.recover.1", owner);
  close.reason = "cell closed by operator";
  close.close_incident_recovery = CloseComponentIncidentRecoveryDTO{"incident.1"};
  ComponentIncidentRecoveryDTO expected{
      "incident.1", component, LifecycleBlockingCauseClassDTO::kFault, "operator.ada", 140,
      ComponentIncidentClosureReasonDTO::kCellClosedByOperator};
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    close.request_context.request_owner_epoch = owner;
    ASSERT_TRUE(journal.OpenComponentIncident(
        TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kFault, 100), 0U).ok());
    auto foreign = close;
    foreign.close_incident_recovery->expected_incident_id = "incident.foreign";
    const auto refused = journal.RecoverComponentIncident(owner, foreign, "operator.ada", 120);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "component_lifecycle_close_incident_mismatch");
    EXPECT_TRUE(journal.LoadOpenComponentIncident(component).value());
    EXPECT_FALSE(journal.ReadLifecycleRequestResult(foreign).value());
    const auto recovered = journal.RecoverComponentIncident(owner, close, "operator.ada", 140);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_TRUE(recovered.value());
    EXPECT_EQ(recovered.value()->incident_recovery, expected);
    EXPECT_FALSE(recovered.value()->acceptance);
    EXPECT_FALSE(recovered.value()->status);
    const auto events = journal.LoadComponentIncidentEvents(0U, 16U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1U);
    EXPECT_NE(events.value().front().payload_json.find(
        R"("closure_reason":"cell_closed_by_operator")"), std::string::npos);
    EXPECT_NE(events.value().front().payload_json.find(
        R"("acknowledged_by":"operator.ada")"), std::string::npos);
    const auto replay = journal.RecoverComponentIncident(owner, close, "other.operator", 150);
    ASSERT_TRUE(replay.ok() && replay.value());
    EXPECT_EQ(replay.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(replay.value()->incident_recovery, expected);
    const auto conflict = journal.RecoverComponentIncident(owner, foreign, "operator.ada", 150);
    ASSERT_TRUE(conflict.ok() && conflict.value());
    EXPECT_EQ(conflict.value()->replay_status, RequestReplayStatusDTO::kRequestIdConflict);
    auto closed = close;
    closed.request_context.request_id = RequestId("close.again");
    EXPECT_FALSE(journal.RecoverComponentIncident(owner, closed, "operator.ada", 160).ok());
    ASSERT_TRUE(journal.OpenComponentIncident(
        TestIncident("incident.2", component, LifecycleBlockingCauseClassDTO::kFault, 200), 1U).ok());
    EXPECT_FALSE(journal.RecoverComponentIncident(owner, closed, "operator.ada", 220).ok());
    EXPECT_EQ(journal.LoadOpenComponentIncident(component).value()->incident.incident_id, "incident.2");
    journal.Close();
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const auto replay = reopened.ReadLifecycleRequestResult(close);
  ASSERT_TRUE(replay.ok() && replay.value());
  EXPECT_EQ(replay.value()->incident_recovery, expected);
  EXPECT_EQ(replay.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
}

TEST(SqliteCCLCJournal, CloseRecoveryWithNoActualIncidentNeverFallsBackToOrdinaryRecover) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  auto close = RecoverIntent("close.recover", owner);
  close.reason = "cell closed by operator";
  close.close_incident_recovery = CloseComponentIncidentRecoveryDTO{"incident.absent"};
  const auto refused = journal.RecoverComponentIncident(owner, close, "operator.ada", 100);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_close_incident_mismatch");
  EXPECT_FALSE(journal.ReadLifecycleRequestResult(close).value());
  EXPECT_TRUE(journal.LoadComponentIncidentEvents(0U, 16U).value().empty());
  close.close_incident_recovery.reset();
  const auto ordinary = journal.RecoverComponentIncident(owner, close, "operator.ada", 100);
  ASSERT_TRUE(ordinary.ok());
  EXPECT_FALSE(ordinary.value());
}

TEST(SqliteCCLCJournal, ChangedConfigurationRetiresReachedIntentAndOperatorRecoverRemainsExplicit) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto route = ActiveConfiguration(ExpectedDomain());
  const ControllableComponentId component("component.1");
  const auto plan =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
  ASSERT_TRUE(plan.ok());
  // While the recorded plan is unadopted its component's change is requested.
  EXPECT_TRUE(journal.ComponentInUnadoptedApplyPlan(component).value());
  EXPECT_FALSE(
      journal.ComponentInUnadoptedApplyPlan(ControllableComponentId("component.other")).value());
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                          plan.value().components,
                                          {route}})
                  .ok());
  EXPECT_FALSE(journal.ComponentInUnadoptedApplyPlan(component).value());
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value()->reached);
  EXPECT_TRUE(journal.RecordLifecycleTargetReached(component, true, 0U).value());
  EXPECT_FALSE(journal.RecordLifecycleTargetReached(component, true, 0U).value());
  // An operator target keeps it.
  ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                         LifecycleTargetOriginDTO::kOperator)
                  .ok());
  EXPECT_TRUE(journal.LoadLifecycleTarget(component).value()->reached);

  // A Prepared pursuit remains after incident retirement, and its
  // operator Recover remembers the cause without creating a new pursuit.
  const auto record = TestIncident("incident.1", component,
                                   LifecycleBlockingCauseClassDTO::kExternalControl, 10);
  ASSERT_TRUE(journal.OpenComponentIncident(record, 0U).ok());
  const auto recovered = journal.RecoverComponentIncident(
      owner, RecoverIntent("operator.recover.1", owner), "operator.ada", 20);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_TRUE(recovered.value().has_value());
  auto target = journal.LoadLifecycleTargets().value().front();
  EXPECT_FALSE(target.reached);
  EXPECT_EQ(target.recovered_cause, LifecycleBlockingCauseClassDTO::kExternalControl);
  // Reaching the target again ends that recovery.
  EXPECT_TRUE(journal.RecordLifecycleTargetReached(component, true, 1U).value());
  target = journal.LoadLifecycleTargets().value().front();
  EXPECT_TRUE(target.reached);
  EXPECT_FALSE(target.recovered_cause.has_value());

  // A later changed configuration retires intent without pretending Recover.
  auto next_route = route;
  next_route.active_control_configuration_generation = ActiveControlConfigurationGeneration(
      route.active_control_configuration_generation.value() + 1U);
  const auto second =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.2", 1U, "bundle.b", route));
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.2", 1U, "bundle.b", 3U),
                                          second.value().components,
                                          {next_route}})
                  .ok());
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value().recovery_generation, 1U);
}

TEST(SqliteCCLCJournal, IncidentRecoverUsesTheOrdinaryRequestIdentityAndReplay) {
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("component.1");
  const ComponentIncidentRecoveryDTO expected{
      "incident.1", component, LifecycleBlockingCauseClassDTO::kFault, "operator.ada", 200};
  RequestOwnerEpoch owner("owner.1");
  const auto recover = [&] { return RecoverIntent("operator.recover.1", owner); };
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    ASSERT_TRUE(journal
                    .OpenComponentIncident(TestIncident(
                        "incident.1", component, LifecycleBlockingCauseClassDTO::kFault, 100), 0U)
                    .ok());

    // A request from another owner epoch closes nothing: the ordinary check
    // refuses it as stale.
    const auto stale = RecoverIntent("operator.recover.1", RequestOwnerEpoch("owner.stale"));
    const auto stale_result = journal.RecoverComponentIncident(owner, stale, "operator.ada", 150);
    ASSERT_TRUE(stale_result.ok()) << stale_result.error().message;
    EXPECT_FALSE(stale_result.value().has_value());
    const auto stale_inspection = journal.InspectLifecycleRequest(owner, stale);
    ASSERT_TRUE(stale_inspection.ok() && stale_inspection.value().has_value());
    EXPECT_EQ(stale_inspection.value()->replay_status,
              RequestReplayStatusDTO::kStaleRequestOwnerEpoch);
    ASSERT_TRUE(journal.LoadOpenComponentIncident(component).value().has_value());

    const auto accepted = journal.RecoverComponentIncident(owner, recover(), "operator.ada", 200);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(accepted.value().has_value());
    EXPECT_EQ(accepted.value()->replay_status, RequestReplayStatusDTO::kAcceptedNew);
    EXPECT_EQ(accepted.value()->incident_recovery, expected);
    EXPECT_FALSE(journal.LoadOpenComponentIncident(component).value().has_value());

    // A newer incident opens on the same component.
    ASSERT_TRUE(journal
                    .OpenComponentIncident(
                        TestIncident("incident.2", component,
                                     LifecycleBlockingCauseClassDTO::kCollision, 300),
                        1U)
                    .ok());

    // Exact replay (attribution is not content) is idempotent: it does not
    // close the newer incident and replays the original result.
    auto retransmitted = recover();
    retransmitted.attribution = {RequestTransportDTO::kHttp,
                                 RequestAssuranceDTO::kExternallyAuthenticated,
                                 std::string("operator")};
    const auto again = journal.RecoverComponentIncident(owner, retransmitted, "operator", 400);
    ASSERT_TRUE(again.ok()) << again.error().message;
    EXPECT_FALSE(again.value().has_value());
    const auto replay = journal.InspectLifecycleRequest(owner, retransmitted);
    ASSERT_TRUE(replay.ok() && replay.value().has_value());
    EXPECT_EQ(replay.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_TRUE(replay.value()->locally_valid());
    EXPECT_EQ(replay.value()->incident_recovery, expected);

    // The same request id with other canonical content is refused and
    // closes nothing.
    auto conflicting = recover();
    conflicting.reason = "a different recover";
    const auto conflict = journal.RecoverComponentIncident(owner, conflicting, "operator.ada", 500);
    ASSERT_TRUE(conflict.ok()) << conflict.error().message;
    EXPECT_FALSE(conflict.value().has_value());
    const auto conflict_inspection = journal.InspectLifecycleRequest(owner, conflicting);
    ASSERT_TRUE(conflict_inspection.ok() && conflict_inspection.value().has_value());
    EXPECT_EQ(conflict_inspection.value()->replay_status,
              RequestReplayStatusDTO::kRequestIdConflict);
    const auto still_open = journal.LoadOpenComponentIncident(component);
    ASSERT_TRUE(still_open.ok() && still_open.value().has_value());
    EXPECT_EQ(still_open.value()->incident.incident_id, "incident.2");
    // One acknowledgement event only.
    const auto events = journal.LoadComponentIncidentEvents(0U, 16U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1U);
    EXPECT_EQ(events.value().front().event_kind, kComponentIncidentAcknowledgedEventKind);
    EXPECT_EQ(events.value().front().incident_id, "incident.1");
    journal.Close();
  }
  // The retained result survives reopen (and its validation) unchanged.
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const auto after = reopened.ReadLifecycleRequestResult(recover());
  ASSERT_TRUE(after.ok()) << after.error().message;
  ASSERT_TRUE(after.value().has_value());
  EXPECT_EQ(after.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(after.value()->incident_recovery, expected);
  // A copied earlier payload cannot be served through the acknowledged/replay reader.
  ExecRaw(JournalPath(temporary),
          "UPDATE cclc_component_incidents SET record_json=replace(record_json,"
          "'manyforge.component_incident_event.v2','manyforge.component_incident_event.v1') "
          "WHERE incident_id='incident.1'");
  const auto obsolete = reopened.ReadLifecycleRequestResult(recover());
  ASSERT_FALSE(obsolete.ok());
  EXPECT_EQ(obsolete.error().code, "component_lifecycle_journal_corrupt");
  EXPECT_EQ(RawScalar(JournalPath(temporary),
                      "SELECT count(*) FROM cclc_component_incidents WHERE incident_id='incident.1' "
                      "AND record_json LIKE '%manyforge.component_incident_event.v1%'"), 1);
}

TEST(SqliteCCLCJournal, OperatorTargetIsRecordedOnlyInItsAcceptanceCommit) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const auto route = ActiveConfiguration(ExpectedDomain());
  const ControllableComponentId component("component.1");
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  const auto plan =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                          plan.value().components,
                                          {route}})
                  .ok());
  ASSERT_FALSE(journal.LoadLifecycleTarget(component).value());
  const auto adopted_generation = plan.value().components.front().generation.value();
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());

  // The operator's Disable of the Active component.
  auto disable = DisableIntent("operator.disable.1", owner, adopted_generation);
  disable.request_context.source_label = "composer";
  const auto acceptance = Acceptance(disable, 1U, "lifecycle.1");
  const auto status = InitialDisableStatus(disable, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, disable, acceptance).ok());
  const auto nothing_recorded = [&] {
    const auto inspected = journal.InspectLifecycleRequest(owner, disable);
    ASSERT_TRUE(inspected.ok()) << inspected.error().message;
    EXPECT_FALSE(inspected.value().has_value());
    EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());
    EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().durable_generation,
              ComponentLifecycleGeneration(adopted_generation));
  };

  // An interrupted acceptance writes no target.
  ExecRaw(path,
          "CREATE TRIGGER inject_acceptance_failure BEFORE INSERT ON lifecycle_transactions "
          "BEGIN SELECT RAISE(ABORT, 'injected acceptance failure'); END");
  const auto acceptance_failed = journal.AcceptOrReplayLifecycle(owner, disable, acceptance, status);
  ExecRaw(path, "DROP TRIGGER inject_acceptance_failure");
  ASSERT_FALSE(acceptance_failed.ok());
  nothing_recorded();

  // An interrupted target write leaves no accepted request (that would keep
  // Active requested after an accepted Disable).
  ExecRaw(path,
          "CREATE TRIGGER inject_target_insert_failure BEFORE INSERT ON "
          "cclc_component_lifecycle_targets "
          "BEGIN SELECT RAISE(ABORT, 'injected target failure'); END;"
          "CREATE TRIGGER inject_target_update_failure BEFORE UPDATE ON "
          "cclc_component_lifecycle_targets "
          "BEGIN SELECT RAISE(ABORT, 'injected target failure'); END");
  const auto target_failed = journal.AcceptOrReplayLifecycle(owner, disable, acceptance, status);
  ExecRaw(path,
          "DROP TRIGGER inject_target_insert_failure; DROP TRIGGER inject_target_update_failure");
  ASSERT_FALSE(target_failed.ok());
  nothing_recorded();
  const auto unread = journal.ReadLifecycleRequestResult(disable);
  ASSERT_TRUE(unread.ok()) << unread.error().message;
  EXPECT_FALSE(unread.value().has_value());

  // Uninterrupted, both commit together; exact replay writes nothing more.
  const ComponentLifecycleTargetDTO disabled{component, LifecycleTargetStateDTO::kDisabled,
                                             LifecycleTargetOriginDTO::kOperator, 1U};
  const auto accepted = journal.AcceptOrReplayLifecycle(owner, disable, acceptance, status);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), disabled);
  const auto replayed = journal.AcceptOrReplayLifecycle(owner, disable, acceptance, status);
  ASSERT_TRUE(replayed.ok()) << replayed.error().message;
  EXPECT_EQ(replayed.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), disabled);
}

TEST(SqliteCCLCJournal, IncidentTimesUseTheFullWidthClockContract) {
  // 2^53 + 1 (not representable as a JSON number) and the largest clock.
  constexpr std::int64_t kBeyondSafe = 9'007'199'254'740'993LL;
  constexpr std::int64_t kMaximumClock = std::numeric_limits<std::int64_t>::max();
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("component.1");
  const auto record =
      TestIncident("incident.1", component, LifecycleBlockingCauseClassDTO::kFault, kBeyondSafe);
  ASSERT_TRUE(record.incident.locally_valid());
  // Encoded as canonical decimal text and decoded back exactly.
  EXPECT_NE(record.record_json.find(R"("detected_at_ns":"9007199254740993")"), std::string::npos);
  EXPECT_NE(record.record_json.find(R"("timeline":[{"at_ns":"9007199254740993",)"),
            std::string::npos);
  const auto parsed = content_identity::parse_canonical_json(record.record_json);
  ASSERT_NE(parsed.find_object_member("detected_at_ns"), nullptr);
  EXPECT_EQ(content_identity::parse_nonnegative_int64(*parsed.find_object_member("detected_at_ns")),
            kBeyondSafe);
  const auto changed = ComponentIncidentRecordWithChange(
      record.record_json, kMaximumClock, LifecycleBlockingCauseClassDTO::kFault, {});
  EXPECT_NE(changed.find(R"({"at_ns":"9223372036854775807",)"), std::string::npos);

  // The seed becomes the journal-minted owner epoch once it begins.
  RequestOwnerEpoch owner("owner.1");
  const auto recover = [&] { return RecoverIntent("operator.recover.1", owner); };
  {
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    const auto stored = journal.OpenComponentIncident(record, 0U);
    ASSERT_TRUE(stored.ok()) << stored.error().message;
    const auto loaded = journal.LoadOpenComponentIncident(component);
    ASSERT_TRUE(loaded.ok() && loaded.value().has_value());
    EXPECT_EQ(*loaded.value(), record);
    // A negative time is refused, never clamped.
    EXPECT_FALSE(journal
                     .RecoverComponentIncident(owner, RecoverIntent("operator.recover.0", owner),
                                               "operator.ada", -1)
                     .ok());
    const auto recovered =
        journal.RecoverComponentIncident(owner, recover(), "operator.ada", kMaximumClock);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_TRUE(recovered.value().has_value());
    ASSERT_TRUE(recovered.value()->incident_recovery.has_value());
    EXPECT_EQ(recovered.value()->incident_recovery->acknowledged_at_ns, kMaximumClock);
    const auto events = journal.LoadComponentIncidentEvents(0U, 16U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().size(), 1U);
    EXPECT_EQ(events.value().front().occurred_at_ns, kMaximumClock);
    EXPECT_NE(events.value().front().payload_json.find(
                  R"("acknowledged_at_ns":"9223372036854775807")"),
              std::string::npos);
    journal.Close();
  }
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
  const auto replay = reopened.ReadLifecycleRequestResult(recover());
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_TRUE(replay.value().has_value() && replay.value()->incident_recovery.has_value());
  EXPECT_EQ(replay.value()->incident_recovery->acknowledged_at_ns, kMaximumClock);

  // Negative incident times are invalid records, not clamped encodings.
  auto negative = record.incident;
  negative.detected_at_ns = -1;
  EXPECT_FALSE(negative.locally_valid());
  EXPECT_THROW(static_cast<void>(ComponentIncidentEventPayload(
                   negative,
                   LifecycleIncidentRecoveryPolicyFor(DefaultLifecycleIncidentRecoveryPolicy(),
                                                      LifecycleBlockingCauseClassDTO::kFault),
                   kComponentIncidentEventKind)),
               content_identity::CanonicalValueError);
  EXPECT_FALSE(reopened.OpenComponentIncident({negative, record.record_json}, 0U).ok());
  auto observed = record.incident;
  observed.last_good_observation =
      ComponentIncidentObservationDTO{ControlDomainId("domain.1"), 1U, -1,
                                      ComponentLifecycleStateDTO::kActive, {}};
  EXPECT_FALSE(observed.locally_valid());
  EXPECT_THROW(static_cast<void>(ComponentIncidentRecordWithChange(
                   record.record_json, -1, LifecycleBlockingCauseClassDTO::kFault, {})),
               content_identity::CanonicalValueError);
  EXPECT_FALSE((ComponentIncidentRecoveryDTO{"incident.1", component,
                                             LifecycleBlockingCauseClassDTO::kFault,
                                             "operator.ada", -1})
                   .locally_valid());
}

TEST(SqliteCCLCJournal, UnadoptedApplyPlanNamesOnlyChangedOrRemovedComponents) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto first_route = ActiveConfiguration(ExpectedDomain());
  const auto second_route = SecondDomainRoute();
  const ControllableComponentId first("component.1");
  const ControllableComponentId second("component.2");
  const auto in_plan = [&](const ControllableComponentId& component) {
    const auto result = journal.ComponentInUnadoptedApplyPlan(component);
    EXPECT_TRUE(result.ok()) << result.error().message;
    return result.ok() && result.value();
  };

  // An installing plan names its component until adopted.
  const auto installed =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", first_route));
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_TRUE(in_plan(first));
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                          installed.value().components,
                                          {first_route}})
                  .ok());
  EXPECT_FALSE(in_plan(first));

  // A retains its runtime unchanged (not named); C is installed (named).
  const auto added = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
      "apply.2", "bundle.b", {first, second},
      {{first_route.control_domain_id, first_route.active_control_configuration_generation},
       {second_route.control_domain_id, second_route.active_control_configuration_generation}},
      {first}, {}, {first_route}, {}));
  ASSERT_TRUE(added.ok()) << added.error().message;
  EXPECT_FALSE(in_plan(first));
  EXPECT_TRUE(in_plan(second));
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.2", 1U, "bundle.b", 3U),
                                          added.value().components,
                                          {first_route, second_route}})
                  .ok());
  EXPECT_FALSE(in_plan(first));
  EXPECT_FALSE(in_plan(second));

  // A is removed (named); C is retained unchanged (not named).
  const auto removed = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
      "apply.3", "bundle.c", {second},
      {{second_route.control_domain_id, second_route.active_control_configuration_generation}},
      {second}, {first}, {second_route}, {first_route.control_domain_id}));
  ASSERT_TRUE(removed.ok()) << removed.error().message;
  EXPECT_TRUE(in_plan(first));
  EXPECT_FALSE(in_plan(second));
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.3", 1U, "bundle.c", 4U),
                                          removed.value().components,
                                          {second_route}})
                  .ok());
  EXPECT_FALSE(in_plan(first));
  EXPECT_FALSE(in_plan(second));
}

TEST(SqliteCCLCJournal, OrdinaryOperatorRecoverClearsRetainedCausesInItsAcceptanceCommit) {
  ScopedTemporaryDirectory temporary;
  const ControllableComponentId component("component.1");
  const ControlDomainId domain("domain.1");
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  // A cause retained before the component first reached its target: no
  // incident is open, so the Recover is an ordinary CCLC request.
  ASSERT_TRUE(journal
                  .RecordInterventionCause(component, domain,
                                           LifecycleBlockingCauseClassDTO::kFault, 100, 0U)
                  .value());
  ASSERT_FALSE(journal.LoadOpenComponentIncident(component).value().has_value());

  // A system-attributed Recover clears nothing.
  auto system = RecoverIntent("lifecycle_target.recover.1", owner);
  system.request_context.source_label = std::string(kLifecycleTargetSourceLabel);
  const auto system_acceptance = Acceptance(system, 1U, "lifecycle.1");
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, system, system_acceptance).ok());
  const auto system_accepted = journal.AcceptOrReplayLifecycle(
      owner, system, system_acceptance, InitialStatus(system, system_acceptance));
  ASSERT_TRUE(system_accepted.ok()) << system_accepted.error().message;
  EXPECT_EQ(system_accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(journal.LoadInterventionCauses(component).value().size(), 1U);
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value().recovery_generation, 0U);

  // The operator's Recover clears it in its acceptance commit.
  auto operator_recover = Intent("operator.recover.1", owner, 2U, "recover");
  operator_recover.intent = ComponentLifecycleIntentKindDTO::kRecover;
  operator_recover.request_context.source_label = "composer";
  const auto acceptance = Acceptance(operator_recover, 2U, "lifecycle.2");
  const auto status = InitialStatus(operator_recover, acceptance);
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, operator_recover, acceptance).ok());
  const auto accepted = journal.AcceptOrReplayLifecycle(owner, operator_recover, acceptance, status);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_TRUE(journal.LoadInterventionCauses(component).value().empty());
  // A driver pass that read the cause before this acceptance may not retain
  // it again: the acceptance advanced the recovery generation.
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value().recovery_generation, 1U);
  const auto stale =
      journal.RecordInterventionCause(component, domain,
                                      LifecycleBlockingCauseClassDTO::kFault, 250, 0U);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, kLifecycleFactsStaleErrorCode);
  EXPECT_TRUE(journal.LoadInterventionCauses(component).value().empty());

  // A cause retained after the acceptance survives the exact replay.
  ASSERT_TRUE(journal
                  .RecordInterventionCause(component, domain,
                                           LifecycleBlockingCauseClassDTO::kEmergencyStop, 300, 1U)
                  .value());
  const auto replayed = journal.AcceptOrReplayLifecycle(owner, operator_recover, acceptance, status);
  ASSERT_TRUE(replayed.ok()) << replayed.error().message;
  EXPECT_EQ(replayed.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  const auto causes = journal.LoadInterventionCauses(component);
  ASSERT_TRUE(causes.ok());
  ASSERT_EQ(causes.value().size(), 1U);
  EXPECT_EQ(causes.value().front().cause_class, LifecycleBlockingCauseClassDTO::kEmergencyStop);
  // The exact replay is no second Recover: the generation is unchanged.
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value().recovery_generation, 1U);
}

TEST(SqliteCCLCJournal, RecoverRefusesDriverWritesBasedOnFactsReadBeforeIt) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto route = ActiveConfiguration(ExpectedDomain());
  const ControllableComponentId component("component.1");
  const ControlDomainId domain("domain.1");
  const auto plan =
      journal.PlanNativeApply(NativeApplyPlanRequest("apply.1", 1U, "bundle.a", route));
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_TRUE(journal
                  .AdoptNativeApplyFacts({NativeApplyPublication("apply.1", 1U, "bundle.a", 2U),
                                          plan.value().components,
                                          {route}})
                  .ok());
  RequestOwnerEpoch owner("owner.1");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  // Incident retirement leaves this Prepared pursuit present; recovery
  // still fences every marker/cause/incident write from earlier facts.
  ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  ASSERT_TRUE(journal.RecordLifecycleTargetReached(component, true, 0U).value());
  ASSERT_TRUE(journal
                  .RecordInterventionCause(component, domain,
                                           LifecycleBlockingCauseClassDTO::kEmergencyStop, 100, 0U)
                  .value());
  ASSERT_TRUE(journal
                  .OpenComponentIncident(
                      TestIncident("incident.1", component,
                                   LifecycleBlockingCauseClassDTO::kEmergencyStop, 100),
                      0U)
                  .ok());

  // A driver pass reads every fact it decides from in one snapshot ...
  const auto before = journal.LoadLifecycleTargetFacts(component);
  ASSERT_TRUE(before.ok()) << before.error().message;
  ASSERT_TRUE(before.value().target.has_value());
  EXPECT_TRUE(before.value().target->reached);
  EXPECT_EQ(before.value().recovery_generation, 0U);
  ASSERT_EQ(before.value().intervention_causes.size(), 1U);
  ASSERT_TRUE(before.value().open_incident.has_value());
  const auto read_generation = before.value().recovery_generation;

  // ... and the operator's Recover is accepted before the pass writes.
  const auto recovered = journal.RecoverComponentIncident(
      owner, RecoverIntent("operator.recover.1", owner), "operator.ada", 200);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_TRUE(recovered.value().has_value());
  const auto after = journal.LoadLifecycleTargetFacts(component);
  ASSERT_TRUE(after.ok()) << after.error().message;
  EXPECT_EQ(after.value().recovery_generation, 1U);
  EXPECT_TRUE(after.value().intervention_causes.empty());
  EXPECT_FALSE(after.value().open_incident.has_value());
  ASSERT_TRUE(after.value().target.has_value());
  EXPECT_FALSE(after.value().target->reached);

  // Every write based on the earlier facts is refused and writes nothing:
  // the cause stays cleared, the incident closed, the marker reset.
  const auto stale_cause = journal.RecordInterventionCause(
      component, domain, LifecycleBlockingCauseClassDTO::kEmergencyStop, 210, read_generation);
  ASSERT_FALSE(stale_cause.ok());
  EXPECT_EQ(stale_cause.error().code, kLifecycleFactsStaleErrorCode);
  const auto stale_incident = journal.OpenComponentIncident(
      TestIncident("incident.2", component, LifecycleBlockingCauseClassDTO::kEmergencyStop, 210),
      read_generation);
  ASSERT_FALSE(stale_incident.ok());
  EXPECT_EQ(stale_incident.error().code, kLifecycleFactsStaleErrorCode);
  const auto stale_reached = journal.RecordLifecycleTargetReached(component, true, read_generation);
  ASSERT_FALSE(stale_reached.ok());
  EXPECT_EQ(stale_reached.error().code, kLifecycleFactsStaleErrorCode);
  // The closed incident refuses an append by its own open-only condition.
  EXPECT_FALSE(journal
                   .UpdateOpenComponentIncident("incident.1",
                                                LifecycleBlockingCauseClassDTO::kCollision, {},
                                                before.value().open_incident->record_json)
                   .value());
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value(), after.value());

  // A pass that read the Recover's facts writes normally.
  EXPECT_TRUE(journal
                  .RecordInterventionCause(component, domain,
                                           LifecycleBlockingCauseClassDTO::kCollision, 220,
                                           after.value().recovery_generation)
                  .value());
  EXPECT_EQ(journal.LoadInterventionCauses(component).value().size(), 1U);
}

[[nodiscard]] ComponentLifecycleStatusDTO RetargetApplyStatus(
    ComponentLifecycleStatusDTO status, ComponentLifecycleIntentKindDTO kind, bool initial = false) {
  status.accepted_intent.intent = kind;
  const auto target = kind == ComponentLifecycleIntentKindDTO::kPrepare
      ? ComponentLifecycleStateDTO::kPrepared
      : kind == ComponentLifecycleIntentKindDTO::kDisable
          ? ComponentLifecycleStateDTO::kDisabled : ComponentLifecycleStateDTO::kActive;
  status.requested_state = {ComponentLifecycleRequestedStatePresenceDTO::kResolved, target};
  for (auto& domain : status.domains) domain.requested_state = status.requested_state;
  auto& evidence = *status.apply_bring_up;
  if (initial) {
    status.previous_state.reset();
    status.current_state.reset();
    for (auto& expected : status.accepted_intent.expected_control_domains) {
      expected.controller().expected_active_control_configuration = {
          ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
    }
    for (auto& domain : status.domains) {
      domain.expected_domain = status.accepted_intent.expected_control_domains.front();
      domain.current_scope.reset();
      domain.resolved_provider_occurrence.reset();
      domain.previous_state.reset();
      domain.current_state.reset();
    }
    for (auto& domain : evidence.selected_scope.domains) domain.predecessor_control_domain_revision.reset();
    for (auto& binding : evidence.selected_scope.bindings) binding.predecessor_control_binding_revision.reset();
    evidence.parent_authority.held_fence_query.expected_scope = evidence.selected_scope;
  }
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id, evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch, evidence.parent_authority.parent);
  return status;
}

[[nodiscard]] NativeApplyPlanRequestDTO LinkedNativeRequest(
    const ComponentLifecycleStatusDTO& status) {
  const auto& evidence = *status.apply_bring_up;
  auto request = NativeApplyPlanRequest(evidence.parent_authority.parent.owner.apply_id.value(),
      evidence.parent_authority.parent.owner.apply_generation.value(),
      evidence.candidate_projection_revision.retained_bundle_id.value(),
      ActiveConfiguration(status.accepted_intent.expected_control_domains.front()));
  request.candidate_graph_revision = evidence.candidate_projection_revision.graph_revision;
  request.component_bring_up = NativeApplyComponentBringUpDTO{
      evidence.parent_authority.parent,
      {{status.accepted_intent.request_context, status.accepted_intent.controllable_component_id,
        status.acceptance, evidence.original_request_payload_digest}}};
  // Ordinary ROS planning would choose Declared; linked rows must instead
  // retain their exact child target and already accepted generation.
  request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  return request;
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO LinkedNativePublication(
    const ComponentLifecycleStatusDTO& status) {
  const auto& evidence = *status.apply_bring_up;
  const NamedApplyPublicationTransactionDTO apply{evidence.parent_authority.parent.owner.apply_id,
                                                 evidence.parent_authority.parent.owner.apply_generation};
  return {{apply, evidence.candidate_projection_revision,
           evidence.candidate_projection_revision.retained_bundle_id,
           evidence.candidate_projection_revision.graph_revision,
           ActiveBundlePublicationGeneration(2U), ContentDigest(Sha('c'))},
          ActiveBundlePublicationGeneration(3U), ContentDigest(Sha('d'))};
}

[[nodiscard]] ComponentLifecycleStatusDTO InstalledLinkedChild(ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.latest_updated_at_ns += 1;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  status.current_state = ComponentLifecycleStateDTO::kDeclared;
  for (auto& domain : status.domains) {
    const auto& expected = *domain.expected_domain;
    auto provider = CurrentProviderOccurrence();
    provider.control_domain_id = expected.control_domain_id;
    provider.control_domain_revision = expected.control_domain_revision;
    domain.current_scope =
        ResolvedLifecycleControlDomainScopeDTO{expected.control_domain_id,
                                               expected.control_domain_revision,
                                               expected.controller().controller_topology_id,
                                               expected.controller().topology_revision,
                                               std::nullopt,
                                               expected.controller().expected_control_bindings,
                                               status.apply_bring_up->selected_scope.resources};
    domain.resolved_provider_occurrence = provider;
    domain.resolved_provider_occurrence->provider_instance_id = ProviderInstanceId("installed.original");
    if (!domain.previous_state) domain.previous_state = ComponentLifecycleStateDTO::kDeclared;
    domain.current_state = ComponentLifecycleStateDTO::kDeclared;
    domain.action_state = ComponentLifecycleDomainActionStateDTO::kReserved;
    domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  }
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO HoldingLinkedChild(ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.latest_updated_at_ns += 1;
  status.current_state = status.requested_state.state;
  for (auto& domain : status.domains) {
    domain.current_state = status.requested_state.state;
    domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
  }
  return status;
}

[[nodiscard]] ComponentLifecycleStatusDTO TerminalLinkedChild(ComponentLifecycleStatusDTO status) {
  status.status_sequence += 1U;
  status.latest_updated_at_ns += 1;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
  status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
  status.remaining_duration_ns = 0;
  return status;
}

TEST(SqliteCCLCJournal, LinkedNativePlanAndAdoptionUseOneAcceptanceGenerationForInitialAndLowerTargets) {
  for (const auto initial : {false, true}) {
    for (const auto& [kind, preserve_startup_target] : {
             std::pair{ComponentLifecycleIntentKindDTO::kPrepare, false},
             std::pair{ComponentLifecycleIntentKindDTO::kEnable, false},
             std::pair{ComponentLifecycleIntentKindDTO::kDisable, false},
             std::pair{ComponentLifecycleIntentKindDTO::kPrepare, true}}) {
      if (initial && preserve_startup_target) continue;
      SCOPED_TRACE(initial);
      SCOPED_TRACE(static_cast<unsigned>(kind));
      SCOPED_TRACE(preserve_startup_target);
      ScopedTemporaryDirectory temporary;
      SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
      const ControllableComponentId component("component.1");
      RequestOwnerEpoch owner("owner.linked.native");
      ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
      if (!initial) {
        ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
        // Explicit retained legacy row for the unwired linked branch; M
        // configuration adoption must never create this row implicitly.
        ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                               LifecycleTargetOriginDTO::kStartup).ok());
        if (!preserve_startup_target && kind != ComponentLifecycleIntentKindDTO::kEnable) {
          ASSERT_TRUE(journal.SetLifecycleTarget(component,
              kind == ComponentLifecycleIntentKindDTO::kPrepare ? LifecycleTargetStateDTO::kPrepared
                                                               : LifecycleTargetStateDTO::kDisabled,
              LifecycleTargetOriginDTO::kOperator).ok());
        }
      }
      const auto target_before = journal.LoadLifecycleTarget(component);
      ASSERT_TRUE(target_before.ok());
      ASSERT_EQ(target_before.value().has_value(), !initial);
      auto child = RetargetApplyStatus(ApplyBringUpStatus(journal, owner), kind, initial);
      ASSERT_TRUE(child.locally_valid());
      const auto accepted = journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
          child.acceptance, child, child.apply_bring_up->original_request_payload_digest);
      ASSERT_TRUE(accepted.ok()) << accepted.error().message;
      const auto accepted_bytes = EncodeApplyDelegatedComponentLifecycleRequestResult(accepted.value());
      EXPECT_EQ(EncodeApplyDelegatedComponentLifecycleRequestResult(
          DecodeApplyDelegatedComponentLifecycleRequestResult(accepted_bytes)), accepted_bytes);
      const auto request = LinkedNativeRequest(child);
      const auto planned = journal.PlanNativeApply(request);
      ASSERT_TRUE(planned.ok()) << planned.error().message;
      ASSERT_EQ(planned.value().components.size(), 1U);
      EXPECT_EQ(planned.value().components.front().generation, child.acceptance.component_lifecycle_generation);
      EXPECT_EQ(planned.value().components.front().state, *child.requested_state.state);
      EXPECT_EQ(planned.value().component_bring_up, request.component_bring_up);
      auto route = ActiveConfiguration(child.accepted_intent.expected_control_domains.front());
      route.active_control_configuration_generation = planned.value().routes.front().generation;
      const auto publication = LinkedNativePublication(child);
      const NativeApplyFactAdoptionDTO adoption{
          publication, planned.value().components, {route}, request.component_bring_up};
      EXPECT_FALSE(journal.AdoptNativeApplyFacts(adoption).ok());
      child = InstalledLinkedChild(child);
      ASSERT_TRUE(child.locally_valid());
      ASSERT_TRUE(journal.RecordLifecycleStatus(owner, child).ok());
      // Exact plan replay is observation-only after installation.
      EXPECT_EQ(journal.PlanNativeApply(request).value(), planned.value());
      child = HoldingLinkedChild(child);
      ASSERT_TRUE(child.locally_valid());
      ASSERT_TRUE(journal.RecordLifecycleStatus(owner, child).ok());
      const auto holding = journal.AdoptNativeApplyFacts(adoption);
      ASSERT_FALSE(holding.ok());
      EXPECT_EQ(holding.error().code, "component_lifecycle_native_apply_unsettled");
      EXPECT_FALSE(journal.HasNativeApplyFacts(publication).value());
      child = TerminalLinkedChild(child);
      ASSERT_TRUE(child.locally_valid());
      ASSERT_TRUE(journal.RecordLifecycleStatus(owner, child).ok());
      auto foreign = adoption;
      foreign.publication.request.candidate_projection_revision.scene_revision.revision_id = 2U;
      ASSERT_TRUE(foreign.publication.valid());
      EXPECT_FALSE(journal.AdoptNativeApplyFacts(foreign).ok());
      foreign = adoption;
      foreign.current_routes.front().source_runtime_epoch = RuntimeEpoch("foreign.provider");
      ASSERT_TRUE(foreign.current_routes.front().locally_valid());
      EXPECT_FALSE(journal.AdoptNativeApplyFacts(foreign).ok());
      const auto adopted = journal.AdoptNativeApplyFacts(adoption);
      ASSERT_TRUE(adopted.ok()) << adopted.error().message;
      EXPECT_TRUE(journal.HasNativeApplyFacts(publication).value());
      const auto current = journal.LoadCurrentComponentLifecycleFact(child.accepted_intent.controllable_component_id);
      ASSERT_TRUE(current.ok());
      EXPECT_EQ(current.value().durable_generation, child.acceptance.component_lifecycle_generation);
      EXPECT_EQ(current.value().current_state, child.requested_state.state);
      const auto target = journal.LoadLifecycleTarget(child.accepted_intent.controllable_component_id);
      ASSERT_TRUE(target.ok());
      ASSERT_TRUE(target.value());
      if (initial) {
        EXPECT_EQ(target.value()->requested_state,
            kind == ComponentLifecycleIntentKindDTO::kDisable ? LifecycleTargetStateDTO::kDisabled
                                                             : LifecycleTargetStateDTO::kActive);
        EXPECT_EQ(target.value()->origin,
            kind == ComponentLifecycleIntentKindDTO::kDisable ? LifecycleTargetOriginDTO::kOperator
                                                             : LifecycleTargetOriginDTO::kStartup);
      } else {
        // Genuine desired targets survive the child step, including Active
        // while this child has only established Prepared.
        EXPECT_EQ(target.value(), target_before.value());
      }
      EXPECT_TRUE(journal.AdoptNativeApplyFacts(adoption).ok());
      EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), target.value());
      journal.Close();
      SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"));
      EXPECT_EQ(reopened.LoadNativeApplyPlan(request.apply).value(), planned.value());
      EXPECT_TRUE(reopened.HasNativeApplyFacts(publication).value());
      EXPECT_EQ(reopened.LoadLifecycleTarget(component).value(), target.value());
    }
  }
}

TEST(SqliteCCLCJournal, LinkedNativePlanRefusesForeignReceiptsBeforeWritingAndKeepsRetainedPeerFacts) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto first = ActiveConfiguration(ExpectedDomain());
  const auto second = SecondDomainRoute();
  const auto baseline = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
      "apply.baseline", "bundle.a", {ControllableComponentId("component.1"), ControllableComponentId("component.2")},
      {{first.control_domain_id, first.active_control_configuration_generation},
       {second.control_domain_id, second.active_control_configuration_generation}}, {}, {}, {}, {}));
  ASSERT_TRUE(baseline.ok());
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.baseline", 1U, "bundle.a", 2U),
      baseline.value().components, {first, second}}).ok());
  const auto retained_before = journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.2"));
  ASSERT_TRUE(retained_before.ok());
  RequestOwnerEpoch owner("owner.linked.native");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto child = RetargetApplyStatus(ApplyBringUpStatus(journal, owner), ComponentLifecycleIntentKindDTO::kPrepare);
  ASSERT_TRUE(child.locally_valid());
  ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent, child.acceptance,
      child, child.apply_bring_up->original_request_payload_digest).ok());
  auto request = LinkedNativeRequest(child);
  request.components.push_back(ControllableComponentId("component.2"));
  request.compiled_routes.push_back({second.control_domain_id, second.active_control_configuration_generation});
  request.retained_components = {ControllableComponentId("component.2")};
  request.retained_routes = {second};
  // Each negative starts from an actual valid accepted receipt.
  for (unsigned field = 0U; field != 7U; ++field) {
    SCOPED_TRACE(field);
    auto foreign = request;
    auto& receipt = foreign.component_bring_up->children.front();
    if (field == 0U) foreign.component_bring_up->parent.plan_digest = ContentDigest(Sha('f'));
    if (field == 1U) receipt.request_context.request_owner_epoch = RequestOwnerEpoch("foreign.owner");
    if (field == 2U) receipt.request_context.source_label = "foreign.source";
    if (field == 3U) receipt.acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(99U);
    if (field == 4U) receipt.original_request_payload_digest = ContentDigest(Sha('f'));
    if (field == 5U) foreign.candidate_graph_revision = GraphRevision("foreign.graph");
    if (field == 6U) foreign.compiled_routes.erase(foreign.compiled_routes.begin());
    EXPECT_FALSE(journal.PlanNativeApply(foreign).ok());
    EXPECT_FALSE(journal.LoadNativeApplyPlan(request.apply).value());
  }
  const auto planned = journal.PlanNativeApply(request);
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_EQ(planned.value().components.front().generation, child.acceptance.component_lifecycle_generation);
  EXPECT_EQ(planned.value().components.back().state, *retained_before.value().current_state);
  EXPECT_EQ(planned.value().components.back().generation, *retained_before.value().durable_generation);
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(ControllableComponentId("component.2")).value(), retained_before.value());
  EXPECT_EQ(planned.value().retained_components, request.retained_components);
  EXPECT_EQ(planned.value().retained_domains, (std::vector<ControlDomainId>{second.control_domain_id}));
}

TEST(SqliteCCLCJournal, LinkedNativeAdoptionPreservesMixedRequestedTargetsAndIndependentReceipts) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  auto first_route = ActiveConfiguration(ExpectedDomain());
  auto second_route = SecondDomainRoute();
  const auto baseline = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
      "apply.baseline", "bundle.a", {ControllableComponentId("component.1"), ControllableComponentId("component.2")},
      {{first_route.control_domain_id, first_route.active_control_configuration_generation},
       {second_route.control_domain_id, second_route.active_control_configuration_generation}}, {}, {}, {}, {}));
  ASSERT_TRUE(baseline.ok());
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.baseline", 1U, "bundle.a", 2U),
      baseline.value().components, {first_route, second_route}}).ok());
  const auto first_target = journal.SetLifecycleTarget(ControllableComponentId("component.1"),
      LifecycleTargetStateDTO::kPrepared, LifecycleTargetOriginDTO::kOperator);
  const auto second_target = journal.SetLifecycleTarget(ControllableComponentId("component.2"),
      LifecycleTargetStateDTO::kDisabled, LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(first_target.ok());
  ASSERT_TRUE(second_target.ok());
  RequestOwnerEpoch owner("owner.linked.mixed");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  auto first = RetargetApplyStatus(ApplyBringUpStatus(journal, owner), ComponentLifecycleIntentKindDTO::kPrepare);
  auto second = first;
  second.accepted_intent.controllable_component_id = ControllableComponentId("component.2");
  second.accepted_intent.request_context.request_id = RequestId("apply.child.2");
  const auto selection = journal.ReadApplyComponentGenerationSelection(owner, second.accepted_intent.controllable_component_id);
  ASSERT_TRUE(selection.ok());
  ASSERT_TRUE(selection.value().predecessor_generation);
  second.accepted_intent.apply_generation_selection = selection.value();
  second.accepted_intent.expected_component_lifecycle_generation = *selection.value().predecessor_generation;
  second.acceptance = {second.accepted_intent.request_context.request_id, RequestReplayStatusDTO::kAcceptedNew,
      LifecycleTransactionId("apply-lifecycle.2"), LifecycleTransactionGeneration(2U),
      selection.value().candidate_generation, first.acceptance.accepted_at_ns, first.acceptance.timeout_duration_ns};
  auto& expected = second.accepted_intent.expected_control_domains.front();
  expected.control_domain_id = second_route.control_domain_id;
  expected.controller().expected_control_bindings.front().control_binding_id = second_route.binding_routes.front().control_binding_id;
  expected.controller().expected_active_control_configuration.active_control_configuration_id =
      second_route.active_control_configuration_id;
  auto& domain = second.domains.front();
  domain.control_domain_id = expected.control_domain_id;
  domain.expected_domain = expected;
  domain.current_scope->control_domain_id = expected.control_domain_id;
  domain.current_scope->controller().current_control_bindings = expected.controller().expected_control_bindings;
  domain.current_scope->controller().active_control_configuration = second_route;
  domain.current_scope->controller().current_resources = {ResourceId("resource.2")};
  domain.resolved_provider_occurrence->control_domain_id = expected.control_domain_id;
  domain.resolved_provider_occurrence->provider_instance_id = ProviderInstanceId("provider.2");
  auto& second_scope = second.apply_bring_up->selected_scope;
  second_scope.controllable_component_ids = {second.accepted_intent.controllable_component_id};
  second_scope.domains.front().control_domain_id = expected.control_domain_id;
  second_scope.bindings.front().control_binding_id = expected.controller().expected_control_bindings.front().control_binding_id;
  second_scope.resources = {ResourceId("resource.2")};
  auto aggregate = first.apply_bring_up->selected_scope;
  aggregate.controllable_component_ids.push_back(second.accepted_intent.controllable_component_id);
  aggregate.domains.push_back(second_scope.domains.front());
  aggregate.bindings.push_back(second_scope.bindings.front());
  aggregate.resources.push_back(ResourceId("resource.2"));
  first.apply_bring_up->parent_authority.held_fence_query.expected_scope = aggregate;
  second.apply_bring_up->parent_authority = first.apply_bring_up->parent_authority;
  second = RetargetApplyStatus(std::move(second), ComponentLifecycleIntentKindDTO::kDisable);
  ASSERT_TRUE(first.locally_valid());
  ASSERT_TRUE(second.locally_valid());
  ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, first.accepted_intent, first.acceptance,
      first, first.apply_bring_up->original_request_payload_digest).ok());
  ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, second.accepted_intent, second.acceptance,
      second, second.apply_bring_up->original_request_payload_digest).ok());
  auto request = LinkedNativeRequest(first);
  const auto second_request = LinkedNativeRequest(second);
  request.components.push_back(second.accepted_intent.controllable_component_id);
  request.compiled_routes.push_back(second_request.compiled_routes.front());
  request.component_bring_up->children.push_back(second_request.component_bring_up->children.front());
  const auto planned = journal.PlanNativeApply(request);
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  ASSERT_EQ(planned.value().components.size(), 2U);
  EXPECT_EQ(planned.value().components.front().state, ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(planned.value().components.back().state, ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(planned.value().components.front().generation, first.acceptance.component_lifecycle_generation);
  EXPECT_EQ(planned.value().components.back().generation, second.acceptance.component_lifecycle_generation);
  for (auto* child : {&first, &second}) {
    *child = InstalledLinkedChild(*child);
    ASSERT_TRUE(child->locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(owner, *child).ok());
    *child = HoldingLinkedChild(*child);
    ASSERT_TRUE(child->locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(owner, *child).ok());
    *child = TerminalLinkedChild(*child);
    ASSERT_TRUE(child->locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(owner, *child).ok());
  }
  first_route.active_control_configuration_generation = planned.value().routes.front().generation;
  second_route.active_control_configuration_generation = planned.value().routes.back().generation;
  const NativeApplyFactAdoptionDTO adoption{LinkedNativePublication(first), planned.value().components,
      {first_route, second_route}, request.component_bring_up};
  const auto adopted = journal.AdoptNativeApplyFacts(adoption);
  ASSERT_TRUE(adopted.ok()) << adopted.error().message;
  EXPECT_EQ(journal.LoadLifecycleTarget(first.accepted_intent.controllable_component_id).value(),
            first_target.value());
  EXPECT_EQ(journal.LoadLifecycleTarget(second.accepted_intent.controllable_component_id).value(),
            second_target.value());
}

TEST(SqliteCCLCJournal, LinkedNativeAcceptedGenerationHonorsReservationsAndHistoricalReadd) {
  for (const auto prior_watermark : {0U, 7U}) {
    SCOPED_TRACE(prior_watermark);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    auto route = ActiveConfiguration(ExpectedDomain());
    for (unsigned index = 1U; index <= prior_watermark; ++index) {
      const auto id = "apply.history." + std::to_string(index);
      const auto plan = journal.PlanNativeApply(NativeApplyPlanRequest(id, 1U, "bundle.a", route));
      ASSERT_TRUE(plan.ok());
      route.active_control_configuration_generation = plan.value().routes.front().generation;
      ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication(id, 1U, "bundle.a", index + 1U),
          plan.value().components, {route}}).ok());
    }
    const auto reservation = prior_watermark == 0U ? 8U : 9U;
    for (unsigned index = prior_watermark + 1U; index <= reservation; ++index) {
      const auto plan = journal.PlanNativeApply(NativeApplyPlanRequest(
          "apply.abandoned." + std::to_string(index), 1U, "bundle.a", route));
      ASSERT_TRUE(plan.ok());
      EXPECT_EQ(plan.value().components.front().generation, ComponentLifecycleGeneration(index));
    }
    const auto removal = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
        "apply.removal", "bundle.removal", {}, {}, {}, {ControllableComponentId("component.1")},
        {}, {route.control_domain_id}));
    ASSERT_TRUE(removal.ok());
    ASSERT_TRUE(journal.AdoptNativeApplyFacts({
        NativeApplyPublication("apply.removal", 1U, "bundle.removal", reservation + 2U), {}, {}}).ok());
    RequestOwnerEpoch owner("owner.linked.readd");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    auto child = RetargetApplyStatus(ApplyBringUpStatus(journal, owner), ComponentLifecycleIntentKindDTO::kPrepare, true);
    ASSERT_TRUE(child.locally_valid());
    EXPECT_EQ(child.accepted_intent.apply_generation_selection->prior_reservation_generation, reservation);
    EXPECT_EQ(child.acceptance.component_lifecycle_generation, ComponentLifecycleGeneration(reservation + 1U));
    ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
        child.acceptance, child, child.apply_bring_up->original_request_payload_digest).ok());
    const auto accepted_fact = journal.LoadCurrentComponentLifecycleFact(child.accepted_intent.controllable_component_id);
    ASSERT_TRUE(accepted_fact.ok());
    EXPECT_EQ(accepted_fact.value().durable_generation, child.acceptance.component_lifecycle_generation);
    const auto planned = journal.PlanNativeApply(LinkedNativeRequest(child));
    ASSERT_TRUE(planned.ok()) << planned.error().message;
    EXPECT_EQ(planned.value().components.front().generation, child.acceptance.component_lifecycle_generation);
    EXPECT_EQ(journal.ReadApplyComponentGenerationSelection(owner,
        child.accepted_intent.controllable_component_id).value().candidate_generation,
        ComponentLifecycleGeneration(reservation + 2U));
  }
}

TEST(SqliteCCLCJournal, LinkedNativeFailedOrUnknownChildrenDoNotAuthorizeAdoption) {
  for (const auto unknown : {false, true}) {
    SCOPED_TRACE(unknown);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
    RequestOwnerEpoch owner("owner.linked.failure");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    auto child = RetargetApplyStatus(ApplyBringUpStatus(journal, owner), ComponentLifecycleIntentKindDTO::kPrepare);
    ASSERT_TRUE(child.locally_valid());
    ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
        child.acceptance, child, child.apply_bring_up->original_request_payload_digest).ok());
    const auto request = LinkedNativeRequest(child);
    const auto planned = journal.PlanNativeApply(request);
    ASSERT_TRUE(planned.ok());
    child = InstalledLinkedChild(child);
    ASSERT_TRUE(child.locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(owner, child).ok());
    child.status_sequence += 1U;
    child.latest_updated_at_ns += 1;
    child.remaining_duration_ns = 0;
    child.error = ErrorDTO{unknown ? "apply_parent_publication_unknown" : "apply_child_prepare_failed",
        unknown ? "installed candidate lacks a known parent publication" : "candidate preparation refused before a primitive",
        {{"effect", unknown ? "unknown" : "none"}}};
    child.reconciliation_required = unknown;
    if (unknown) {
      child.phase = ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown;
      // Installation is retained; no brand primitive was dispatched.
    } else {
      child.phase = ComponentLifecycleTransactionPhaseDTO::kFailed;
      child.terminal_result = ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect;
      child.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
      child.domains.front().error = child.error;
    }
    ASSERT_TRUE(child.locally_valid());
    ASSERT_TRUE(journal.RecordLifecycleStatus(owner, child).ok());
    auto route = ActiveConfiguration(child.accepted_intent.expected_control_domains.front());
    route.active_control_configuration_generation = planned.value().routes.front().generation;
    const auto publication = LinkedNativePublication(child);
    EXPECT_FALSE(journal.AdoptNativeApplyFacts({publication, planned.value().components,
        {route}, request.component_bring_up}).ok());
    EXPECT_FALSE(journal.HasNativeApplyFacts(publication).value());
    EXPECT_EQ(journal.LoadNativeApplyPlan(request.apply).value(), planned.value());
    EXPECT_EQ(journal.ReadUnsettledNativeApplyPlan().value(), request.apply);
    EXPECT_EQ(journal.LoadRetainedLifecycleStatus(child.acceptance.lifecycle_transaction_id).value(), child);
  }
}

TEST(SqliteCCLCJournal, LinkedNativeAcceptedPlanReopensAsDataAndCannotBeAdoptedByReplacementOwner) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  RequestOwnerEpoch owner("owner.linked.original");
  std::optional<NativeApplyPlanDTO> saved_plan;
  std::optional<ComponentLifecycleStatusDTO> saved_child;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    saved_child = RetargetApplyStatus(ApplyBringUpStatus(journal, owner), ComponentLifecycleIntentKindDTO::kPrepare);
    ASSERT_TRUE(saved_child->locally_valid());
    ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, saved_child->accepted_intent,
        saved_child->acceptance, *saved_child, saved_child->apply_bring_up->original_request_payload_digest).ok());
    const auto planned = journal.PlanNativeApply(LinkedNativeRequest(*saved_child));
    ASSERT_TRUE(planned.ok()) << planned.error().message;
    saved_plan = planned.value();
  }
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  RequestOwnerEpoch replacement("owner.linked.replacement");
  const auto recovery = BeginTestOwnerEpoch(reopened, replacement, 200, kDefaultCCLCExactResultHistory);
  ASSERT_TRUE(recovery.ok());
  ASSERT_EQ(recovery.value().interrupted_transactions.size(), 1U);
  EXPECT_EQ(recovery.value().interrupted_transactions.front(), *saved_child);
  EXPECT_TRUE(recovery.value().cell_claims.empty());
  EXPECT_EQ(reopened.LoadNativeApplyPlan(saved_plan->apply).value(), saved_plan);
  EXPECT_EQ(reopened.PlanNativeApply(LinkedNativeRequest(*saved_child)).value(), *saved_plan);
  auto route = ActiveConfiguration(saved_child->accepted_intent.expected_control_domains.front());
  route.active_control_configuration_generation = saved_plan->routes.front().generation;
  const auto refused = reopened.AdoptNativeApplyFacts({LinkedNativePublication(*saved_child),
      saved_plan->components, {route}, saved_plan->component_bring_up});
  ASSERT_FALSE(refused.ok());
  EXPECT_FALSE(reopened.HasNativeApplyFacts(LinkedNativePublication(*saved_child)).value());
  EXPECT_EQ(reopened.ReadUnsettledNativeApplyPlan().value(), saved_plan->apply);
  EXPECT_EQ(reopened.LoadComponentsWithUnsettledLifecycle().value(),
            (std::vector<ControllableComponentId>{saved_child->accepted_intent.controllable_component_id}));
}

TEST(SqliteCCLCJournal, EarlierDraftV17WithoutLinkedNativeColumnIsRefusedWithoutChangingInterruptedFile) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
    RequestOwnerEpoch owner("owner.linked.draft");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    const auto child = ApplyBringUpStatus(journal, owner);
    ASSERT_TRUE(journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
        child.acceptance, child, child.apply_bring_up->original_request_payload_digest).ok());
  }
  ExecRaw(path, "ALTER TABLE cclc_native_apply_plans DROP COLUMN component_bring_up");
  const auto bytes = [&] {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto before = bytes();
  ASSERT_FALSE(before.empty());
  try {
    SqliteCCLCJournal unsupported(path, CellId("cell.1"));
    FAIL() << "earlier draft was opened";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("unsupported earlier-draft"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("File left unchanged"), std::string::npos);
  }
  EXPECT_EQ(bytes(), before);
}

[[nodiscard]] ComponentLifecycleCurrentSnapshotDTO QualifiedApplySnapshot(
    ComponentLifecycleGeneration generation = ComponentLifecycleGeneration(1U),
    ComponentLifecycleStateDTO state = ComponentLifecycleStateDTO::kDeclared) {
  ComponentLifecycleCurrentSnapshotDTO snapshot{
      ContentDigest(Sha('0')), ControllableComponentId("component.1"), generation, state,
      NativeApplyPublication("apply.bootstrap", 1U, "bundle.bootstrap", 2U),
      {CurrentScope()}, {CurrentProviderOccurrence()}};
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
  if (!digest.ok()) throw std::runtime_error(digest.error().message);
  snapshot.snapshot_digest = digest.value();
  if (!VerifyComponentLifecycleCurrentSnapshot(snapshot).ok())
    throw std::runtime_error("qualified Apply snapshot was not canonically verified");
  return snapshot;
}

[[nodiscard]] ComponentLifecycleStatusDTO QualifiedApplyStatus(
    SqliteCCLCJournal& journal, const RequestOwnerEpoch& owner,
    const ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  if (!VerifyComponentLifecycleCurrentSnapshot(snapshot).ok())
    throw std::runtime_error("qualified Apply predecessor is not verified");
  const ApplyComponentQualifiedPredecessorDTO predecessor{
      snapshot.component_lifecycle_generation, snapshot.snapshot_digest};
  const auto selection = journal.ReadApplyComponentGenerationSelection(owner,
      snapshot.controllable_component_id, predecessor);
  if (!selection.ok()) throw std::runtime_error(selection.error().message);
  auto status = ApplyBringUpStatus(journal, owner);
  status.accepted_intent.apply_generation_selection = selection.value();
  status.accepted_intent.expected_component_lifecycle_generation = snapshot.component_lifecycle_generation;
  status.accepted_intent.expected_graph_revision = snapshot.active_publication_receipt.request.candidate_graph_revision;
  status.acceptance.component_lifecycle_generation = selection.value().candidate_generation;
  status.previous_state = snapshot.component_lifecycle_state;
  status.current_state = snapshot.component_lifecycle_state;
  auto& expected = status.accepted_intent.expected_control_domains.front();
  expected.controller().expected_active_control_configuration = {
      ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
  auto& domain = status.domains.front();
  domain.expected_domain = expected;
  domain.current_scope = snapshot.domains.front();
  domain.resolved_provider_occurrence = snapshot.provider_occurrences.front();
  domain.previous_state = snapshot.component_lifecycle_state;
  domain.current_state = snapshot.component_lifecycle_state;
  return RetargetApplyStatus(std::move(status), ComponentLifecycleIntentKindDTO::kPrepare);
}

TEST(SqliteCCLCJournal, QualifiedBootstrapSnapshotSelectsTwoWithoutCreatingWatermarkBeforeAcceptance) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const auto snapshot = QualifiedApplySnapshot();
  std::optional<ComponentLifecycleStatusDTO> saved;
  {
    SqliteCCLCJournal journal(path, CellId("cell.1"));
    RequestOwnerEpoch owner("owner.qualified.bootstrap");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    const auto without_snapshot = journal.ReadApplyComponentGenerationSelection(owner, snapshot.controllable_component_id);
    ASSERT_TRUE(without_snapshot.ok());
    EXPECT_FALSE(without_snapshot.value().predecessor_generation);
    EXPECT_EQ(without_snapshot.value().candidate_generation, ComponentLifecycleGeneration(1U));
    const ApplyComponentQualifiedPredecessorDTO qualified{snapshot.component_lifecycle_generation, snapshot.snapshot_digest};
    const auto selection = journal.ReadApplyComponentGenerationSelection(owner, snapshot.controllable_component_id, qualified);
    ASSERT_TRUE(selection.ok());
    EXPECT_EQ(selection.value(), (ApplyComponentGenerationSelectionDTO{
        std::nullopt, ComponentLifecycleGeneration(2U), 0U, qualified}));
    EXPECT_FALSE(journal.LoadCurrentComponentLifecycleFact(snapshot.controllable_component_id).value().durable_generation);
    const auto child = QualifiedApplyStatus(journal, owner, snapshot);
    ASSERT_TRUE(child.locally_valid());
    EXPECT_EQ(child.accepted_intent.expected_component_lifecycle_generation, ComponentLifecycleGeneration(1U));
    EXPECT_FALSE(child.accepted_intent.apply_generation_selection->predecessor_generation);
    const auto bytes = EncodeComponentLifecycleIntent(child.accepted_intent);
    EXPECT_EQ(DecodeComponentLifecycleIntent(bytes), child.accepted_intent);
    const auto accepted = journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
        child.acceptance, child, child.apply_bring_up->original_request_payload_digest);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    const auto result_bytes = EncodeApplyDelegatedComponentLifecycleRequestResult(accepted.value());
    const auto decoded = DecodeApplyDelegatedComponentLifecycleRequestResult(result_bytes);
    EXPECT_EQ(decoded.status, child);
    EXPECT_EQ(EncodeApplyDelegatedComponentLifecycleRequestResult(decoded), result_bytes);
    EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(snapshot.controllable_component_id).value().durable_generation,
              ComponentLifecycleGeneration(2U));
    const auto replay = journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
        child.acceptance, child, child.apply_bring_up->original_request_payload_digest);
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(replay.value().status, child);
    saved = child;
  }
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  const auto observed = reopened.ReadApplyComponentBringUpRequestResult(saved->accepted_intent,
      saved->apply_bring_up->original_request_payload_digest);
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value());
  EXPECT_EQ(observed.value()->status, saved);
}

TEST(SqliteCCLCJournal, QualifiedBootstrapHonorsReservationFloorAndExactReservationRace) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const auto route = ActiveConfiguration(ExpectedDomain());
  ASSERT_TRUE(journal.PlanNativeApply(NativeApplyPlanRequest("apply.abandoned", 1U, "bundle.a", route)).ok());
  RequestOwnerEpoch owner("owner.qualified.reservation");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto snapshot = QualifiedApplySnapshot();
  auto child = QualifiedApplyStatus(journal, owner, snapshot);
  ASSERT_TRUE(child.locally_valid());
  EXPECT_FALSE(child.accepted_intent.apply_generation_selection->predecessor_generation);
  EXPECT_EQ(child.accepted_intent.apply_generation_selection->prior_reservation_generation, 1U);
  EXPECT_EQ(child.acceptance.component_lifecycle_generation, ComponentLifecycleGeneration(2U));
  ASSERT_TRUE(journal.PlanNativeApply(NativeApplyPlanRequest("apply.parent", 4U, "bundle.a", route)).ok());
  const auto raced = journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
      child.acceptance, child, child.apply_bring_up->original_request_payload_digest);
  ASSERT_FALSE(raced.ok());
  EXPECT_EQ(raced.error().code, "component_lifecycle_journal_generation_conflict");
  EXPECT_FALSE(journal.LoadRetainedLifecycleStatus(child.acceptance.lifecycle_transaction_id).value());
  EXPECT_FALSE(journal.LoadCurrentComponentLifecycleFact(snapshot.controllable_component_id).value().durable_generation);
  child = QualifiedApplyStatus(journal, owner, snapshot);
  ASSERT_TRUE(child.locally_valid());
  EXPECT_EQ(child.accepted_intent.apply_generation_selection->prior_reservation_generation, 2U);
  EXPECT_EQ(child.acceptance.component_lifecycle_generation, ComponentLifecycleGeneration(3U));
  const auto accepted = journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
      child.acceptance, child, child.apply_bring_up->original_request_payload_digest);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(snapshot.controllable_component_id).value().durable_generation,
            ComponentLifecycleGeneration(3U));
}

TEST(SqliteCCLCJournal, QualifiedBootstrapDoesNotIgnoreWatermarkPresenceRaceOrForeignSnapshotSeal) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  RequestOwnerEpoch owner("owner.qualified.watermark");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto snapshot = QualifiedApplySnapshot();
  const auto child = QualifiedApplyStatus(journal, owner, snapshot);
  ASSERT_TRUE(child.locally_valid());
  auto forged = child;
  forged.accepted_intent.apply_generation_selection->owner_qualified_predecessor->snapshot_digest = ContentDigest(Sha('f'));
  EXPECT_FALSE(forged.locally_valid());
  EXPECT_FALSE(journal.AcceptOrReplayApplyComponentBringUp(owner, forged.accepted_intent,
      forged.acceptance, forged, child.apply_bring_up->original_request_payload_digest).ok());
  // SQL W becomes present at the same numerical predecessor. Presence is
  // still a different CAS observation and must not be ignored.
  ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
  const auto raced = journal.AcceptOrReplayApplyComponentBringUp(owner, child.accepted_intent,
      child.acceptance, child, child.apply_bring_up->original_request_payload_digest);
  ASSERT_FALSE(raced.ok());
  EXPECT_EQ(raced.error().code, "component_lifecycle_journal_generation_conflict");
  const ApplyComponentQualifiedPredecessorDTO foreign{ComponentLifecycleGeneration(2U), snapshot.snapshot_digest};
  const auto mismatch = journal.ReadApplyComponentGenerationSelection(owner, snapshot.controllable_component_id, foreign);
  ASSERT_FALSE(mismatch.ok());
  EXPECT_EQ(mismatch.error().code, "component_lifecycle_journal_generation_conflict");
  const auto invalid_digest = journal.ReadApplyComponentGenerationSelection(owner, snapshot.controllable_component_id,
      ApplyComponentQualifiedPredecessorDTO{ComponentLifecycleGeneration(1U), ContentDigest("not-a-snapshot-digest")});
  ASSERT_FALSE(invalid_digest.ok());
  EXPECT_EQ(invalid_digest.error().code, "component_lifecycle_journal_query_invalid");
  auto current = QualifiedApplyStatus(journal, owner, snapshot);
  ASSERT_TRUE(current.locally_valid());
  EXPECT_EQ(current.accepted_intent.apply_generation_selection->predecessor_generation, ComponentLifecycleGeneration(1U));
  current.apply_bring_up->recovery = ApplyComponentBringUpRecoveryEvidenceDTO{
      snapshot.snapshot_digest, "incident.selected", StopId("stop.selected"), StopGeneration(7U)};
  current.apply_bring_up->predecessor_occurrence = SelectedControllerOccurrenceDTO{
      ExecutionBindingEpoch(12U), {{ControlDomainId("domain.1"), 30U}}};
  current.apply_bring_up->successor_occurrence_floor = SelectedControllerOccurrenceDTO{
      ExecutionBindingEpoch(13U), {{ControlDomainId("domain.1"), 31U}}};
  ASSERT_TRUE(current.locally_valid());
  current.apply_bring_up->recovery->predecessor_snapshot_digest = ContentDigest(Sha('f'));
  EXPECT_FALSE(current.locally_valid());
  EXPECT_TRUE(journal.LoadComponentsWithUnsettledLifecycle().value().empty());
}

TEST(SqliteCCLCJournal, QualifiedRepresentationLeavesOrdinaryBootstrapAndExactWatermarkCASUnchanged) {
  for (const auto existing : {false, true}) {
    SCOPED_TRACE(existing);
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    if (existing) { ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok()); }
    RequestOwnerEpoch owner("owner.ordinary.bootstrap");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    const auto intent = Intent("ordinary.bootstrap", owner, existing ? 7U : 1U);
    const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
    const auto status = InitialStatus(intent, acceptance);
    ASSERT_TRUE(status.locally_valid());
    const auto bytes = EncodeComponentLifecycleIntent(intent);
    EXPECT_EQ(EncodeComponentLifecycleIntent(DecodeComponentLifecycleIntent(bytes)), bytes);
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    const auto accepted = journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status);
    if (existing) {
      ASSERT_FALSE(accepted.ok());
      EXPECT_EQ(accepted.error().code, "component_lifecycle_journal_generation_conflict");
      EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id).value().durable_generation,
                ComponentLifecycleGeneration(1U));
    } else {
      ASSERT_TRUE(accepted.ok()) << accepted.error().message;
      EXPECT_EQ(accepted.value().status, status);
      EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(intent.controllable_component_id).value().durable_generation,
                ComponentLifecycleGeneration(2U));
    }
  }
}

TEST(SqliteCCLCJournal, OrdinaryActivateAfterConfigurationCommitsTargetsAndReplaysOnce) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const ControllableComponentId component("component.1");
  const auto route = ActiveConfiguration(ExpectedDomain());
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  RequestOwnerEpoch owner("owner.m.activate");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  auto request = NativeApplyPlanRequest("apply.m.activate", 1U, "bundle.a", route);
  request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  const auto plan = journal.PlanNativeApply(request);
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.m.activate", 1U, "bundle.a", 2U),
      plan.value().components, {route}}).ok());
  ASSERT_FALSE(journal.LoadLifecycleTarget(component).value());
  const auto configured_generation = plan.value().components.front().generation.value();

  // Activate uses the ordinary Prepare then Enable journal paths. The actual
  // owner records Prepared before the next command, rather than configuration
  // publication asserting Active or synthesizing a lifecycle predecessor.
  auto prepare = Intent("operator.m.prepare", owner, configured_generation);
  prepare.expected_control_domains = {ExpectedDisableDomain()};
  const auto prepare_acceptance = Acceptance(prepare, 1U, "lifecycle.1");
  auto prepare_status = InitialStatus(prepare, prepare_acceptance);
  prepare_status.domains.front().expected_domain = prepare.expected_control_domains.front();
  prepare_status.domains.front().current_scope = CurrentDisableScope();
  ASSERT_TRUE(prepare_status.locally_valid());
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, prepare, prepare_acceptance).ok());
  const auto prepared = journal.AcceptOrReplayLifecycle(owner, prepare, prepare_acceptance, prepare_status);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
  const ComponentLifecycleTargetDTO prepared_target{component, LifecycleTargetStateDTO::kPrepared,
                                                   LifecycleTargetOriginDTO::kOperator, 1U};
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), prepared_target);
  const auto prepare_replay = journal.AcceptOrReplayLifecycle(owner, prepare, prepare_acceptance, prepare_status);
  ASSERT_TRUE(prepare_replay.ok()) << prepare_replay.error().message;
  EXPECT_EQ(prepare_replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), prepared_target);
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().durable_generation,
            prepare_acceptance.component_lifecycle_generation);
  ASSERT_TRUE(RecordSettledTerminal(journal, owner, TerminalSuccess(prepare_status, 2U)).ok());
  ASSERT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().current_state,
            ComponentLifecycleStateDTO::kPrepared);

  auto enable = Intent("operator.m.enable", owner, prepare_acceptance.component_lifecycle_generation.value());
  enable.intent = ComponentLifecycleIntentKindDTO::kEnable;
  enable.expected_control_domains = {ExpectedDisableDomain()};
  const auto enable_acceptance = Acceptance(enable, 2U, "lifecycle.2");
  auto enable_status = InitialStatus(enable, enable_acceptance);
  enable_status.previous_state = ComponentLifecycleStateDTO::kPrepared;
  enable_status.current_state = ComponentLifecycleStateDTO::kPrepared;
  enable_status.requested_state = {ComponentLifecycleRequestedStatePresenceDTO::kResolved,
                                   ComponentLifecycleStateDTO::kActive};
  auto& domain = enable_status.domains.front();
  domain.expected_domain = enable.expected_control_domains.front();
  domain.current_scope = CurrentDisableScope();
  domain.previous_state = ComponentLifecycleStateDTO::kPrepared;
  domain.current_state = ComponentLifecycleStateDTO::kPrepared;
  domain.requested_state = enable_status.requested_state;
  ASSERT_TRUE(enable_status.locally_valid());
  ASSERT_TRUE(SeedLifecycleClaim(journal, owner, enable, enable_acceptance).ok());
  const auto enabled = journal.AcceptOrReplayLifecycle(owner, enable, enable_acceptance, enable_status);
  ASSERT_TRUE(enabled.ok()) << enabled.error().message;
  EXPECT_EQ(enabled.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
  const ComponentLifecycleTargetDTO active_target{component, LifecycleTargetStateDTO::kActive,
                                                 LifecycleTargetOriginDTO::kOperator, 2U};
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), active_target);
  const auto enable_replay = journal.AcceptOrReplayLifecycle(owner, enable, enable_acceptance, enable_status);
  ASSERT_TRUE(enable_replay.ok()) << enable_replay.error().message;
  EXPECT_EQ(enable_replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), active_target);
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().durable_generation,
            enable_acceptance.component_lifecycle_generation);
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().current_state,
            ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(RawScalar(path, "SELECT highest_transaction_generation FROM cclc_journal_metadata"), 2);
  EXPECT_EQ(RawScalar(path, "SELECT count(*) FROM lifecycle_transactions"), 2);
}

TEST(SqliteCCLCJournal, FirstTargetRequiresAdoptedMembershipAndAnOrdinaryOperatorRequest) {
  for (const bool adopted : {false, true}) {
    ScopedTemporaryDirectory temporary;
    SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
    RequestOwnerEpoch owner("owner.m.target-membership");
    ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
    std::uint64_t predecessor = 1U;
    if (adopted) {
      const auto route = ActiveConfiguration(ExpectedDomain());
      auto request = NativeApplyPlanRequest("apply.m.membership", 1U, "bundle.a", route);
      request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
      const auto plan = journal.PlanNativeApply(request);
      ASSERT_TRUE(plan.ok());
      ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.m.membership", 1U, "bundle.a", 2U),
          plan.value().components, {route}}).ok());
      predecessor = plan.value().components.front().generation.value();
    }
    auto intent = Intent("request.m.no-first-target", owner, predecessor);
    if (adopted) intent.request_context.source_label = std::string(kLifecycleTargetSourceLabel);
    const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
    const auto status = InitialStatus(intent, acceptance);
    ASSERT_TRUE(SeedLifecycleClaim(journal, owner, intent, acceptance).ok());
    const auto accepted = journal.AcceptOrReplayLifecycle(owner, intent, acceptance, status);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    EXPECT_EQ(accepted.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
    EXPECT_FALSE(journal.LoadLifecycleTarget(intent.controllable_component_id).value());
    const auto explicit_target = journal.SetLifecycleTarget(ControllableComponentId("component.unadopted"),
        LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator);
    EXPECT_FALSE(explicit_target.ok());
    EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
  }
}

TEST(SqliteCCLCJournal, ConfigurationChangeRetiresOnlyIntentAndPreservesIncidentRecoveryEvidence) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const ControllableComponentId component("component.1");
  const ControlDomainId domain("domain.1");
  const auto route = ActiveConfiguration(ExpectedDomain());
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  RequestOwnerEpoch owner("owner.m.configuration");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  auto request = NativeApplyPlanRequest("apply.m.initial", 1U, "bundle.a", route);
  request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  const auto initial = journal.PlanNativeApply(request);
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.m.initial", 1U, "bundle.a", 2U),
      initial.value().components, {route}}).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  ASSERT_TRUE(journal.OpenComponentIncident(
      TestIncident("incident.m.recovered", component, LifecycleBlockingCauseClassDTO::kFault, 10), 0U).ok());
  const auto recovered = journal.RecoverComponentIncident(
      owner, RecoverIntent("operator.m.recover", owner), "operator.ada", 20);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_TRUE(journal.RecordInterventionCause(component, domain,
      LifecycleBlockingCauseClassDTO::kEmergencyStop, 30, 1U).ok());
  ASSERT_TRUE(journal.OpenComponentIncident(
      TestIncident("incident.m.open", component, LifecycleBlockingCauseClassDTO::kEmergencyStop, 30), 1U).ok());
  const auto before = journal.LoadLifecycleTargetFacts(component);
  ASSERT_TRUE(before.ok());
  ASSERT_TRUE(before.value().target);
  ASSERT_EQ(before.value().target->recovered_cause, LifecycleBlockingCauseClassDTO::kFault);
  const auto incident_events = journal.LoadComponentIncidentEvents(0U, 16U);
  ASSERT_TRUE(incident_events.ok());
  request.apply = {ApplyId("apply.m.changed"), ApplyGeneration(1U)};
  request.candidate_retained_bundle_id = RetainedBundleId("bundle.b");
  const auto changed = journal.PlanNativeApply(request);
  ASSERT_TRUE(changed.ok()) << changed.error().message;
  auto changed_route = route;
  changed_route.active_control_configuration_generation = ActiveControlConfigurationGeneration(
      route.active_control_configuration_generation.value() + 1U);
  const NativeApplyFactAdoptionDTO adoption{
      NativeApplyPublication("apply.m.changed", 1U, "bundle.b", 3U), changed.value().components, {changed_route}};
  ASSERT_TRUE(journal.AdoptNativeApplyFacts(adoption).ok());
  const auto after = journal.LoadLifecycleTargetFacts(component);
  ASSERT_TRUE(after.ok());
  EXPECT_FALSE(after.value().target);
  EXPECT_EQ(after.value().recovery_generation, before.value().recovery_generation);
  EXPECT_EQ(after.value().intervention_causes, before.value().intervention_causes);
  EXPECT_EQ(after.value().open_incident, before.value().open_incident);
  EXPECT_EQ(journal.LoadComponentIncidentEvents(0U, 16U).value(), incident_events.value());
  EXPECT_EQ(RawScalar(path, "SELECT target_generation FROM cclc_component_lifecycle_targets"),
            static_cast<std::int64_t>(before.value().target->target_generation));
  EXPECT_EQ(RawScalar(path, "SELECT recovered_cause FROM cclc_component_lifecycle_targets"),
            static_cast<std::int64_t>(*before.value().target->recovered_cause));
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().durable_generation,
            changed.value().components.front().generation);
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value().current_state,
            ComponentLifecycleStateDTO::kDeclared);
  ASSERT_TRUE(journal.AdoptNativeApplyFacts(adoption).ok());
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value(), after.value());
  journal.Close();
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  EXPECT_EQ(reopened.LoadLifecycleTargetFacts(component).value(), after.value());
  const auto explicit_target = reopened.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                                          LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(explicit_target.ok());
  EXPECT_EQ(explicit_target.value().target_generation, before.value().target->target_generation + 1U);
  EXPECT_EQ(reopened.LoadLifecycleTargetFacts(component).value().recovery_generation,
            before.value().recovery_generation);
}

TEST(SqliteCCLCJournal, UnrelatedConfigurationRetainsExactActiveTargetAndObservedState) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"));
  const ControllableComponentId retained("component.1");
  const ControllableComponentId added("component.2");
  const auto first_route = ActiveConfiguration(ExpectedDomain());
  const auto second_route = SecondDomainRoute();
  RequestOwnerEpoch owner("owner.m.retained");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  // Existing native-adoption fixture represents an already Active owner.
  const auto initial = journal.PlanNativeApply(NativeApplyPlanRequest("apply.m.first", 1U, "bundle.a", first_route));
  ASSERT_TRUE(initial.ok());
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.m.first", 1U, "bundle.a", 2U),
      initial.value().components, {first_route}}).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(retained, LifecycleTargetStateDTO::kActive,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  ASSERT_TRUE(journal.RecordLifecycleTargetReached(retained, true, 0U).value());
  const auto target = journal.LoadLifecycleTarget(retained);
  const auto fact = journal.LoadCurrentComponentLifecycleFact(retained);
  ASSERT_TRUE(target.ok() && target.value());
  ASSERT_TRUE(fact.ok());
  auto request = NativeApplyPlanRequestWithDelta("apply.m.add", "bundle.b", {retained, added},
      {{first_route.control_domain_id, first_route.active_control_configuration_generation},
       {second_route.control_domain_id, second_route.active_control_configuration_generation}},
      {retained}, {}, {first_route}, {});
  request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
  const auto plan = journal.PlanNativeApply(request);
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.m.add", 1U, "bundle.b", 3U),
      plan.value().components, {first_route, second_route}}).ok());
  EXPECT_EQ(journal.LoadLifecycleTarget(retained).value(), target.value());
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(retained).value(), fact.value());
  EXPECT_FALSE(journal.LoadLifecycleTarget(added).value());
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(added).value().current_state,
            ComponentLifecycleStateDTO::kDeclared);
  EXPECT_EQ(journal.LoadLifecycleTargets().value().size(), 1U);
}

TEST(SqliteCCLCJournal, OwnerRestartRetiresActiveIntentWithoutInventingInactiveHardwareOrClearingRecovery) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const ControllableComponentId component("component.1");
  const ControllableComponentId prepared("component.2");
  const auto first_route = ActiveConfiguration(ExpectedDomain());
  const auto second_route = SecondDomainRoute();
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  RequestOwnerEpoch owner("owner.m.before.restart");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  const auto plan = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta("apply.m.restart", "bundle.a",
      {component, prepared},
      {{first_route.control_domain_id, first_route.active_control_configuration_generation},
       {second_route.control_domain_id, second_route.active_control_configuration_generation}}, {}, {}, {}, {}));
  ASSERT_TRUE(plan.ok());
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.m.restart", 1U, "bundle.a", 2U),
      plan.value().components, {first_route, second_route}}).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(prepared, LifecycleTargetStateDTO::kPrepared,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  ASSERT_TRUE(journal.OpenComponentIncident(
      TestIncident("incident.m.restart.recovered", prepared, LifecycleBlockingCauseClassDTO::kFault, 10), 0U).ok());
  auto recover = RecoverIntent("operator.m.restart.recover", owner);
  recover.controllable_component_id = prepared;
  auto recovered_domain = ExpectedDomain();
  recovered_domain.control_domain_id = second_route.control_domain_id;
  recovered_domain.controller().expected_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  recover.expected_control_domains = {recovered_domain};
  ASSERT_TRUE(journal.RecoverComponentIncident(owner, recover, "operator.ada", 20).ok());
  ASSERT_TRUE(journal.RecordInterventionCause(prepared, second_route.control_domain_id,
      LifecycleBlockingCauseClassDTO::kEmergencyStop, 30, 1U).ok());
  ASSERT_TRUE(journal.OpenComponentIncident(
      TestIncident("incident.m.restart.open", prepared, LifecycleBlockingCauseClassDTO::kEmergencyStop, 30), 1U).ok());
  const auto before = journal.LoadLifecycleTargetFacts(component);
  const auto before_fact = journal.LoadCurrentComponentLifecycleFact(component);
  const auto prepared_facts = journal.LoadLifecycleTargetFacts(prepared);
  const auto events = journal.LoadComponentIncidentEvents(0U, 16U);
  ASSERT_TRUE(before.ok() && before.value().target && before_fact.ok() && prepared_facts.ok() && prepared_facts.value().target && events.ok());
  ASSERT_EQ(prepared_facts.value().target->recovered_cause, LifecycleBlockingCauseClassDTO::kFault);
  journal.Close();
  SqliteCCLCJournal reopened(path, CellId("cell.1"));
  // Open alone retains observations; owner Start atomically stops following Active.
  EXPECT_EQ(reopened.LoadLifecycleTarget(component).value(), before.value().target);
  RequestOwnerEpoch next("owner.m.after.restart");
  ASSERT_TRUE(reopened.ReadQuiescentOwner(owner).ok());
  // A new coordinator Start can require the exact settled predecessor. The
  // unchanged four-argument call still denotes restart and retires Active.
  const auto restarted = reopened.BeginOwnerEpoch(next, 40, kDefaultCCLCExactResultHistory, owner);
  ASSERT_TRUE(restarted.ok()) << restarted.error().message;
  next = restarted.value().request_owner_epoch;
  const auto after = reopened.LoadLifecycleTargetFacts(component);
  ASSERT_TRUE(after.ok());
  EXPECT_FALSE(after.value().target);
  EXPECT_EQ(after.value().recovery_generation, before.value().recovery_generation);
  EXPECT_EQ(after.value().intervention_causes, before.value().intervention_causes);
  EXPECT_EQ(after.value().open_incident, before.value().open_incident);
  EXPECT_EQ(reopened.LoadComponentIncidentEvents(0U, 16U).value(), events.value());
  EXPECT_EQ(reopened.LoadLifecycleTargetFacts(prepared).value(), prepared_facts.value());
  EXPECT_EQ(reopened.LoadCurrentComponentLifecycleFact(component).value(), before_fact.value());
  EXPECT_EQ(before_fact.value().current_state, ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(RawScalar(path, "SELECT target_generation FROM cclc_component_lifecycle_targets WHERE controllable_component_id='component.1'"),
            static_cast<std::int64_t>(before.value().target->target_generation));
  EXPECT_EQ(RawScalar(path, "SELECT recovered_cause FROM cclc_component_lifecycle_targets WHERE controllable_component_id='component.2'"),
            static_cast<std::int64_t>(*prepared_facts.value().target->recovered_cause));
  RequestOwnerEpoch third("owner.m.third.restart");
  ASSERT_TRUE(BeginTestOwnerEpoch(reopened, third, 50, kDefaultCCLCExactResultHistory).ok());
  EXPECT_FALSE(reopened.LoadLifecycleTarget(component).value());
  const auto explicit_target = reopened.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                                          LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(explicit_target.ok());
  EXPECT_EQ(explicit_target.value().target_generation, before.value().target->target_generation + 1U);
}

TEST(SqliteCCLCJournal, LiveCapacityCheckpointPreservesExactActiveTargetAndRecoveryFacts) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const ControllableComponentId component("component.1");
  const ControllableComponentId prepared("component.2");
  const auto route = ActiveConfiguration(ExpectedDomain());
  const auto second_route = SecondDomainRoute();
  SqliteCCLCJournal journal(path, CellId("cell.1"), 2U);
  RequestOwnerEpoch owner("owner.m.live-checkpoint");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 2U).ok());
  const auto plan = journal.PlanNativeApply(NativeApplyPlanRequestWithDelta("apply.m.checkpoint", "bundle.a",
      {component, prepared},
      {{route.control_domain_id, route.active_control_configuration_generation},
       {second_route.control_domain_id, second_route.active_control_configuration_generation}}, {}, {}, {}, {}));
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_TRUE(journal.AdoptNativeApplyFacts({NativeApplyPublication("apply.m.checkpoint", 1U, "bundle.a", 2U),
      plan.value().components, {route, second_route}}).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(prepared, LifecycleTargetStateDTO::kPrepared,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  ASSERT_TRUE(journal.OpenComponentIncident(
      TestIncident("incident.m.checkpoint.recovered", prepared, LifecycleBlockingCauseClassDTO::kFault, 10), 0U).ok());
  auto recover = RecoverIntent("operator.m.checkpoint.recover", owner);
  recover.controllable_component_id = prepared;
  auto recovered_domain = ExpectedDomain();
  recovered_domain.control_domain_id = second_route.control_domain_id;
  recovered_domain.controller().expected_control_bindings.front().control_binding_id = ControlBindingId("binding.2");
  recover.expected_control_domains = {recovered_domain};
  ASSERT_TRUE(journal.RecoverComponentIncident(owner, recover, "operator.ada", 20).ok());
  ASSERT_TRUE(journal.RecordInterventionCause(prepared, second_route.control_domain_id,
      LifecycleBlockingCauseClassDTO::kEmergencyStop, 30, 1U).ok());
  ASSERT_TRUE(journal.OpenComponentIncident(
      TestIncident("incident.m.checkpoint.open", prepared, LifecycleBlockingCauseClassDTO::kEmergencyStop, 30), 1U).ok());
  const auto target_facts = journal.LoadLifecycleTargetFacts(component);
  const auto prepared_facts = journal.LoadLifecycleTargetFacts(prepared);
  const auto current_fact = journal.LoadCurrentComponentLifecycleFact(component);
  const auto incident_events = journal.LoadComponentIncidentEvents(0U, 16U);
  ASSERT_TRUE(target_facts.ok() && target_facts.value().target && current_fact.ok() && incident_events.ok());
  ASSERT_EQ(target_facts.value().target->requested_state, LifecycleTargetStateDTO::kActive);
  ASSERT_TRUE(prepared_facts.ok() && prepared_facts.value().target);
  ASSERT_EQ(prepared_facts.value().target->recovered_cause, LifecycleBlockingCauseClassDTO::kFault);
  ASSERT_EQ(current_fact.value().current_state, ComponentLifecycleStateDTO::kActive);
  // Recover is already one exact request in this namespace. A rejected
  // ordinary request fills the other slot without creating live work.
  const auto rejected = Intent("request.m.checkpoint.rejected", owner,
      plan.value().components.front().generation.value());
  const ErrorDTO refusal{"invalid_state_transition", "fixture owner refused before acceptance",
                         {{"effect", "not_started"}}};
  ASSERT_TRUE(journal.RecordLifecycleRejection(owner, rejected, refusal, std::nullopt).ok());
  const auto unseen = Intent("request.m.checkpoint.unseen", owner,
      plan.value().components.front().generation.value());
  const auto full = journal.InspectLifecycleRequest(owner, unseen);
  ASSERT_TRUE(full.ok() && full.value());
  ASSERT_EQ(full.value()->replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
  ICCLCJournal& journal_port = journal;
  ASSERT_TRUE(journal_port.ReadQuiescentOwner(owner).ok());
  const auto missing_predecessor = journal_port.BeginOwnerEpoch(
      RequestOwnerEpoch("owner.m.live-checkpoint"), 40, 2U, std::nullopt,
      CCLCOwnerEpochTransitionPurpose::kLiveCapacityCheckpoint);
  ASSERT_FALSE(missing_predecessor.ok());
  EXPECT_EQ(missing_predecessor.error().code, "component_lifecycle_journal_owner_epoch_invalid");
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value(), target_facts.value());
  ASSERT_TRUE(journal_port.ReadQuiescentOwner(owner).ok());

  // This is the exact caller contract used by CheckpointFullRequestNamespace:
  // same live owner, full settled ledger, exact quiescent predecessor and
  // explicit trusted live-capacity purpose.
  const auto checkpoint = journal_port.BeginOwnerEpoch(RequestOwnerEpoch("owner.m.live-checkpoint"), 40, 2U, owner,
      CCLCOwnerEpochTransitionPurpose::kLiveCapacityCheckpoint);
  ASSERT_TRUE(checkpoint.ok()) << checkpoint.error().message;
  EXPECT_NE(checkpoint.value().request_owner_epoch, owner);
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(component).value(), target_facts.value());
  EXPECT_EQ(journal.LoadLifecycleTargetFacts(prepared).value(), prepared_facts.value());
  EXPECT_EQ(journal.LoadCurrentComponentLifecycleFact(component).value(), current_fact.value());
  EXPECT_EQ(journal.LoadComponentIncidentEvents(0U, 16U).value(), incident_events.value());
  EXPECT_EQ(RawScalar(path, "SELECT target_generation FROM cclc_component_lifecycle_targets WHERE controllable_component_id='component.1'"),
            static_cast<std::int64_t>(target_facts.value().target->target_generation));
  EXPECT_EQ(RawScalar(path, "SELECT recovered_cause FROM cclc_component_lifecycle_targets WHERE controllable_component_id='component.2'"),
            static_cast<std::int64_t>(*prepared_facts.value().target->recovered_cause));
  ASSERT_TRUE(journal.ReadQuiescentOwner(checkpoint.value().request_owner_epoch).ok());
  const auto available = journal.InspectLifecycleRequest(checkpoint.value().request_owner_epoch,
      Intent("request.m.after-checkpoint", checkpoint.value().request_owner_epoch,
             plan.value().components.front().generation.value()));
  ASSERT_TRUE(available.ok());
  EXPECT_FALSE(available.value());
}

TEST(SqliteCCLCJournal, OwnerRestartAndTargetRetirementRollbackInOneTransaction) {
  ScopedTemporaryDirectory temporary;
  const auto path = JournalPath(temporary);
  const ControllableComponentId component("component.1");
  SqliteCCLCJournal journal(path, CellId("cell.1"));
  RequestOwnerEpoch owner("owner.m.atomic");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, kDefaultCCLCExactResultHistory).ok());
  ASSERT_TRUE(SeedApplyPredecessorFacts(journal).ok());
  ASSERT_TRUE(journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                        LifecycleTargetOriginDTO::kOperator).ok());
  const auto target = journal.LoadLifecycleTarget(component);
  ASSERT_TRUE(target.ok());
  const auto sequence = RawScalar(path, "SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs'");
  ExecRaw(path, "CREATE TRIGGER reject_m_target_retirement BEFORE UPDATE ON cclc_component_lifecycle_targets "
                "BEGIN SELECT RAISE(ABORT,'fixture retirement failure'); END");
  const auto refused = journal.BeginOwnerEpoch(RequestOwnerEpoch("owner.m.failed"), 2, kDefaultCCLCExactResultHistory);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(journal.LoadLifecycleTarget(component).value(), target.value());
  EXPECT_EQ(RawScalar(path, "SELECT seq FROM sqlite_sequence WHERE name='cclc_owner_epochs'"), sequence);
  EXPECT_TRUE(journal.ReadQuiescentOwner(owner).ok());
  ExecRaw(path, "DROP TRIGGER reject_m_target_retirement");
  RequestOwnerEpoch next("owner.m.retry");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, next, 3, kDefaultCCLCExactResultHistory).ok());
  EXPECT_FALSE(journal.LoadLifecycleTarget(component).value());
}

}  // namespace

namespace {

class TargetCASOperationJournal final : public IOperationJournal {
 public:
  Result<OperationJournalRecoveryDTO> BeginOwnerEpoch(const RequestOwnerEpoch&, std::int64_t,
                                                      std::size_t,
                                                      const std::vector<ResourceId>&) override {
    return Result<OperationJournalRecoveryDTO>::Ok({});
  }
  Result<std::uint64_t> RecordOperationAccepted(const RequestOwnerEpoch&, const OperationIntentDTO&,
                                                const OperationAcceptedDTO&,
                                                const OperationStateDTO&,
                                                std::span<const OperationId>) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordControlAccepted(const RequestOwnerEpoch&,
                                              const OperationControlRequestDTO&,
                                              const OperationControlAcceptedDTO&,
                                              const OperationStateDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  EmergencyStopAcceptanceProjectionResultDTO RecordStopAccepted(
      const RequestOwnerEpoch&, const EmergencyStopAcceptanceRecordDTO&,
      const EmergencyStopProjectedIntentDTO&, const StopAcceptedDTO&,
      const EmergencyStopAffectedStateSnapshotDTO&) noexcept override {
    return {};
  }
  StopForensicProjectionResultDTO RecordStopTerminal(
      const RequestOwnerEpoch&, const StopStatusDTO&, const StopCertificateDTO&,
      const std::optional<StopFaultLatchDTO>&) noexcept override {
    return {};
  }
  StopForensicProjectionResultDTO RecordStopSurveillanceCheckpoint(
      const RequestOwnerEpoch&, const StopPostTerminalSurveillanceDTO&, const StopFaultLatchDTO&,
      std::int64_t) noexcept override {
    return {};
  }
  Result<std::uint64_t> RecordStopFaultCleared(const RequestOwnerEpoch&,
                                               const StopFaultRecoveryIntentDTO&,
                                               const StopFaultRecoveryResultDTO&,
                                               const StopFaultLatchDTO&,
                                               const StopStatusDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> CloseOwnerEpoch(const RequestOwnerEpoch&, std::int64_t) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordRuntimeScaleAccepted(const RequestOwnerEpoch&,
                                                   const RuntimeScaleRequestDTO&,
                                                   const RuntimeScaleAcceptedDTO&,
                                                   const OperationStateDTO&,
                                                   const RuntimeScaleStatusDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordOperationState(
      const RequestOwnerEpoch&, const OperationStateDTO&, const RuntimeScaleStatusDTO*, const char*,
      std::span<const PreparedMotionDiagnosticsDTO>) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimAcquired(const RequestOwnerEpoch&,
                                                         const CellAdmissionClaimDTO&) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimReleased(const RequestOwnerEpoch&,
                                                         const CellAdmissionClaimDTO&,
                                                         const CellAdmissionClaimReleaseDTO&,
                                                         AdmissionGeneration) override {
    if (fail_release) return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimsReconciled(
      const RequestOwnerEpoch&, const CellAdmissionReconcileDTO&,
      const CellAdmissionReconcileValidationDTO&, AdmissionGeneration) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionGenerationAdvanced(const RequestOwnerEpoch&,
                                                              AdmissionGeneration,
                                                              AdmissionGeneration) override {
    if (fail_generation) return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
    return Result<std::uint64_t>::Ok(++sequence);
  }

  bool fail_generation{};
  bool fail_release{};
  std::atomic<std::uint64_t> sequence{};
};

[[nodiscard]] ManagerLivenessRequiredSetDTO TargetCASEmptyLiveness() {
  ManagerLivenessRequiredSetDTO set{CellId("cell.1"), RetainedBundleId("bundle.a"),
      ContentDigest(Sha('a')), RuntimeUnitId("manager"), RuntimeEpoch("manager.target-cas"),
      RequestOwnerEpoch("manager.target-cas"), {}};
  set.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(set.required_receivers);
  return set;
}

class TargetCASFixture final {
 public:
  explicit TargetCASFixture(std::size_t history = 2U, std::size_t memberships = 8U)
      : path(JournalPath(temporary)), journal(path, CellId("cell.1"), history),
        kernel(RequestOwnerEpoch("kernel.target-cas"), 16U, clock, operation_journal, {},
               AdmissionGeneration(0U), {},
               [source = path](const CellAdmissionClaimDTO& claim,
                               const CellAdmissionClaimReleaseDTO& release) {
                 return ValidateCCLCCellAdmissionSettlementReadOnly(source, CellId("cell.1"), claim, release);
               }, {}, {}, {}, TargetCASEmptyLiveness()) {
    const auto started = BeginTestOwnerEpoch(journal, owner, 1, memberships);
    if (!started.ok()) throw std::runtime_error(started.error().message);
    const auto route = ActiveConfiguration(ExpectedDomain());
    auto request = NativeApplyPlanRequest("apply.target-cas", 1U, "bundle.a", route);
    request.changed_component_state = ComponentLifecycleStateDTO::kDeclared;
    const auto plan = journal.PlanNativeApply(request);
    if (!plan.ok()) throw std::runtime_error(plan.error().message);
    const auto adopted = journal.AdoptNativeApplyFacts(
        {NativeApplyPublication("apply.target-cas", 1U, "bundle.a", 2U), plan.value().components, {route}});
    if (!adopted.ok()) throw std::runtime_error(adopted.error().message);
    const CellAdmissionReadinessSnapshotDTO ready{
        true, ProgramCycleStatusDTO{RequestOwnerEpoch("program.target-cas"), 1U,
               ProgramCycleStateDTO::kIdleAfterReset, std::nullopt, std::nullopt, std::nullopt},
        true, 2U, 2U, true, true, 0U, 0U, 0U};
    const auto configured = kernel.UpdateCellAdmissionReadiness(ready);
    if (!configured.ok()) throw std::runtime_error(configured.error().message);
  }

  ComponentLifecycleIntentDTO Request(std::string id) {
    auto intent = Intent(std::move(id), owner, 1U);
    intent.expected_admission_generation = kernel.GetAuthoringAdmission().admission_generation;
    return intent;
  }

  Result<CellAdmissionClaimDTO> Acquire(const ComponentLifecycleIntentDTO& intent,
                                        const ComponentLifecycleAcceptedDTO& acceptance) {
    const CellAdmissionClaimIntentDTO expected{
        CellAdmissionClaimKindDTO::kComponentLifecycle,
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, owner,
        acceptance.lifecycle_transaction_id.value(), intent.request_context.request_id,
        ContentDigest(detail::ComponentLifecycleSemanticIntentDigest(intent)),
        intent.expected_admission_generation, {}, std::nullopt, std::nullopt,
        intent.program_node_evidence};
    const auto attempt = journal.RecordCellClaimAttempt(owner, expected);
    if (!attempt.ok()) return Result<CellAdmissionClaimDTO>::Err(attempt.error());
    const auto acquired = kernel.AcquireCellAdmissionClaim(expected);
    if (!acquired.ok()) return acquired;
    const auto recorded = journal.RecordCellClaimAcquired(owner, acquired.value());
    return recorded.ok() ? acquired : Result<CellAdmissionClaimDTO>::Err(recorded.error());
  }

  static CellAdmissionClaimReleaseDTO Release(const CellAdmissionClaimDTO& claim) {
    return {claim.intent.owner_role, claim.intent.owner_epoch, claim.intent.owner_transaction_id,
            claim.intent.request_id, claim.intent.request_payload_digest,
            claim.acquired_admission_generation, claim.claim_generation,
            {CellAdmissionSettlementKindDTO::kProvenNoEffect, claim.intent.request_payload_digest}};
  }

  ScopedTemporaryDirectory temporary;
  std::filesystem::path path;
  SqliteCCLCJournal journal;
  RequestOwnerEpoch owner{"owner.target-cas"};
  FakeClock clock;
  TargetCASOperationJournal operation_journal;
  KernelCellAdmissionOwner kernel;
};

}  // namespace

TEST(SqliteCCLCJournal, TargetCASRetiredClaimKeepsExactReasonThroughUnknownReleaseAndReopen) {
  TargetCASFixture f;
  const ControllableComponentId component("component.1");
  const auto target = f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                                   LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(target.ok());
  const ComponentLifecycleTargetExpectationDTO expected{component, target.value().target_generation};
  const auto intent = f.Request("target.retired");
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  const auto status = InitialStatus(intent, acceptance);
  const auto claim = f.Acquire(intent, acceptance);
  ASSERT_TRUE(claim.ok()) << claim.error().message;
  const auto fact = f.journal.LoadCurrentComponentLifecycleFact(component);
  ASSERT_TRUE(fact.ok());
  const auto retired = f.journal.RetireLifecycleTargetsForClose(f.owner);
  ASSERT_TRUE(retired.ok());
  ASSERT_EQ(retired.value(), (std::vector<ComponentLifecycleTargetDTO>{target.value()}));
  const auto result = f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance, status,
                                                              expected, claim.value());
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().rejection);
  EXPECT_EQ(result.value().rejection->code, "component_lifecycle_target_stale");
  EXPECT_EQ(result.value().rejection->details.at("reason"), "target_retired");
  EXPECT_EQ(result.value().rejection->details.at("effect"), "not_started");
  EXPECT_FALSE(result.value().acceptance);
  EXPECT_FALSE(result.value().status);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM lifecycle_transactions"), 0);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_effect_attempts"), 0);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_route_effect_attempts"), 0);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_lifecycle_transaction_identity_guards"), 0);
  EXPECT_EQ(RawScalar(f.path, "SELECT highest_transaction_generation FROM cclc_journal_metadata"), 0);
  EXPECT_EQ(f.journal.LoadCurrentComponentLifecycleFact(component).value(), fact.value());
  EXPECT_FALSE(f.journal.ReadQuiescentOwner(f.owner).ok());
  const auto release = TargetCASFixture::Release(claim.value());
  const CellAdmissionRecoveryReceiptQueryDTO query{claim.value().intent, claim.value(), release};
  ASSERT_TRUE(ValidateCCLCCellAdmissionSettlementReadOnly(f.path, CellId("cell.1"),
                                                        claim.value(), release).ok());
  f.operation_journal.fail_release = true;
  EXPECT_FALSE(f.kernel.ReleaseCellAdmissionClaim(release).ok());
  const auto held = f.kernel.GetCellAdmissionRecoveryReceipt(query);
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  EXPECT_EQ(held.value().matched_claim, claim.value());
  EXPECT_FALSE(f.journal.RecordCellClaimReleaseReceipt(f.owner, release, held.value()).ok());
  const auto replay = f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance, status,
      {ControllableComponentId("foreign"), 999U}, claim.value());
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().rejection, result.value().rejection);

  f.journal.Close();
  SqliteCCLCJournal reopened(f.path, CellId("cell.1"), 2U);
  const auto reopened_result = reopened.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(reopened_result.ok() && reopened_result.value());
  EXPECT_EQ(reopened_result.value()->rejection, result.value().rejection);
  RequestOwnerEpoch successor("owner.target-cas.reopened");
  const auto recovery = BeginTestOwnerEpoch(reopened, successor, 500, 8U);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().cell_claims.size(), 1U);
  const auto& original = recovery.value().cell_claims.front();
  EXPECT_EQ(original.claim, claim.value());
  EXPECT_EQ(original.release_intent, release);
  EXPECT_FALSE(original.transaction_accepted);
  // Existing startup generic helper must preserve the committed CAS reason.
  ASSERT_TRUE(reopened.RecordUnacceptedCellClaimRelease(successor, release).ok());
  EXPECT_EQ(reopened.ReadLifecycleRequestResult(intent).value()->rejection, result.value().rejection);
  f.operation_journal.fail_release = false;
  ASSERT_TRUE(f.kernel.ReleaseCellAdmissionClaim(release).ok());
  const auto receipt = f.kernel.GetCellAdmissionRecoveryReceipt(query);
  ASSERT_TRUE(receipt.ok());
  ASSERT_EQ(receipt.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
  ASSERT_EQ(receipt.value().matched_release, release);
  ASSERT_TRUE(reopened.RecordCellClaimReleaseReceipt(successor, release, receipt.value()).ok());
  EXPECT_TRUE(reopened.ReadQuiescentOwner(successor).ok());
  EXPECT_EQ(reopened.ReadLifecycleRequestResult(intent).value()->rejection, result.value().rejection);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_lifecycle_cell_claims"), 0);
}

TEST(SqliteCCLCJournal, TargetCASExactClaimAndGenerationRefusalReuseOnlyUnacceptedCandidate) {
  TargetCASFixture f;
  const ControllableComponentId component("component.1");
  const auto target = f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                                   LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(target.ok());
  const auto intent = f.Request("target.stale");
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  const auto status = InitialStatus(intent, acceptance);
  const auto claim = f.Acquire(intent, acceptance);
  ASSERT_TRUE(claim.ok());
  const auto successor = f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                                      LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(successor.ok());
  const ComponentLifecycleTargetExpectationDTO expected{component, target.value().target_generation};
  auto foreign = claim.value();
  ++foreign.accepted_at_ns;
  EXPECT_FALSE(f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance, status,
                                                       expected, foreign).ok());
  EXPECT_FALSE(f.journal.ReadLifecycleRequestResult(intent).value());
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_request_memberships"), 0);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_lifecycle_cell_claims WHERE release_kind IS NOT NULL"), 0);
  const auto refused = f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance, status,
                                                               expected, claim.value());
  ASSERT_TRUE(refused.ok() && refused.value().rejection);
  EXPECT_EQ(refused.value().rejection->details.at("reason"), "target_generation_changed");
  auto crossing = f.Request("target.crossing");
  const auto same_candidate = Acceptance(crossing, 1U, "lifecycle.1");
  EXPECT_FALSE(f.Acquire(crossing, same_candidate).ok());
  const auto release = TargetCASFixture::Release(claim.value());
  ASSERT_TRUE(f.kernel.ReleaseCellAdmissionClaim(release).ok());
  const CellAdmissionRecoveryReceiptQueryDTO query{claim.value().intent, claim.value(), release};
  const auto receipt = f.kernel.GetCellAdmissionRecoveryReceipt(query);
  ASSERT_TRUE(receipt.ok());
  ASSERT_TRUE(f.journal.RecordCellClaimReleaseReceipt(f.owner, release, receipt.value()).ok());
  // Same N=1 is legal only now: the first request never accepted a transaction.
  auto next = f.Request("target.next");
  const auto next_acceptance = Acceptance(next, 1U, "lifecycle.1");
  const auto next_claim = f.Acquire(next, next_acceptance);
  ASSERT_TRUE(next_claim.ok()) << next_claim.error().message;
  EXPECT_GT(next_claim.value().claim_generation.value(), claim.value().claim_generation.value());
  const auto accepted = f.journal.AcceptOrReplayTargetLifecycle(f.owner, next, next_acceptance,
      InitialStatus(next, next_acceptance), {component, successor.value().target_generation}, next_claim.value());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_TRUE(accepted.value().acceptance);
  EXPECT_FALSE(accepted.value().rejection);
  EXPECT_EQ(RawScalar(f.path, "SELECT highest_transaction_generation FROM cclc_journal_metadata"), 1);
  EXPECT_EQ(f.journal.LoadLifecycleTarget(component).value(), successor.value());
  EXPECT_EQ(f.journal.LoadCurrentComponentLifecycleFact(component).value().durable_generation,
            next_acceptance.component_lifecycle_generation);
  ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner).ok());
  const auto replay = f.journal.AcceptOrReplayTargetLifecycle(f.owner, next, next_acceptance,
      InitialStatus(next, next_acceptance), expected, claim.value());
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().acceptance->lifecycle_transaction_id, next_acceptance.lifecycle_transaction_id);
  EXPECT_EQ(f.journal.LoadRetainedLifecycleStatus(next_acceptance.lifecycle_transaction_id).value(),
            accepted.value().status);
}

TEST(SqliteCCLCJournal, TargetCASComponentMismatchAndInvalidExpectationNeverAcceptForeignTarget) {
  TargetCASFixture f;
  const auto target = f.journal.SetLifecycleTarget(ControllableComponentId("component.1"),
      LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(target.ok());
  const auto intent = f.Request("target.foreign-component");
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  const auto claim = f.Acquire(intent, acceptance);
  ASSERT_TRUE(claim.ok());
  EXPECT_FALSE(f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance,
      InitialStatus(intent, acceptance), {intent.controllable_component_id, 0U}, claim.value()).ok());
  EXPECT_FALSE(f.journal.ReadLifecycleRequestResult(intent).value());
  const auto refused = f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance,
      InitialStatus(intent, acceptance), {ControllableComponentId("foreign"), target.value().target_generation},
      claim.value());
  ASSERT_TRUE(refused.ok() && refused.value().rejection);
  EXPECT_EQ(refused.value().rejection->details.at("reason"), "target_component_mismatch");
  EXPECT_EQ(f.journal.LoadLifecycleTarget(intent.controllable_component_id).value(), target.value());
  EXPECT_EQ(RawScalar(f.path, "SELECT highest_transaction_generation FROM cclc_journal_metadata"), 0);
}

TEST(SqliteCCLCJournal, TargetCASRefusalPinsReasonWhileClaimUnsettledThenExpiresWithMembership) {
  TargetCASFixture f(1U, 2U);
  const auto intent = f.Request("target.no-target");
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  const auto claim = f.Acquire(intent, acceptance);
  ASSERT_TRUE(claim.ok());
  const auto refused = f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance,
      InitialStatus(intent, acceptance), {intent.controllable_component_id, 1U}, claim.value());
  ASSERT_TRUE(refused.ok() && refused.value().rejection);
  const auto other = f.Request("target.other-rejection");
  const ErrorDTO error{"invalid_state_transition", "no lifecycle acceptance",
                       {{"effect", "not_started"}}};
  ASSERT_TRUE(f.journal.RecordLifecycleRejection(f.owner, other, error, std::nullopt).ok());
  const auto retained = f.journal.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(retained.ok() && retained.value());
  EXPECT_EQ(retained.value()->rejection, refused.value().rejection);
  auto conflict = intent;
  conflict.reason = "different original request";
  EXPECT_EQ(f.journal.ReadLifecycleRequestResult(conflict).value()->replay_status,
            RequestReplayStatusDTO::kRequestIdConflict);
  const auto unseen = f.Request("target.full-namespace");
  const auto full = f.journal.AcceptOrReplayTargetLifecycle(f.owner, unseen, acceptance,
      InitialStatus(intent, acceptance), {intent.controllable_component_id, 1U}, claim.value());
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(full.value().replay_status, RequestReplayStatusDTO::kRequestLedgerFull);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_request_memberships"), 2);
  // Lost-release reply is resolved by the actual readonly Kernel receipt.
  const auto release = TargetCASFixture::Release(claim.value());
  ASSERT_TRUE(f.kernel.ReleaseCellAdmissionClaim(release).ok());
  const CellAdmissionRecoveryReceiptQueryDTO query{claim.value().intent, claim.value(), release};
  const auto receipt = f.kernel.GetCellAdmissionRecoveryReceipt(query);
  ASSERT_TRUE(receipt.ok());
  ASSERT_TRUE(f.journal.RecordCellClaimReleaseReceipt(f.owner, release, receipt.value()).ok());
  const auto expired = f.journal.ReadLifecycleRequestResult(intent);
  ASSERT_TRUE(expired.ok() && expired.value());
  EXPECT_EQ(expired.value()->replay_status, RequestReplayStatusDTO::kRequestResultExpired);
  EXPECT_EQ(f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance,
      InitialStatus(intent, acceptance), {intent.controllable_component_id, 1U}, claim.value()).value().replay_status,
      RequestReplayStatusDTO::kRequestResultExpired);
}

TEST(SqliteCCLCJournal, TargetCASRefusalAndReleaseIntentRollbackTogetherOnSQLFailure) {
  TargetCASFixture f;
  const auto intent = f.Request("target.rollback");
  const auto acceptance = Acceptance(intent, 1U, "lifecycle.1");
  const auto claim = f.Acquire(intent, acceptance);
  ASSERT_TRUE(claim.ok());
  ExecRaw(f.path, "CREATE TRIGGER reject_target_release BEFORE UPDATE OF release_kind ON cclc_lifecycle_cell_claims "
                  "BEGIN SELECT RAISE(ABORT,'injected no-effect release write failure'); END");
  const auto failed = f.journal.AcceptOrReplayTargetLifecycle(f.owner, intent, acceptance,
      InitialStatus(intent, acceptance), {intent.controllable_component_id, 1U}, claim.value());
  EXPECT_FALSE(failed.ok());
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_request_memberships"), 0);
  EXPECT_EQ(RawScalar(f.path, "SELECT highest_terminal_sequence FROM cclc_journal_metadata"), 0);
  EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_lifecycle_cell_claims WHERE release_kind IS NOT NULL"), 0);
  const CellAdmissionRecoveryReceiptQueryDTO query{claim.value().intent, claim.value(), std::nullopt};
  const auto held = f.kernel.GetCellAdmissionRecoveryReceipt(query);
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  EXPECT_FALSE(f.journal.ReadQuiescentOwner(f.owner).ok());
  ExecRaw(f.path, "DROP TRIGGER reject_target_release");
  EXPECT_FALSE(f.journal.ReadLifecycleRequestResult(intent).value());
}

TEST(SqliteCCLCJournal, ReachedMarkerAtomicallyRejectsSupersededTargetAndRecoveryGeneration) {
  TargetCASFixture f;
  const ControllableComponentId component("component.1");
  const auto first = f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                                  LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(first.ok());
  const ComponentLifecycleTargetExpectationDTO old{component, first.value().target_generation};
  const auto next = f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                                 LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(next.ok());
  const auto stale = f.journal.RecordLifecycleTargetReached(old, true, 0U);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, kLifecycleFactsStaleErrorCode);
  EXPECT_EQ(stale.error().details.at("reason"), "target_retired_or_superseded");
  EXPECT_FALSE(f.journal.LoadLifecycleTarget(component).value()->reached);
  const ComponentLifecycleTargetExpectationDTO current{component, next.value().target_generation};
  EXPECT_TRUE(f.journal.RecordLifecycleTargetReached(current, true, 0U).value());
  ASSERT_TRUE(f.journal.OpenComponentIncident(
      TestIncident("incident.target-reached", component, LifecycleBlockingCauseClassDTO::kFault, 10), 0U).ok());
  ASSERT_TRUE(f.journal.RecoverComponentIncident(f.owner,
      RecoverIntent("target.recover", f.owner), "operator", 20).ok());
  const auto old_recovery = f.journal.RecordLifecycleTargetReached(current, true, 0U);
  ASSERT_FALSE(old_recovery.ok());
  EXPECT_EQ(old_recovery.error().code, kLifecycleFactsStaleErrorCode);
  EXPECT_EQ(old_recovery.error().details.at("reason"), "component_lifecycle_journal_failure");
  EXPECT_FALSE(f.journal.LoadLifecycleTarget(component).value()->reached);
  ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner).ok());
  const auto retired = f.journal.RecordLifecycleTargetReached(current, true, 1U);
  ASSERT_FALSE(retired.ok());
  EXPECT_EQ(retired.error().code, kLifecycleFactsStaleErrorCode);
  EXPECT_EQ(retired.error().details.at("reason"), "target_retired_or_superseded");
}

TEST(SqliteCCLCJournal, IncidentRetiresOnlyActiveIntentAndRecoverDoesNotRestoreIt) {
  for (const auto state : {LifecycleTargetStateDTO::kActive, LifecycleTargetStateDTO::kPrepared,
                           LifecycleTargetStateDTO::kDisabled}) {
    SCOPED_TRACE(to_string(state));
    TargetCASFixture f;
    const ControllableComponentId component("component.1");
    const auto target = f.journal.SetLifecycleTarget(component, state, LifecycleTargetOriginDTO::kOperator);
    ASSERT_TRUE(target.ok());
    ASSERT_TRUE(f.journal.RecordInterventionCause(component, ExpectedDomain().control_domain_id,
        LifecycleBlockingCauseClassDTO::kFault, 10, 0U).ok());
    const auto before = f.journal.LoadLifecycleTargetFacts(component);
    const auto fact = f.journal.LoadCurrentComponentLifecycleFact(component);
    ASSERT_TRUE(before.ok() && fact.ok());
    const auto incident = TestIncident("incident.retire", component, LifecycleBlockingCauseClassDTO::kFault, 10);
    const auto opened = f.journal.OpenComponentIncident(incident, 0U);
    ASSERT_TRUE(opened.ok());
    const auto after = f.journal.LoadLifecycleTargetFacts(component);
    ASSERT_TRUE(after.ok() && after.value().open_incident);
    EXPECT_EQ(after.value().recovery_generation, before.value().recovery_generation);
    EXPECT_EQ(after.value().intervention_causes, before.value().intervention_causes);
    EXPECT_EQ(after.value().open_incident->record_json, incident.record_json);
    EXPECT_EQ(f.journal.LoadCurrentComponentLifecycleFact(component).value(), fact.value());
    EXPECT_EQ(after.value().target.has_value(), state != LifecycleTargetStateDTO::kActive);
    EXPECT_EQ(RawScalar(f.path, "SELECT target_generation FROM cclc_component_lifecycle_targets"),
              static_cast<std::int64_t>(target.value().target_generation));
    EXPECT_EQ(f.journal.OpenComponentIncident(incident, 0U).value(), opened.value());
    const auto records_before_recover = f.operation_journal.sequence.load();
    const auto recover = RecoverIntent("target.incident-recover", f.owner);
    const auto recovered = f.journal.RecoverComponentIncident(f.owner, recover, "operator", 20);
    ASSERT_TRUE(recovered.ok() && recovered.value() && recovered.value()->incident_recovery);
    EXPECT_FALSE(recovered.value()->acceptance);
    EXPECT_EQ(f.operation_journal.sequence.load(), records_before_recover);
    EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM lifecycle_transactions"), 0);
    EXPECT_EQ(RawScalar(f.path, "SELECT count(*) FROM cclc_effect_attempts"), 0);
    EXPECT_EQ(f.journal.LoadLifecycleTarget(component).value().has_value(), state != LifecycleTargetStateDTO::kActive);
    EXPECT_FALSE(f.journal.LoadOpenComponentIncident(component).value());
    EXPECT_EQ(f.journal.ReadLifecycleRequestResult(recover).value()->incident_recovery,
              recovered.value()->incident_recovery);
    EXPECT_FALSE(f.journal.RecoverComponentIncident(f.owner, recover, "operator", 30).value());
    const auto history = f.journal.LoadComponentIncidentEvents(0U, 16U);
    ASSERT_TRUE(history.ok());
    EXPECT_EQ(history.value().back().event_kind, kComponentIncidentAcknowledgedEventKind);
  }
}

TEST(SqliteCCLCJournal, CloseRetiresEveryActualTargetPreservingCausesAndIncidentHistory) {
  for (const auto state : {LifecycleTargetStateDTO::kActive, LifecycleTargetStateDTO::kPrepared,
                           LifecycleTargetStateDTO::kDisabled}) {
    SCOPED_TRACE(to_string(state));
    TargetCASFixture f;
    const ControllableComponentId component("component.1");
    const ControllableComponentId second("component.2");
    const auto route = ActiveConfiguration(ExpectedDomain());
    const auto second_route = SecondDomainRoute();
    const auto plan = f.journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
        "apply.close-retire", "bundle.b", {component, second},
        {{route.control_domain_id, route.active_control_configuration_generation},
         {second_route.control_domain_id, second_route.active_control_configuration_generation}},
        {}, {}, {}, {}));
    ASSERT_TRUE(plan.ok()) << plan.error().message;
    const auto planned_route = std::find_if(plan.value().routes.begin(), plan.value().routes.end(),
        [&](const auto& row) { return row.control_domain_id == route.control_domain_id; });
    ASSERT_NE(planned_route, plan.value().routes.end());
    ASSERT_EQ(planned_route->generation.value(),
              route.active_control_configuration_generation.value() + 1U);
    auto changed_route = route;
    changed_route.active_control_configuration_generation = planned_route->generation;
    const auto adopted = f.journal.AdoptNativeApplyFacts({
        NativeApplyPublication("apply.close-retire", 1U, "bundle.b", 3U),
        plan.value().components, {changed_route, second_route}});
    ASSERT_TRUE(adopted.ok()) << adopted.error().code << ": " << adopted.error().message;
    // A Prepared pursuit permits open-incident evidence without incident-time
    // Active retirement. Close's retirement itself applies to every kind.
    const auto target = f.journal.SetLifecycleTarget(component, state, LifecycleTargetOriginDTO::kOperator);
    ASSERT_TRUE(target.ok());
    const auto second_target = f.journal.SetLifecycleTarget(second,
        LifecycleTargetStateDTO::kPrepared, LifecycleTargetOriginDTO::kOperator);
    ASSERT_TRUE(second_target.ok());
    ASSERT_TRUE(f.journal.RecordInterventionCause(second, second_route.control_domain_id,
        LifecycleBlockingCauseClassDTO::kFault, 10, 0U).ok());
    const auto second_facts = f.journal.LoadLifecycleTargetFacts(second);
    ASSERT_TRUE(second_facts.ok());
    ASSERT_TRUE(f.journal.RecordInterventionCause(component, ExpectedDomain().control_domain_id,
        LifecycleBlockingCauseClassDTO::kEmergencyStop, 10, 0U).ok());
    if (state != LifecycleTargetStateDTO::kActive) {
      ASSERT_TRUE(f.journal.OpenComponentIncident(TestIncident("incident.close-retire", component,
          LifecycleBlockingCauseClassDTO::kEmergencyStop, 10), 0U).ok());
    }
    const auto before = f.journal.LoadLifecycleTargetFacts(component);
    const auto history = f.journal.LoadComponentIncidentEvents(0U, 16U);
    ASSERT_TRUE(before.ok() && history.ok());
    EXPECT_FALSE(f.journal.RetireLifecycleTargetsForClose(RequestOwnerEpoch("foreign-owner")).ok());
    EXPECT_EQ(f.journal.LoadLifecycleTargetFacts(component).value(), before.value());
    const auto retired = f.journal.RetireLifecycleTargetsForClose(f.owner);
    ASSERT_TRUE(retired.ok());
    EXPECT_EQ(retired.value(), (std::vector<ComponentLifecycleTargetDTO>{target.value(), second_target.value()}));
    const auto after = f.journal.LoadLifecycleTargetFacts(component);
    ASSERT_TRUE(after.ok());
    EXPECT_FALSE(after.value().target);
    const auto second_after = f.journal.LoadLifecycleTargetFacts(second);
    ASSERT_TRUE(second_after.ok());
    EXPECT_FALSE(second_after.value().target);
    EXPECT_EQ(second_after.value().recovery_generation, second_facts.value().recovery_generation);
    EXPECT_EQ(second_after.value().intervention_causes, second_facts.value().intervention_causes);
    EXPECT_EQ(after.value().recovery_generation, before.value().recovery_generation);
    EXPECT_EQ(after.value().intervention_causes, before.value().intervention_causes);
    EXPECT_EQ(after.value().open_incident, before.value().open_incident);
    EXPECT_EQ(f.journal.LoadComponentIncidentEvents(0U, 16U).value(), history.value());
    EXPECT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner).value().empty());
    f.journal.Close();
    SqliteCCLCJournal reopened(f.path, CellId("cell.1"), 2U);
    EXPECT_EQ(reopened.LoadLifecycleTargetFacts(component).value(), after.value());
    EXPECT_EQ(reopened.LoadLifecycleTargetFacts(second).value(), second_after.value());
  }
}

TEST(SqliteCCLCJournal, TargetRetirementCodecPreservesCompleteCASAndRejectsUnprovedRows) {
  CCLCLifecycleTargetRetirementIntentDTO intent{
      {RequestId("close.targets.1"), RequestOwnerEpoch("owner.1"), "composer.close", std::nullopt},
      {RequestOwnerEpoch("owner.1"), ActiveBundlePublicationGeneration(3U),
       ContentDigest("sha256:" + std::string(64U, 'a'))},
      {{ControllableComponentId("arm"), LifecycleTargetStateDTO::kDisabled,
        LifecycleTargetOriginDTO::kOperator, 7U, true, LifecycleBlockingCauseClassDTO::kFault}}};
  ASSERT_TRUE(intent.locally_valid());
  const detail::CCLCTargetRetirementRecordDTO record{intent, intent.expected_targets};
  const auto bytes = detail::EncodeCCLCTargetRetirementRecord(record);
  EXPECT_EQ(detail::DecodeCCLCTargetRetirementRecord(bytes), record);
  EXPECT_THROW(detail::DecodeCCLCTargetRetirementRecord(bytes + "x"), std::invalid_argument);
  EXPECT_THROW(detail::DecodeCCLCTargetRetirementRecord(bytes.substr(0U, bytes.size() - 1U)),
               std::invalid_argument);
  const auto digest = detail::CCLCTargetRetirementSemanticIntentDigest(intent);
  auto changed = intent;
  changed.expected_targets.front().reached = false;
  EXPECT_NE(detail::CCLCTargetRetirementSemanticIntentDigest(changed), digest);
  changed = intent;
  changed.expected_targets.front().recovered_cause.reset();
  EXPECT_NE(detail::CCLCTargetRetirementSemanticIntentDigest(changed), digest);
  changed = intent;
  changed.scope.publication_generation = ActiveBundlePublicationGeneration(4U);
  EXPECT_NE(detail::CCLCTargetRetirementSemanticIntentDigest(changed), digest);
  changed = intent;
  changed.request_context.correlation_id = "different attribution";
  EXPECT_NE(detail::CCLCTargetRetirementSemanticIntentDigest(changed), digest);
  changed = intent;
  changed.expected_targets.push_back(changed.expected_targets.front());
  EXPECT_FALSE(changed.locally_valid());
  auto wrong = record;
  wrong.retired_targets.clear();
  EXPECT_THROW(detail::EncodeCCLCTargetRetirementRecord(wrong), std::invalid_argument);
  changed = intent;
  changed.expected_targets.clear();
  const detail::CCLCTargetRetirementRecordDTO empty{changed, {}};
  EXPECT_EQ(detail::DecodeCCLCTargetRetirementRecord(detail::EncodeCCLCTargetRetirementRecord(empty)),
            empty);
}

namespace {
class RetirementTestLease final : public ICellCloseAdmissionLease {
 public:
  mutable int checks{};
  int fail_at{};
  Result<void> ValidatePrerequisites() const override {
    ++checks;
    if (checks == fail_at)
      return Result<void>::Err({"actual_close_guard_unknown", "actual prerequisite changed", {{"cause", "test-fault"}}});
    return Result<void>::Ok();
  }
};

CCLCLifecycleTargetRetirementIntentDTO RetirementIntent(SqliteCCLCJournal& journal,
    const RequestOwnerEpoch& owner, const std::string& id) {
  const auto rows = journal.LoadLifecycleTargetsForClose(owner);
  if (!rows.ok()) throw std::runtime_error(rows.error().message);
  return {{RequestId(id), owner, "composer.close", std::nullopt},
          {owner, ActiveBundlePublicationGeneration(2U), ContentDigest(Sha('d'))}, rows.value()};
}
}  // namespace

TEST(SqliteCCLCJournal, TargetRetirementExactReplayCannotRetireSuccessorAndSurvivesReopen) {
  TargetCASFixture f;
  ASSERT_TRUE(f.journal.SetLifecycleTarget(ControllableComponentId("component.1"),
      LifecycleTargetStateDTO::kDisabled, LifecycleTargetOriginDTO::kOperator).ok());
  const auto intent = RetirementIntent(f.journal, f.owner, "close.retire.1");
  RetirementTestLease lease;
  const auto retired = f.journal.RetireLifecycleTargetsForClose(f.owner, intent, lease);
  ASSERT_TRUE(retired.ok()) << retired.error().message;
  EXPECT_EQ(retired.value().retired_targets, intent.expected_targets);
  EXPECT_EQ(lease.checks, 2);
  EXPECT_TRUE(f.journal.LoadLifecycleTargets().value().empty());
  const auto successor = f.journal.SetLifecycleTarget(ControllableComponentId("component.1"),
      LifecycleTargetStateDTO::kPrepared, LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(successor.ok());
  const auto replay = f.journal.RetireLifecycleTargetsForClose(RequestOwnerEpoch("different.current.owner"), intent, lease);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(lease.checks, 2);
  EXPECT_EQ(f.journal.LoadLifecycleTargets().value(), std::vector<ComponentLifecycleTargetDTO>{successor.value()});
  f.journal.Close();
  SqliteCCLCJournal reopened(f.path, CellId("cell.1"), 2U);
  const auto read = reopened.ReadLifecycleTargetRetirementResult(intent);
  ASSERT_TRUE(read.ok()) << read.error().message;
  ASSERT_TRUE(read.value());
  EXPECT_EQ(read.value()->retired_targets, intent.expected_targets);
  EXPECT_EQ(reopened.LoadLifecycleTargets().value(), std::vector<ComponentLifecycleTargetDTO>{successor.value()});
}

TEST(SqliteCCLCJournal, TargetRetirementCompleteCASConflictsWithoutMembershipOrTargetWrites) {
  TargetCASFixture f;
  ASSERT_TRUE(f.journal.SetLifecycleTarget(ControllableComponentId("component.1"),
      LifecycleTargetStateDTO::kPrepared, LifecycleTargetOriginDTO::kOperator).ok());
  const auto intent = RetirementIntent(f.journal, f.owner, "close.cas.1");
  auto changed = intent;
  changed.expected_targets.front().reached = !changed.expected_targets.front().reached;
  RetirementTestLease lease;
  const auto refused = f.journal.RetireLifecycleTargetsForClose(f.owner, changed, lease);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_target_retirement_conflict");
  EXPECT_EQ(lease.checks, 0);
  EXPECT_EQ(f.journal.LoadLifecycleTargets().value(), intent.expected_targets);
  EXPECT_FALSE(f.journal.ReadLifecycleTargetRetirementResult(changed).value());
  const auto accepted = f.journal.RetireLifecycleTargetsForClose(f.owner, intent, lease);
  ASSERT_TRUE(accepted.ok());
  changed = intent;
  changed.scope.publication_generation = ActiveBundlePublicationGeneration(3U);
  const auto conflict = f.journal.RetireLifecycleTargetsForClose(f.owner, changed, lease);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "request_id_conflict");
  EXPECT_EQ(lease.checks, 2);
}

TEST(SqliteCCLCJournal, TargetRetirementPrecommitGuardRollsBackRowsMembershipAndTerminalSequence) {
  for (int fail_at : {1, 2}) {
    TargetCASFixture f;
    ASSERT_TRUE(f.journal.SetLifecycleTarget(ControllableComponentId("component.1"),
        LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator).ok());
    const auto intent = RetirementIntent(f.journal, f.owner, "close.guard.1");
    RetirementTestLease lease;
    lease.fail_at = fail_at;
    const auto refused = f.journal.RetireLifecycleTargetsForClose(f.owner, intent, lease);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "actual_close_guard_unknown");
    EXPECT_EQ(refused.error().details.at("cause"), "test-fault");
    EXPECT_EQ(f.journal.LoadLifecycleTargets().value(), intent.expected_targets);
    EXPECT_FALSE(f.journal.ReadLifecycleTargetRetirementResult(intent).value());
    RetirementTestLease allowed;
    ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner, intent, allowed).ok());
    f.journal.Close();
    SqliteCCLCJournal reopened(f.path, CellId("cell.1"), 2U);
    ASSERT_TRUE(reopened.ReadLifecycleTargetRetirementResult(intent).value());
  }
}

TEST(SqliteCCLCJournal, TargetRetirementSharesOrdinaryRequestNamespaceAndRefusesStaleFreshOwner) {
  TargetCASFixture f;
  const auto ordinary = Intent("shared.request.1", f.owner, 1U);
  ASSERT_TRUE(f.journal.RecordLifecycleRejection(f.owner, ordinary,
      {"no_admission", "known refusal", {{"effect", "not_started"}}}, std::nullopt).ok());
  const auto intent = RetirementIntent(f.journal, f.owner, "shared.request.1");
  RetirementTestLease lease;
  const auto collision = f.journal.RetireLifecycleTargetsForClose(f.owner, intent, lease);
  ASSERT_FALSE(collision.ok());
  EXPECT_EQ(collision.error().code, "request_id_conflict");
  auto stale = intent;
  stale.request_context.request_id = RequestId("fresh.stale.1");
  const auto refused = f.journal.RetireLifecycleTargetsForClose(RequestOwnerEpoch("stale.owner"), stale, lease);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "stale_request_owner_epoch");
  EXPECT_EQ(lease.checks, 0);
}

TEST(SqliteCCLCJournal, TargetRetirementSharesGlobalHistoryCapacityAndKeepsExpiryTombstone) {
  TargetCASFixture f(1U, 3U);
  RetirementTestLease lease;
  const auto first = RetirementIntent(f.journal, f.owner, "close.history.1");
  ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner, first, lease).ok());
  const auto second = RetirementIntent(f.journal, f.owner, "close.history.2");
  ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner, second, lease).ok());
  const auto expired = f.journal.ReadLifecycleTargetRetirementResult(first);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code, "request_result_expired");
  const auto ordinary = Intent("ordinary.history.1", f.owner, 1U);
  ASSERT_TRUE(f.journal.RecordLifecycleRejection(f.owner, ordinary,
      {"no_admission", "ordinary refusal", {{"effect", "not_started"}}}, std::nullopt).ok());
  const auto shared_expired = f.journal.ReadLifecycleTargetRetirementResult(second);
  ASSERT_FALSE(shared_expired.ok());
  EXPECT_EQ(shared_expired.error().code, "request_result_expired");
  auto fresh = second;
  fresh.request_context.request_id = RequestId("close.history.full");
  const auto full = f.journal.RetireLifecycleTargetsForClose(f.owner, fresh, lease);
  ASSERT_FALSE(full.ok());
  EXPECT_EQ(full.error().code, "request_ledger_full");
  f.journal.Close();
  SqliteCCLCJournal reopened(f.path, CellId("cell.1"), 1U);
  EXPECT_EQ(reopened.ReadLifecycleTargetRetirementResult(first).error().code, "request_result_expired");
}

TEST(SqliteCCLCJournal, TargetRetirementOversizedResultUsesExistingByteCompactionAndNeverInventsReplay) {
  ScopedTemporaryDirectory temporary;
  SqliteCCLCJournal journal(JournalPath(temporary), CellId("cell.1"), 1U, 1U);
  RequestOwnerEpoch owner("owner.bytes.seed");
  ASSERT_TRUE(BeginTestOwnerEpoch(journal, owner, 1, 2U).ok());
  const auto intent = RetirementIntent(journal, owner, "close.bytes.1");
  RetirementTestLease lease;
  ASSERT_TRUE(journal.RetireLifecycleTargetsForClose(owner, intent, lease).ok());
  const auto expired = journal.ReadLifecycleTargetRetirementResult(intent);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code, "request_result_expired");
  EXPECT_TRUE(journal.LoadLifecycleTargets().value().empty());
  journal.Close();
  SqliteCCLCJournal reopened(JournalPath(temporary), CellId("cell.1"), 1U, 1U);
  EXPECT_EQ(reopened.ReadLifecycleTargetRetirementResult(intent).error().code, "request_result_expired");
}

TEST(SqliteCCLCJournal, TargetRetirementStartupRejectsMissingResultAndRegressedTerminalOrder) {
  for (const auto* corruption : {
      "UPDATE cclc_request_memberships SET target_retirement_sequence=0",
      "UPDATE cclc_request_memberships SET target_retirement_blob=NULL"}) {
    TargetCASFixture f;
    RetirementTestLease lease;
    const auto intent = RetirementIntent(f.journal, f.owner, "close.corrupt.1");
    ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner, intent, lease).ok());
    f.journal.Close();
    ExecRaw(f.path, corruption);
    EXPECT_THROW((SqliteCCLCJournal(f.path, CellId("cell.1"), 2U)), std::runtime_error);
  }
}

TEST(SqliteCCLCJournal, TargetRetirementStartupRejectsDuplicateGlobalTerminalOrder) {
  TargetCASFixture f;
  RetirementTestLease lease;
  const auto first = RetirementIntent(f.journal, f.owner, "close.order.1");
  ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner, first, lease).ok());
  const auto second = RetirementIntent(f.journal, f.owner, "close.order.2");
  ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner, second, lease).ok());
  EXPECT_EQ(RawScalar(f.path, "SELECT count(DISTINCT target_retirement_sequence) FROM cclc_request_memberships"), 2);
  f.journal.Close();
  ExecRaw(f.path, "UPDATE cclc_request_memberships SET target_retirement_sequence=1 WHERE target_retirement_sequence IS NOT NULL");
  EXPECT_THROW((SqliteCCLCJournal(f.path, CellId("cell.1"), 2U)), std::runtime_error);
}

TEST(SqliteCCLCJournal, CloseRetirementReasonSurvivesAuthenticRemovalAndPassiveReplay) {
  TargetCASFixture f;
  const ControllableComponentId component("component.1");
  ASSERT_TRUE(f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                          LifecycleTargetOriginDTO::kOperator).ok());
  const auto intent = RetirementIntent(f.journal, f.owner, "close.reason.1");
  RetirementTestLease lease;
  ASSERT_TRUE(f.journal.RetireLifecycleTargetsForClose(f.owner, intent, lease).ok());
  auto retained = f.journal.ReadLifecycleTargetRetention(component);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value());
  EXPECT_FALSE(retained.value()->present);
  EXPECT_EQ(retained.value()->retirement_reason, LifecycleTargetRetirementReasonDTO::kClose);
  EXPECT_EQ(retained.value()->target.target_generation, intent.expected_targets.front().target_generation);
  const auto removed = f.journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
      "apply.reason.remove", "bundle.reason.empty", {}, {}, {}, {component}, {},
      {ControlDomainId("domain.1")}));
  ASSERT_TRUE(removed.ok()) << removed.error().message;
  ASSERT_TRUE(f.journal.AdoptNativeApplyFacts(
      {NativeApplyPublication("apply.reason.remove", 1U, "bundle.reason.empty", 3U),
       removed.value().components, {}}).ok());
  retained = f.journal.ReadLifecycleTargetRetention(component);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value());
  EXPECT_EQ(retained.value()->retirement_reason, LifecycleTargetRetirementReasonDTO::kClose);
  EXPECT_EQ(retained.value()->target.target_generation, intent.expected_targets.front().target_generation);
  f.journal.Close();
  SqliteCCLCJournal reopened(f.path, CellId("cell.1"), 2U);
  ASSERT_EQ(reopened.ReadLifecycleTargetRetention(component).value()->retirement_reason,
            LifecycleTargetRetirementReasonDTO::kClose);
  auto route = ActiveConfiguration(ExpectedDomain());
  const auto readded = reopened.PlanNativeApply(
      NativeApplyPlanRequest("apply.reason.readd", 1U, "bundle.a", route));
  ASSERT_TRUE(readded.ok()) << readded.error().message;
  ASSERT_EQ(readded.value().routes.size(), 1U);
  route.active_control_configuration_generation = readded.value().routes.front().generation;
  ASSERT_TRUE(reopened.AdoptNativeApplyFacts(
      {NativeApplyPublication("apply.reason.readd", 1U, "bundle.a", 4U),
       readded.value().components, {route}}).ok());
  const auto successor = reopened.SetLifecycleTarget(component, LifecycleTargetStateDTO::kPrepared,
                                                     LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(successor.ok());
  const auto fresh = reopened.ReadLifecycleTargetRetention(component);
  ASSERT_TRUE(fresh.ok());
  ASSERT_TRUE(fresh.value());
  EXPECT_TRUE(fresh.value()->present);
  EXPECT_EQ(fresh.value()->retirement_reason, LifecycleTargetRetirementReasonDTO::kNone);
  EXPECT_GT(fresh.value()->target.target_generation, intent.expected_targets.front().target_generation);
  const auto replay = reopened.RetireLifecycleTargetsForClose(f.owner, intent, lease);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(reopened.ReadLifecycleTargetRetention(component).value(), fresh.value());
}

TEST(SqliteCCLCJournal, OrdinaryTargetCancellationHasDistinctRetirementReason) {
  TargetCASFixture f;
  const ControllableComponentId component("component.1");
  ASSERT_FALSE(f.journal.ReadLifecycleTargetRetention(component).value());
  const auto target = f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                                  LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(target.ok());
  const auto removed = f.journal.PlanNativeApply(NativeApplyPlanRequestWithDelta(
      "apply.reason.remove", "bundle.reason.empty", {}, {}, {}, {component}, {},
      {ControlDomainId("domain.1")}));
  ASSERT_TRUE(removed.ok()) << removed.error().message;
  ASSERT_TRUE(f.journal.AdoptNativeApplyFacts(
      {NativeApplyPublication("apply.reason.remove", 1U, "bundle.reason.empty", 3U),
       removed.value().components, {}}).ok());
  const auto retained = f.journal.ReadLifecycleTargetRetention(component);
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value());
  EXPECT_FALSE(retained.value()->present);
  EXPECT_EQ(retained.value()->retirement_reason, LifecycleTargetRetirementReasonDTO::kOrdinaryCancellation);
  EXPECT_EQ(retained.value()->target.target_generation, target.value().target_generation);
}

TEST(SqliteCCLCJournal, TargetRetirementReasonRejectsCorruptProofAndEarlierFormatWithoutMigration) {
  for (const auto* corruption : {
      "PRAGMA ignore_check_constraints=ON; UPDATE cclc_component_lifecycle_targets SET retirement_reason=99",
      "PRAGMA ignore_check_constraints=ON; UPDATE cclc_component_lifecycle_targets SET retirement_reason=2",
      "PRAGMA user_version=18"}) {
    TargetCASFixture f;
    const ControllableComponentId component("component.1");
    ASSERT_TRUE(f.journal.SetLifecycleTarget(component, LifecycleTargetStateDTO::kActive,
                                            LifecycleTargetOriginDTO::kOperator).ok());
    f.journal.Close();
    ExecRaw(f.path, corruption);
    const auto before_version = RawScalar(f.path, "PRAGMA user_version");
    const auto before_reason = RawScalar(f.path,
        "SELECT retirement_reason FROM cclc_component_lifecycle_targets WHERE controllable_component_id='component.1'");
    EXPECT_THROW((SqliteCCLCJournal(f.path, CellId("cell.1"), 2U)), std::runtime_error);
    EXPECT_EQ(RawScalar(f.path, "PRAGMA user_version"), before_version);
    EXPECT_EQ(RawScalar(f.path,
        "SELECT retirement_reason FROM cclc_component_lifecycle_targets WHERE controllable_component_id='component.1'"),
        before_reason);
  }
}

TEST(SqliteCCLCJournal, TargetRetirementCommitRollbackIsOrdinaryOnlyWithExactReadableOriginals) {
  for (const bool unreadable : {false, true}) {
    SCOPED_TRACE(unreadable);
    test::CloseRetirementCommitFailure failure;
    test::ScopedCloseRetirementCommitFailure hook(failure);
    TargetCASFixture f;
    ASSERT_TRUE(f.journal.SetLifecycleTarget(ControllableComponentId("component.1"),
        LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator).ok());
    const auto intent = RetirementIntent(f.journal, f.owner, "close.commit.1");
    RetirementTestLease lease;
    failure.fail_readback = unreadable;
    failure.armed = true;
    const auto result = f.journal.RetireLifecycleTargetsForClose(f.owner, intent, lease);
    EXPECT_EQ(failure.commit_failures, 1);
    ASSERT_FALSE(result.ok());
    if (unreadable) {
      EXPECT_EQ(result.error().code, "persistence_outcome_unknown");
      EXPECT_EQ(result.error().details.at("reason"), "close_target_retirement_commit_unknown");
    } else {
      EXPECT_NE(result.error().code, "persistence_outcome_unknown");
    }
    failure.deny_reads = false;
    EXPECT_EQ(f.journal.LoadLifecycleTargets().value(), intent.expected_targets);
    EXPECT_FALSE(f.journal.ReadLifecycleTargetRetirementResult(intent).value());
    EXPECT_EQ(f.journal.ReadLifecycleTargetRetention(ControllableComponentId("component.1")).value()->retirement_reason,
              LifecycleTargetRetirementReasonDTO::kNone);
  }
}
}  // namespace manyforge::core
