#include <algorithm>
#include <cstdio>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Value>
[[nodiscard]] Result<Value> Reject(std::string code, std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{std::move(code), "cell-admission request rejected", {{"reason", reason}}});
}

template <typename Value>
[[nodiscard]] Result<Value> AuthoringNotReady(AdmissionGeneration generation, std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{"authoring_not_ready",
               "cell-authoring admission is not ready",
               {{"admission_generation", std::to_string(generation.value())},
                {"retryable", "true"},
                {"required_action", "inspect_owner_status_and_restore_authoring_readiness"},
                {"reason", std::move(reason)}}});
}

[[nodiscard]] bool SameRequest(const CellAdmissionClaimIntentDTO& left,
                               const CellAdmissionClaimIntentDTO& right) {
  return left.owner_epoch == right.owner_epoch && left.request_id == right.request_id;
}

[[nodiscard]] bool SameReleaseIdentity(const CellAdmissionClaimDTO& claim,
                                       const CellAdmissionClaimReleaseDTO& release) {
  return claim.intent.owner_role == release.owner_role &&
         claim.intent.owner_epoch == release.owner_epoch &&
         claim.intent.owner_transaction_id == release.owner_transaction_id &&
         claim.intent.request_id == release.request_id &&
         claim.intent.request_payload_digest == release.request_payload_digest &&
         claim.acquired_admission_generation == release.acquired_admission_generation &&
         claim.claim_generation == release.claim_generation;
}

[[nodiscard]] bool SettlementKindCompatible(CellAdmissionClaimKindDTO claim,
                                            CellAdmissionSettlementKindDTO settlement) noexcept {
  switch (claim) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
    case CellAdmissionClaimKindDTO::kProgramReset:
      return settlement == CellAdmissionSettlementKindDTO::kExplicitReset;
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return settlement == CellAdmissionSettlementKindDTO::kOwnerCommit ||
             settlement == CellAdmissionSettlementKindDTO::kProvenNoEffect ||
             settlement == CellAdmissionSettlementKindDTO::kReconciled;
    case CellAdmissionClaimKindDTO::kManagerApply:
    case CellAdmissionClaimKindDTO::kComponentLifecycle:
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
    case CellAdmissionClaimKindDTO::kManualOperation:
      return settlement == CellAdmissionSettlementKindDTO::kTerminalEffectsSettled ||
             settlement == CellAdmissionSettlementKindDTO::kProvenNoEffect ||
             settlement == CellAdmissionSettlementKindDTO::kReconciled;
  }
  return false;
}

}  // namespace

KernelCellAdmissionOwner::KernelCellAdmissionOwner(
    RequestOwnerEpoch kernel_owner_epoch, std::size_t capacity, IClock& clock,
    IOperationJournal& journal,
    std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO> recovery,
    AdmissionGeneration recovered_generation_high_water,
    CellAdmissionInterventionValidator intervention_validator,
    CellAdmissionSettlementValidator settlement_validator,
    CellAdmissionReconcileValidator reconcile_validator,
    std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionMembershipDTO>
        recovered_membership,
    std::vector<CellAdmissionCurrentOwnerEpochDTO> recovered_current_owners,
    std::optional<ManagerLivenessRequiredSetDTO> manager_liveness_required_set,
    CellAdmissionClaimGeneration recovered_claim_generation_high_water,
    CellAdmissionProgramNodeValidator program_node_validator,
    std::function<bool()> stop_fault_active,
    CellAdmissionProgramResetStatusSource program_reset_status_source,
    std::function<Result<void>(std::shared_ptr<IIoSignalService>*)> required_health_loss_observer,
    std::function<StopFaultAdmissionScope()> stop_fault_scope)
    : kernel_owner_epoch_(std::move(kernel_owner_epoch)),
      capacity_(capacity),
      clock_(clock),
      journal_(journal),
      intervention_validator_(std::move(intervention_validator)),
      settlement_validator_(std::move(settlement_validator)),
      program_reset_status_source_(std::move(program_reset_status_source)),
      reconcile_validator_(std::move(reconcile_validator)),
      program_node_validator_(std::move(program_node_validator)),
      stop_fault_active_(std::move(stop_fault_active)),
      stop_fault_scope_(std::move(stop_fault_scope)),
      required_health_loss_observer_(std::move(required_health_loss_observer)),
      manager_liveness_required_set_(std::move(manager_liveness_required_set)),
      membership_(std::move(recovered_membership)),
      current_owners_(std::move(recovered_current_owners)) {
  if (kernel_owner_epoch_.empty() || capacity_ == 0U || capacity_ > kMaximumCellAdmissionClaims ||
      recovery.size() > capacity_ || membership_.size() > capacity_ ||
      current_owners_.size() > kMaximumCellAdmissionClaims) {
    throw std::invalid_argument("Kernel cell-admission owner configuration is invalid");
  }
  const auto owners_canonical =
      std::is_sorted(
          current_owners_.begin(), current_owners_.end(),
          [](const auto& left, const auto& right) { return left.owner_role < right.owner_role; }) &&
      std::adjacent_find(current_owners_.begin(), current_owners_.end(),
                         [](const auto& left, const auto& right) {
                           return left.owner_role == right.owner_role;
                         }) == current_owners_.end() &&
      std::none_of(current_owners_.begin(), current_owners_.end(),
                   [](const auto& owner) { return owner.owner_epoch.empty(); });
  if (!owners_canonical) {
    throw std::runtime_error("durable cell-admission current-owner truth is invalid");
  }
  if (manager_liveness_required_set_.has_value()) {
    if (!ValidateManagerLivenessRequiredSet(*manager_liveness_required_set_).ok()) {
      throw std::invalid_argument("Kernel Manager-liveness required set is invalid");
    }
    manager_liveness_rows_.reserve(manager_liveness_required_set_->required_receivers.size());
    for (const auto& receiver : manager_liveness_required_set_->required_receivers) {
      manager_liveness_rows_.push_back({receiver, 0U, false, false, false, 0U, false});
    }
  }
  for (std::size_t index = 0; index < membership_.size(); ++index) {
    const auto& member = membership_[index];
    if (member.owner_epoch.empty() || member.request_id.empty() ||
        !content_identity::is_sha256_digest(member.semantic_intent_digest.value()) ||
        std::any_of(membership_.begin(), membership_.begin() + index, [&](const auto& prior) {
          return prior.owner_epoch == member.owner_epoch && prior.request_id == member.request_id;
        })) {
      throw std::runtime_error("durable cell-admission membership is invalid");
    }
  }
  generation_ =
      AdmissionGeneration(std::max<std::uint64_t>(1U, recovered_generation_high_water.value()));
  if (recovered_claim_generation_high_water.value() >= 9'007'199'254'740'991ULL) {
    throw std::runtime_error("durable cell-admission claim generation high-water is invalid");
  }
  next_claim_generation_ = CellAdmissionClaimGeneration(
      std::max<std::uint64_t>(1U, recovered_claim_generation_high_water.value() + 1U));
  entries_.reserve(capacity_);
  membership_.reserve(capacity_);
  current_owners_.reserve(7U);
  reconciliations_.reserve(capacity_);
  bool held_program_cycle = false;
  for (auto& recovered : recovery) {
    if (!ValidateCellAdmissionClaimIntent(recovered.claim.intent).ok() ||
        !recovered.claim.claim_generation.nonzero() ||
        !recovered.claim.acquired_admission_generation.nonzero() ||
        recovered.claim.claim_generation.value() > 9'007'199'254'740'991ULL ||
        recovered.claim.acquired_admission_generation.value() > 9'007'199'254'740'989ULL ||
        recovered.claim.accepted_at_ns < 0 ||
        (recovered.release.has_value() &&
         (!ValidateCellAdmissionClaimRelease(*recovered.release).ok() ||
          !SameReleaseIdentity(recovered.claim, *recovered.release)))) {
      throw std::runtime_error("durable cell-admission recovery is invalid");
    }
    generation_ = AdmissionGeneration(
        std::max(generation_.value(), recovered.claim.acquired_admission_generation.value() +
                                          (recovered.release.has_value() ? 2U : 1U)));
    next_claim_generation_ = CellAdmissionClaimGeneration(
        std::max(next_claim_generation_.value(), recovered.claim.claim_generation.value() + 1U));
    if (recovered.release.has_value() && recovered.reconcile_settlement.has_value()) {
      throw std::runtime_error("durable cell-admission recovery is invalid");
    }
    if (recovered.reconcile_settlement.has_value() &&
        (recovered.reconcile_settlement->predecessor_claim != recovered.claim ||
         recovered.reconcile_settlement->successor_owner_role !=
             recovered.claim.intent.owner_role ||
         recovered.reconcile_settlement->successor_owner_epoch.empty() ||
         recovered.reconcile_settlement->successor_owner_epoch ==
             recovered.claim.intent.owner_epoch ||
         recovered.reconcile_settlement->settlement_predicate !=
             CellAdmissionSettlementKindDTO::kReconciled ||
         !content_identity::is_sha256_digest(
             recovered.reconcile_settlement->reconciliation_evidence_digest.value()))) {
      throw std::runtime_error("durable cell-admission recovery is invalid");
    }
    entries_.push_back(Entry{std::move(recovered.claim), std::move(recovered.release),
                             std::move(recovered.reconcile_settlement)});
    if (!entries_.back().release.has_value() && !entries_.back().reconcile_settlement.has_value()) {
      if (entries_.back().claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle) {
        if (held_program_cycle) throw std::runtime_error("multiple held Program-cycle claims");
        held_program_cycle = true;
      }
      ++held_claim_count_;
    }
  }
  for (const auto& entry : entries_) {
    const auto digest = CellAdmissionClaimIntentDigest(entry.claim.intent);
    if (!digest.ok()) {
      throw std::runtime_error("durable cell-admission membership is invalid");
    }
    const auto found =
        std::find_if(membership_.begin(), membership_.end(), [&](const auto& member) {
          return member.owner_epoch == entry.claim.intent.owner_epoch &&
                 member.request_id == entry.claim.intent.request_id;
        });
    if (found == membership_.end()) {
      membership_.push_back({entry.claim.intent.owner_role, entry.claim.intent.owner_epoch,
                             entry.claim.intent.request_id, digest.value()});
    } else if (found->owner_role != entry.claim.intent.owner_role ||
               found->semantic_intent_digest != digest.value()) {
      throw std::runtime_error("durable cell-admission membership conflicts with claim truth");
    }
  }
  if (membership_.size() > capacity_) {
    throw std::runtime_error("durable cell-admission membership exceeds its bound");
  }
  std::vector<CellAdmissionRecoveryLedgerEntryDTO> footprint_entries;
  footprint_entries.reserve(entries_.size());
  for (const auto& entry : entries_) {
    footprint_entries.push_back({entry.claim, entry.release, entry.reconcile_settlement});
  }
  const auto footprint = ComputeCellAdmissionRecoveryFootprint(kernel_owner_epoch_, generation_,
                                                               footprint_entries, current_owners_);
  if (!footprint.ok() || !footprint.value().admitted()) {
    throw std::runtime_error("durable cell-admission recovery footprint exceeds its bound");
  }
}

Result<KernelCellAdmissionOwner::ProgramBlackboardAdmissionGuard>
KernelCellAdmissionOwner::AcquireProgramBlackboardAdmission() const {
  std::unique_lock lock(mutex_);
  if (persistence_outcome_unknown_ || !manager_liveness_required_set_ ||
      manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size() ||
      !readiness_.program_cycle_status || readiness_.program_runner_loss)
    return Result<ProgramBlackboardAdmissionGuard>::Err(
        {"stale_generation",
         "Program blackboard requires current Runner health and cycle",
         {{"reason", "program_bb_owner_not_current"}}});
  // Healthy Manager and Runner, but the Manager closed cycle readiness for a
  // transition (it does so before every Run end and Reset step). Distinct
  // reason: a BB frame is retryable not-ready in this window, not a fault.
  if (!readiness_.program_cycle_status_current)
    return Result<ProgramBlackboardAdmissionGuard>::Err(
        {"stale_generation",
         "Program blackboard requires current Runner health and cycle",
         {{"reason", "program_bb_cycle_status_closed"}}});
  return Result<ProgramBlackboardAdmissionGuard>::Ok(
      ProgramBlackboardAdmissionGuard(std::move(lock), *readiness_.program_cycle_status,
                                      manager_liveness_required_set_->active_bundle_id));
}

