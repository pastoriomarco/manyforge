#include "admission_timing_internal.hpp"

#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace manyforge::core::detail {
namespace { thread_local AdmissionTimingData* admission_timing = nullptr; }
AdmissionTimingData* CurrentAdmissionTiming() noexcept { return admission_timing; }
void SetAdmissionTiming(AdmissionTimingData* data) noexcept { admission_timing = data; }
void CaptureAdmissionJournalTiming(const char* stage, std::int64_t elapsed_ns,
                                   std::int64_t rows, std::int64_t changes, const char* outcome,
                                   AdmissionJournalRowsKind rows_kind) noexcept {
  auto* data = CurrentAdmissionTiming();
  if (!data || elapsed_ns < 50'000'000) return;
  if (data->event_count == data->events.size()) { ++data->dropped; return; }
  data->events[data->event_count++] = {stage, elapsed_ns, rows, changes, outcome, rows_kind};
}
}  // namespace manyforge::core::detail

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
  return left.owner_role == right.owner_role && left.request_key == right.request_key;
}

// A release names its claim by the claim's own identity. Its request_key is
// the key of the request that causes the release (contract 1.15.2), so it is
// recorded but never compared with the claim's key.
[[nodiscard]] bool SameReleaseIdentity(const CellAdmissionClaimDTO& claim,
                                       const CellAdmissionClaimReleaseDTO& release) {
  return claim.intent.owner_role == release.owner_role &&
         claim.intent.owner_transaction_id == release.owner_transaction_id &&
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
             settlement == CellAdmissionSettlementKindDTO::kProvenNoEffect;
    case CellAdmissionClaimKindDTO::kManagerApply:
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
    case CellAdmissionClaimKindDTO::kManualOperation:
      return settlement == CellAdmissionSettlementKindDTO::kTerminalEffectsSettled ||
             settlement == CellAdmissionSettlementKindDTO::kProvenNoEffect;
  }
  return false;
}

}  // namespace

KernelCellAdmissionOwner::KernelCellAdmissionOwner(
    std::size_t capacity, IClock& clock, CellAdmissionJournalPoster post_records,
    CellAdmissionInterventionValidator intervention_validator,
    CellAdmissionSettlementValidator settlement_validator,
    std::optional<ManagerLivenessRequiredSetDTO> manager_liveness_required_set,
    CellAdmissionProgramNodeValidator program_node_validator,
    std::function<Result<void>(std::shared_ptr<IIoSignalService>*)> required_health_loss_observer)
    : capacity_(capacity),
      clock_(clock),
      post_records_(std::move(post_records)),
      intervention_validator_(std::move(intervention_validator)),
      settlement_validator_(std::move(settlement_validator)),
      program_node_validator_(std::move(program_node_validator)),
      required_health_loss_observer_(std::move(required_health_loss_observer)),
      manager_liveness_required_set_(std::move(manager_liveness_required_set)) {
  if (capacity_ == 0U || capacity_ > kMaximumCellAdmissionClaims) {
    throw std::invalid_argument("Kernel cell-admission owner configuration is invalid");
  }
  if (manager_liveness_required_set_.has_value()) {
    if (!ValidateManagerLivenessRequiredSet(*manager_liveness_required_set_).ok()) {
      throw std::invalid_argument("Kernel Manager-liveness required set is invalid");
    }
    manager_liveness_rows_.reserve(manager_liveness_required_set_->required_receivers.size());
    for (const auto& receiver : manager_liveness_required_set_->required_receivers) {
      manager_liveness_rows_.push_back({receiver, 0U, false, false, 0U, false});
    }
  }
  entries_.reserve(capacity_);
  membership_.reserve(capacity_);
}

