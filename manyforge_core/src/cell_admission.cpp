#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;
constexpr std::uint64_t kMaximumSafeCounter = 9'007'199'254'740'991ULL;

template <typename Value>
[[nodiscard]] Result<Value> Invalid(std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{"invalid_request", "cell-admission value is invalid", {{"reason", reason}}});
}

[[nodiscard]] bool BoundedIdentity(std::string_view value) noexcept {
  return !value.empty() && value.size() <= kMaximumCellAdmissionIdentityBytes;
}

[[nodiscard]] bool Digest(const ContentDigest& value) noexcept {
  return content_identity::is_sha256_digest(value.value());
}

[[nodiscard]] std::size_t BinaryString(std::string_view value) {
  return sizeof(std::uint64_t) + value.size();
}

[[nodiscard]] std::size_t BinaryIntent(
    const CellAdmissionClaimIntentDTO& intent) {
  std::size_t result =
      3U * sizeof(std::uint64_t) +
      BinaryString(intent.owner_epoch.value()) +
      BinaryString(intent.owner_transaction_id) +
      BinaryString(intent.request_id.value()) +
      BinaryString(intent.request_payload_digest.value()) +
      2U * sizeof(std::uint64_t);
  for (const auto& resource : intent.scope.resources) {
    result += BinaryString(resource.value());
  }
  for (const auto& configuration : intent.scope.configurations) {
    result += BinaryString(configuration.control_domain_id.value()) +
              BinaryString(configuration.active_configuration_id.value()) +
              sizeof(std::uint64_t);
  }
  result += 3U;
  if (intent.program_cycle_evidence.has_value()) {
    result += BinaryString(
                  intent.program_cycle_evidence->draft_program_revision.value()) +
              BinaryString(
                  intent.program_cycle_evidence->applied_program_revision.value());
  }
  if (intent.intervention_evidence.has_value()) {
    const auto& evidence = *intent.intervention_evidence;
    result += sizeof(std::uint64_t) +
              BinaryString(evidence.offering_owner_epoch.value()) +
              BinaryString(evidence.offer_transaction_id) +
              BinaryString(evidence.offer_digest.value()) + 1U;
    if (evidence.parent_program_cycle_claim_generation.has_value()) {
      result += sizeof(std::uint64_t);
    }
  }
  if (intent.program_node_evidence.has_value()) {
    const auto& evidence = *intent.program_node_evidence;
    result += BinaryString(evidence.program_runner_owner_epoch.value()) +
              BinaryString(evidence.program_run_id.value()) + sizeof(std::uint64_t) +
              BinaryString(evidence.node_instance_id) + BinaryString(evidence.node_attempt_id) +
              BinaryString(evidence.owner_api);
  }
  return result;
}

[[nodiscard]] std::size_t BinaryClaim(const CellAdmissionClaimDTO& claim) {
  return BinaryIntent(claim.intent) + 3U * sizeof(std::uint64_t);
}

[[nodiscard]] std::size_t BinaryRelease(
    const CellAdmissionClaimReleaseDTO& release) {
  return 4U * sizeof(std::uint64_t) +
         BinaryString(release.owner_epoch.value()) +
         BinaryString(release.owner_transaction_id) +
         BinaryString(release.request_id.value()) +
         BinaryString(release.request_payload_digest.value()) +
         BinaryString(release.settlement.evidence_digest.value());
}

[[nodiscard]] std::size_t BinaryOwner(
    const CellAdmissionCurrentOwnerEpochDTO& owner) {
  return sizeof(std::uint64_t) + BinaryString(owner.owner_epoch.value());
}

[[nodiscard]] std::size_t BinaryReconcileSettlement(
    const CellAdmissionReconcileSettlementDTO& settlement) {
  return BinaryClaim(settlement.predecessor_claim) +
         2U * sizeof(std::uint64_t) +
         BinaryString(settlement.successor_owner_epoch.value()) +
         BinaryString(settlement.reconciliation_evidence_digest.value());
}

constexpr std::size_t kRecoveryCommandEnvelopeBinaryBytes =
    5U + sizeof(std::uint64_t) +
    std::string_view("cell_admission_recovery_receipt").size();
constexpr std::size_t kJsonStructuralAllowanceBytes = 16U * 1024U;

[[nodiscard]] std::size_t JsonUpperBound(std::size_t binary_bytes) {
  // Any JSON string byte can expand to a six-byte \u00XX escape. Binary
  // framing already charges eight bytes per scalar/collection; the fixed
  // allowance covers every field name and both command envelopes.
  return 6U * binary_bytes + kJsonStructuralAllowanceBytes;
}

