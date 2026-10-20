#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {

namespace {
bool recovery_request_names_assessment(const KernelRecoveryActionRequestDTO& request,
                                       const KernelRecoveryAssessmentDTO& assessment) {
  return request.expected_assessment_revision == assessment.assessment_revision &&
         request.expected_control_command_epoch == assessment.control_command_epoch &&
         request.expected_execution_id == assessment.execution_id &&
         request.expected_latch_id == assessment.latch_id &&
         request.expected_stop_certificate_id == assessment.stop_certificate_id &&
         request.expected_stop_owner_generation == assessment.stop_owner_generation;
}
bool recovery_requests_match(const KernelRecoveryActionRequestDTO& left,
                             const KernelRecoveryActionRequestDTO& right) {
  return left.request_id == right.request_id && left.action == right.action &&
         left.principal_id == right.principal_id &&
         left.expected_assessment_revision == right.expected_assessment_revision &&
         left.expected_control_command_epoch == right.expected_control_command_epoch &&
         left.expected_execution_id == right.expected_execution_id &&
         left.expected_latch_id == right.expected_latch_id &&
         left.expected_stop_certificate_id == right.expected_stop_certificate_id &&
         left.expected_stop_owner_generation == right.expected_stop_owner_generation &&
         left.reason == right.reason && left.fresh_motion_attempt == right.fresh_motion_attempt;
}

}  // namespace

Result<OperationStateDTO> ResourceAddressedExecutionKernel::get_operation_state(
    const OperationId& operation_id) const {
  auto projection = get_operation_projection(operation_id);
  if (!projection.ok()) {
    return Result<OperationStateDTO>::Err(std::move(projection.error()));
  }
  return Result<OperationStateDTO>::Ok(std::move(projection.value().operation));
}

OperationReadProjectionDTO ResourceAddressedExecutionKernel::operation_projection_locked(
    const OperationStateDTO& state, const bool active, const bool control_pending) const {
  // Residency is the Kernel's authority over the operation, not table
  // membership. A retired manual operation still holds its cell-admission
  // claim until the owner releases it (advancing the admission generation);
  // publishing non-resident earlier lets a reader capture a generation that
  // the pending release is about to invalidate.
  const bool resident = active || (manual_operation_claim_.has_value() &&
                                   manual_operation_claim_->intent.owner_transaction_id ==
                                       state.operation_id.value());
  const auto control = std::find_if(
      control_replay_ledger_.begin(), control_replay_ledger_.end(), [&](const auto& entry) {
        return entry.durable && entry.accepted.operation_id == state.operation_id;
      });
  return {state, resident, config_.request_owner_epoch,
          control == control_replay_ledger_.end() ? std::nullopt : std::optional(control->accepted),
          control_pending};
}