Result<KernelCellAdmissionOwner::ManagedProductAdmissionGuard>
KernelCellAdmissionOwner::AcquireManagedProductAdmission() const {
  std::unique_lock lock(mutex_);
  if (persistence_outcome_unknown_ || !manager_liveness_required_set_ ||
      manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size())
    return Result<ManagedProductAdmissionGuard>::Err(
        {"stale_generation", "Global interrupt rules require the current managed product owner",
         {{"reason", "global_interrupt_owner_not_current"}}});
  return Result<ManagedProductAdmissionGuard>::Ok(
      ManagedProductAdmissionGuard(std::move(lock), manager_liveness_required_set_->active_bundle_id));
}

bool KernelCellAdmissionOwner::NonCycleReadinessLocked() const noexcept {
  const bool manager_liveness_current =
      manager_liveness_required_set_.has_value() &&
      manager_liveness_current_unfenced_count_ == manager_liveness_rows_.size();
  return manager_liveness_current && readiness_.required_component_status_current &&
         readiness_.required_component_total ==
             readiness_.required_component_current_compatible_ready_converged &&
         readiness_.deployment_status_current && readiness_.deployment_converged &&
         readiness_.blocking_fault_count == 0U && readiness_.unresolved_owner_count == 0U &&
         readiness_.unsettled_effect_count == 0U;
}

bool KernelCellAdmissionOwner::BaseReadinessLocked() const noexcept {
  return NonCycleReadinessLocked() && readiness_.program_cycle_status_current &&
         readiness_.program_cycle_status.has_value() &&
         readiness_.program_cycle_status->state == ProgramCycleStateDTO::kIdleAfterReset;
}

std::optional<std::string_view> KernelCellAdmissionOwner::LifecycleBlockingReasonLocked()
    const noexcept {
  // An intentionally disabled required component is not execution-ready, but
  // must still be eligible for an explicit idle-time Enable. All other cell
  // freshness, convergence and fault requirements remain mandatory. The first
  // failing condition is reported so the caller knows what must settle.
  if (!manager_liveness_required_set_.has_value() ||
      manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size()) {
    return "component_lifecycle_manager_liveness_not_current";
  }
  if (!readiness_.required_component_status_current || !readiness_.deployment_status_current) {
    return "component_lifecycle_status_not_current";
  }
  if (!readiness_.deployment_converged) return "component_lifecycle_deployment_not_converged";
  if (readiness_.blocking_fault_count != 0U) return "component_lifecycle_blocking_fault";
  if (readiness_.unresolved_owner_count != 0U) return "component_lifecycle_owner_unresolved";
  if (readiness_.unsettled_effect_count != 0U) return "component_lifecycle_effects_not_settled";
  if (!readiness_.program_cycle_status_current || !readiness_.program_cycle_status.has_value()) {
    return "component_lifecycle_program_status_not_current";
  }
  if (readiness_.program_cycle_status->state != ProgramCycleStateDTO::kIdleAfterReset) {
    return "component_lifecycle_program_not_idle_after_reset";
  }
  if (held_claim_count_ != 0U) return "component_lifecycle_conflicting_claim_held";
  return std::nullopt;
}

bool KernelCellAdmissionOwner::ManualParentReadyLocked(const Entry* parent) const noexcept {
  const auto& cycle = readiness_.program_cycle_status;
  return parent && NonCycleReadinessLocked() && readiness_.program_cycle_status_current && cycle &&
         cycle->state == ProgramCycleStateDTO::kRunning &&
         cycle->program_runner_owner_epoch == parent->claim.intent.owner_epoch &&
         cycle->program_run_id &&
         cycle->program_run_id->value() == parent->claim.intent.owner_transaction_id &&
         cycle->cycle_claim_generation == parent->claim.claim_generation;
}

Result<void> KernelCellAdmissionOwner::CheckManualCellAdmissionReadiness(
    const std::optional<CellAdmissionClaimGeneration> parent) const {
  std::lock_guard lock(mutex_);
  if (persistence_outcome_unknown_) return Result<void>::Err(*persistence_outcome_unknown_);
  if (!parent) {
    return AdmissionLocked().authoring_allowed
               ? Result<void>::Ok()
               : AuthoringNotReady<void>(generation_, "positive_admission_required");
  }
  const auto held = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return entry.claim.claim_generation == *parent && !entry.release &&
           !entry.reconcile_settlement &&
           entry.claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle;
  });
  if (held == entries_.end() || !ManualParentReadyLocked(&*held))
    return AuthoringNotReady<void>(generation_, "manual_handover_parent_not_current");
  if (held_claim_count_ != 1U)
    return AuthoringNotReady<void>(generation_, "manual_handover_claim_conflict");
  return Result<void>::Ok();
}

bool KernelCellAdmissionOwner::MatchingStartingProgramCycleLocked(
    const CellAdmissionClaimIntentDTO& intent) const noexcept {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kProgramCycle ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kProgramRunner || held_claim_count_ != 0U ||
      !NonCycleReadinessLocked() || !readiness_.program_cycle_status_current ||
      !readiness_.program_cycle_status.has_value()) {
    return false;
  }
  const auto& cycle = *readiness_.program_cycle_status;
  return cycle.state == ProgramCycleStateDTO::kStarting &&
         cycle.program_runner_owner_epoch == intent.owner_epoch &&
         cycle.program_run_id.has_value() &&
         cycle.program_run_id->value() == intent.owner_transaction_id &&
         !cycle.cycle_claim_generation.has_value() &&
         !cycle.terminal_or_settlement_reference.has_value();
}

Result<KernelCellAdmissionOwner::PreparedProgramResetStatus>
KernelCellAdmissionOwner::PrepareProgramResetStatus(
    const ProgramResetJournalReadPurpose purpose, const CellAdmissionClaimIntentDTO& intent,
    const std::optional<CellAdmissionClaimDTO>& claim) const {
  const auto valid = ValidateCellAdmissionClaimIntent(intent);
  if (!valid.ok()) return Result<PreparedProgramResetStatus>::Err(valid.error());
  PreparedProgramResetStatus proof(intent);
  proof.owner = this;
  {
    std::lock_guard lock(mutex_);
    proof.generation = generation_;
    proof.readiness = readiness_;
    proof.manager = manager_liveness_required_set_;
    proof.manager_current_count = manager_liveness_current_unfenced_count_;
    proof.purpose = purpose;
    proof.claim = claim;
    // Existing exact replay is independent of current journal/readiness.
    if (purpose == ProgramResetJournalReadPurpose::kAcquire && FindReplayLocked(intent))
      return Result<PreparedProgramResetStatus>::Ok(std::move(proof));
    if (!readiness_.program_cycle_status_current || !readiness_.program_cycle_status)
      return AuthoringNotReady<PreparedProgramResetStatus>(generation_, "program_reset_status_not_current");
  }
  // No admission, liveness-transition or owner lock spans SQLite. In particular
  // Manager-loss containment must be able to close admission while this reads.
  if (!program_reset_status_source_)
    return Result<PreparedProgramResetStatus>::Err(
        {"authoring_not_ready", "Program Reset phase proof is unavailable",
         {{"reason", "program_reset_phase_proof_unavailable"}}});
  auto current = program_reset_status_source_(purpose, intent, claim);
  if (!current.ok()) return Result<PreparedProgramResetStatus>::Err(current.error());
  proof.status = std::move(current).value();
  return Result<PreparedProgramResetStatus>::Ok(std::move(proof));
}

Result<KernelCellAdmissionOwner::PreparedProgramResetStatus>
KernelCellAdmissionOwner::PrepareProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization) const {
  const auto valid = manyforge::core::ValidateProgramResetSceneAuthorization(authorization);
  if (!valid.ok()) return Result<PreparedProgramResetStatus>::Err(valid.error());
  std::optional<CellAdmissionClaimDTO> claim;
  {
    std::lock_guard lock(mutex_);
    const auto generation = authorization.reset_claim
        ? authorization.reset_claim->reset_claim_generation : authorization.cycle_claim_generation;
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& row) {
      return row.claim.claim_generation == generation && !row.release;
    });
    if (entry == entries_.end())
      return AuthoringNotReady<PreparedProgramResetStatus>(generation_, "program_reset_claim_not_exact");
    claim = entry->claim;
  }
  return PrepareProgramResetStatus(ProgramResetJournalReadPurpose::kReloadInitialScene,
                                   claim->intent, claim);
}

Result<KernelCellAdmissionOwner::PreparedProgramResetStatus>
KernelCellAdmissionOwner::PrepareProgramResetPositionAuthorization(
    const ProgramResetPositionAuthorizationDTO& authorization) const {
  const auto valid = manyforge::core::ValidateProgramResetPositionAuthorization(authorization);
  if (!valid.ok()) return Result<PreparedProgramResetStatus>::Err(valid.error());
  std::optional<CellAdmissionClaimDTO> claim;
  {
    std::lock_guard lock(mutex_);
    const auto generation = authorization.reset_claim_generation
        ? *authorization.reset_claim_generation : *authorization.cycle_claim_generation;
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& row) {
      return row.claim.claim_generation == generation && !row.release;
    });
    if (entry == entries_.end())
      return AuthoringNotReady<PreparedProgramResetStatus>(generation_, "program_reset_claim_not_exact");
    claim = entry->claim;
  }
  return PrepareProgramResetStatus(ProgramResetJournalReadPurpose::kResetRobotPosition,
                                   claim->intent, claim);
}

Result<void> KernelCellAdmissionOwner::ValidatePreparedProgramResetPositionAuthorization(
    const ProgramResetPositionAuthorizationDTO& authorization,
    const PreparedProgramResetStatus& proof) const {
  const auto valid = manyforge::core::ValidateProgramResetPositionAuthorization(authorization);
  if (!valid.ok()) return valid;
  std::lock_guard lock(mutex_);
  const auto generation = authorization.reset_claim_generation
      ? *authorization.reset_claim_generation : *authorization.cycle_claim_generation;
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& row) {
    return row.claim.claim_generation == generation && !row.release;
  });
  const auto kind = authorization.reset_claim_generation ? CellAdmissionClaimKindDTO::kProgramReset
                                                       : CellAdmissionClaimKindDTO::kProgramCycle;
  if (entry == entries_.end() || entry->claim.intent.claim_kind != kind ||
      entry->claim.intent.owner_role != CellAdmissionOwnerRoleDTO::kProgramRunner ||
      (authorization.reset_claim_generation && (entry->reconcile_settlement || held_claim_count_ != 1U)) ||
      HasUnsettledChildrenLocked(generation))
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  const auto current = CurrentResetLocked(ProgramResetJournalReadPurpose::kResetRobotPosition,
                                         entry->claim.intent, entry->claim, proof);
  if (!current.ok()) return Result<void>::Err(current.error());
  const auto& status = current.value();
  const auto& accepted = *status.accepted_reset;
  if (!accepted.reset_robot_position ||
      status.program_runner_owner_epoch != authorization.program_runner_owner_epoch ||
      status.cycle_generation != authorization.cycle_generation ||
      status.program_run_id != authorization.program_run_id ||
      status.cycle_claim_generation != authorization.cycle_claim_generation ||
      status.terminal_or_settlement_reference != authorization.terminal_or_settlement_reference ||
      accepted.request_owner_epoch != authorization.request_owner_epoch ||
      accepted.request_id != authorization.request_id ||
      accepted.request_payload_digest != authorization.request_payload_digest ||
      accepted.reset_claim_generation != authorization.reset_claim_generation)
    return AuthoringNotReady<void>(generation_, "program_reset_status_mismatch");
  const bool held_by_current_owner = !entry->reconcile_settlement &&
      entry->claim.intent.owner_epoch == authorization.program_runner_owner_epoch;
  const bool reconciled_to_current_owner = entry->reconcile_settlement &&
      entry->reconcile_settlement->predecessor_claim == entry->claim &&
      entry->reconcile_settlement->successor_owner_role == CellAdmissionOwnerRoleDTO::kProgramRunner &&
      entry->reconcile_settlement->successor_owner_epoch == authorization.program_runner_owner_epoch &&
      entry->reconcile_settlement->settlement_predicate == CellAdmissionSettlementKindDTO::kReconciled;
  if (!held_by_current_owner && !reconciled_to_current_owner)
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_current");
  if (!authorization.reset_claim_generation) {
    if (entry->claim.intent.owner_transaction_id != authorization.program_run_id->value())
      return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  } else {
    const auto identity = ProgramResetClaimRequestId(status);
    if (!identity.ok() || entry->claim.intent.request_id != identity.value() ||
        entry->claim.intent.owner_transaction_id != identity.value().value() ||
        entry->claim.intent.request_payload_digest != accepted.request_payload_digest)
      return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  }
  return Result<void>::Ok();
}