Result<KernelCellAdmissionOwner::ProgramBlackboardAdmissionGuard>
KernelCellAdmissionOwner::AcquireProgramBlackboardAdmission() const {
  std::unique_lock lock(mutex_);
  if (!manager_liveness_required_set_ ||
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
  if (!manager_liveness_required_set_ ||
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

bool KernelCellAdmissionOwner::ManualParentReadyLocked(const Entry* parent) const noexcept {
  const auto& cycle = readiness_.program_cycle_status;
  return parent && NonCycleReadinessLocked() && readiness_.program_cycle_status_current && cycle &&
         cycle->state == ProgramCycleStateDTO::kRunning && cycle->program_run_id &&
         parent->claim.intent.owner_transaction_id ==
             CellAdmissionOwnerTransactionId(*cycle->program_run_id) &&
         cycle->cycle_claim_generation == parent->claim.claim_generation;
}

Result<void> KernelCellAdmissionOwner::CheckManualCellAdmissionReadiness(
    const std::optional<CellAdmissionClaimGeneration> parent) const {
  std::lock_guard lock(mutex_);
  if (!parent) {
    return AdmissionLocked().authoring_allowed
               ? Result<void>::Ok()
               : AuthoringNotReady<void>(generation_, "positive_admission_required");
  }
  const auto held = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return entry.claim.claim_generation == *parent && !entry.release &&
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
      intent.owner_role != ApplicationRole::kProgramRunner || held_claim_count_ != 0U ||
      !NonCycleReadinessLocked() || !readiness_.program_cycle_status_current ||
      !readiness_.program_cycle_status.has_value()) {
    return false;
  }
  const auto& cycle = *readiness_.program_cycle_status;
  // K-6: `starting` names its run only after this claim; until then the run
  // number is the cycle generation.
  const auto run_id = cycle.program_run_id.value_or(ProgramRunNumber(cycle.cycle_generation));
  return cycle.state == ProgramCycleStateDTO::kStarting &&
         intent.owner_transaction_id == CellAdmissionOwnerTransactionId(run_id) &&
         !cycle.cycle_claim_generation.has_value() &&
         !cycle.terminal_or_settlement_reference.has_value();
}

Result<std::optional<ProgramRunNumber>> KernelCellAdmissionOwner::ProgramResetRunForCleanup(
    const CellAdmissionClaimIntentDTO& intent,
    const ProgramCycleStatusDTO& program_cycle_status) const {
  using RunResult = Result<std::optional<ProgramRunNumber>>;
  const auto valid = ValidateCellAdmissionClaimIntent(intent);
  if (!valid.ok()) return RunResult::Err(valid.error());
  std::lock_guard lock(mutex_);
  // Existing exact replay is independent of the current status.
  if (FindReplayLocked(intent)) return RunResult::Ok(std::optional<ProgramRunNumber>{});
  if (!readiness_.program_cycle_status_current || !readiness_.program_cycle_status)
    return AuthoringNotReady<std::optional<ProgramRunNumber>>(generation_,
                                                              "program_reset_status_not_current");
  return RunResult::Ok(MatchingProgramResetLocked(intent, program_cycle_status)
                           ? program_cycle_status.program_run_id
                           : std::optional<ProgramRunNumber>{});
}

Result<void> KernelCellAdmissionOwner::ValidateProgramResetPositionAuthorization(
    const ProgramResetPositionAuthorizationDTO& authorization) const {
  const auto valid = manyforge::core::ValidateProgramResetPositionAuthorization(authorization);
  if (!valid.ok()) return valid;
  std::lock_guard lock(mutex_);
  const auto generation = authorization.reset_claim_generation
      ? *authorization.reset_claim_generation : *authorization.cycle_claim_generation;
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& row) {
    return row.claim.claim_generation == generation && !row.release;
  });
  if (entry == entries_.end())
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  if (!readiness_.program_cycle_status_current || !readiness_.program_cycle_status)
    return AuthoringNotReady<void>(generation_, "program_reset_status_not_current");
  const auto kind = authorization.reset_claim_generation ? CellAdmissionClaimKindDTO::kProgramReset
                                                       : CellAdmissionClaimKindDTO::kProgramCycle;
  if (entry->claim.intent.claim_kind != kind ||
      entry->claim.intent.owner_role != ApplicationRole::kProgramRunner ||
      (authorization.reset_claim_generation && held_claim_count_ != 1U) ||
      HasUnsettledChildrenLocked(generation))
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  const auto current = CurrentResetLocked(entry->claim.intent, authorization.program_cycle_status);
  if (!current.ok()) return Result<void>::Err(current.error());
  const auto& status = current.value();
  const auto& accepted = *status.accepted_reset;
  if (!accepted.reset_robot_position ||
      status.cycle_generation != authorization.cycle_generation ||
      status.program_run_id != authorization.program_run_id ||
      status.cycle_claim_generation != authorization.cycle_claim_generation ||
      status.terminal_or_settlement_reference != authorization.terminal_or_settlement_reference ||
      accepted.request_key != authorization.request_key ||
      accepted.request_payload_digest != authorization.request_payload_digest ||
      accepted.reset_claim_generation != authorization.reset_claim_generation)
    return AuthoringNotReady<void>(generation_, "program_reset_status_mismatch");
  if (!authorization.reset_claim_generation) {
    if (entry->claim.intent.owner_transaction_id !=
        CellAdmissionOwnerTransactionId(*authorization.program_run_id))
      return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  } else {
    const auto identity = ProgramResetClaimRequestKey(status);
    if (!identity.ok() || entry->claim.intent.request_key != identity.value())
      return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  }
  return Result<void>::Ok();
}

