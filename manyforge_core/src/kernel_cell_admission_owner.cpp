#include <algorithm>
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
[[nodiscard]] Result<Value> AuthoringNotReady(AdmissionGeneration generation,
                                              std::string reason) {
  return Result<Value>::Err(ErrorDTO{
      "authoring_not_ready",
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

[[nodiscard]] bool SettlementKindCompatible(
    CellAdmissionClaimKindDTO claim, CellAdmissionSettlementKindDTO settlement) noexcept {
  switch (claim) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
    case CellAdmissionClaimKindDTO::kManualOperation:
      return settlement == CellAdmissionSettlementKindDTO::kExplicitReset;
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return settlement == CellAdmissionSettlementKindDTO::kOwnerCommit ||
             settlement == CellAdmissionSettlementKindDTO::kProvenNoEffect ||
             settlement == CellAdmissionSettlementKindDTO::kReconciled;
    case CellAdmissionClaimKindDTO::kManagerApply:
    case CellAdmissionClaimKindDTO::kComponentLifecycle:
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
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
    std::vector<CellAdmissionCurrentOwnerEpochDTO> recovered_current_owners)
    : kernel_owner_epoch_(std::move(kernel_owner_epoch)),
      capacity_(capacity),
      clock_(clock),
      journal_(journal),
      intervention_validator_(std::move(intervention_validator)),
      settlement_validator_(std::move(settlement_validator)),
      reconcile_validator_(std::move(reconcile_validator)),
      membership_(std::move(recovered_membership)),
      current_owners_(std::move(recovered_current_owners)) {
  if (kernel_owner_epoch_.empty() || capacity_ == 0U ||
      capacity_ > kMaximumCellAdmissionClaims || recovery.size() > capacity_ ||
      membership_.size() > capacity_ ||
      current_owners_.size() > kMaximumCellAdmissionClaims) {
    throw std::invalid_argument("Kernel cell-admission owner configuration is invalid");
  }
  const auto owners_canonical =
      std::is_sorted(current_owners_.begin(), current_owners_.end(),
                     [](const auto& left, const auto& right) {
                       return left.owner_role < right.owner_role;
                     }) &&
      std::adjacent_find(current_owners_.begin(), current_owners_.end(),
                         [](const auto& left, const auto& right) {
                           return left.owner_role == right.owner_role;
                         }) == current_owners_.end() &&
      std::none_of(current_owners_.begin(), current_owners_.end(),
                   [](const auto& owner) { return owner.owner_epoch.empty(); });
  if (!owners_canonical) {
    throw std::runtime_error("durable cell-admission current-owner truth is invalid");
  }
  for (std::size_t index = 0; index < membership_.size(); ++index) {
    const auto& member = membership_[index];
    if (member.owner_epoch.empty() || member.request_id.empty() ||
        !content_identity::is_sha256_digest(member.semantic_intent_digest.value()) ||
        std::any_of(membership_.begin(), membership_.begin() + index,
                    [&](const auto& prior) {
                      return prior.owner_epoch == member.owner_epoch &&
                             prior.request_id == member.request_id;
                    })) {
      throw std::runtime_error("durable cell-admission membership is invalid");
    }
  }
  generation_ =
      AdmissionGeneration(std::max<std::uint64_t>(1U, recovered_generation_high_water.value()));
  entries_.reserve(capacity_);
  reconciliations_.reserve(capacity_);
  for (auto& recovered : recovery) {
    if (!ValidateCellAdmissionClaimIntent(recovered.claim.intent).ok() ||
        !recovered.claim.claim_generation.nonzero() ||
        !recovered.claim.acquired_admission_generation.nonzero() ||
        recovered.claim.claim_generation.value() > 9'007'199'254'740'991ULL ||
        recovered.claim.acquired_admission_generation.value() >
            9'007'199'254'740'989ULL ||
        recovered.claim.accepted_at_ns < 0 ||
        (recovered.release.has_value() &&
         (!ValidateCellAdmissionClaimRelease(*recovered.release).ok() ||
          !SameReleaseIdentity(recovered.claim, *recovered.release)))) {
      throw std::runtime_error("durable cell-admission recovery is invalid");
    }
    generation_ = AdmissionGeneration(std::max(
        generation_.value(), recovered.claim.acquired_admission_generation.value() +
                                 (recovered.release.has_value() ? 2U : 1U)));
    next_claim_generation_ =
        CellAdmissionClaimGeneration(std::max(next_claim_generation_.value(),
                                              recovered.claim.claim_generation.value() + 1U));
    entries_.push_back(Entry{std::move(recovered.claim), std::move(recovered.release)});
    if (!entries_.back().release.has_value()) {
      ++held_claim_count_;
    }
  }
  for (const auto& entry : entries_) {
    const auto digest = CellAdmissionClaimIntentDigest(entry.claim.intent);
    if (!digest.ok()) {
      throw std::runtime_error("durable cell-admission membership is invalid");
    }
    const auto found = std::find_if(
        membership_.begin(), membership_.end(), [&](const auto& member) {
          return member.owner_epoch == entry.claim.intent.owner_epoch &&
                 member.request_id == entry.claim.intent.request_id;
        });
    if (found == membership_.end()) {
      membership_.push_back(
          {entry.claim.intent.owner_role, entry.claim.intent.owner_epoch,
           entry.claim.intent.request_id, digest.value()});
    } else if (found->owner_role != entry.claim.intent.owner_role ||
               found->semantic_intent_digest != digest.value()) {
      throw std::runtime_error("durable cell-admission membership conflicts with claim truth");
    }
  }
  if (membership_.size() > capacity_) {
    throw std::runtime_error("durable cell-admission membership exceeds its bound");
  }
}

bool KernelCellAdmissionOwner::BaseReadinessLocked() const noexcept {
  return readiness_.program_cycle_status_current &&
         readiness_.program_cycle_status.has_value() &&
         readiness_.program_cycle_status->state == ProgramCycleStateDTO::kIdleAfterReset &&
         readiness_.manager_status_current && readiness_.manager_current &&
         readiness_.required_component_status_current &&
         readiness_.required_component_total ==
             readiness_.required_component_current_compatible_ready_converged &&
         readiness_.deployment_status_current && readiness_.deployment_converged &&
         readiness_.blocking_fault_count == 0U && readiness_.unresolved_owner_count == 0U &&
         readiness_.unsettled_effect_count == 0U;
}

AuthoringAdmissionDTO KernelCellAdmissionOwner::AdmissionLocked() const noexcept {
  return AuthoringAdmissionDTO{BaseReadinessLocked() && held_claim_count_ == 0U, generation_};
}

AuthoringAdmissionDTO KernelCellAdmissionOwner::GetAuthoringAdmission() const {
  std::lock_guard lock(mutex_);
  return AdmissionLocked();
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::UpdateCellAdmissionReadiness(
    const CellAdmissionReadinessSnapshotDTO& snapshot) {
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
  std::lock_guard lock(mutex_);
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

const KernelCellAdmissionOwner::Entry* KernelCellAdmissionOwner::FindReplayLocked(
    const CellAdmissionClaimIntentDTO& intent) const {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const auto& entry) {
                                    return SameRequest(entry.claim.intent, intent);
                                  });
  return found == entries_.end() ? nullptr : &*found;
}

KernelCellAdmissionOwner::Entry* KernelCellAdmissionOwner::FindHeldLocked(
    CellAdmissionClaimGeneration generation) {
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return entry.claim.claim_generation == generation && !entry.release.has_value();
  });
  return found == entries_.end() ? nullptr : &*found;
}

const KernelCellAdmissionOwner::Entry* KernelCellAdmissionOwner::ParentCycleLocked(
    const CellAdmissionClaimIntentDTO& intent) const {
  if (!intent.intervention_evidence.has_value() ||
      !intent.intervention_evidence->parent_program_cycle_claim_generation.has_value()) {
    return nullptr;
  }
  const auto generation =
      *intent.intervention_evidence->parent_program_cycle_claim_generation;
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return !entry.release.has_value() &&
           entry.claim.claim_generation == generation &&
           entry.claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle;
  });
  return found == entries_.end() ? nullptr : &*found;
}