Result<ProgramCycleStatusDTO> KernelCellAdmissionOwner::CurrentResetLocked(
    const ProgramResetJournalReadPurpose purpose, const CellAdmissionClaimIntentDTO& intent,
    const std::optional<CellAdmissionClaimDTO>& claim,
    const PreparedProgramResetStatus& proof) const {
  const bool reset_claim = intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset;
  if ((reset_claim && !NonCycleReadinessLocked()) || !readiness_.program_cycle_status_current ||
      !readiness_.program_cycle_status) {
    return AuthoringNotReady<ProgramCycleStatusDTO>(generation_, "program_reset_status_not_current");
  }
  if (proof.owner != this || proof.generation != generation_ || proof.readiness != readiness_ ||
      proof.manager != manager_liveness_required_set_ ||
      proof.manager_current_count != manager_liveness_current_unfenced_count_ ||
      !manager_liveness_required_set_ ||
      manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size() ||
      proof.purpose != purpose ||
      proof.intent != intent || proof.claim != claim || !proof.status ||
      persistence_outcome_unknown_ || published_stop_fault_active_ ||
      (stop_fault_active_ && stop_fault_active_()) || route_recovery_pending_) {
    return AuthoringNotReady<ProgramCycleStatusDTO>(generation_, "program_reset_dependencies_changed");
  }
  const auto current = Result<ProgramCycleStatusDTO>::Ok(*proof.status);
  const auto& cycle = current.value();
  const auto& observed = *readiness_.program_cycle_status;
  if (!ValidateProgramCycleStatus(cycle).ok() || cycle.state != ProgramCycleStateDTO::kResetting ||
      !cycle.accepted_reset || cycle.program_run_id != observed.program_run_id ||
      cycle.program_runner_owner_epoch != observed.program_runner_owner_epoch ||
      cycle.cycle_generation != observed.cycle_generation ||
      (reset_claim && observed.state != ProgramCycleStateDTO::kIdleAfterReset &&
       observed.state != ProgramCycleStateDTO::kResetting &&
       observed.state != ProgramCycleStateDTO::kRecoveryRequired &&
       !(observed.state == ProgramCycleStateDTO::kTerminalAwaitingReset &&
         cycle.program_run_id && !cycle.cycle_claim_generation))) {
    return AuthoringNotReady<ProgramCycleStatusDTO>(generation_, "program_reset_status_mismatch");
  }
  if (observed.accepted_reset) {
    auto previous = *observed.accepted_reset;
    auto accepted = *cycle.accepted_reset;
    previous.reset_claim_generation.reset();
    accepted.reset_claim_generation.reset();
    if (previous != accepted) {
      return AuthoringNotReady<ProgramCycleStatusDTO>(generation_, "program_reset_accepted_identity_mismatch");
    }
  }
  return current;
}

bool KernelCellAdmissionOwner::MatchingProgramResetLocked(
    const CellAdmissionClaimIntentDTO& intent, const PreparedProgramResetStatus& proof) const {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kProgramReset || held_claim_count_ != 0U) {
    return false;
  }
  const auto current = CurrentResetLocked(ProgramResetJournalReadPurpose::kAcquire, intent, std::nullopt, proof);
  if (!current.ok()) return false;
  const auto& cycle = current.value();
  if (cycle.cycle_claim_generation || cycle.accepted_reset->reset_claim_generation ||
      cycle.program_runner_owner_epoch != intent.owner_epoch) {
    return false;
  }
  const auto identity = ProgramResetClaimRequestId(cycle);
  return identity.ok() && intent.request_id == identity.value() &&
         intent.owner_transaction_id == identity.value().value() &&
         intent.request_payload_digest == cycle.accepted_reset->request_payload_digest;
}

Result<void> KernelCellAdmissionOwner::AdvanceGenerationLocked() {
  if (generation_.value() >= 9'007'199'254'740'991ULL) {
    return Result<void>::Err(ErrorDTO{"authoring_not_ready",
                                      "cell-authoring admission is not ready",
                                      {{"reason", "admission_generation_exhausted"}}});
  }
  const AdmissionGeneration next(generation_.value() + 1U);
  auto recorded =
      journal_.RecordCellAdmissionGenerationAdvanced(kernel_owner_epoch_, generation_, next);
  if (!recorded.ok()) {
    return Result<void>::Err(recorded.error());
  }
  generation_ = next;
  return Result<void>::Ok();
}

AuthoringAdmissionDTO KernelCellAdmissionOwner::AdmissionLocked() const noexcept {
  return AuthoringAdmissionDTO{!persistence_outcome_unknown_ && !StopFaultClosesCellLocked() &&
                                   !route_recovery_pending_ && BaseReadinessLocked() &&
                                   held_claim_count_ == 0U,
                               generation_};
}

std::optional<ErrorDTO> KernelCellAdmissionOwner::persistence_outcome_unknown() const {
  std::lock_guard lock(mutex_);
  return persistence_outcome_unknown_;
}

void KernelCellAdmissionOwner::CloseForPersistenceUnknownLocked(
    ErrorDTO&& error, const CellAdmissionClaimDTO& claim) {
  if (persistence_outcome_unknown_) return;
  persistence_outcome_unknown_ = std::move(error);
  // One bounded first-cause log; physical Stop never takes this ordinary path.
  std::fprintf(
      stderr,
      "ManyForge Kernel admission persistence outcome unknown: owner=%.256s request=%.256s "
      "transaction=%.256s claim=%llu\n",
      claim.intent.owner_epoch.value().c_str(), claim.intent.request_id.value().c_str(),
      claim.intent.owner_transaction_id.c_str(),
      static_cast<unsigned long long>(claim.claim_generation.value()));
}

Result<void> KernelCellAdmissionOwner::RefreshStopFaultAdmission() {
  std::lock_guard transition_lock(liveness_transition_mutex_);
  std::lock_guard lock(mutex_);
  const bool active = stop_fault_active_ && stop_fault_active_();
  if (active == published_stop_fault_active_) return Result<void>::Ok();
  published_stop_fault_active_ = true;
  auto advanced = AdvanceGenerationLocked();
  if (!advanced.ok()) return advanced;
  published_stop_fault_active_ = active;
  return Result<void>::Ok();
}

Result<void> KernelCellAdmissionOwner::SetRouteRecoveryPending(bool pending) {
  std::lock_guard transition_lock(liveness_transition_mutex_);
  std::lock_guard lock(mutex_);
  if (route_recovery_pending_ == pending) return Result<void>::Ok();
  // A failed journal write must never reopen admission during attachment.
  route_recovery_pending_ = true;
  auto advanced = AdvanceGenerationLocked();
  if (!advanced.ok()) return advanced;
  route_recovery_pending_ = pending;
  return Result<void>::Ok();
}

bool KernelCellAdmissionOwner::route_recovery_pending() const {
  std::lock_guard lock(mutex_);
  return route_recovery_pending_;
}

AuthoringAdmissionDTO KernelCellAdmissionOwner::GetAuthoringAdmission() const {
  std::lock_guard lock(mutex_);
  return AdmissionLocked();
}

bool KernelCellAdmissionOwner::AuthoringClosedOnlyByClosing() const {
  std::lock_guard lock(mutex_);
  return !persistence_outcome_unknown_ &&
         StopFaultScopeLocked() == StopFaultAdmissionScope::kClosing &&
         !route_recovery_pending_ && BaseReadinessLocked() && held_claim_count_ == 0U;
}