[[nodiscard]] bool OwnerMatchesKind(CellAdmissionClaimKindDTO kind,
                                    CellAdmissionOwnerRoleDTO role) noexcept {
  switch (kind) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
      return role == CellAdmissionOwnerRoleDTO::kProgramRunner;
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
      return role == CellAdmissionOwnerRoleDTO::kAuthoringOwner;
    case CellAdmissionClaimKindDTO::kManagerApply:
      return role == CellAdmissionOwnerRoleDTO::kManager;
    case CellAdmissionClaimKindDTO::kComponentLifecycle:
      return role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner;
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return role == CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner;
    case CellAdmissionClaimKindDTO::kManualOperation:
      return role == CellAdmissionOwnerRoleDTO::kManualOperationOwner;
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
      return role == CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner ||
             role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner;
  }
  return false;
}

[[nodiscard]] CanonicalValue ScopeValue(const CellAdmissionScopeDTO& scope) {
  CanonicalValue::Array resources;
  resources.reserve(scope.resources.size());
  for (const auto& resource : scope.resources) {
    resources.emplace_back(resource.value());
  }
  CanonicalValue::Array configurations;
  configurations.reserve(scope.configurations.size());
  for (const auto& configuration : scope.configurations) {
    configurations.emplace_back(CanonicalValue::Object{
        {"active_configuration_generation",
         CanonicalValue(static_cast<double>(
             configuration.active_configuration_generation.value()))},
        {"active_configuration_id", CanonicalValue(configuration.active_configuration_id.value())},
        {"control_domain_id", CanonicalValue(configuration.control_domain_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"configurations", CanonicalValue(std::move(configurations))},
      {"resources", CanonicalValue(std::move(resources))},
  });
}

}  // namespace

std::string_view to_string(CellAdmissionClaimKindDTO value) noexcept {
  switch (value) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
      return "program_cycle";
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
      return "authoring_mutation";
    case CellAdmissionClaimKindDTO::kManagerApply:
      return "manager_apply";
    case CellAdmissionClaimKindDTO::kComponentLifecycle:
      return "component_lifecycle";
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return "authored_scene";
    case CellAdmissionClaimKindDTO::kManualOperation:
      return "manual_operation";
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
      return "intervention_or_recovery";
  }
  return {};
}

std::string_view to_string(CellAdmissionOwnerRoleDTO value) noexcept {
  switch (value) {
    case CellAdmissionOwnerRoleDTO::kProgramRunner:
      return "program_runner";
    case CellAdmissionOwnerRoleDTO::kAuthoringOwner:
      return "authoring_owner";
    case CellAdmissionOwnerRoleDTO::kManager:
      return "manager";
    case CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner:
      return "component_lifecycle_owner";
    case CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner:
      return "authored_scene_owner";
    case CellAdmissionOwnerRoleDTO::kManualOperationOwner:
      return "manual_operation_owner";
    case CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner:
      return "intervention_or_recovery_owner";
  }
  return {};
}

std::string_view to_string(ProgramCycleStateDTO value) noexcept {
  switch (value) {
    case ProgramCycleStateDTO::kIdleAfterReset:
      return "idle_after_reset";
    case ProgramCycleStateDTO::kStarting:
      return "starting";
    case ProgramCycleStateDTO::kRunning:
      return "running";
    case ProgramCycleStateDTO::kStopping:
      return "stopping";
    case ProgramCycleStateDTO::kTerminalAwaitingReset:
      return "terminal_awaiting_reset";
    case ProgramCycleStateDTO::kInterventionOrRecovery:
      return "intervention_or_recovery";
    case ProgramCycleStateDTO::kResetting:
      return "resetting";
    case ProgramCycleStateDTO::kRecoveryRequired:
      return "recovery_required";
  }
  return {};
}

std::string_view to_string(CellAdmissionSettlementKindDTO value) noexcept {
  switch (value) {
    case CellAdmissionSettlementKindDTO::kOwnerCommit:
      return "owner_commit";
    case CellAdmissionSettlementKindDTO::kProvenNoEffect:
      return "proven_no_effect";
    case CellAdmissionSettlementKindDTO::kTerminalEffectsSettled:
      return "terminal_effects_settled";
    case CellAdmissionSettlementKindDTO::kExplicitReset:
      return "explicit_reset";
    case CellAdmissionSettlementKindDTO::kReconciled:
      return "reconciled";
  }
  return {};
}