bool KernelCellAdmissionOwner::IsStaleOwnerLocked(
    const CellAdmissionClaimIntentDTO& intent) const {
  const auto current =
      std::find_if(current_owners_.begin(), current_owners_.end(), [&](const auto& owner) {
        return owner.owner_role == intent.owner_role;
      });
  return current != current_owners_.end() && current->owner_epoch != intent.owner_epoch;
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent) {
  auto valid = ValidateCellAdmissionClaimIntent(intent);
  if (!valid.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(valid.error());
  }
  std::lock_guard lock(mutex_);
  if (IsStaleOwnerLocked(intent)) {
    return Reject<CellAdmissionClaimDTO>("stale_request_owner_epoch",
                                         "request_owner_epoch_stale");
  }
  if (const auto* replay = FindReplayLocked(intent); replay != nullptr) {
    if (replay->claim.intent != intent) {
      return Reject<CellAdmissionClaimDTO>("request_id_conflict", "request_reuse_conflict");
    }
    if (replay->release.has_value()) {
      return Reject<CellAdmissionClaimDTO>("request_result_expired",
                                           "request_result_compacted");
    }
    return Result<CellAdmissionClaimDTO>::Ok(replay->claim);
  }
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
    return Reject<CellAdmissionClaimDTO>("request_result_expired",
                                         "request_result_compacted");
  }
  if (intent.expected_admission_generation != generation_) {
    return Reject<CellAdmissionClaimDTO>("stale_generation", "expected_generation_stale");
  }
  if (membership_.size() >= capacity_) {
    return Reject<CellAdmissionClaimDTO>("request_ledger_full", "claim_capacity_exhausted");
  }

  const auto* parent = ParentCycleLocked(intent);
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery) {
    const bool compatible = held_claim_count_ == 0U ||
                            (held_claim_count_ == 1U && parent != nullptr);
    if (!compatible || !intervention_validator_) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                       "intervention_offer_not_exact");
    }
    auto evidence = intervention_validator_(
        intent, parent == nullptr ? std::nullopt
                                  : std::optional<CellAdmissionClaimDTO>(parent->claim));
    if (!evidence.ok()) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_,
                                                       "intervention_offer_not_current");
    }
  } else if (!AdmissionLocked().authoring_allowed) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "positive_admission_required");
  }

  CellAdmissionClaimDTO claim{intent, next_claim_generation_, generation_, clock_.now()};
  if (claim.accepted_at_ns < 0 ||
      generation_.value() >= 9'007'199'254'740'991ULL ||
      next_claim_generation_.value() >= 9'007'199'254'740'991ULL) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "acceptance_time_invalid");
  }
  auto recorded = journal_.RecordCellAdmissionClaimAcquired(kernel_owner_epoch_, claim);
  if (!recorded.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(recorded.error());
  }
  entries_.push_back(Entry{claim, std::nullopt});
  membership_.push_back({intent.owner_role, intent.owner_epoch, intent.request_id,
                         intent_digest.value()});
  if (std::none_of(current_owners_.begin(), current_owners_.end(), [&](const auto& owner) {
        return owner.owner_role == intent.owner_role;
      })) {
    current_owners_.push_back({intent.owner_role, intent.owner_epoch});
    std::sort(current_owners_.begin(), current_owners_.end(),
              [](const auto& left, const auto& right) {
                return left.owner_role < right.owner_role;
              });
  }
  ++held_claim_count_;
  next_claim_generation_ =
      CellAdmissionClaimGeneration(next_claim_generation_.value() + 1U);
  generation_ = AdmissionGeneration(generation_.value() + 1U);
  return Result<CellAdmissionClaimDTO>::Ok(std::move(claim));
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReleaseCellAdmissionClaim(
    const CellAdmissionClaimReleaseDTO& release) {
  auto valid = ValidateCellAdmissionClaimRelease(release);
  if (!valid.ok()) {
    return Result<AuthoringAdmissionDTO>::Err(valid.error());
  }
  std::lock_guard lock(mutex_);
  const auto current_owner =
      std::find_if(current_owners_.begin(), current_owners_.end(), [&](const auto& owner) {
        return owner.owner_role == release.owner_role;
      });
  if (current_owner != current_owners_.end() &&
      current_owner->owner_epoch != release.owner_epoch) {
    return Reject<AuthoringAdmissionDTO>("stale_generation",
                                         "claim_owner_epoch_stale");
  }
  auto* entry = FindHeldLocked(release.claim_generation);
  if (entry == nullptr || !SameReleaseIdentity(entry->claim, release) ||
      !SettlementKindCompatible(entry->claim.intent.claim_kind,
                                release.settlement.settlement_kind)) {
    return Reject<AuthoringAdmissionDTO>("stale_generation", "claim_release_not_exact");
  }
  if (entry->claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle &&
      std::any_of(entries_.begin(), entries_.end(), [&](const auto& candidate) {
        return !candidate.release.has_value() &&
               candidate.claim.intent.claim_kind ==
                   CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
               candidate.claim.intent.intervention_evidence.has_value() &&
               candidate.claim.intent.intervention_evidence
                       ->parent_program_cycle_claim_generation ==
                   entry->claim.claim_generation;
      })) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(
        generation_, "parent_cycle_intervention_unsettled");
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
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                     "admission_generation_exhausted");
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

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReconcileCellAdmissionClaims(
    const CellAdmissionReconcileDTO& reconcile) {
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
      std::is_sorted(reconcile.current_owner_epochs.begin(),
                     reconcile.current_owner_epochs.end(),
                     [](const auto& left, const auto& right) {
                       return std::tie(left.owner_role, left.owner_epoch) <
                              std::tie(right.owner_role, right.owner_epoch);
                     }) &&
      std::adjacent_find(reconcile.current_owner_epochs.begin(),
                         reconcile.current_owner_epochs.end(),
                         [](const auto& left, const auto& right) {
                           return left.owner_role == right.owner_role;
                         }) ==
          reconcile.current_owner_epochs.end();
  if (reconcile.kernel_owner_epoch != kernel_owner_epoch_ ||
      reconcile.retained_claims.size() > capacity_ ||
      reconcile.current_owner_epochs.size() > kMaximumCellAdmissionClaims ||
      !claims_canonical || !epochs_canonical ||
      !content_identity::is_sha256_digest(reconcile.reconciliation_evidence_digest.value()) ||
      !reconcile_validator_) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "reconcile_shape_invalid");
  }
  const auto replay = std::find_if(
      reconciliations_.begin(), reconciliations_.end(), [&](const auto& recorded) {
        return recorded.first.reconciliation_evidence_digest ==
               reconcile.reconciliation_evidence_digest;
      });
  if (replay != reconciliations_.end()) {
    if (replay->first != reconcile) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                       "reconcile_replay_conflict");
    }
    auto replay_validation =
        reconcile_validator_(reconcile.retained_claims, reconcile);
    if (!replay_validation.ok() ||
        replay_validation.value() != replay->second) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                       "reconcile_replay_conflict");
    }
    auto recorded = journal_.RecordCellAdmissionClaimsReconciled(
        kernel_owner_epoch_, reconcile, replay_validation.value(), generation_);
    if (!recorded.ok()) {
      return Result<AuthoringAdmissionDTO>::Err(recorded.error());
    }
    return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
  }
  if (reconciliations_.size() >= capacity_) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                     "reconcile_history_full");
  }
  std::vector<CellAdmissionClaimDTO> held;
  for (const auto& entry : entries_) {
    if (!entry.release.has_value()) {
      held.push_back(entry.claim);
    }
  }
  std::sort(held.begin(), held.end(), [](const auto& left, const auto& right) {
    return left.claim_generation < right.claim_generation;
  });
  if (held != reconcile.retained_claims) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                     "reconcile_inventory_not_exact");
  }
  auto validation = reconcile_validator_(held, reconcile);
  if (!validation.ok()) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                     "reconcile_evidence_not_exact");
  }
  if (validation.value().current_owner_epochs != reconcile.current_owner_epochs) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(
        generation_, "reconcile_current_owner_not_authorized");
  }
  const auto& authorizations = validation.value().settlements;
  const auto authorizations_canonical =
      authorizations.size() <= held.size() &&
      std::is_sorted(authorizations.begin(), authorizations.end(),
                     [](const auto& left, const auto& right) {
                       return left.predecessor_claim.claim_generation <
                              right.predecessor_claim.claim_generation;
                     }) &&
      std::adjacent_find(
          authorizations.begin(), authorizations.end(),
          [](const auto& left, const auto& right) {
            return left.predecessor_claim.claim_generation ==
                   right.predecessor_claim.claim_generation;
          }) == authorizations.end();
  if (!authorizations_canonical) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(
        generation_, "reconcile_authorization_not_exact");
  }
  for (const auto& authorization : authorizations) {
    const auto predecessor =
        std::find(held.begin(), held.end(), authorization.predecessor_claim);
    const auto successor = std::find(
        validation.value().current_owner_epochs.begin(),
        validation.value().current_owner_epochs.end(),
        CellAdmissionCurrentOwnerEpochDTO{authorization.successor_owner_role,
                                          authorization.successor_owner_epoch});
    if (predecessor == held.end() ||
        authorization.successor_owner_role !=
            authorization.predecessor_claim.intent.owner_role ||
        authorization.successor_owner_epoch ==
            authorization.predecessor_claim.intent.owner_epoch ||
        successor == validation.value().current_owner_epochs.end() ||
        authorization.settlement_predicate !=
            CellAdmissionSettlementKindDTO::kReconciled ||
        authorization.reconciliation_evidence_digest !=
            reconcile.reconciliation_evidence_digest) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(
          generation_, "reconcile_authorization_not_exact");
    }
  }
  for (const auto& predecessor : held) {
    const auto successor = std::find_if(
        validation.value().current_owner_epochs.begin(),
        validation.value().current_owner_epochs.end(), [&](const auto& owner) {
          return owner.owner_role == predecessor.intent.owner_role;
        });
    if (successor == validation.value().current_owner_epochs.end()) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(
          generation_, "reconcile_current_owner_not_authorized");
    }
    if (successor->owner_epoch == predecessor.intent.owner_epoch) {
      continue;
    }
    const bool settled = std::any_of(
        authorizations.begin(), authorizations.end(), [&](const auto& authorization) {
          return authorization.predecessor_claim == predecessor &&
                 authorization.successor_owner_role == successor->owner_role &&
                 authorization.successor_owner_epoch == successor->owner_epoch;
        });
    if (!settled) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(
          generation_, "reconcile_predecessor_unsettled");
    }
  }
  const bool changed = !authorizations.empty() ||
                       current_owners_ != validation.value().current_owner_epochs;
  if (changed && generation_.value() >= 9'007'199'254'740'991ULL) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                     "admission_generation_exhausted");
  }
  const AdmissionGeneration next(generation_.value() + (changed ? 1U : 0U));
  auto recorded = journal_.RecordCellAdmissionClaimsReconciled(
      kernel_owner_epoch_, reconcile, validation.value(), next);
  if (!recorded.ok()) {
    return Result<AuthoringAdmissionDTO>::Err(recorded.error());
  }
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return std::any_of(authorizations.begin(), authorizations.end(),
                           [&](const auto& authorization) {
                             return authorization.predecessor_claim == entry.claim;
                           });
      }),
      entries_.end());
  held_claim_count_ -= authorizations.size();
  current_owners_ = validation.value().current_owner_epochs;
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        const auto current = std::find_if(
            current_owners_.begin(), current_owners_.end(), [&](const auto& owner) {
              return owner.owner_role == entry.claim.intent.owner_role;
            });
        return entry.release.has_value() && current != current_owners_.end() &&
               current->owner_epoch != entry.claim.intent.owner_epoch;
      }),
      entries_.end());
  membership_.erase(
      std::remove_if(membership_.begin(), membership_.end(), [&](const auto& member) {
        const auto current = std::find_if(
            current_owners_.begin(), current_owners_.end(), [&](const auto& owner) {
              return owner.owner_role == member.owner_role;
            });
        const bool unresolved = std::any_of(
            entries_.begin(), entries_.end(), [&](const auto& entry) {
              return !entry.release.has_value() &&
                     entry.claim.intent.owner_role == member.owner_role &&
                     entry.claim.intent.owner_epoch == member.owner_epoch;
            });
        return current != current_owners_.end() &&
               current->owner_epoch != member.owner_epoch && !unresolved;
      }),
      membership_.end());
  generation_ = next;
  reconciliations_.emplace_back(reconcile, validation.value());
  return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
}

bool KernelCellAdmissionOwner::has_unresolved_claims() const {
  std::lock_guard lock(mutex_);
  return held_claim_count_ != 0U;
}

}  // namespace manyforge::core