Result<void> KernelCellAdmissionOwner::ValidateProgramRunnerContactLossSource(
    const ProgramRunnerContactLossDTO& loss) const {
  const auto valid = ValidateProgramRunnerContactLoss(loss);
  if (!valid.ok()) return valid;
  std::lock_guard lock(mutex_);
  if (!manager_liveness_required_set_ ||
      loss.manager_owner_epoch != manager_liveness_required_set_->manager_owner_epoch ||
      loss.manager_runtime_epoch != manager_liveness_required_set_->manager_runtime_epoch) {
    return Reject<void>("invalid_request", "program_runner_contact_loss_invalid");
  }
  return Result<void>::Ok();
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::UpdateCellAdmissionReadiness(
    const CellAdmissionReadinessSnapshotDTO& snapshot,
    std::shared_ptr<IIoSignalService>* actual_retired_service) {
  if (actual_retired_service) actual_retired_service->reset();
  if (snapshot.required_component_total > kMaximumCellAdmissionScopeEntries ||
      snapshot.required_component_current_compatible_ready_converged >
          snapshot.required_component_total ||
      snapshot.blocking_fault_count > kMaximumCellAdmissionScopeEntries ||
      snapshot.unresolved_owner_count > kMaximumCellAdmissionScopeEntries ||
      snapshot.unsettled_effect_count > kMaximumCellAdmissionScopeEntries ||
      !snapshot.program_cycle_status.has_value() ||
      !ValidateProgramCycleStatus(*snapshot.program_cycle_status).ok()) {
    return Reject<AuthoringAdmissionDTO>("invalid_request", "readiness_snapshot_invalid");
  }
  std::lock_guard transition_lock(liveness_transition_mutex_);
  std::lock_guard lock(mutex_);
  if (snapshot.program_runner_loss &&
      (!ValidateProgramRunnerContactLoss(*snapshot.program_runner_loss).ok() ||
       snapshot.program_cycle_status_current || !manager_liveness_required_set_ ||
       snapshot.program_runner_loss->manager_owner_epoch !=
           manager_liveness_required_set_->manager_owner_epoch ||
       snapshot.program_runner_loss->manager_runtime_epoch !=
           manager_liveness_required_set_->manager_runtime_epoch)) {
    return Reject<AuthoringAdmissionDTO>("invalid_request", "program_runner_contact_loss_invalid");
  }
  if (readiness_ != snapshot) {
    if (generation_.value() >= 9'007'199'254'740'991ULL) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "admission_generation_exhausted");
    }
    const AdmissionGeneration next(generation_.value() + 1U);
    auto recorded =
        journal_.RecordCellAdmissionGenerationAdvanced(kernel_owner_epoch_, generation_, next);
    if (!recorded.ok()) {
      return Result<AuthoringAdmissionDTO>::Err(recorded.error());
    }
    readiness_ = snapshot;
    generation_ = next;
  }
  return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
}

Result<void> KernelCellAdmissionOwner::Publish(const ManagerLivenessObservationDTO& observation) {
  return Publish(observation, nullptr);
}

Result<void> KernelCellAdmissionOwner::Publish(
    const ManagerLivenessObservationDTO& observation,
    std::shared_ptr<IIoSignalService>* actual_retired_service) {
  if (!ValidateManagerLivenessObservation(observation).ok()) {
    return Reject<void>("invalid_request", "manager_liveness_observation_invalid");
  }
  std::lock_guard transition_lock(liveness_transition_mutex_);
  std::unique_lock lock(mutex_);
  if (!manager_liveness_required_set_.has_value()) {
    return Reject<void>("invalid_request", "manager_liveness_set_not_configured");
  }
  const auto& expected = *manager_liveness_required_set_;
  const auto& status = observation.status;
  if (status.cell_id != expected.cell_id ||
      observation.active_bundle_id != expected.active_bundle_id ||
      observation.required_runtime_set_digest != expected.required_runtime_set_digest ||
      status.manager_runtime_unit_id != expected.manager_runtime_unit_id ||
      status.manager_runtime_epoch != expected.manager_runtime_epoch ||
      status.manager_owner_epoch != expected.manager_owner_epoch) {
    return Reject<void>("invalid_request", "manager_liveness_identity_mismatch");
  }
  const auto found = std::lower_bound(manager_liveness_rows_.begin(), manager_liveness_rows_.end(),
                                      status.receiver_runtime_unit_id,
                                      [](const ManagerLivenessRow& row, const RuntimeUnitId& id) {
                                        return row.receiver.runtime_unit_id < id;
                                      });
  if (found == manager_liveness_rows_.end() ||
      found->receiver.runtime_unit_id != status.receiver_runtime_unit_id ||
      found->receiver.runtime_epoch != status.receiver_runtime_epoch ||
      found->receiver.runtime_policy_id != status.runtime_policy_id ||
      found->receiver.runtime_policy_revision != status.runtime_policy_revision) {
    return Reject<void>("invalid_request", "manager_liveness_receiver_foreign");
  }
  const bool retry_pending_fence = status.receiver_status_sequence == found->status_sequence &&
                                   found->generation_advance_pending && status.command_fenced;
  if (status.receiver_status_sequence < found->status_sequence ||
      (status.receiver_status_sequence == found->status_sequence && !retry_pending_fence)) {
    return Reject<void>("invalid_request", "manager_liveness_status_not_strict");
  }
  const bool next_current =
      status.state == ManagerLivenessStateDTO::kCurrent && !status.command_fenced;
  if (next_current && found->direct_loss_inhibited) {
    return Reject<void>("invalid_state_transition", "manager_liveness_loss_inhibited");
  }
  const auto token = status.receiver_status_sequence;
  bool transition_required = retry_pending_fence;
  if (!retry_pending_fence) {
    found->status_sequence = token;
    if (found->current_unfenced != next_current) {
      transition_required = true;
      found->generation_advance_pending = true;
      found->generation_pending_sequence = token;
      found->generation_pending_current = next_current;
      if (!next_current) {
        --manager_liveness_current_unfenced_count_;
        found->current_unfenced = false;
      }
    } else if (found->generation_advance_pending) {
      if (found->generation_pending_current == next_current) {
        transition_required = true;
        found->generation_pending_sequence = token;
      } else {
        found->generation_advance_pending = false;
      }
    }
  }
  // Invalidate already acquired observations at the same health boundary as
  // new admissions, before journal I/O releases this mutex. A registered rule
  // must not keep consuming the old BB snapshot while persistence is blocked.
  auto invalidated = !next_current && required_health_loss_observer_
                         ? required_health_loss_observer_(actual_retired_service)
                         : Result<void>::Ok();
  if (!transition_required) return invalidated;
  if (generation_.value() >= kMaximumManagerLivenessCounter) {
    if (!invalidated.ok()) return invalidated;
    return Reject<void>("authoring_not_ready", "admission_generation_exhausted");
  }
  const auto previous_generation = generation_;
  const AdmissionGeneration next_generation(generation_.value() + 1U);
  lock.unlock();
  auto recorded = journal_.RecordCellAdmissionGenerationAdvanced(
      kernel_owner_epoch_, previous_generation, next_generation);
  lock.lock();
  if (!recorded.ok()) {
    auto error = recorded.error();
    if (!invalidated.ok())
      error.details.emplace("program_bb_invalidation_error", invalidated.error().code);
    return Result<void>::Err(std::move(error));
  }
  generation_ = next_generation;
  const auto current = std::lower_bound(
      manager_liveness_rows_.begin(), manager_liveness_rows_.end(), status.receiver_runtime_unit_id,
      [](const ManagerLivenessRow& row, const RuntimeUnitId& id) {
        return row.receiver.runtime_unit_id < id;
      });
  if (current != manager_liveness_rows_.end() &&
      current->receiver.runtime_unit_id == status.receiver_runtime_unit_id &&
      current->generation_advance_pending && current->generation_pending_sequence == token) {
    current->generation_advance_pending = false;
    if (next_current && current->status_sequence == token && !current->direct_loss_inhibited &&
        !current->current_unfenced) {
      current->current_unfenced = true;
      ++manager_liveness_current_unfenced_count_;
    }
  }
  return invalidated;
}

bool KernelCellAdmissionOwner::LatchManagerLivenessLoss(
    const ManagerLivenessLossIntentDTO& intent,
    std::optional<ErrorDTO>* source_invalidation_error) noexcept {
  return LatchManagerLivenessLoss(intent, source_invalidation_error, nullptr);
}

bool KernelCellAdmissionOwner::LatchManagerLivenessLoss(
    const ManagerLivenessLossIntentDTO& intent,
    std::optional<ErrorDTO>* source_invalidation_error,
    std::shared_ptr<IIoSignalService>* actual_retired_service) noexcept {
  bool committed = false;
  try {
    if (!ValidateManagerLivenessLossIntent(intent).ok()) {
      return false;
    }
    std::lock_guard lock(mutex_);
    if (!manager_liveness_required_set_.has_value()) {
      return false;
    }
    const auto& expected = *manager_liveness_required_set_;
    if (intent.cell_id != expected.cell_id ||
        intent.active_bundle_id != expected.active_bundle_id ||
        intent.required_runtime_set_digest != expected.required_runtime_set_digest ||
        intent.lost_manager_runtime_unit_id != expected.manager_runtime_unit_id ||
        intent.lost_manager_runtime_epoch != expected.manager_runtime_epoch ||
        intent.lost_manager_owner_epoch != expected.manager_owner_epoch) {
      return false;
    }
    const auto found =
        std::lower_bound(manager_liveness_rows_.begin(), manager_liveness_rows_.end(),
                         intent.receiver_runtime_unit_id,
                         [](const ManagerLivenessRow& row, const RuntimeUnitId& id) {
                           return row.receiver.runtime_unit_id < id;
                         });
    if (found == manager_liveness_rows_.end() ||
        found->receiver.runtime_unit_id != intent.receiver_runtime_unit_id ||
        found->receiver.runtime_epoch != intent.receiver_runtime_epoch ||
        found->receiver.runtime_policy_id != intent.runtime_policy_id ||
        found->receiver.runtime_policy_revision != intent.runtime_policy_revision ||
        intent.receiver_status_sequence <= found->status_sequence) {
      return false;
    }
    found->status_sequence = intent.receiver_status_sequence;
    found->direct_loss_inhibited = true;
    if (found->current_unfenced) {
      found->current_unfenced = false;
      --manager_liveness_current_unfenced_count_;
      found->generation_advance_pending = true;
      found->generation_pending_sequence = intent.receiver_status_sequence;
      found->generation_pending_current = false;
    }
    committed = true;
    if (required_health_loss_observer_) {
      auto invalidated = required_health_loss_observer_(actual_retired_service);
      if (!invalidated.ok() && source_invalidation_error)
        *source_invalidation_error = invalidated.error();
    }
    return true;
  } catch (...) {
    // A bounded invalidation/diagnostic failure cannot turn a committed real
    // health loss into a stale request and suppress mandatory containment.
    return committed;
  }
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReplaceManagerLivenessRequiredSet(
    const ManagerLivenessRequiredSetDTO& required_set) {
  return ReplaceManagerLivenessRequiredSet(required_set, nullptr);
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReplaceManagerLivenessRequiredSet(
    const ManagerLivenessRequiredSetDTO& required_set,
    std::shared_ptr<IIoSignalService>* actual_retired_service) {
  if (!ValidateManagerLivenessRequiredSet(required_set).ok()) {
    return Reject<AuthoringAdmissionDTO>("invalid_request",
                                         "manager_liveness_required_set_invalid");
  }
  auto next_set = required_set;
  std::vector<ManagerLivenessRow> next_rows;
  next_rows.reserve(required_set.required_receivers.size());
  for (const auto& receiver : required_set.required_receivers) {
    next_rows.push_back({receiver, 0U, false, false, false, 0U, false});
  }
  std::lock_guard transition_lock(liveness_transition_mutex_);
  std::lock_guard lock(mutex_);
  if (manager_liveness_required_set_.has_value() &&
      *manager_liveness_required_set_ == required_set) {
    return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
  }
  auto advanced = AdvanceGenerationLocked();
  if (!advanced.ok()) {
    return Result<AuthoringAdmissionDTO>::Err(advanced.error());
  }
  manager_liveness_required_set_ = std::move(next_set);
  manager_liveness_rows_ = std::move(next_rows);
  manager_liveness_current_unfenced_count_ = 0U;
  if (required_health_loss_observer_) {
    auto invalidated = required_health_loss_observer_(actual_retired_service);
    if (!invalidated.ok()) return Result<AuthoringAdmissionDTO>::Err(invalidated.error());
  }
  return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
}

const KernelCellAdmissionOwner::Entry* KernelCellAdmissionOwner::FindReplayLocked(
    const CellAdmissionClaimIntentDTO& intent) const {
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return SameRequest(entry.claim.intent, intent);
  });
  return found == entries_.end() ? nullptr : &*found;
}

KernelCellAdmissionOwner::Entry* KernelCellAdmissionOwner::FindHeldLocked(
    CellAdmissionClaimGeneration generation) {
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return entry.claim.claim_generation == generation && !entry.release.has_value() &&
           !entry.reconcile_settlement.has_value();
  });
  return found == entries_.end() ? nullptr : &*found;
}

const KernelCellAdmissionOwner::Entry* KernelCellAdmissionOwner::ParentCycleLocked(
    const CellAdmissionClaimIntentDTO& intent) const {
  const auto generation = parent_program_cycle_claim(intent);
  if (!generation.has_value()) return nullptr;
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return !entry.release.has_value() && !entry.reconcile_settlement.has_value() &&
           entry.claim.claim_generation == *generation &&
           entry.claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle;
  });
  return found == entries_.end() ? nullptr : &*found;
}

bool KernelCellAdmissionOwner::IsStaleOwnerLocked(const CellAdmissionClaimIntentDTO& intent) const {
  const auto current =
      std::find_if(current_owners_.begin(), current_owners_.end(),
                   [&](const auto& owner) { return owner.owner_role == intent.owner_role; });
  return current != current_owners_.end() && current->owner_epoch != intent.owner_epoch;
}

bool KernelCellAdmissionOwner::HasUnsettledChildrenLocked(
    const CellAdmissionClaimGeneration parent,
    const std::vector<CellAdmissionReconcileSettlementDTO>& prospective) const {
  return std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
    if (entry.release.has_value() || entry.reconcile_settlement.has_value() ||
        parent_program_cycle_claim(entry.claim.intent) != parent)
      return false;
    // Reconcile authorizations are exact and generation-sorted before this
    // check. Assess the whole prospective batch before changing any claim.
    const auto settlement =
        std::lower_bound(prospective.begin(), prospective.end(), entry.claim.claim_generation,
                         [](const auto& candidate, const auto generation) {
                           return candidate.predecessor_claim.claim_generation < generation;
                         });
    return settlement == prospective.end() || settlement->predecessor_claim != entry.claim;
  });
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent) {
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset) {
    auto proof = PrepareProgramResetStatus(ProgramResetJournalReadPurpose::kAcquire, intent);
    if (!proof.ok()) return Result<CellAdmissionClaimDTO>::Err(proof.error());
    return AcquirePreparedProgramResetClaim(intent, proof.value());
  }
  return AcquireClaim(intent, intervention_validator_);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquirePreparedProgramResetClaim(
    const CellAdmissionClaimIntentDTO& intent, const PreparedProgramResetStatus& proof) {
  return AcquireClaim(intent, intervention_validator_, false, {}, &proof);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireManualCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionInterventionValidator& validate_offer) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kManualOperation || !validate_offer) {
    return Reject<CellAdmissionClaimDTO>("invalid_request", "manual_offer_validator_required");
  }
  return AcquireClaim(intent, validate_offer);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireRouteRecoveryClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionInterventionValidator& validate_offer) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kInterventionOrRecovery ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner ||
      !intent.owner_transaction_id.starts_with("route-selection.") ||
      !intent.scope.resources.empty() || intent.scope.configurations.size() != 1U ||
      !validate_offer) {
    return Reject<CellAdmissionClaimDTO>("invalid_request", "route_recovery_validator_required");
  }
  return AcquireClaim(intent, validate_offer);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireSelectedBringUpClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionInterventionValidator& validate_offer) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kInterventionOrRecovery ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner ||
      !intent.owner_transaction_id.starts_with("selected-lifecycle.") || !validate_offer) {
    return Reject<CellAdmissionClaimDTO>("invalid_request", "selected_bring_up_validator_required");
  }
  return AcquireClaim(intent, validate_offer);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireApplyBringUpClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionApplyBringUpValidator& validate_plan) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kManager ||
      !intent.scope.resources.empty() || !intent.scope.configurations.empty() ||
      intent.program_cycle_evidence || intent.intervention_evidence || intent.program_node_evidence ||
      !validate_plan) {
    return Reject<CellAdmissionClaimDTO>("invalid_request", "apply_bring_up_validator_required");
  }
  return AcquireClaim(intent, [&](const auto& checked, const auto&) {
    // AcquireClaim invokes this synchronously under this owner's mutex. The
    // composing Kernel joins permitted selected deficits to these actual
    // published counts, rather than trusting caller-supplied readiness flags.
    return validate_plan(checked, readiness_, manager_liveness_required_set_->manager_runtime_epoch);
  }, true);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireCloseDisableLifecycleClaim(
    const CellAdmissionClaimIntentDTO& intent) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kComponentLifecycle ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner ||
      intent.program_node_evidence || intent.intervention_evidence)
    return Reject<CellAdmissionClaimDTO>("invalid_request", "close_disable_claim_invalid");
  return AcquireClaim(intent, intervention_validator_, false, {}, nullptr, {}, true);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireConfigurationApplyClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionConfigurationApplyValidator& validate, const bool canonical_empty_candidate) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kManager || !intent.scope.resources.empty() ||
      !intent.scope.configurations.empty() || intent.program_cycle_evidence ||
      intent.intervention_evidence || intent.program_node_evidence || !validate) {
    return Reject<CellAdmissionClaimDTO>("invalid_request",
                                         "configuration_apply_validator_required");
  }
  return AcquireClaim(intent, {}, false, validate, nullptr, {}, canonical_empty_candidate);
}