Result<OperationReadProjectionDTO> ResourceAddressedExecutionKernel::get_operation_projection(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<OperationReadProjectionDTO>("operation identity is empty", "operation_id_empty",
                                              ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto* active_operation = find_active_operation_locked(operation_id);
  if (active_operation != nullptr) {
    return Result<OperationReadProjectionDTO>::Ok(operation_projection_locked(
        active_operation->state, true, active_operation->pending_control.has_value()));
  }
  if (const auto* pending = find_pending_motion_operation_locked(operation_id)) {
    return Result<OperationReadProjectionDTO>::Ok(
        operation_projection_locked(pending->state, true, pending->pending_control.has_value()));
  }
  const auto historical = std::find_if(
      historical_operations_.begin(), historical_operations_.end(),
      [&](const OperationStateDTO& state) { return state.operation_id == operation_id; });
  if (historical != historical_operations_.end()) {
    return Result<OperationReadProjectionDTO>::Ok(
        operation_projection_locked(*historical, false, false));
  }
  return reject<OperationReadProjectionDTO>("operation is not resident or retained",
                                            "operation_not_resident", ErrorCode::kMissingReference);
}

std::vector<OperationStateDTO> ResourceAddressedExecutionKernel::get_resident_operations(
    const std::vector<ProgramRunId>& program_run_ids) const {
  std::lock_guard state_lock(state_mutex_);
  std::vector<OperationStateDTO> operations;
  operations.reserve(active_operations_.size());
  for (const auto& [operation_id, operation] : active_operations_) {
    static_cast<void>(operation_id);
    if (std::find(program_run_ids.begin(), program_run_ids.end(),
                  operation->state.program_run_id) != program_run_ids.end()) {
      operations.push_back(operation->state);
      for (const auto* slot : {&operation->pending_motion_takeover, &operation->next_motion_takeover})
        if (!slot->empty()) operations.push_back(slot->mapped()->state);
    }
  }
  return operations;
}

std::vector<OperationReadProjectionDTO> ResourceAddressedExecutionKernel::get_latest_run_operations(
    const std::vector<ProgramRunId>& program_run_ids) const {
  std::lock_guard state_lock(state_mutex_);
  std::vector<OperationReadProjectionDTO> result;
  for (const auto& run_id : program_run_ids) {
    const auto before = result.size();
    for (const auto& [id, operation] : active_operations_) {
      static_cast<void>(id);
      if (operation->state.program_run_id == run_id) {
        result.push_back(operation_projection_locked(operation->state, true,
                                                     operation->pending_control.has_value()));
        for (const auto* slot : {&operation->pending_motion_takeover, &operation->next_motion_takeover})
          if (!slot->empty())
            result.push_back(operation_projection_locked(slot->mapped()->state, true,
                                                         slot->mapped()->pending_control.has_value()));
      }
    }
    if (result.size() != before) continue;
    const auto latest =
        std::find_if(historical_operations_.rbegin(), historical_operations_.rend(),
                     [&](const auto& operation) { return operation.program_run_id == run_id; });
    if (latest != historical_operations_.rend())
      result.push_back(operation_projection_locked(*latest, false, false));
  }
  return result;
}

std::optional<std::vector<std::size_t>>
ResourceAddressedExecutionKernel::fresh_motion_node_indices_locked(
    const ActiveOperation& operation) const {
  if (operation.manual || !program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
      program_cycle_claim_run_->program_run_id != operation.state.program_run_id ||
      operation.participants.empty() ||
      operation.participants.size() != operation.state.participants.size())
    return std::nullopt;
  const auto& capture = *program_cycle_claim_run_->capture;
  if (!capture.table) return std::nullopt;
  std::vector<std::size_t> indices;
  for (std::size_t index = 0; index < operation.participants.size(); ++index) {
    const auto& participant = operation.participants[index];
    const auto& scope = operation.state.participants[index].execution_scope;
    if (participant.payload_kind != OperationParticipantPayloadKind::kMotion || !scope ||
        scope->node_attempt_id.empty())
      return std::nullopt;
    const auto node = std::lower_bound(
        capture.table->nodes.begin(), capture.table->nodes.end(), scope->node_instance_id,
        [](const auto& row, const auto& id) { return row.instance_id < id; });
    if (node == capture.table->nodes.end() || node->instance_id != scope->node_instance_id ||
        node->owner_api != "kernel.operation")
      return std::nullopt;
    const auto node_index = static_cast<std::size_t>(node - capture.table->nodes.begin());
    if (node_index >= program_cycle_claim_run_->motion_continuations.size()) return std::nullopt;
    indices.push_back(node_index);
  }
  return indices;
}

MotionRetryEligibilityDTO ResourceAddressedExecutionKernel::operation_retry_eligibility_locked(
    const ActiveOperation& operation, const std::vector<KernelRecoveryAssessmentDTO>& assessments,
    const bool recovery_owner) const {
  const auto refuse = [](MotionRetryIneligibilityReason reason,
                         MotionRetryEffect effect = MotionRetryEffect::kEffectUncertain) {
    return MotionRetryEligibilityDTO{false, MotionRetryCause::kNotEligible, effect, reason};
  };
  if (operation.retry_run_ended)
    return refuse(MotionRetryIneligibilityReason::kProgramScopeRequired);
  if (is_active_stop_fault_latch(active_stop_fault_latch_))
    return refuse(MotionRetryIneligibilityReason::kLatchPresent);
  if ((!recovery_owner && operation.recovery_effect_in_flight) || operation.stop_effect_in_flight ||
      operation.ordinary_commit_in_flight || operation.ordinary_acceptance_pending ||
      operation.pending_control || operation.runtime_scale_effect_in_flight ||
      operation.coordinated_cancellation_in_flight || operation.dispatch_effect_in_flight() ||
      operation.reconciliation_in_flight() ||
      std::any_of(operation.participants.begin(), operation.participants.end(),
                  [](const auto& row) { return row.preparation_borrow_in_flight(); }))
    return refuse(MotionRetryIneligibilityReason::kOperationInFlight);
  if (operation.participants.size() != 1U) return {};
  if (assessments.empty() || assessments.size() != operation.participants.size() ||
      std::any_of(operation.participants.begin(), operation.participants.end(),
                  [](const auto& row) {
                    return row.payload_kind != OperationParticipantPayloadKind::kMotion;
                  }))
    return {};
  const bool collision =
      std::any_of(operation.participants.begin(), operation.participants.end(),
                  [](const auto& row) { return row.runtime_collision_error.has_value(); });
  if (!collision) {
    if (operation.state.active_stop_id)
      return refuse(MotionRetryIneligibilityReason::kNotRetryableState);
    if (!std::all_of(assessments.begin(), assessments.end(), [](const auto& row) {
          return row.retry_eligibility.eligible &&
                 row.retry_eligibility.cause == MotionRetryCause::kPlanningNoEffect &&
                 row.retry_eligibility.effect == MotionRetryEffect::kKnownNoEffect;
        })) {
      const auto ineligible =
          std::find_if(assessments.begin(), assessments.end(),
                       [](const auto& row) { return !row.retry_eligibility.eligible; });
      return ineligible == assessments.end() ? MotionRetryEligibilityDTO{}
                                             : ineligible->retry_eligibility;
    }
    return {true, MotionRetryCause::kPlanningNoEffect, MotionRetryEffect::kKnownNoEffect,
            MotionRetryIneligibilityReason::kNone};
  }
  for (const auto& row : operation.participants) {
    if (!row.runtime_collision_error) continue;
    if (!conclusive_runtime_collision(*row.runtime_collision_error))
      return refuse(MotionRetryIneligibilityReason::kCollisionNotConclusive);
  }
  if (!operation.settled_stop || !operation.settled_stop->certificate ||
      !operation.terminal_stop_result())
    return refuse(MotionRetryIneligibilityReason::kStopNotSettled);
  const auto& stop = *operation.settled_stop;
  if (stop.result != StopCertificateResultDTO::kSucceededNominal)
    return refuse(MotionRetryIneligibilityReason::kStopNotNominal,
                  MotionRetryEffect::kEffectObserved);
  if (!stop.requested_controlled_stop_succeeded)
    return refuse(MotionRetryIneligibilityReason::kStopNotControlled,
                  MotionRetryEffect::kEffectObserved);
  for (const auto& row : assessments) {
    if (row.stop_owner_active) return refuse(MotionRetryIneligibilityReason::kStopOwnerActive);
    if (row.stronger_authority_active)
      return refuse(MotionRetryIneligibilityReason::kStrongerAuthorityActive);
    if (row.queued_latch_present)
      return refuse(MotionRetryIneligibilityReason::kQueuedLatchPresent);
    if (!row.latch_id.empty()) return refuse(MotionRetryIneligibilityReason::kLatchPresent);
    if (row.execution_state != ExecutionState::kStopped ||
        row.recovery_class != RecoveryClass::kOrdinaryOperatorStop || !row.retry_admissible ||
        !row.backend_stop_confirmed || !row.observed_state_available || !row.observed_state_fresh ||
        !row.observed_state_at_rest || row.observed_state_valid_until_ns <= clock_.now() ||
        row.stop_certificate_id != stop.certificate->stop_certificate_id.value() ||
        row.stop_certificate_revision != stop.certificate->stop_certificate_revision.value() ||
        row.stop_owner_generation == 0U)
      return refuse(MotionRetryIneligibilityReason::kObservationNotQualified,
                    MotionRetryEffect::kEffectObserved);
  }
  return {true, MotionRetryCause::kCollisionStopNominal, MotionRetryEffect::kEffectObserved,
          MotionRetryIneligibilityReason::kNone};
}

bool ResourceAddressedExecutionKernel::operation_close_eligible_locked(
    const ActiveOperation& operation, const std::vector<KernelRecoveryAssessmentDTO>& assessments,
    const bool closure_owner) const {
  if (is_active_stop_fault_latch(active_stop_fault_latch_) || operation.participants.empty() ||
      operation.participants.size() != operation.state.participants.size() ||
      operation.participants.size() != assessments.size() ||
      (!closure_owner &&
       (operation.recovery_effect_in_flight || operation.ordinary_commit_in_flight)) ||
      operation.stop_effect_in_flight || operation.ordinary_acceptance_pending ||
      operation.pending_control || operation.runtime_scale_effect_in_flight ||
      operation.coordinated_cancellation_in_flight || operation.dispatch_effect_in_flight() ||
      operation.reconciliation_in_flight() || operation.takeover_advance_in_flight ||
      !operation.pending_motion_takeover.empty() || !operation.next_motion_takeover.empty() ||
      operation.takeover_source || !operation.stop_allows_retirement())
    return false;
  const bool no_effect = operation.never_dispatched_and_quiescent() ||
                         (closure_owner && !operation.coordinated_dispatch_barrier_released &&
                          !operation.state.active_stop_id &&
                          std::all_of(operation.participants.begin(), operation.participants.end(),
                                      [](const auto& row) {
                                        return !row.prepared_dispatch_released &&
                                               row.slot_state == ResourceSlotState::kTerminal;
                                      }));
  const bool terminal_state =
      operation.state.state == OperationStateKind::kCompleted ||
      operation.state.state == OperationStateKind::kFailed ||
      operation.state.state == OperationStateKind::kStopped ||
      operation.state.state == OperationStateKind::kCancelledPreDispatch ||
      (operation.state.state == OperationStateKind::kInterventionRequired && no_effect);
  if (!terminal_state) return false;
  for (std::size_t index = 0; index < operation.participants.size(); ++index) {
    const auto& row = operation.participants[index];
    const auto& state = operation.state.participants[index];
    const auto& assessment = assessments[index];
    const auto* associated_fact = selected_cause_terminal_fact(state);
    if (!terminal(state.state) || row.preparation_borrow_in_flight() ||
        row.deferred_stop_settlement || assessment.stop_owner_active ||
        assessment.stronger_authority_active || assessment.queued_latch_present ||
        !assessment.latch_id.empty() ||
        (associated_fact &&
         associated_fact->effect == ParticipantEffectKnowledge::kEffectUncertain))
      return false;
    if (row.ownership_transferred) {
      if (!associated_fact) return false;
      const auto& fact = *associated_fact;
      const bool moving = fact.kind == ParticipantExecutionFactKind::kPreemptedByReplacement &&
                          fact.effect == ParticipantEffectKnowledge::kEffectSettled;
      const bool effect_prevented =
          fact.effect == ParticipantEffectKnowledge::kNoEffect &&
          (fact.kind == ParticipantExecutionFactKind::kCancelledBeforeDispatch ||
           fact.kind == ParticipantExecutionFactKind::kLocalFailure);
      const bool stopped =
          fact.kind == ParticipantExecutionFactKind::kPreemptedByAddressedStop &&
          fact.effect == ParticipantEffectKnowledge::kEffectSettled && operation.settled_stop &&
          operation.settled_stop->certificate &&
          operation.state.active_stop_id == operation.settled_stop->stop_id &&
          operation.state.active_stop_generation == operation.settled_stop->generation;
      // The exact owner CAS already detached this private row after moving
      // recognition, Stop settlement or local NoEffect cleanup. Preserve its
      // actual fact rather than manufacturing a moving-replacement outcome.
      if ((!moving && !effect_prevented && !stopped) || !row.resources.empty() || row.engine ||
          row.prepared_motion_engine || row.stop_execution_port || row.subordinate_id != 0U ||
          row.preparation_id != 0U || row.slot_state != ResourceSlotState::kTerminal)
        return false;
      continue;
    }
    // A Prepare refusal before an identity exists has no subordinate to retire.
    if (no_effect && row.preparation_id == 0U && row.subordinate_id == 0U) continue;
    if (!assessment.eligible_close) return false;
    if (operation.state.active_stop_id &&
        (!operation.settled_stop || !operation.settled_stop->certificate ||
         assessment.stop_certificate_id !=
             operation.settled_stop->certificate->stop_certificate_id.value() ||
         assessment.stop_certificate_revision !=
             operation.settled_stop->certificate->stop_certificate_revision.value()))
      return false;
  }
  return true;
}

Result<OperationRecoveryProjectionDTO>
ResourceAddressedExecutionKernel::get_operation_recovery_assessment(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<OperationRecoveryProjectionDTO>("operation identity is empty",
                                                  "operation_id_empty", ErrorCode::kInvalidRequest);
  }
  std::vector<IResolvedParticipantExecutionEngine*> engines;
  OperationRevision revision(0U);
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  std::vector<OperationRecoveryParticipantDTO> participants;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr || operation->participants.empty() ||
        operation->stop_effect_in_flight ||
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& participant) { return participant.engine == nullptr && !participant.ownership_transferred; })) {
      return reject<OperationRecoveryProjectionDTO>("operation has no resident recovery owner",
                                                    "operation_recovery_unavailable",
                                                    ErrorCode::kInvalidStateTransition);
    }
    engines.reserve(operation->participants.size());
    revision = operation->state.operation_revision;
    stop_id = operation->state.active_stop_id;
    stop_generation = operation->state.active_stop_generation;
    participants.reserve(operation->participants.size());
    for (const auto& participant : operation->participants) {
      engines.push_back(participant.engine);
      const auto& binding = participant.binding.value();
      std::optional<CapabilityProviderBindingId> provider;
      if (binding.capability_provider_path.has_value()) {
        provider = binding.capability_provider_path->capability_provider_binding_id;
      }
      participants.push_back(OperationRecoveryParticipantDTO{
          participant.slot_owner.participant_id, binding.controllable_component_id,
          binding.command_target_id, binding.capability_id,
          participant.slot_owner.binding_identity.control_domain_id,
          participant.slot_owner.binding_identity.controller()
              ? std::optional(participant.slot_owner.binding_identity.controller()->control_binding_id) : std::nullopt,
          std::move(provider),
          participant.slot_owner.controller_authority
              ? std::optional(participant.slot_owner.controller().control_route_id) : std::nullopt,
          participant.slot_owner.controller_authority
              ? std::optional(participant.slot_owner.controller().active_control_configuration_id) : std::nullopt,
          participant.slot_owner.controller_authority
              ? std::optional(participant.slot_owner.controller().active_control_configuration_generation) : std::nullopt,
          participant.resources});
    }
  }
  std::vector<KernelRecoveryAssessmentDTO> assessments;
  assessments.reserve(engines.size());
  try {
    for (auto* engine : engines) {
      if (engine) {
        assessments.push_back(engine->get_recovery_assessment());
      } else {
        KernelRecoveryAssessmentDTO transferred;
        transferred.assessment_revision = 1U;
        transferred.execution_state = ExecutionState::kStopped;
        transferred.eligible_close = true;
        transferred.reason_code = "physical_ownership_transferred";
        assessments.push_back(std::move(transferred));
      }
    }
  } catch (...) {
    return reject<OperationRecoveryProjectionDTO>("participant recovery assessment is unavailable",
                                                  "operation_recovery_unavailable",
                                                  ErrorCode::kInternalError);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr || operation->state.operation_revision != revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || operation->participants.size() != engines.size() ||
        !std::equal(operation->participants.begin(), operation->participants.end(), engines.begin(),
                    [](const auto& participant, const auto* engine) {
                      return participant.engine == engine;
                    })) {
      return reject<OperationRecoveryProjectionDTO>(
          "operation changed while its recovery assessment was read", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
    KernelRecoveryAssessmentDTO assessment;
    if (assessments.size() == 1U) {
      assessment = assessments.front();
    } else {
      assessment.schema_version = "manyforge.kernel_recovery_assessment.v1";
      assessment.assessment_revision = operation->recovery_assessment_revision;
      assessment.assessed_at_ns =
          std::max_element(assessments.begin(), assessments.end(),
                           [](const auto& left, const auto& right) {
                             return left.assessed_at_ns < right.assessed_at_ns;
                           })
              ->assessed_at_ns;
      assessment.execution_state = assessments.front().execution_state;
      assessment.recovery_class = assessments.front().recovery_class;
      if (std::any_of(assessments.begin(), assessments.end(), [&](const auto& row) {
            return row.execution_state != assessment.execution_state;
          }))
        assessment.execution_state = ExecutionState::kInterrupted;
      if (std::any_of(assessments.begin(), assessments.end(), [&](const auto& row) {
            return row.recovery_class != assessment.recovery_class;
          }))
        assessment.recovery_class = RecoveryClass::kUnknownFault;
      assessment.stop_owner_active =
          std::any_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.stop_owner_active; });
      assessment.backend_stop_confirmed =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.backend_stop_confirmed; });
      assessment.observed_state_available =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_available; });
      assessment.observed_state_fresh =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_fresh; });
      assessment.observed_state_at_rest =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_at_rest; });
      assessment.observed_state_measured =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_measured; });
      assessment.stronger_authority_active =
          std::any_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.stronger_authority_active; });
      assessment.queued_latch_present =
          std::any_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.queued_latch_present; });
      assessment.allowed_actions = assessments.front().allowed_actions;
      std::erase_if(assessment.allowed_actions, [&](const auto action) {
        return std::any_of(assessments.begin() + 1, assessments.end(), [&](const auto& row) {
          return std::find(row.allowed_actions.begin(), row.allowed_actions.end(), action) ==
                 row.allowed_actions.end();
        });
      });
      assessment.retry_admissible =
          !assessment.allowed_actions.empty() &&
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.retry_admissible; });
      if (assessment.retry_admissible) {
        assessment.status = RecoveryAssessmentStatus::kRetryReady;
        assessment.reason_code = "coordinated_operation_recovery_ready";
      } else if (std::any_of(assessments.begin(), assessments.end(), [](const auto& row) {
                   return row.status == RecoveryAssessmentStatus::kResetRequired;
                 })) {
        assessment.status = RecoveryAssessmentStatus::kResetRequired;
        assessment.reason_code = "coordinated_operation_reset_required";
      } else {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = "coordinated_operation_recovery_waiting";
      }
    }
    assessment.retry_eligibility = operation_retry_eligibility_locked(*operation, assessments);
    assessment.eligible_close = operation_close_eligible_locked(*operation, assessments);
    if (std::any_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& row) { return row.runtime_collision_error.has_value(); }))
      assessment.recovery_class = RecoveryClass::kRuntimeCollision;
    if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
      const auto& latch = *active_stop_fault_latch_;
      // A participant may be physically stopped and locally retry-ready while
      // the addressed cell owner has retained a stronger Stop-fault latch.
      // Publish the composed truth: motion is settled, but only Program Reset
      // may clear the exact outer latch.  Never offer Resume that the same
      // owner would subsequently reject at motion admission.
      assessment.recovery_class = RecoveryClass::kStopFaultIntervention;
      assessment.status = RecoveryAssessmentStatus::kResetRequired;
      assessment.latch_id = latch.latch_id.value();
      assessment.stop_owner_generation = latch.stop_generation.value();
      assessment.retry_admissible = false;
      assessment.allowed_actions = {RecoveryAction::kResetCycle};
      assessment.reason_code = "active_stop_fault_requires_reset";
      if (latch.stop_certificate.has_value()) {
        assessment.stop_certificate_id = latch.stop_certificate->stop_certificate_id.value();
        assessment.stop_certificate_revision =
            latch.stop_certificate->stop_certificate_revision.value();
      }
    }
    const auto& retained = operation->recovery_assessment_inputs;
    bool semantic_change =
        !operation->published_recovery_assessment || retained.size() != assessments.size();
    for (std::size_t index = 0; !semantic_change && index < assessments.size(); ++index)
      semantic_change =
          retained[index].assessment_revision != assessments[index].assessment_revision ||
          recovery_assessment_semantically_differs(retained[index], assessments[index]);
    if (!semantic_change)
      semantic_change = recovery_assessment_semantically_differs(
          *operation->published_recovery_assessment, assessment);
    if (semantic_change) {
      if (operation->recovery_assessment_revision >= kMaximumKernelJsonInteger)
        return reject<OperationRecoveryProjectionDTO>("recovery assessment revision is exhausted",
                                                      "operation_recovery_unavailable",
                                                      ErrorCode::kInternalError);
      operation->recovery_assessment_revision =
          !operation->published_recovery_assessment && assessments.size() == 1U
              ? assessments.front().assessment_revision
              : operation->recovery_assessment_revision + 1U;
    }
    assessment.assessment_revision = operation->recovery_assessment_revision;
    operation->recovery_assessment_inputs = assessments;
    operation->published_recovery_assessment = assessment;
    return Result<OperationRecoveryProjectionDTO>::Ok(
        OperationRecoveryProjectionDTO{revision, std::move(stop_id), std::move(stop_generation),
                                       std::move(assessment), std::move(participants)});
  }
  return reject<OperationRecoveryProjectionDTO>("operation recovery assessment changed",
                                                "operation_recovery_stale",
                                                ErrorCode::kStaleGeneration);
}