std::string_view to_string(CellAdmissionRecoveryReceiptStatusDTO value) noexcept {
  switch (value) {
    case CellAdmissionRecoveryReceiptStatusDTO::kUnknown:
      return "unknown";
    case CellAdmissionRecoveryReceiptStatusDTO::kConflicting:
      return "conflicting";
    case CellAdmissionRecoveryReceiptStatusDTO::kHeldExact:
      return "held_exact";
    case CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact:
      return "released_exact";
    case CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact:
      return "reconciled_exact";
  }
  return {};
}

Result<void> ValidateCellAdmissionScope(const CellAdmissionScopeDTO& scope) {
  if (scope.resources.size() > kMaximumCellAdmissionScopeEntries ||
      scope.configurations.size() > kMaximumCellAdmissionScopeEntries ||
      !std::is_sorted(scope.resources.begin(), scope.resources.end()) ||
      std::adjacent_find(scope.resources.begin(), scope.resources.end()) != scope.resources.end() ||
      std::any_of(scope.resources.begin(), scope.resources.end(),
                  [](const auto& value) {
                    return !BoundedIdentity(value.value());
                  })) {
    return Invalid<void>("resource_scope_not_canonical");
  }
  const auto less = [](const auto& left, const auto& right) {
    return std::tie(left.control_domain_id, left.active_configuration_id,
                    left.active_configuration_generation) <
           std::tie(right.control_domain_id, right.active_configuration_id,
                    right.active_configuration_generation);
  };
  if (!std::is_sorted(scope.configurations.begin(), scope.configurations.end(), less) ||
      std::adjacent_find(scope.configurations.begin(), scope.configurations.end()) !=
          scope.configurations.end() ||
      std::any_of(scope.configurations.begin(), scope.configurations.end(), [](const auto& row) {
        return !BoundedIdentity(row.control_domain_id.value()) ||
               !BoundedIdentity(row.active_configuration_id.value()) ||
               !row.active_configuration_generation.nonzero() ||
               row.active_configuration_generation.value() > kMaximumSafeCounter;
      })) {
    return Invalid<void>("configuration_scope_not_canonical");
  }
  return Result<void>::Ok();
}

bool ProgramNodeAdmissionEvidenceDTO::locally_valid() const noexcept {
  return BoundedIdentity(program_runner_owner_epoch.value()) &&
         BoundedIdentity(program_run_id.value()) &&
         parent_program_cycle_claim_generation.nonzero() &&
         parent_program_cycle_claim_generation.value() <= kMaximumSafeCounter &&
         BoundedIdentity(node_instance_id) && !node_attempt_id.empty() &&
         node_attempt_id.size() <= 512U && node_instance_id.find('\0') == std::string::npos &&
         node_attempt_id.find('\0') == std::string::npos &&
         (owner_api == "scene.transaction" || owner_api == "scene.carried_attachment" ||
          owner_api == "cclc.lifecycle" || owner_api == "cclc.control_route");
}