StopFaultAdmissionScope KernelCellAdmissionOwner::StopFaultScopeLocked() const {
  if (stop_fault_scope_) return stop_fault_scope_();
  return stop_fault_active_ && stop_fault_active_() ? StopFaultAdmissionScope::kCell
                                                    : StopFaultAdmissionScope::kNone;
}

bool KernelCellAdmissionOwner::StopFaultClosesCellLocked() const {
  const auto scope = StopFaultScopeLocked();
  // A carried latch opens the cell at once; any other stale publication of an
  // active latch keeps it closed until RefreshStopFaultAdmission (unchanged).
  return scope == StopFaultAdmissionScope::kCell || scope == StopFaultAdmissionScope::kClosing ||
         (published_stop_fault_active_ && scope != StopFaultAdmissionScope::kCarried);
}

bool KernelCellAdmissionOwner::IoOutputRecoveryDependenciesReadyLocked() const noexcept {
  return !persistence_outcome_unknown_ && !published_stop_fault_active_ &&
         !(stop_fault_active_ && stop_fault_active_()) && !route_recovery_pending_ &&
         manager_liveness_required_set_ &&
         manager_liveness_current_unfenced_count_ == manager_liveness_rows_.size() &&
         readiness_.required_component_status_current && readiness_.deployment_status_current &&
         readiness_.deployment_converged && readiness_.blocking_fault_count == 0U &&
         readiness_.unresolved_owner_count == 0U && readiness_.unsettled_effect_count == 0U &&
         readiness_.program_cycle_status_current && readiness_.program_cycle_status &&
         !readiness_.program_runner_loss;
}

Result<void> KernelCellAdmissionOwner::ValidateHeldIoOutputRecoveryLifecycleClaim(
    const LifecycleTransactionId& transaction,
    const CellAdmissionIoOutputRecoveryValidator& validate) const {
  std::lock_guard lock(mutex_);
  if (!validate || !IoOutputRecoveryDependenciesReadyLocked())
    return AuthoringNotReady<void>(generation_, "io_output_recovery_dependencies_not_current");
  const auto claim = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return !entry.release && !entry.reconcile_settlement &&
           entry.claim.intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle &&
           entry.claim.intent.owner_transaction_id == transaction.value();
  });
  if (claim == entries_.end())
    return AuthoringNotReady<void>(generation_, "io_output_recovery_lifecycle_claim_not_held");
  std::vector<CellAdmissionClaimDTO> held;
  held.reserve(held_claim_count_);
  for (const auto& entry : entries_)
    if (!entry.release && !entry.reconcile_settlement && &entry != &*claim)
      held.push_back(entry.claim);
  return validate(claim->claim.intent, held, readiness_,
                  manager_liveness_required_set_->manager_runtime_epoch);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireIoOutputRecoveryLifecycleClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionIoOutputRecoveryValidator& validate) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kComponentLifecycle ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner ||
      intent.program_node_evidence || intent.intervention_evidence || !validate)
    return Reject<CellAdmissionClaimDTO>("invalid_request", "io_output_recovery_claim_invalid");
  return AcquireClaim(intent, {}, false, {}, nullptr, validate);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionInterventionValidator& validate_offer, const bool selected_apply_bring_up,
    const CellAdmissionConfigurationApplyValidator& configuration_apply_validator,
    const PreparedProgramResetStatus* reset_proof,
    const CellAdmissionIoOutputRecoveryValidator& io_output_recovery_validator,
    const bool close_step_claim) {
  const bool configuration_apply = static_cast<bool>(configuration_apply_validator);
  std::lock_guard transition_lock(liveness_transition_mutex_);
  auto valid = ValidateCellAdmissionClaimIntent(intent);
  if (!valid.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(valid.error());
  }
  std::lock_guard lock(mutex_);
  if (!configuration_apply && IsStaleOwnerLocked(intent)) {
    return Reject<CellAdmissionClaimDTO>("stale_request_owner_epoch", "request_owner_epoch_stale");
  }
  if (const auto* replay = FindReplayLocked(intent); replay != nullptr) {
    if (replay->claim.intent != intent) {
      return Reject<CellAdmissionClaimDTO>("request_id_conflict", "request_reuse_conflict");
    }
    if (replay->release.has_value() || replay->reconcile_settlement.has_value()) {
      return Reject<CellAdmissionClaimDTO>("request_result_expired", "request_result_compacted");
    }
    return Result<CellAdmissionClaimDTO>::Ok(replay->claim);
  }
  if (persistence_outcome_unknown_)
    return Result<CellAdmissionClaimDTO>::Err(*persistence_outcome_unknown_);
  const auto intent_digest = CellAdmissionClaimIntentDigest(intent);
  if (!intent_digest.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(intent_digest.error());
  }
  const auto compacted =
      std::find_if(membership_.begin(), membership_.end(), [&](const auto& member) {
        return member.owner_epoch == intent.owner_epoch && member.request_id == intent.request_id;
      });
  if (compacted != membership_.end()) {
    if (compacted->owner_role != intent.owner_role ||
        compacted->semantic_intent_digest != intent_digest.value()) {
      return Reject<CellAdmissionClaimDTO>("request_id_conflict", "request_reuse_conflict");
    }
    return Reject<CellAdmissionClaimDTO>("request_result_expired", "request_result_compacted");
  }
  if (!intent.program_node_evidence && intent.expected_admission_generation != generation_) {
    return Reject<CellAdmissionClaimDTO>("stale_generation", "expected_generation_stale");
  }
  // Exact replay above remains observable during a fault. The private Q91
  // parent alternative still requires its positive plan validation below.
  // D6: Close of a latched Scenario admits its motion-free steps, and a latch
  // carried past Close refuses only claims that lead to motion; the latch
  // itself stays until the operator's Recover cell clears it.
  const auto fault_scope = StopFaultScopeLocked();
  const bool latch_exempt =
      selected_apply_bring_up ||
      (intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
       intent.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner);
  const bool motion_intent = intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle ||
                             intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation ||
                             intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery;
  // Only Close's own claims: the Kernel-verified Disable and the exact
  // canonical-empty Apply. Close's Program Reset selects no Scene or position
  // effect and so takes no Reset claim; every kProgramReset claim is refused.
  const bool close_step = fault_scope == StopFaultAdmissionScope::kClosing && close_step_claim;
  // While Close runs (kClosing) only its own steps pass, ahead of every other
  // latch exemption: the canonical-empty Apply composes no component, so it
  // needs no bring-up, route-recovery or selected bring-up claim.
  if (fault_scope == StopFaultAdmissionScope::kClosing
          ? !close_step
          : !latch_exempt && (StopFaultClosesCellLocked() ||
                              (fault_scope != StopFaultAdmissionScope::kNone && motion_intent))) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                    "active_stop_fault_requires_recovery");
  }
  if (route_recovery_pending_ &&
      intent.claim_kind != CellAdmissionClaimKindDTO::kInterventionOrRecovery) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "route_recovery_pending");
  }
  const auto retire_released = [&](const auto& entry) {
    return entry.release.has_value() && entry.claim.intent.owner_role == intent.owner_role &&
           entry.claim.intent.owner_epoch == intent.owner_epoch;
  };
  const auto retired_count =
      static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), retire_released));
  if (membership_.size() - retired_count >= capacity_) {
    return Reject<CellAdmissionClaimDTO>("request_ledger_full", "claim_capacity_exhausted");
  }

  std::vector<CellAdmissionClaimDTO> configuration_held;
  if (configuration_apply) {
    for (const auto& entry : entries_) {
      if (!entry.release && !entry.reconcile_settlement) {
        if (entry.claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
            entry.claim.intent.owner_role != CellAdmissionOwnerRoleDTO::kManager) {
          return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                          "configuration_apply_foreign_claim");
        }
        configuration_held.push_back(entry.claim);
      }
    }
    std::sort(configuration_held.begin(), configuration_held.end(),
              [](const auto& a, const auto& b) { return a.claim_generation < b.claim_generation; });
  }

  const auto* parent = ParentCycleLocked(intent);
  // Reset's Scene authorization releases this mutex before Scene I/O. Once
  // resetting is published, no fresh child may invalidate its no-child check.
  // Exact held-claim replay above still observes an admission made before Reset.
  if (parent_program_cycle_claim(intent).has_value() &&
      readiness_.program_cycle_status.has_value() &&
      readiness_.program_cycle_status->state == ProgramCycleStateDTO::kResetting &&
      readiness_.program_cycle_status->cycle_claim_generation ==
          parent_program_cycle_claim(intent)) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "program_cycle_resetting");
  }
  if (intent.program_node_evidence.has_value()) {
    // A Program runs under a held cycle claim. Component lifecycle changes
    // are explicit idle-time configuration operations, so no Program child
    // claim may acquire their authority, even with valid node evidence.
    if (intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(
          generation_, "component_lifecycle_requires_idle_cell");
    }
    const auto& evidence = *intent.program_node_evidence;
    const auto& cycle = readiness_.program_cycle_status;
    if (parent == nullptr || !program_node_validator_ ||
        parent->claim.intent.owner_epoch != evidence.program_runner_owner_epoch ||
        parent->claim.intent.owner_transaction_id != evidence.program_run_id.value()) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                      "program_node_parent_not_current");
    }
    // A captured first tick can overtake Manager's first Starting publication.
    // Only the exact held parent and sealed capture below authorize that child;
    // a current Idle predecessor is not a second, contradictory cycle owner.
    const bool idle_predecessor = cycle && cycle->state == ProgramCycleStateDTO::kIdleAfterReset &&
                                  !cycle->program_run_id && !cycle->cycle_claim_generation &&
                                  !cycle->terminal_or_settlement_reference;
    const bool matching_cycle =
        cycle &&
        (cycle->state == ProgramCycleStateDTO::kRunning ||
         cycle->state == ProgramCycleStateDTO::kStarting) &&
        cycle->program_run_id == evidence.program_run_id &&
        (cycle->cycle_claim_generation == evidence.parent_program_cycle_claim_generation ||
         (cycle->state == ProgramCycleStateDTO::kStarting && !cycle->cycle_claim_generation));
    if (!readiness_.program_cycle_status_current || !cycle ||
        cycle->program_runner_owner_epoch != evidence.program_runner_owner_epoch ||
        (!idle_predecessor && !matching_cycle)) {
      auto rejected =
          AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "program_node_cycle_not_current");
      auto error = rejected.error();
      error.details.emplace("program_cycle_status_current",
                            readiness_.program_cycle_status_current ? "true" : "false");
      error.details.emplace("observed_program_cycle_state",
                            cycle ? std::string(to_string(cycle->state)) : "unknown");
      return Result<CellAdmissionClaimDTO>::Err(std::move(error));
    }
    if (!manager_liveness_required_set_.has_value() ||
        manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size() ||
        !readiness_.deployment_status_current || !readiness_.deployment_converged ||
        readiness_.blocking_fault_count != 0U || readiness_.unresolved_owner_count != 0U ||
        readiness_.unsettled_effect_count != 0U) {
      auto rejected = AuthoringNotReady<CellAdmissionClaimDTO>(
          generation_, "program_node_dependencies_not_current");
      auto error = rejected.error();
      error.details.emplace("manager_liveness_required_set_present",
                            manager_liveness_required_set_.has_value() ? "true" : "false");
      error.details.emplace("manager_liveness_current_unfenced_count",
                            std::to_string(manager_liveness_current_unfenced_count_));
      error.details.emplace("manager_liveness_required_count",
                            std::to_string(manager_liveness_rows_.size()));
      error.details.emplace("deployment_status_current",
                            readiness_.deployment_status_current ? "true" : "false");
      error.details.emplace("deployment_converged",
                            readiness_.deployment_converged ? "true" : "false");
      error.details.emplace("blocking_fault_count",
                            std::to_string(readiness_.blocking_fault_count));
      error.details.emplace("unresolved_owner_count",
                            std::to_string(readiness_.unresolved_owner_count));
      error.details.emplace("unsettled_effect_count",
                            std::to_string(readiness_.unsettled_effect_count));
      // Program failure projections retain the bounded message, while their
      // registered detail keys intentionally omit private admission facts.
      for (const auto* key : {"manager_liveness_required_set_present",
                              "manager_liveness_current_unfenced_count",
                              "manager_liveness_required_count", "deployment_status_current",
                              "deployment_converged", "blocking_fault_count",
                              "unresolved_owner_count", "unsettled_effect_count"}) {
        error.message.append("; ").append(key).append("=").append(error.details.at(key));
      }
      return Result<CellAdmissionClaimDTO>::Err(std::move(error));
    }
    // Concurrent children of this parent retain their owners' existing narrow
    // reservations. No authoring/manual/recovery claim inherits this exception.
    if (std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
          return !entry.release.has_value() && !entry.reconcile_settlement.has_value() &&
                 &entry != parent &&
                 (!entry.claim.intent.program_node_evidence.has_value() ||
                  parent_program_cycle_claim(entry.claim.intent) != parent->claim.claim_generation);
        })) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "program_node_claim_conflict");
    }
    // Ordinary admission is held by the composing Kernel across this bounded
    // fence check and journal write. Stop can overtake the write, but Resume
    // cannot release the fence until this exact child becomes visible/settled.
    const auto admitted = program_node_validator_(evidence);
    if (!admitted.ok()) return Result<CellAdmissionClaimDTO>::Err(admitted.error());
    if (intent.expected_admission_generation != generation_) {
      return Reject<CellAdmissionClaimDTO>("stale_generation", "expected_generation_stale");
    }
  } else if (configuration_apply) {
    if (!manager_liveness_required_set_ ||
        intent.owner_epoch != manager_liveness_required_set_->manager_owner_epoch ||
        manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size() ||
        !readiness_.required_component_status_current || !readiness_.deployment_status_current ||
        !readiness_.program_cycle_status_current || !readiness_.program_cycle_status ||
        readiness_.program_cycle_status->state != ProgramCycleStateDTO::kIdleAfterReset) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(
          generation_, "configuration_apply_dependencies_not_current");
    }
    // Installed inventory and each immutable old terminal must be independently
    // proved. This is not a generic readiness/fault bypass or live-owner takeover.
    const auto validated =
        configuration_apply_validator(intent, configuration_held, readiness_,
                                      manager_liveness_required_set_->manager_runtime_epoch);
    if (!validated.ok()) return Result<CellAdmissionClaimDTO>::Err(validated.error());
  } else if (selected_apply_bring_up) {
    if (held_claim_count_ != 0U || !validate_offer) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                      "apply_bring_up_claim_conflict");
    }
    if (!manager_liveness_required_set_.has_value() ||
        intent.owner_epoch != manager_liveness_required_set_->manager_owner_epoch ||
        manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size() ||
        !readiness_.required_component_status_current || !readiness_.deployment_status_current ||
        !readiness_.program_cycle_status_current || !readiness_.program_cycle_status ||
        readiness_.program_cycle_status->state != ProgramCycleStateDTO::kIdleAfterReset ||
        readiness_.unresolved_owner_count != 0U || readiness_.unsettled_effect_count != 0U) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                      "apply_bring_up_dependencies_not_current");
    }
    // Validate while the existing owner serializes acquisition. This callback
    // is supplied only by the composing Kernel, never by a public request or
    // restored status, and creates no second pending-plan registry.
    const auto validated = validate_offer(intent, std::nullopt);
    if (!validated.ok()) {
      return Result<CellAdmissionClaimDTO>::Err(validated.error());
    }
  } else if (intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery ||
             (intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation &&
              intent.intervention_evidence.has_value())) {
    if (intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation) {
      if (!ManualParentReadyLocked(parent)) {
        return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                        "manual_handover_parent_not_current");
      }
    }
    const bool compatible =
        held_claim_count_ == 0U || (held_claim_count_ == 1U && parent != nullptr);
    if (!compatible || !validate_offer) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "intervention_offer_not_exact");
    }
    auto evidence = validate_offer(
        intent,
        parent == nullptr ? std::nullopt : std::optional<CellAdmissionClaimDTO>(parent->claim));
    if (!evidence.ok()) {
      auto error =
          AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "intervention_offer_not_current")
              .error();
      const auto reason = evidence.error().details.find("reason");
      error.details.emplace("intervention_offer_reason", reason == evidence.error().details.end()
                                                             ? evidence.error().code
                                                             : reason->second);
      return Result<CellAdmissionClaimDTO>::Err(std::move(error));
    }
  } else if (io_output_recovery_validator) {
    // Preserve every unrelated health/Apply/Scene fault. These counters come
    // from those actual owners, not from the retained output operation.
    if (!IoOutputRecoveryDependenciesReadyLocked())
      return AuthoringNotReady<CellAdmissionClaimDTO>(
          generation_, "io_output_recovery_dependencies_not_current");
    std::vector<CellAdmissionClaimDTO> held;
    held.reserve(held_claim_count_);
    for (const auto& entry : entries_)
      if (!entry.release && !entry.reconcile_settlement) held.push_back(entry.claim);
    const auto checked = io_output_recovery_validator(
        intent, held, readiness_, manager_liveness_required_set_->manager_runtime_epoch);
    if (!checked.ok()) return Result<CellAdmissionClaimDTO>::Err(checked.error());
  } else if (intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle) {
    const auto blocking = LifecycleBlockingReasonLocked();
    if (intent.intervention_evidence.has_value() || blocking.has_value() ||
        route_recovery_pending_) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(
          generation_, blocking.has_value()                ? std::string(*blocking)
                       : route_recovery_pending_          ? "component_lifecycle_route_recovery_pending"
                                                          : "component_lifecycle_requires_idle_cell");
    }
  } else if (intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset) {
    if (!reset_proof || !MatchingProgramResetLocked(intent, *reset_proof)) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "program_reset_not_current");
    }
  } else if (!AdmissionLocked().authoring_allowed && !MatchingStartingProgramCycleLocked(intent)) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "positive_admission_required");
  }

  CellAdmissionClaimDTO claim{intent, next_claim_generation_, generation_, clock_.now()};
  if (claim.accepted_at_ns < 0 || generation_.value() >= 9'007'199'254'740'991ULL ||
      next_claim_generation_.value() >= 9'007'199'254'740'991ULL) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "acceptance_time_invalid");
  }
  std::vector<CellAdmissionRecoveryLedgerEntryDTO> footprint_entries;
  footprint_entries.reserve(entries_.size() + 1U);
  for (const auto& entry : entries_) {
    if (!retire_released(entry)) {
      footprint_entries.push_back({entry.claim, entry.release, entry.reconcile_settlement});
    }
  }
  footprint_entries.push_back({claim, std::nullopt, std::nullopt});
  auto footprint_owners = current_owners_;
  if (std::none_of(footprint_owners.begin(), footprint_owners.end(),
                   [&](const auto& owner) { return owner.owner_role == intent.owner_role; })) {
    footprint_owners.push_back({intent.owner_role, intent.owner_epoch});
  }
  if (configuration_apply) {
    for (auto& owner : footprint_owners) {
      if (owner.owner_role == CellAdmissionOwnerRoleDTO::kManager)
        owner.owner_epoch = intent.owner_epoch;
    }
  }
  const auto footprint = ComputeCellAdmissionRecoveryFootprint(
      kernel_owner_epoch_, AdmissionGeneration(generation_.value() + 1U), footprint_entries,
      footprint_owners);
  if (!footprint.ok() || !footprint.value().admitted()) {
    return Reject<CellAdmissionClaimDTO>("invalid_request", "recovery_footprint_exceeded");
  }
  std::vector<RequestId> retired_request_ids;
  retired_request_ids.reserve(retired_count);
  for (const auto& entry : entries_) {
    if (retire_released(entry)) {
      retired_request_ids.push_back(entry.claim.intent.request_id);
    }
  }
  // Prepare all allocation-bearing in-memory state before durable commit.
  Entry prepared_entry{claim, std::nullopt, std::nullopt};
  OperationJournalRecoveryDTO::RecoveredCellAdmissionMembershipDTO prepared_member{
      intent.owner_role, intent.owner_epoch, intent.request_id, intent_digest.value()};
  auto prepared_owners = footprint_owners;
  std::sort(prepared_owners.begin(), prepared_owners.end(),
            [](const auto& a, const auto& b) { return a.owner_role < b.owner_role; });
  if (configuration_apply) {
    auto recorded = journal_.RecordConfigurationApplyClaimAcquired(
        kernel_owner_epoch_, claim, {CellAdmissionOwnerRoleDTO::kManager, intent.owner_epoch},
        configuration_held, current_owners_);
    if (!recorded.ok()) {
      if (recorded.error().code == "persistence_outcome_unknown") {
        CloseForPersistenceUnknownLocked(std::move(recorded.error()), claim);
        return Result<CellAdmissionClaimDTO>::Err(*persistence_outcome_unknown_);
      }
      return Result<CellAdmissionClaimDTO>::Err(recorded.error());
    }
    if (recorded.value().intent != claim.intent ||
        recorded.value().claim_generation != claim.claim_generation ||
        recorded.value().acquired_admission_generation != claim.acquired_admission_generation ||
        recorded.value().accepted_at_ns < 0) {
      return Reject<CellAdmissionClaimDTO>("operation_journal_request_conflict",
                                           "configuration_apply_durable_claim_not_exact");
    }
    prepared_entry.claim.accepted_at_ns = recorded.value().accepted_at_ns;
    claim.accepted_at_ns = recorded.value().accepted_at_ns;
  } else {
    auto recorded = journal_.RecordCellAdmissionClaimAcquired(kernel_owner_epoch_, claim);
    if (!recorded.ok()) return Result<CellAdmissionClaimDTO>::Err(recorded.error());
  }
  // Both prepared copies carry the exact persisted timestamp on replay.
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), retire_released), entries_.end());
  membership_.erase(
      std::remove_if(membership_.begin(), membership_.end(),
                     [&](const auto& member) {
                       return member.owner_role == intent.owner_role &&
                              member.owner_epoch == intent.owner_epoch &&
                              std::find(retired_request_ids.begin(), retired_request_ids.end(),
                                        member.request_id) != retired_request_ids.end();
                     }),
      membership_.end());
  entries_.push_back(std::move(prepared_entry));
  membership_.push_back(std::move(prepared_member));
  current_owners_ = std::move(prepared_owners);
  ++held_claim_count_;
  next_claim_generation_ = CellAdmissionClaimGeneration(next_claim_generation_.value() + 1U);
  generation_ = AdmissionGeneration(generation_.value() + 1U);
  return Result<CellAdmissionClaimDTO>::Ok(std::move(claim));
}

