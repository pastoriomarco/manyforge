#include <manyforge_core/io_rule_stop_response.hpp>
#include "resource_addressed_execution_kernel_internal.hpp"
#include "program_stop_projection_internal.hpp"

#include <cstdio>

namespace manyforge::core {

StopScopeResolverSnapshotDTO ResourceAddressedExecutionKernel::capture_stop_scope_locked(
    const ResourceSlotTableSnapshotDTO& slots) const {
  StopScopeResolverSnapshotDTO snapshot{config_.request_owner_epoch,
                                        *stop_graph_revision_,
                                        slots.operation_resource_generation,
                                        cell_stop_generation_,
                                        {},
                                        stop_component_scope_,
                                        {},
                                        {}};
  snapshot.operations.reserve(active_operations_.size() + historical_operations_.size());
  for (const auto& [operation_id, operation] : active_operations_) {
    const bool effect_uncertain = operation->containment_effect_uncertain();
    const bool fully_transferred =
        !operation->participants.empty() &&
        std::all_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& participant) { return participant.ownership_transferred; });
    std::vector<ControllableComponentId> components;
    std::vector<ResourceId> resources;
    components.reserve(operation->participants.size());
    for (const auto& participant : operation->participants) {
      if (participant.ownership_transferred) continue;
      components.push_back(participant.binding.value().controllable_component_id);
      resources.insert(resources.end(), participant.resources.begin(), participant.resources.end());
    }
    std::sort(components.begin(), components.end());
    components.erase(std::unique(components.begin(), components.end()), components.end());
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    snapshot.operations.push_back(StopScopeOperationSnapshotDTO{
        operation_id, operation->state.program_run_id, operation->state.operation_revision,
        operation->state.active_stop_generation.value_or(StopGeneration(0U)),
        operation->state.state, !fully_transferred, effect_uncertain, std::move(components),
        std::move(resources),
        operation->manual_handover
            ? std::optional<ProgramRunId>{operation->manual_handover->capture.program_run_id}
            : std::nullopt});
  }
  std::set<ProgramRunId> unresolved_historical_runs;
  std::vector<ResourceId> unresolved_resources;
  for (const auto& slot : slots.slots) {
    if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal)
      unresolved_resources.push_back(slot.resource_id);
  }
  std::sort(unresolved_resources.begin(), unresolved_resources.end());
  for (const auto& operation : historical_operations_) {
    if (active_operations_.contains(operation.operation_id)) {
      continue;
    }
    const bool unresolved_predecessor = std::any_of(
        operation.participants.begin(), operation.participants.end(), [&](const auto& participant) {
          return std::any_of(participant.resources.begin(), participant.resources.end(),
                             [&](const auto& resource) {
                               return std::binary_search(unresolved_resources.begin(),
                                                         unresolved_resources.end(), resource);
                             });
        });
    if (unresolved_predecessor) {
      // This historical terminal kind is not terminal physical evidence.
      // Leave the selector unknown so the existing resolver captures the full
      // orphan-resource scope, not a nonresident operation it cannot fence.
      unresolved_historical_runs.insert(operation.program_run_id);
      continue;
    }
    snapshot.operations.push_back(
        StopScopeOperationSnapshotDTO{operation.operation_id,
                                      operation.program_run_id,
                                      operation.operation_revision,
                                      operation.active_stop_generation.value_or(StopGeneration(0U)),
                                      operation.state,
                                      false,
                                      false,
                                      {},
                                      {}});
  }
  std::sort(
      snapshot.operations.begin(), snapshot.operations.end(),
      [](const auto& left, const auto& right) { return left.operation_id < right.operation_id; });

  snapshot.known_program_runs.reserve(program_run_stop_fences_.size() + 3U);
  if (current_program_run_id_.has_value()) {
    snapshot.known_program_runs.push_back(*current_program_run_id_);
  }
  if (program_cycle_claim_run_.has_value()) {
    snapshot.known_program_runs.push_back(program_cycle_claim_run_->program_run_id);
    if (!unresolved_resources.empty()) {
      // The cell-wide recovered claim may precede every operation record.
      // Orphan inventory still prevents certifying that run as empty/rested.
      unresolved_historical_runs.insert(program_cycle_claim_run_->program_run_id);
    }
  }
  if (pending_program_run_admission_.has_value()) {
    snapshot.known_program_runs.push_back(*pending_program_run_admission_);
  }
  for (const auto& fence : program_run_stop_fences_) {
    snapshot.known_program_runs.push_back(fence.program_run_id);
  }
  std::sort(snapshot.known_program_runs.begin(), snapshot.known_program_runs.end());
  snapshot.known_program_runs.erase(
      std::unique(snapshot.known_program_runs.begin(), snapshot.known_program_runs.end()),
      snapshot.known_program_runs.end());
  std::erase_if(snapshot.known_program_runs,
                [&](const auto& run) { return unresolved_historical_runs.contains(run); });
  // A settled sibling in the same retired run cannot turn an unresolved run
  // selector into a successful empty capture.
  std::erase_if(snapshot.operations, [&](const auto& operation) {
    return !active_operations_.contains(operation.operation_id) &&
           unresolved_historical_runs.contains(operation.program_run_id);
  });

  snapshot.resources.reserve(slots.slots.size());
  for (const auto& slot : slots.slots) {
    bool effect_uncertain = slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal;
    if (slot.owner.has_value()) {
      const auto* operation = find_active_operation_locked(slot.owner->operation_id);
      effect_uncertain =
          effect_uncertain || operation == nullptr || operation->containment_effect_uncertain();
    }
    snapshot.resources.push_back(
        StopScopeResourceSnapshotDTO{slot.resource_id, slot.stop_generation, effect_uncertain});
  }
  return snapshot;
}

void ResourceAddressedExecutionKernel::ScheduleStopSurveillance(
    StopCertificateId certificate_id, const std::int64_t deadline_ns) noexcept {
  if (!config_.stop_surveillance.has_value()) {
    return;
  }
  std::uint64_t generation = 0U;
  {
    std::lock_guard state_lock(state_mutex_);
    if (stop_surveillance_shutting_down_) {
      return;
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
        });
    if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
        (*found)->surveillance_state->projection.state !=
            StopPostTerminalSurveillanceStateDTO::kMonitoring ||
        (*found)->surveillance_callback_pending ||
        (*found)->surveillance_schedule_generation == std::numeric_limits<std::uint64_t>::max()) {
      return;
    }
    generation = ++(*found)->surveillance_schedule_generation;
    (*found)->surveillance_callback_pending = true;
    ++stop_surveillance_schedule_publications_;
  }

  std::optional<ScheduleHandle> handle;
  try {
    handle = scheduler_.schedule(
        deadline_ns,
        [this, certificate_id, generation] { RunStopSurveillance(certificate_id, generation); },
        SchedulerPriority::Monitoring);
  } catch (...) {
    // The publication boundary itself is infrastructure, not source truth.
  }

  bool cancel_late_handle = false;
  {
    std::lock_guard state_lock(state_mutex_);
    if (stop_surveillance_schedule_publications_ == 0U) {
      std::terminate();
    }
    --stop_surveillance_schedule_publications_;
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
        });
    if (!handle.has_value() || found == stop_terminal_history_.end() ||
        (*found)->surveillance_schedule_generation != generation ||
        !(*found)->surveillance_callback_pending || stop_surveillance_shutting_down_) {
      cancel_late_handle = handle.has_value();
      if (found != stop_terminal_history_.end() &&
          (*found)->surveillance_schedule_generation == generation) {
        (*found)->surveillance_callback_pending = false;
        (*found)->surveillance_schedule_handle.reset();
      }
      if (!handle.has_value()) {
        motion_admission_closed_ = true;
      }
    } else {
      (*found)->surveillance_schedule_handle = *handle;
    }
    stop_surveillance_schedule_cv_.notify_all();
  }
  if (cancel_late_handle) {
    try {
      scheduler_.cancel(*handle);
    } catch (...) {
      std::terminate();
    }
  }
}

void ResourceAddressedExecutionKernel::RunStopSurveillance(
    StopCertificateId certificate_id, const std::uint64_t schedule_generation) noexcept {
  try {
    std::int64_t next_deadline_ns = 0;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() ||
          (*found)->surveillance_schedule_generation != schedule_generation ||
          !(*found)->surveillance_callback_pending) {
        return;
      }
      (*found)->surveillance_callback_pending = false;
      (*found)->surveillance_schedule_handle.reset();
      if (stop_surveillance_shutting_down_ || (*found)->surveillance_state == nullptr ||
          (*found)->surveillance_state->projection.state !=
              StopPostTerminalSurveillanceStateDTO::kMonitoring) {
        return;
      }
    }

    const auto now = clock_.now();
    if (now < 0) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return;
    }
    std::size_t source_count = 0U;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr) {
        return;
      }
      source_count = (*found)->surveillance_state->source_plans.size();
    }
    for (std::size_t source_index = 0U; source_index < source_count; ++source_index) {
      std::optional<StopSurveillanceObservationQueryDTO> query;
      {
        std::lock_guard state_lock(state_mutex_);
        const auto found = std::find_if(
            stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
              return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
            });
        if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
            (*found)->surveillance_state->projection.state !=
                StopPostTerminalSurveillanceStateDTO::kMonitoring) {
          break;
        }
        auto made =
            MakeStopSurveillanceObservationQuery(*(*found)->surveillance_state, source_index, now);
        if (!made.ok()) {
          motion_admission_closed_ = true;
          continue;
        }
        query = std::move(made.value());
      }
      auto sample = config_.stop_surveillance->observer->Observe(*query);
      {
        std::lock_guard state_lock(state_mutex_);
        const auto found = std::find_if(
            stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
              return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
            });
        if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
            (*found)->surveillance_state->projection.state !=
                StopPostTerminalSurveillanceStateDTO::kMonitoring) {
          break;
        }
        auto reduced =
            ReduceStopSurveillanceObservation(*(*found)->surveillance_state, *query, sample);
        if (!reduced.ok()) {
          motion_admission_closed_ = true;
          continue;
        }
        *(*found)->surveillance_state = std::move(reduced.value());
        (*found)->status.surveillance = (*found)->surveillance_state->projection;
        if ((*found)->fault_latch.has_value()) {
          (*found)->status.fault_latch = (*found)->fault_latch;
        }
      }
    }

    std::optional<StopControlWorkToken> checkpoint_token;
    bool reschedule = false;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr) {
        return;
      }
      auto& record = **found;
      const auto& policy = record.surveillance_state->projection.policy;
      if (now >= record.next_surveillance_checkpoint_ns && record.fault_latch.has_value() &&
          !record.surveillance_projection_token.has_value()) {
        auto checkpoint = MakeStopSurveillanceCheckpoint(*record.surveillance_state);
        if (checkpoint.ok()) {
          checkpoint_token = allocate_stop_projection_job_locked();
          if (checkpoint_token.has_value()) {
            auto& projection = *stop_projection_jobs_[checkpoint_token->slot];
            projection.surveillance_checkpoint = std::move(checkpoint.value());
            projection.surveillance_fault_latch = *record.fault_latch;
            record.surveillance_projection_token = checkpoint_token;
            if (now <= std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
              record.next_surveillance_checkpoint_ns = now + policy.checkpoint_interval_ns;
            }
          } else {
            motion_admission_closed_ = true;
          }
        }
      }
      reschedule = record.surveillance_state->projection.state ==
                   StopPostTerminalSurveillanceStateDTO::kMonitoring;
      if (reschedule) {
        if (now > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns) {
          motion_admission_closed_ = true;
          reschedule = false;
        } else {
          const auto elapsed = std::max<std::int64_t>(0, now - record.next_surveillance_sample_ns);
          const auto skipped = elapsed / policy.sample_cadence_ns + 1;
          if (skipped >
              (std::numeric_limits<std::int64_t>::max() - record.next_surveillance_sample_ns) /
                  policy.sample_cadence_ns) {
            motion_admission_closed_ = true;
            reschedule = false;
          } else {
            record.next_surveillance_sample_ns += skipped * policy.sample_cadence_ns;
            next_deadline_ns = record.next_surveillance_sample_ns;
          }
        }
      }
    }
    if (checkpoint_token.has_value()) {
      stop_projection_worker_->Publish(*checkpoint_token);
    }
    if (reschedule) {
      ScheduleStopSurveillance(std::move(certificate_id), next_deadline_ns);
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

Result<StopStatusDTO> ResourceAddressedExecutionKernel::get_stop_status(
    const StopId& stop_id) const {
  if (stop_id.empty()) {
    return reject<StopStatusDTO>("Stop status identity is empty", "stop_id_invalid",
                                 ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto terminal = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->certificate.stop_id == stop_id;
      });
  if (terminal != stop_terminal_history_.end()) {
    return Result<StopStatusDTO>::Ok((*terminal)->status);
  }
  const auto active = std::find_if(
      stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& transaction) {
        return transaction != nullptr && transaction->active && transaction->stop_id == stop_id &&
               transaction->accepted.has_value();
      });
  if (active == stop_transactions_.end()) {
    return reject<StopStatusDTO>("Stop status is not resident", "stop_status_not_resident",
                                 ErrorCode::kMissingReference);
  }
  const auto& transaction = **active;
  std::vector<StopScopeEntryDTO> entries;
  std::vector<StopAttemptedStageResultDTO> attempts;
  entries.reserve(transaction.participants.size() + transaction.newly_captured_resources.size());
  for (const auto& participant : transaction.participants) {
    entries.push_back(participant_scope_entry(
        participant, *transaction.stop_id, transaction.stop_generation,
        transaction.accepted->accepted_at_ns));
    attempts.insert(attempts.end(), participant.retained_results.begin(),
                    participant.retained_results.end());
  }
  for (const auto& resource : transaction.newly_captured_resources) {
    entries.push_back(StopScopeEntryDTO{
        StopScopeEntryKeyDTO{*transaction.stop_id, transaction.stop_generation, resource},
        resource,
        StopScopeEntryDispositionDTO::kNotStoppable,
        StopScopeDispatchStateDTO::kEffectUncertain,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        transaction.accepted->accepted_at_ns});
  }
  const auto projection = [&] {
    switch (transaction.acceptance_projection_state) {
      case EmergencyStopProjectionStateDTO::kPending:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                              std::nullopt};
      case EmergencyStopProjectionStateDTO::kRecorded:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                              std::nullopt, std::nullopt};
      case EmergencyStopProjectionStateDTO::kFailed:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                              ErrorDTO{"operation_journal_stop_projection_failed",
                                                       "Stop acceptance forensic projection failed",
                                                       {}},
                                              1U};
    }
    return StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kFailed,
        ErrorDTO{"stop_projection_state_invalid", "Stop projection state is invalid", {}}, 1U};
  }();
  StopStatusDTO status{*transaction.accepted,
                       transaction.fresh_scope_complete ? StopStatusStateDTO::kCertifying
                                                        : StopStatusStateDTO::kStopping,
                       std::move(entries),
                       {},
                       std::move(attempts),
                       {},
                       {},
                       {},
                       projection,
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                      std::nullopt, std::nullopt},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                                      std::nullopt, std::nullopt},
                       std::nullopt,
                       transaction.fault_latch,
                       std::nullopt};
  return Result<StopStatusDTO>::Ok(std::move(status));
}

CellStopActionProjectionDTO ResourceAddressedExecutionKernel::cell_stop_action_projection() const {
  std::lock_guard state_lock(state_mutex_);
  CellStopActionProjectionDTO projection{cell_stop_generation_, std::nullopt, std::nullopt};
  if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
    const auto& latch = *active_stop_fault_latch_;
    projection.fault = CellStopFaultSummaryDTO{
        latch.latch_id,        latch.latch_generation, latch.state,           latch.trigger,
        latch.recovery_status, latch.reason_code,      latch.stop_certificate,
        latch.observed_triggers};
  }
  if (stop_terminal_history_.empty() || stop_terminal_history_.back() == nullptr) {
    return projection;
  }
  const auto& certificate = stop_terminal_history_.back()->certificate;
  CellStopTerminalSummaryDTO terminal{
      StopCertificateRefDTO{certificate.stop_certificate_id, certificate.stop_certificate_revision,
                            certificate.stop_id, certificate.stop_generation},
      certificate.result,
      certificate.result == StopCertificateResultDTO::kSucceededEscalated,
      false,
      {}};
  terminal.executed_stages.reserve(certificate.attempted_stage_results.size());
  for (const auto& attempted : certificate.attempted_stage_results) {
    if (attempted.authority != StopStageResultAuthorityDTO::kAuthoritative ||
        attempted.result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched) {
      continue;
    }
    const auto& request = attempted.result.request;
    const auto stage_index = request.stage_index;
    if (stage_index >= request.participant_stop_plan.stage_commands.size()) {
      continue;
    }
    const auto& stage = request.participant_stop_plan.stage_commands[stage_index].resolved_stage;
    const auto action_class = stage.command_qualification.stop_action_class;
    terminal.escalated = terminal.escalated || stage_index > 0U;
    terminal.hard_stop_floor_used =
        terminal.hard_stop_floor_used || action_class != StopActionClass::kControlled;
    terminal.executed_stages.push_back(CellStopStageSummaryDTO{
        request.operation_id, request.participant_id, stage.stop_escalation_stage_id, stage_index,
        action_class, attempted.result.outcome, attempted.result.actual_provider_contract_mode,
        attempted.result.recoverable_substitution_reason});
  }
  projection.latest_terminal = std::move(terminal);
  return projection;
}

Result<StopCertificateDTO> ResourceAddressedExecutionKernel::get_stop_certificate(
    const StopCertificateId& certificate_id) const {
  if (certificate_id.empty()) {
    return reject<StopCertificateDTO>("Stop certificate identity is empty",
                                      "stop_certificate_id_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
      });
  if (found == stop_terminal_history_.end()) {
    return reject<StopCertificateDTO>("Stop certificate is not resident",
                                      "stop_certificate_not_resident",
                                      ErrorCode::kMissingReference);
  }
  return Result<StopCertificateDTO>::Ok((*found)->certificate);
}

std::optional<StopFaultLatchDTO> ResourceAddressedExecutionKernel::get_stop_fault_latch() const {
  std::lock_guard state_lock(state_mutex_);
  return active_stop_fault_latch_;
}

StopFaultLatchOriginDTO ResourceAddressedExecutionKernel::get_stop_fault_latch_origin(
    const StopFaultLatchDTO& latch) const {
  std::lock_guard state_lock(state_mutex_);
  for (const auto& stop : attributed_containment_stops_) {
    if (stop.stop_id == latch.stop_id && stop.stop_generation == latch.stop_generation) {
      return stop.origin;
    }
  }
  for (const auto& record : stop_terminal_history_) {
    if (record != nullptr && record->certificate.stop_id == latch.stop_id &&
        record->certificate.stop_generation == latch.stop_generation) {
      return stop_fault_latch_certificate_origin(record->certificate);
    }
  }
  return StopFaultLatchOriginDTO::kUnattributed;
}

Result<std::optional<StopFaultRecoveryOfferDTO>>
ResourceAddressedExecutionKernel::get_stop_fault_recovery_offer() const {
  // Owner before state lock (the owner's scope callback takes the state lock).
  const bool closing_only = cell_admission_owner_->AuthoringClosedOnlyByClosing();
  std::lock_guard state_lock(state_mutex_);
  if (!active_stop_fault_latch_.has_value() ||
      active_stop_fault_latch_->state == StopFaultLatchStateDTO::kCleared) {
    return Result<std::optional<StopFaultRecoveryOfferDTO>>::Ok(std::nullopt);
  }
  if (!active_stop_fault_latch_->stop_certificate.has_value()) {
    // Publish the real fault immediately, but no actionable recovery context
    // before terminal containment. The current response already carries its
    // waiting status; an empty expectation table cannot authorize recovery.
    return Result<std::optional<StopFaultRecoveryOfferDTO>>::Ok(
        StopFaultRecoveryOfferDTO{*active_stop_fault_latch_, {}, std::nullopt});
  }
  const auto terminal = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr &&
               record->certificate.stop_id == active_stop_fault_latch_->stop_id &&
               record->certificate.stop_generation == active_stop_fault_latch_->stop_generation &&
               record->status.fault_latch.has_value() &&
               *record->status.fault_latch == *active_stop_fault_latch_;
      });
  if (terminal == stop_terminal_history_.end()) {
    return reject<std::optional<StopFaultRecoveryOfferDTO>>(
        "active Stop fault latch has no exact resident terminal status",
        "stop_fault_recovery_offer_terminal_missing", ErrorCode::kInvariantViolation);
  }

  // One shared owner derives this table and the native facts table. A
  // cell-wide Stop that captured no participant configuration (for example
  // a recognized-malformed Stop promoted to Full while idle) is offered the
  // installed control domains' active configurations. When no such proof
  // exists the latch is still published, explicitly unavailable.
  std::vector<const RuntimeParticipantBinding*> installed_bindings;
  installed_bindings.reserve(registrations_.size());
  for (const auto& registration : registrations_) {
    installed_bindings.push_back(&registration.binding);
  }
  auto expectations =
      StopFaultRecoveryConfigurationExpectations((*terminal)->status, installed_bindings);
  if (!expectations.ok() &&
      expectations.error().code == kStopFaultRecoveryTerminalConfigurationConflict) {
    return reject<std::optional<StopFaultRecoveryOfferDTO>>(
        "Stop terminal status contains conflicting control-domain configuration truth",
        "stop_fault_recovery_offer_configuration_conflict", ErrorCode::kInvariantViolation);
  }
  StopFaultRecoveryOfferDTO offer{*active_stop_fault_latch_, {}, std::nullopt};
  if (stop_fault_admission_scope_locked() == StopFaultAdmissionScope::kClosing) {
    // D6: Scenario Close keeps the latch; recovery is unavailable until Close
    // ends. `stop_fault_recovery_closing` also says that authoring is closed
    // only by the closing scope (SX-370): the one fact the canonical-empty
    // preparation may waive. Any other blocker reports `_not_ready`.
    offer.recovery_unavailable_reason =
        closing_only ? "stop_fault_recovery_closing" : "stop_fault_recovery_closing_not_ready";
  } else if (expectations.ok()) {
    offer.configuration_expectations = std::move(expectations.value());
  } else {
    offer.recovery_unavailable_reason = expectations.error().code;
  }
  if (stop_fault_recovery_offer_field_error(offer).has_value()) {
    return reject<std::optional<StopFaultRecoveryOfferDTO>>(
        "Stop fault-recovery offer is not a complete canonical owner projection",
        "stop_fault_recovery_offer_invalid", ErrorCode::kInvariantViolation);
  }
  return Result<std::optional<StopFaultRecoveryOfferDTO>>::Ok(std::move(offer));
}

Result<void> ResourceAddressedExecutionKernel::publish_stop_fault_assessment_locked(
    const StopStatusDTO& expected, const StopFaultLatchDTO& assessed) {
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(),
      [&](const auto& record) { return record != nullptr && record->status == expected; });
  if (found == stop_terminal_history_.end() || active_stop_fault_latch_ != expected.fault_latch) {
    return Result<void>::Err(error("stop_fault_recovery_assessment_conflict",
                                   "Stop fault truth changed during assessment",
                                   "stop_fault_recovery_assessment_conflict"));
  }
  (*found)->fault_latch = assessed;
  (*found)->status.fault_latch = assessed;
  active_stop_fault_latch_ = assessed;
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::refresh_stop_fault_recovery_assessment() {
  std::lock_guard recovery_lock(stop_fault_recovery_mutex_);
  const auto admission = cell_admission_owner_->RefreshStopFaultAdmission();
  if (!admission.ok()) return admission;
  std::optional<StopStatusDTO> status;
  {
    std::lock_guard state_lock(state_mutex_);
    if (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
        !active_stop_fault_latch_->stop_certificate.has_value()) {
      return Result<void>::Ok();
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->fault_latch == active_stop_fault_latch_;
        });
    if (found == stop_terminal_history_.end()) {
      return Result<void>::Err(error("stop_fault_recovery_terminal_missing",
                                     "Stop terminal truth is not resident",
                                     "stop_fault_recovery_terminal_missing"));
    }
    status = (*found)->status;
  }
  if (!config_.stop_fault_recovery_facts_resolver) {
    return Result<void>::Err(error("stop_fault_recovery_facts_unavailable",
                                   "Stop recovery facts owner is not installed",
                                   "stop_fault_recovery_facts_unavailable"));
  }
  auto facts = config_.stop_fault_recovery_facts_resolver(*status, clock_.now());
  if (!facts.ok()) return Result<void>::Err(facts.error());
  auto assessed = AssessStopFaultRecovery(*status, facts.value());
  if (!assessed.ok()) return Result<void>::Err(assessed.error());
  std::lock_guard state_lock(state_mutex_);
  return publish_stop_fault_assessment_locked(*status, assessed.value());
}

namespace {

[[nodiscard]] Result<StopFaultRecoveryResultDTO> stop_fault_recovery_closing() {
  return Result<StopFaultRecoveryResultDTO>::Err(
      error("stop_fault_recovery_closing",
            "Scenario Close keeps the Stop fault; use Recover cell after Close",
            "stop_fault_kept_by_close"));
}

}  // namespace