// The carried status is the Program runner's own current cycle status
// (OD-139). It is validated here, in memory, against the held claim and the
// Manager-relayed readiness; the runner alone sequences the Reset phases.
Result<ProgramCycleStatusDTO> KernelCellAdmissionOwner::CurrentResetLocked(
    const CellAdmissionClaimIntentDTO& intent, const ProgramCycleStatusDTO& carried) const {
  const bool reset_claim = intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset;
  if ((reset_claim && !NonCycleReadinessLocked()) || !readiness_.program_cycle_status_current ||
      !readiness_.program_cycle_status || !manager_liveness_required_set_ ||
      manager_liveness_current_unfenced_count_ != manager_liveness_rows_.size() ||
      route_recovery_pending_) {
    return AuthoringNotReady<ProgramCycleStatusDTO>(generation_, "program_reset_status_not_current");
  }
  const auto current = Result<ProgramCycleStatusDTO>::Ok(carried);
  const auto& cycle = current.value();
  const auto& observed = *readiness_.program_cycle_status;
  if (!ValidateProgramCycleStatus(cycle).ok() || cycle.state != ProgramCycleStateDTO::kResetting ||
      !cycle.accepted_reset || cycle.program_run_id != observed.program_run_id ||
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
    const CellAdmissionClaimIntentDTO& intent, const ProgramCycleStatusDTO& carried) const {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kProgramReset || held_claim_count_ != 0U) {
    return false;
  }
  const auto current = CurrentResetLocked(intent, carried);
  if (!current.ok()) return false;
  const auto& cycle = current.value();
  if (cycle.cycle_claim_generation || cycle.accepted_reset->reset_claim_generation) {
    return false;
  }
  const auto identity = ProgramResetClaimRequestKey(cycle);
  return identity.ok() && intent.request_key == identity.value();
}

Result<void> KernelCellAdmissionOwner::AdvanceGenerationLocked() {
  if (generation_.value() >= 9'007'199'254'740'991ULL) {
    return Result<void>::Err(ErrorDTO{"authoring_not_ready",
                                      "cell-authoring admission is not ready",
                                      {{"reason", "admission_generation_exhausted"}}});
  }
  generation_ = AdmissionGeneration(generation_.value() + 1U);
  return Result<void>::Ok();
}