std::vector<std::string> KernelCellAdmissionOwner::unsettled_program_child_nodes(
    const CellAdmissionClaimGeneration parent) const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> nodes;
  for (const auto& entry : entries_) {
    if (!entry.release.has_value() && !entry.reconcile_settlement.has_value() &&
        parent_program_cycle_claim(entry.claim.intent) == parent) {
      nodes.push_back(entry.claim.intent.program_node_evidence.has_value()
                          ? entry.claim.intent.program_node_evidence->node_instance_id
                          : std::string{});
    }
  }
  return nodes;
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReleaseCellAdmissionClaim(
    const CellAdmissionClaimReleaseDTO& release) {
  std::lock_guard transition_lock(liveness_transition_mutex_);
  auto valid = ValidateCellAdmissionClaimRelease(release);
  if (!valid.ok()) {
    return Result<AuthoringAdmissionDTO>::Err(valid.error());
  }
  std::lock_guard lock(mutex_);
  const auto current_owner =
      std::find_if(current_owners_.begin(), current_owners_.end(),
                   [&](const auto& owner) { return owner.owner_role == release.owner_role; });
  if (current_owner != current_owners_.end() && current_owner->owner_epoch != release.owner_epoch) {
    return Reject<AuthoringAdmissionDTO>("stale_generation", "claim_owner_epoch_stale");
  }
  auto* entry = FindHeldLocked(release.claim_generation);
  if (entry == nullptr || !SameReleaseIdentity(entry->claim, release) ||
      !SettlementKindCompatible(entry->claim.intent.claim_kind,
                                release.settlement.settlement_kind)) {
    return Reject<AuthoringAdmissionDTO>("stale_generation", "claim_release_not_exact");
  }
  if (entry->claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle &&
      HasUnsettledChildrenLocked(entry->claim.claim_generation)) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                    "parent_cycle_intervention_unsettled");
  }
  if (!settlement_validator_) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                    "settlement_validator_unavailable");
  }
  auto settlement = settlement_validator_(entry->claim, release);
  if (!settlement.ok()) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "settlement_not_proven");
  }
  if (generation_.value() >= 9'007'199'254'740'991ULL) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "admission_generation_exhausted");
  }
  const AdmissionGeneration next(generation_.value() + 1U);
  auto recorded =
      journal_.RecordCellAdmissionClaimReleased(kernel_owner_epoch_, entry->claim, release, next);
  if (!recorded.ok()) {
    return Result<AuthoringAdmissionDTO>::Err(recorded.error());
  }
  entry->release = release;
  --held_claim_count_;
  generation_ = next;
  return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReleaseTerminalApplyClaimsAfterSuccess(
    const CellAdmissionClaimDTO& fresh_claim,
    const CellAdmissionClaimReleaseDTO& fresh_success_release,
    const CellAdmissionConfigurationApplySuccessValidator& validate) {
  std::lock_guard transition_lock(liveness_transition_mutex_);
  std::lock_guard lock(mutex_);
  if (persistence_outcome_unknown_)
    return Result<AuthoringAdmissionDTO>::Err(*persistence_outcome_unknown_);
  auto* fresh = FindHeldLocked(fresh_claim.claim_generation);
  if (!validate || !fresh || fresh->claim != fresh_claim ||
      fresh_claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
      fresh_claim.intent.owner_role != CellAdmissionOwnerRoleDTO::kManager ||
      IsStaleOwnerLocked(fresh_claim.intent) ||
      !ValidateCellAdmissionClaimRelease(fresh_success_release).ok() ||
      !SameReleaseIdentity(fresh_claim, fresh_success_release) ||
      fresh_success_release.settlement.settlement_kind !=
          CellAdmissionSettlementKindDTO::kTerminalEffectsSettled) {
    return Reject<AuthoringAdmissionDTO>("invalid_request", "fresh_apply_success_not_exact");
  }
  std::vector<CellAdmissionClaimDTO> held;
  std::vector<CellAdmissionClaimDTO> old;
  std::vector<CellAdmissionClaimReleaseDTO> releases;
  for (const auto& entry : entries_) {
    if (entry.release || entry.reconcile_settlement) continue;
    if (entry.claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
        entry.claim.intent.owner_role != CellAdmissionOwnerRoleDTO::kManager) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "fresh_apply_success_foreign_claim");
    }
    held.push_back(entry.claim);
    if (entry.claim == fresh_claim) continue;
    old.push_back(entry.claim);
  }
  const auto order = [](const auto& a, const auto& b) {
    return a.claim_generation < b.claim_generation;
  };
  std::sort(held.begin(), held.end(), order);
  std::sort(old.begin(), old.end(), order);
  const auto validated = validate(fresh_claim, fresh_success_release, old);
  if (!validated.ok()) return Result<AuthoringAdmissionDTO>::Err(validated.error());
  if (old.empty()) return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
  for (const auto& claim : old) {
    releases.push_back({claim.intent.owner_role, claim.intent.owner_epoch,
                        claim.intent.owner_transaction_id, claim.intent.request_id,
                        claim.intent.request_payload_digest, claim.acquired_admission_generation,
                        claim.claim_generation, fresh_success_release.settlement});
  }
  auto recorded = journal_.RecordTerminalApplyClaimsReleasedBySuccess(
      kernel_owner_epoch_, fresh_claim, fresh_success_release, held, current_owners_, generation_);
  if (!recorded.ok()) {
    if (recorded.error().code == "persistence_outcome_unknown") {
      CloseForPersistenceUnknownLocked(std::move(recorded.error()), fresh_claim);
      return Result<AuthoringAdmissionDTO>::Err(*persistence_outcome_unknown_);
    }
    return Result<AuthoringAdmissionDTO>::Err(recorded.error());
  }
  if (recorded.value().value() != generation_.value() + 1U) {
    return Reject<AuthoringAdmissionDTO>("operation_journal_request_conflict",
                                         "fresh_apply_success_generation_not_exact");
  }
  for (auto& release : releases) {
    FindHeldLocked(release.claim_generation)->release = std::move(release);
    --held_claim_count_;
  }
  generation_ = recorded.value();
  return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
}