Result<StopFaultRecoveryResultDTO> ResourceAddressedExecutionKernel::recover_stop_fault(
    const StopFaultRecoveryIntentDTO& intent) {
  if (stop_fault_recovery_intent_field_error(intent).has_value()) {
    return reject<StopFaultRecoveryResultDTO>("Stop fault-recovery intent is invalid",
                                              "stop_fault_recovery_intent_invalid",
                                              ErrorCode::kInvalidRequest);
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<StopFaultRecoveryResultDTO>::Err(
        error("stale_request_owner_epoch", "Stop fault recovery names a stale owner epoch",
              "stale_request_owner_epoch"));
  }

  // Recovery is an ordinary, durable control path. Serialize it independently
  // from emergency Stop so two clear attempts cannot both evaluate one latch
  // and race their post-commit publication.
  std::lock_guard recovery_lock(stop_fault_recovery_mutex_);

  std::optional<StopStatusDTO> status;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        stop_fault_recovery_replay_ledger_.begin(), stop_fault_recovery_replay_ledger_.end(),
        [&](const auto& row) {
          return row.request_id() == intent.request_context.request_id;
        });
    if (replay != stop_fault_recovery_replay_ledger_.end()) {
      if (!std::holds_alternative<StopFaultRecoveryIntentDTO>(replay->intent) ||
          std::get<StopFaultRecoveryIntentDTO>(replay->intent) != intent) {
        return Result<StopFaultRecoveryResultDTO>::Err(
            error("request_id_conflict", "Stop fault-recovery request ID conflicts",
                  "request_id_conflict"));
      }
      auto result = std::get<StopFaultRecoveryResultDTO>(replay->result);
      result.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<StopFaultRecoveryResultDTO>::Ok(std::move(result));
    }
    // D6: no recovery clears the latch while Scenario Close runs (Recover
    // cell clears it after Close). Rechecked before the durable clear below.
    if (stop_fault_admission_scope_locked() == StopFaultAdmissionScope::kClosing)
      return stop_fault_recovery_closing();
    if (std::any_of(stop_fault_recovery_replay_ledger_.begin(),
          stop_fault_recovery_replay_ledger_.end(),
          [](const auto& row) { return row.pending_global_record.has_value(); }))
      return Result<StopFaultRecoveryResultDTO>::Err(
          error("invalid_state_transition", "A durable global recovery is unresolved",
                "io_global_rule_recovery_pending"));
    const auto request_id = intent.request_context.request_id;
    const bool request_id_used_elsewhere =
        emergency_stop_reserve_.find(request_id.value()).has_value() ||
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& row) {
                      return row.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(runtime_scale_control_replay_ledger_.begin(),
                    runtime_scale_control_replay_ledger_.end(),
                    [&](const RuntimeScaleControlReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    });
    if (request_id_used_elsewhere) {
      return Result<StopFaultRecoveryResultDTO>::Err(
          error("request_id_conflict", "Stop fault-recovery request ID is already retained",
                "request_id_conflict"));
    }
    if (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
        active_stop_fault_latch_->latch_id != intent.expected_latch_id ||
        active_stop_fault_latch_->latch_generation != intent.expected_latch_generation ||
        !active_stop_fault_latch_->stop_certificate.has_value()) {
      return reject<StopFaultRecoveryResultDTO>("Stop fault latch is not current",
                                                "stop_fault_recovery_latch_stale",
                                                ErrorCode::kInvalidStateTransition);
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->fault_latch.has_value() &&
                 record->fault_latch->latch_id == intent.expected_latch_id &&
                 record->fault_latch->latch_generation == intent.expected_latch_generation;
        });
    if (found == stop_terminal_history_.end()) {
      return reject<StopFaultRecoveryResultDTO>("Stop terminal truth is not resident",
                                                "stop_fault_recovery_terminal_missing",
                                                ErrorCode::kMissingReference);
    }
    status = (*found)->status;
  }
  // Preserve exact recovery replay even if admission-generation publication is
  // temporarily unavailable. New recovery work still requires the current
  // Stop restriction to be durably reflected before evaluation proceeds.
  const auto admission = cell_admission_owner_->RefreshStopFaultAdmission();
  if (!admission.ok()) return Result<StopFaultRecoveryResultDTO>::Err(admission.error());
  if (!config_.stop_fault_recovery_facts_resolver) {
    return reject<StopFaultRecoveryResultDTO>("Stop recovery facts owner is not installed",
                                              "stop_fault_recovery_facts_unavailable",
                                              ErrorCode::kMissingReference);
  }
  auto facts = config_.stop_fault_recovery_facts_resolver(*status, clock_.now());
  const auto closing = [&] {
    std::lock_guard state_lock(state_mutex_);
    return stop_fault_admission_scope_locked() == StopFaultAdmissionScope::kClosing;
  };
  // Close may have retired the latched Scenario while facts were resolved.
  if (closing()) return stop_fault_recovery_closing();
  if (!facts.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(facts.error());
  }
  auto evaluated = EvaluateStopFaultRecovery(intent, *status, facts.value());
  if (!evaluated.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(evaluated.error());
  }
  if (!evaluated.value().result.accepted) {
    std::lock_guard state_lock(state_mutex_);
    // A current-facts assessment is owner truth even when the caller's
    // revision is stale. Publish the newest non-cleared latch so the next
    // exact request can name it; this performs no clear and no external
    // effect.
    auto published = publish_stop_fault_assessment_locked(*status, evaluated.value().result.latch);
    if (!published.ok()) return Result<StopFaultRecoveryResultDTO>::Err(published.error());
    stop_fault_recovery_replay_ledger_.clear();
    stop_fault_recovery_replay_ledger_.push_back(
        StopFaultRecoveryReplayEntry{intent, evaluated.value().result});
    return Result<StopFaultRecoveryResultDTO>::Ok(std::move(evaluated.value().result));
  }
  // D6 (no new lock): the last check before the durable clear. A Close that
  // retires between this check and the write still finds the latch cleared.
  if (closing()) return stop_fault_recovery_closing();
  const auto persisted = journal_.RecordStopFaultCleared(
      config_.request_owner_epoch, intent, evaluated.value().result, evaluated.value().result.latch,
      *evaluated.value().resulting_stop_status);
  if (!persisted.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(persisted.error());
  }
  std::unique_lock state_lock(state_mutex_);
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->fault_latch.has_value() &&
               record->fault_latch->latch_id == intent.expected_latch_id &&
               record->fault_latch->latch_generation == intent.expected_latch_generation;
      });
  if (found == stop_terminal_history_.end()) {
    motion_admission_closed_ = true;
    return reject<StopFaultRecoveryResultDTO>("Stop fault truth changed after durable clear",
                                              "stop_fault_recovery_postcommit_conflict",
                                              ErrorCode::kInvariantViolation);
  }
  (*found)->fault_latch = evaluated.value().result.latch;
  (*found)->status = *evaluated.value().resulting_stop_status;
  (*found)->surveillance_state.reset();
  if (active_stop_fault_latch_.has_value() &&
      active_stop_fault_latch_->latch_id == intent.expected_latch_id &&
      active_stop_fault_latch_->latch_generation == intent.expected_latch_generation) {
    active_stop_fault_latch_ = evaluated.value().result.latch;
  }
  refresh_stop_fault_latch_locked();
  stop_fault_recovery_replay_ledger_.clear();
  stop_fault_recovery_replay_ledger_.push_back(
      StopFaultRecoveryReplayEntry{intent, evaluated.value().result});
  // Orphan fences are resource-scoped and enforced by the slot table. A
  // successful durable fault clear removes the cell-wide Stop latch without
  // weakening any remaining orphan resource.
  // A containment can begin after the external recovery facts read, before
  // its Stop is accepted (or acceptance can fail). Its existing pending row
  // is already restrictive truth. Keep the historical clear/replay truthful
  // without reopening admission under that newer incident.
  // A restart-only fault (internal or journal write) keeps it closed.
  motion_admission_closed_ =
      restart_required_fault_locked() || manager_liveness_motion_inhibited_ ||
      is_active_stop_fault_latch(active_stop_fault_latch_) ||
      std::any_of(incident_containments_.begin(), incident_containments_.end(),
                  [](const auto& row) { return !row.receipt.has_value(); });
  state_lock.unlock();
  // The clear result is already durable. A failed generation publication keeps
  // authoring closed and is retried by the existing reconciliation lane.
  static_cast<void>(cell_admission_owner_->RefreshStopFaultAdmission());
  return Result<StopFaultRecoveryResultDTO>::Ok(std::move(evaluated.value().result));
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::accept_stop(
    EmergencyStopProjectedIntentDTO projected_intent,
    const PartialDispatchStopScope* partial_dispatch_scope,
    const MalformedStopIngressDTO* malformed_ingress,
    const std::optional<StopFaultTriggerDTO> required_fault_latch,
    const MotionTakeoverStopSelection* takeover_selection,
    const InterruptStopSelection* interrupt_selection,
    std::span<const InterruptStopSelection> disjoint_interrupt_selections) {
  const auto accepted_at_ns = clock_.now();
  StopWorkHandoff handoff;
  const ScopeExit publish([&] { publish_stop_work(handoff); });
  auto result = [&] {
    // Neither ordinary admission nor its durable journal is on this lane.
    std::lock_guard state_lock(state_mutex_);
    return accept_stop_locked(std::move(projected_intent), handoff, accepted_at_ns,
                              partial_dispatch_scope, malformed_ingress, required_fault_latch,
                              takeover_selection, interrupt_selection,
                              disjoint_interrupt_selections);
  }();
  return result;
}

void ResourceAddressedExecutionKernel::publish_stop_work(const StopWorkHandoff& handoff) noexcept {
  if (handoff.control.has_value()) {
    stop_control_executor_->Publish(*handoff.control);
  }
  if (handoff.projection.has_value()) {
    stop_projection_worker_->Publish(*handoff.projection);
  }
}