Result<KernelRecoveryActionResultDTO>
ResourceAddressedExecutionKernel::apply_operation_recovery_action(
    const OperationId& operation_id, const OperationRevision expected_operation_revision,
    const KernelRecoveryActionRequestDTO& request) {
  if (request.action == RecoveryAction::kCloseOperation)
    return close_operation(operation_id, expected_operation_revision, request);
  bool coordinated_recovery = false;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (operation != nullptr && operation->participants.size() > 1U) {
      if (operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll) {
        return reject<KernelRecoveryActionResultDTO>(
            "multi-participant recovery requires one coordinated operation",
            "operation_recovery_invalid", ErrorCode::kInvalidRequest);
      }
      coordinated_recovery = true;
    }
  }
  // The helper takes the same operation-level recovery mutex and performs all
  // effect calls outside Kernel state authority.
  if (coordinated_recovery)
    return apply_coordinated_operation_recovery_action(operation_id, expected_operation_revision,
                                                       request);
  const auto retain_current_recovery = [&](OperationRecoveryReplayEntry entry) {
    std::erase_if(operation_recovery_replay_ledger_,
                  [&](const OperationRecoveryReplayEntry& current) {
                    return current.operation_id == entry.operation_id;
                  });
    operation_recovery_replay_ledger_.push_back(std::move(entry));
  };
  if (operation_id.empty() || expected_operation_revision.value() == 0U ||
      request.request_id.empty()) {
    return reject<KernelRecoveryActionResultDTO>("operation recovery request is incomplete",
                                                 "operation_recovery_invalid",
                                                 ErrorCode::kInvalidRequest);
  }
  std::lock_guard recovery_lock(operation_recovery_mutex_);
  // Replay precedes observation: accepted retirement removes the resident row.
  if (request.fresh_motion_attempt) {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        operation_recovery_replay_ledger_.begin(), operation_recovery_replay_ledger_.end(),
        [&](const auto& row) { return row.request.request_id == request.request_id; });
    if (replay != operation_recovery_replay_ledger_.end()) {
      if (replay->operation_id != operation_id ||
          replay->operation_revision != expected_operation_revision ||
          !recovery_requests_match(replay->request, request))
        return reject<KernelRecoveryActionResultDTO>(
            "operation recovery request identity was reused", "request_id_conflict",
            ErrorCode::kInvalidRequest);
      if (replay->outcome_unresolved)
        return reject<KernelRecoveryActionResultDTO>(
            "participant recovery outcome remains unresolved",
            "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
      return Result<KernelRecoveryActionResultDTO>::Ok(replay->result);
    }
  }
  std::optional<OperationRecoveryProjectionDTO> fresh_projection;
  if (request.fresh_motion_attempt) {
    auto projection = get_operation_recovery_assessment(operation_id);
    if (!projection.ok()) return Result<KernelRecoveryActionResultDTO>::Err(projection.error());
    fresh_projection = std::move(projection.value());
  }
  auto participant_request = request;
  std::optional<std::vector<std::size_t>> fresh_node_indices;
  IResolvedParticipantExecutionEngine* engine = nullptr;
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  std::optional<ProgramRunExecutionCaptureDTO> continuation_capture;
  std::size_t continuation_index{};
  std::shared_ptr<ProgramCycleClaimRun::MotionContinuation> continuation;
  IResolvedPreparedParticipantExecutionEngine* prepared_engine = nullptr;
  std::uint64_t preparation_id{};
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(operation_recovery_replay_ledger_.begin(),
                                     operation_recovery_replay_ledger_.end(),
                                     [&](const OperationRecoveryReplayEntry& entry) {
                                       return entry.request.request_id == request.request_id;
                                     });
    if (replay != operation_recovery_replay_ledger_.end()) {
      if (replay->operation_id != operation_id ||
          replay->operation_revision != expected_operation_revision ||
          !recovery_requests_match(replay->request, request)) {
        return reject<KernelRecoveryActionResultDTO>(
            "operation recovery request identity was reused", "request_id_conflict",
            ErrorCode::kInvalidRequest);
      }
      if (replay->outcome_unresolved) {
        return reject<KernelRecoveryActionResultDTO>(
            "participant recovery outcome remains unresolved",
            "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
      }
      return Result<KernelRecoveryActionResultDTO>::Ok(replay->result);
    }
    if (request.fresh_motion_attempt &&
        std::any_of(operation_recovery_replay_ledger_.begin(),
                    operation_recovery_replay_ledger_.end(), [&](const auto& row) {
                      return row.operation_id == operation_id && row.outcome_unresolved;
                    }))
      return reject<KernelRecoveryActionResultDTO>(
          "operation recovery publication remains unresolved",
          "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->participants.size() != 1U || operation->participants.front().engine == nullptr ||
        operation->stop_effect_in_flight) {
      return reject<KernelRecoveryActionResultDTO>(
          "operation recovery fence is stale or unsupported", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
    if (operation->has_pending_io_rule_response() || operation->takeover_advance_in_flight ||
        !operation->pending_motion_takeover.empty() ||
        !operation->next_motion_takeover.empty() || operation->ordinary_commit_in_flight ||
        operation->ordinary_acceptance_pending || operation->pending_control.has_value() ||
        operation->runtime_scale_effect_in_flight || operation->recovery_effect_in_flight ||
        operation->coordinated_cancellation_in_flight || operation->dispatch_effect_in_flight() ||
        operation->reconciliation_in_flight() ||
        std::any_of(
            operation->participants.begin(), operation->participants.end(),
            [](const auto& participant) { return participant.preparation_borrow_in_flight(); }))
      return reject<KernelRecoveryActionResultDTO>("operation work is still in flight",
                                                   "operation_recovery_busy",
                                                   ErrorCode::kInvalidStateTransition);
    if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
      return Result<KernelRecoveryActionResultDTO>::Err(
          ErrorDTO{"operation_recovery_blocked_by_stop_fault",
                   "Program Reset is required before motion can resume",
                   {{"reason", "active_stop_fault_requires_reset"},
                    {"latch_id", active_stop_fault_latch_->latch_id.value()}}});
    }
    if (motion_producing_recovery(request.action) && !operation->manual) {
      if (auto refusal = operator_alarm_attention_refusal_locked(
              operation_resources(operation->participants))) {
        return Result<KernelRecoveryActionResultDTO>::Err(std::move(*refusal));
      }
    }
    if (!operation->stop_allows_retirement()) {
      return reject<KernelRecoveryActionResultDTO>(
          "operation recovery awaits its exact successful Stop certificate",
          "operation_recovery_busy", ErrorCode::kInvalidStateTransition);
    }
    if (request.fresh_motion_attempt) {
      fresh_node_indices = fresh_motion_node_indices_locked(*operation);
      const auto& assessment = fresh_projection->assessment;
      if (fresh_projection->operation_revision != expected_operation_revision ||
          !recovery_request_names_assessment(request, assessment) ||
          !assessment.retry_eligibility.eligible || !assessment.retry_admissible ||
          !motion_producing_recovery(request.action) ||
          std::find(assessment.allowed_actions.begin(), assessment.allowed_actions.end(),
                    request.action) == assessment.allowed_actions.end() ||
          !fresh_node_indices)
        return reject<KernelRecoveryActionResultDTO>(
            "fresh motion recovery is stale or not eligible", "operation_recovery_invalid",
            ErrorCode::kInvalidStateTransition);
      continuation_capture = *program_cycle_claim_run_->capture;
    }
    // The published addressed revision may include owner-only Stop facts.
    // A stale outer revision can equal the current local revision; it must
    // never fall through to the local owner as if it named that assessment.
    if (operation->published_recovery_assessment &&
        !recovery_request_names_assessment(request, *operation->published_recovery_assessment))
      return reject<KernelRecoveryActionResultDTO>("operation recovery assessment is stale",
                                                   "operation_recovery_stale",
                                                   ErrorCode::kStaleGeneration);
    // Translate only an exact addressed request to the local identity.
    if (operation->published_recovery_assessment &&
        recovery_request_names_assessment(request, *operation->published_recovery_assessment) &&
        operation->recovery_assessment_inputs.size() == 1U)
      bind_recovery_request(participant_request, operation->recovery_assessment_inputs.front());
    engine = operation->participants.front().engine;
    stop_id = operation->state.active_stop_id;
    stop_generation = operation->state.active_stop_generation;
    const auto& participant = operation->participants.front();
    const auto& public_participant = operation->state.participants.front();
    if (!request.fresh_motion_attempt &&
        (request.action == RecoveryAction::kRetryMotionFromCurrentState ||
         request.action == RecoveryAction::kReplanFromStoppedState ||
         request.action == RecoveryAction::kAcknowledgeAndContinue ||
         request.action == RecoveryAction::kRetryAfterSceneRepair) &&
        participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
        participant.prepared_dispatch_released && participant.prepared_participant &&
        public_participant.execution_scope && program_cycle_claim_run_ &&
        program_cycle_claim_run_->program_run_id == operation->state.program_run_id &&
        program_cycle_claim_run_->capture) {
      const auto& capture = *program_cycle_claim_run_->capture;
      const auto& scope = *public_participant.execution_scope;
      const auto node = std::lower_bound(
          capture.table->nodes.begin(), capture.table->nodes.end(), scope.node_instance_id,
          [](const auto& row, const auto& id) { return row.instance_id < id; });
      // An internal pick/place phase has the same motion payload shape, but
      // its continuation belongs to the existing sequence owner, not this slot.
      if (node != capture.table->nodes.end() && node->instance_id == scope.node_instance_id &&
          node->owner_api == "kernel.operation") {
        continuation_capture = capture;
        continuation_index = static_cast<std::size_t>(node - capture.table->nodes.begin());
        const auto& source =
            std::get<PreparedMotionExecutionDTO>(participant.prepared_participant->payload)
                .plan_request;
        continuation = std::make_shared<ProgramCycleClaimRun::MotionContinuation>(
            ProgramCycleClaimRun::MotionContinuation{
                scope,
                operation_id,
                public_participant.participant_id,
                participant.prepared_participant->prepared_contract_digest,
                participant.binding,
                {},
                source.base_frame,
                source.tcp_frame,
                source.planner_capability_binding});
        prepared_engine = participant.prepared_motion_engine;
        preparation_id = participant.subordinate_id;
        continuation->authored_motion_kind =
            std::get<MotionParticipantIntentDTO>(operation->intent.participants.front().payload)
                .motion.index();
      }
    }
    operation->recovery_effect_in_flight = true;
  }

  const ScopeExit release_recovery_borrow([&] {
    {
      std::lock_guard state_lock(state_mutex_);
      if (auto* operation = find_active_operation_locked(operation_id))
        operation->recovery_effect_in_flight = false;
    }
  });
  // Copy the semantic destination before recovery clears the subordinate.
  // This observer never grants rest: the existing exact Stop certificate is
  // checked both above and at publication below. Stop can overtake this read.
  const auto capture_rejected = [](ErrorDTO error) {
    error.details["continuation_error_code"] = error.code;
    error.code = "operation_recovery_invalid";
    return Result<KernelRecoveryActionResultDTO>::Err(std::move(error));
  };
  try {
    if (continuation) {
      if (!prepared_engine)
        return capture_rejected(error("motion_continuation_unavailable",
                                      "motion continuation has no prepared owner",
                                      "motion_continuation_unavailable"));
      auto snapshot = prepared_engine->ObservePreparedMotion(preparation_id);
      if (!snapshot.ok()) return capture_rejected(snapshot.error());
      const auto& observed = snapshot.value();
      if (observed.execution_id != preparation_id)
        return capture_rejected(error("motion_continuation_identity_mismatch",
                                      "motion continuation changed preparation identity",
                                      "motion_continuation_identity_mismatch"));
      const auto& source = observed.last_dispatched_participant;
      if (!source &&
          (observed.state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch ||
           observed.state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed)) {
        // Only a never-dispatched attempt can leave the prior slot unchanged.
        // A dispatched attempt retains its actual prior dispatch below.
        continuation.reset();
      } else if (!source) {
        return capture_rejected(error("motion_continuation_dispatch_unsettled",
                                      "motion dispatch outcome is not settled",
                                      "motion_continuation_dispatch_unsettled"));
      } else {
        if (source->prepared_contract_digest != continuation->prepared_contract_digest ||
            source->operation_id != operation_id ||
            source->participant_id != continuation->participant_id ||
            !std::holds_alternative<PreparedMotionExecutionDTO>(source->payload))
          return capture_rejected(error("motion_continuation_identity_mismatch",
                                        "motion continuation differs from the retired participant",
                                        "motion_continuation_identity_mismatch"));
        const auto& motion = std::get<PreparedMotionExecutionDTO>(source->payload);
        continuation->base_frame = motion.plan_request.base_frame;
        continuation->tcp_frame = motion.plan_request.tcp_frame;
        continuation->planner_capability = motion.plan_request.planner_capability_binding;
        auto captured = detail::capture_motion_resume_intent(
            motion.plan_request.motion, motion.trajectory, observed.stopped_trajectory_time_s);
        if (!captured.ok()) return capture_rejected(captured.error());
        continuation->intent =
            std::make_shared<const detail::MotionResumeIntent>(std::move(captured.value()));
      }
    }
  } catch (...) {
    return capture_rejected(error("motion_continuation_capture_failed",
                                  "motion continuation could not be captured before recovery",
                                  "motion_continuation_capture_failed"));
  }
  if (request.fresh_motion_attempt) {
    std::vector<KernelRecoveryAssessmentDTO> current;
    try {
      current.push_back(engine->get_recovery_assessment());
    } catch (...) {
      return reject<KernelRecoveryActionResultDTO>("participant recovery assessment is unavailable",
                                                   "operation_recovery_unavailable",
                                                   ErrorCode::kInternalError);
    }
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (!operation || !operation_retry_eligibility_locked(*operation, current, true).eligible ||
        !fresh_motion_node_indices_locked(*operation) ||
        !recovery_request_names_assessment(participant_request, current.front()))
      return reject<KernelRecoveryActionResultDTO>(
          "fresh recovery eligibility changed before its effect", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (!operation || operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || !operation->stop_allows_retirement() ||
        operation->participants.size() != 1U || operation->participants.front().engine != engine ||
        (continuation_capture &&
         (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
          program_cycle_claim_run_->capture->table != continuation_capture->table ||
          program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
          program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id)))
      return Result<KernelRecoveryActionResultDTO>::Err(
          error("operation_recovery_stale", "recovery was overtaken before its effect",
                "operation_recovery_stale"));
  }

  KernelRecoveryActionResultDTO result;
  try {
    result = engine->apply_recovery_action(participant_request);
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id))
      operation->recovery_effect_in_flight = false;
    motion_admission_closed_ = true;
    retain_current_recovery(OperationRecoveryReplayEntry{
        operation_id, expected_operation_revision, request,
        KernelRecoveryActionResultDTO{
            request.request_id,
            request.action,
            false,
            {},
            ErrorDTO{"operation_recovery_outcome_unresolved",
                     "participant recovery outcome is unknown",
                     {{"reason", "operation_recovery_outcome_unresolved"}}}},
        true});
    return reject<KernelRecoveryActionResultDTO>("participant recovery outcome is unknown",
                                                 "operation_recovery_outcome_unresolved",
                                                 ErrorCode::kInvariantViolation);
  }
  std::unique_lock retirement_submission(submission_mutex_, std::defer_lock);
  if (result.accepted) retirement_submission.lock();
  std::optional<OperationCommitGuard> retirement_commit;
  if (result.accepted) retirement_commit.emplace(*this, operation_id);
  {
    std::unique_lock state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation != nullptr) operation->recovery_effect_in_flight = false;
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || operation->takeover_advance_in_flight ||
        !operation->pending_motion_takeover.empty() || !operation->next_motion_takeover.empty() ||
        operation->participants.size() != 1U || operation->participants.front().engine != engine ||
        !operation->stop_allows_retirement() || result.request_id != request.request_id ||
        result.action != request.action) {
      motion_admission_closed_ = true;
      retain_current_recovery(OperationRecoveryReplayEntry{
          operation_id, expected_operation_revision, request, result, true});
      return reject<KernelRecoveryActionResultDTO>(
          "operation or participant recovery result changed during the effect",
          "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
    }
    if (!result.accepted) {
      retain_current_recovery(
          OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
      return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
    }
    if ((continuation || request.fresh_motion_attempt) &&
        (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
         program_cycle_claim_run_->capture->table != continuation_capture->table ||
         program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
         program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id ||
         (request.fresh_motion_attempt &&
          (is_active_stop_fault_latch(active_stop_fault_latch_) || operation->retry_run_ended)) ||
         continuation_index >= program_cycle_claim_run_->motion_continuations.size())) {
      motion_admission_closed_ = true;
      retain_current_recovery(OperationRecoveryReplayEntry{
          operation_id, expected_operation_revision, request, result, true});
      return reject<KernelRecoveryActionResultDTO>(
          "Program capture changed during motion retirement",
          "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
    }
    auto owner_rows =
        retained_owner_rows(operation->participants, resource_slots_->coherent_snapshot());
    if (!owner_rows.ok()) {
      motion_admission_closed_ = true;
      retain_current_recovery(OperationRecoveryReplayEntry{
          operation_id, expected_operation_revision, request, result, true});
      return Result<KernelRecoveryActionResultDTO>::Err(owner_rows.error());
    }
    if (!owner_rows.value().empty()) {
      auto released = resource_slots_->release_batch(owner_rows.value());
      if (!released.ok()) {
        motion_admission_closed_ = true;
        retain_current_recovery(OperationRecoveryReplayEntry{
            operation_id, expected_operation_revision, request, result, true});
        return Result<KernelRecoveryActionResultDTO>::Err(released.error());
      }
    }
    retain_current_recovery(
        OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
    if (fresh_node_indices)
      for (const auto index : *fresh_node_indices)
        program_cycle_claim_run_->motion_continuations[index].reset();
    if (continuation)
      program_cycle_claim_run_->motion_continuations[continuation_index] = std::move(continuation);
    // The accepted retirement must also end live ownership:
    // emergency Stop bypasses submission ordering and must see the existing
    // historical, nonresident representation once the slots are released.
    retain_historical_operation_locked(operation->state);
    active_operations_.erase(operation_id);
  }
  return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
}

Result<void> ResourceAddressedExecutionKernel::retire_preempted_motion_operations() {
  std::vector<OperationId> sources;
  {
    std::lock_guard state_lock(state_mutex_);
    for (const auto& [id, operation] : active_operations_) {
      if (!operation->manual && operation->state.preemption &&
          operation->state.state == OperationStateKind::kStopped &&
          operation->participants.size() == 1U &&
          operation->participants.front().payload_kind == OperationParticipantPayloadKind::kMotion)
        sources.push_back(id);
    }
  }
  for (const auto& id : sources) {
    // Logical preemption already ended the Runner task. The existing closure
    // owner alone decides when its remaining physical/local work can retire.
    const auto projection = get_operation_recovery_assessment(id);
    if (!projection.ok() || !projection.value().assessment.eligible_close) continue;
    const auto& assessment = projection.value().assessment;
    KernelRecoveryActionRequestDTO request;
    request.request_id = content_identity::sha256_digest(
        config_.request_owner_epoch.value() + "\npreempted-motion-close\n" + id.value() + "\n" +
        std::to_string(projection.value().operation_revision.value()) + "\n" +
        std::to_string(assessment.assessment_revision));
    request.action = RecoveryAction::kCloseOperation;
    request.principal_id = "kernel.motion_takeover";
    request.reason = "retire_preempted_motion";
    bind_recovery_request(request, assessment);
    const auto closed = close_operation(id, projection.value().operation_revision, request);
    if (!closed.ok()) {
      // A newer Stop or another exact cleanup can win between the read and
      // closure. Its ownership remains authoritative; never bypass it.
      const auto reason = closed.error().details.find("reason");
      if (reason != closed.error().details.end() &&
          reason->second == "operation_recovery_stale")
        continue;
      return Result<void>::Err(closed.error());
    }
  }
  return Result<void>::Ok();
}

Result<KernelRecoveryActionResultDTO> ResourceAddressedExecutionKernel::close_operation(
    const OperationId& operation_id, const OperationRevision expected_operation_revision,
    const KernelRecoveryActionRequestDTO& request) {
  if (operation_id.empty() || expected_operation_revision.value() == 0U ||
      request.request_id.empty() || request.principal_id.empty())
    return reject<KernelRecoveryActionResultDTO>("operation closure request is invalid",
                                                 "operation_recovery_invalid",
                                                 ErrorCode::kInvalidRequest);
  std::lock_guard recovery_lock(operation_recovery_mutex_);
  const auto retain = [&](OperationRecoveryReplayEntry entry) {
    std::erase_if(operation_recovery_replay_ledger_,
                  [&](const auto& row) { return row.operation_id == entry.operation_id; });
    operation_recovery_replay_ledger_.push_back(std::move(entry));
  };
  {
    std::lock_guard state_lock(state_mutex_);
    for (const auto& entry : operation_recovery_replay_ledger_) {
      if (entry.request.request_id != request.request_id) continue;
      if (entry.operation_id != operation_id ||
          entry.operation_revision != expected_operation_revision ||
          !recovery_requests_match(entry.request, request))
        return reject<KernelRecoveryActionResultDTO>(
            "operation recovery request identity was reused", "request_id_conflict",
            ErrorCode::kInvalidRequest);
      if (entry.outcome_unresolved)
        return reject<KernelRecoveryActionResultDTO>("operation closure remains unresolved",
                                                     "operation_recovery_outcome_unresolved",
                                                     ErrorCode::kInvariantViolation);
      return Result<KernelRecoveryActionResultDTO>::Ok(entry.result);
    }
    if (std::any_of(operation_recovery_replay_ledger_.begin(),
                    operation_recovery_replay_ledger_.end(), [&](const auto& row) {
                      return row.operation_id == operation_id && row.outcome_unresolved;
                    }))
      return reject<KernelRecoveryActionResultDTO>("operation closure remains unresolved",
                                                   "operation_recovery_outcome_unresolved",
                                                   ErrorCode::kInvariantViolation);
  }
  if (request.fresh_motion_attempt)
    return reject<KernelRecoveryActionResultDTO>("closure cannot authorize a fresh motion attempt",
                                                 "operation_recovery_invalid",
                                                 ErrorCode::kInvalidRequest);
  auto projection = get_operation_recovery_assessment(operation_id);
  if (!projection.ok()) return Result<KernelRecoveryActionResultDTO>::Err(projection.error());
  const auto assessment = projection.value().assessment;
  struct Target {
    IResolvedParticipantExecutionEngine* engine;
    KernelRecoveryActionRequestDTO request;
  };
  std::vector<Target> targets;
  std::vector<KernelRecoveryAssessmentDTO> closure_assessments;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (!operation || operation->ordinary_commit_in_flight ||
        operation->recovery_effect_in_flight ||
        operation->state.operation_revision != expected_operation_revision ||
        projection.value().operation_revision != expected_operation_revision ||
        !recovery_request_names_assessment(request, assessment) || !assessment.eligible_close ||
        !operation->published_recovery_assessment ||
        !recovery_request_names_assessment(request, *operation->published_recovery_assessment) ||
        !operation_close_eligible_locked(*operation, operation->recovery_assessment_inputs, true))
      return reject<KernelRecoveryActionResultDTO>("operation closure is stale or not eligible",
                                                   "operation_recovery_stale",
                                                   ErrorCode::kStaleGeneration);
    for (std::size_t index = 0; index < operation->participants.size(); ++index) {
      const auto& row = operation->participants[index];
      if (row.ownership_transferred || (row.preparation_id == 0U && row.subordinate_id == 0U))
        continue;
      auto local_request = request;
      bind_recovery_request(local_request, operation->recovery_assessment_inputs[index]);
      targets.push_back({row.engine, std::move(local_request)});
    }
    closure_assessments = operation->recovery_assessment_inputs;
    operation->recovery_effect_in_flight = true;
  }
  const ScopeExit release_borrow([&] {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id))
      operation->recovery_effect_in_flight = false;
  });
  KernelRecoveryActionResultDTO result{request.request_id, request.action, true, {}, std::nullopt};
  const auto unresolved = [&](std::string message) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
    retain({operation_id, expected_operation_revision, request, result, true});
    return reject<KernelRecoveryActionResultDTO>(std::move(message),
                                                 "operation_recovery_outcome_unresolved",
                                                 ErrorCode::kInvariantViolation);
  };
  std::size_t consumed = 0U;
  try {
    for (auto& target : targets) {
      const auto local = target.engine->apply_recovery_action(target.request);
      if (local.request_id != request.request_id || local.action != request.action)
        return unresolved("participant closure identity changed");
      if (!local.accepted) {
        if (consumed != 0U) return unresolved("participant closure disagreed after cleanup began");
        result.accepted = false;
        result.resulting_assessment = assessment;
        result.error = local.error;
        std::lock_guard state_lock(state_mutex_);
        retain({operation_id, expected_operation_revision, request, result});
        return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
      }
      ++consumed;
    }
  } catch (...) {
    return unresolved("participant closure cleanup outcome is unknown");
  }
  // Local cleanup can emit participant callbacks. Acquire ordinary commit
  // serialization only after those calls return, in the existing lock order.
  // Emergency Stop bypasses submission ordering and is rechecked below.
  std::lock_guard submission_lock(submission_mutex_);
  OperationCommitGuard commit(*this, operation_id);
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (!operation || operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != projection.value().stop_id ||
        operation->state.active_stop_generation != projection.value().stop_generation ||
        !operation_close_eligible_locked(*operation, closure_assessments, true)) {
      motion_admission_closed_ = true;
      retain({operation_id, expected_operation_revision, request, result, true});
      return reject<KernelRecoveryActionResultDTO>("operation changed during closure cleanup",
                                                   "operation_recovery_outcome_unresolved",
                                                   ErrorCode::kInvariantViolation);
    }
    result.resulting_assessment.assessment_revision = assessment.assessment_revision;
    result.resulting_assessment.reason_code = "operation_closed";
    const auto retired = retire_operation_ownership_locked(operation_id, true, true);
    if (!retired.ok()) {
      motion_admission_closed_ = true;
      retain({operation_id, expected_operation_revision, request, result, true});
      return Result<KernelRecoveryActionResultDTO>::Err(retired.error());
    }
    // Accepted replay becomes visible only after resource and continuation release.
    retain({operation_id, expected_operation_revision, request, result});
  }
  return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
}