AuthoringAdmissionDTO KernelCellAdmissionOwner::AdmissionLocked() const noexcept {
  return AuthoringAdmissionDTO{!route_recovery_pending_ && BaseReadinessLocked() &&
                                   held_claim_count_ == 0U,
                               generation_};
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

Result<void> KernelCellAdmissionOwner::ValidateProgramRunnerContactLossSource(
    const ProgramRunnerContactLossDTO& loss) const {
  const auto valid = ValidateProgramRunnerContactLoss(loss);
  if (!valid.ok()) return valid;
  std::lock_guard lock(mutex_);
  if (!manager_liveness_required_set_) {
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
       snapshot.program_cycle_status_current || !manager_liveness_required_set_)) {
    return Reject<AuthoringAdmissionDTO>("invalid_request", "program_runner_contact_loss_invalid");
  }
  if (readiness_ != snapshot) {
    if (generation_.value() >= 9'007'199'254'740'991ULL) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "admission_generation_exhausted");
    }
    readiness_ = snapshot;
    generation_ = AdmissionGeneration(generation_.value() + 1U);
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
  const auto& status = observation.status;
  const auto found = std::lower_bound(manager_liveness_rows_.begin(), manager_liveness_rows_.end(),
                                      status.receiver_runtime_unit_id,
                                      [](const ManagerLivenessRow& row, const RuntimeUnitId& id) {
                                        return row.receiver.runtime_unit_id < id;
                                      });
  if (found == manager_liveness_rows_.end() ||
      found->receiver.runtime_unit_id != status.receiver_runtime_unit_id) {
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
  // new admissions: a registered rule must not keep consuming the old BB snapshot.
  auto invalidated = !next_current && required_health_loss_observer_
                         ? required_health_loss_observer_(actual_retired_service)
                         : Result<void>::Ok();
  if (!transition_required) return invalidated;
  if (generation_.value() >= kMaximumManagerLivenessCounter) {
    if (!invalidated.ok()) return invalidated;
    return Reject<void>("authoring_not_ready", "admission_generation_exhausted");
  }
  generation_ = AdmissionGeneration(generation_.value() + 1U);
  const auto current = std::lower_bound(
      manager_liveness_rows_.begin(), manager_liveness_rows_.end(), status.receiver_runtime_unit_id,
      [](const ManagerLivenessRow& row, const RuntimeUnitId& id) {
        return row.receiver.runtime_unit_id < id;
      });
  if (current != manager_liveness_rows_.end() &&
      current->receiver.runtime_unit_id == status.receiver_runtime_unit_id &&
      current->generation_advance_pending && current->generation_pending_sequence == token) {
    current->generation_advance_pending = false;
    if (next_current && current->status_sequence == token && !current->current_unfenced) {
      current->current_unfenced = true;
      ++manager_liveness_current_unfenced_count_;
    }
  }
  return invalidated;
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
    next_rows.push_back({receiver, 0U, false, false, 0U, false});
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
    return entry.claim.claim_generation == generation && !entry.release.has_value();
  });
  return found == entries_.end() ? nullptr : &*found;
}

const KernelCellAdmissionOwner::Entry* KernelCellAdmissionOwner::ParentCycleLocked(
    const CellAdmissionClaimIntentDTO& intent) const {
  const auto generation = parent_program_cycle_claim(intent);
  if (!generation.has_value()) return nullptr;
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return !entry.release.has_value() &&
           entry.claim.claim_generation == *generation &&
           entry.claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle;
  });
  return found == entries_.end() ? nullptr : &*found;
}

bool KernelCellAdmissionOwner::HasUnsettledChildrenLocked(
    const CellAdmissionClaimGeneration parent) const {
  return std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return !entry.release.has_value() && parent_program_cycle_claim(entry.claim.intent) == parent;
  });
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent) {
  // A Program Reset claim needs the carried status (AcquireProgramResetClaim).
  return AcquireClaim(intent, intervention_validator_);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireProgramResetClaim(
    const CellAdmissionClaimIntentDTO& intent, const ProgramCycleStatusDTO& program_cycle_status) {
  return AcquireClaim(intent, intervention_validator_, {}, &program_cycle_status);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireManualCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionInterventionValidator& validate_offer) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kManualOperation || !validate_offer) {
    return Reject<CellAdmissionClaimDTO>("invalid_request", "manual_offer_validator_required");
  }
  return AcquireClaim(intent, validate_offer);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireConfigurationApplyClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionConfigurationApplyValidator& validate) {
  if (intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
      intent.owner_role != ApplicationRole::kManager || !intent.scope.resources.empty() ||
      !intent.scope.configurations.empty() || intent.program_cycle_evidence ||
      intent.intervention_evidence || intent.program_node_evidence || !validate) {
    return Reject<CellAdmissionClaimDTO>("invalid_request",
                                         "configuration_apply_validator_required");
  }
  return AcquireClaim(intent, {}, validate);
}