bool ResourceAddressedExecutionKernel::ordinary_request_id_retained_locked(
    const RequestId& request_id) const {
  return std::any_of(stop_fault_recovery_replay_ledger_.begin(),
                     stop_fault_recovery_replay_ledger_.end(), [&](const auto& row) {
                       return std::holds_alternative<IoGlobalRuleRecoveryIntentDTO>(row.intent) &&
                              row.request_id() == request_id;
                     }) ||
         std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                     [&](const auto& row) {
                       return row.intent.request_context.request_id == request_id;
                     }) ||
         std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                     [&](const auto& row) {
                       return row.request.request_context.request_id == request_id;
                     }) ||
         std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                     [&](const auto& row) {
                       return row.request.request_context.request_id == request_id;
                     }) ||
         std::any_of(runtime_scale_control_replay_ledger_.begin(),
                     runtime_scale_control_replay_ledger_.end(), [&](const auto& row) {
                       return row.request.request_context.request_id == request_id;
                     });
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::accept_stop_locked(
    EmergencyStopProjectedIntentDTO projected_intent, StopWorkHandoff& handoff,
    const TimePoint accepted_at_ns, const PartialDispatchStopScope* partial_dispatch_scope,
    const MalformedStopIngressDTO* malformed_ingress,
    const std::optional<StopFaultTriggerDTO> required_fault_latch,
    const MotionTakeoverStopSelection* takeover_selection,
    const InterruptStopSelection* primary_interrupt_selection,
    std::span<const InterruptStopSelection> disjoint_interrupt_selections,
    const GlobalInterruptStopSelection* global_selection) {
  if (global_selection) {
    const auto valid = validate_global_io_rule_stop_locked(*global_selection);
    if (!valid.ok()) return Result<StopAcceptedDTO>::Err(valid.error());
  }
  std::vector<const InterruptStopSelection*> interrupt_selections;
  if (primary_interrupt_selection) interrupt_selections.push_back(primary_interrupt_selection);
  for (const auto& selection : disjoint_interrupt_selections)
    interrupt_selections.push_back(&selection);
  std::set<std::pair<OperationId, OperationParticipantId>> interrupt_memberships;
  for (const auto* interrupt_selection : interrupt_selections) {
    const auto* exact =
        std::get_if<IoOperationRuleScopeDTO>(&interrupt_selection->registration.scope);
    if (!exact || !interrupt_memberships.emplace(exact->operation_id, exact->participant_id).second)
      return reject<StopAcceptedDTO>("interrupt response repeats an exact participant",
                                     "io_rule_response_membership_invalid",
                                     ErrorCode::kInvalidRequest);
    const auto* operation = find_active_operation_locked(exact->operation_id);
    const auto participant_row =
        operation ? std::find_if(operation->participants.begin(), operation->participants.end(),
                                 [&](const auto& row) {
                                   return row.slot_owner.participant_id == exact->participant_id;
                                 })
                  : std::vector<ActiveOperationParticipant>::const_iterator{};
    const auto* participant =
        operation && participant_row != operation->participants.end() ? &*participant_row : nullptr;
    if (!operation || !participant || terminal(operation->state.state) ||
        !io_rule_scope_current_locked(*operation, *participant,
                                      interrupt_selection->registration) ||
        std::find(participant->io_rule_registrations.begin(),
                  participant->io_rule_registrations.end(),
                  interrupt_selection->registration) == participant->io_rule_registrations.end())
      return reject<StopAcceptedDTO>("interrupt response lost its exact prepared owner",
                                     "io_rule_response_scope_stale", ErrorCode::kStaleGeneration);
    const auto* motion =
        std::get_if<PreparedMotionExecutionDTO>(&participant->prepared_participant->payload);
    const auto policy =
        std::find_if(participant->io_motion_rule_policies.begin(),
                     participant->io_motion_rule_policies.end(), [&](const auto& value) {
                       return value.policy_id == interrupt_selection->registration.policy_id;
                     });
    if (policy == participant->io_motion_rule_policies.end() || !policy->semantics ||
        !policy->maximum_scope ||
        policy->maximum_scope->stop_scope != interrupt_selection->stop_scope ||
        policy->effective_priority != interrupt_selection->priority ||
        policy->semantics->preemption_action != interrupt_selection->action)
      return reject<StopAcceptedDTO>("interrupt response lost its compiled action",
                                     "io_rule_response_policy_stale", ErrorCode::kStaleGeneration);
    const bool desired =
        interrupt_selection->action == InterruptPreemptionAction::kCompleteMoveOnSignal;
    if (operation->state.active_stop_id) {
      const auto* resident = io_rule_response_owner_locked(*operation, *participant);
      if (desired || !policy->semantics->affected_phases.contains(OperationStateKind::kStopping) ||
          !resident ||
          (resident && std::any_of(resident->interrupt_winners.begin(),
                                   resident->interrupt_winners.end(), [&](const auto& winner) {
                                     const auto& owner = std::get<IoOperationRuleScopeDTO>(
                                         winner.registration.scope);
                                     return owner.operation_id == exact->operation_id &&
                                            owner.participant_id == exact->participant_id &&
                                            interrupt_selection->priority <= winner.priority;
                                   })))
        return reject<StopAcceptedDTO>("interrupt cannot replace the current logical Stop owner",
                                       "io_rule_response_priority_stale",
                                       ErrorCode::kStaleGeneration);
    }
    const auto scope =
        motion ? std::find_if(motion->interrupt_scopes.begin(), motion->interrupt_scopes.end(),
                              [&](const auto& value) {
                                return value.scope_id == exact->motion_interrupt_scope_id &&
                                       value.interrupt_policy_id ==
                                           interrupt_selection->registration.policy_id;
                              })
               : std::vector<PreparedMotionInterruptScopeDTO>::const_iterator{};
    const auto* ordinary = std::get_if<StopIntentDTO>(&projected_intent);
    const auto* targets =
        ordinary ? std::get_if<std::vector<StopOperationTargetDTO>>(&ordinary->selector) : nullptr;
    const bool full_cell = std::holds_alternative<FullStopIntentDTO>(projected_intent) &&
                           std::any_of(interrupt_selections.begin(), interrupt_selections.end(),
                                       [](const auto* value) {
                                         return value->stop_scope == IoRuleStopScopeDTO::kFullCell;
                                       });
    if (!motion || scope == motion->interrupt_scopes.end() ||
        scope->preemption_action != interrupt_selection->action || !scope->stop_entry ||
        *scope->stop_entry != interrupt_selection->stop_entry || !scope->success_through ||
        *scope->success_through != interrupt_selection->success_through ||
        !VerifyPreparedIoRuleSuccessThrough(interrupt_selection->plan,
                                            interrupt_selection->stop_entry,
                                            interrupt_selection->success_through)
             .ok() ||
        !VerifyPreparedIoRuleStopEntry(interrupt_selection->plan, interrupt_selection->stop_entry)
             .ok() ||
        (desired ? scope->stop_plan
                 : participant->prepared_participant->controller().ordinary_stop_plan) !=
            interrupt_selection->plan ||
        (!full_cell &&
         (!targets || targets->size() != 1 ||
          targets->front().operation_id != exact->operation_id ||
          targets->front().expected_operation_revision != operation->state.operation_revision)) ||
        (desired && (interrupt_selection->plan.participant_stop_profile_purpose !=
                         ParticipantStopProfilePurposeDTO::kMotionInterruptScope ||
                     interrupt_selection->plan.motion_interrupt_scope_id !=
                         exact->motion_interrupt_scope_id.value())))
      return reject<StopAcceptedDTO>("interrupt response differs from its sealed Stop plan",
                                     "io_rule_stop_plan_mismatch", ErrorCode::kInvalidRequest);
  }
  if (takeover_selection != nullptr) {
    const auto* source = find_active_operation_locked(takeover_selection->incumbent_id);
    if (source == nullptr || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != takeover_selection->candidate_id ||
        source->state.active_stop_id.has_value() || source->stop_effect_in_flight ||
        source->participants.size() != 1U ||
        source->participants.front().non_motion_transaction()) {
      return reject<StopAcceptedDTO>("Takeover Stop selection lost its exact source or candidate",
                                     "motion_takeover_stop_selection_stale", ErrorCode::kExecutionRejected);
    }
    const auto* selected = std::get_if<StopIntentDTO>(&projected_intent);
    const auto* targets = selected == nullptr ? nullptr :
        std::get_if<std::vector<StopOperationTargetDTO>>(&selected->selector);
    if (targets == nullptr || targets->size() != 1U ||
        targets->front().operation_id != source->state.operation_id) {
      return reject<StopAcceptedDTO>("Takeover Stop must select its existing physical source",
                                     "motion_takeover_stop_selection_invalid", ErrorCode::kInvalidRequest);
    }
  }
  struct PostReserveInvariantGuard final {
    bool armed{};
    ~PostReserveInvariantGuard() {
      if (armed) {
        // Once the emergency reserve has accepted a recognized Stop, returning
        // a normal rejection or unwinding would lose its already-linearized
        // containment authority. Restated facts refuse in the reserve's
        // precommit and a fence failure is accepted-degraded, so what remains
        // here is an owner bug or allocation failure (D5): it terminates
        // fail-closed rather than pretending the Stop was rejected.
        std::terminate();
      }
    }
  } post_reserve_guard;
  const auto* ordinary = std::get_if<StopIntentDTO>(&projected_intent);
  const auto* full = std::get_if<FullStopIntentDTO>(&projected_intent);
  const auto* program_run_targets =
      ordinary == nullptr ? nullptr
                          : std::get_if<std::vector<StopProgramRunTargetDTO>>(&ordinary->selector);
  const auto normalized_alternatives = static_cast<unsigned>(ordinary != nullptr) +
                                       static_cast<unsigned>(full != nullptr) +
                                       static_cast<unsigned>(malformed_ingress != nullptr);
  if (normalized_alternatives != 1U ||
      (malformed_ingress != nullptr && !std::holds_alternative<std::monostate>(projected_intent))) {
    return reject<StopAcceptedDTO>("Stop ingress has no normalized request identity",
                                   "stop_ingress_not_normalized", ErrorCode::kInvalidRequest);
  }
  std::optional<FullStopIntentDTO> malformed_full;
  if (malformed_ingress != nullptr) {
    if (malformed_stop_ingress_field_error(*malformed_ingress).has_value()) {
      return reject<StopAcceptedDTO>("recognized malformed Stop ingress is invalid",
                                     "malformed_stop_ingress_invalid", ErrorCode::kInvalidRequest);
    }
    if (malformed_ingress->receiver_owner_epoch != config_.request_owner_epoch) {
      return reject<StopAcceptedDTO>("recognized malformed Stop ingress names a stale owner",
                                     "stale_request_owner_epoch", ErrorCode::kInvalidRequest);
    }
    malformed_full.emplace(PublicRequestContextDTO{malformed_ingress->request_id,
                                                   malformed_ingress->receiver_owner_epoch,
                                                   "recognized_stop_ingress", std::nullopt},
                           StopGeneration(0U), malformed_ingress->attribution);
    full = &*malformed_full;
  }
  // The Kernel resolves a Stop against its own current owner epoch. A stale
  // request epoch survives only as the resolver's stale diagnostic; the
  // accepted, replayed and journaled intent carries the current epoch.
  std::optional<RequestOwnerEpoch> received_owner_epoch;
  if (malformed_ingress == nullptr) {
    auto& received = ordinary != nullptr
                         ? std::get<StopIntentDTO>(projected_intent).request_context
                         : std::get<FullStopIntentDTO>(projected_intent).request_context;
    if (received.request_owner_epoch != config_.request_owner_epoch) {
      received_owner_epoch =
          std::exchange(received.request_owner_epoch, config_.request_owner_epoch);
    }
  }
  const auto& context = ordinary != nullptr ? ordinary->request_context : full->request_context;
  const bool runtime_collision_containment =
      ordinary != nullptr &&
      ordinary->reason == std::optional<std::string>{"runtime_collision_fault"} &&
      ordinary->request_context.source_label == "kernel.runtime_collision";
  const auto& attribution = ordinary != nullptr ? ordinary->attribution : full->attribution;
  if (!attribution.locally_valid()) {
    return reject<StopAcceptedDTO>("Stop ingress attribution is invalid",
                                   "stop_request_attribution_invalid", ErrorCode::kInvalidRequest);
  }
  std::optional<ContentDigest> normalized_digest;
  if (malformed_ingress == nullptr) {
    const auto computed = ordinary != nullptr ? ComputeNormalizedStopIntentDigest(*ordinary)
                                              : ComputeNormalizedStopIntentDigest(*full);
    if (!computed.ok()) {
      return Result<StopAcceptedDTO>::Err(computed.error());
    }
    normalized_digest = computed.value();
  }
  const auto requested_selector_digest =
      malformed_ingress != nullptr
          ? ComputeRequestedStopSelectorDigest(*malformed_ingress)
          : (ordinary != nullptr ? ComputeRequestedStopSelectorDigest(*ordinary)
                                 : ComputeRequestedStopSelectorDigest(*full));
  if (!requested_selector_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(requested_selector_digest.error());
  }

  EmergencyStopAcceptanceOutcome outcome;
  std::optional<StopAcceptedDTO> accepted;
  StopControlWorkToken work_token;
  std::optional<StopControlWorkToken> projection_token;
  bool handoff_work = false;
  {
    // The caller retains state_mutex_ through selection and acceptance.
    if (ordinary != nullptr && malformed_ingress == nullptr) {
      const auto replay =
          std::find_if(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                       [&](const StopReplayEntry& entry) { return entry.matches(*ordinary); });
      if (replay != stop_replay_ledger_.end()) {
        auto replayed = std::get<StopAcceptedDTO>(replay->accepted);
        replayed.replay_status = RequestReplayStatusDTO::kReplayedExact;
        return Result<StopAcceptedDTO>::Ok(std::move(replayed));
      }
    }
    if (program_run_targets != nullptr) {
      std::size_t missing_fences = 0U;
      for (const auto& target : *program_run_targets) {
        const auto retained =
            std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                         [&](const ProgramRunStopFence& fence) {
                           return fence.program_run_id == target.program_run_id;
                         });
        missing_fences += retained == program_run_stop_fences_.end() ? 1U : 0U;
      }
      if (missing_fences >
          config_.request_replay_capacity -
              std::min(config_.request_replay_capacity, program_run_stop_fences_.size())) {
        return reject<StopAcceptedDTO>("Program-run Stop fence capacity is exhausted",
                                       "program_run_stop_fence_capacity_exhausted",
                                       ErrorCode::kInternalError);
      }
    }
    // Every per-robot Stop-time deadline is measured from this acceptance and
    // must stay representable on the Kernel clock (Q51: there is no shared
    // outer budget; this is only the representability bound).
    if (checked_deadline(accepted_at_ns, kMaximumStopDurationNs) < 0) {
      return reject<StopAcceptedDTO>("Stop deadline cannot be represented monotonically",
                                     "stop_deadline_overflow", ErrorCode::kInternalError);
    }
    const auto proposed_number = next_stop_id_;
    if (proposed_number == 0U || proposed_number > kMaximumKernelJsonInteger ||
        cell_stop_generation_.value() >= kMaximumKernelJsonInteger) {
      return reject<StopAcceptedDTO>("Stop identity or generation space is exhausted",
                                     "stop_counter_exhausted", ErrorCode::kInternalError);
    }
    const auto proposed_stop_id = StopId("stop." + config_.request_owner_epoch.value() + "." +
                                         std::to_string(proposed_number));
    const auto proposed_generation = StopGeneration(cell_stop_generation_.value() + 1U);
    const auto slot_snapshot = resource_slots_->coherent_snapshot();
    auto resolver_snapshot = capture_stop_scope_locked(slot_snapshot);
    // Pending newcomers name the same physical arm still presented solely by
    // the incumbent. Only this resolution view is canonicalized; the accepted
    // request/digest and its original authored target remain unchanged.
    std::optional<StopIntentDTO> physical_intent;
    std::vector<StopScopeStaleDiagnosticDTO> alias_stale;
    bool has_motion_alias = false;
    std::optional<FullStopIntentDTO> received_full;
    if (full != nullptr && received_owner_epoch.has_value()) {
      received_full = *full;
      received_full->request_context.request_owner_epoch = *received_owner_epoch;
    }
    if (ordinary) {
      physical_intent = *ordinary;
      if (received_owner_epoch.has_value()) {
        physical_intent->request_context.request_owner_epoch = *received_owner_epoch;
      }
      if (auto* targets = std::get_if<std::vector<StopOperationTargetDTO>>(&physical_intent->selector)) {
        for (auto& target : *targets) {
          for (const auto& [id, owner] : active_operations_) {
            const auto* pending = !owner->pending_motion_takeover.empty() &&
                                          owner->pending_motion_takeover.key() == target.operation_id
                                      ? owner->pending_motion_takeover.mapped().get()
                                : !owner->next_motion_takeover.empty() &&
                                          owner->next_motion_takeover.key() == target.operation_id
                                      ? owner->next_motion_takeover.mapped().get() : nullptr;
            if (!pending) continue;
            has_motion_alias = true;
            if (target.expected_operation_revision != pending->state.operation_revision)
              alias_stale.push_back({StopScopeStaleFieldDTO::kOperationRevision, target.operation_id,
                                     target.expected_operation_revision,
                                     pending->state.operation_revision});
            const auto current_stop = owner->state.active_stop_generation.value_or(StopGeneration(0U));
            if (target.expected_stop_generation != current_stop)
              alias_stale.push_back({StopScopeStaleFieldDTO::kOperationStopGeneration, target.operation_id,
                                     target.expected_stop_generation, current_stop});
            target = {id, owner->state.operation_revision, current_stop};
            break;
          }
        }
        if (has_motion_alias) {
          // Preserve an original incumbent target's stale metadata before
          // deduplicating it against a translated newcomer target.
          for (auto& target : *targets) {
            const auto* current = find_active_operation_locked(target.operation_id);
            if (!current) continue;
            if (target.expected_operation_revision != current->state.operation_revision)
              alias_stale.push_back({StopScopeStaleFieldDTO::kOperationRevision, target.operation_id,
                                     target.expected_operation_revision, current->state.operation_revision});
            const auto generation = current->state.active_stop_generation.value_or(StopGeneration(0U));
            if (target.expected_stop_generation != generation)
              alias_stale.push_back({StopScopeStaleFieldDTO::kOperationStopGeneration, target.operation_id,
                                     target.expected_stop_generation, generation});
            target.expected_operation_revision = current->state.operation_revision;
            target.expected_stop_generation = generation;
          }
        }
        std::sort(targets->begin(), targets->end(), [](const auto& a, const auto& b) {
          return a.operation_id < b.operation_id;
        });
        targets->erase(std::unique(targets->begin(), targets->end(), [](const auto& a, const auto& b) {
          return a.operation_id == b.operation_id;
        }), targets->end());
      }
    }
    auto resolution = ordinary != nullptr ? resolve_stop_scope(*physical_intent, resolver_snapshot)
                                          : resolve_full_stop_scope(received_full ? *received_full : *full,
                                                                    resolver_snapshot);
    if (!resolution.ok()) {
      return Result<StopAcceptedDTO>::Err(resolution.error());
    }
    if (!alias_stale.empty() && !resolution.value().full_stop) {
      resolution.value().stale_diagnostics.insert(resolution.value().stale_diagnostics.end(),
                                                 alias_stale.begin(), alias_stale.end());
      canonicalize_stop_scope_stale_diagnostics(resolution.value().stale_diagnostics);
      if (resolution.value().disposition != StopResolutionDispositionDTO::kAlreadyTerminalNoEffect) {
        resolution.value().disposition = StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
        resolution.value().reason = StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope;
      }
    }
    if (malformed_ingress != nullptr) {
      resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
      resolution.value().reason = StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent;
      resolution.value().full_stop = true;
      resolution.value().stale_diagnostics.clear();
    }
    if (partial_dispatch_scope != nullptr) {
      if (ordinary == nullptr || partial_dispatch_scope->operation_id.empty() ||
          !partial_dispatch_scope->operation_revision.nonzero() ||
          partial_dispatch_scope->participant_ids.empty() ||
          !std::is_sorted(partial_dispatch_scope->participant_ids.begin(),
                          partial_dispatch_scope->participant_ids.end()) ||
          std::adjacent_find(partial_dispatch_scope->participant_ids.begin(),
                             partial_dispatch_scope->participant_ids.end()) !=
              partial_dispatch_scope->participant_ids.end()) {
        return reject<StopAcceptedDTO>("partial-dispatch containment scope is invalid",
                                       "partial_dispatch_scope_invalid",
                                       ErrorCode::kInvariantViolation);
      }
      auto* operation = find_active_operation_locked(partial_dispatch_scope->operation_id);
      if (operation == nullptr ||
          operation->state.operation_revision != partial_dispatch_scope->operation_revision ||
          operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll) {
        return reject<StopAcceptedDTO>("partial-dispatch operation authority changed",
                                       "partial_dispatch_operation_mismatch",
                                       ErrorCode::kInvariantViolation);
      }
      StopScopeResolutionDTO exact_partial{StopResolutionDispositionDTO::kExact,
                                           StopScopeResolutionReasonDTO::kExactCurrentScope,
                                           false,
                                           {partial_dispatch_scope->operation_id},
                                           {},
                                           {},
                                           {}};
      for (const auto& participant_id : partial_dispatch_scope->participant_ids) {
        const auto participant =
            std::find_if(operation->participants.begin(), operation->participants.end(),
                         [&](const auto& candidate) {
                           return candidate.slot_owner.participant_id == participant_id;
                         });
        if (participant == operation->participants.end() ||
            !participant->prepared_dispatch_released ||
            participant->slot_state != ResourceSlotState::kDispatched) {
          return reject<StopAcceptedDTO>(
              "partial-dispatch scope names work not released to a backend",
              "partial_dispatch_participant_not_released", ErrorCode::kInvariantViolation);
        }
        exact_partial.controllable_components.push_back(
            participant->binding.value().controllable_component_id);
        exact_partial.resources.insert(exact_partial.resources.end(),
                                       participant->resources.begin(),
                                       participant->resources.end());
      }
      std::sort(exact_partial.controllable_components.begin(),
                exact_partial.controllable_components.end());
      exact_partial.controllable_components.erase(
          std::unique(exact_partial.controllable_components.begin(),
                      exact_partial.controllable_components.end()),
          exact_partial.controllable_components.end());
      std::sort(exact_partial.resources.begin(), exact_partial.resources.end());
      exact_partial.resources.erase(
          std::unique(exact_partial.resources.begin(), exact_partial.resources.end()),
          exact_partial.resources.end());
      resolution = Result<StopScopeResolutionDTO>::Ok(std::move(exact_partial));
    }
    if (global_selection && !global_selection->everything) {
      StopScopeResolutionDTO exact{StopResolutionDispositionDTO::kExact,
                                    StopScopeResolutionReasonDTO::kExactCurrentScope,
                                    false, {}, {}, {}, {}};
      for (const auto& selected : global_selection->participants) {
        const auto* operation = find_active_operation_locked(selected.operation_id);
        const auto participant = std::find_if(
            operation->participants.begin(), operation->participants.end(), [&](const auto& value) {
              return value.slot_owner.participant_id == selected.participant_id;
            });
        exact.operations.push_back(selected.operation_id);
        exact.controllable_components.push_back(participant->binding.value().controllable_component_id);
        exact.resources.insert(exact.resources.end(), participant->resources.begin(),
                                participant->resources.end());
      }
      const auto canonicalize = [](auto& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
      };
      canonicalize(exact.operations);
      canonicalize(exact.controllable_components);
      canonicalize(exact.resources);
      resolution = Result<StopScopeResolutionDTO>::Ok(std::move(exact));
    }
    auto full_resolution =
        ordinary != nullptr
            ? resolve_full_stop_scope(
                  FullStopIntentDTO{context, cell_stop_generation_, attribution}, resolver_snapshot)
            : resolution;
    if (!full_resolution.ok()) {
      return Result<StopAcceptedDTO>::Err(full_resolution.error());
    }
    const bool request_id_used_by_ordinary_command =
        ordinary_request_id_retained_locked(context.request_id);
    const bool request_id_used_by_prior_stop =
        ordinary != nullptr && malformed_ingress == nullptr &&
        std::any_of(
            stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
            [&](const StopReplayEntry& entry) { return entry.request_id() == context.request_id; });
    if (request_id_used_by_ordinary_command || request_id_used_by_prior_stop) {
      auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
      resolution = full_resolution;
      resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
      resolution.value().reason = StopScopeResolutionReasonDTO::kRequestIdentityConflict;
      resolution.value().stale_diagnostics = std::move(retained_diagnostics);
    }
    const bool request_id_retained_by_stop =
        request_id_used_by_prior_stop ||
        emergency_stop_reserve_.find(context.request_id.value()).has_value();

    StopScopeResolutionDTO proof_captured_resolution;
    std::vector<internal::StopProofPredecessorScope> proof_predecessor_scopes;
    std::vector<StopCertificateDTO> proof_predecessor_certificates;
    std::vector<StopPredecessorAcceptanceRefDTO> proof_acceptance_refs;
    std::vector<OperationId> proof_operations;
    struct ProofSource final {
      const StopTerminalRecord* record{};
      internal::StopProofPredecessorScope scope;
    };
    std::vector<ProofSource> proof_sources;
    const auto clear_proof_state = [&] {
      proof_predecessor_scopes.clear();
      proof_predecessor_certificates.clear();
      proof_acceptance_refs.clear();
      proof_operations.clear();
      proof_sources.clear();
    };
    const auto has_settled_proof = [&](const OperationId& operation_id) {
      auto* operation = find_active_operation_locked(operation_id);
      if (operation == nullptr || !operation->settled_stop.has_value() ||
          !operation->settled_stop->certificate.has_value() || operation->pending_control.has_value() ||
          operation->stop_effect_in_flight || operation->dispatch_effect_in_flight() ||
          operation->recovery_effect_in_flight || operation->coordinated_cancellation_in_flight ||
          operation->state.state == OperationStateKind::kInterrupted)
        return false;
      const auto& settled = *operation->settled_stop;
      const auto found_record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->accepted.has_value() &&
                   record->certificate.stop_certificate_id ==
                       settled.certificate->stop_certificate_id &&
                   record->certificate.stop_certificate_revision ==
                       settled.certificate->stop_certificate_revision &&
                   record->certificate.stop_id == settled.stop_id &&
                   record->certificate.stop_generation == settled.generation;
          });
      if (found_record == stop_terminal_history_.end()) return false;
      const auto& record = **found_record;
      const auto& certificate = record.certificate;
      const auto& accepted_source = *record.accepted;
      const auto terminal_result = operation->terminal_stop_result();
      if (record.status.terminal_projection.state == StopDurableProjectionStateDTO::kFailed ||
          !terminal_result.has_value() || *terminal_result != certificate.result)
        return false;
      const auto accepted_operation = std::find_if(
          accepted_source.affected_operation_revisions.begin(),
          accepted_source.affected_operation_revisions.end(), [&](const auto& row) {
            return row.operation_id == operation_id;
          });
      if (accepted_operation == accepted_source.affected_operation_revisions.end() ||
          operation->state.operation_revision != accepted_operation->operation_revision_after_acceptance)
        return false;

      ProofSource proof_source{&record, {*settled.certificate, {}}};
      for (const auto& participant : operation->participants) {
        if (participant.non_motion_transaction() || participant.ownership_transferred) continue;
        if (participant.dispatch_effect_in_flight || participant.reconciliation_in_flight ||
            participant.prepare_in_flight || participant.prepared_advance_in_flight ||
            participant.preparation_borrow_in_flight() || participant.deferred_stop_settlement.has_value())
          return false;
        const auto& participant_id = participant.slot_owner.participant_id;
        const StopScopeEntryKeyDTO key{
            settled.stop_id, settled.generation,
            StopParticipantScopeKeyDTO{operation_id, participant_id}};
        const auto entry = std::find_if(certificate.scope_entries.begin(), certificate.scope_entries.end(),
                                        [&](const auto& row) { return row.key == key; });
        const auto state_row = std::find_if(operation->state.participants.begin(),
                                            operation->state.participants.end(),
                                            [&](const auto& row) {
                                              return row.participant_id == participant_id;
                                            });
        // The earlier certificate's own row is the proof, whatever kind of
        // Stop wrote it: this exact dispatched motion was physically
        // contained and its participant settled. The one exception is an
        // interrupt source lost under that Stop (the same fact that ended the
        // participant Failed in ProgressStop): it is stopped physically again.
        if (entry == certificate.scope_entries.end() || state_row == operation->state.participants.end() ||
            !std::holds_alternative<StopParticipantScopeFactsDTO>(entry->facts) ||
            std::any_of(state_row->io_rule_outcomes.begin(), state_row->io_rule_outcomes.end(),
                        [](const auto& outcome) { return !outcome.evidence.source_losses.empty(); }) ||
            std::find(certificate.unsettled.begin(), certificate.unsettled.end(), key) !=
                certificate.unsettled.end() ||
            std::find(certificate.failed.begin(), certificate.failed.end(), key) !=
                certificate.failed.end() ||
            std::find(certificate.dispatched_stopped.begin(), certificate.dispatched_stopped.end(), key) ==
                certificate.dispatched_stopped.end())
          return false;
        const auto& facts = std::get<StopParticipantScopeFactsDTO>(entry->facts);
        const auto& binding = participant.binding.value();
        if (entry->dispatch_state != StopScopeDispatchStateDTO::kDispatched ||
            facts.terminal_settlement !=
                StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled ||
            facts.operation_revision_after_acceptance !=
                accepted_operation->operation_revision_after_acceptance ||
            facts.controllable_component_id != state_row->controllable_component_id ||
            facts.command_target_id != state_row->command_target_id ||
            facts.capability_id != state_row->capability_id ||
            facts.controller_goal_id != state_row->command_id ||
            facts.controller_goal_generation != state_row->command_generation ||
            facts.resources != participant.resources || !binding.control_path.has_value())
          return false;
        const auto& path = *binding.control_path;
        if (std::tie(facts.control_domain_id, facts.control_domain_revision, facts.control_binding_id,
                     facts.control_binding_revision, facts.active_control_configuration_id,
                     facts.active_control_configuration_generation, facts.controller_topology_id,
                     facts.topology_revision, facts.control_route_id, facts.provider_runtime_epoch) !=
            std::tie(path.control_domain_id, path.control_domain_revision, path.control_binding_id,
                     path.control_binding_revision, path.active_control_configuration_id,
                     path.active_control_configuration_generation, path.controller_topology_id,
                     path.topology_revision, path.control_route_id, path.provider_runtime_epoch))
          return false;
        if (certificate.terminal_error.has_value()) {
          const auto operation_detail = certificate.terminal_error->details.find("operation_id");
          const bool attributed =
              (operation_detail != certificate.terminal_error->details.end() &&
               operation_detail->second == operation_id.value()) ||
              std::any_of(certificate.attempted_stage_results.begin(),
                          certificate.attempted_stage_results.end(), [&](const auto& attempt) {
                            return attempt.scope_key == key && attempt.result.error.has_value() &&
                                   *attempt.result.error == *certificate.terminal_error;
                          });
          if (attributed) return false;
        }
        proof_source.scope.matched_scope_rows.push_back(key);
      }
      if (proof_source.scope.matched_scope_rows.empty()) return false;
      std::sort(proof_source.scope.matched_scope_rows.begin(), proof_source.scope.matched_scope_rows.end(),
                stop_scope_entry_key_less);
      const auto existing = std::find_if(proof_sources.begin(), proof_sources.end(),
                                         [&](const auto& candidate) {
                                           return candidate.scope.certificate.stop_id ==
                                                      proof_source.scope.certificate.stop_id &&
                                                  candidate.scope.certificate.stop_generation ==
                                                      proof_source.scope.certificate.stop_generation;
                                         });
      if (existing == proof_sources.end()) proof_sources.push_back(std::move(proof_source));
      else existing->scope.matched_scope_rows.insert(existing->scope.matched_scope_rows.end(),
                                                      proof_source.scope.matched_scope_rows.begin(),
                                                      proof_source.scope.matched_scope_rows.end());
      return true;
    };
    proof_captured_resolution = resolution.value();
    if (malformed_ingress == nullptr &&
        resolution.value().disposition != StopResolutionDispositionDTO::kPromotedToFull) {
      for (const auto& operation_id : proof_captured_resolution.operations)
        if (has_settled_proof(operation_id)) proof_operations.push_back(operation_id);
      for (auto& proof : proof_sources) {
        std::sort(proof.scope.matched_scope_rows.begin(), proof.scope.matched_scope_rows.end(),
                  stop_scope_entry_key_less);
        proof.scope.matched_scope_rows.erase(
            std::unique(proof.scope.matched_scope_rows.begin(), proof.scope.matched_scope_rows.end()),
            proof.scope.matched_scope_rows.end());
        proof_predecessor_scopes.push_back(proof.scope);
        const auto source_ref = proof.scope.certificate;
        proof_predecessor_certificates.push_back(proof.record->certificate);
        proof_acceptance_refs.push_back({source_ref.stop_id, source_ref.stop_generation,
                                         proof.record->accepted->captured_scope_digest});
        for (const auto& row : proof.scope.matched_scope_rows) {
          if (row.origin_stop_id == source_ref.stop_id &&
              row.origin_stop_generation == source_ref.stop_generation) continue;
          const auto ancestor = std::find_if(
              proof.record->accepted->joined_predecessor_stops.begin(),
              proof.record->accepted->joined_predecessor_stops.end(), [&](const auto& ref) {
                return ref.predecessor_stop_id == row.origin_stop_id &&
                       ref.predecessor_stop_generation == row.origin_stop_generation;
              });
          if (ancestor == proof.record->accepted->joined_predecessor_stops.end()) {
            clear_proof_state();
            break;
          }
          proof_acceptance_refs.push_back(*ancestor);
        }
        if (proof_predecessor_scopes.empty()) break;
      }
      std::sort(proof_predecessor_scopes.begin(), proof_predecessor_scopes.end(),
                [](const auto& a, const auto& b) {
                  return a.certificate.stop_id < b.certificate.stop_id;
                });
      std::sort(proof_predecessor_certificates.begin(), proof_predecessor_certificates.end(),
                [](const auto& a, const auto& b) { return a.stop_id < b.stop_id; });
      std::sort(proof_acceptance_refs.begin(), proof_acceptance_refs.end(),
                [](const auto& a, const auto& b) {
                  return a.predecessor_stop_id < b.predecessor_stop_id;
                });
      proof_acceptance_refs.erase(
          std::unique(proof_acceptance_refs.begin(), proof_acceptance_refs.end()),
          proof_acceptance_refs.end());
      if (proof_acceptance_refs.size() > kMaximumStopPredecessors ||
          proof_predecessor_scopes.size() != proof_predecessor_certificates.size())
        clear_proof_state();
      if (!proof_operations.empty()) {
        const auto physical_operations = [&] {
          auto selected = proof_captured_resolution.operations;
          std::erase_if(selected, [&](const auto& operation_id) {
            return std::binary_search(proof_operations.begin(), proof_operations.end(), operation_id);
          });
          return selected;
        }();
        std::vector<ResourceId> physical_resources;
        std::vector<ResourceId> proof_resources;
        std::vector<ControllableComponentId> physical_components;
        std::vector<ControllableComponentId> proof_components;
        const auto collect_scope = [&](const OperationId& operation_id, auto& resources,
                                       auto& components) {
          const auto operation = std::lower_bound(
              resolver_snapshot.operations.begin(), resolver_snapshot.operations.end(), operation_id,
              [](const auto& row, const auto& id) { return row.operation_id < id; });
          if (operation == resolver_snapshot.operations.end() || operation->operation_id != operation_id)
            return;
          resources.insert(resources.end(), operation->resources.begin(), operation->resources.end());
          components.insert(components.end(), operation->controllable_components.begin(),
                            operation->controllable_components.end());
        };
        for (const auto& operation_id : physical_operations)
          collect_scope(operation_id, physical_resources, physical_components);
        for (const auto& operation_id : proof_operations)
          collect_scope(operation_id, proof_resources, proof_components);
        const auto canonicalize = [](auto& values) {
          std::sort(values.begin(), values.end());
          values.erase(std::unique(values.begin(), values.end()), values.end());
        };
        canonicalize(physical_resources);
        canonicalize(proof_resources);
        canonicalize(physical_components);
        canonicalize(proof_components);
        auto physical_resolution = proof_captured_resolution;
        physical_resolution.operations = physical_operations;
        std::erase_if(physical_resolution.resources, [&](const auto& resource) {
          const auto slot = std::lower_bound(
              slot_snapshot.slots.begin(), slot_snapshot.slots.end(), resource,
              [](const auto& row, const auto& id) { return row.resource_id < id; });
          const bool orphan = slot != slot_snapshot.slots.end() && slot->resource_id == resource &&
                              slot->state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal;
          return !orphan && std::binary_search(proof_resources.begin(), proof_resources.end(), resource) &&
                 !std::binary_search(physical_resources.begin(), physical_resources.end(), resource);
        });
        std::erase_if(physical_resolution.controllable_components, [&](const auto& component) {
          return std::binary_search(proof_components.begin(), proof_components.end(), component) &&
                 !std::binary_search(physical_components.begin(), physical_components.end(), component);
        });
        resolution = Result<StopScopeResolutionDTO>::Ok(std::move(physical_resolution));
      }
    }
    auto captured_digest = ComputeCapturedStopScopeDigest(
        proof_operations.empty() ? resolution.value() : proof_captured_resolution);
    auto full_digest = ComputeCapturedStopScopeDigest(full_resolution.value());
    if (!captured_digest.ok()) {
      return Result<StopAcceptedDTO>::Err(captured_digest.error());
    }
    if (!full_digest.ok()) {
      return Result<StopAcceptedDTO>::Err(full_digest.error());
    }

    const auto transaction_intersects = [&](const StopTransaction& transaction,
                                            const StopScopeResolutionDTO& scope) {
      const bool operation_overlap =
          transaction.accepted.has_value() &&
          std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                      transaction.accepted->affected_operation_revisions.end(),
                      [&](const auto& affected) {
                        return std::binary_search(scope.operations.begin(), scope.operations.end(),
                                                  affected.operation_id);
                      });
      const bool resource_overlap = std::any_of(
          transaction.resources.begin(), transaction.resources.end(), [&](const auto& resource) {
            return std::binary_search(scope.resources.begin(), scope.resources.end(), resource);
          });
      // A global groups/all-motion selection can name only some participants
      // of one operation. Its independent physical resources must not acquire
      // an unrelated resident Stop merely through the enclosing operation ID.
      return resource_overlap ||
             (operation_overlap && (!global_selection || global_selection->everything));
    };
    const auto transaction_covers = [](const StopTransaction& transaction,
                                       const StopScopeResolutionDTO& scope) {
      if (!transaction.accepted.has_value()) {
        return false;
      }
      return std::all_of(scope.operations.begin(), scope.operations.end(),
                         [&](const auto& operation_id) {
                           const auto found = std::lower_bound(
                               transaction.accepted->affected_operation_revisions.begin(),
                               transaction.accepted->affected_operation_revisions.end(),
                               operation_id, [](const auto& affected, const OperationId& id) {
                                 return affected.operation_id < id;
                               });
                           return found !=
                                      transaction.accepted->affected_operation_revisions.end() &&
                                  found->operation_id == operation_id;
                         }) &&
             std::all_of(scope.resources.begin(), scope.resources.end(), [&](const auto& resource) {
               return std::binary_search(transaction.resources.begin(), transaction.resources.end(),
                                         resource);
             });
    };
    const auto resident_transactions = [&](const StopScopeResolutionDTO& scope) {
      std::vector<StopTransaction*> result;
      result.reserve(stop_transactions_.size());
      for (const auto& candidate : stop_transactions_) {
        if (candidate != nullptr && candidate->active && !candidate->terminal &&
            candidate->stop_id.has_value() && candidate->accepted.has_value() &&
            transaction_intersects(*candidate, scope)) {
          result.push_back(candidate.get());
        }
      }
      std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return *left->stop_id < *right->stop_id;
      });
      return result;
    };
    const auto transaction_matches_selector = [&](const StopTransaction& transaction) {
      if (ordinary == nullptr || !transaction.accepted.has_value()) {
        return false;
      }
      return std::visit(
          [&](const auto& targets) {
            using Targets = std::decay_t<decltype(targets)>;
            if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                                   transaction.accepted->affected_operation_revisions.end(),
                                   [&](const auto& affected) {
                                     return affected.operation_id == target.operation_id;
                                   });
              });
            } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::binary_search(transaction.resources.begin(),
                                          transaction.resources.end(), target.resource_id);
              });
            } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(
                    transaction.accepted->affected_operation_revisions.begin(),
                    transaction.accepted->affected_operation_revisions.end(),
                    [&](const auto& affected) {
                      const auto found = std::lower_bound(
                          resolver_snapshot.operations.begin(), resolver_snapshot.operations.end(),
                          affected.operation_id, [](const auto& operation, const OperationId& id) {
                            return operation.operation_id < id;
                          });
                      return found != resolver_snapshot.operations.end() &&
                             found->operation_id == affected.operation_id &&
                             std::binary_search(found->controllable_components.begin(),
                                                found->controllable_components.end(),
                                                target.controllable_component_id);
                    });
              });
            } else {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                                   transaction.accepted->affected_operation_revisions.end(),
                                   [&](const auto& affected) {
                                     const auto found = std::lower_bound(
                                         resolver_snapshot.operations.begin(),
                                         resolver_snapshot.operations.end(), affected.operation_id,
                                         [](const auto& operation, const OperationId& id) {
                                           return operation.operation_id < id;
                                         });
                                     return found != resolver_snapshot.operations.end() &&
                                            found->operation_id == affected.operation_id &&
                                            found->program_run_id == target.program_run_id;
                                   });
              });
            }
          },
          ordinary->selector);
    };

    std::optional<std::string_view> overlapping_stop_id;
    if (ordinary != nullptr && !resolution.value().full_stop) {
      auto resident = resident_transactions(resolution.value());
      if (resident.empty() && resolution.value().disposition ==
                                  StopResolutionDispositionDTO::kAlreadyTerminalNoEffect) {
        for (const auto& candidate : stop_transactions_) {
          if (candidate != nullptr && candidate->active && !candidate->terminal &&
              candidate->stop_id.has_value() && transaction_matches_selector(*candidate)) {
            resident.push_back(candidate.get());
          }
        }
        std::sort(resident.begin(), resident.end(), [](const auto* left, const auto* right) {
          return *left->stop_id < *right->stop_id;
        });
      }
      const auto covering_full =
          std::find_if(resident.begin(), resident.end(), [&](const auto* tx) {
            return tx->full_stop && transaction_covers(*tx, resolution.value());
          });
      if (covering_full != resident.end()) {
        overlapping_stop_id = (*covering_full)->stop_id->value();
      } else if (resident.size() == 1U &&
                 transaction_covers(*resident.front(), resolution.value())) {
        overlapping_stop_id = resident.front()->stop_id->value();
      } else if (!resident.empty()) {
        auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
        resolution = full_resolution;
        resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
        resolution.value().reason = StopScopeResolutionReasonDTO::kOverlappingStopTransactions;
        resolution.value().stale_diagnostics = std::move(retained_diagnostics);
        clear_proof_state();
        captured_digest = ComputeCapturedStopScopeDigest(resolution.value());
        if (!captured_digest.ok()) {
          return Result<StopAcceptedDTO>::Err(captured_digest.error());
        }
      }
    }
    const auto full_predecessors = resident_transactions(full_resolution.value());
    std::vector<StopAcceptedPredecessorInput> full_predecessor_inputs;
    std::vector<StopControlWorkToken> full_predecessor_tokens;
    std::vector<StopCertificateDTO> full_predecessor_certificates;
    full_predecessor_inputs.reserve(full_predecessors.size());
    full_predecessor_tokens.reserve(full_predecessors.size());
    full_predecessor_certificates.reserve(full_predecessors.size());
    for (const auto* predecessor : full_predecessors) {
      full_predecessor_inputs.push_back({*predecessor->accepted});
      if (predecessor->certificate.has_value()) {
        full_predecessor_certificates.push_back(*predecessor->certificate);
      } else {
        full_predecessor_tokens.push_back(predecessor->token);
      }
    }

    const auto find_slot = [&](const ResourceId& resource) {
      const auto found = std::lower_bound(
          slot_snapshot.slots.begin(), slot_snapshot.slots.end(), resource,
          [](const auto& slot, const ResourceId& id) { return slot.resource_id < id; });
      return found != slot_snapshot.slots.end() && found->resource_id == resource ? &*found
                                                                                  : nullptr;
    };
    // A sibling cancelled before dispatch whose reservation the partial
    // dispatch path already released owns nothing this Stop can fence. The
    // typed fact and the coherent snapshot must agree; its resources may since
    // belong to another operation. Any other unowned resource is drift.
    const auto released_pre_dispatch = [&](const ActiveOperationParticipant& participant) {
      return participant.stop_cancelled_pre_dispatch && !participant.prepared_dispatch_released &&
             participant.slot_state == ResourceSlotState::kTerminal &&
             !participant.resources.empty() &&
             std::none_of(participant.resources.begin(), participant.resources.end(),
                          [&](const ResourceId& resource) {
                            const auto* slot = find_slot(resource);
                            return slot != nullptr && slot->owner.has_value() &&
                                   slot->owner->operation_id == participant.slot_owner.operation_id &&
                                   slot->owner->participant_id == participant.slot_owner.participant_id;
                          });
    };
    const auto inherited_by_predecessor = [&](const OperationId& operation_id) {
      return std::any_of(full_predecessor_inputs.begin(), full_predecessor_inputs.end(),
                         [&](const auto& predecessor) {
                           const auto found = std::lower_bound(
                               predecessor.acceptance.affected_operation_revisions.begin(),
                               predecessor.acceptance.affected_operation_revisions.end(),
                               operation_id, [](const auto& affected, const OperationId& id) {
                                 return affected.operation_id < id;
                               });
                           return found != predecessor.acceptance.affected_operation_revisions.end() &&
                                  found->operation_id == operation_id;
                         });
    };
    // One participant scope rule for the precommit check and the fence below.
    const auto stop_participant_in_scope = [&](const bool exact_partial_capture,
                                               const OperationId& operation_id,
                                               const OperationParticipantId& participant_id) {
      if (global_selection && exact_partial_capture)
        return std::any_of(global_selection->participants.begin(),
                            global_selection->participants.end(), [&](const auto& value) {
                              return value.operation_id == operation_id &&
                                     value.participant_id == participant_id;
                            });
      return !exact_partial_capture ||
             (operation_id == partial_dispatch_scope->operation_id &&
              std::binary_search(partial_dispatch_scope->participant_ids.begin(),
                                 partial_dispatch_scope->participant_ids.end(), participant_id));
    };
    // Runs inside the reserve on its own selection, before it commits: every
    // row the fresh fence below will actually name must be held by its
    // recorded owner (stop_fence_batch semantics), and the facts the
    // post-reserve path restates (resident operation, revision bound,
    // prepared Stop plan) must hold, so they refuse here, before any reserve.
    // Replays and joins fence nothing new. Mirrors the post-reserve resolution
    // choice exactly.
    const char* precommit_refusal_message =
        "Stop participant resources are not held by their recorded owner";
    const char* precommit_refusal_code = "stop_operation_not_fenceable";
    const auto fence_rows_held = [&](const EmergencyStopReservePlan& plan) noexcept {
      if (plan.replay || plan.joined_resident) return true;
      if (plan.resident_work_token.has_value()) {
        const auto& token = *plan.resident_work_token;
        if (token.slot < stop_transactions_.size() && stop_transactions_[token.slot] != nullptr &&
            stop_transactions_[token.slot]->active && stop_transactions_[token.slot]->token == token)
          return true;
      }
      const auto* fenced = plan.full_stop && !resolution.value().full_stop ? &full_resolution.value()
                                                                           : &resolution.value();
      if (fenced->full_stop && !full_predecessor_inputs.empty() && !proof_operations.empty())
        fenced = &proof_captured_resolution;
      const bool aggregate = fenced->full_stop && !full_predecessor_inputs.empty();
      const bool exact_partial_capture =
          (partial_dispatch_scope != nullptr || global_selection != nullptr) && !fenced->full_stop;
      const auto refuse = [&](const char* message, const char* code) noexcept {
        precommit_refusal_message = message;
        precommit_refusal_code = code;
        return true;
      };
      return std::none_of(fenced->operations.begin(), fenced->operations.end(), [&](const auto& id) {
        if (aggregate && inherited_by_predecessor(id)) return false;
        const auto* operation = find_active_operation_locked(id);
        if (operation == nullptr)
          return refuse("resolved Stop operation is not resident or exhausted",
                        "stop_operation_not_fenceable");
        auto revision_before_stop = operation->state.operation_revision;
        if (operation->pending_control.has_value() &&
            operation->pending_control->accepted.operation_revision_after_acceptance >
                revision_before_stop)
          revision_before_stop =
              operation->pending_control->accepted.operation_revision_after_acceptance;
        if (revision_before_stop.value() >= kMaximumKernelJsonInteger)
          return refuse("resolved Stop operation revision is exhausted",
                        "stop_operation_not_fenceable");
        return std::any_of(
            operation->participants.begin(), operation->participants.end(),
            [&](const auto& participant) {
              if (released_pre_dispatch(participant)) return false;
              if (std::any_of(participant.resources.begin(), participant.resources.end(),
                              [&](const ResourceId& resource) {
                                const auto* slot = find_slot(resource);
                                return slot == nullptr || slot->owner != participant.slot_owner;
                              }))
                return true;
              if (!stop_participant_in_scope(exact_partial_capture, id,
                                             participant.slot_owner.participant_id) ||
                  participant.non_motion_transaction())
                return false;
              const auto& stop_plan =
                  runtime_collision_containment &&
                          participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                          participant.fault_stop_plan != nullptr
                      ? participant.fault_stop_plan
                      : participant.ordinary_stop_plan;
              return stop_plan == nullptr &&
                     refuse("resolved Stop participant has no prepared plan",
                            "stop_participant_plan_missing");
            });
      });
    };

    const EmergencyStopAcceptanceCandidate candidate{
        context.request_id.value(),
        normalized_digest.has_value() ? std::optional<std::string_view>(normalized_digest->value())
                                      : std::nullopt,
        attribution.transport,
        attribution.assurance,
        attribution.principal_id.has_value()
            ? std::optional<std::string_view>(*attribution.principal_id)
            : std::nullopt,
        malformed_ingress != nullptr ? std::optional<std::string_view>(malformed_ingress->reason)
                                     : std::nullopt,
        proposed_stop_id.value(),
        proposed_generation,
        accepted_at_ns,
        resolution.value().disposition,
        captured_digest.value().value(),
        full_digest.value().value(),
        overlapping_stop_id,
        resolution.value().full_stop};
    outcome = emergency_stop_reserve_.reserve_or_join(candidate, fence_rows_held);
    if (outcome.error == EmergencyStopReserveError::kPrecommitRefused) {
      motion_admission_closed_ = true;
      return reject<StopAcceptedDTO>(precommit_refusal_message, precommit_refusal_code,
                                     ErrorCode::kInvariantViolation);
    }
    if (!outcome.accepted() || outcome.work_token.slot >= stop_transactions_.size() ||
        outcome.correlation_slot >= stop_correlations_.size()) {
      return reject<StopAcceptedDTO>("emergency Stop reserve rejected its prepared candidate",
                                     "emergency_stop_reserve_invariant",
                                     ErrorCode::kInvariantViolation);
    }
    post_reserve_guard.armed = true;

    std::optional<EmergencyStopAffectedStateSnapshotDTO> projection_state;
    if (outcome.replay_status == RequestReplayStatusDTO::kReplayedExact) {
      const auto& retained = stop_correlations_[outcome.correlation_slot];
      if (!retained.has_value()) {
        return reject<StopAcceptedDTO>("Stop reserve replay has no retained result",
                                       "stop_replay_result_missing",
                                       ErrorCode::kInvariantViolation);
      }
      accepted = *retained;
      accepted->replay_status = RequestReplayStatusDTO::kReplayedExact;
      work_token = outcome.work_token;
    } else if (outcome.acceptance_path == EmergencyStopAcceptancePathDTO::kJoinedResident ||
               (stop_transactions_[outcome.work_token.slot]->active &&
                stop_transactions_[outcome.work_token.slot]->token == outcome.work_token)) {
      auto& transaction = *stop_transactions_[outcome.work_token.slot];
      if (!transaction.active || transaction.token != outcome.work_token ||
          !transaction.accepted.has_value() || !transaction.stop_id.has_value()) {
        return reject<StopAcceptedDTO>("joined Stop transaction is not resident",
                                       "stop_join_transaction_missing",
                                       ErrorCode::kInvariantViolation);
      }
      accepted = *transaction.accepted;
      accepted->request_id = context.request_id;
      accepted->replay_status = RequestReplayStatusDTO::kAcceptedNew;
      accepted->requested_selector_digest = requested_selector_digest.value();
      for (const auto* interrupt_selection : interrupt_selections) {
        // Same resident Stop: its accepted time, plan/digest, attempt state and
        // deadline remain untouched. Only the higher-priority logical owner
        // changes, retaining the new rule's actual evidence independently.
        const auto& scope =
            std::get<IoOperationRuleScopeDTO>(interrupt_selection->registration.scope);
        const auto current = std::find_if(
            transaction.interrupt_winners.begin(), transaction.interrupt_winners.end(),
            [&](const auto& winner) {
              const auto& owner = std::get<IoOperationRuleScopeDTO>(winner.registration.scope);
              return owner.operation_id == scope.operation_id &&
                     owner.participant_id == scope.participant_id;
            });
        if (current != transaction.interrupt_winners.end()) {
          const auto& prior = current->registration;
          if (auto* operation = find_active_operation_locked(scope.operation_id)) {
            for (auto& row : operation->state.participants)
              for (auto& rule : row.io_rule_outcomes)
                if (rule.registration == prior)
                  rule.arbitration_disposition = IoRuleArbitrationDispositionDTO::kSuperseded;
          }
          *current = *interrupt_selection;
        } else {
          transaction.interrupt_winners.push_back(*interrupt_selection);
        }
      }
      if (global_selection)
        transaction.global_interrupt_winners.push_back(*global_selection);
      // A containment that joins a resident Stop still latches its outcome.
      if (required_fault_latch.has_value() && !transaction.certificate.has_value()) {
        latch_stop_fault_locked(transaction, *required_fault_latch);
      }
      if (malformed_ingress != nullptr) {
        accepted->resolution_disposition = StopResolutionDispositionDTO::kPromotedToFull;
        accepted->resolution_reason = resolution.value().reason;
        accepted->stale_diagnostics.clear();
      } else if (full != nullptr) {
        // The physical transaction is shared; freshness diagnostics describe
        // this caller's selector, not the request that originally created it.
        accepted->resolution_disposition = resolution.value().disposition;
        accepted->resolution_reason = resolution.value().reason;
        accepted->stale_diagnostics = std::move(resolution.value().stale_diagnostics);
      } else if (!transaction.full_stop) {
        if (resolution.value().disposition ==
            StopResolutionDispositionDTO::kAlreadyTerminalNoEffect) {
          accepted->resolution_disposition =
              resolution.value().stale_diagnostics.empty()
                  ? StopResolutionDispositionDTO::kExact
                  : StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
          accepted->resolution_reason =
              resolution.value().stale_diagnostics.empty()
                  ? StopScopeResolutionReasonDTO::kExactCurrentScope
                  : StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope;
        } else {
          accepted->resolution_disposition = resolution.value().disposition;
          accepted->resolution_reason = resolution.value().reason;
        }
        accepted->stale_diagnostics = std::move(resolution.value().stale_diagnostics);
      }
      accepted->accepted_at_ns = accepted_at_ns;
      if (outcome.correlation_retained) {
        stop_correlations_[outcome.correlation_slot] = *accepted;
      }
      projection_state.emplace();
      projection_state->operations.reserve(accepted->affected_operation_revisions.size());
      for (const auto& affected : accepted->affected_operation_revisions) {
        const auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation != nullptr) {
          projection_state->operations.push_back(operation->state);
        }
      }
      projection_state->resources = transaction.resources;
      work_token = outcome.work_token;
    } else {
      if (outcome.full_stop && !resolution.value().full_stop) {
        auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
        resolution = std::move(full_resolution);
        resolution.value().disposition = outcome.disposition;
        resolution.value().reason = request_id_retained_by_stop
                                        ? StopScopeResolutionReasonDTO::kRequestIdentityConflict
                                        : StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced;
        resolution.value().stale_diagnostics = std::move(retained_diagnostics);
      }
      bool aggregate = resolution.value().full_stop && !full_predecessor_inputs.empty();
      if (aggregate && !proof_operations.empty()) {
        resolution = Result<StopScopeResolutionDTO>::Ok(proof_captured_resolution);
        clear_proof_state();
      }
      auto acceptance_resolution = resolution.value();
      aggregate = acceptance_resolution.full_stop && !full_predecessor_inputs.empty();
      if (aggregate) {
        for (const auto& predecessor : full_predecessor_inputs) {
          for (const auto& affected : predecessor.acceptance.affected_operation_revisions) {
            acceptance_resolution.operations.push_back(affected.operation_id);
          }
        }
        for (const auto* predecessor : full_predecessors) {
          acceptance_resolution.resources.insert(acceptance_resolution.resources.end(),
                                                 predecessor->resources.begin(),
                                                 predecessor->resources.end());
        }
        std::sort(acceptance_resolution.operations.begin(), acceptance_resolution.operations.end());
        acceptance_resolution.operations.erase(std::unique(acceptance_resolution.operations.begin(),
                                                           acceptance_resolution.operations.end()),
                                               acceptance_resolution.operations.end());
        std::sort(acceptance_resolution.resources.begin(), acceptance_resolution.resources.end());
        acceptance_resolution.resources.erase(std::unique(acceptance_resolution.resources.begin(),
                                                          acceptance_resolution.resources.end()),
                                              acceptance_resolution.resources.end());
      }
      std::vector<StopAffectedOperationRevisionDTO> affected_revisions;
      std::vector<PreparedParticipantStopPlanDTO> plans;
      std::vector<ResourceSlotStopFenceOwnerDTO> owner_rows;
      std::vector<ResourceSlotStopFenceOwnerDTO> revision_only_rows;
      std::vector<OperationId> newly_fenced_operations;
      EmergencyStopAffectedStateSnapshotDTO affected_state;
      const bool exact_partial_capture =
          (partial_dispatch_scope != nullptr || global_selection != nullptr) &&
          !acceptance_resolution.full_stop;
      const auto participant_selected = [&](const OperationId& operation_id,
                                            const OperationParticipantId& participant_id) {
        return stop_participant_in_scope(exact_partial_capture, operation_id, participant_id);
      };
      std::size_t participant_capacity = 0U;
      for (const auto& operation_id : resolution.value().operations) {
        const auto* operation = find_active_operation_locked(operation_id);
        if (operation != nullptr) {
          participant_capacity += static_cast<std::size_t>(std::count_if(
              operation->participants.begin(), operation->participants.end(),
              [&](const auto& participant) {
                return participant_selected(operation_id, participant.slot_owner.participant_id) &&
                       !released_pre_dispatch(participant);
              }));
        }
      }
      affected_revisions.reserve(resolution.value().operations.size());
      plans.reserve(participant_capacity);
      owner_rows.reserve(participant_capacity);
      newly_fenced_operations.reserve(resolution.value().operations.size());
      affected_state.operations.reserve(resolution.value().operations.size());
      for (const auto& operation_id : resolution.value().operations) {
        if (aggregate && inherited_by_predecessor(operation_id)) {
          continue;
        }
        auto* operation = find_active_operation_locked(operation_id);
        if (operation == nullptr) {
          return reject<StopAcceptedDTO>("resolved Stop operation is not resident or exhausted",
                                         "stop_operation_not_fenceable",
                                         ErrorCode::kInvariantViolation);
        }
        auto revision_before_stop = operation->state.operation_revision;
        if (operation->pending_control.has_value() &&
            operation->pending_control->accepted.operation_revision_after_acceptance >
                revision_before_stop) {
          revision_before_stop =
              operation->pending_control->accepted.operation_revision_after_acceptance;
        }
        if (revision_before_stop.value() >= kMaximumKernelJsonInteger) {
          return reject<StopAcceptedDTO>("resolved Stop operation revision is exhausted",
                                         "stop_operation_not_fenceable",
                                         ErrorCode::kInvariantViolation);
        }
        const OperationRevision next_revision(revision_before_stop.value() + 1U);
        affected_revisions.push_back({operation_id, next_revision});
        newly_fenced_operations.push_back(operation_id);
        for (const auto& participant : operation->participants) {
          if (released_pre_dispatch(participant)) continue;
          if (!participant_selected(operation_id, participant.slot_owner.participant_id) ||
              participant.non_motion_transaction()) {
            revision_only_rows.push_back({participant.slot_owner, participant.resources, next_revision});
            continue;
          }
          const auto& selected_stop_plan =
              runtime_collision_containment &&
                      participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                      participant.fault_stop_plan != nullptr
                  ? participant.fault_stop_plan
                  : participant.ordinary_stop_plan;
          if (selected_stop_plan == nullptr) {
            return reject<StopAcceptedDTO>("resolved Stop participant has no prepared plan",
                                           "stop_participant_plan_missing",
                                           ErrorCode::kInvariantViolation);
          }
          const auto* effective_plan = selected_stop_plan.get();
          for (const auto* selection : interrupt_selections) {
            const auto& owner = std::get<IoOperationRuleScopeDTO>(selection->registration.scope);
            if (owner.operation_id == operation_id &&
                owner.participant_id == participant.slot_owner.participant_id)
              effective_plan = &selection->plan;
          }
          plans.push_back(*effective_plan);
          owner_rows.push_back(ResourceSlotStopFenceOwnerDTO{participant.slot_owner,
                                                             participant.resources, next_revision});
        }
      }
      std::sort(owner_rows.begin(), owner_rows.end(), [](const auto& left, const auto& right) {
        return std::tie(left.expected_owner.operation_id, left.expected_owner.participant_id) <
               std::tie(right.expected_owner.operation_id, right.expected_owner.participant_id);
      });
      std::sort(revision_only_rows.begin(), revision_only_rows.end(), [](const auto& left, const auto& right) {
        return std::tie(left.expected_owner.operation_id, left.expected_owner.participant_id) <
               std::tie(right.expected_owner.operation_id, right.expected_owner.participant_id);
      });
      std::vector<ResourceId> orphan_resources;
      for (const auto& resource : resolution.value().resources) {
        if (aggregate && std::any_of(full_predecessors.begin(), full_predecessors.end(),
                                     [&](const auto* predecessor) {
                                       return std::binary_search(predecessor->resources.begin(),
                                                                 predecessor->resources.end(),
                                                                 resource);
                                     })) {
          continue;
        }
        const auto found = std::lower_bound(
            slot_snapshot.slots.begin(), slot_snapshot.slots.end(), resource,
            [](const auto& slot, const ResourceId& id) { return slot.resource_id < id; });
        if (found != slot_snapshot.slots.end() && found->resource_id == resource &&
            !found->owner.has_value() &&
            found->state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
          orphan_resources.push_back(resource);
        }
      }
      auto made =
          aggregate
              ? (ordinary != nullptr
                     ? MakeAggregatedStopAccepted(*ordinary, StopId(std::string(outcome.stop_id())),
                                                  outcome.stop_generation, acceptance_resolution,
                                                  affected_revisions, accepted_at_ns,
                                                  full_predecessor_inputs)
                     : MakeAggregatedStopAccepted(*full, StopId(std::string(outcome.stop_id())),
                                                  outcome.stop_generation, acceptance_resolution,
                                                  affected_revisions, accepted_at_ns,
                                                  full_predecessor_inputs))
              : (ordinary != nullptr
                     ? MakeStopAccepted(*ordinary, StopId(std::string(outcome.stop_id())),
                                        outcome.stop_generation, acceptance_resolution,
                                        affected_revisions, accepted_at_ns)
                     : MakeStopAccepted(*full, StopId(std::string(outcome.stop_id())),
                                        outcome.stop_generation, acceptance_resolution,
                                        affected_revisions, accepted_at_ns));
      if (!made.ok()) {
        return Result<StopAcceptedDTO>::Err(made.error());
      }
      accepted = std::move(made.value());
      if (!proof_operations.empty()) {
        accepted->captured_scope_digest = captured_digest.value();
        accepted->joined_predecessor_stops = proof_acceptance_refs;
        if (const auto field = stop_accepted_field_error(*accepted); field.has_value())
          return reject<StopAcceptedDTO>(std::string(*field), "stop_acceptance_proof_invalid",
                                         ErrorCode::kInvariantViolation);
      }
      if (malformed_ingress != nullptr) {
        accepted->requested_selector_digest = requested_selector_digest.value();
      }
      auto fenced =
          owner_rows.empty() && orphan_resources.empty() && revision_only_rows.empty()
              ? Result<ResourceSlotStopFenceResultDTO>::Ok(ResourceSlotStopFenceResultDTO{
                    slot_snapshot.operation_resource_generation, outcome.stop_generation, {}, {}})
              : resource_slots_->stop_fence_batch(ResourceSlotStopFenceRequestDTO{
                    slot_snapshot.operation_resource_generation, outcome.stop_generation,
                    std::move(owner_rows), orphan_resources, std::move(revision_only_rows)});
      // Accepted-degraded: the reserve already linearized this Stop, so a
      // fence failure no longer rejects it. Its work is still handed off;
      // admission closes and the transaction latches a typed Stop fault.
      const bool fence_failed = !fenced.ok();
      if (fence_failed) {
        motion_admission_closed_ = true;
        (void)std::fprintf(stderr,
                           "manyforge_fault code=stop_post_reserve_fence_failed error=%s\n",
                           fenced.error().details.contains("reason")
                               ? fenced.error().details.at("reason").c_str()
                               : fenced.error().code.c_str());
        fenced = Result<ResourceSlotStopFenceResultDTO>::Ok(ResourceSlotStopFenceResultDTO{
            slot_snapshot.operation_resource_generation, outcome.stop_generation, {}, {}});
      }
      // The resource/operation Stop fence is now linearized. Install the
      // matching process-local participant fence before publishing any
      // physical Stop stage so late work from the replaced trajectory cannot
      // win a competing inner terminal. This seam is non-blocking and has no
      // physical effect; containment still belongs exclusively to StopControl.
      for (const auto& affected : affected_revisions) {
        auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation == nullptr) {
          continue;
        }
        for (auto& participant : operation->participants) {
          if (!participant_selected(affected.operation_id, participant.slot_owner.participant_id) ||
              participant.non_motion_transaction() || released_pre_dispatch(participant) ||
              participant.engine == nullptr || participant.preparation_id == 0U) {
            continue;
          }
          const auto fence =
              participant.engine->fence_addressed_participant_stop(participant.preparation_id);
          participant.stop_cancelled_pre_dispatch =
              participant.stop_cancelled_pre_dispatch ||
              fence == AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch;
        }
      }
      for (const auto& affected : affected_revisions) {
        auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation == nullptr) {
          continue;
        }
        if (operation->pending_control.has_value()) {
          operation->pending_control->superseded_by_stop = true;
        }
        operation->state.operation_revision = affected.operation_revision_after_acceptance;
        if (operation->runtime_scale_status.has_value()) {
          operation->runtime_scale_status->operation_revision =
              affected.operation_revision_after_acceptance;
          operation->runtime_scale_status->updated_at_ns = accepted_at_ns;
        }
        for (auto& participant : operation->participants) {
          if (!participant_selected(affected.operation_id, participant.slot_owner.participant_id) ||
              participant.non_motion_transaction() || released_pre_dispatch(participant)) {
            continue;
          }
          participant.stop_containment_owned = true;
          participant.stop_cancelled_pre_dispatch =
              participant.stop_cancelled_pre_dispatch ||
              (participant.payload_kind == OperationParticipantPayloadKind::kMotion
                   ? !participant.prepared_dispatch_released
                   : (!participant.prepared_dispatch_released &&
                      participant.slot_state == ResourceSlotState::kReserved));
        }
        // Stop owns the public terminal from acceptance through aggregate
        // settlement. A participant that never crossed the dispatch barrier
        // is recorded below, but the operation remains Stopping until that
        // participant owner has committed the same no-effect outcome.
        // Preserve each observed non-motion row while moving rows enter Stop.
        operation->state.state = OperationStateKind::kStopping;
        operation->state.active_stop_id = StopId(std::string(outcome.stop_id()));
        operation->state.active_stop_generation = outcome.stop_generation;
        operation->state.updated_at_ns = accepted_at_ns;
        for (auto& participant_state : operation->state.participants) {
          if (participant_selected(affected.operation_id, participant_state.participant_id)) {
            const auto participant = std::find_if(
                operation->participants.begin(), operation->participants.end(),
                [&](const auto& candidate) {
                  return candidate.slot_owner.participant_id == participant_state.participant_id;
                });
            if (participant != operation->participants.end() &&
                participant->stop_owns_reconciliation() && !released_pre_dispatch(*participant))
              participant_state.state = participant != operation->participants.end() &&
                                                participant->stop_cancelled_pre_dispatch
                                            ? OperationStateKind::kCancelledPreDispatch
                                            : OperationStateKind::kStopping;
            // Containment does not acknowledge or repair a retained fault.
          }
        }
        // A failed fence changed nothing in the slot table. Keep the Kernel's
        // slot owner and state equal to the table's, so settlement, reset and
        // a later Stop fence (including an operator Full Stop) name the owner
        // the table really holds.
        for (auto& participant : operation->participants) {
          if (fence_failed) break;
          participant.slot_owner.operation_revision = affected.operation_revision_after_acceptance;
          if (participant.stop_owns_reconciliation() && !released_pre_dispatch(participant))
            participant.slot_state = ResourceSlotState::kStopping;
        }
        if (std::none_of(operation->participants.begin(), operation->participants.end(),
                         [](const auto& row) { return row.stop_owns_reconciliation(); }))
          operation->state.state = aggregate_participant_state(operation->state.participants);
        operation->stop_effect_in_flight = true;
        stamp_operation_capture_locked(*operation, operation->state);
        affected_state.operations.push_back(operation->state);
      }
      affected_state.operations.clear();
      affected_state.operations.reserve(accepted->affected_operation_revisions.size());
      for (const auto& affected : accepted->affected_operation_revisions) {
        const auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation != nullptr) {
          affected_state.operations.push_back(operation->state);
        }
      }
      affected_state.resources = acceptance_resolution.resources;
      if (takeover_selection != nullptr) {
        const auto* candidate = find_pending_motion_operation_locked(takeover_selection->candidate_id);
        if (candidate != nullptr) {
          // This accepted newcomer's decision is forensic context only. It
          // adds no affected operation, resource, revision or Stop authority.
          affected_state.motion_takeover_candidate_snapshot = candidate->state;
        }
      }

      auto& transaction = *stop_transactions_[outcome.work_token.slot];
      transaction = StopTransaction{
          true,
          outcome.work_token,
          StopId(std::string(outcome.stop_id())),
          outcome.stop_generation,
          outcome.full_stop,
          false,
          EmergencyStopProjectionStateDTO::kPending,
          false,
          false,
          false,
          *accepted,
          projected_intent,
          affected_revisions,
          fenced.value().orphan_resources,
          std::move(newly_fenced_operations),
          exact_partial_capture && partial_dispatch_scope ? partial_dispatch_scope->participant_ids
                                : std::vector<OperationParticipantId>{},
          acceptance_resolution.resources,
          aggregate ? full_predecessor_tokens : std::vector<StopControlWorkToken>{},
          aggregate ? full_predecessor_certificates : std::vector<StopCertificateDTO>{},
          false,
          {},
          std::nullopt,
          std::nullopt,
          malformed_ingress != nullptr ? std::optional<std::string>(malformed_ingress->reason)
                                       : std::nullopt,
          std::nullopt,
          {}};
      transaction.proof_predecessor_scopes = proof_predecessor_scopes;
      transaction.predecessor_certificates.insert(transaction.predecessor_certificates.end(),
                                                  proof_predecessor_certificates.begin(),
                                                  proof_predecessor_certificates.end());
      if (primary_interrupt_selection != nullptr) {
        transaction.interrupt_selection = *primary_interrupt_selection;
        transaction.disjoint_interrupt_selections.assign(disjoint_interrupt_selections.begin(),
                                                         disjoint_interrupt_selections.end());
        for (const auto* selection : interrupt_selections)
          transaction.interrupt_winners.push_back(*selection);
      }
      if (global_selection) {
        transaction.global_interrupt_selection = *global_selection;
        transaction.global_interrupt_winners.push_back(*global_selection);
      }
      if (takeover_selection != nullptr) {
        transaction.motion_takeover_candidate_id = takeover_selection->candidate_id;
        transaction.force_spring_return = takeover_selection->force_spring_return;
      }
      if (accepted->resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull) {
        latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kScopePromotion);
      }
      // Incident containment latches even a nominal Stop of an idle cell:
      // physical rest after an unexpected loss is not yet proven.
      if (required_fault_latch.has_value()) {
        latch_stop_fault_locked(transaction, *required_fault_latch);
      }
      if (fence_failed) {
        latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kPostReserveFenceFailed);
      }
      if (outcome.correlation_retained) {
        stop_correlations_[outcome.correlation_slot] = *accepted;
      }
      projection_state = std::move(affected_state);
      work_token = outcome.work_token;
      handoff_work = true;
      // Q51: no shared Stop deadline is armed; the handed-off work starts at
      // once and every later wakeup is a robot's own stage deadline or a
      // stage completion.
      next_stop_id_ = proposed_number + 1U;
      cell_stop_generation_ = std::max(cell_stop_generation_, outcome.stop_generation);
    }

    if (outcome.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
        outcome.correlation_retained && accepted.has_value() && projection_state.has_value()) {
      projection_token = allocate_stop_projection_job_locked();
      if (!projection_token.has_value()) {
        // The containment effect is already linearized and handed to the
        // prequalified StopControl owner. Forensic projection capacity may
        // therefore degrade admission, but it must never reject, widen, pin,
        // or terminate an accepted Stop.
        auto& transaction = *stop_transactions_[outcome.work_token.slot];
        transaction.persistence_degraded = true;
        transaction.acceptance_projection_state = EmergencyStopProjectionStateDTO::kFailed;
        motion_admission_closed_ = true;
      } else {
        auto& projection = *stop_projection_jobs_[projection_token->slot];
        projection.intent = projected_intent;
        projection.affected_state = std::move(*projection_state);
        projection.acceptance = EmergencyStopAcceptanceRecordDTO{
            config_.request_owner_epoch,
            context.request_id,
            normalized_digest,
            malformed_ingress != nullptr ? std::optional<std::string>(malformed_ingress->reason)
                                         : std::nullopt,
            attribution,
            accepted->replay_status,
            accepted->stop_id,
            accepted->stop_generation,
            accepted_at_ns,
            accepted->resolution_disposition,
            accepted->captured_scope_digest,
            EmergencyStopEntryStateDTO::kActive,
            outcome.acceptance_path,
            EmergencyStopProjectionStateDTO::kPending};
        projection.accepted = *accepted;
      }
    }
    if (program_run_targets != nullptr && accepted.has_value() && !outcome.full_stop &&
        accepted->resolution_disposition != StopResolutionDispositionDTO::kPromotedToFull) {
      for (const auto& target : *program_run_targets) {
        const auto retained =
            std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                         [&](const ProgramRunStopFence& fence) {
                           return fence.program_run_id == target.program_run_id;
                         });
        ProgramRunStopFence* installed = nullptr;
        if (retained == program_run_stop_fences_.end()) {
          program_run_stop_fences_.push_back(ProgramRunStopFence{
              target.program_run_id, accepted->stop_id, accepted->stop_generation, false});
          installed = &program_run_stop_fences_.back();
        } else {
          retained->stop_id = accepted->stop_id;
          retained->stop_generation = accepted->stop_generation;
          retained->releasable = false;
          installed = &*retained;
        }
        auto& fence = *installed;
        if (fence.selection_revision < kMaximumKernelJsonInteger) ++fence.selection_revision;
        fence.legacy_selection_revision = fence.selection_revision;
        // The whole-run Stop closes the run through its stop identity. Only
        // effectful work that may be moving is contained here; Ready (never
        // started), Held and non-effectful nodes keep their motion state, so
        // releasing the Stop later does not grant motion.
        for (std::size_t index = 0U; index < fence.node_states.size(); ++index) {
          auto& state = fence.node_states[index];
          if (state == ProgramNodeHoldStateDTO::kRunning &&
              program_node_effectful_locked(fence.program_run_id, index))
            state = program_node_non_motion_locked(fence.program_run_id, index)
                        ? ProgramNodeHoldStateDTO::kHeld : ProgramNodeHoldStateDTO::kStopping;
        }
        fence_held_prepared_program_commands_locked(fence.program_run_id);
      }
    }
    if (ordinary != nullptr && malformed_ingress == nullptr && accepted.has_value()) {
      const auto retained =
          std::find_if(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                       [&](const StopReplayEntry& entry) { return entry.matches(*ordinary); });
      if (retained == stop_replay_ledger_.end()) {
        if (stop_replay_ledger_.size() >= config_.request_replay_capacity) {
          stop_replay_ledger_.erase(stop_replay_ledger_.begin());
        }
        stop_replay_ledger_.push_back(StopReplayEntry{*ordinary, *accepted});
      }
    }
  }

  if (handoff_work) {
    handoff.control = work_token;
  }
  if (projection_token.has_value()) {
    handoff.projection = projection_token;
  }
  if (!accepted.has_value()) {
    return reject<StopAcceptedDTO>("Stop acceptance result was not retained",
                                   "stop_acceptance_result_missing",
                                   ErrorCode::kInvariantViolation);
  }
  post_reserve_guard.armed = false;
  return Result<StopAcceptedDTO>::Ok(std::move(*accepted));
}