std::optional<CellAdmissionClaimDTO> KernelCellAdmissionOwner::held_claim(
    const CellAdmissionClaimKindDTO kind) const {
  std::lock_guard lock(mutex_);
  for (const auto& entry : entries_) {
    if (entry.claim.intent.claim_kind == kind && !entry.release.has_value() &&
        !entry.reconcile_settlement.has_value()) {
      return entry.claim;
    }
  }
  return std::nullopt;
}

std::optional<CellAdmissionClaimDTO> KernelCellAdmissionOwner::held_claim(
    const CellAdmissionClaimKindDTO kind, const std::string& owner_transaction_id) const {
  std::lock_guard lock(mutex_);
  for (const auto& entry : entries_) {
    if (entry.claim.intent.claim_kind == kind &&
        entry.claim.intent.owner_transaction_id == owner_transaction_id &&
        !entry.release.has_value() && !entry.reconcile_settlement.has_value()) {
      return entry.claim;
    }
  }
  return std::nullopt;
}

std::vector<CellAdmissionClaimDTO> KernelCellAdmissionOwner::held_claims(
    const CellAdmissionClaimKindDTO kind) const {
  std::lock_guard lock(mutex_);
  std::vector<CellAdmissionClaimDTO> claims;
  claims.reserve(held_claim_count_);
  for (const auto& entry : entries_) {
    if (entry.claim.intent.claim_kind == kind && !entry.release && !entry.reconcile_settlement)
      claims.push_back(entry.claim);
  }
  std::sort(claims.begin(), claims.end(),
            [](const auto& a, const auto& b) { return a.claim_generation < b.claim_generation; });
  return claims;
}

Result<std::optional<CellAdmissionReconcileDTO>>
KernelCellAdmissionOwner::manual_owner_reconciliation_request() const {
  using Request = std::optional<CellAdmissionReconcileDTO>;
  std::lock_guard lock(mutex_);
  constexpr auto role = CellAdmissionOwnerRoleDTO::kManualOperationOwner;
  const auto owner = std::find_if(current_owners_.begin(), current_owners_.end(),
                                  [](const auto& row) { return row.owner_role == role; });
  if (owner == current_owners_.end() || owner->owner_epoch == kernel_owner_epoch_)
    return Result<Request>::Ok(std::nullopt);
  const auto digest =
      CellAdmissionOwnerEpochReconciliationEvidenceDigest(role, kernel_owner_epoch_);
  if (!digest.ok()) return Result<Request>::Err(digest.error());
  CellAdmissionReconcileDTO request{kernel_owner_epoch_, {}, current_owners_, digest.value()};
  for (auto& row : request.current_owner_epochs)
    if (row.owner_role == role) row.owner_epoch = kernel_owner_epoch_;
  for (const auto& entry : entries_)
    if (!entry.release.has_value() && !entry.reconcile_settlement.has_value())
      request.retained_claims.push_back(entry.claim);
  std::sort(request.retained_claims.begin(), request.retained_claims.end(),
            [](const auto& left, const auto& right) {
              return left.claim_generation < right.claim_generation;
            });
  return Result<Request>::Ok(std::move(request));
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReconcileCellAdmissionClaims(
    const CellAdmissionReconcileDTO& reconcile) {
  std::lock_guard transition_lock(liveness_transition_mutex_);
  std::lock_guard lock(mutex_);
  const auto claims_canonical =
      std::is_sorted(reconcile.retained_claims.begin(), reconcile.retained_claims.end(),
                     [](const auto& left, const auto& right) {
                       return left.claim_generation < right.claim_generation;
                     }) &&
      std::adjacent_find(reconcile.retained_claims.begin(), reconcile.retained_claims.end(),
                         [](const auto& left, const auto& right) {
                           return left.claim_generation == right.claim_generation;
                         }) == reconcile.retained_claims.end();
  const auto epochs_canonical =
      std::is_sorted(reconcile.current_owner_epochs.begin(), reconcile.current_owner_epochs.end(),
                     [](const auto& left, const auto& right) {
                       return std::tie(left.owner_role, left.owner_epoch) <
                              std::tie(right.owner_role, right.owner_epoch);
                     }) &&
      std::adjacent_find(reconcile.current_owner_epochs.begin(),
                         reconcile.current_owner_epochs.end(),
                         [](const auto& left, const auto& right) {
                           return left.owner_role == right.owner_role;
                         }) == reconcile.current_owner_epochs.end();
  if (reconcile.kernel_owner_epoch != kernel_owner_epoch_ ||
      reconcile.retained_claims.size() > capacity_ ||
      reconcile.current_owner_epochs.size() > kMaximumCellAdmissionClaims || !claims_canonical ||
      !epochs_canonical ||
      !content_identity::is_sha256_digest(reconcile.reconciliation_evidence_digest.value()) ||
      !reconcile_validator_) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "reconcile_shape_invalid");
  }
  const auto replay =
      std::find_if(reconciliations_.begin(), reconciliations_.end(), [&](const auto& recorded) {
        return recorded.first.reconciliation_evidence_digest ==
               reconcile.reconciliation_evidence_digest;
      });
  if (replay != reconciliations_.end()) {
    if (replay->first != reconcile) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "reconcile_replay_conflict");
    }
    // This exact receipt was installed only after its durable commit. Reading
    // it neither settles today's work nor republishes predecessor ownership.
    // Do not revalidate changed resource facts or rewrite the same journal row.
    return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
  }
  std::vector<CellAdmissionClaimDTO> held;
  for (const auto& entry : entries_) {
    if (!entry.release.has_value() && !entry.reconcile_settlement.has_value()) {
      held.push_back(entry.claim);
    }
  }
  std::sort(held.begin(), held.end(), [](const auto& left, const auto& right) {
    return left.claim_generation < right.claim_generation;
  });
  if (held != reconcile.retained_claims) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "reconcile_inventory_not_exact");
  }
  auto validation = reconcile_validator_(held, current_owners_, reconcile);
  if (!validation.ok()) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "reconcile_evidence_not_exact");
  }
  if (validation.value().current_owner_epochs != reconcile.current_owner_epochs) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                    "reconcile_current_owner_not_authorized");
  }
  const auto& authorizations = validation.value().settlements;
  const auto authorizations_canonical =
      authorizations.size() <= held.size() &&
      std::is_sorted(authorizations.begin(), authorizations.end(),
                     [](const auto& left, const auto& right) {
                       return left.predecessor_claim.claim_generation <
                              right.predecessor_claim.claim_generation;
                     }) &&
      std::adjacent_find(authorizations.begin(), authorizations.end(),
                         [](const auto& left, const auto& right) {
                           return left.predecessor_claim.claim_generation ==
                                  right.predecessor_claim.claim_generation;
                         }) == authorizations.end();
  if (!authorizations_canonical) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                    "reconcile_authorization_not_exact");
  }
  for (const auto& authorization : authorizations) {
    const auto predecessor = std::find(held.begin(), held.end(), authorization.predecessor_claim);
    const auto successor =
        std::find(validation.value().current_owner_epochs.begin(),
                  validation.value().current_owner_epochs.end(),
                  CellAdmissionCurrentOwnerEpochDTO{authorization.successor_owner_role,
                                                    authorization.successor_owner_epoch});
    if (predecessor == held.end() ||
        authorization.successor_owner_role != authorization.predecessor_claim.intent.owner_role ||
        authorization.successor_owner_epoch == authorization.predecessor_claim.intent.owner_epoch ||
        successor == validation.value().current_owner_epochs.end() ||
        authorization.settlement_predicate != CellAdmissionSettlementKindDTO::kReconciled ||
        authorization.reconciliation_evidence_digest != reconcile.reconciliation_evidence_digest) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "reconcile_authorization_not_exact");
    }
  }
  for (const auto& authorization : authorizations) {
    const auto& parent = authorization.predecessor_claim;
    if (parent.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle &&
        HasUnsettledChildrenLocked(parent.claim_generation, authorizations)) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "reconcile_parent_child_unsettled");
    }
  }
  for (const auto& predecessor : held) {
    const auto successor = std::find_if(
        validation.value().current_owner_epochs.begin(),
        validation.value().current_owner_epochs.end(),
        [&](const auto& owner) { return owner.owner_role == predecessor.intent.owner_role; });
    if (successor == validation.value().current_owner_epochs.end()) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "reconcile_current_owner_not_authorized");
    }
    if (successor->owner_epoch == predecessor.intent.owner_epoch) {
      continue;
    }
    const bool settled =
        std::any_of(authorizations.begin(), authorizations.end(), [&](const auto& authorization) {
          return authorization.predecessor_claim == predecessor &&
                 authorization.successor_owner_role == successor->owner_role &&
                 authorization.successor_owner_epoch == successor->owner_epoch;
        });
    if (!settled) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "reconcile_predecessor_unsettled");
    }
  }
  const bool changed =
      !authorizations.empty() || current_owners_ != validation.value().current_owner_epochs;
  if (changed && generation_.value() >= 9'007'199'254'740'991ULL) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "admission_generation_exhausted");
  }
  const AdmissionGeneration next(generation_.value() + (changed ? 1U : 0U));
  std::vector<CellAdmissionRecoveryLedgerEntryDTO> footprint_entries;
  footprint_entries.reserve(entries_.size());
  for (const auto& entry : entries_) {
    CellAdmissionRecoveryLedgerEntryDTO prospective{entry.claim, entry.release,
                                                    entry.reconcile_settlement};
    const auto authorization = std::find_if(
        authorizations.begin(), authorizations.end(),
        [&](const auto& candidate) { return candidate.predecessor_claim == entry.claim; });
    if (authorization != authorizations.end()) {
      prospective.reconcile_settlement = *authorization;
    }
    const auto current = std::find_if(
        validation.value().current_owner_epochs.begin(),
        validation.value().current_owner_epochs.end(),
        [&](const auto& owner) { return owner.owner_role == entry.claim.intent.owner_role; });
    const bool released_obsolete = prospective.release.has_value() &&
                                   current != validation.value().current_owner_epochs.end() &&
                                   current->owner_epoch != prospective.claim.intent.owner_epoch;
    const bool reconciled_obsolete =
        prospective.reconcile_settlement.has_value() &&
        current != validation.value().current_owner_epochs.end() &&
        prospective.reconcile_settlement->successor_owner_epoch != current->owner_epoch;
    if (!released_obsolete && !reconciled_obsolete) {
      footprint_entries.push_back(std::move(prospective));
    }
  }
  const auto footprint = ComputeCellAdmissionRecoveryFootprint(
      kernel_owner_epoch_, next, footprint_entries, validation.value().current_owner_epochs);
  if (!footprint.ok() || !footprint.value().admitted()) {
    return Reject<AuthoringAdmissionDTO>("invalid_request", "recovery_footprint_exceeded");
  }
  auto recorded = journal_.RecordCellAdmissionClaimsReconciled(kernel_owner_epoch_, reconcile,
                                                               validation.value(), next);
  if (!recorded.ok()) {
    return Result<AuthoringAdmissionDTO>::Err(recorded.error());
  }
  for (auto& entry : entries_) {
    const auto authorization = std::find_if(
        authorizations.begin(), authorizations.end(),
        [&](const auto& candidate) { return candidate.predecessor_claim == entry.claim; });
    if (authorization != authorizations.end()) {
      entry.reconcile_settlement = *authorization;
    }
  }
  held_claim_count_ -= authorizations.size();
  current_owners_ = validation.value().current_owner_epochs;
  entries_.erase(
      std::remove_if(
          entries_.begin(), entries_.end(),
          [&](const auto& entry) {
            const auto current = std::find_if(
                current_owners_.begin(), current_owners_.end(), [&](const auto& owner) {
                  return owner.owner_role == entry.claim.intent.owner_role;
                });
            const bool released_obsolete = entry.release.has_value() &&
                                           current != current_owners_.end() &&
                                           current->owner_epoch != entry.claim.intent.owner_epoch;
            const bool reconciled_obsolete =
                entry.reconcile_settlement.has_value() && current != current_owners_.end() &&
                entry.reconcile_settlement->successor_owner_role == current->owner_role &&
                entry.reconcile_settlement->successor_owner_epoch != current->owner_epoch;
            return released_obsolete || reconciled_obsolete;
          }),
      entries_.end());
  membership_.erase(
      std::remove_if(membership_.begin(), membership_.end(),
                     [&](const auto& member) {
                       const auto current = std::find_if(
                           current_owners_.begin(), current_owners_.end(), [&](const auto& owner) {
                             return owner.owner_role == member.owner_role;
                           });
                       const bool unresolved =
                           std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
                             return !entry.release.has_value() &&
                                    !entry.reconcile_settlement.has_value() &&
                                    entry.claim.intent.owner_role == member.owner_role &&
                                    entry.claim.intent.owner_epoch == member.owner_epoch;
                           });
                       const bool reconciled =
                           std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
                             return entry.reconcile_settlement.has_value() &&
                                    entry.claim.intent.owner_role == member.owner_role &&
                                    entry.claim.intent.owner_epoch == member.owner_epoch;
                           });
                       return current != current_owners_.end() &&
                              current->owner_epoch != member.owner_epoch && !unresolved &&
                              !reconciled;
                     }),
      membership_.end());
  reconciliations_.erase(
      std::remove_if(reconciliations_.begin(), reconciliations_.end(),
                     [&](const auto& recorded) {
                       return std::none_of(
                           recorded.second.settlements.begin(), recorded.second.settlements.end(),
                           [&](const auto& settlement) {
                             return std::any_of(entries_.begin(), entries_.end(),
                                                [&](const auto& entry) {
                                                  return entry.reconcile_settlement.has_value() &&
                                                         *entry.reconcile_settlement == settlement;
                                                });
                           });
                     }),
      reconciliations_.end());
  generation_ = next;
  reconciliations_.emplace_back(reconcile, validation.value());
  return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
}