Result<CellAdmissionClaimDTO> KernelCellAdmissionOwner::AcquireClaim(
    const CellAdmissionClaimIntentDTO& intent,
    const CellAdmissionInterventionValidator& validate_offer,
    const CellAdmissionConfigurationApplyValidator& configuration_apply_validator,
    const ProgramCycleStatusDTO* program_reset_status) {
  detail::AdmissionTiming timing(false, {});
  const bool configuration_apply = static_cast<bool>(configuration_apply_validator);
  auto wait_start = detail::AdmissionTiming::Clock::now();
  std::unique_lock transition_lock(liveness_transition_mutex_);
  timing.record->transition_wait += detail::AdmissionTiming::Clock::now() - wait_start;
  auto valid = ValidateCellAdmissionClaimIntent(intent);
  if (!valid.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(valid.error());
  }
  wait_start = detail::AdmissionTiming::Clock::now();
  std::unique_lock lock(mutex_);
  timing.record->owner_wait += detail::AdmissionTiming::Clock::now() - wait_start;
  if (const auto* replay = FindReplayLocked(intent); replay != nullptr) {
    if (replay->claim.intent != intent) {
      return Reject<CellAdmissionClaimDTO>("request_id_conflict", "request_reuse_conflict");
    }
    if (replay->release.has_value()) {
      return Reject<CellAdmissionClaimDTO>("request_result_expired", "request_result_compacted");
    }
    return Result<CellAdmissionClaimDTO>::Ok(replay->claim);
  }
  const auto intent_digest = CellAdmissionClaimIntentDigest(intent);
  if (!intent_digest.ok()) {
    return Result<CellAdmissionClaimDTO>::Err(intent_digest.error());
  }
  const auto compacted =
      std::find_if(membership_.begin(), membership_.end(), [&](const auto& member) {
        return member.owner_role == intent.owner_role && member.request_key == intent.request_key;
      });
  if (compacted != membership_.end()) {
    if (compacted->owner_role != intent.owner_role ||
        compacted->semantic_intent_digest != intent_digest.value()) {
      return Reject<CellAdmissionClaimDTO>("request_id_conflict", "request_reuse_conflict");
    }
    return Reject<CellAdmissionClaimDTO>("request_result_expired", "request_result_compacted");
  }
  // The intent's expected admission generation is claim evidence only and is
  // never compared: every admission fact below is evaluated now, under these
  // locks, so a generation that merely moved since the caller's read is not a
  // refusal.
  // Exact replay above remains observable during a fault. The private
  // parent alternative still requires its positive plan validation below.
  if (route_recovery_pending_ &&
      intent.claim_kind != CellAdmissionClaimKindDTO::kInterventionOrRecovery) {
    return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "route_recovery_pending");
  }
  const auto retire_released = [&](const auto& entry) {
    return entry.release.has_value() && entry.claim.intent.owner_role == intent.owner_role;
  };
  const auto retired_count =
      static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), retire_released));
  if (membership_.size() - retired_count >= capacity_) {
    return Reject<CellAdmissionClaimDTO>("request_ledger_full", "claim_capacity_exhausted");
  }

  std::vector<CellAdmissionClaimDTO> configuration_held;
  if (configuration_apply) {
    for (const auto& entry : entries_) {
      if (!entry.release) {
        if (entry.claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
            entry.claim.intent.owner_role != ApplicationRole::kManager) {
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
    const auto& evidence = *intent.program_node_evidence;
    const auto& cycle = readiness_.program_cycle_status;
    if (parent == nullptr || !program_node_validator_ ||
        parent->claim.intent.owner_transaction_id !=
            CellAdmissionOwnerTransactionId(evidence.program_run_id)) {
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
          return !entry.release.has_value() &&
                 &entry != parent &&
                 (!entry.claim.intent.program_node_evidence.has_value() ||
                  parent_program_cycle_claim(entry.claim.intent) != parent->claim.claim_generation);
        })) {
      return AuthoringNotReady<CellAdmissionClaimDTO>(generation_, "program_node_claim_conflict");
    }
    // Ordinary admission is held by the composing Kernel across this bounded
    // fence check and the commit below. Resume cannot release the fence until
    // this exact child becomes visible/settled.
    const auto admitted = program_node_validator_(evidence);
    if (!admitted.ok()) return Result<CellAdmissionClaimDTO>::Err(admitted.error());
  } else if (configuration_apply) {
    if (!manager_liveness_required_set_ ||
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
        configuration_apply_validator(intent, configuration_held, readiness_);
    if (!validated.ok()) return Result<CellAdmissionClaimDTO>::Err(validated.error());
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
  } else if (intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset) {
    if (!program_reset_status || !MatchingProgramResetLocked(intent, *program_reset_status)) {
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
      footprint_entries.push_back({entry.claim, entry.release});
    }
  }
  footprint_entries.push_back({claim, std::nullopt});
  const auto footprint = ComputeCellAdmissionRecoveryFootprint(
      AdmissionGeneration(generation_.value() + 1U), footprint_entries);
  if (!footprint.ok() || !footprint.value().admitted()) {
    return Reject<CellAdmissionClaimDTO>("invalid_request", "recovery_footprint_exceeded");
  }
  std::vector<RequestKey> retired_request_keys;
  retired_request_keys.reserve(retired_count);
  for (const auto& entry : entries_) {
    if (retire_released(entry)) {
      retired_request_keys.push_back(entry.claim.intent.request_key);
    }
  }
  Entry prepared_entry{claim, std::nullopt};
  Membership prepared_member{intent.owner_role, intent.request_key, intent_digest.value()};
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), retire_released), entries_.end());
  membership_.erase(
      std::remove_if(membership_.begin(), membership_.end(),
                     [&](const auto& member) {
                       return member.owner_role == intent.owner_role &&
                              std::find(retired_request_keys.begin(), retired_request_keys.end(),
                                        member.request_key) != retired_request_keys.end();
                     }),
      membership_.end());
  entries_.push_back(std::move(prepared_entry));
  membership_.push_back(std::move(prepared_member));
  ++held_claim_count_;
  next_claim_generation_ = CellAdmissionClaimGeneration(next_claim_generation_.value() + 1U);
  generation_ = AdmissionGeneration(generation_.value() + 1U);
  // The claim holds from here; its record follows, posted outside both locks.
  JournalRecord record{JournalCellClaimAcquired{claim}, "cell_claim_acquired", std::nullopt, false,
                       false};
  lock.unlock();
  transition_lock.unlock();
  post_records_(std::span<JournalRecord>(&record, 1U));
  return Result<CellAdmissionClaimDTO>::Ok(std::move(claim));
}