void ResourceAddressedExecutionKernel::ProjectStop(const StopControlWorkToken token) noexcept {
  StopProjectionJob job;
  try {
    {
      std::lock_guard state_lock(state_mutex_);
      if (!token.locally_valid() || token.slot >= stop_projection_jobs_.size()) {
        motion_admission_closed_ = true;
        return;
      }
      auto& retained = *stop_projection_jobs_[token.slot];
      if (!retained.active || retained.token != token) {
        return;
      }
      job = retained;
      retained.active = false;
    }

    if (job.terminal_status.has_value()) {
      if (!job.terminal_certificate.has_value()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
        return;
      }
      std::optional<ErrorDTO> operation_state_projection_error;
      for (const auto& operation_state : job.terminal_operation_states) {
        Result<std::uint64_t> recorded = Result<std::uint64_t>::Err(
            ErrorDTO{"operation_journal_stop_terminal_state_projection_failed",
                     "Stop terminal operation-state projection did not return a result",
                     {}});
        try {
          recorded = journal_.RecordOperationState(config_.request_owner_epoch, operation_state,
                                                   nullptr, "stop_terminal_committed");
        } catch (...) {
        }
        if (!recorded.ok() && !operation_state_projection_error.has_value()) {
          operation_state_projection_error = recorded.error();
        }
      }
      if (!job.terminal_fault_latch.has_value() &&
          (operation_state_projection_error.has_value() ||
           job.terminal_status->acceptance_projection.state ==
               StopDurableProjectionStateDTO::kFailed)) {
        std::lock_guard state_lock(state_mutex_);
        if (!stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value() ||
            cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger) {
          motion_admission_closed_ = true;
          return;
        }
        cell_stop_fault_latch_generation_ =
            StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
        job.terminal_fault_latch = persistence_fault_latch(
            config_.cell_id, config_.request_owner_epoch, *stop_retained_bundle_id_,
            *stop_graph_revision_, *job.terminal_certificate, cell_stop_fault_latch_generation_,
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                           std::nullopt});
        job.terminal_status->fault_latch = job.terminal_fault_latch;
        active_stop_fault_latch_ = job.terminal_fault_latch;
        motion_admission_closed_ = true;
      }
      auto projected =
          journal_.RecordStopTerminal(config_.request_owner_epoch, *job.terminal_status,
                                      *job.terminal_certificate, job.terminal_fault_latch);
      if (operation_state_projection_error.has_value() &&
          projected.state == StopForensicProjectionStateDTO::kRecorded) {
        projected = StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kFailed,
                                                    projected.journal_sequence,
                                                    operation_state_projection_error};
      }
      std::lock_guard state_lock(state_mutex_);
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.terminal_certificate->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.terminal_certificate->stop_generation;
          });
      if (record == stop_terminal_history_.end()) {
        motion_admission_closed_ = true;
        return;
      }
      if ((*record)->terminal_projection_token == token) {
        (*record)->terminal_projection_token.reset();
      }
      if (job.terminal_fault_latch.has_value() && !(*record)->fault_latch.has_value()) {
        (*record)->fault_latch = job.terminal_fault_latch;
        (*record)->status.fault_latch = job.terminal_fault_latch;
      }
      if (projected.state == StopForensicProjectionStateDTO::kRecorded) {
        (*record)->status.terminal_projection = StopDurableProjectionStatusDTO{
            StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
        if ((*record)->fault_latch.has_value()) {
          (*record)->fault_latch->terminal_projection = StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
          (*record)->status.fault_latch = (*record)->fault_latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == (*record)->fault_latch->latch_id) {
            active_stop_fault_latch_ = (*record)->fault_latch;
          }
        }
      } else {
        motion_admission_closed_ = true;
        const auto failure =
            projected.error.value_or(ErrorDTO{"operation_journal_stop_terminal_projection_failed",
                                              "Stop terminal forensic projection failed",
                                              {}});
        (*record)->status.terminal_projection = StopDurableProjectionStatusDTO{
            StopDurableProjectionStateDTO::kFailed, failure, kStopJournalProjectionAttempt};
        if (!(*record)->fault_latch.has_value() && stop_retained_bundle_id_.has_value() &&
            stop_graph_revision_.has_value() &&
            cell_stop_fault_latch_generation_.value() < kMaximumKernelJsonInteger) {
          cell_stop_fault_latch_generation_ =
              StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
          (*record)->fault_latch = persistence_fault_latch(
              config_.cell_id, config_.request_owner_epoch, *stop_retained_bundle_id_,
              *stop_graph_revision_, (*record)->certificate, cell_stop_fault_latch_generation_,
              StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure,
                                             kStopJournalProjectionAttempt});
          (*record)->status.fault_latch = (*record)->fault_latch;
          active_stop_fault_latch_ = (*record)->fault_latch;
        }
        if ((*record)->fault_latch.has_value()) {
          (*record)->fault_latch->terminal_projection = StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kFailed, failure, kStopJournalProjectionAttempt};
          (*record)->status.fault_latch = (*record)->fault_latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == (*record)->fault_latch->latch_id) {
            active_stop_fault_latch_ = (*record)->fault_latch;
          }
        }
      }
      return;
    }

    if (job.surveillance_checkpoint.has_value()) {
      if (!job.surveillance_fault_latch.has_value()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
        return;
      }
      const auto projected_at_ns = clock_.now();
      const auto projected = journal_.RecordStopSurveillanceCheckpoint(
          config_.request_owner_epoch, *job.surveillance_checkpoint, *job.surveillance_fault_latch,
          projected_at_ns);
      std::lock_guard state_lock(state_mutex_);
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.surveillance_checkpoint->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.surveillance_checkpoint->stop_generation;
          });
      if (record == stop_terminal_history_.end()) {
        motion_admission_closed_ = true;
        return;
      }
      if ((*record)->surveillance_projection_token == token) {
        (*record)->surveillance_projection_token.reset();
      }
      if ((*record)->surveillance_state == nullptr) {
        motion_admission_closed_ = true;
        return;
      }
      auto checkpoint_status = StopDurableProjectionStatusDTO{
          StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
      if (projected.state != StopForensicProjectionStateDTO::kRecorded) {
        const auto failure = projected.error.value_or(
            ErrorDTO{"operation_journal_stop_surveillance_projection_failed",
                     "Stop surveillance checkpoint projection failed",
                     {}});
        checkpoint_status = StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                                           failure, kStopJournalProjectionAttempt};
        motion_admission_closed_ = true;
        if ((*record)->fault_latch.has_value()) {
          auto& latch = *(*record)->fault_latch;
          const bool assessment_changed =
              !latch.persistence_degraded ||
              latch.recovery_status != StopFaultRecoveryStatusDTO::kWaitingForDurableRecord ||
              latch.reason_code != "stop_fault_recovery_waiting_for_durable_record";
          latch.persistence_degraded = true;
          latch.recovery_status = StopFaultRecoveryStatusDTO::kWaitingForDurableRecord;
          latch.reason_code = "stop_fault_recovery_waiting_for_durable_record";
          if (assessment_changed &&
              latch.recovery_assessment_revision < std::numeric_limits<std::uint64_t>::max()) {
            ++latch.recovery_assessment_revision;
          }
          (*record)->status.fault_latch = latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == latch.latch_id &&
              active_stop_fault_latch_->latch_generation == latch.latch_generation) {
            active_stop_fault_latch_ = latch;
          }
        }
      }
      auto& current = (*record)->surveillance_state->projection;
      if (current.surveillance_generation == job.surveillance_checkpoint->surveillance_generation &&
          current.checkpoint_generation == job.surveillance_checkpoint->checkpoint_generation) {
        current.checkpoint_projection = checkpoint_status;
      }
      (*record)->status.surveillance = current;
      (*record)->status.surveillance_projection = current.checkpoint_projection;
      if ((*record)->fault_latch.has_value()) {
        (*record)->status.fault_latch = (*record)->fault_latch;
      }
      return;
    }

    if (!job.acceptance.has_value() || !job.intent.has_value() || !job.accepted.has_value() ||
        !job.affected_state.has_value()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return;
    }

    const auto projected =
        journal_.RecordStopAccepted(config_.request_owner_epoch, *job.acceptance, *job.intent,
                                    *job.accepted, *job.affected_state);
    const auto state = projected.projection_state == EmergencyStopProjectionStateDTO::kRecorded
                           ? EmergencyStopProjectionStateDTO::kRecorded
                           : EmergencyStopProjectionStateDTO::kFailed;
    const auto durable_status =
        state == EmergencyStopProjectionStateDTO::kRecorded
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{
                  StopDurableProjectionStateDTO::kFailed,
                  projected.error.value_or(ErrorDTO{"operation_journal_stop_projection_failed",
                                                    "Stop acceptance forensic projection failed",
                                                    {}}),
                  kStopJournalProjectionAttempt};
    // The Stop's acceptance projection is its creating request's recovery
    // projection. A joined request's durable receipt can never upgrade that
    // status; its failure still degrades the Stop fail-closed.
    const bool publishes_acceptance_status =
        job.acceptance->acceptance_path != EmergencyStopAcceptancePathDTO::kJoinedResident ||
        state == EmergencyStopProjectionStateDTO::kFailed;
    const bool marked =
        !publishes_acceptance_status ||
        emergency_stop_reserve_.mark_projection(job.acceptance->stop_id.value(), state);
    std::optional<StopControlWorkToken> terminal_projection;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
                   *candidate->stop_id == job.acceptance->stop_id;
          });
      if (transaction != stop_transactions_.end() && publishes_acceptance_status) {
        (*transaction)->acceptance_projection_state = state;
        if ((*transaction)->terminal_projection_token.has_value()) {
          auto& terminal_job =
              *stop_projection_jobs_[(*transaction)->terminal_projection_token->slot];
          if (terminal_job.terminal_status.has_value()) {
            terminal_job.terminal_status->acceptance_projection = durable_status;
          }
        }
        if ((*transaction)->terminal && (*transaction)->terminal_projection_token.has_value()) {
          terminal_projection = (*transaction)->terminal_projection_token;
        }
      }
      const auto terminal_record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_id == job.acceptance->stop_id &&
                   record->certificate.stop_generation == job.acceptance->stop_generation;
          });
      if (terminal_record != stop_terminal_history_.end() && publishes_acceptance_status) {
        (*terminal_record)->status.acceptance_projection = durable_status;
        if ((*terminal_record)->terminal_projection_token.has_value()) {
          const auto retained_token = *(*terminal_record)->terminal_projection_token;
          if (retained_token.slot < stop_projection_jobs_.size()) {
            auto& terminal_job = *stop_projection_jobs_[retained_token.slot];
            if (terminal_job.active && terminal_job.token == retained_token &&
                terminal_job.terminal_status.has_value()) {
              terminal_job.terminal_status->acceptance_projection = durable_status;
              terminal_projection = retained_token;
            }
          }
        }
      }
      if (state == EmergencyStopProjectionStateDTO::kFailed ||
          (!marked && transaction == stop_transactions_.end() &&
           terminal_record == stop_terminal_history_.end())) {
        motion_admission_closed_ = true;
        if (transaction != stop_transactions_.end()) {
          (*transaction)->persistence_degraded = true;
        }
      }
    }
    if (terminal_projection.has_value()) {
      stop_projection_worker_->Publish(*terminal_projection);
    }
  } catch (...) {
    std::optional<StopControlWorkToken> terminal_projection;
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
    if (job.surveillance_checkpoint.has_value()) {
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.surveillance_checkpoint->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.surveillance_checkpoint->stop_generation;
          });
      if (record != stop_terminal_history_.end()) {
        if ((*record)->surveillance_projection_token == token) {
          (*record)->surveillance_projection_token.reset();
        }
        const ErrorDTO failure{"operation_journal_stop_surveillance_projection_failed",
                               "Stop surveillance checkpoint projection failed",
                               {}};
        (*record)->status.surveillance_projection =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        if ((*record)->surveillance_state != nullptr) {
          (*record)->surveillance_state->projection.checkpoint_projection =
              (*record)->status.surveillance_projection;
          (*record)->status.surveillance = (*record)->surveillance_state->projection;
        }
        if ((*record)->fault_latch.has_value()) {
          auto& latch = *(*record)->fault_latch;
          const bool assessment_changed =
              !latch.persistence_degraded ||
              latch.recovery_status != StopFaultRecoveryStatusDTO::kWaitingForDurableRecord ||
              latch.reason_code != "stop_fault_recovery_waiting_for_durable_record";
          latch.persistence_degraded = true;
          latch.recovery_status = StopFaultRecoveryStatusDTO::kWaitingForDurableRecord;
          latch.reason_code = "stop_fault_recovery_waiting_for_durable_record";
          if (assessment_changed &&
              latch.recovery_assessment_revision < std::numeric_limits<std::uint64_t>::max()) {
            ++latch.recovery_assessment_revision;
          }
          (*record)->status.fault_latch = latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == latch.latch_id &&
              active_stop_fault_latch_->latch_generation == latch.latch_generation) {
            active_stop_fault_latch_ = latch;
          }
        }
      }
      return;
    }
    if (job.acceptance.has_value()) {
      static_cast<void>(emergency_stop_reserve_.mark_projection(
          job.acceptance->stop_id.value(), EmergencyStopProjectionStateDTO::kFailed));
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
                   *candidate->stop_id == job.acceptance->stop_id;
          });
      if (transaction != stop_transactions_.end()) {
        (*transaction)->acceptance_projection_state = EmergencyStopProjectionStateDTO::kFailed;
        (*transaction)->persistence_degraded = true;
        if ((*transaction)->terminal && (*transaction)->terminal_projection_token.has_value()) {
          terminal_projection = (*transaction)->terminal_projection_token;
        }
      }
      const auto terminal_record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_id == job.acceptance->stop_id &&
                   record->certificate.stop_generation == job.acceptance->stop_generation;
          });
      if (terminal_record != stop_terminal_history_.end()) {
        const ErrorDTO failure{"operation_journal_stop_projection_failed",
                               "Stop acceptance forensic projection failed",
                               {}};
        (*terminal_record)->status.acceptance_projection =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        if ((*terminal_record)->terminal_projection_token.has_value()) {
          const auto retained_token = *(*terminal_record)->terminal_projection_token;
          if (retained_token.slot < stop_projection_jobs_.size()) {
            auto& terminal_job = *stop_projection_jobs_[retained_token.slot];
            if (terminal_job.active && terminal_job.token == retained_token &&
                terminal_job.terminal_status.has_value()) {
              terminal_job.terminal_status->acceptance_projection =
                  (*terminal_record)->status.acceptance_projection;
              terminal_projection = retained_token;
            }
          }
        }
      }
    }
    if (terminal_projection.has_value()) {
      stop_projection_worker_->Publish(*terminal_projection);
    }
  }
}

