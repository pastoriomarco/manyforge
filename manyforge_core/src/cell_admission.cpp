#include <algorithm>
#include <charconv>
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

// An optional transaction id: one presence byte and one number.
constexpr std::size_t kBinaryTransactionIdBytes = 1U + sizeof(std::uint64_t);

[[nodiscard]] std::size_t BinaryIntent(
    const CellAdmissionClaimIntentDTO& intent) {
  std::size_t result =
      3U * sizeof(std::uint64_t) + kBinaryTransactionIdBytes +
      sizeof(RequestKey) +
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
    result += sizeof(std::uint64_t) + kBinaryTransactionIdBytes +
              BinaryString(evidence.offer_digest.value()) + 1U;
    if (evidence.parent_program_cycle_claim_generation.has_value()) {
      result += sizeof(std::uint64_t);
    }
  }
  if (intent.program_node_evidence.has_value()) {
    const auto& evidence = *intent.program_node_evidence;
    result += 2U * sizeof(std::uint64_t) +
              BinaryString(evidence.node_instance_id) + sizeof(std::uint64_t) +
              BinaryString(evidence.owner_api);
  }
  return result;
}

[[nodiscard]] std::size_t BinaryClaim(const CellAdmissionClaimDTO& claim) {
  return BinaryIntent(claim.intent) + 3U * sizeof(std::uint64_t);
}

[[nodiscard]] std::size_t BinaryRelease(
    const CellAdmissionClaimReleaseDTO& release) {
  return 4U * sizeof(std::uint64_t) + kBinaryTransactionIdBytes +
         sizeof(RequestKey) +
         BinaryString(release.settlement.evidence_digest.value());
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
                                    ApplicationRole role) noexcept {
  switch (kind) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
    case CellAdmissionClaimKindDTO::kProgramReset:
      return role == ApplicationRole::kProgramRunner;
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
      return role == ApplicationRole::kAuthoringOwner;
    case CellAdmissionClaimKindDTO::kManagerApply:
      return role == ApplicationRole::kManager;
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return role == ApplicationRole::kAuthoredSceneOwner;
    case CellAdmissionClaimKindDTO::kManualOperation:
      return role == ApplicationRole::kManualOperationOwner;
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
      return role == ApplicationRole::kInterventionOrRecoveryOwner;
  }
  return false;
}

// A present id has the type of `role` and a nonzero number within the JSON bound.
[[nodiscard]] bool TransactionIdFits(
    ApplicationRole role, const std::optional<CellAdmissionOwnerTransactionId>& id) noexcept {
  if (!id) return true;
  const auto number = CellAdmissionOwnerTransactionNumber(*id);
  const auto expected = CellAdmissionOwnerTransactionIdFor(role, number);
  return expected && expected->index() == id->index() && number != 0U &&
         number <= kMaximumSafeCounter;
}

// The claim kinds whose owner has its own transaction number.
[[nodiscard]] bool KindCarriesTransactionId(CellAdmissionClaimKindDTO kind) noexcept {
  return kind == CellAdmissionClaimKindDTO::kProgramCycle ||
         kind == CellAdmissionClaimKindDTO::kManagerApply ||
         kind == CellAdmissionClaimKindDTO::kManualOperation;
}

[[nodiscard]] CanonicalValue TransactionIdValue(
    const std::optional<CellAdmissionOwnerTransactionId>& id) {
  return id ? CanonicalValue(static_cast<double>(CellAdmissionOwnerTransactionNumber(*id)))
            : CanonicalValue(nullptr);
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
    case CellAdmissionClaimKindDTO::kProgramReset:
      return "program_reset";
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
      return "authoring_mutation";
    case CellAdmissionClaimKindDTO::kManagerApply:
      return "manager_apply";
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return "authored_scene";
    case CellAdmissionClaimKindDTO::kManualOperation:
      return "manual_operation";
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
      return "intervention_or_recovery";
  }
  return {};
}

bool IsCellAdmissionOwnerRole(const ApplicationRole role) noexcept {
  switch (role) {
    case ApplicationRole::kProgramRunner:
    case ApplicationRole::kAuthoringOwner:
    case ApplicationRole::kManager:
    case ApplicationRole::kAuthoredSceneOwner:
    case ApplicationRole::kManualOperationOwner:
    case ApplicationRole::kInterventionOrRecoveryOwner:
      return true;
    default:
      return false;
  }
}

std::optional<CellAdmissionOwnerTransactionId> CellAdmissionOwnerTransactionIdFor(
    const ApplicationRole role, const std::uint64_t value) noexcept {
  switch (role) {
    case ApplicationRole::kProgramRunner:
      return CellAdmissionOwnerTransactionId(ProgramRunNumber(value));
    case ApplicationRole::kManager:
      return CellAdmissionOwnerTransactionId(ApplyId(value));
    case ApplicationRole::kManualOperationOwner:
      return CellAdmissionOwnerTransactionId(OperationNumber(value));
    default:
      return std::nullopt;
  }
}