std::vector<std::string> KernelCellAdmissionOwner::unsettled_program_child_nodes(
    const CellAdmissionClaimGeneration parent) const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> nodes;
  for (const auto& entry : entries_) {
    if (!entry.release.has_value() &&
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
  detail::AdmissionTiming timing(true, {});
  auto wait_start = detail::AdmissionTiming::Clock::now();
  std::unique_lock transition_lock(liveness_transition_mutex_);
  timing.record->transition_wait += detail::AdmissionTiming::Clock::now() - wait_start;
  auto valid = ValidateCellAdmissionClaimRelease(release);
  if (!valid.ok()) {
    return Result<AuthoringAdmissionDTO>::Err(valid.error());
  }
  wait_start = detail::AdmissionTiming::Clock::now();
  std::unique_lock lock(mutex_);
  timing.record->owner_wait += detail::AdmissionTiming::Clock::now() - wait_start;
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
  // Program-cycle and Program-Reset releases: the exact held identity, the
  // settlement kind and the settled children above are the whole proof; the
  // Program runner alone sequences its Reset (OD-139). No owner file is read.
  if (entry->claim.intent.claim_kind != CellAdmissionClaimKindDTO::kProgramCycle &&
      entry->claim.intent.claim_kind != CellAdmissionClaimKindDTO::kProgramReset) {
    if (!settlement_validator_) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "settlement_validator_unavailable");
    }
    const auto settlement_start = detail::AdmissionTiming::Clock::now();
    auto settlement = settlement_validator_(entry->claim, release);
    timing.record->settlement += detail::AdmissionTiming::Clock::now() - settlement_start;
    if (!settlement.ok()) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "settlement_not_proven");
    }
  }
  if (generation_.value() >= 9'007'199'254'740'991ULL) {
    return AuthoringNotReady<AuthoringAdmissionDTO>(generation_, "admission_generation_exhausted");
  }
  entry->release = release;
  --held_claim_count_;
  generation_ = AdmissionGeneration(generation_.value() + 1U);
  auto admission = AdmissionLocked();
  JournalRecord record{JournalCellClaimReleased{entry->claim, release, clock_.now()},
                       "cell_claim_released", std::nullopt, false, false};
  lock.unlock();
  transition_lock.unlock();
  post_records_(std::span<JournalRecord>(&record, 1U));
  return Result<AuthoringAdmissionDTO>::Ok(admission);
}