Result<void> ValidateCellAdmissionClaimIntent(const CellAdmissionClaimIntentDTO& intent) {
  if (to_string(intent.claim_kind).empty() || to_string(intent.owner_role).empty() ||
      !OwnerMatchesKind(intent.claim_kind, intent.owner_role) ||
      !BoundedIdentity(intent.owner_epoch.value()) ||
      !BoundedIdentity(intent.owner_transaction_id) ||
      !BoundedIdentity(intent.request_id.value()) || !Digest(intent.request_payload_digest) ||
      !intent.expected_admission_generation.nonzero() ||
      intent.expected_admission_generation.value() > kMaximumSafeCounter) {
    return Invalid<void>("claim_identity_invalid");
  }
  auto scope = ValidateCellAdmissionScope(intent.scope);
  if (!scope.ok()) {
    return scope;
  }
  const bool intervention = intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery;
  const bool held_manual = intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation &&
                           intent.intervention_evidence.has_value();
  if ((intervention || held_manual) != intent.intervention_evidence.has_value()) {
    return Invalid<void>("intervention_evidence_presence_invalid");
  }
  const bool program_cycle = intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle;
  if (program_cycle != intent.program_cycle_evidence.has_value() ||
      (intent.program_cycle_evidence.has_value() &&
       (intent.program_cycle_evidence->draft_program_revision.empty() ||
        intent.program_cycle_evidence->applied_program_revision.empty() ||
        intent.program_cycle_evidence->draft_program_revision !=
            intent.program_cycle_evidence->applied_program_revision))) {
    return Invalid<void>("program_cycle_draft_applied_mismatch");
  }
  if (intent.intervention_evidence.has_value()) {
    const auto& evidence = *intent.intervention_evidence;
    if (to_string(evidence.offering_owner_role).empty() ||
        !BoundedIdentity(evidence.offering_owner_epoch.value()) ||
        !BoundedIdentity(evidence.offer_transaction_id) || !Digest(evidence.offer_digest) ||
        (evidence.parent_program_cycle_claim_generation.has_value() &&
         !evidence.parent_program_cycle_claim_generation->nonzero())) {
      return Invalid<void>("intervention_evidence_invalid");
    }
    if (held_manual &&
        (evidence.offering_owner_role != CellAdmissionOwnerRoleDTO::kManualOperationOwner ||
         evidence.offering_owner_epoch != intent.owner_epoch ||
         evidence.offer_transaction_id != intent.owner_transaction_id ||
         !evidence.parent_program_cycle_claim_generation || intent.scope.resources.empty())) {
      return Invalid<void>("manual_handover_evidence_invalid");
    }
    // Both recovery offers remain private CCLC/Kernel composition. The route
    // owner retains its observation-only, one-configuration shape; selected
    // bring-up has the explicitly selected nonempty physical closure. Shape
    // validation alone never authorizes either operation.
    if (intervention && intent.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
      const bool exact_owner =
          evidence.offering_owner_role == intent.owner_role &&
          evidence.offering_owner_epoch == intent.owner_epoch &&
          evidence.offer_transaction_id == intent.owner_transaction_id &&
          evidence.offer_digest == intent.request_payload_digest &&
          !evidence.parent_program_cycle_claim_generation.has_value();
      const bool route = intent.owner_transaction_id.starts_with("route-selection.") &&
                         intent.scope.resources.empty() && intent.scope.configurations.size() == 1U;
      const bool selected = intent.owner_transaction_id.starts_with("selected-lifecycle.") &&
                            !intent.scope.resources.empty() && !intent.scope.configurations.empty();
      if (!exact_owner || (!route && !selected)) {
        return Invalid<void>(intent.owner_transaction_id.starts_with("selected-lifecycle.")
                                 ? "cclc_selected_recovery_evidence_invalid"
                                 : "cclc_route_recovery_evidence_invalid");
      }
    }
  }
  if (intent.program_node_evidence.has_value()) {
    const auto& evidence = *intent.program_node_evidence;
    const bool scene = intent.claim_kind == CellAdmissionClaimKindDTO::kAuthoredScene &&
                       (evidence.owner_api == "scene.transaction" ||
                        evidence.owner_api == "scene.carried_attachment");
    if (!scene || intent.program_cycle_evidence.has_value() ||
        intent.intervention_evidence.has_value() || !evidence.locally_valid()) {
      return Invalid<void>("program_node_evidence_invalid");
    }
  }
  return Result<void>::Ok();
}

Result<void> ValidateCellAdmissionClaimRelease(const CellAdmissionClaimReleaseDTO& release) {
  if (to_string(release.owner_role).empty() || !BoundedIdentity(release.owner_epoch.value()) ||
      !BoundedIdentity(release.owner_transaction_id) ||
      !BoundedIdentity(release.request_id.value()) || !Digest(release.request_payload_digest) ||
      !release.acquired_admission_generation.nonzero() || !release.claim_generation.nonzero() ||
      release.acquired_admission_generation.value() > kMaximumSafeCounter ||
      release.claim_generation.value() > kMaximumSafeCounter ||
      to_string(release.settlement.settlement_kind).empty() ||
      !Digest(release.settlement.evidence_digest)) {
    return Invalid<void>("release_identity_invalid");
  }
  return Result<void>::Ok();
}