Result<CellAdmissionRecoveryReceiptDTO> KernelCellAdmissionOwner::GetCellAdmissionRecoveryReceipt(
    const CellAdmissionRecoveryReceiptQueryDTO& query) const {
  auto valid = ValidateCellAdmissionRecoveryReceiptQuery(query);
  if (!valid.ok()) {
    return Result<CellAdmissionRecoveryReceiptDTO>::Err(valid.error());
  }
  std::lock_guard lock(mutex_);
  CellAdmissionRecoveryReceiptDTO receipt{
      .status = CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
      .kernel_owner_epoch = kernel_owner_epoch_,
      .admission_generation = generation_,
      .matched_claim = std::nullopt,
      .matched_release = std::nullopt,
      .matched_reconcile_settlement = std::nullopt,
      .held_claims = {},
      .current_owner_epochs = {},
  };
  receipt.current_owner_epochs = current_owners_;
  for (const auto& entry : entries_) {
    if (!entry.release.has_value() && !entry.reconcile_settlement.has_value()) {
      receipt.held_claims.push_back(entry.claim);
    }
  }
  std::sort(receipt.held_claims.begin(), receipt.held_claims.end(),
            [](const auto& left, const auto& right) {
              return left.claim_generation < right.claim_generation;
            });

  const auto digest = CellAdmissionClaimIntentDigest(query.intent);
  const auto member = std::find_if(membership_.begin(), membership_.end(), [&](const auto& row) {
    return row.owner_epoch == query.intent.owner_epoch && row.request_id == query.intent.request_id;
  });
  if (member == membership_.end()) {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kUnknown;
    return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
  }
  if (!digest.ok() || member->owner_role != query.intent.owner_role ||
      member->semantic_intent_digest != digest.value()) {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kConflicting;
    return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
  }
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& row) {
    return row.claim.intent.owner_epoch == query.intent.owner_epoch &&
           row.claim.intent.request_id == query.intent.request_id;
  });
  if (entry == entries_.end()) {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kUnknown;
    return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
  }
  if (entry->claim.intent != query.intent ||
      (query.expected_claim.has_value() && *query.expected_claim != entry->claim) ||
      (query.expected_release.has_value() && entry->release.has_value() &&
       *query.expected_release != *entry->release)) {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kConflicting;
    return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
  }
  receipt.matched_claim = entry->claim;
  if (entry->reconcile_settlement.has_value()) {
    if (query.expected_release.has_value()) {
      receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kConflicting;
      receipt.matched_claim.reset();
      return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
    }
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact;
    receipt.matched_reconcile_settlement = entry->reconcile_settlement;
  } else if (entry->release.has_value()) {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact;
    receipt.matched_release = entry->release;
  } else {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
  }
  return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
}

Result<void> KernelCellAdmissionOwner::ValidateProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization) const {
  auto proof = PrepareProgramResetSceneAuthorization(authorization);
  if (!proof.ok()) return Result<void>::Err(proof.error());
  return ValidatePreparedProgramResetSceneAuthorization(authorization, proof.value());
}

Result<void> KernelCellAdmissionOwner::ValidatePreparedProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization,
    const PreparedProgramResetStatus& proof) const {
  const auto valid = manyforge::core::ValidateProgramResetSceneAuthorization(authorization);
  if (!valid.ok()) return Result<void>::Err(valid.error());
  std::lock_guard lock(mutex_);
  if (!readiness_.program_cycle_status_current || !readiness_.program_cycle_status.has_value()) {
    return AuthoringNotReady<void>(generation_, "program_reset_status_not_current");
  }
  if (authorization.reset_claim) {
    const auto& expected = *authorization.reset_claim;
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& row) {
      return row.claim.claim_generation == expected.reset_claim_generation;
    });
    if (entry == entries_.end() || entry->release || entry->reconcile_settlement || held_claim_count_ != 1U ||
        entry->claim.intent.claim_kind != CellAdmissionClaimKindDTO::kProgramReset ||
        entry->claim.intent.owner_role != CellAdmissionOwnerRoleDTO::kProgramRunner) {
      return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
    }
    const auto current = CurrentResetLocked(ProgramResetJournalReadPurpose::kReloadInitialScene,
                                                entry->claim.intent, entry->claim, proof);
    if (!current.ok()) return Result<void>::Err(current.error());
    const auto& status = current.value();
    const auto& accepted = *status.accepted_reset;
    const auto identity = ProgramResetClaimRequestId(status);
    if (!accepted.reload_initial_scene ||
        status.program_runner_owner_epoch != authorization.program_runner_owner_epoch ||
        status.cycle_generation != expected.cycle_generation ||
        accepted.request_owner_epoch != expected.request_owner_epoch ||
        accepted.request_id != expected.request_id ||
        accepted.request_payload_digest != expected.request_payload_digest ||
        accepted.reset_claim_generation != expected.reset_claim_generation ||
        !identity.ok() || entry->claim.intent.owner_epoch != status.program_runner_owner_epoch ||
        entry->claim.intent.request_id != identity.value() ||
        entry->claim.intent.owner_transaction_id != identity.value().value() ||
        entry->claim.intent.request_payload_digest != accepted.request_payload_digest) {
      return AuthoringNotReady<void>(generation_, "program_reset_status_mismatch");
    }
    return Result<void>::Ok();
  }
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& candidate) {
    return candidate.claim.claim_generation == authorization.cycle_claim_generation;
  });
  if (entry == entries_.end() || entry->release.has_value() ||
      entry->claim.intent.claim_kind != CellAdmissionClaimKindDTO::kProgramCycle ||
      entry->claim.intent.owner_role != CellAdmissionOwnerRoleDTO::kProgramRunner ||
      entry->claim.intent.owner_transaction_id != authorization.program_run_id.value()) {
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  }
  const auto current = CurrentResetLocked(ProgramResetJournalReadPurpose::kReloadInitialScene,
                                         entry->claim.intent, entry->claim, proof);
  if (!current.ok()) return Result<void>::Err(current.error());
  const auto& status = current.value();
  if (!status.accepted_reset->reload_initial_scene) {
    return AuthoringNotReady<void>(generation_, "program_reset_scene_not_selected");
  }
  if (status.program_runner_owner_epoch != authorization.program_runner_owner_epoch ||
      status.program_run_id != std::optional<ProgramRunId>{authorization.program_run_id} ||
      status.cycle_claim_generation !=
          std::optional<CellAdmissionClaimGeneration>{authorization.cycle_claim_generation} ||
      status.terminal_or_settlement_reference !=
          std::optional<ContentDigest>{authorization.terminal_or_settlement_reference}) {
    return AuthoringNotReady<void>(generation_, "program_reset_status_mismatch");
  }
  const bool held_by_current_owner =
      !entry->reconcile_settlement.has_value() &&
      entry->claim.intent.owner_epoch == authorization.program_runner_owner_epoch;
  const bool reconciled_to_current_owner =
      entry->reconcile_settlement.has_value() &&
      entry->reconcile_settlement->predecessor_claim == entry->claim &&
      entry->reconcile_settlement->successor_owner_role ==
          CellAdmissionOwnerRoleDTO::kProgramRunner &&
      entry->reconcile_settlement->successor_owner_epoch ==
          authorization.program_runner_owner_epoch &&
      entry->reconcile_settlement->settlement_predicate ==
          CellAdmissionSettlementKindDTO::kReconciled;
  if (!held_by_current_owner && !reconciled_to_current_owner) {
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_current");
  }
  if (HasUnsettledChildrenLocked(entry->claim.claim_generation)) {
    return AuthoringNotReady<void>(generation_, "program_reset_child_unsettled");
  }
  return Result<void>::Ok();
}

bool KernelCellAdmissionOwner::has_unresolved_claims() const {
  std::lock_guard lock(mutex_);
  return held_claim_count_ != 0U;
}

bool KernelCellAdmissionOwner::has_unresolved_claims_except(
    CellAdmissionOwnerRoleDTO owner_role) const {
  std::lock_guard lock(mutex_);
  return std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return !entry.release && !entry.reconcile_settlement &&
           entry.claim.intent.owner_role != owner_role;
  });
}

}  // namespace manyforge::core