Result<AuthoringAdmissionDTO> KernelCellAdmissionOwner::ReleaseTerminalApplyClaimsAfterSuccess(
    const CellAdmissionClaimDTO& fresh_claim,
    const CellAdmissionClaimReleaseDTO& fresh_success_release,
    const CellAdmissionConfigurationApplySuccessValidator& validate) {
  std::unique_lock transition_lock(liveness_transition_mutex_);
  std::unique_lock lock(mutex_);
  auto* fresh = FindHeldLocked(fresh_claim.claim_generation);
  if (!validate || !fresh || fresh->claim != fresh_claim ||
      fresh_claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
      fresh_claim.intent.owner_role != ApplicationRole::kManager ||
      !ValidateCellAdmissionClaimRelease(fresh_success_release).ok() ||
      !SameReleaseIdentity(fresh_claim, fresh_success_release) ||
      fresh_success_release.settlement.settlement_kind !=
          CellAdmissionSettlementKindDTO::kTerminalEffectsSettled) {
    return Reject<AuthoringAdmissionDTO>("invalid_request", "fresh_apply_success_not_exact");
  }
  std::vector<CellAdmissionClaimDTO> old;
  for (const auto& entry : entries_) {
    if (entry.release) continue;
    if (entry.claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManagerApply ||
        entry.claim.intent.owner_role != ApplicationRole::kManager) {
      return AuthoringNotReady<AuthoringAdmissionDTO>(generation_,
                                                      "fresh_apply_success_foreign_claim");
    }
    if (entry.claim == fresh_claim) continue;
    old.push_back(entry.claim);
  }
  std::sort(old.begin(), old.end(),
            [](const auto& a, const auto& b) { return a.claim_generation < b.claim_generation; });
  const auto validated = validate(fresh_claim, fresh_success_release, old);
  if (!validated.ok()) return Result<AuthoringAdmissionDTO>::Err(validated.error());
  if (old.empty()) return Result<AuthoringAdmissionDTO>::Ok(AdmissionLocked());
  std::vector<JournalRecord> records;
  records.reserve(old.size());
  const auto released_at_ns = clock_.now();
  for (auto& claim : old) {
    CellAdmissionClaimReleaseDTO release{claim.intent.owner_role,
                                         claim.intent.owner_transaction_id,
                                         claim.intent.request_key,
                                         claim.acquired_admission_generation,
                                         claim.claim_generation,
                                         fresh_success_release.settlement};
    FindHeldLocked(claim.claim_generation)->release = release;
    --held_claim_count_;
    records.emplace_back(
        JournalCellClaimReleased{std::move(claim), std::move(release), released_at_ns},
        "cell_claim_released", std::nullopt, false, false);
  }
  generation_ = AdmissionGeneration(generation_.value() + 1U);
  auto admission = AdmissionLocked();
  lock.unlock();
  transition_lock.unlock();
  post_records_(records);
  return Result<AuthoringAdmissionDTO>::Ok(admission);
}

std::optional<CellAdmissionClaimDTO> KernelCellAdmissionOwner::held_claim(
    const CellAdmissionClaimKindDTO kind) const {
  std::lock_guard lock(mutex_);
  for (const auto& entry : entries_) {
    if (entry.claim.intent.claim_kind == kind && !entry.release.has_value()) {
      return entry.claim;
    }
  }
  return std::nullopt;
}

