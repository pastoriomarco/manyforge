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
      return role == CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner;
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

Result<void> ValidateCellAdmissionScope(const CellAdmissionScopeDTO& scope) {
  if (scope.resources.size() > kMaximumCellAdmissionScopeEntries ||
      scope.configurations.size() > kMaximumCellAdmissionScopeEntries ||
      !std::is_sorted(scope.resources.begin(), scope.resources.end()) ||
      std::adjacent_find(scope.resources.begin(), scope.resources.end()) != scope.resources.end() ||
      std::any_of(scope.resources.begin(), scope.resources.end(),
                  [](const auto& value) { return value.empty(); })) {
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
        return row.control_domain_id.empty() || row.active_configuration_id.empty() ||
               !row.active_configuration_generation.nonzero() ||
               row.active_configuration_generation.value() > kMaximumSafeCounter;
      })) {
    return Invalid<void>("configuration_scope_not_canonical");
  }
  return Result<void>::Ok();
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
  const bool intervention =
      intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery;
  if (intervention != intent.intervention_evidence.has_value()) {
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

Result<void> ValidateProgramCycleStatus(const ProgramCycleStatusDTO& status) {
  if (!BoundedIdentity(status.program_runner_owner_epoch.value()) ||
      status.cycle_generation == 0U || status.cycle_generation > kMaximumSafeCounter ||
      to_string(status.state).empty()) {
    return Invalid<void>("program_cycle_status_identity_invalid");
  }
  const bool idle = status.state == ProgramCycleStateDTO::kIdleAfterReset;
  if (idle != (!status.program_run_id.has_value() &&
               !status.cycle_claim_generation.has_value() &&
               !status.terminal_or_settlement_reference.has_value()) ||
      (!idle &&
       (!status.program_run_id.has_value() || status.program_run_id->empty() ||
        !status.cycle_claim_generation.has_value() ||
        !status.cycle_claim_generation->nonzero() ||
        status.cycle_claim_generation->value() > kMaximumSafeCounter)) ||
      (status.terminal_or_settlement_reference.has_value() &&
       !Digest(*status.terminal_or_settlement_reference)) ||
      (status.state == ProgramCycleStateDTO::kTerminalAwaitingReset &&
       !status.terminal_or_settlement_reference.has_value())) {
    return Invalid<void>("program_cycle_status_shape_invalid");
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
  const CanonicalValue value(CanonicalValue::Object{
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
  });
  return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
      "cell-admission-claim-intent-digest", "1.0", value)));
}

}  // namespace manyforge::core