Result<void> ValidateCellAdmissionRecoveryReceiptQuery(
    const CellAdmissionRecoveryReceiptQueryDTO& query) {
  auto intent = ValidateCellAdmissionClaimIntent(query.intent);
  if (!intent.ok()) {
    return intent;
  }
  if (query.expected_claim.has_value()) {
    const auto& claim = *query.expected_claim;
    if (claim.intent != query.intent || !claim.claim_generation.nonzero() ||
        !claim.acquired_admission_generation.nonzero() ||
        claim.acquired_admission_generation !=
            claim.intent.expected_admission_generation ||
        claim.accepted_at_ns < 0) {
      return Invalid<void>("recovery_expected_claim_invalid");
    }
  }
  if (query.expected_release.has_value()) {
    if (!query.expected_claim.has_value() ||
        !ValidateCellAdmissionClaimRelease(*query.expected_release).ok()) {
      return Invalid<void>("recovery_expected_release_invalid");
    }
    const auto& claim = *query.expected_claim;
    const auto& release = *query.expected_release;
    if (claim.intent.owner_role != release.owner_role ||
        claim.intent.owner_epoch != release.owner_epoch ||
        claim.intent.owner_transaction_id != release.owner_transaction_id ||
        claim.intent.request_id != release.request_id ||
        claim.intent.request_payload_digest != release.request_payload_digest ||
        claim.acquired_admission_generation !=
            release.acquired_admission_generation ||
        claim.claim_generation != release.claim_generation) {
      return Invalid<void>("recovery_expected_release_not_exact");
    }
  }
  return Result<void>::Ok();
}

Result<void> ValidateCellAdmissionRecoveryReceipt(
    const CellAdmissionRecoveryReceiptDTO& receipt) {
  if (to_string(receipt.status).empty() || receipt.kernel_owner_epoch.empty() ||
      !receipt.admission_generation.nonzero() ||
      receipt.held_claims.size() > kMaximumCellAdmissionClaims ||
      receipt.current_owner_epochs.size() > kMaximumCellAdmissionClaims ||
      !std::is_sorted(receipt.held_claims.begin(), receipt.held_claims.end(),
                      [](const auto& left, const auto& right) {
                        return left.claim_generation < right.claim_generation;
                      }) ||
      std::adjacent_find(receipt.held_claims.begin(), receipt.held_claims.end(),
                         [](const auto& left, const auto& right) {
                           return left.claim_generation == right.claim_generation;
                         }) != receipt.held_claims.end() ||
      !std::is_sorted(receipt.current_owner_epochs.begin(),
                      receipt.current_owner_epochs.end(),
                      [](const auto& left, const auto& right) {
                        return left.owner_role < right.owner_role;
                      }) ||
      std::adjacent_find(receipt.current_owner_epochs.begin(),
                         receipt.current_owner_epochs.end(),
                         [](const auto& left, const auto& right) {
                           return left.owner_role == right.owner_role;
                         }) != receipt.current_owner_epochs.end()) {
    return Invalid<void>("recovery_receipt_inventory_invalid");
  }
  for (const auto& claim : receipt.held_claims) {
    if (!ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
        !claim.claim_generation.nonzero() ||
        !claim.acquired_admission_generation.nonzero() ||
        claim.accepted_at_ns < 0) {
      return Invalid<void>("recovery_receipt_held_claim_invalid");
    }
  }
  if (std::any_of(receipt.current_owner_epochs.begin(),
                  receipt.current_owner_epochs.end(), [](const auto& owner) {
                    return to_string(owner.owner_role).empty() ||
                           owner.owner_epoch.empty();
                  })) {
    return Invalid<void>("recovery_receipt_owner_invalid");
  }
  const bool exact =
      receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact ||
      receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact;
  if (exact != receipt.matched_claim.has_value() ||
      (receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact) !=
          receipt.matched_release.has_value() ||
      (receipt.status ==
       CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact) !=
          receipt.matched_reconcile_settlement.has_value() ||
      (receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
       (receipt.matched_release.has_value() ||
        receipt.matched_reconcile_settlement.has_value())) ||
      (!exact && (receipt.matched_release.has_value() ||
                  receipt.matched_reconcile_settlement.has_value()))) {
    return Invalid<void>("recovery_receipt_status_shape_invalid");
  }
  if (receipt.matched_claim.has_value() &&
      (!ValidateCellAdmissionClaimIntent(receipt.matched_claim->intent).ok() ||
       !receipt.matched_claim->claim_generation.nonzero() ||
       !receipt.matched_claim->acquired_admission_generation.nonzero() ||
       receipt.matched_claim->accepted_at_ns < 0)) {
    return Invalid<void>("recovery_receipt_claim_invalid");
  }
  if (receipt.matched_release.has_value() &&
      !ValidateCellAdmissionClaimRelease(*receipt.matched_release).ok()) {
    return Invalid<void>("recovery_receipt_release_invalid");
  }
  if (receipt.matched_claim.has_value()) {
    const bool held_inventory_contains_match =
        std::find(receipt.held_claims.begin(), receipt.held_claims.end(),
                  *receipt.matched_claim) != receipt.held_claims.end();
    if ((receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kHeldExact) !=
        held_inventory_contains_match) {
      return Invalid<void>("recovery_receipt_held_inventory_mismatch");
    }
  }
  if (receipt.matched_claim.has_value() && receipt.matched_release.has_value() &&
      !ValidateCellAdmissionRecoveryReceiptQuery(
           {receipt.matched_claim->intent,
            receipt.matched_claim, receipt.matched_release})
           .ok()) {
    return Invalid<void>("recovery_receipt_release_not_exact");
  }
  if (receipt.matched_reconcile_settlement.has_value()) {
    const auto& settlement = *receipt.matched_reconcile_settlement;
    if (!receipt.matched_claim.has_value() ||
        settlement.predecessor_claim != *receipt.matched_claim ||
        settlement.successor_owner_role !=
            settlement.predecessor_claim.intent.owner_role ||
        settlement.successor_owner_epoch.empty() ||
        settlement.successor_owner_epoch ==
            settlement.predecessor_claim.intent.owner_epoch ||
        settlement.settlement_predicate !=
            CellAdmissionSettlementKindDTO::kReconciled ||
        !Digest(settlement.reconciliation_evidence_digest)) {
      return Invalid<void>("recovery_receipt_reconcile_invalid");
    }
  }
  return Result<void>::Ok();
}