std::uint64_t CellAdmissionOwnerTransactionNumber(
    const CellAdmissionOwnerTransactionId& value) noexcept {
  return std::visit([](const auto& number) { return number.value(); }, value);
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
  return program_run_id.nonzero() && program_run_id.value() <= kMaximumSafeCounter &&
         parent_program_cycle_claim_generation.nonzero() &&
         parent_program_cycle_claim_generation.value() <= kMaximumSafeCounter &&
         BoundedIdentity(node_instance_id) && node_attempt_id.nonzero() &&
         node_attempt_id.value() <= kMaximumSafeCounter &&
         node_instance_id.find('\0') == std::string::npos &&
         (owner_api == "scene.transaction" || owner_api == "scene.carried_attachment");
}

Result<void> ValidateCellAdmissionClaimIntent(const CellAdmissionClaimIntentDTO& intent) {
  if (to_string(intent.claim_kind).empty() ||
      !OwnerMatchesKind(intent.claim_kind, intent.owner_role) ||
      KindCarriesTransactionId(intent.claim_kind) != intent.owner_transaction_id.has_value() ||
      !TransactionIdFits(intent.owner_role, intent.owner_transaction_id) ||
      !intent.expected_admission_generation.nonzero() ||
      intent.expected_admission_generation.value() > kMaximumSafeCounter) {
    return Invalid<void>("claim_identity_invalid");
  }
  auto scope = ValidateCellAdmissionScope(intent.scope);
  if (!scope.ok()) {
    return scope;
  }
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset &&
      (!intent.scope.resources.empty() || !intent.scope.configurations.empty() ||
       intent.program_cycle_evidence || intent.intervention_evidence ||
       intent.program_node_evidence)) {
    return Invalid<void>("program_reset_claim_shape_invalid");
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
    if (!IsCellAdmissionOwnerRole(evidence.offering_owner_role) ||
        !TransactionIdFits(evidence.offering_owner_role, evidence.offer_transaction_id) ||
        !Digest(evidence.offer_digest) ||
        (evidence.parent_program_cycle_claim_generation.has_value() &&
         !evidence.parent_program_cycle_claim_generation->nonzero())) {
      return Invalid<void>("intervention_evidence_invalid");
    }
    if (held_manual &&
        (evidence.offering_owner_role != ApplicationRole::kManualOperationOwner ||
         evidence.offer_transaction_id != intent.owner_transaction_id ||
         !evidence.parent_program_cycle_claim_generation || intent.scope.resources.empty())) {
      return Invalid<void>("manual_handover_evidence_invalid");
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
  if (!IsCellAdmissionOwnerRole(release.owner_role) ||
      !TransactionIdFits(release.owner_role, release.owner_transaction_id) ||
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
        !claim.acquired_admission_generation.nonzero() || claim.accepted_at_ns < 0) {
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
        claim.intent.owner_transaction_id != release.owner_transaction_id ||
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
  if (to_string(receipt.status).empty() ||
      !receipt.admission_generation.nonzero() ||
      receipt.held_claims.size() > kMaximumCellAdmissionClaims ||
      !std::is_sorted(receipt.held_claims.begin(), receipt.held_claims.end(),
                      [](const auto& left, const auto& right) {
                        return left.claim_generation < right.claim_generation;
                      }) ||
      std::adjacent_find(receipt.held_claims.begin(), receipt.held_claims.end(),
                         [](const auto& left, const auto& right) {
                           return left.claim_generation == right.claim_generation;
                         }) != receipt.held_claims.end()) {
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
  const bool exact =
      receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact;
  if (exact != receipt.matched_claim.has_value() ||
      (receipt.status == CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact) !=
          receipt.matched_release.has_value() ||
      (!exact && receipt.matched_release.has_value())) {
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
  return Result<void>::Ok();
}

Result<CellAdmissionRecoveryFootprintDTO>
ComputeCellAdmissionRecoveryFootprint(
    AdmissionGeneration admission_generation,
    const std::vector<CellAdmissionRecoveryLedgerEntryDTO>& entries) {
  if (!admission_generation.nonzero() ||
      entries.size() > kMaximumCellAdmissionClaims) {
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
         (!ValidateCellAdmissionClaimRelease(*entry.release).ok() ||
          entry.release->claim_generation != entry.claim.claim_generation))) {
      return Invalid<CellAdmissionRecoveryFootprintDTO>(
          "recovery_footprint_entry_invalid");
    }
    if (!entry.release.has_value()) {
      held_inventory_binary += BinaryClaim(entry.claim);
    }
  }

  CellAdmissionRecoveryFootprintDTO result;
  for (const auto& entry : entries) {
    const auto claim_bytes = BinaryClaim(entry.claim);
    const auto future_release_bytes =
        4U * sizeof(std::uint64_t) + kBinaryTransactionIdBytes +
        sizeof(RequestKey) +
        BinaryString(std::string_view(
            "sha256:0000000000000000000000000000000000000000000000000000000000000000"));
    std::size_t query =
        kRecoveryCommandEnvelopeBinaryBytes + BinaryIntent(entry.claim.intent) +
        2U + claim_bytes;
    if (entry.release.has_value()) {
      query += BinaryRelease(*entry.release);
    } else {
      query += future_release_bytes;
    }

    std::size_t receipt =
        kRecoveryCommandEnvelopeBinaryBytes + 1U + sizeof(std::uint64_t) +
        2U + claim_bytes + held_inventory_binary;
    if (entry.release.has_value()) {
      receipt += BinaryRelease(*entry.release);
    } else {
      receipt =
          std::max(receipt, receipt - claim_bytes + future_release_bytes);
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

Result<void> ValidateProgramRunnerContactLoss(const ProgramRunnerContactLossDTO& loss) {
  const auto& cycle = loss.established_cycle;
  const bool active = cycle.state == ProgramCycleStateDTO::kStarting ||
                      cycle.state == ProgramCycleStateDTO::kRunning ||
                      cycle.state == ProgramCycleStateDTO::kStopping;
  if (!ValidateProgramCycleStatus(cycle).ok() || !active || !cycle.program_run_id ||
      !cycle.cycle_claim_generation || cycle.accepted_reset ||
      cycle.terminal_or_settlement_reference ||
      (loss.failure_predicate != "program_runner_liveness_probe" &&
       loss.failure_predicate != "program_runner_liveness_renewal" &&
       loss.failure_predicate != "program_runner_reconciliation_renewal" &&
       loss.failure_predicate != "program_runner_fault") ||
      !loss.fault_code || loss.fault_code->empty() || loss.fault_code->size() > 128U ||
      // A Runner fault carries its own code; a lost contact carries
      // program_runner_contact_lost (OD-296 (a)).
      ((loss.failure_predicate == "program_runner_fault") ==
       (*loss.fault_code == kProgramRunnerContactLostCode)) ||
      loss.message.size() > 1024U || !content_identity::is_utf8_nfc(loss.message)) {
    return Invalid<void>("program_runner_contact_loss_invalid");
  }
  return Result<void>::Ok();
}

Result<void> ValidateProgramCycleStatus(const ProgramCycleStatusDTO& status) {
  if (status.cycle_generation == 0U || status.cycle_generation > kMaximumSafeCounter ||
      to_string(status.state).empty()) {
    return Invalid<void>("program_cycle_status_identity_invalid");
  }
  const bool idle = status.state == ProgramCycleStateDTO::kIdleAfterReset;
  const bool recovery =
      status.state == ProgramCycleStateDTO::kInterventionOrRecovery ||
      status.state == ProgramCycleStateDTO::kRecoveryRequired;
  if (status.accepted_reset) {
    const auto& reset = *status.accepted_reset;
    if ((status.state != ProgramCycleStateDTO::kResetting && !recovery) ||
        !Digest(reset.request_payload_digest) ||
        (reset.reset_claim_generation &&
         (!reset.reset_claim_generation->nonzero() ||
          reset.reset_claim_generation->value() > kMaximumSafeCounter ||
          status.cycle_claim_generation.has_value())) ||
        (!status.program_run_id &&
         ((!reset.reload_initial_scene && !reset.reset_robot_position) ||
          status.cycle_claim_generation || status.terminal_or_settlement_reference))) {
      return Invalid<void>("program_cycle_accepted_reset_invalid");
    }
  }
  const bool reset_claim = status.state == ProgramCycleStateDTO::kResetting &&
                          !status.program_run_id && status.accepted_reset;
  // The start window: the run number is announced only after the Kernel
  // claim (K-6; contract 1.15.5), so `starting` may name no run and no claim.
  const bool unannounced_start = status.state == ProgramCycleStateDTO::kStarting &&
                                 !status.program_run_id && !status.cycle_claim_generation;
  const bool pending_acquire =
      (status.state == ProgramCycleStateDTO::kStarting ||
       status.state == ProgramCycleStateDTO::kStopping ||
       status.state == ProgramCycleStateDTO::kResetting || recovery ||
       (status.state == ProgramCycleStateDTO::kTerminalAwaitingReset &&
        status.terminal_or_settlement_reference.has_value())) &&
      !status.cycle_claim_generation.has_value();
  if ((status.program_run_id && (!status.program_run_id->nonzero() ||
                                 status.program_run_id->value() > kMaximumSafeCounter)) ||
      (idle && (status.program_run_id.has_value() ||
                status.cycle_claim_generation.has_value() ||
                status.terminal_or_settlement_reference.has_value())) ||
      (!idle &&
       ((!recovery && !reset_claim && !unannounced_start && !status.program_run_id.has_value()) ||
        (status.cycle_claim_generation.has_value() && !status.program_run_id.has_value()) ||
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

Result<RequestKey> ProgramResetClaimRequestKey(const ProgramCycleStatusDTO& status) {
  if (!ValidateProgramCycleStatus(status).ok() || !status.accepted_reset ||
      status.cycle_claim_generation) {
    return Invalid<RequestKey>("program_reset_claim_context_invalid");
  }
  // The Reset's claim is the Reset request's part 2 (contract 1.4, 1.15.2).
  return Result<RequestKey>::Ok(status.accepted_reset->request_key.WithPart(2U));
}

Result<void> ValidateProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization) {
  if (authorization.reset_claim) {
    const auto& reset = *authorization.reset_claim;
    if (authorization.program_run_id.nonzero() || authorization.cycle_claim_generation.nonzero() ||
        !authorization.terminal_or_settlement_reference.empty() ||
        reset.cycle_generation == 0U || reset.cycle_generation > kMaximumSafeCounter ||
        !Digest(reset.request_payload_digest) ||
        !reset.reset_claim_generation.nonzero() ||
        reset.reset_claim_generation.value() > kMaximumSafeCounter) {
      return Invalid<void>("program_reset_scene_authorization_invalid");
    }
    return Result<void>::Ok();
  }
  if (!authorization.program_run_id.nonzero() ||
      authorization.program_run_id.value() > kMaximumSafeCounter ||
      !authorization.cycle_claim_generation.nonzero() ||
      authorization.cycle_claim_generation.value() > kMaximumSafeCounter ||
      !Digest(authorization.terminal_or_settlement_reference)) {
    return Invalid<void>("program_reset_scene_authorization_invalid");
  }
  return Result<void>::Ok();
}

Result<void> ValidateProgramResetPositionAuthorization(
    const ProgramResetPositionAuthorizationDTO& authorization) {
  const auto counter = [](const auto& value) {
    return value && value->nonzero() && value->value() <= kMaximumSafeCounter;
  };
  if (authorization.cycle_generation == 0U || authorization.cycle_generation > kMaximumSafeCounter ||
      !Digest(authorization.request_payload_digest) ||
      (authorization.reset_claim_generation
          ? (!counter(authorization.reset_claim_generation) || authorization.cycle_claim_generation ||
             (authorization.program_run_id
                 ? (!counter(authorization.program_run_id) ||
                    !authorization.terminal_or_settlement_reference ||
                    !Digest(*authorization.terminal_or_settlement_reference))
                 : authorization.terminal_or_settlement_reference.has_value()))
          : (!counter(authorization.program_run_id) ||
             !counter(authorization.cycle_claim_generation) ||
             !authorization.terminal_or_settlement_reference ||
             !Digest(*authorization.terminal_or_settlement_reference)))) {
    return Invalid<void>("program_reset_position_authorization_invalid");
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
        {"offer_transaction_id", TransactionIdValue(evidence.offer_transaction_id)},
        {"offering_owner_role",
         CanonicalValue(std::string(ApplicationRoleText(evidence.offering_owner_role)))},
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
      {"owner_role", CanonicalValue(std::string(ApplicationRoleText(intent.owner_role)))},
      {"owner_transaction_id", TransactionIdValue(intent.owner_transaction_id)},
      {"program_cycle_evidence", std::move(program_cycle)},
      {"request_key", CanonicalValue(RequestKeyText(intent.request_key))},
      {"scope", ScopeValue(intent.scope)},
  };
  // Preserve existing claim identities on journal migration. New attribution
  // is identity-bearing only when present; it cannot be removed on replay.
  if (intent.program_node_evidence.has_value()) {
    const auto& evidence = *intent.program_node_evidence;
    fields.emplace_back("program_node_evidence",
                        CanonicalValue(CanonicalValue::Object{
                            {"program_run_id", CanonicalValue(static_cast<double>(
                                                   evidence.program_run_id.value()))},
                            {"parent_program_cycle_claim_generation",
                             CanonicalValue(static_cast<double>(
                                 evidence.parent_program_cycle_claim_generation.value()))},
                            {"node_instance_id", CanonicalValue(evidence.node_instance_id)},
                            {"node_attempt_id",
                             CanonicalValue(static_cast<double>(evidence.node_attempt_id.value()))},
                            {"owner_api", CanonicalValue(evidence.owner_api)},
                        }));
  }
  const CanonicalValue value(std::move(fields));
  return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
      "cell-admission-claim-intent-digest", "2.0", value)));
}

}  // namespace manyforge::core