Result<KernelRecoveryActionResultDTO>
ResourceAddressedExecutionKernel::apply_coordinated_operation_recovery_action(
    const OperationId& operation_id, const OperationRevision expected_operation_revision,
    const KernelRecoveryActionRequestDTO& request) {
  const auto retain_current = [&](OperationRecoveryReplayEntry entry) {
    std::erase_if(operation_recovery_replay_ledger_,
                  [&](const OperationRecoveryReplayEntry& current) {
                    return current.operation_id == entry.operation_id;
                  });
    operation_recovery_replay_ledger_.push_back(std::move(entry));
  };
  if (operation_id.empty() || expected_operation_revision.value() == 0U ||
      request.request_id.empty()) {
    return reject<KernelRecoveryActionResultDTO>("operation recovery request is incomplete",
                                                 "operation_recovery_invalid",
                                                 ErrorCode::kInvalidRequest);
  }

  std::lock_guard recovery_lock(operation_recovery_mutex_);
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(operation_recovery_replay_ledger_.begin(),
                                     operation_recovery_replay_ledger_.end(),
                                     [&](const OperationRecoveryReplayEntry& entry) {
                                       return entry.request.request_id == request.request_id;
                                     });
    if (replay != operation_recovery_replay_ledger_.end()) {
      if (replay->operation_id != operation_id ||
          replay->operation_revision != expected_operation_revision ||
          !recovery_requests_match(replay->request, request)) {
        return reject<KernelRecoveryActionResultDTO>(
            "operation recovery request identity was reused", "request_id_conflict",
            ErrorCode::kInvalidRequest);
      }
      if (replay->outcome_unresolved) {
        return reject<KernelRecoveryActionResultDTO>(
            "coordinated recovery outcome remains unresolved",
            "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
      }
      return Result<KernelRecoveryActionResultDTO>::Ok(replay->result);
    }
  }

  auto projection = get_operation_recovery_assessment(operation_id);
  if (!projection.ok()) {
    return Result<KernelRecoveryActionResultDTO>::Err(projection.error());
  }
  const auto& assessment = projection.value().assessment;
  const bool exact_assessment =
      projection.value().operation_revision == expected_operation_revision &&
      request.expected_assessment_revision == assessment.assessment_revision &&
      request.expected_control_command_epoch == assessment.control_command_epoch &&
      request.expected_execution_id == assessment.execution_id &&
      request.expected_latch_id == assessment.latch_id &&
      request.expected_stop_certificate_id == assessment.stop_certificate_id &&
      request.expected_stop_owner_generation == assessment.stop_owner_generation;
  // Q8a admits no multi-motion operation; supported coordinated command
  // mixtures are outside plain-motion automatic retry. Manual recovery keeps
  // the existing action intersection and participant dispatch unchanged.
  const bool action_allowed =
      !request.fresh_motion_attempt && assessment.retry_admissible &&
      std::find(assessment.allowed_actions.begin(), assessment.allowed_actions.end(),
                request.action) != assessment.allowed_actions.end() &&
      request.action != RecoveryAction::kResetCycle;
  if (!exact_assessment || !action_allowed) {
    KernelRecoveryActionResultDTO rejected{
        request.request_id, request.action, false, assessment,
        ErrorDTO{exact_assessment ? "operation_recovery_invalid" : "operation_recovery_stale",
                 exact_assessment ? "recovery action is not offered to every participant"
                                  : "coordinated recovery assessment changed",
                 {{"reason", exact_assessment ? "recovery_action_not_offered"
                                              : "recovery_assessment_changed"}}}};
    {
      std::lock_guard state_lock(state_mutex_);
      retain_current(OperationRecoveryReplayEntry{operation_id, expected_operation_revision,
                                                  request, rejected});
    }
    return Result<KernelRecoveryActionResultDTO>::Ok(std::move(rejected));
  }

  struct Target final {
    IResolvedParticipantExecutionEngine* engine{};
    KernelRecoveryAssessmentDTO assessment;
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    std::uint64_t preparation_id{};
    std::size_t continuation_index{};
    std::shared_ptr<ProgramCycleClaimRun::MotionContinuation> continuation;
  };
  std::vector<Target> targets;
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  std::optional<ProgramRunExecutionCaptureDTO> continuation_capture;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll ||
        operation->participants.size() < 2U ||
        operation->participants.size() != operation->recovery_assessment_inputs.size() ||
        operation->stop_effect_in_flight || operation->ordinary_commit_in_flight ||
        operation->ordinary_acceptance_pending || operation->pending_control.has_value() ||
        operation->runtime_scale_effect_in_flight || operation->recovery_effect_in_flight ||
        operation->coordinated_cancellation_in_flight || operation->dispatch_effect_in_flight() ||
        operation->reconciliation_in_flight() ||
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& participant) {
                      return participant.engine == nullptr ||
                             participant.preparation_borrow_in_flight();
                    })) {
      return reject<KernelRecoveryActionResultDTO>(
          "coordinated operation recovery fence is stale or busy", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
    if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
      return Result<KernelRecoveryActionResultDTO>::Err(
          ErrorDTO{"operation_recovery_blocked_by_stop_fault",
                   "Program Reset is required before motion can resume",
                   {{"reason", "active_stop_fault_requires_reset"},
                    {"latch_id", active_stop_fault_latch_->latch_id.value()}}});
    }
    if (motion_producing_recovery(request.action) && !operation->manual) {
      if (auto refusal = operator_alarm_attention_refusal_locked(
              operation_resources(operation->participants))) {
        return Result<KernelRecoveryActionResultDTO>::Err(std::move(*refusal));
      }
    }
    if (!operation->stop_allows_retirement()) {
      return reject<KernelRecoveryActionResultDTO>(
          "operation recovery awaits its exact successful Stop certificate",
          "operation_recovery_busy", ErrorCode::kInvalidStateTransition);
    }
    stop_id = operation->state.active_stop_id;
    stop_generation = operation->state.active_stop_generation;
    if (program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
        program_cycle_claim_run_->program_run_id == operation->state.program_run_id) {
      continuation_capture = *program_cycle_claim_run_->capture;
    }
    targets.reserve(operation->participants.size());
    const bool capture_continuation =
        request.action == RecoveryAction::kRetryMotionFromCurrentState ||
        request.action == RecoveryAction::kReplanFromStoppedState ||
        request.action == RecoveryAction::kAcknowledgeAndContinue ||
        request.action == RecoveryAction::kRetryAfterSceneRepair;
    for (std::size_t index = 0U; index < operation->participants.size(); ++index) {
      auto& participant = operation->participants[index];
      const auto& public_participant = operation->state.participants[index];
      Target target{
          participant.engine, operation->recovery_assessment_inputs[index], nullptr, 0U, 0U, {}};
      if (capture_continuation &&
          participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
          participant.prepared_dispatch_released && participant.prepared_participant &&
          public_participant.execution_scope && continuation_capture) {
        const auto& scope = *public_participant.execution_scope;
        const auto node =
            std::lower_bound(continuation_capture->table->nodes.begin(),
                             continuation_capture->table->nodes.end(), scope.node_instance_id,
                             [](const auto& row, const auto& id) { return row.instance_id < id; });
        if (node != continuation_capture->table->nodes.end() &&
            node->instance_id == scope.node_instance_id && node->owner_api == "kernel.operation") {
          target.continuation_index =
              static_cast<std::size_t>(node - continuation_capture->table->nodes.begin());
          const auto& source =
              std::get<PreparedMotionExecutionDTO>(participant.prepared_participant->payload)
                  .plan_request;
          target.continuation = std::make_shared<ProgramCycleClaimRun::MotionContinuation>(
              ProgramCycleClaimRun::MotionContinuation{
                  scope,
                  operation_id,
                  public_participant.participant_id,
                  participant.prepared_participant->prepared_contract_digest,
                  participant.binding,
                  {},
                  source.base_frame,
                  source.tcp_frame,
                  source.planner_capability_binding,
                  std::get<MotionParticipantIntentDTO>(
                      operation->intent.participants[index].payload)
                      .motion.index()});
          target.prepared_engine = participant.prepared_motion_engine;
          target.preparation_id = participant.subordinate_id;
        }
      }
      targets.push_back(std::move(target));
    }
    operation->recovery_effect_in_flight = true;
  }

  const ScopeExit release_recovery_borrow([&] {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id))
      operation->recovery_effect_in_flight = false;
  });
  const auto unresolved_locked = [&](std::string message) {
    motion_admission_closed_ = true;
    KernelRecoveryActionResultDTO result{
        request.request_id, request.action, false, assessment,
        ErrorDTO{"operation_recovery_outcome_unresolved",
                 std::move(message),
                 {{"reason", "operation_recovery_outcome_unresolved"}}}};
    retain_current(OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request,
                                                result, true});
    return reject<KernelRecoveryActionResultDTO>(
        "coordinated participant recovery outcome is unknown",
        "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
  };
  const auto unresolved = [&](std::string message) {
    std::lock_guard state_lock(state_mutex_);
    return unresolved_locked(std::move(message));
  };

  try {
    for (auto& target : targets) {
      if (!target.continuation) continue;
      if (target.prepared_engine == nullptr) {
        return reject<KernelRecoveryActionResultDTO>("motion continuation has no prepared owner",
                                                     "operation_recovery_invalid",
                                                     ErrorCode::kInvalidRequest);
      }
      auto snapshot = target.prepared_engine->ObservePreparedMotion(target.preparation_id);
      if (!snapshot.ok()) return Result<KernelRecoveryActionResultDTO>::Err(snapshot.error());
      const auto& observed = snapshot.value();
      if (observed.execution_id != target.preparation_id) {
        return reject<KernelRecoveryActionResultDTO>(
            "motion continuation changed preparation identity", "operation_recovery_invalid",
            ErrorCode::kInvalidRequest);
      }
      const auto& source = observed.last_dispatched_participant;
      if (!source &&
          (observed.state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch ||
           observed.state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed)) {
        target.continuation.reset();
        continue;
      }
      if (!source ||
          source->prepared_contract_digest != target.continuation->prepared_contract_digest ||
          source->operation_id != operation_id ||
          source->participant_id != target.continuation->participant_id ||
          !std::holds_alternative<PreparedMotionExecutionDTO>(source->payload)) {
        return reject<KernelRecoveryActionResultDTO>(
            "motion continuation dispatch identity is unsettled", "operation_recovery_invalid",
            ErrorCode::kInvalidRequest);
      }
      const auto& motion = std::get<PreparedMotionExecutionDTO>(source->payload);
      target.continuation->base_frame = motion.plan_request.base_frame;
      target.continuation->tcp_frame = motion.plan_request.tcp_frame;
      target.continuation->planner_capability = motion.plan_request.planner_capability_binding;
      auto captured = detail::capture_motion_resume_intent(
          motion.plan_request.motion, motion.trajectory, observed.stopped_trajectory_time_s);
      if (!captured.ok()) return Result<KernelRecoveryActionResultDTO>::Err(captured.error());
      target.continuation->intent =
          std::make_shared<const detail::MotionResumeIntent>(std::move(captured.value()));
    }
  } catch (...) {
    return reject<KernelRecoveryActionResultDTO>(
        "motion continuation could not be captured before coordinated recovery",
        "operation_recovery_invalid", ErrorCode::kInternalError);
  }

  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    bool participant_changed =
        operation == nullptr || operation->participants.size() != targets.size();
    for (std::size_t index = 0U; !participant_changed && index < targets.size(); ++index) {
      participant_changed = operation->participants[index].engine != targets[index].engine;
    }
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || !operation->stop_allows_retirement() ||
        participant_changed ||
        (continuation_capture &&
         (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
          program_cycle_claim_run_->capture->table != continuation_capture->table ||
          program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
          program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id))) {
      return reject<KernelRecoveryActionResultDTO>(
          "coordinated operation changed before recovery effects", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
  }

  std::size_t accepted_count = 0U;
  std::optional<KernelRecoveryActionResultDTO> rejected;
  for (auto& target : targets) {
    auto participant_request = request;
    participant_request.expected_assessment_revision = target.assessment.assessment_revision;
    participant_request.expected_control_command_epoch = target.assessment.control_command_epoch;
    participant_request.expected_execution_id = target.assessment.execution_id;
    participant_request.expected_latch_id = target.assessment.latch_id;
    participant_request.expected_stop_certificate_id = target.assessment.stop_certificate_id;
    participant_request.expected_stop_owner_generation = target.assessment.stop_owner_generation;
    try {
      auto result = target.engine->apply_recovery_action(participant_request);
      if (result.request_id != request.request_id || result.action != request.action) {
        return unresolved("participant recovery returned a different command identity");
      }
      if (!result.accepted) {
        rejected = std::move(result);
        break;
      }
      ++accepted_count;
    } catch (...) {
      return unresolved("participant recovery threw after coordinated acceptance began");
    }
  }
  if (rejected.has_value()) {
    if (accepted_count != 0U)
      return unresolved("coordinated participants disagreed after a recovery effect");
    KernelRecoveryActionResultDTO result{request.request_id, request.action, false, assessment,
                                         rejected->error};
    {
      std::lock_guard state_lock(state_mutex_);
      retain_current(
          OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
    }
    return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
  }

  KernelRecoveryActionResultDTO result{request.request_id, request.action, true, {}, std::nullopt};
  result.resulting_assessment.schema_version = "manyforge.kernel_recovery_assessment.v1";
  result.resulting_assessment.assessment_revision = assessment.assessment_revision;
  result.resulting_assessment.reason_code = "coordinated_operation_recovered";
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->participants.size() != targets.size() || !operation->stop_allows_retirement()) {
      return unresolved_locked("coordinated operation changed while recovery results returned");
    }
    if (continuation_capture &&
        (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
         program_cycle_claim_run_->capture->table != continuation_capture->table ||
         program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
         program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id)) {
      return unresolved_locked("Program capture changed during coordinated motion retirement");
    }
    if (program_cycle_claim_run_ &&
        std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
          return target.continuation &&
                 target.continuation_index >= program_cycle_claim_run_->motion_continuations.size();
        })) {
      return unresolved_locked("motion continuation index changed during retirement");
    }
    auto owner_rows =
        retained_owner_rows(operation->participants, resource_slots_->coherent_snapshot());
    if (!owner_rows.ok())
      return unresolved_locked("coordinated resource ownership changed during retirement");
    if (!owner_rows.value().empty()) {
      auto released = resource_slots_->release_batch(owner_rows.value());
      if (!released.ok())
        return unresolved_locked("coordinated resource claims could not be retired");
    }
    if (program_cycle_claim_run_) {
      for (auto& target : targets) {
        if (target.continuation) {
          program_cycle_claim_run_->motion_continuations[target.continuation_index] =
              std::move(target.continuation);
        }
      }
    }
    retain_current(
        OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
    retain_historical_operation_locked(operation->state);
    active_operations_.erase(operation_id);
  }
  return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