Result<CellAdmissionRecoveryFootprintDTO>
ComputeCellAdmissionRecoveryFootprint(
    const RequestOwnerEpoch& kernel_owner_epoch,
    AdmissionGeneration admission_generation,
    const std::vector<CellAdmissionRecoveryLedgerEntryDTO>& entries,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current_owner_epochs) {
  if (!BoundedIdentity(kernel_owner_epoch.value()) ||
      !admission_generation.nonzero() ||
      entries.size() > kMaximumCellAdmissionClaims ||
      current_owner_epochs.size() > kMaximumCellAdmissionClaims) {
    return Invalid<CellAdmissionRecoveryFootprintDTO>(
        "recovery_footprint_input_invalid");
  }
  std::size_t held_inventory_binary = sizeof(std::uint64_t);
  for (const auto& entry : entries) {
    if (!ValidateCellAdmissionClaimIntent(entry.claim.intent).ok() ||
        !entry.claim.claim_generation.nonzero() ||
        !entry.claim.acquired_admission_generation.nonzero() ||
        entry.claim.accepted_at_ns < 0 ||
        (entry.release.has_value() &&
         entry.reconcile_settlement.has_value()) ||
        (entry.release.has_value() &&
         (!ValidateCellAdmissionClaimRelease(*entry.release).ok() ||
          entry.release->claim_generation != entry.claim.claim_generation)) ||
        (entry.reconcile_settlement.has_value() &&
         entry.reconcile_settlement->predecessor_claim != entry.claim)) {
      return Invalid<CellAdmissionRecoveryFootprintDTO>(
          "recovery_footprint_entry_invalid");
    }
    if (!entry.release.has_value() &&
        !entry.reconcile_settlement.has_value()) {
      held_inventory_binary += BinaryClaim(entry.claim);
    }
  }
  std::size_t owner_inventory_binary = sizeof(std::uint64_t);
  for (const auto& owner : current_owner_epochs) {
    if (to_string(owner.owner_role).empty() ||
        !BoundedIdentity(owner.owner_epoch.value())) {
      return Invalid<CellAdmissionRecoveryFootprintDTO>(
          "recovery_footprint_owner_invalid");
    }
    owner_inventory_binary += BinaryOwner(owner);
  }

  CellAdmissionRecoveryFootprintDTO result;
  for (const auto& entry : entries) {
    const auto claim_bytes = BinaryClaim(entry.claim);
    const auto future_release_bytes =
        4U * sizeof(std::uint64_t) +
        BinaryString(entry.claim.intent.owner_epoch.value()) +
        BinaryString(entry.claim.intent.owner_transaction_id) +
        BinaryString(entry.claim.intent.request_id.value()) +
        BinaryString(entry.claim.intent.request_payload_digest.value()) +
        BinaryString(std::string_view(
            "sha256:0000000000000000000000000000000000000000000000000000000000000000"));
    const auto future_reconcile_bytes =
        claim_bytes + 2U * sizeof(std::uint64_t) +
        sizeof(std::uint64_t) + kMaximumCellAdmissionIdentityBytes +
        BinaryString(std::string_view(
            "sha256:0000000000000000000000000000000000000000000000000000000000000000"));
    std::size_t query =
        kRecoveryCommandEnvelopeBinaryBytes + BinaryIntent(entry.claim.intent) +
        2U + claim_bytes;
    if (entry.release.has_value()) {
      query += BinaryRelease(*entry.release);
    } else if (!entry.reconcile_settlement.has_value()) {
      query += future_release_bytes;
    }

    std::size_t receipt =
        kRecoveryCommandEnvelopeBinaryBytes + 1U + sizeof(std::uint64_t) +
        BinaryString(kernel_owner_epoch.value()) + sizeof(std::uint64_t) +
        3U + claim_bytes + held_inventory_binary + owner_inventory_binary;
    if (entry.release.has_value()) {
      receipt += BinaryRelease(*entry.release);
    } else if (entry.reconcile_settlement.has_value()) {
      receipt +=
          BinaryReconcileSettlement(*entry.reconcile_settlement);
    } else {
      receipt =
          std::max(receipt, receipt - claim_bytes + future_release_bytes);
      receipt =
          std::max(receipt, receipt - claim_bytes + future_reconcile_bytes);
    }
    result.maximum_binary_request_bytes =
        std::max(result.maximum_binary_request_bytes, query);
    result.maximum_binary_response_bytes =
        std::max(result.maximum_binary_response_bytes, receipt);
    result.maximum_json_request_bytes =
        std::max(result.maximum_json_request_bytes, JsonUpperBound(query));
    result.maximum_json_response_bytes =
        std::max(result.maximum_json_response_bytes, JsonUpperBound(receipt));
  }
  return Result<CellAdmissionRecoveryFootprintDTO>::Ok(result);
}