void ResourceAddressedExecutionKernel::StageAttemptCompleted(
    const StopStageAttemptHandle handle) noexcept {
  std::optional<StopControlWorkToken> owner_token;
  try {
    {
      std::lock_guard state_lock(state_mutex_);
      for (const auto& transaction : stop_transactions_) {
        if (transaction == nullptr || !transaction->active || transaction->terminal) {
          continue;
        }
        const bool owns = std::any_of(
            transaction->participants.begin(), transaction->participants.end(),
            [&](const auto& participant) {
              return std::any_of(participant.attempts.begin(), participant.attempts.end(),
                                 [&](const auto& attempt) {
                                   return attempt.handle.has_value() && *attempt.handle == handle &&
                                          !attempt.result_consumed;
                                 });
            });
        if (owns) {
          owner_token = transaction->token;
          // Deadline mutation for one physical Stop slot is serialized by
          // state_mutex_. This cannot race with ProgressStop rearming the
          // participant phase watchdog after it publishes an attempt.
          stop_control_executor_->ArmDeadline(*owner_token, clock_.now());
          break;
        }
      }
    }
    if (owner_token.has_value()) {
      // Publish coalesces an already-delivered generation. Rearming its fixed
      // deadline at the current instant under the Kernel state lock is the
      // allocation-free wakeup path for an asynchronously completed attempt.
      stop_control_executor_->Publish(*owner_token);
      return;
    }
    // A terminal transaction may already have handed off its immutable
    // certificate. Consume and release a late isolated result so one hung
    // predecessor can never exhaust future Stop-stage capacity. Later P5d
    // projection owns the bounded late-audit sink.
    const auto late = stop_stage_attempt_executor_->TryTake(handle);
    if (late.status != StopStageAttemptTakeStatus::kPending) {
      static_cast<void>(stop_stage_attempt_executor_->Release(handle));
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

std::optional<StopControlWorkToken>
ResourceAddressedExecutionKernel::allocate_stop_projection_job_locked() noexcept {
  for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
    auto& job = *stop_projection_jobs_[index];
    if (job.active) {
      continue;
    }
    auto& generation = stop_projection_generations_[index];
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
      continue;
    }
    ++generation;
    const StopControlWorkToken token{static_cast<StopControlSlotIndex>(index), generation};
    job = StopProjectionJob{};
    job.token = token;
    job.active = true;
    return token;
  }
  return std::nullopt;
}

void ResourceAddressedExecutionKernel::latch_stop_fault_locked(StopTransaction& transaction,
                                                               const StopFaultTriggerDTO trigger) {
  motion_admission_closed_ = true;
  if (transaction.fault_latch.has_value()) {
    auto& latch = *transaction.fault_latch;
    if (fault_trigger_priority(trigger) > fault_trigger_priority(latch.trigger)) {
      latch.trigger = trigger;
      latch.reason_code = fault_trigger_reason(trigger);
      ++latch.recovery_assessment_revision;
    }
    observe_fault_trigger(latch, trigger);
    latch.persistence_degraded = latch.persistence_degraded || transaction.persistence_degraded;
    if (active_stop_fault_latch_.has_value() &&
        active_stop_fault_latch_->latch_id == latch.latch_id)
      active_stop_fault_latch_ = latch;
    return;
  }
  if (!transaction.accepted.has_value() || !transaction.stop_id.has_value() ||
      !stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value() ||
      cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger)
    return;
  const auto generation = StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
  std::vector<OperationId> operations;
  operations.reserve(transaction.accepted->affected_operation_revisions.size());
  for (const auto& affected : transaction.accepted->affected_operation_revisions)
    operations.push_back(affected.operation_id);
  StopFaultLatchDTO latch{
      config_.cell_id,
      *transaction.stop_id,
      transaction.stop_generation,
      std::nullopt,
      StopFaultLatchId("stop-fault." + config_.request_owner_epoch.value() + "." +
                       std::to_string(generation.value())),
      generation,
      trigger,
      transaction.persistence_degraded,
      std::move(operations),
      transaction.resources,
      *stop_retained_bundle_id_,
      *stop_graph_revision_,
      {StopDurableProjectionStateDTO::kPending, std::nullopt, std::nullopt},
      {StopDurableProjectionStateDTO::kNotApplicable, std::nullopt, std::nullopt},
      StopFaultLatchStateDTO::kLatched,
      1U,
      StopFaultRecoveryStatusDTO::kWaitingForObservation,
      fault_trigger_reason(trigger),
      {},
      {trigger}};
  if (stop_fault_latch_field_error(latch).has_value()) return;
  cell_stop_fault_latch_generation_ = generation;
  transaction.fault_latch = latch;
  active_stop_fault_latch_ = std::move(latch);
}

void ResourceAddressedExecutionKernel::refresh_stop_fault_latch_locked() {
  // Recovery of one exact fault must not clear another. Reuse the bounded
  // resident transaction/history tables; no additional ledger or timer.
  const auto retain_newest = [&](const auto& latch) {
    if (is_active_stop_fault_latch(latch) &&
        (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
         active_stop_fault_latch_->latch_generation <= latch->latch_generation))
      active_stop_fault_latch_ = latch;
  };
  for (const auto& transaction : stop_transactions_)
    if (transaction != nullptr && transaction->active && !transaction->terminal)
      retain_newest(transaction->fault_latch);
  for (const auto& record : stop_terminal_history_)
    if (record != nullptr) retain_newest(record->fault_latch);
}

StopFaultAdmissionScope ResourceAddressedExecutionKernel::stop_fault_admission_scope_locked()
    const noexcept {
  if (!is_active_stop_fault_latch(active_stop_fault_latch_)) return StopFaultAdmissionScope::kNone;
  // D6: the latch names the product it latched. Once Close replaced that
  // product, the latch stays (a fact about its robots) but blocks only motion.
  const auto& latch = *active_stop_fault_latch_;
  if (stop_retained_bundle_id_.has_value() && stop_graph_revision_.has_value() &&
      (latch.retained_bundle_id != *stop_retained_bundle_id_ ||
       latch.graph_revision != *stop_graph_revision_))
    return StopFaultAdmissionScope::kCarried;
  return close_retired_product_ ? StopFaultAdmissionScope::kClosing
                                : StopFaultAdmissionScope::kCell;
}

bool ResourceAddressedExecutionKernel::commit_stop_terminal_locked(
    StopTransaction& transaction, const std::int64_t completed_at_ns) noexcept {
  try {
    if (!transaction.accepted.has_value() || !transaction.stop_id.has_value() ||
        transaction.certificate.has_value()) {
      return false;
    }
    std::vector<StopCertificateDTO> predecessors = transaction.predecessor_certificates;
    predecessors.reserve(predecessors.size() + transaction.predecessor_tokens.size());
    for (const auto& predecessor_token : transaction.predecessor_tokens) {
      if (predecessor_token.slot >= stop_transactions_.size()) {
        return false;
      }
      const auto& predecessor = *stop_transactions_[predecessor_token.slot];
      if (!predecessor.active || predecessor.token != predecessor_token ||
          !predecessor.certificate.has_value()) {
        return false;
      }
      predecessors.push_back(*predecessor.certificate);
    }
    std::sort(predecessors.begin(), predecessors.end(),
              [](const auto& left, const auto& right) { return left.stop_id < right.stop_id; });
    if (std::adjacent_find(predecessors.begin(), predecessors.end(),
                           [](const auto& left, const auto& right) {
                             return left.stop_id == right.stop_id;
                           }) != predecessors.end() ||
        (transaction.proof_predecessor_scopes.empty() &&
         predecessors.size() != transaction.accepted->joined_predecessor_stops.size()) ||
        !std::all_of(predecessors.begin(), predecessors.end(), [&](const auto& predecessor) {
          return std::any_of(transaction.accepted->joined_predecessor_stops.begin(),
                             transaction.accepted->joined_predecessor_stops.end(),
                             [&](const auto& ref) {
                               return ref.predecessor_stop_id == predecessor.stop_id &&
                                      ref.predecessor_stop_generation == predecessor.stop_generation;
                             });
        })) {
      return false;
    }

    std::vector<StopScopeEntryDTO> entries;
    std::vector<StopAttemptedStageResultDTO> attempts;
    entries.reserve(transaction.participants.size() + transaction.newly_captured_resources.size());
    for (const auto& participant : transaction.participants) {
      entries.push_back(participant_scope_entry(
          participant, *transaction.stop_id, transaction.stop_generation,
          transaction.accepted->accepted_at_ns));
      attempts.insert(attempts.end(), participant.retained_results.begin(),
                      participant.retained_results.end());
    }
    for (const auto& resource : transaction.newly_captured_resources) {
      entries.push_back(StopScopeEntryDTO{
          StopScopeEntryKeyDTO{*transaction.stop_id, transaction.stop_generation, resource},
          resource,
          StopScopeEntryDispositionDTO::kNotStoppable,
          StopScopeDispatchStateDTO::kEffectUncertain,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          transaction.accepted->accepted_at_ns});
    }
    // Fresh operation IDs and orphan resources are already canonical. The key
    // variant orders participant subjects before resource-only subjects.
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
      const auto& left_key = left.key;
      const auto& right_key = right.key;
      if (left_key.origin_stop_id != right_key.origin_stop_id) {
        return left_key.origin_stop_id < right_key.origin_stop_id;
      }
      if (left_key.origin_stop_generation != right_key.origin_stop_generation) {
        return left_key.origin_stop_generation < right_key.origin_stop_generation;
      }
      if (left_key.subject.index() != right_key.subject.index()) {
        return left_key.subject.index() < right_key.subject.index();
      }
      if (const auto* left_participant =
              std::get_if<StopParticipantScopeKeyDTO>(&left_key.subject)) {
        const auto& right_participant = std::get<StopParticipantScopeKeyDTO>(right_key.subject);
        return std::tie(left_participant->operation_id, left_participant->participant_id) <
               std::tie(right_participant.operation_id, right_participant.participant_id);
      }
      return std::get<ResourceId>(left_key.subject) < std::get<ResourceId>(right_key.subject);
    });
    std::sort(attempts.begin(), attempts.end(), [](const auto& left, const auto& right) {
      const auto key = [](const auto& attempt) {
        const auto& participant = std::get<StopParticipantScopeKeyDTO>(attempt.scope_key.subject);
        return std::tie(attempt.scope_key.origin_stop_id, attempt.scope_key.origin_stop_generation,
                        participant.operation_id, participant.participant_id,
                        attempt.result.request.stage_index, attempt.result_sequence);
      };
      return key(left) < key(right);
    });

    std::optional<ErrorDTO> terminal_error;
    for (const auto& participant : transaction.participants) {
      if (participant.participant_settlement_error.has_value()) {
        terminal_error = participant.participant_settlement_error;
        break;
      }
      if (!participant.contained && participant.terminal_error.has_value()) {
        terminal_error = participant.terminal_error;
        break;
      }
    }
    // Escalated, partial or uncontained Stops end their operations
    // Stopped/InterventionRequired without a cell-wide Stop-fault latch
    // (items 25, Q29; `main` had none). Only scope promotion, persistence
    // degradation, newly captured idle resources and a containment that
    // requested its latch at acceptance keep one.
    const bool known_latch_required =
        transaction.fault_latch.has_value() || transaction.persistence_degraded ||
        transaction.accepted->resolution_disposition ==
            StopResolutionDispositionDTO::kPromotedToFull ||
        !transaction.newly_captured_resources.empty();
    std::optional<StopFaultLatchId> latch_id =
        transaction.fault_latch.has_value() ? std::optional(transaction.fault_latch->latch_id)
                                            : std::nullopt;
    std::optional<StopFaultLatchGeneration> latch_generation =
        transaction.fault_latch.has_value()
            ? std::optional(transaction.fault_latch->latch_generation)
            : std::nullopt;
    const auto assign_latch_identity = [&]() {
      if (latch_id.has_value()) {
        return true;
      }
      if (cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger) {
        return false;
      }
      latch_generation = StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
      latch_id = StopFaultLatchId("stop-fault." + config_.request_owner_epoch.value() + "." +
                                  std::to_string(latch_generation->value()));
      return true;
    };
    if (known_latch_required && !assign_latch_identity()) {
      return false;
    }
    StopTerminalReductionInputDTO reduction{
        StopCertificateId("stop-certificate." + transaction.stop_id->value()),
        StopCertificateRevision(1U),
        *transaction.stop_id,
        transaction.stop_generation,
        requested_selector_snapshot(transaction.projected_intent,
                                    transaction.accepted->requested_selector_digest,
                                    transaction.malformed_ingress_reason),
        transaction.accepted->resolution_disposition,
        transaction.accepted->resolution_reason,
        transaction.accepted->stale_diagnostics,
        {StopScopeCaptureHistoryDTO{
            1U, transaction.accepted->resolution_disposition,
            transaction.accepted->resolution_reason, transaction.accepted->captured_scope_digest,
            transaction.accepted->joined_predecessor_stops,
            transaction.newly_fenced_operation_revisions, transaction.newly_captured_resources}},
        std::move(predecessors),
        std::move(entries),
        std::move(attempts),
        transaction.interrupt_selection
            ? std::optional(StopInterruptCorrelationDTO{
                  transaction.interrupt_selection->source_id,
                  transaction.interrupt_selection->registration.policy_id,
                  std::get<IoOperationRuleScopeDTO>(
                      transaction.interrupt_selection->registration.scope)
                      .motion_interrupt_scope_id,
                  std::nullopt})
            : std::nullopt,
        latch_id,
        latch_generation,
        completed_at_ns,
        terminal_error};
    for (const auto& participant : transaction.participants) {
      if (participant.lane_hard_stop_requested) {
        reduction.lane_hard_stop_requested.push_back(StopScopeEntryKeyDTO{
            *transaction.stop_id, transaction.stop_generation,
            StopParticipantScopeKeyDTO{participant.operation_id, participant.plan.participant_id}});
      }
    }
    std::sort(reduction.lane_hard_stop_requested.begin(), reduction.lane_hard_stop_requested.end(),
              [](const auto& left, const auto& right) {
                const auto& l = std::get<StopParticipantScopeKeyDTO>(left.subject);
                const auto& r = std::get<StopParticipantScopeKeyDTO>(right.subject);
                return std::tie(l.operation_id, l.participant_id) <
                       std::tie(r.operation_id, r.participant_id);
              });
    auto reduced = transaction.proof_predecessor_scopes.empty()
                       ? ReduceStopTerminal(reduction)
                       : internal::ReduceStopTerminalWithProofScopes(
                             reduction, transaction.proof_predecessor_scopes);
    // The reducer owns the authoritative nominal/escalated/partial outcome.
    // Pre-classification above avoids a second reduction in the common case,
    // while this exact retry closes outcomes (for example evidence-derived
    // partial containment) that cannot be guessed before reduction.
    if (!reduced.ok() && reduced.error().code == "stop_terminal_fault_latch_required" &&
        !latch_id.has_value() && assign_latch_identity()) {
      reduction.fault_latch_id = latch_id;
      reduction.fault_latch_generation = latch_generation;
      reduced = transaction.proof_predecessor_scopes.empty()
                    ? ReduceStopTerminal(reduction)
                    : internal::ReduceStopTerminalWithProofScopes(
                          reduction, transaction.proof_predecessor_scopes);
    }
    if (!reduced.ok()) {
      return false;
    }
    auto certificate = std::move(reduced.value());
    bool projection_capacity_available = false;
    for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
      if (!stop_projection_jobs_[index]->active &&
          stop_projection_generations_[index] != std::numeric_limits<std::uint64_t>::max()) {
        projection_capacity_available = true;
        break;
      }
    }
    std::optional<ErrorDTO> projection_capacity_error;
    if (!projection_capacity_available) {
      projection_capacity_error =
          ErrorDTO{"operation_journal_stop_terminal_projection_capacity_exhausted",
                   "Stop terminal forensic projection capacity is exhausted",
                   {}};
      transaction.persistence_degraded = true;
      motion_admission_closed_ = true;
      if (!assign_latch_identity()) {
        return false;
      }
    }
    const auto terminal_projection_status =
        projection_capacity_error.has_value()
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                             projection_capacity_error, 1U}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                             std::nullopt};
    std::optional<StopFaultLatchDTO> latch;
    if (transaction.fault_latch.has_value() ||
        terminal_fault_required(certificate, transaction.persistence_degraded)) {
      if (!latch_id.has_value() || !latch_generation.has_value() ||
          !stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value()) {
        return false;
      }
      const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                            certificate.stop_certificate_revision,
                                            certificate.stop_id, certificate.stop_generation};
      auto [operations, resources] = fault_latch_capture(certificate);
      const auto certificate_trigger =
          primary_fault_trigger(certificate, transaction.persistence_degraded);
      auto trigger = certificate_trigger;
      if (transaction.fault_latch.has_value() &&
          fault_trigger_priority(transaction.fault_latch->trigger) >
              fault_trigger_priority(trigger))
        trigger = transaction.fault_latch->trigger;
      latch = StopFaultLatchDTO{
          config_.cell_id,
          certificate.stop_id,
          certificate.stop_generation,
          reference,
          *latch_id,
          *latch_generation,
          trigger,
          transaction.persistence_degraded,
          std::move(operations),
          std::move(resources),
          *stop_retained_bundle_id_,
          *stop_graph_revision_,
          terminal_projection_status,
          StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                         std::nullopt, std::nullopt},
          StopFaultLatchStateDTO::kLatched,
          transaction.fault_latch.has_value()
              ? transaction.fault_latch->recovery_assessment_revision + 1U
              : 1U,
          StopFaultRecoveryStatusDTO::kWaitingForObservation,
          fault_trigger_reason(trigger),
          {},
          transaction.fault_latch.has_value() ? transaction.fault_latch->observed_triggers
                                              : std::vector<StopFaultTriggerDTO>{}};
      observe_fault_trigger(*latch, trigger);
      observe_fault_trigger(*latch, certificate_trigger);
      if (stop_fault_latch_field_error(*latch).has_value()) {
        return false;
      }
    }
    std::unique_ptr<StopSurveillanceReducerState> surveillance_state;
    if (latch.has_value() && stop_requires_surveillance(certificate, *latch)) {
      if (!config_.stop_surveillance.has_value()) {
        motion_admission_closed_ = true;
      } else {
        auto source_plans = stop_surveillance_source_plans(certificate);
        if (!source_plans.ok()) {
          motion_admission_closed_ = true;
        } else {
          auto seeded = MakeStopSurveillanceReducerState(
              config_.request_owner_epoch, 1U, certificate, *latch,
              config_.stop_surveillance->policy, std::move(source_plans.value()));
          if (!seeded.ok()) {
            return false;
          }
          surveillance_state =
              std::make_unique<StopSurveillanceReducerState>(std::move(seeded.value()));
        }
      }
    }
    const auto terminal_state = [&] {
      switch (certificate.result) {
        case StopCertificateResultDTO::kSucceededNominal:
        case StopCertificateResultDTO::kSucceededEscalated:
          return StopStatusStateDTO::kSucceeded;
        case StopCertificateResultDTO::kPartial:
          return StopStatusStateDTO::kPartial;
        case StopCertificateResultDTO::kFailedUncontained:
          return StopStatusStateDTO::kFailed;
      }
      return StopStatusStateDTO::kFailed;
    }();
    const auto acceptance_projection = [&] {
      switch (transaction.acceptance_projection_state) {
        case EmergencyStopProjectionStateDTO::kPending:
          return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                std::nullopt, std::nullopt};
        case EmergencyStopProjectionStateDTO::kRecorded:
          return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                                std::nullopt, std::nullopt};
        case EmergencyStopProjectionStateDTO::kFailed:
          return StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kFailed,
              ErrorDTO{"operation_journal_stop_projection_failed",
                       "Stop acceptance forensic projection failed",
                       {}},
              1U};
      }
      return StopDurableProjectionStatusDTO{
          StopDurableProjectionStateDTO::kFailed,
          ErrorDTO{"stop_projection_state_invalid", "Stop projection state is invalid", {}}, 1U};
    }();
    const auto surveillance_projection =
        surveillance_state != nullptr
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                             std::nullopt, std::nullopt};
    StopStatusDTO status{*transaction.accepted,
                         terminal_state,
                         certificate.scope_entries,
                         {},
                         certificate.attempted_stage_results,
                         certificate.evidence_items,
                         certificate.unsettled,
                         certificate.failed,
                         acceptance_projection,
                         terminal_projection_status,
                         surveillance_projection,
                         StopCertificateRefDTO{certificate.stop_certificate_id,
                                               certificate.stop_certificate_revision,
                                               certificate.stop_id, certificate.stop_generation},
                         latch,
                         surveillance_state != nullptr
                             ? std::optional(surveillance_state->projection)
                             : std::nullopt};
    if (stop_terminal_projection_field_error(status, certificate, latch).has_value()) {
      return false;
    }
    if (stop_terminal_history_.size() == kMaximumResidentStopTerminalRecords) {
      const auto referenced_by_live_aggregate = [&](const StopTerminalRecord& record) {
        return std::any_of(
            stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
              return candidate != nullptr && candidate->active && !candidate->terminal &&
                     candidate->accepted.has_value() &&
                     std::any_of(candidate->accepted->joined_predecessor_stops.begin(),
                                 candidate->accepted->joined_predecessor_stops.end(),
                                 [&](const auto& predecessor) {
                                   return predecessor.predecessor_stop_id ==
                                              record.certificate.stop_id &&
                                          predecessor.predecessor_stop_generation ==
                                              record.certificate.stop_generation;
                                 });
            });
      };
      const auto compactable = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr &&
                   record->status.terminal_projection.state ==
                       StopDurableProjectionStateDTO::kRecorded &&
                   (!record->fault_latch.has_value() ||
                    record->fault_latch->state == StopFaultLatchStateDTO::kCleared) &&
                   (!record->status.surveillance.has_value() ||
                    record->status.surveillance->state ==
                        StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved) &&
                   !referenced_by_live_aggregate(*record);
          });
      if (compactable == stop_terminal_history_.end()) {
        return false;
      }
      stop_terminal_history_.erase(compactable);
    }
    std::optional<StopControlWorkToken> projection_token;
    if (projection_capacity_available) {
      projection_token = allocate_stop_projection_job_locked();
      if (!projection_token.has_value()) {
        return false;
      }
      auto& projection = *stop_projection_jobs_[projection_token->slot];
      projection.terminal_status = status;
      projection.terminal_certificate = certificate;
      projection.terminal_fault_latch = latch;
      const auto snapshot_ids = stop_operation_snapshot_ids(transaction);
      projection.terminal_operation_states.reserve(snapshot_ids.size());
      for (const auto& operation_id : snapshot_ids) {
        auto* operation = find_active_operation_locked(operation_id);
        if (operation == nullptr) return false;
        // Only the actual selected moving rows belong to this certificate.
        // The full honest snapshot may still contain ordinary Executing work.
        for (const auto& work : transaction.participants) {
          if (work.operation_id != operation_id) continue;
          const auto row = std::find_if(operation->state.participants.begin(),
              operation->state.participants.end(), [&](const auto& candidate) {
                return candidate.participant_id == work.plan.participant_id;
              });
          if (operation->state.active_stop_id != certificate.stop_id ||
              operation->state.active_stop_generation != certificate.stop_generation ||
              !work.terminal || !work.terminal_projection_complete ||
              (work.contained && !work.settlement_not_required() && !work.participant_settlement_complete) ||
              row == operation->state.participants.end() || !terminal(row->state)) return false;
        }
        stamp_operation_capture_locked(*operation, operation->state);
        projection.terminal_operation_states.push_back(operation->state);
      }
    }
    std::int64_t first_surveillance_sample_ns = 0;
    std::int64_t first_surveillance_checkpoint_ns = 0;
    if (surveillance_state != nullptr) {
      const auto& policy = config_.stop_surveillance->policy;
      if (completed_at_ns > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns ||
          completed_at_ns >
              std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
        return false;
      }
      first_surveillance_sample_ns = completed_at_ns + policy.sample_cadence_ns;
      first_surveillance_checkpoint_ns = completed_at_ns + policy.checkpoint_interval_ns;
    }
    auto record = std::make_unique<StopTerminalRecord>(StopTerminalRecord{
        status, certificate, latch, projection_token, std::move(surveillance_state), std::nullopt,
        0U, false, std::nullopt, first_surveillance_sample_ns, first_surveillance_checkpoint_ns,
        transaction.accepted});
    transaction.certificate = certificate;
    finalize_io_rule_stop_locked(transaction, certificate);
    if (projection_token) {
      auto& states = stop_projection_jobs_[projection_token->slot]->terminal_operation_states;
      for (auto& state : states) {
        const auto* operation = find_active_operation_locked(state.operation_id);
        if (operation) state = operation->state;
      }
    }
    transaction.fault_latch = latch;
    transaction.terminal_projection_token = projection_token;
    stop_terminal_history_.push_back(std::move(record));
    for (const auto& operation_id : transaction.operations) {
      auto* operation = find_active_operation_locked(operation_id);
      if (operation != nullptr && operation->state.active_stop_id == certificate.stop_id &&
          operation->state.active_stop_generation == certificate.stop_generation) {
        if (transaction.motion_takeover_candidate_id &&
            !operation->pending_motion_takeover.empty() &&
            operation->pending_motion_takeover.key() == *transaction.motion_takeover_candidate_id) {
          auto& fallback = operation->pending_motion_takeover.mapped()->motion_takeover_spring_fallback;
          if (fallback && fallback->stop_id == certificate.stop_id &&
              fallback->stop_generation == certificate.stop_generation) {
            fallback->certificate = std::make_shared<const StopCertificateDTO>(certificate);
            auto& decision = operation->pending_motion_takeover.mapped()->state.motion_takeover;
            if (decision) {
              decision->stop_certificate = StopCertificateRefDTO{
                  certificate.stop_certificate_id, certificate.stop_certificate_revision,
                  certificate.stop_id, certificate.stop_generation};
              auto& candidate = *operation->pending_motion_takeover.mapped();
              stamp_operation_capture_locked(candidate, candidate.state);
            }
          }
        }
        // Retain only the exact qualification needed by recovery. Diagnostic
        // certificate rotation and later disjoint Stops cannot change this fact.
        const bool requested_controlled_stop_succeeded =
            certificate.result == StopCertificateResultDTO::kSucceededNominal &&
            !certificate.terminal_error &&
            std::all_of(operation->participants.begin(), operation->participants.end(),
                        [&](const auto& participant) {
              if (participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
                  !participant.fault_stop_plan ||
                  !participant.fault_stop_plan->participant_stop_profile ||
                  participant.fault_stop_plan->participant_stop_profile_purpose !=
                      ParticipantStopProfilePurposeDTO::kFault) return false;
              bool succeeded = false;
              for (const auto& attempt : certificate.attempted_stage_results) {
                const auto& result = attempt.result;
                const auto& stage = result.request;
                if (stage.operation_id != operation_id ||
                    stage.participant_id != participant.slot_owner.participant_id ||
                    attempt.authority != StopStageResultAuthorityDTO::kAuthoritative) continue;
                if (stage.stop_id != certificate.stop_id ||
                    stage.stop_generation != certificate.stop_generation || result.error ||
                    result.outcome != ParticipantStopStageOutcomeDTO::kSucceeded ||
                    result.attempt_disposition != StopStageAttemptDispositionDTO::kDispatched ||
                    stage.participant_stop_plan != *participant.fault_stop_plan ||
                    stage.stage_index >= stage.participant_stop_plan.stage_commands.size()) return false;
                const auto& command = stage.participant_stop_plan.stage_commands[stage.stage_index];
                if (command.resolved_stage.containment_command.command_kind !=
                        StopContainmentCommandKind::kMotionControlledStop ||
                    command.resolved_stage.command_qualification.stop_action_class !=
                        StopActionClass::kControlled) return false;
                succeeded = true;
              }
              return succeeded;
            });
        operation->settled_stop = ActiveOperation::SettledStop{
            certificate.stop_id, certificate.stop_generation, certificate.result,
            StopCertificateRefDTO{certificate.stop_certificate_id,
                                  certificate.stop_certificate_revision, certificate.stop_id,
                                  certificate.stop_generation},
            requested_controlled_stop_succeeded};
      }
    }
    const bool program_run_fence_releasable =
        certificate.result == StopCertificateResultDTO::kSucceededNominal ||
        certificate.result == StopCertificateResultDTO::kSucceededEscalated;
    for (auto& fence : program_run_stop_fences_) {
      if (fence.stop_id == certificate.stop_id &&
          fence.stop_generation == certificate.stop_generation) {
        fence.releasable = program_run_fence_releasable;
        bool selection_changed = false;
        for (auto& state : fence.node_states) {
          if (!program_run_fence_releasable) {
            selection_changed |= terminal_program_fault_changes_selection(state);
            if (state != ProgramNodeHoldStateDTO::kFaulted) state = ProgramNodeHoldStateDTO::kFaulted;
          } else if (state == ProgramNodeHoldStateDTO::kStopping) {
            // Contained work waits for Resume; every other state is kept.
            state = ProgramNodeHoldStateDTO::kHeld;
          }
        }
        if (selection_changed)
          advance_terminal_program_fault_selection(
              fence.selection_revision, fence.legacy_selection_revision, kMaximumKernelJsonInteger);
      }
    }
    reduce_program_selection_stop_locked(certificate);
    if (latch.has_value()) {
      cell_stop_fault_latch_generation_ =
          std::max(cell_stop_fault_latch_generation_, *latch_generation);
      if (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
          active_stop_fault_latch_->latch_generation <= *latch_generation)
        active_stop_fault_latch_ = *latch;
      motion_admission_closed_ = true;
    }
    static_cast<void>(
        emergency_stop_reserve_.release_after_terminal_handoff(transaction.stop_id->value()));
    return true;
  } catch (...) {
    return false;
  }
}