std::optional<CellAdmissionClaimDTO> KernelCellAdmissionOwner::held_claim(
    const CellAdmissionClaimKindDTO kind,
    const CellAdmissionOwnerTransactionId& owner_transaction_id) const {
  std::lock_guard lock(mutex_);
  for (const auto& entry : entries_) {
    if (entry.claim.intent.claim_kind == kind &&
        entry.claim.intent.owner_transaction_id == owner_transaction_id &&
        !entry.release.has_value()) {
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
    if (entry.claim.intent.claim_kind == kind && !entry.release)
      claims.push_back(entry.claim);
  }
  std::sort(claims.begin(), claims.end(),
            [](const auto& a, const auto& b) { return a.claim_generation < b.claim_generation; });
  return claims;
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
      .admission_generation = generation_,
      .matched_claim = std::nullopt,
      .matched_release = std::nullopt,
      .held_claims = {},
  };
  for (const auto& entry : entries_) {
    if (!entry.release.has_value()) {
      receipt.held_claims.push_back(entry.claim);
    }
  }
  std::sort(receipt.held_claims.begin(), receipt.held_claims.end(),
            [](const auto& left, const auto& right) {
              return left.claim_generation < right.claim_generation;
            });

  const auto digest = CellAdmissionClaimIntentDigest(query.intent);
  const auto member = std::find_if(membership_.begin(), membership_.end(), [&](const auto& row) {
    return row.owner_role == query.intent.owner_role && row.request_key == query.intent.request_key;
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
    return row.claim.intent.owner_role == query.intent.owner_role &&
           row.claim.intent.request_key == query.intent.request_key;
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
  if (entry->release.has_value()) {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact;
    receipt.matched_release = entry->release;
  } else {
    receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
  }
  return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
}

Result<void> KernelCellAdmissionOwner::ValidateProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization) const {
  const auto valid = manyforge::core::ValidateProgramResetSceneAuthorization(authorization);
  if (!valid.ok()) return Result<void>::Err(valid.error());
  std::lock_guard lock(mutex_);
  const auto held_generation = authorization.reset_claim
      ? authorization.reset_claim->reset_claim_generation : authorization.cycle_claim_generation;
  if (std::none_of(entries_.begin(), entries_.end(), [&](const auto& row) {
        return row.claim.claim_generation == held_generation && !row.release;
      })) {
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  }
  if (!readiness_.program_cycle_status_current || !readiness_.program_cycle_status.has_value()) {
    return AuthoringNotReady<void>(generation_, "program_reset_status_not_current");
  }
  if (authorization.reset_claim) {
    const auto& expected = *authorization.reset_claim;
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& row) {
      return row.claim.claim_generation == expected.reset_claim_generation;
    });
    if (entry == entries_.end() || entry->release || held_claim_count_ != 1U ||
        entry->claim.intent.claim_kind != CellAdmissionClaimKindDTO::kProgramReset ||
        entry->claim.intent.owner_role != ApplicationRole::kProgramRunner) {
      return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
    }
    const auto current = CurrentResetLocked(entry->claim.intent, authorization.program_cycle_status);
    if (!current.ok()) return Result<void>::Err(current.error());
    const auto& status = current.value();
    const auto& accepted = *status.accepted_reset;
    const auto identity = ProgramResetClaimRequestKey(status);
    if (!accepted.reload_initial_scene ||
        status.cycle_generation != expected.cycle_generation ||
        accepted.request_key != expected.request_key ||
        accepted.request_payload_digest != expected.request_payload_digest ||
        accepted.reset_claim_generation != expected.reset_claim_generation ||
        !identity.ok() ||
        entry->claim.intent.request_key != identity.value()) {
      return AuthoringNotReady<void>(generation_, "program_reset_status_mismatch");
    }
    return Result<void>::Ok();
  }
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const auto& candidate) {
    return candidate.claim.claim_generation == authorization.cycle_claim_generation;
  });
  if (entry == entries_.end() || entry->release.has_value() ||
      entry->claim.intent.claim_kind != CellAdmissionClaimKindDTO::kProgramCycle ||
      entry->claim.intent.owner_role != ApplicationRole::kProgramRunner ||
      entry->claim.intent.owner_transaction_id !=
          CellAdmissionOwnerTransactionId(authorization.program_run_id)) {
    return AuthoringNotReady<void>(generation_, "program_reset_claim_not_exact");
  }
  const auto current = CurrentResetLocked(entry->claim.intent, authorization.program_cycle_status);
  if (!current.ok()) return Result<void>::Err(current.error());
  const auto& status = current.value();
  if (!status.accepted_reset->reload_initial_scene) {
    return AuthoringNotReady<void>(generation_, "program_reset_scene_not_selected");
  }
  if (status.program_run_id != std::optional<ProgramRunNumber>{authorization.program_run_id} ||
      status.cycle_claim_generation !=
          std::optional<CellAdmissionClaimGeneration>{authorization.cycle_claim_generation} ||
      status.terminal_or_settlement_reference !=
          std::optional<ContentDigest>{authorization.terminal_or_settlement_reference}) {
    return AuthoringNotReady<void>(generation_, "program_reset_status_mismatch");
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
    ApplicationRole owner_role) const {
  std::lock_guard lock(mutex_);
  return std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return !entry.release &&
           entry.claim.intent.owner_role != owner_role;
  });
}

}  // namespace manyforge::core