Result<void> ValidateProgramCycleStatus(const ProgramCycleStatusDTO& status) {
  if (!BoundedIdentity(status.program_runner_owner_epoch.value()) ||
      status.cycle_generation == 0U || status.cycle_generation > kMaximumSafeCounter ||
      to_string(status.state).empty()) {
    return Invalid<void>("program_cycle_status_identity_invalid");
  }
  const bool idle = status.state == ProgramCycleStateDTO::kIdleAfterReset;
  const bool recovery =
      status.state == ProgramCycleStateDTO::kInterventionOrRecovery ||
      status.state == ProgramCycleStateDTO::kRecoveryRequired;
  const bool pending_acquire =
      (status.state == ProgramCycleStateDTO::kStarting ||
       status.state == ProgramCycleStateDTO::kStopping ||
       status.state == ProgramCycleStateDTO::kResetting || recovery ||
       (status.state == ProgramCycleStateDTO::kTerminalAwaitingReset &&
        status.terminal_or_settlement_reference.has_value())) &&
      !status.cycle_claim_generation.has_value();
  if ((idle && (status.program_run_id.has_value() ||
                status.cycle_claim_generation.has_value() ||
                status.terminal_or_settlement_reference.has_value())) ||
      (!idle &&
       ((!recovery && (!status.program_run_id.has_value() ||
                       status.program_run_id->empty())) ||
        (status.cycle_claim_generation.has_value() &&
         (!status.program_run_id.has_value() ||
          status.program_run_id->empty())) ||
        (!pending_acquire &&
         (!status.cycle_claim_generation.has_value() ||
          !status.cycle_claim_generation->nonzero() ||
          status.cycle_claim_generation->value() > kMaximumSafeCounter)))) ||
      (status.terminal_or_settlement_reference.has_value() &&
       !Digest(*status.terminal_or_settlement_reference)) ||
      (status.state == ProgramCycleStateDTO::kTerminalAwaitingReset &&
       !status.terminal_or_settlement_reference.has_value())) {
    return Invalid<void>("program_cycle_status_shape_invalid");
  }
  return Result<void>::Ok();
}

Result<void> ValidateProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization) {
  if (!BoundedIdentity(authorization.program_runner_owner_epoch.value()) ||
      !BoundedIdentity(authorization.program_run_id.value()) ||
      !authorization.cycle_claim_generation.nonzero() ||
      authorization.cycle_claim_generation.value() > kMaximumSafeCounter ||
      !Digest(authorization.terminal_or_settlement_reference)) {
    return Invalid<void>("program_reset_scene_authorization_invalid");
  }
  return Result<void>::Ok();
}