void ResourceAddressedExecutionKernel::ProgressStop(const StopControlWorkToken token) noexcept {
  try {
    std::optional<OperationId> takeover_source_wakeup;
    const ScopeExit wake_takeover([&] {
      if (!takeover_source_wakeup) return;
      try { static_cast<void>(advance_motion_takeover(*takeover_source_wakeup)); }
      catch (...) {
        std::lock_guard lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    });
    // Q60: late cancellation replies of never-dispatched participants whose
    // Stop already closed are tidied up on this lane, before any Stop work.
    settle_deferred_pre_dispatch_cancellations();
    struct PreparedCancellation final {
      OperationId operation_id;
      OperationParticipantId participant_id;
      OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
      IResolvedPreparedParticipantExecutionEngine* engine{};
      IResolvedParticipantExecutionEngine* execution_engine{};
      std::uint64_t execution_id{};
    };
    struct AddressedParticipantStopSettlement final {
      OperationId operation_id;
      OperationParticipantId participant_id;
      IResolvedParticipantExecutionEngine* engine{};
      AddressedParticipantStopSettlementRequestDTO request;
    };
    std::optional<std::int64_t> next_deadline;
    std::vector<StopControlWorkToken> dependents;
    std::vector<PreparedCancellation> prepared_cancellations;
    std::vector<PreparedCancellation> prepared_classifications;
    std::vector<AddressedParticipantStopSettlement> participant_settlements;
    std::vector<OperationControlEffectRequest> reset_effects;
    std::optional<StopControlWorkToken> terminal_projection;
    std::optional<std::pair<StopCertificateId, std::int64_t>> surveillance_schedule;
    bool transaction_became_terminal = false;
    {
      std::lock_guard state_lock(state_mutex_);
      if (!token.locally_valid() || token.slot >= stop_transactions_.size()) {
        motion_admission_closed_ = true;
        return;
      }
      auto& transaction = *stop_transactions_[token.slot];
      if (!transaction.active || transaction.token != token || transaction.terminal ||
          transaction.progress_in_flight || !transaction.stop_id.has_value()) {
        return;
      }
      transaction.progress_in_flight = true;
      const auto stop_id = *transaction.stop_id;
      const auto stop_generation = transaction.stop_generation;
      if (transaction.motion_takeover_candidate_id && transaction.operations.size() == 1U)
        takeover_source_wakeup = transaction.operations.front();
      const auto now = clock_.now();
      const auto* ordinary_stop = std::get_if<StopIntentDTO>(&transaction.projected_intent);
      const bool runtime_collision_containment =
          ordinary_stop != nullptr &&
          ordinary_stop->reason == std::optional<std::string>{"runtime_collision_fault"} &&
          ordinary_stop->request_context.source_label == "kernel.runtime_collision";
      // The Manager stopped a motion whose carried object was lost: a settled
      // participant ends Failed with that typed cause, so the Program node
      // fails through its normal failure handling.
      const bool carried_object_lost_stop =
          ordinary_stop != nullptr &&
          ordinary_stop->reason == std::optional<std::string>{"carried_object_lost"} &&
          ordinary_stop->request_context.source_label == "manager.carried_object_lost";
      const auto internal_stop = internal_stop_kind(ordinary_stop);
      // Neither a Reset nor a hold-timeout Stop is a degraded Stop.
      const bool internal_control_stop = internal_stop != InternalStopKind::kNone;

      if (!transaction.participant_work_initialized) {
        transaction.participants.reserve(transaction.operations.size());
        for (const auto& operation_id : transaction.operations) {
          const auto* operation = find_active_operation_locked(operation_id);
          if (operation == nullptr ||
              operation->state.participants.size() != operation->participants.size()) {
            motion_admission_closed_ = true;
            continue;
          }
          const auto accepted_revision =
              std::lower_bound(transaction.accepted->affected_operation_revisions.begin(),
                               transaction.accepted->affected_operation_revisions.end(),
                               operation_id, [](const auto& affected, const OperationId& id) {
                                 return affected.operation_id < id;
                               });
          if (accepted_revision == transaction.accepted->affected_operation_revisions.end() ||
              accepted_revision->operation_id != operation_id) {
            motion_admission_closed_ = true;
            continue;
          }
          for (std::size_t index = 0U; index < operation->participants.size(); ++index) {
            const auto& active_participant = operation->participants[index];
            const auto& participant_state = operation->state.participants[index];
            if (active_participant.non_motion_transaction()) continue;
            if (!transaction.participant_ids.empty() &&
                !std::binary_search(transaction.participant_ids.begin(),
                                    transaction.participant_ids.end(),
                                    participant_state.participant_id)) {
              continue;
            }
            if (transaction.global_interrupt_selection && !transaction.full_stop &&
                std::none_of(transaction.global_interrupt_selection->participants.begin(),
                             transaction.global_interrupt_selection->participants.end(),
                             [&](const auto& selected) {
                               return selected.operation_id == operation_id &&
                                      selected.participant_id == participant_state.participant_id;
                             }))
              continue;
            const auto& selected_active_stop_plan =
                runtime_collision_containment &&
                        active_participant.payload_kind ==
                            OperationParticipantPayloadKind::kMotion &&
                        active_participant.fault_stop_plan != nullptr
                    ? active_participant.fault_stop_plan
                    : active_participant.ordinary_stop_plan;
            if (selected_active_stop_plan == nullptr ||
                active_participant.slot_owner.participant_id != participant_state.participant_id) {
              motion_admission_closed_ = true;
              continue;
            }
            const auto* effective_plan = selected_active_stop_plan.get();
            const InterruptStopSelection* initial_interrupt = nullptr;
            if (transaction.interrupt_selection &&
                std::get<IoOperationRuleScopeDTO>(
                    transaction.interrupt_selection->registration.scope)
                        .operation_id == operation_id &&
                transaction.interrupt_selection->plan.participant_id ==
                    active_participant.slot_owner.participant_id) {
              effective_plan = &transaction.interrupt_selection->plan;
              initial_interrupt = &*transaction.interrupt_selection;
            }
            for (const auto& selection : transaction.disjoint_interrupt_selections) {
              const auto& owner = std::get<IoOperationRuleScopeDTO>(selection.registration.scope);
              if (owner.operation_id == operation_id &&
                  owner.participant_id == active_participant.slot_owner.participant_id) {
                effective_plan = &selection.plan;
                initial_interrupt = &selection;
              }
            }
            const GlobalInterruptStopParticipant* initial_global = nullptr;
            if (transaction.global_interrupt_selection) {
              const auto& members = transaction.global_interrupt_selection->participants;
              const auto selected = std::find_if(members.begin(), members.end(), [&](const auto& value) {
                return value.operation_id == operation_id &&
                       value.participant_id == participant_state.participant_id;
              });
              if (selected != members.end()) {
                initial_global = &*selected;
                effective_plan = &selected->plan;
              }
            }
            const auto& stop_plan = *effective_plan;
            const auto& stop_goal_id = participant_state.command_id;
            const auto stop_goal_generation = participant_state.command_generation;
            StopParticipantWork work{operation_id,
                                     accepted_revision->operation_revision_after_acceptance,
                                     active_participant.payload_kind,
                                     stop_goal_id,
                                     stop_goal_generation,
                                     active_participant.binding,
                                     *active_participant.stop_readiness,
                                     active_participant.retained_default_stop_limits,
                                     active_participant.stop_node_category,
                                     stop_plan,
                                     active_participant.resources,
                                     active_participant.stop_execution_port,
                                     active_participant.prepared_motion_engine,
                                     active_participant.engine,
                                     active_participant.preparation_id,
                                     active_participant.stop_cancelled_pre_dispatch,
                                     false,
                                     false,
                                     false,
                                     false,
                                     false,
                                     0U,
                                     1U,
                                     std::nullopt,
                                     {},
                                     {},
                                     false,
                                     false,
                                     std::nullopt,
                                     false,
                                     false,
                                     std::nullopt,
                                     false,
                                     false,
                                     std::nullopt,
                                     std::nullopt};
            if (initial_interrupt) {
              if (!VerifyPreparedIoRuleStopEntry(work.plan, initial_interrupt->stop_entry).ok()) {
                motion_admission_closed_ = true;
                continue;
              }
              work.current_stage_index =
                  static_cast<std::size_t>(initial_interrupt->stop_entry.stage_index);
              work.selected_interrupt_suffix = true;
            }
            if (initial_global) {
              if (!VerifyPreparedIoRuleStopEntry(work.plan, initial_global->stop_entry).ok()) {
                motion_admission_closed_ = true;
                continue;
              }
              work.current_stage_index =
                  static_cast<std::size_t>(initial_global->stop_entry.stage_index);
              work.selected_interrupt_suffix = true;
            }
            work.gripper_actuation = active_participant.gripper_actuation;
            work.retained_results.reserve(kMaxStopEscalationStages * 2U);
            if (runtime_collision_containment &&
                active_participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                active_participant.fault_stop_plan == nullptr) {
              // Admission required this exact prepared cancel row. The selected
              // ordinary plan stays a dispatched-goal member; no profile is invented.
              work.current_stage_index = controller_goal_cancel_stage_index(work.plan);
            }
            work.lane_hard_stop_requested =
                !work.plan.stage_commands.empty() &&
                (internal_stop == InternalStopKind::kZeroSpeedHold ||
                 (internal_stop == InternalStopKind::kReset &&
                  !active_participant.default_operator_stop_profile.has_value()));
            if (work.lane_hard_stop_requested) {
              work.current_stage_index = work.plan.stage_commands.size() - 1U;
            }
            for (auto& [pending_operation, pending_participant, pending_error] :
                 transaction.pending_stop_path_collisions) {
              if (pending_operation == operation_id &&
                  pending_participant == participant_state.participant_id &&
                  !work.collision_escalation_error.has_value()) {
                work.collision_escalation_pending = true;
                work.collision_escalation_error = pending_error;
              }
            }
            transaction.participants.push_back(std::move(work));
          }
        }
        transaction.participant_work_initialized = true;
      }

      for (auto& item : transaction.participants) {
        const auto retain_first_error = [&](ErrorDTO failure) {
          if (!item.terminal_error.has_value()) {
            item.terminal_error = std::move(failure);
          }
        };
        const auto advance_to_strongest = [&] {
          // The sealed chain is strength-ordered. Nominal failure/expiry goes
          // straight to its strongest authorized action, never another Soft
          // or Spring attempt. The original transaction budget is unchanged.
          const auto count = item.plan.stage_commands.size();
          item.current_stage_index = item.current_stage_index + 1U < count ? count - 1U : count;
        };
        const auto advance_after_failed_stage = [&] {
          if (item.selected_interrupt_suffix) {
            if (item.current_stage_index < item.plan.stage_commands.size())
              ++item.current_stage_index;
          } else {
            advance_to_strongest();
          }
        };
        if (item.terminal && item.terminal_projection_complete) {
          continue;
        }
        if (item.collision_escalation_pending) {
          // A collision on the Stop path aborts the running (controlled,
          // on-path or replacement) stage at once and selects the strongest
          // authorized stage, without waiting for the stage deadline. The
          // participant then ends InterventionRequired with the collision.
          item.collision_escalation_pending = false;
          auto collision_error = std::move(item.collision_escalation_error);
          item.collision_escalation_error.reset();
          if (!item.terminal && !item.cancelled_pre_dispatch && collision_error.has_value()) {
            if (!item.stop_path_collision.has_value()) item.stop_path_collision = collision_error;
            // Q29: the participant ends InterventionRequired with the
            // collision; no cell-wide Stop-fault latch (as `main`).
            const auto strongest = item.plan.stage_commands.empty()
                                       ? std::size_t{0}
                                       : item.plan.stage_commands.size() - 1U;
            if (item.current_stage_index < strongest) {
              auto& running = item.attempts[item.current_stage_index];
              if (running.started && !running.result_consumed && !running.superseded) {
                running.superseded = true;
                if (running.handle.has_value()) {
                  static_cast<void>(stop_stage_attempt_executor_->RequestCancel(*running.handle));
                }
              }
              retain_first_error(std::move(*collision_error));
              advance_to_strongest();
            }
          }
        }
        if (item.cancelled_pre_dispatch && !item.terminal) {
          bool preparation_in_flight = false;
          if (item.prepared_execution_id == 0U) {
            auto* operation = find_active_operation_locked(item.operation_id);
            ActiveOperationParticipant* participant = nullptr;
            if (operation != nullptr) {
              const auto found = std::find_if(
                  operation->participants.begin(), operation->participants.end(),
                  [&](const auto& candidate) {
                    return candidate.slot_owner.participant_id == item.plan.participant_id;
                  });
              if (found != operation->participants.end()) {
                participant = &*found;
              }
            }
            if (participant != nullptr) {
              preparation_in_flight =
                  participant->prepare_in_flight || participant->prepared_advance_in_flight;
              if (!preparation_in_flight) item.prepared_execution_id = participant->preparation_id;
              item.prepared_motion_engine = participant->prepared_motion_engine;
              item.execution_engine = participant->engine;
              if (participant->last_subordinate_error.has_value()) {
                retain_first_error(*participant->last_subordinate_error);
              }
            }
          }
          const bool needs_cancellation =
              item.prepared_motion_engine != nullptr && item.prepared_execution_id != 0U;
          if (needs_cancellation && !item.prepared_cancel_requested) {
            item.prepared_cancel_requested = true;
            prepared_cancellations.push_back({item.operation_id, item.plan.participant_id,
                                              item.payload_kind, item.prepared_motion_engine,
                                              item.execution_engine, item.prepared_execution_id});
          }
          // The bounded, local Prepare/read borrow and the synchronous
          // cancellation call still finish inside this Stop lane (this pass
          // issues the call below and re-enters directly). Q60: the
          // cancellation's own asynchronous terminal is not awaited.
          const bool waiting =
              preparation_in_flight || (needs_cancellation && !item.prepared_cancel_complete);
          const bool cancel_terminal_outstanding =
              needs_cancellation && item.prepared_cancel_complete &&
              item.payload_kind == OperationParticipantPayloadKind::kMotion &&
              !item.prepared_cancel_terminal_observed &&
              !item.unclassified_prepared_terminal.has_value();
          if (waiting) {
            // Q51: no shared Stop deadline; the never-dispatched participant's
            // own owner settles it (its completion wakes this Stop).
            continue;
          } else if (cancel_terminal_outstanding) {
            // Q60: nothing was dispatched and the dispatch barrier stays
            // closed, so the Stop closes now. The participant's settlement is
            // deferred to its cancellation terminal (below).
            item.cancel_terminal_deferred = true;
            item.contained = true;
            item.terminal = true;
          } else if (item.unclassified_prepared_terminal.has_value()) {
            if (item.prepared_motion_engine == nullptr || item.prepared_execution_id == 0U) {
              fail_prepared_cancellation(item, *item.unclassified_prepared_terminal);
              motion_admission_closed_ = true;
            } else {
              if (!item.prepared_classification_requested) {
                item.prepared_classification_requested = true;
                prepared_classifications.push_back(
                    {item.operation_id, item.plan.participant_id, item.payload_kind,
                     item.prepared_motion_engine, item.execution_engine,
                     item.prepared_execution_id});
              }
              continue;
            }
          } else {
            item.contained = true;
            item.terminal = true;
          }
        }

        const auto expire_attempt = [&](auto& attempt, const TimePoint observed_at) {
          if (!attempt.started || attempt.result_consumed || attempt.superseded ||
              !attempt.stage_deadline_ns.has_value() || observed_at < *attempt.stage_deadline_ns) {
            return false;
          }
          attempt.superseded = true;
          if (attempt.handle.has_value()) {
            static_cast<void>(stop_stage_attempt_executor_->RequestCancel(*attempt.handle));
          }
          retain_first_error(error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                                   "Controlled Stop did not prove containment before its deadline",
                                   "participant_stop_stage_deadline_expired"));
          // Ordinary watchdog expiry retains main's hard-floor jump. A
          // selected interrupt advances its same qualified suffix; the
          // transaction deadline and attempted-stage evidence are unchanged.
          advance_after_failed_stage();
          return true;
        };
        for (auto& attempt : item.attempts) {
          if (!attempt.started || attempt.result_consumed || !attempt.handle.has_value()) {
            continue;
          }
          if (transaction.motion_takeover_candidate_id) {
            auto* source = find_active_operation_locked(item.operation_id);
            if (source != nullptr && !source->pending_motion_takeover.empty() &&
                source->pending_motion_takeover.key() == *transaction.motion_takeover_candidate_id) {
              auto& candidate = *source->pending_motion_takeover.mapped();
              auto& fallback = candidate.motion_takeover_spring_fallback;
              if (fallback && !fallback->return_point && fallback->stop_id == stop_id &&
                  fallback->stop_generation == stop_generation &&
                  source->state.active_stop_id == stop_id &&
                  source->state.active_stop_generation == stop_generation) {
                auto point = stop_stage_attempt_executor_->ReadSpringReturnPoint(*attempt.handle);
                if (point && point->source_goal.controller_goal_id == item.controller_goal_id &&
                    point->source_goal.controller_goal_generation == item.controller_goal_generation &&
                    point->binding_epoch == point->trip.binding_epoch) {
                  fallback->stage = attempt.stage;
                  fallback->return_point = std::move(point);
                }
              }
            }
          }
          auto result = stop_stage_attempt_executor_->TryTake(*attempt.handle);
          const auto observed_at = clock_.now();
          // Deadline precedence applies even when a result is already queued.
          // Provider-reported elapsed time cannot backdate Kernel receipt.
          static_cast<void>(expire_attempt(attempt, observed_at));
          if (result.status == StopStageAttemptTakeStatus::kPending) {
            continue;
          }
          attempt.result_consumed = true;
          const auto released = stop_stage_attempt_executor_->Release(*attempt.handle);
          if (released != StopStageAttemptReleaseStatus::kReleased) {
            motion_admission_closed_ = true;
          }
          if (result.status == StopStageAttemptTakeStatus::kCompleted &&
              result.result.has_value()) {
            auto valid = ValidateParticipantStopStageResult(
                *result.result, item.readiness.value().resolved_stop_base_plan, item.binding,
                item.retained_default_stop_limits, item.node_category, item.resources);
            if (valid.ok()) {
              item.retained_results.push_back(StopAttemptedStageResultDTO{
                  StopScopeEntryKeyDTO{
                      stop_id, stop_generation,
                      StopParticipantScopeKeyDTO{item.operation_id, item.plan.participant_id}},
                  item.next_result_sequence++, observed_at,
                  attempt.superseded ? StopStageResultAuthorityDTO::kSupersededLate
                                     : StopStageResultAuthorityDTO::kAuthoritative,
                  std::move(*result.result)});
              if (!attempt.superseded) {
                const auto& retained = item.retained_results.back().result;
                if (retained.error.has_value()) {
                  retain_first_error(*retained.error);
                }
                if (retained.outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
                  item.contained = true;
                  item.terminal = true;
                  if (retained.request.stage_index > 0U && item.terminal_error.has_value()) {
                    item.escalation_cause = std::move(item.terminal_error);
                    // Q33: a hard stop the Stop planner chose is the intended
                    // Stop, so its certificate is nominal, not escalated.
                    if (planner_selected_hard_stop(*item.escalation_cause)) {
                      item.lane_hard_stop_requested = true;
                    }
                  }
                  item.terminal_error.reset();
                } else if (retained.request.stage_index == 0U &&
                           controlled_stop_unsettled_result(retained)) {
                  // Q18: the controller finished the Stop ramp outside its
                  // goal tolerance. As `main`'s confirmed-but-not-settled
                  // case: InterventionRequired, no second Stop command.
                  item.controlled_stop_unsettled = true;
                  item.terminal = true;
                  item.contained = false;
                  // `main`'s disposition details, within the bounded error
                  // shape the certificate accepts.
                  if (item.terminal_error.has_value() &&
                      item.terminal_error->details.size() <= 6U) {
                    item.terminal_error->details.insert_or_assign(
                        "emergency_fallback", "not_attempted_controlled_stop_already_confirmed");
                    item.terminal_error->details.insert_or_assign("hard_stop_transitioned",
                                                                  "false");
                  }
                } else {
                  // No cell-wide latch for an escalation (items 25, Q29).
                  advance_after_failed_stage();
                }
              }
            } else if (!attempt.superseded) {
              // Q29: an invalid provider result escalates; the participant's
              // outcome carries it, no cell-wide latch or admission closure.
              retain_first_error(valid.error());
              advance_after_failed_stage();
            }
          } else if (!attempt.superseded) {
            retain_first_error(error(std::string(to_string(ErrorCode::kInternalError)),
                                     "Stop-stage execution lane failed without a result",
                                     "stop_stage_attempt_infrastructure_failure"));
            advance_after_failed_stage();
          }
        }

        while (!item.terminal) {
          if (item.current_stage_index >= item.plan.stage_commands.size()) {
            // Q29: an exhausted cascade ends the participant
            // InterventionRequired without a cell-wide latch (as `main`).
            item.terminal = true;
            item.contained = false;
            if (!item.terminal_error.has_value()) {
              item.terminal_error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                          "Stop cascade exhausted without proof of containment",
                                          "participant_stop_cascade_exhausted");
            }
            break;
          }
          auto& attempt = item.attempts[item.current_stage_index];
          if (attempt.started && !attempt.result_consumed) {
            if (expire_attempt(attempt, clock_.now())) continue;
            if (attempt.stage_deadline_ns.has_value()) {
              next_deadline = !next_deadline.has_value()
                                  ? attempt.stage_deadline_ns
                                  : std::min(*next_deadline, *attempt.stage_deadline_ns);
            }
            break;
          }
          if (attempt.started) {
            advance_after_failed_stage();
            continue;
          }
          const auto dispatch_now = clock_.now();
          auto budget = remaining_stop_budget(item.plan.stage_commands[item.current_stage_index],
                                              stop_id, stop_generation, dispatch_now,
                                              transaction.accepted->accepted_at_ns);
          const auto watchdog = stage_watchdog_deadline(budget, dispatch_now);
          if (budget.remaining_controlled_deadline_ns.has_value() &&
              *budget.remaining_controlled_deadline_ns <= 0) {
            retain_first_error(error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                                     "Controlled Stop deadline passed before its dispatch",
                                     "participant_stop_stage_deadline_expired"));
            advance_after_failed_stage();
            continue;
          }
          if (item.port == nullptr) {
            retain_first_error(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                     "prepared Stop participant has no execution port",
                                     "participant_stop_execution_port_missing"));
            advance_after_failed_stage();
            continue;
          }
          auto stage = resolve_participant_stop_stage(item, stop_id, stop_generation,
                                                      item.current_stage_index, budget);
          if (!stage.ok()) {
            retain_first_error(stage.error());
            advance_after_failed_stage();
            continue;
          }
          attempt.stage =
              std::make_shared<const ResolvedParticipantStopStageDTO>(std::move(stage.value()));
          attempt.budget = std::make_shared<const RemainingStopBudgetDTO>(std::move(budget));
          const auto started = stop_stage_attempt_executor_->TryStart(
              StopStageAttemptRequest{item.port, attempt.stage, attempt.budget,
                  transaction.force_spring_return && item.current_stage_index == 0U,
                  &clock_, transaction.accepted->accepted_at_ns});
          if (started.status != StopStageAttemptStartStatus::kStarted ||
              !started.handle.has_value()) {
            item.terminal = true;
            item.contained = false;
            item.terminal_error =
                error(std::string(to_string(ErrorCode::kInvariantViolation)),
                      "prequalified Stop-stage isolation could not accept prepared work",
                      "stop_stage_attempt_prequalification_failed");
            break;
          }
          attempt.started = true;
          attempt.handle = *started.handle;
          attempt.stage_deadline_ns = watchdog;
          if (watchdog.has_value()) {
            next_deadline = !next_deadline.has_value()
                                ? watchdog
                                : std::optional(std::min(*next_deadline, *watchdog));
          }
          break;
        }

        if (!item.terminal) {
          continue;
        }
        auto* operation = find_active_operation_locked(item.operation_id);
        if (operation == nullptr || operation->state.active_stop_id != stop_id ||
            operation->state.active_stop_generation != stop_generation) {
          motion_admission_closed_ = true;
          continue;
        }
        const auto participant_state =
            std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                         [&](const auto& candidate) {
                           return candidate.participant_id == item.plan.participant_id;
                         });
        const auto active_participant =
            std::find_if(operation->participants.begin(), operation->participants.end(),
                         [&](const auto& candidate) {
                           return candidate.slot_owner.participant_id == item.plan.participant_id;
                         });
        if (participant_state == operation->state.participants.end() ||
            active_participant == operation->participants.end()) {
          motion_admission_closed_ = true;
          continue;
        }
        const bool participant_settlement_required = !item.settlement_not_required();
        if (item.contained && participant_settlement_required &&
            !item.participant_settlement_complete) {
          // Q58 (as `main`): the gripper's cancel is fire-and-forget and the
          // Stop does not wait for the still-returning command; a stuck
          // gripper cannot hold up the Stop. Its exact local settlement is
          // handed to the dispatch return (local cleanup needs the returned
          // command identity); the dispatch borrow stays retained until then.
          const bool defer_settlement =
              item.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
              active_participant->dispatch_effect_in_flight;
          if (!item.participant_settlement_complete && !item.participant_settlement_requested) {
            item.participant_settlement_requested = true;
            if (active_participant->engine == nullptr || item.prepared_execution_id == 0U) {
              item.participant_settlement_complete = true;
              item.participant_settlement_error =
                  error(std::string(to_string(ErrorCode::kInvariantViolation)),
                        "Stopped participant lost its local execution owner",
                        "participant_stop_settlement_authority_missing");
              motion_admission_closed_ = true;
            } else {
              std::optional<ParticipantStopStageResultDTO> settlement_evidence;
              if (item.cancelled_pre_dispatch) {
                auto budget = remaining_stop_budget(item.plan.stage_commands.front(), stop_id,
                                                    stop_generation, now,
                                                    transaction.accepted->accepted_at_ns);
                auto stage =
                    resolve_participant_stop_stage(item, stop_id, stop_generation, 0U, budget);
                if (stage.ok()) {
                  settlement_evidence =
                      ParticipantStopStageResultDTO{std::move(stage.value()),
                                                    StopStageAttemptDispositionDTO::kNotDispatched,
                                                    ParticipantStopStageOutcomeDTO::kSucceeded,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    0,
                                                    {},
                                                    std::nullopt};
                } else {
                  item.participant_settlement_complete = true;
                  item.participant_settlement_error = stage.error();
                  motion_admission_closed_ = true;
                }
              } else {
                const auto containment = std::find_if(
                    item.retained_results.rbegin(), item.retained_results.rend(),
                    [](const auto& candidate) {
                      return candidate.authority == StopStageResultAuthorityDTO::kAuthoritative &&
                             candidate.result.outcome ==
                                 ParticipantStopStageOutcomeDTO::kSucceeded &&
                             !candidate.result.error.has_value();
                    });
                if (containment != item.retained_results.rend()) {
                  settlement_evidence = containment->result;
                }
              }
              if (!item.participant_settlement_complete && !settlement_evidence.has_value()) {
                item.participant_settlement_complete = true;
                item.participant_settlement_error =
                    error(std::string(to_string(ErrorCode::kInvariantViolation)),
                          "Contained participant has no exact containment result",
                          "participant_stop_containment_result_missing");
                motion_admission_closed_ = true;
              } else if (settlement_evidence.has_value()) {
                AddressedParticipantStopSettlementRequestDTO settlement_request{
                    item.prepared_execution_id,
                    StopCertificateRefDTO{StopCertificateId("stop-certificate." + stop_id.value()),
                                          StopCertificateRevision(1U), stop_id, stop_generation},
                    std::move(*settlement_evidence), item.payload_kind,
                    item.cancelled_pre_dispatch};
                if (defer_settlement) {
                  active_participant->deferred_stop_settlement = std::move(settlement_request);
                  item.participant_settlement_complete = true;
                } else if (item.cancel_terminal_deferred) {
                  // Q60: as Q58, the exact settlement waits for the
                  // cancellation's own terminal; the Stop does not.
                  active_participant->deferred_cancel_settlement =
                      ActiveOperationParticipant::DeferredPreDispatchCancelSettlement{
                          std::move(settlement_request), token, std::nullopt, std::nullopt,
                          false};
                  item.participant_settlement_complete = true;
                } else {
                  participant_settlements.push_back(AddressedParticipantStopSettlement{
                      item.operation_id, item.plan.participant_id, active_participant->engine,
                      std::move(settlement_request)});
                }
              }
            }
          }
          if (!item.participant_settlement_complete) {
            continue;
          }
        }
        const auto rules_retired = retire_participant_io_rules_locked(*active_participant);
        if (!rules_retired.ok()) {
          motion_admission_closed_ = true;
          item.participant_settlement_error = rules_retired.error();
        }
        const bool io_source_failed = std::any_of(
            participant_state->io_rule_outcomes.begin(), participant_state->io_rule_outcomes.end(),
            [](const auto& outcome) { return !outcome.evidence.source_losses.empty(); });
        // A failed preparation keeps its owner's failure terminal and recovery
        // authority; containment had nothing physical to stop.
        const bool participant_settled =
            item.contained && !item.preparation_failed && !item.stop_path_collision.has_value() &&
            (!participant_settlement_required || (item.participant_settlement_complete &&
                                                  !item.participant_settlement_error.has_value()));
        // A runtime collision outside an operator Stop ends the colliding
        // participant InterventionRequired once contained, through the
        // ordinary outcome path (item 19); its siblings end Stopped.
        const bool runtime_collision_participant =
            runtime_collision_containment &&
            active_participant->runtime_collision_error.has_value();
        participant_state->state =
            item.participant_failure_preserved ? OperationStateKind::kFailed :
            participant_settled && !runtime_collision_participant
                ? (io_source_failed || carried_object_lost_stop ? OperationStateKind::kFailed
                                                                : OperationStateKind::kStopped)
                : OperationStateKind::kInterventionRequired;
        if (item.stop_path_collision.has_value()) {
          // The collision on the Stop path is the participant's cause.
          participant_state->terminal_error = item.stop_path_collision;
        } else if (!participant_settled && !participant_state->terminal_error.has_value()) {
          participant_state->terminal_error = item.participant_settlement_error.has_value()
                                                  ? item.participant_settlement_error
                                                  : item.terminal_error;
        } else if (io_source_failed && participant_settled && !runtime_collision_participant) {
          participant_state->terminal_error =
              error("interrupt_source_unavailable", "A required motion interrupt source failed",
                    "io_rule_required_source_lost");
        } else if (carried_object_lost_stop && participant_settled &&
                   !runtime_collision_participant) {
          participant_state->terminal_error =
              error("carried_object_lost", "The carried object was lost from its grasp",
                    "carried_object_lost");
        } else if (runtime_collision_participant) {
          participant_state->terminal_error = active_participant->runtime_collision_error;
        } else if (runtime_collision_containment &&
                   !participant_state->terminal_error.has_value()) {
          participant_state->terminal_error = active_participant->last_subordinate_error;
        } else if (participant_settled && item.escalation_cause.has_value() &&
                   !internal_control_stop && !planner_selected_hard_stop(*item.escalation_cause) &&
                   !participant_state->terminal_error.has_value()) {
          // A successful fallback hard stop ends Stopped with `main`'s
          // degraded-Stop error (item 25).
          const auto containing = std::find_if(
              item.retained_results.rbegin(), item.retained_results.rend(),
              [](const auto& candidate) {
                return candidate.authority == StopStageResultAuthorityDTO::kAuthoritative &&
                       candidate.result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded;
              });
          participant_state->terminal_error = degraded_stop_error(
              *item.escalation_cause, item.plan,
              containing != item.retained_results.rend() ? containing->result.request.stage_index
                                                         : item.plan.stage_commands.size());
        }
        const bool all_participants_terminal =
            std::all_of(operation->state.participants.begin(), operation->state.participants.end(),
                        [](const auto& candidate) { return terminal(candidate.state); });
        if (all_participants_terminal) {
          const bool has_pneumatic_transaction = std::any_of(
              operation->participants.begin(), operation->participants.end(),
              [](const auto& row) { return row.finishes_current_gripper_transaction(); });
          operation->state.state =
              (has_pneumatic_transaction || io_source_failed)
                  ? aggregate_participant_state(operation->state.participants)
              : std::all_of(operation->state.participants.begin(),
                            operation->state.participants.end(),
                            [](const auto& candidate) {
                              return candidate.state == OperationStateKind::kStopped;
                            })
                  ? OperationStateKind::kStopped
                  : OperationStateKind::kInterventionRequired;
        } else {
          // Once the selected moving rows have settled, ordinary siblings
          // retain their truthful aggregate state independently of Stop.
          const bool moving_settled = std::all_of(
              operation->participants.begin(), operation->participants.end(),
              [&](const auto& candidate) {
                if (!candidate.stop_owns_reconciliation()) return true;
                const auto row = std::find_if(
                    operation->state.participants.begin(), operation->state.participants.end(),
                    [&](const auto& state) {
                      return state.participant_id == candidate.slot_owner.participant_id;
                    });
                return row != operation->state.participants.end() && terminal(row->state);
              });
          if (moving_settled)
            operation->state.state = aggregate_participant_state(operation->state.participants);
        }
        operation->state.updated_at_ns = now;
        // A production Stop backend can publish its terminal state before the
        // isolated Stop-stage result is consumed.  That callback already
        // terminalizes the same resource slots under state_mutex_.  Treat the
        // subsequent certificate commit as the same terminal fact instead of
        // attempting the illegal Terminal -> Terminal transition and masking
        // the original Stop outcome as an intervention.
        if (active_participant->slot_state != ResourceSlotState::kTerminal) {
          const auto transitioned = resource_slots_->transition(active_participant->slot_owner,
                                                                active_participant->resources,
                                                                ResourceSlotState::kTerminal);
          if (transitioned.ok()) {
            active_participant->slot_state = ResourceSlotState::kTerminal;
          } else {
            motion_admission_closed_ = true;
            operation->state.state = OperationStateKind::kInterventionRequired;
            participant_state->state = OperationStateKind::kInterventionRequired;
            participant_state->terminal_error = transitioned.error();
          }
        }
        if (item.payload_kind == OperationParticipantPayloadKind::kMotion) {
          if (item.preparation_failed) {
            // The exact prepared snapshot proved this failure never
            // dispatched. Preserve any already-published local failure.
            if (!participant_state->terminal_fact)
              participant_state->terminal_fact = ParticipantExecutionFact{
                  ParticipantExecutionFactKind::kLocalFailure,
                  ParticipantEffectKnowledge::kNoEffect, participant_state->terminal_error};
          } else if (participant_settled && !runtime_collision_participant &&
                     !item.participant_failure_preserved) {
            // These are the same containment/local-settlement proofs used
            // above, not an inference from the public Stopped state. A
            // deferred pre-dispatch cleanup still retains its existing borrow.
            participant_state->terminal_fact = ParticipantExecutionFact{
                item.cancelled_pre_dispatch
                    ? ParticipantExecutionFactKind::kCancelledBeforeDispatch
                    : ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                item.cancelled_pre_dispatch ? ParticipantEffectKnowledge::kNoEffect
                                            : ParticipantEffectKnowledge::kEffectSettled,
                participant_state->terminal_error};
          }
        }
        item.terminal_projection_complete = true;
      }

      for (const auto& operation_id : transaction.operations) {
        auto* operation = find_active_operation_locked(operation_id);
        if (operation != nullptr &&
            std::all_of(transaction.participants.begin(), transaction.participants.end(),
                        [&](const auto& participant) {
                          return participant.operation_id != operation_id ||
                                 (participant.terminal &&
                                  participant.terminal_projection_complete &&
                                  (!participant.contained ||
                                   participant.settlement_not_required() ||
                                   participant.participant_settlement_complete));
                        })) {
          operation->stop_effect_in_flight = false;
        }
      }

      transaction.fresh_scope_complete =
          std::all_of(transaction.participants.begin(), transaction.participants.end(),
                      [](const auto& participant) {
                        return participant.terminal && participant.terminal_projection_complete &&
                               (!participant.contained ||
                                participant.settlement_not_required() ||
                                participant.participant_settlement_complete);
                      });
      if (transaction.fresh_scope_complete) {
        const bool predecessors_terminal = std::all_of(
            transaction.predecessor_tokens.begin(), transaction.predecessor_tokens.end(),
            [&](const auto& predecessor_token) {
              return predecessor_token.slot < stop_transactions_.size() &&
                     stop_transactions_[predecessor_token.slot]->active &&
                     stop_transactions_[predecessor_token.slot]->token == predecessor_token &&
                     stop_transactions_[predecessor_token.slot]->certificate.has_value();
            });
        transaction_became_terminal = transaction.fresh_scope_complete && predecessors_terminal;
        if (transaction_became_terminal) {
          // The immutable local certificate is the terminal handoff boundary.
          // Do not expose terminal state, release the emergency reserve slot,
          // or wake dependent aggregates until the complete predecessor and
          // fresh-scope truth has been reduced and retained locally.
          if (!commit_stop_terminal_locked(transaction, now)) {
            transaction.progress_in_flight = false;
            motion_admission_closed_ = true;
            return;
          }
          transaction.terminal = true;
          const auto retain_terminal_capture = [&](const OperationStateDTO& capture) {
            if (!transaction.terminal_projection_token.has_value()) return;
            auto& states = stop_projection_jobs_[transaction.terminal_projection_token->slot]
                               ->terminal_operation_states;
            const auto row = std::find_if(states.begin(), states.end(), [&](const auto& item) {
              return item.operation_id == capture.operation_id;
            });
            if (row != states.end()) *row = capture;
          };
          for (const auto& operation_id : stop_operation_snapshot_ids(transaction)) {
            auto* operation = find_active_operation_locked(operation_id);
            if (operation == nullptr || !operation->pending_control.has_value() ||
                !operation->pending_control->durable) {
              continue;
            }
            if (operation->has_pending_io_rule_response()) continue;
            auto& pending = *operation->pending_control;
            if (pending.accepted.control != OperationControlKind::kReset) {
              if (pending.accepted.control == OperationControlKind::kAcknowledgeCompleted &&
                  operation->ordinary_pneumatic_acknowledgement() &&
                  !pending.first_failure.has_value()) {
                // Either the ordinary ACK callback or this exact certificate
                // is last. Preserve the durable claim until both arrive.
                settle_completed_control_locked(operation_id);
                continue;
              }
              if (pending.first_failure.has_value()) {
                retain_terminal_capture(capture_control_failure_locked(*operation));
              }
              if (pending.completed_effects != pending.expected_effects) {
                // Retain the exact control claim until the bounded control
                // lane reports completion.
                continue;
              }
              operation->pending_control.reset();
              continue;
            }
            if (pending.effects_dispatched) {
              // A Reset already executing when this Stop arrived remains one
              // effect. Either its callback or this certificate handoff is
              // last; only the latter may release Stop-owned resources.
              if (pending.completed_effects == pending.expected_effects) {
                if (pending.first_failure.has_value()) {
                  const auto fault = publish_settlement_fault_locked(*operation);
                  // Reuse this Stop's already-reserved durable projection;
                  // never perform ordinary fault I/O on the Stop worker.
                  retain_terminal_capture(fault);
                } else if (!settle_control_operation_locked(operation_id,
                                                            OperationControlKind::kReset)
                                .ok()) {
                  motion_admission_closed_ = true;
                }
              }
              continue;
            }
            auto requests =
                reset_effect_requests(operation_id, operation->participants,
                                      pending.accepted.operation_revision_after_acceptance);
            if (!requests.ok() || pending.completed_effects != 0U) {
              operation->state.state = OperationStateKind::kInterventionRequired;
              operation->state.updated_at_ns = now;
              operation->pending_control.reset();
              motion_admission_closed_ = true;
              continue;
            }
            pending.superseded_by_stop = false;
            // Stop may have cancelled a motion prepared after Reset admission.
            // Its inner execution now needs cleanup too. Freeze this bounded
            // effect set at dispatch, not before containment knows the set.
            pending.expected_effects = requests.value().size();
            pending.effects_dispatched = true;
            if (requests.value().empty()) {
              if (!settle_control_operation_locked(operation_id, OperationControlKind::kReset)
                       .ok()) {
                motion_admission_closed_ = true;
              }
              continue;
            }
            reset_effects.insert(reset_effects.end(),
                                 std::make_move_iterator(requests.value().begin()),
                                 std::make_move_iterator(requests.value().end()));
          }
          if (transaction.acceptance_projection_state !=
              EmergencyStopProjectionStateDTO::kPending) {
            terminal_projection = transaction.terminal_projection_token;
          }
          const auto terminal_record = std::find_if(
              stop_terminal_history_.begin(), stop_terminal_history_.end(),
              [&](const auto& record) {
                return record != nullptr &&
                       record->certificate.stop_id == transaction.certificate->stop_id &&
                       record->certificate.stop_generation ==
                           transaction.certificate->stop_generation;
              });
          if (terminal_record != stop_terminal_history_.end() &&
              (*terminal_record)->surveillance_state != nullptr) {
            surveillance_schedule = std::pair((*terminal_record)->certificate.stop_certificate_id,
                                              (*terminal_record)->next_surveillance_sample_ns);
          }
          for (const auto& candidate : stop_transactions_) {
            if (candidate == nullptr || !candidate->active || candidate->terminal) {
              continue;
            }
            const auto predecessor = std::find(candidate->predecessor_tokens.begin(),
                                               candidate->predecessor_tokens.end(), token);
            if (predecessor != candidate->predecessor_tokens.end()) {
              candidate->predecessor_certificates.push_back(*transaction.certificate);
              std::sort(
                  candidate->predecessor_certificates.begin(),
                  candidate->predecessor_certificates.end(),
                  [](const auto& left, const auto& right) { return left.stop_id < right.stop_id; });
              candidate->predecessor_tokens.erase(predecessor);
              stop_control_executor_->ArmDeadline(candidate->token, now);
              dependents.push_back(candidate->token);
            }
          }
        }
      }
      transaction.progress_in_flight = false;
      if (transaction_became_terminal) {
        stop_control_executor_->CancelDeadline(token);
      } else if (next_deadline.has_value()) {
        stop_control_executor_->ArmDeadline(token, *next_deadline);
      }
    }
    for (const auto& handoff : participant_settlements) {
      Result<AddressedParticipantStopSettlementDTO> settled =
          Result<AddressedParticipantStopSettlementDTO>::Err(
              error(std::string(to_string(ErrorCode::kInternalError)),
                    "addressed motion Stop settlement did not return a result",
                    "participant_stop_settlement_failed"));
      try {
        settled = handoff.engine->settle_addressed_participant_stop(handoff.request);
      } catch (const std::exception& exception) {
        settled = Result<AddressedParticipantStopSettlementDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  std::string("addressed motion Stop settlement threw: ") + exception.what(),
                  "participant_stop_settlement_threw"));
      } catch (...) {
        settled = Result<AddressedParticipantStopSettlementDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  "addressed motion Stop settlement threw", "participant_stop_settlement_threw"));
      }
      if (settled.ok()) {
        const auto& value = settled.value();
        const auto& request = handoff.request;
        const auto& stage = request.containment.request;
        const auto& recovery = value.recovery_assessment;
        const bool failed_contained = value.terminal_fact &&
            is_settled_execution_failure(*value.terminal_fact) &&
            request.payload_kind == OperationParticipantPayloadKind::kMotion &&
            recovery.execution_state == ExecutionState::kFailed &&
            !recovery.retry_admissible;
        const bool ordinary_stopped = !value.terminal_fact &&
            recovery.execution_state == ExecutionState::kStopped &&
            recovery.recovery_class == RecoveryClass::kOrdinaryOperatorStop &&
            recovery.status == RecoveryAssessmentStatus::kRetryReady && recovery.retry_admissible;
        const bool exact = value.execution_id == request.execution_id &&
                           value.aggregate_stop_certificate == request.aggregate_stop_certificate &&
                           value.operation_id == stage.operation_id &&
                           value.participant_id == stage.participant_id &&
                           value.controller_goal_id == stage.controller_goal_id &&
                           value.controller_goal_generation == stage.controller_goal_generation &&
                           recovery.execution_id == request.execution_id &&
                           (ordinary_stopped || failed_contained) && recovery.backend_stop_confirmed &&
                           recovery.stop_owner_generation != 0U &&
                           recovery.stop_certificate_id ==
                               request.aggregate_stop_certificate.stop_certificate_id.value() &&
                           recovery.stop_certificate_revision ==
                               request.aggregate_stop_certificate.stop_certificate_revision.value();
        if (!exact) {
          settled = Result<AddressedParticipantStopSettlementDTO>::Err(
              error(std::string(to_string(ErrorCode::kInvariantViolation)),
                    "participant returned a mismatched addressed Stop settlement",
                    "participant_stop_settlement_mismatch"));
        }
      }
      std::lock_guard state_lock(state_mutex_);
      if (token.slot >= stop_transactions_.size()) {
        motion_admission_closed_ = true;
        continue;
      }
      auto& transaction = *stop_transactions_[token.slot];
      if (!transaction.active || transaction.token != token) {
        continue;
      }
      const auto item =
          std::find_if(transaction.participants.begin(), transaction.participants.end(),
                       [&](const auto& candidate) {
                         return candidate.operation_id == handoff.operation_id &&
                                candidate.plan.participant_id == handoff.participant_id;
                       });
      if (item == transaction.participants.end() ||
          item->prepared_execution_id != handoff.request.execution_id ||
          !item->participant_settlement_requested) {
        motion_admission_closed_ = true;
        continue;
      }
      item->participant_settlement_complete = true;
      if (settled.ok() && settled.value().terminal_fact) {
        auto* operation = find_active_operation_locked(handoff.operation_id);
        auto state = operation == nullptr ? nullptr : [&]() -> OperationParticipantStateDTO* {
          const auto row = std::find_if(operation->state.participants.begin(),
                                       operation->state.participants.end(), [&](const auto& candidate) {
            return candidate.participant_id == handoff.participant_id;
          });
          return row == operation->state.participants.end() ? nullptr : &*row;
        }();
        const auto& fact = *settled.value().terminal_fact;
        if (state == nullptr || (state->terminal_fact && *state->terminal_fact != fact) ||
            (state->terminal_error && state->terminal_error != fact.error)) {
          settled = Result<AddressedParticipantStopSettlementDTO>::Err(
              error(std::string(to_string(ErrorCode::kInvariantViolation)),
                    "participant returned a different contained failure",
                    "participant_stop_settlement_mismatch"));
        } else {
          state->terminal_fact = fact;
          state->terminal_error = fact.error;
          item->participant_failure_preserved = true;
        }
      }
      if (!settled.ok()) {
        // Operation-scoped: the participant ends InterventionRequired and
        // its resources stay owned until Reset; the cell stays admissible.
        item->participant_settlement_error = settled.error();
      }
    }
    for (const auto& cancellation : prepared_cancellations) {
      std::optional<ExecutionStatus> current;
      Result<void> cancelled = Result<void>::Err(error(
          std::string(to_string(ErrorCode::kInternalError)),
          "prepared motion cancellation did not return a result", "prepared_motion_cancel_failed"));
      try {
        cancelled =
            cancellation.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand
                ? cancellation.engine->CancelPreparedCapabilityCommand(cancellation.execution_id)
                : cancellation.engine->CancelPreparedMotion(cancellation.execution_id);
        if (cancelled.ok() &&
            cancellation.payload_kind == OperationParticipantPayloadKind::kMotion) {
          // Acceptance can fence and settle the worker before participant
          // work exists. Idempotent cancellation need not repeat that event.
          // Read the existing local status outside the owner lock; later
          // terminals still use the subscribed event wakeup. No provider I/O.
          current = cancellation.execution_engine->get_status();
        }
      } catch (const std::exception& exception) {
        cancelled = Result<void>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  std::string("prepared motion cancellation threw: ") + exception.what(),
                  "prepared_motion_cancel_threw"));
      } catch (...) {
        cancelled = Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                            "prepared motion cancellation threw",
                                            "prepared_motion_cancel_threw"));
      }
      {
        std::lock_guard state_lock(state_mutex_);
        if (token.slot >= stop_transactions_.size()) {
          motion_admission_closed_ = true;
          continue;
        }
        auto& transaction = *stop_transactions_[token.slot];
        if (!transaction.active || transaction.token != token) {
          continue;
        }
        const auto item =
            std::find_if(transaction.participants.begin(), transaction.participants.end(),
                         [&](const auto& candidate) {
                           return candidate.operation_id == cancellation.operation_id &&
                                  candidate.plan.participant_id == cancellation.participant_id;
                         });
        if (item == transaction.participants.end() ||
            item->prepared_execution_id != cancellation.execution_id) {
          motion_admission_closed_ = true;
          continue;
        }
        item->prepared_cancel_complete = true;
        if (!cancelled.ok()) {
          fail_prepared_cancellation(*item, cancelled.error());
        } else if (item->payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
          // Capability preparation has no asynchronous worker terminal: the
          // successful cancellation call is its complete local no-effect
          // transition. Motion publishes the corresponding engine event.
          item->prepared_cancel_terminal_observed = true;
        } else if (current.has_value()) {
          observe_prepared_cancellation_terminal(
              *item, current->execution_id, participant_operation_state(*current),
              participant_fact_error(*current).value_or(status_error(current->last_error)));
          if (item->terminal_error.has_value()) motion_admission_closed_ = true;
        }
      }
    }
    for (const auto& classification : prepared_classifications) {
      // Read only after a failure terminal was observed. The prepared owner's
      // exact snapshot proves an already-failed, never-dispatched preparation;
      // anything else keeps the failed-cancellation classification.
      std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> prepared;
      try {
        auto snapshot = classification.engine->ObservePreparedMotion(classification.execution_id);
        if (snapshot.ok()) prepared = std::move(snapshot.value());
      } catch (...) {
        prepared.reset();
      }
      std::lock_guard state_lock(state_mutex_);
      if (token.slot >= stop_transactions_.size()) {
        motion_admission_closed_ = true;
        continue;
      }
      auto& transaction = *stop_transactions_[token.slot];
      if (!transaction.active || transaction.token != token) continue;
      const auto item =
          std::find_if(transaction.participants.begin(), transaction.participants.end(),
                       [&](const auto& candidate) {
                         return candidate.operation_id == classification.operation_id &&
                                candidate.plan.participant_id == classification.participant_id;
                       });
      if (item == transaction.participants.end() ||
          item->prepared_execution_id != classification.execution_id ||
          !item->unclassified_prepared_terminal.has_value()) {
        motion_admission_closed_ = true;
        continue;
      }
      if (prepared.has_value() && prepared->execution_id == classification.execution_id &&
          prepared->state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed &&
          !prepared->last_dispatched_participant) {
        settle_failed_preparation(
            *item, prepared->error.value_or(*item->unclassified_prepared_terminal));
      } else {
        fail_prepared_cancellation(*item, *item->unclassified_prepared_terminal);
        motion_admission_closed_ = true;
      }
    }
    if (!prepared_cancellations.empty() || !prepared_classifications.empty() ||
        !participant_settlements.empty()) {
      // The executor coalesces identical token publications. Finish the
      // bounded, effect-free cancellation pass directly after releasing the
      // state lock so this same callback can commit terminal no-effect truth
      // without waiting for an artificial watchdog generation.
      ProgressStop(token);
      return;
    }
    for (auto& effect : reset_effects) {
      const auto identity = effect;
      auto started = operation_control_executor_->TryStart(std::move(effect));
      if (started.status == OperationControlEffectStartStatus::kStarted) {
        continue;
      }
      {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
      CompleteOperationControlEffect(OperationControlEffectResult{
          identity.operation_id, identity.participant_id, identity.operation_revision,
          identity.control, false,
          error(std::string(to_string(ErrorCode::kInvariantViolation)),
                "prequalified operation-control lane rejected certified Reset work",
                "operation_control_effect_handoff_failed")});
    }
    if (transaction_became_terminal) {
      StopWorkHandoff global_handoff;
      {
        std::lock_guard lock(state_mutex_);
        if (global_io_rule_catalog_) {
          const auto evaluated = global_io_rule_service_->EvaluateRules();
          const auto processed = evaluated.ok()
              ? process_global_io_rule_evidence_locked(global_handoff)
              : evaluated;
          if (!processed.ok()) global_io_rule_projection_error_ = processed.error();
        }
      }
      // The existing control lane performs no recovery SQL. Reevaluate idle
      // rules once at this actual terminal boundary, then publish work only.
      publish_stop_work(global_handoff);
      if (terminal_projection.has_value()) {
        stop_projection_worker_->Publish(*terminal_projection);
      }
      for (const auto& dependent : dependents) {
        stop_control_executor_->Publish(dependent);
      }
      if (surveillance_schedule.has_value()) {
        ScheduleStopSurveillance(std::move(surveillance_schedule->first),
                                 surveillance_schedule->second);
      }
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
    if (token.locally_valid() && token.slot < stop_transactions_.size()) {
      auto& transaction = *stop_transactions_[token.slot];
      if (transaction.active && transaction.token == token) {
        transaction.progress_in_flight = false;
      }
    }
  }
}

}  // namespace manyforge::core