Result<ContentDigest> CellAdmissionClaimIntentDigest(const CellAdmissionClaimIntentDTO& intent) {
  auto valid = ValidateCellAdmissionClaimIntent(intent);
  if (!valid.ok()) {
    return Result<ContentDigest>::Err(valid.error());
  }
  CanonicalValue intervention(nullptr);
  if (intent.intervention_evidence.has_value()) {
    const auto& evidence = *intent.intervention_evidence;
    intervention = CanonicalValue(CanonicalValue::Object{
        {"offer_digest", CanonicalValue(evidence.offer_digest.value())},
        {"offer_transaction_id", CanonicalValue(evidence.offer_transaction_id)},
        {"offering_owner_epoch", CanonicalValue(evidence.offering_owner_epoch.value())},
        {"offering_owner_role", CanonicalValue(std::string(to_string(evidence.offering_owner_role)))},
        {"parent_program_cycle_claim_generation",
         evidence.parent_program_cycle_claim_generation.has_value()
             ? CanonicalValue(static_cast<double>(
                   evidence.parent_program_cycle_claim_generation->value()))
             : CanonicalValue(nullptr)},
    });
  }
  CanonicalValue program_cycle(nullptr);
  if (intent.program_cycle_evidence.has_value()) {
    program_cycle = CanonicalValue(CanonicalValue::Object{
        {"applied_program_revision",
         CanonicalValue(intent.program_cycle_evidence->applied_program_revision.value())},
        {"draft_program_revision",
         CanonicalValue(intent.program_cycle_evidence->draft_program_revision.value())},
    });
  }
  CanonicalValue::Object fields{
      {"claim_kind", CanonicalValue(std::string(to_string(intent.claim_kind)))},
      {"expected_admission_generation",
       CanonicalValue(static_cast<double>(intent.expected_admission_generation.value()))},
      {"intervention_evidence", std::move(intervention)},
      {"owner_epoch", CanonicalValue(intent.owner_epoch.value())},
      {"owner_role", CanonicalValue(std::string(to_string(intent.owner_role)))},
      {"owner_transaction_id", CanonicalValue(intent.owner_transaction_id)},
      {"program_cycle_evidence", std::move(program_cycle)},
      {"request_id", CanonicalValue(intent.request_id.value())},
      {"request_payload_digest", CanonicalValue(intent.request_payload_digest.value())},
      {"scope", ScopeValue(intent.scope)},
  };
  // Preserve existing claim identities on journal migration. New attribution
  // is identity-bearing only when present; it cannot be removed on replay.
  if (intent.program_node_evidence.has_value()) {
    const auto& evidence = *intent.program_node_evidence;
    fields.emplace_back("program_node_evidence",
                        CanonicalValue(CanonicalValue::Object{
                            {"program_runner_owner_epoch",
                             CanonicalValue(evidence.program_runner_owner_epoch.value())},
                            {"program_run_id", CanonicalValue(evidence.program_run_id.value())},
                            {"parent_program_cycle_claim_generation",
                             CanonicalValue(static_cast<double>(
                                 evidence.parent_program_cycle_claim_generation.value()))},
                            {"node_instance_id", CanonicalValue(evidence.node_instance_id)},
                            {"node_attempt_id", CanonicalValue(evidence.node_attempt_id)},
                            {"owner_api", CanonicalValue(evidence.owner_api)},
                        }));
  }
  const CanonicalValue value(std::move(fields));
  return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
      "cell-admission-claim-intent-digest", "1.0", value)));
}

Result<ContentDigest> CellAdmissionOwnerEpochReconciliationEvidenceDigest(
    CellAdmissionOwnerRoleDTO owner_role,
    const RequestOwnerEpoch& owner_epoch) {
  if (to_string(owner_role).empty() ||
      !BoundedIdentity(owner_epoch.value())) {
    return Invalid<ContentDigest>(
        "owner_epoch_reconciliation_identity_invalid");
  }
  const CanonicalValue value(CanonicalValue::Object{
      {"owner_epoch", CanonicalValue(owner_epoch.value())},
      {"owner_role", CanonicalValue(std::string(to_string(owner_role)))},
  });
  return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
      "cell-admission-owner-epoch-reconciliation-evidence", "1.0", value)));
}

}  // namespace manyforge::core
