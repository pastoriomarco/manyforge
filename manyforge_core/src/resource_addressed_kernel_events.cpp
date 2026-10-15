#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {

std::optional<OperationId> ResourceAddressedExecutionKernel::find_event_operation_locked(
    const IResolvedParticipantExecutionEngine* engine, const std::uint64_t subordinate_id) const {
  if (engine == nullptr || subordinate_id == 0U) {
    return std::nullopt;
  }
  std::optional<OperationId> result;
  for (const auto& [operation_id, operation] : active_operations_) {
    const auto* participant = operation->participant_for(engine);
    if (participant == nullptr || participant->subordinate_id != subordinate_id) {
      continue;
    }
    if (result.has_value()) {
      // Admission prevents one engine instance from owning two operations.
      // Fail closed if corrupted state ever violates that invariant.
      return std::nullopt;
    }
    result = operation_id;
  }
  return result;
}

Result<bool> ResourceAddressedExecutionKernel::confirm_prepared_command_no_effect(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine,
    const std::uint64_t preparation_id) {
  OperationCommitGuard commit(*this, operation_id);
  IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
  OperationRevision revision(0U);
  std::shared_ptr<std::mutex> resident;
  bool proof_borrow_claimed = false;
  const ScopeExit release_proof_borrow([&] {
    if (!proof_borrow_claimed) return;
    std::lock_guard lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation ? operation->participant_for(engine) : nullptr;
    if (participant && operation->commit_mutex == resident && participant->preparation_id == preparation_id &&
        participant->prepared_motion_engine == prepared_engine) {
      participant->prepared_advance_in_flight = false;
      settle_completed_control_locked(operation_id);
      wake_stop_after_preparation_borrow_locked(operation_id);
    }
  });
  {
    std::lock_guard lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation ? operation->participant_for(engine) : nullptr;
    if (!participant || !participant->non_motion_transaction() || !preparation_id ||
        participant->preparation_id != preparation_id || participant->dispatch_effect_in_flight ||
        participant->prepare_in_flight || operation->pending_control || operation->recovery_effect_in_flight ||
        participant->subordinate_generation != 0U || !participant->prepared_motion_engine)
      return Result<bool>::Ok(false);
    prepared_engine = participant->prepared_motion_engine;
    revision = operation->state.operation_revision;
    resident = operation->commit_mutex;
    if (!participant->prepared_advance_in_flight && !participant->reconciliation_in_flight) {
      participant->prepared_advance_in_flight = true;
      proof_borrow_claimed = true;
    }
  }
  const auto observed = prepared_engine->ObservePreparedCapabilityCommand(preparation_id);
  if (!observed.ok()) return Result<bool>::Err(observed.error());
  const auto& snapshot = observed.value();
  if (snapshot.preparation_id != preparation_id ||
      snapshot.state != ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch ||
      snapshot.dispatch_result || !snapshot.participant)
    return Result<bool>::Ok(false);
  std::lock_guard lock(state_mutex_);
  auto* operation = find_active_operation_locked(operation_id);
  auto* participant = operation ? operation->participant_for(engine) : nullptr;
  if (!participant || operation->commit_mutex != resident ||
      operation->state.operation_revision != revision || operation->pending_control ||
      operation->recovery_effect_in_flight || participant->preparation_id != preparation_id ||
      participant->prepared_motion_engine != prepared_engine || participant->dispatch_effect_in_flight ||
      participant->prepare_in_flight || participant->subordinate_generation != 0U ||
      !participant->stop_cancelled_pre_dispatch || !participant->non_motion_transaction() ||
      snapshot.participant->operation_id != operation_id ||
      snapshot.participant->operation_revision != operation->accepted.operation_revision ||
      snapshot.participant->participant_id != participant->slot_owner.participant_id ||
      snapshot.participant->command_target_id != participant->binding.value().command_target_id)
    return Result<bool>::Ok(false);
  participant->prepared_engine_no_effect_confirmed = true;
  if (!snapshot.error && participant->last_subordinate_error) {
    const auto reason = participant->last_subordinate_error->details.find("reason");
    if (reason != participant->last_subordinate_error->details.end() &&
        participant->last_subordinate_error->code == std::string(to_string(ErrorCode::kInvalidRequest)) &&
        reason->second == "prepared_command_dispatch_mismatch") {
      // The exact actual owner has now proved its irreversible, clean
      // precommit cancellation after the real dispatch call returned. Its
      // expected rejected late Dispatch is a diagnostic, not a failed output.
      // Unknown/mismatched observations never reach this resident CAS.
      participant->last_subordinate_error.reset();
    }
  }
  return Result<bool>::Ok(true);
}

Result<void> ResourceAddressedExecutionKernel::discard_uncommitted_ordinary_preparations(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* failing_engine,
    std::optional<ErrorDTO> failure) {
  struct Target final {
    IResolvedParticipantExecutionEngine* engine{};
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    OperationParticipantPayloadKind kind{};
    bool pneumatic{};
    std::uint64_t preparation_id{};
    std::optional<ErrorDTO> failure;
  };
  std::vector<Target> targets;
  {
    std::lock_guard lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (!operation ||
        (operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll &&
         !(failure && !failing_engine)) || operation->pending_control ||
        operation->recovery_effect_in_flight || operation->coordinated_cancellation_in_flight)
      return Result<void>::Ok();
    if (operation->coordinated_dispatch_barrier_released)
      return failure ? reject<void>("preparation failure arrived after coordinated release",
                                   "coordinated_pre_dispatch_barrier_race", ErrorCode::kInvariantViolation)
                     : Result<void>::Ok();
    const bool stopped_before_barrier = std::any_of(operation->participants.begin(),
        operation->participants.end(), [](const auto& row) { return row.stop_owns_reconciliation(); });
    const bool cancellation_pending = std::any_of(operation->participants.begin(),
        operation->participants.end(), [](const auto& row) { return row.stop_cancelled_pre_dispatch; });
    if (!stopped_before_barrier && !cancellation_pending && !failure) return Result<void>::Ok();
    for (auto& participant : operation->participants) {
      if (participant.stop_owns_reconciliation()) continue;
      if (failure && (!failing_engine || participant.engine == failing_engine))
        participant.last_subordinate_error = failure;
      // Coordinated precommit remains all-or-nothing. No ordinary member may
      // dispatch after a sibling lost that barrier to moving containment.
      if (participant.prepared_dispatch_released || participant.dispatch_effect_in_flight)
        return reject<void>("unreleased coordinated barrier has a dispatched member",
                            "coordinated_pre_dispatch_barrier_race", ErrorCode::kInvariantViolation);
      participant.stop_cancelled_pre_dispatch = true;
      const auto row = std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
          [&](const auto& value) { return value.participant_id == participant.slot_owner.participant_id; });
      if (row == operation->state.participants.end())
        return reject<void>("ordinary cancellation has no participant row", "operation_participant_state_missing",
                            ErrorCode::kInvariantViolation);
      if ((terminal(row->state) && participant.slot_state == ResourceSlotState::kTerminal) ||
          participant.prepare_in_flight || participant.prepared_advance_in_flight ||
          participant.reconciliation_in_flight) continue;
      participant.prepared_advance_in_flight = true;
      targets.push_back({participant.engine, participant.prepared_motion_engine, participant.payload_kind,
                         participant.non_motion_transaction(), participant.preparation_id,
                         participant.last_subordinate_error});
    }
  }
  std::optional<ErrorDTO> first_error;
  for (const auto& target : targets) {
    auto cancelled = target.preparation_id == 0U ? Result<void>::Ok()
        : target.prepared_engine ? cancel_prepared_participant(*target.prepared_engine, target.kind,
                                                              target.preparation_id)
        : reject<void>("ordinary prepared cancellation has no owner", "prepared_cancellation_target_missing",
                       ErrorCode::kInvariantViolation);
    {
      std::lock_guard lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation ? operation->participant_for(target.engine) : nullptr;
      if (!participant || participant->preparation_id != target.preparation_id) continue;
      participant->prepared_advance_in_flight = false;
      if (!cancelled.ok()) participant->last_subordinate_error = cancelled.error();
    }
    if (cancelled.ok() && target.pneumatic &&
        target.preparation_id != 0U) {
      const auto proof = confirm_prepared_command_no_effect(operation_id, target.engine, target.preparation_id);
      if (!proof.ok()) continue;
      if (!proof.value()) continue;
    }
    const auto cause = !cancelled.ok() ? std::optional(cancelled.error()) : target.failure;
    auto applied = apply_subordinate_state(operation_id, target.engine, target.preparation_id,
        cause ? OperationStateKind::kInterventionRequired : OperationStateKind::kCancelledPreDispatch,
        0.0, cause);
    if (!cancelled.ok() && !first_error) first_error = cancelled.error();
    if (!applied.ok() && !first_error) first_error = applied.error();
  }
  return first_error ? Result<void>::Err(*first_error) : Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::fail_pre_dispatch(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* failing_engine,
    ErrorDTO failure) {
  struct Cancellation final {
    IResolvedPreparedParticipantExecutionEngine* engine{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::uint64_t preparation_id{};
  };
  std::vector<Cancellation> cancellations;
  std::vector<ResourceSlotReservationOwnerDTO> reservation_rows;
  const bool admission_refusal = failing_engine == nullptr;
  enum class FailureRoute { kDone, kOrdinary, kCoordinated };
  const auto failure_route_locked = [&](ActiveOperation* operation) {
    if (operation == nullptr ||
        (!admission_refusal &&
         operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll))
      return FailureRoute::kDone;
    if (!operation->state.active_stop_id) return FailureRoute::kCoordinated;
    const auto* failing = operation->participant_for(failing_engine);
    if (admission_refusal || (failing && !failing->stop_owns_reconciliation())) {
      // This decision belongs to EACH locked ownership check. Stop can win
      // while an ordinary failure is waiting for its journal writer.
      return FailureRoute::kOrdinary;
    }
    if (auto* participant = operation->participant_for(failing_engine))
      participant->last_subordinate_error = failure;
    if (operation->state.active_stop_generation) {
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate && candidate->active &&
                   candidate->stop_id == operation->state.active_stop_id &&
                   candidate->stop_generation == *operation->state.active_stop_generation;
          });
      if (transaction != stop_transactions_.end()) {
        stop_control_executor_->ArmDeadline((*transaction)->token, clock_.now());
        stop_control_executor_->Publish((*transaction)->token);
      } else motion_admission_closed_ = true;
    }
    return FailureRoute::kDone;
  };
  FailureRoute route;
  {
    // A Stop-owned cancellation callback must not wait for ordinary storage.
    std::lock_guard state_lock(state_mutex_);
    route = failure_route_locked(find_active_operation_locked(operation_id));
  }
  if (route == FailureRoute::kDone) return Result<void>::Ok();
  if (route == FailureRoute::kOrdinary)
    return discard_uncommitted_ordinary_preparations(operation_id, failing_engine, failure);
  {
    OperationCommitGuard commit(*this, operation_id);
    std::unique_lock state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    route = failure_route_locked(operation);
    if (route == FailureRoute::kDone) return Result<void>::Ok();
    if (route == FailureRoute::kOrdinary) {
      state_lock.unlock();
      commit.release();
      return discard_uncommitted_ordinary_preparations(operation_id, failing_engine, failure);
    }
    if (std::all_of(
            operation->participants.begin(), operation->participants.end(),
            [](const auto& participant) { return participant.stop_cancelled_pre_dispatch; })) {
      // This operation's abort already owns cancellation and final cleanup.
      return Result<void>::Ok();
    }
    if (operation->coordinated_dispatch_barrier_released ||
        std::any_of(
            operation->participants.begin(), operation->participants.end(),
            [](const auto& participant) { return participant.prepared_dispatch_released; })) {
      motion_admission_closed_ = true;
      return reject<void>("pre-dispatch failure raced the coordinated dispatch barrier",
                          "coordinated_pre_dispatch_barrier_race", ErrorCode::kInvariantViolation);
    }

    cancellations.reserve(operation->participants.size());
    reservation_rows.reserve(operation->participants.size());
    operation->coordinated_cancellation_in_flight = true;
    for (auto& participant : operation->participants) {
      participant.stop_cancelled_pre_dispatch = true;
      if (admission_refusal || participant.engine == failing_engine) {
        participant.last_subordinate_error = failure;
      }
      if (participant.prepared_motion_engine != nullptr && participant.subordinate_id != 0U) {
        cancellations.push_back(Cancellation{participant.prepared_motion_engine,
                                             participant.payload_kind, participant.subordinate_id});
      }
      reservation_rows.push_back({participant.slot_owner, participant.resources});
    }
    for (auto& row : operation->state.participants) {
      const auto participant = std::find_if(
          operation->participants.begin(), operation->participants.end(), [&](const auto& current) {
            return current.slot_owner.participant_id == row.participant_id;
          });
      const bool failed =
          participant != operation->participants.end() && participant->engine == failing_engine;
      // An admission refusal applies to every participant alike and takes
      // the same no-effect failure outcome as a failed preparation.
      row.state = failed || admission_refusal ? OperationStateKind::kInterventionRequired
                                              : OperationStateKind::kCancelledPreDispatch;
      row.progress = 0.0;
      row.terminal_error =
          failed || admission_refusal ? std::optional(failure) : std::nullopt;
    }
    operation->state.state = OperationStateKind::kInterventionRequired;
    operation->state.updated_at_ns = clock_.now();
    stamp_operation_capture_locked(*operation, operation->state);
  }

  std::optional<ErrorDTO> cancellation_failure;
  for (const auto& cancellation : cancellations) {
    auto cancelled = cancel_prepared_participant(*cancellation.engine, cancellation.payload_kind,
                                                 cancellation.preparation_id);
    if (!cancelled.ok() && !cancellation_failure.has_value()) {
      cancellation_failure = cancelled.error();
    }
  }

  OperationCommitGuard commit(*this, operation_id);
  std::unique_lock state_lock(state_mutex_);
  auto* operation = find_active_operation_locked(operation_id);
  if (operation != nullptr) {
    operation->coordinated_cancellation_in_flight = false;
  }
  if (operation == nullptr) return Result<void>::Ok();
  if (operation->state.active_stop_id) {
    // Stop may also arrive while the original coordinated abort's provider
    // cancellations are out. Finish only the ordinary rows through their
    // actual owner proof/slot transition; moving rows remain Stop-owned.
    state_lock.unlock();
    commit.release();
    return discard_uncommitted_ordinary_preparations(operation_id, failing_engine, failure);
  }
  if (cancellation_failure.has_value()) {
    motion_admission_closed_ = true;
    for (auto& row : operation->state.participants) {
      if (row.state == OperationStateKind::kCancelledPreDispatch) {
        row.state = OperationStateKind::kInterventionRequired;
        row.terminal_error = cancellation_failure;
      }
    }
  }
  auto captured = operation->state;
  auto durable = record_operation_state_locked(
      state_lock, captured, nullptr,
      cancellation_failure.has_value() ? "coordinated_prepare_cancel_failed"
      : admission_refusal              ? "admission_refused_before_dispatch"
                                       : "coordinated_prepare_failed_before_dispatch");
  if (!durable.ok()) {
    motion_admission_closed_ = true;
    return Result<void>::Err(durable.error());
  }
  if (!durable.value()) return Result<void>::Ok();
  operation->state = std::move(captured);
  if (operation->dispatch_effect_in_flight() || operation->reconciliation_in_flight() ||
      std::any_of(operation->participants.begin(), operation->participants.end(),
                  [](const auto& participant) {
                    return participant.preparation_borrow_in_flight();
                  })) {
    // A sibling may still hold a borrowed engine from before the abort. Keep
    // the fault resident; existing explicit Reset can retire it after drain.
    return Result<void>::Ok();
  }
  std::sort(reservation_rows.begin(), reservation_rows.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.owner.operation_id, left.owner.participant_id) <
                     std::tie(right.owner.operation_id, right.owner.participant_id);
            });
  const auto released = resource_slots_->release_batch(reservation_rows);
  if (!released.ok()) {
    motion_admission_closed_ = true;
    return released;
  }
  for (auto& participant : operation->participants) {
    participant.slot_state = ResourceSlotState::kTerminal;
  }
  retain_historical_operation_locked(operation->state);
  active_operations_.erase(operation_id);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::advance_prepared_motion(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine) {
  struct Target final {
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t subordinate_id{};
    RuntimeParticipantBinding binding;
    RuntimeParticipantStopReadiness readiness;
    std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
    MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
    std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> fault_stop_plan;
  };
  std::optional<Target> target;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant == nullptr ||
        participant->payload_kind != OperationParticipantPayloadKind::kMotion ||
        operation->recovery_effect_in_flight || participant->prepared_motion_engine == nullptr ||
        participant->subordinate_id == 0U || participant->prepared_dispatch_released ||
        participant->prepared_advance_in_flight || participant->prepared_participant.has_value() ||
        operation->coordinated_dispatch_barrier_released ||
        (terminal(operation->state.state) && !operation->state.active_stop_id.has_value())) {
      return Result<void>::Ok();
    }
    participant->prepared_advance_in_flight = true;
    target.emplace(Target{
        participant->prepared_motion_engine, participant->engine, participant->subordinate_id,
        participant->binding, participant->stop_readiness,
        participant->retained_default_stop_limits, participant->stop_node_category,
        participant->ordinary_stop_plan, participant->fault_stop_plan});
  }

  const auto release_claim = [&]() {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(target->engine);
    if (participant != nullptr && participant->subordinate_id == target->subordinate_id) {
      participant->prepared_advance_in_flight = false;
      settle_completed_control_locked(operation_id);
      wake_stop_after_preparation_borrow_locked(operation_id);
    }
  };

  auto snapshot = target->prepared_engine->ObservePreparedMotion(target->subordinate_id);
  if (!snapshot.ok()) {
    release_claim();
    return Result<void>::Err(snapshot.error());
  }
  if (snapshot.value().execution_id != target->subordinate_id) {
    release_claim();
    return reject<void>("prepared execution observation changed identity",
                        "prepared_motion_observation_identity_mismatch",
                        ErrorCode::kInvariantViolation);
  }
  if (snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparing) {
    release_claim();
    return Result<void>::Ok();
  }

  if (snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
      snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch) {
    const bool cancelled =
        snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
    const auto failure = snapshot.value().error.value_or(error(
        std::string(to_string(cancelled ? ErrorCode::kInvalidStateTransition
                                        : ErrorCode::kExecutionRejected)),
        cancelled ? "prepared execution was cancelled before dispatch"
                  : "prepared execution failed before dispatch",
        cancelled ? "prepared_motion_cancelled_pre_dispatch" : "prepared_motion_prepare_failed"));
    bool stop_owns_terminal = false;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(target->engine);
      if (participant != nullptr) {
        participant->prepared_advance_in_flight = false;
        participant->last_subordinate_error = failure;
        stop_owns_terminal = participant->stop_owns_reconciliation();
        if (stop_owns_terminal) {
          settle_completed_control_locked(operation_id);
          wake_stop_after_preparation_borrow_locked(operation_id);
        }
      }
    }
    if (stop_owns_terminal) {
      return Result<void>::Ok();
    }
    bool coordinated = false;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto* operation = find_active_operation_locked(operation_id);
      coordinated = operation != nullptr && operation->intent.coordination_policy ==
                                                OperationCoordinationPolicy::kCoordinatedAll;
    }
    if (!coordinated) {
      return apply_subordinate_state(
          operation_id, target->engine, target->subordinate_id,
          cancelled ? OperationStateKind::kStopped : OperationStateKind::kInterventionRequired, 0.0,
          cancelled ? std::optional<ErrorDTO>{} : std::optional(failure));
    }
    return fail_pre_dispatch(operation_id, target->engine, failure);
  }

  if (!snapshot.value().participant || !snapshot.value().backend_prepare_result ||
      snapshot.value().state != ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
    release_claim();
    return reject<void>("prepared execution observation has an invalid state shape",
                        "prepared_motion_observation_invalid", ErrorCode::kInvariantViolation);
  }
  const auto verified = VerifyPreparedOperationParticipant(
      *snapshot.value().participant, target->binding, target->readiness,
      target->retained_default_stop_limits, target->stop_node_category);
  if (!verified.ok() ||
      snapshot.value().participant->ordinary_stop_plan != *target->ordinary_stop_plan ||
      !prepared_fault_stop_plan_matches(snapshot.value().participant->fault_stop_plan,
                                        target->fault_stop_plan) ||
      prepare_participant_result_field_error(*snapshot.value().backend_prepare_result,
                                             *snapshot.value().participant)
          .has_value()) {
    release_claim();
    return verified.ok()
               ? reject<void>("prepared participant differs from owner authority",
                              "prepared_motion_product_mismatch", ErrorCode::kInvariantViolation)
               : Result<void>::Err(verified.error());
  }

  return release_prepared_dispatch_barrier(operation_id, target->engine, target->subordinate_id,
                                           *snapshot.value().participant);
}

void ResourceAddressedExecutionKernel::settle_deferred_capability_stop(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* participant_engine,
    IResolvedParticipantExecutionEngine* settlement_engine,
    const AddressedParticipantStopSettlementRequestDTO& request) {
  // The same exact settlement the Stop would have requested had the command
  // already returned (Q58: the Stop did not wait for it).
  std::optional<ErrorDTO> failure;
  if (settlement_engine == nullptr) {
    failure = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                    "Stopped participant lost its local execution owner",
                    "participant_stop_settlement_authority_missing");
  } else {
    try {
      const auto settled = settlement_engine->settle_addressed_participant_stop(request);
      if (!settled.ok()) {
        failure = settled.error();
      } else {
        const auto& value = settled.value();
        const auto& recovery = value.recovery_assessment;
        if (value.execution_id != request.execution_id ||
            !(value.aggregate_stop_certificate == request.aggregate_stop_certificate) ||
            recovery.execution_id != request.execution_id ||
            recovery.status != RecoveryAssessmentStatus::kRetryReady) {
          failure = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                          "participant returned a mismatched addressed Stop settlement",
                          "participant_stop_settlement_mismatch");
        }
      }
    } catch (const std::exception& exception) {
      failure = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                      std::string("addressed Stop settlement threw: ") + exception.what(),
                      "participant_stop_settlement_threw");
    } catch (...) {
      failure = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                      "addressed Stop settlement threw", "participant_stop_settlement_threw");
    }
  }
  if (!failure.has_value()) return;
  std::lock_guard state_lock(state_mutex_);
  motion_admission_closed_ = true;
  auto* operation = find_active_operation_locked(operation_id);
  auto* participant =
      operation == nullptr ? nullptr : operation->participant_for(participant_engine);
  if (participant != nullptr) participant->last_subordinate_error = std::move(failure);
}

void ResourceAddressedExecutionKernel::settle_deferred_pre_dispatch_cancellations() noexcept {
  struct ReturnedCancellation final {
    OperationId operation_id;
    IResolvedParticipantExecutionEngine* engine{};
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    AddressedParticipantStopSettlementRequestDTO request;
    OperationStateKind state{OperationStateKind::kStopped};
    std::optional<ErrorDTO> error;
  };
  try {
    std::vector<ReturnedCancellation> returned;
    {
      std::lock_guard state_lock(state_mutex_);
      for (auto& [operation_id, operation] : active_operations_) {
        for (auto& participant : operation->participants) {
          auto& deferred = participant.deferred_cancel_settlement;
          if (!deferred.has_value() || !deferred->returned_state.has_value() ||
              deferred->settlement_in_flight) {
            continue;
          }
          deferred->settlement_in_flight = true;
          returned.push_back(ReturnedCancellation{operation_id, participant.engine,
                                                  participant.prepared_motion_engine,
                                                  deferred->request, *deferred->returned_state,
                                                  deferred->returned_error});
        }
      }
    }
    for (auto& cancellation : returned) {
      std::optional<ErrorDTO> failure;
      bool preparation_failed = false;
      if (cancellation.state == OperationStateKind::kStopped) {
        // The same exact settlement the Stop would have requested had the
        // cancellation already returned; a failed settlement fails closed.
        settle_deferred_capability_stop(cancellation.operation_id, cancellation.engine,
                                        cancellation.engine, cancellation.request);
      } else {
        // As the synchronous path: only the prepared owner's exact snapshot
        // proves an already-failed, never-dispatched preparation; anything
        // else is a failed cancellation.
        failure = cancellation.error.value_or(
            error(std::string(to_string(ErrorCode::kExecutionAborted)),
                  "prepared participant failed while settling pre-dispatch cancellation",
                  "prepared_cancel_terminal_failed"));
        if (cancellation.prepared_engine != nullptr) {
          try {
            const auto execution_id = cancellation.request.execution_id;
            const auto snapshot = cancellation.prepared_engine->ObservePreparedMotion(execution_id);
            preparation_failed =
                snapshot.ok() && snapshot.value().execution_id == execution_id &&
                snapshot.value().state ==
                    ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed &&
                !snapshot.value().last_dispatched_participant;
          } catch (...) {
            preparation_failed = false;
          }
        }
      }
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(cancellation.operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(cancellation.engine);
      if (participant == nullptr || !participant->deferred_cancel_settlement.has_value() ||
          participant->deferred_cancel_settlement->request.execution_id !=
              cancellation.request.execution_id) {
        motion_admission_closed_ = true;
        continue;
      }
      if (failure.has_value()) {
        participant->last_subordinate_error = std::move(failure);
        if (!preparation_failed) motion_admission_closed_ = true;
      }
      participant->deferred_cancel_settlement.reset();
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

Result<void> ResourceAddressedExecutionKernel::release_prepared_dispatch_barrier(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine,
    std::uint64_t preparation_id, const PreparedOperationParticipantDTO& participant_product) {
  struct DispatchTarget final {
    OperationParticipantId participant_id;
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t preparation_id{};
    ContentDigest contract_digest;
    ControllerGoalId controller_goal_id;
    ControllerGoalGeneration controller_goal_generation{0U};
  };
  struct CancellationTarget final {
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::uint64_t preparation_id{};
  };

  bool cancel_instead = false;
  bool non_motion_cancel = false;
  std::optional<CancellationTarget> cancellation_target;
  std::optional<StopControlWorkToken> stop_wakeup;
  std::vector<DispatchTarget> dispatch_targets;
  const ScopeExit release_advance([&] {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant != nullptr && participant->preparation_id == preparation_id) {
      participant->prepared_advance_in_flight = false;
      settle_completed_control_locked(operation_id);
      // The Stop branch below wakes its Stop while this read still holds the
      // borrow; that pass can only wait. Wake it again now that it has ended.
      wake_stop_after_preparation_borrow_locked(operation_id);
    }
  });
  {
    OperationCommitGuard commit(*this, operation_id);
    std::unique_lock state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant == nullptr || participant->subordinate_id != preparation_id) {
      return Result<void>::Ok();
    }
    if (operation->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll &&
        !operation->coordinated_dispatch_barrier_released &&
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& row) { return row.stop_owns_reconciliation(); })) {
      participant->prepared_advance_in_flight = false;
      state_lock.unlock();
      commit.release();
      return discard_uncommitted_ordinary_preparations(operation_id);
    }
    fence_held_prepared_program_commands_locked(operation->state.program_run_id);
    if (participant->stop_owns_reconciliation() && operation->state.active_stop_id.has_value() &&
        operation->state.active_stop_generation.has_value()) {
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active &&
                   candidate->stop_id == operation->state.active_stop_id &&
                   candidate->stop_generation == *operation->state.active_stop_generation;
          });
      if (transaction == stop_transactions_.end()) {
        motion_admission_closed_ = true;
      } else {
        stop_wakeup = (*transaction)->token;
        stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
      }
    } else if (participant->stop_cancelled_pre_dispatch &&
               operation->intent.coordination_policy ==
                   OperationCoordinationPolicy::kCoordinatedAll &&
               terminal(operation->state.state)) {
      // The coordinated abort already owns cancellation. A late prepared
      // read is not permission to cancel or dispatch the participant again.
      return Result<void>::Ok();
    } else if (participant->slot_state != ResourceSlotState::kReserved ||
               participant->stop_cancelled_pre_dispatch || terminal(operation->state.state)) {
      cancel_instead = true;
      non_motion_cancel = participant->non_motion_transaction();
      // The read's borrow continues through disposal of its prepared product.
      participant->prepared_advance_in_flight = true;
      cancellation_target =
          CancellationTarget{participant->prepared_motion_engine, participant->payload_kind,
                             participant->subordinate_id};
    } else {
      participant->prepared_participant = participant_product;
      auto prepared_state = operation->state;
      const auto state_row =
          std::find_if(prepared_state.participants.begin(), prepared_state.participants.end(),
                       [&](const auto& row) {
                         return row.participant_id == participant->slot_owner.participant_id;
                       });
      if (state_row == prepared_state.participants.end()) {
        return reject<void>("prepared participant has no public operation-state row",
                            "operation_participant_state_missing", ErrorCode::kInvariantViolation);
      }
      state_row->state = OperationStateKind::kPrepared;
      prepared_state.state = aggregate_participant_state(prepared_state.participants);
      prepared_state.updated_at_ns = clock_.now();

      const bool all_prepared = std::all_of(
          operation->participants.begin(), operation->participants.end(),
          [](const auto& candidate) { return candidate.prepared_participant.has_value(); });
      if (!all_prepared) {
        auto durable = record_operation_state_locked(state_lock, prepared_state,
                                                     operation->runtime_scale_status.has_value()
                                                         ? &*operation->runtime_scale_status
                                                         : nullptr,
                                                     "coordinated_participant_prepared_waiting");
        if (!durable.ok()) {
          return Result<void>::Err(durable.error());
        }
        if (!durable.value()) {
          participant = operation->participant_for(engine);
          if (participant) participant->prepared_advance_in_flight = false;
          state_lock.unlock();
          commit.release();
          return discard_uncommitted_ordinary_preparations(operation_id);
        }
        operation->state = std::move(prepared_state);
      } else {
        prepared_state.state = OperationStateKind::kPrepared;
        for (auto& row : prepared_state.participants) {
          row.state = OperationStateKind::kPrepared;
        }
        prepared_state.updated_at_ns = clock_.now();
        std::vector<PreparedMotionDiagnosticsDTO> motion_diagnostics;
        motion_diagnostics.reserve(operation->participants.size());
        for (const auto& candidate : operation->participants) {
          if (candidate.payload_kind != OperationParticipantPayloadKind::kMotion) {
            continue;
          }
          if (!candidate.prepared_participant.has_value()) {
            return reject<void>("prepared barrier lost one motion participant product",
                                "prepared_motion_product_missing", ErrorCode::kInvariantViolation);
          }
          auto diagnostic = prepared_motion_diagnostics(*candidate.prepared_participant);
          if (!diagnostic.ok()) {
            return Result<void>::Err(diagnostic.error());
          }
          motion_diagnostics.push_back(std::move(diagnostic.value()));
        }
        std::sort(motion_diagnostics.begin(), motion_diagnostics.end(),
                  [](const auto& left, const auto& right) {
                    return left.participant_id < right.participant_id;
                  });
        auto durable = record_operation_state_locked(
            state_lock, prepared_state,
            operation->runtime_scale_status.has_value() ? &*operation->runtime_scale_status
                                                        : nullptr,
            operation->participants.size() == 1U ? (operation->participants.front().payload_kind ==
                                                            OperationParticipantPayloadKind::kMotion
                                                        ? "subordinate_motion_prepared"
                                                        : "subordinate_command_prepared")
                                                 : "coordinated_prepare_barrier_complete",
            motion_diagnostics);
        if (!durable.ok()) {
          return Result<void>::Err(durable.error());
        }
        if (!durable.value()) {
          participant = operation->participant_for(engine);
          if (participant) participant->prepared_advance_in_flight = false;
          state_lock.unlock();
          commit.release();
          return discard_uncommitted_ordinary_preparations(operation_id);
        }
        fence_held_prepared_program_commands_locked(operation->state.program_run_id);
        const auto cancelled_command = std::find_if(operation->participants.begin(),
            operation->participants.end(), [](const auto& candidate) {
              return candidate.non_motion_transaction() && candidate.stop_cancelled_pre_dispatch;
            });
        if (cancelled_command != operation->participants.end()) {
          auto* prepared_engine = cancelled_command->prepared_motion_engine;
          auto* cancelled_engine = cancelled_command->engine;
          const auto cancelled_id = cancelled_command->preparation_id;
          const auto resident = operation->commit_mutex;
          const bool own_cancel_claim = !cancelled_command->prepared_advance_in_flight;
          cancelled_command->prepared_advance_in_flight = true;
          state_lock.unlock();
          commit.release();
          const ScopeExit release_cancel([&] {
            if (!own_cancel_claim) return;
            std::lock_guard lock(state_mutex_);
            auto* current = find_active_operation_locked(operation_id);
            auto* row = current ? current->participant_for(cancelled_engine) : nullptr;
            if (row && current->commit_mutex == resident && row->preparation_id == cancelled_id) {
              row->prepared_advance_in_flight = false;
              settle_completed_control_locked(operation_id);
              wake_stop_after_preparation_borrow_locked(operation_id);
            }
          });
          const auto cancelled = prepared_engine->CancelPreparedCapabilityCommand(cancelled_id);
          if (!cancelled.ok()) return cancelled;
          const auto proof = confirm_prepared_command_no_effect(operation_id, cancelled_engine, cancelled_id);
          if (!proof.ok()) return Result<void>::Ok();
          if (!proof.value()) return Result<void>::Ok();
          return apply_subordinate_state(operation_id, cancelled_engine, cancelled_id,
              OperationStateKind::kCancelledPreDispatch, 0.0, std::nullopt);
        }
        // Final pre-dispatch recheck (spec 450 §5.2, spec 530 §11). The
        // prepared-state write above released state_mutex_, so attention is
        // evaluated only here, in the same lock hold that transitions the
        // slots and releases dispatch: an alarm published or a projection
        // aged out at any earlier instant is observed. Dispatch none; the
        // shared pre-dispatch path cancels every prepared participant.
        if (!operation->manual) {
          if (auto refusal = operator_alarm_attention_refusal_locked(
                  operation_resources(operation->participants))) {
            operation->state = std::move(prepared_state);
            participant = operation->participant_for(engine);
            if (participant != nullptr) participant->prepared_advance_in_flight = false;
            state_lock.unlock();
            commit.release();
            return fail_pre_dispatch(operation_id, nullptr, std::move(*refusal));
          }
        }

        std::vector<ResourceSlotReservationOwnerDTO> rows;
        rows.reserve(operation->participants.size());
        for (const auto& candidate : operation->participants) {
          rows.push_back({candidate.slot_owner, candidate.resources});
        }
        std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
          return std::tie(left.owner.operation_id, left.owner.participant_id) <
                 std::tie(right.owner.operation_id, right.owner.participant_id);
        });
        auto transitioned =
            operation->participants.size() == 1U
                ? resource_slots_->transition(operation->participants.front().slot_owner,
                                              operation->participants.front().resources,
                                              ResourceSlotState::kDispatched)
                : resource_slots_->transition_batch(rows, ResourceSlotState::kDispatched);
        if (!transitioned.ok()) {
          return transitioned;
        }
        operation->state = std::move(prepared_state);
        operation->coordinated_dispatch_barrier_released = true;
        dispatch_targets.reserve(operation->participants.size());
        for (auto& candidate : operation->participants) {
          if (!candidate.prepared_participant.has_value() ||
              candidate.prepared_motion_engine == nullptr || candidate.subordinate_id == 0U) {
            motion_admission_closed_ = true;
            return reject<void>("complete prepared barrier lost one participant product",
                                "coordinated_prepared_product_missing",
                                ErrorCode::kInvariantViolation);
          }
          const auto public_row =
              std::find_if(operation->state.participants.begin(),
                           operation->state.participants.end(), [&](const auto& row) {
                             return row.participant_id == candidate.slot_owner.participant_id;
                           });
          if (public_row == operation->state.participants.end()) {
            motion_admission_closed_ = true;
            return reject<void>("complete prepared barrier lost one public participant row",
                                "operation_participant_state_missing",
                                ErrorCode::kInvariantViolation);
          }
          candidate.slot_state = ResourceSlotState::kDispatched;
          dispatch_targets.push_back(DispatchTarget{
              candidate.slot_owner.participant_id, candidate.payload_kind,
              candidate.prepared_motion_engine, candidate.engine, candidate.subordinate_id,
              candidate.prepared_participant->prepared_contract_digest,
              public_row->controller_goal_id, public_row->controller_goal_generation});
        }
        std::sort(dispatch_targets.begin(), dispatch_targets.end(),
                  [](const auto& left, const auto& right) {
                    return left.participant_id < right.participant_id;
                  });
      }
    }
  }

  if (stop_wakeup.has_value()) {
    stop_control_executor_->Publish(*stop_wakeup);
    return Result<void>::Ok();
  }
  if (cancel_instead) {
    const auto cancelled =
        cancellation_target.has_value() && cancellation_target->prepared_engine != nullptr
            ? cancel_prepared_participant(*cancellation_target->prepared_engine,
                                          cancellation_target->payload_kind,
                                          cancellation_target->preparation_id)
            : reject<void>("prepared cancellation target is unavailable",
                           "prepared_cancellation_target_missing", ErrorCode::kInvariantViolation);
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
      if (participant != nullptr && participant->subordinate_id == preparation_id) {
        participant->prepared_advance_in_flight = false;
        wake_stop_after_preparation_borrow_locked(operation_id);
        if (cancelled.ok()) {
          settle_completed_control_locked(operation_id);
        } else {
          participant->last_subordinate_error = cancelled.error();
          motion_admission_closed_ = true;
        }
      }
    }
    if (!cancelled.ok()) return Result<void>::Err(cancelled.error());
    if (!non_motion_cancel) return Result<void>::Ok();
    const auto proof = confirm_prepared_command_no_effect(operation_id, engine, preparation_id);
    if (!proof.ok()) return Result<void>::Ok();
    if (!proof.value()) return Result<void>::Ok();
    return apply_subordinate_state(operation_id, engine, preparation_id,
        OperationStateKind::kCancelledPreDispatch, 0.0, std::nullopt);
  }

  std::vector<OperationParticipantId> attempted_participants;
  for (const auto& dispatch_target : dispatch_targets) {
    std::optional<StopIntentDTO> admission_stop;
    std::optional<AddressedParticipantStopSettlementRequestDTO> deferred_settlement;
    IResolvedParticipantExecutionEngine* deferred_settlement_engine{};
    bool cancelled_before_provider = false;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(dispatch_target.engine);
      if (participant == nullptr || participant->subordinate_id != dispatch_target.preparation_id) {
        continue;
      }
      if (participant->stop_owns_reconciliation()) continue;
      fence_held_prepared_program_commands_locked(operation->state.program_run_id);
      cancelled_before_provider = participant->non_motion_transaction() &&
                                  participant->stop_cancelled_pre_dispatch;
      const bool handover_stale =
          operation->manual_handover &&
          !validate_manual_program_handover_locked(*operation->manual_handover,
              registration_for(participant->binding)).ok();
      const bool admission_closed = motion_admission_closed_.restrictive() ||
          (motion_admission_closed_ && !participant->non_motion_transaction());
      if ((admission_closed || handover_stale) && participant->non_motion_transaction()) {
        participant->stop_cancelled_pre_dispatch = true;
        participant->last_subordinate_error = error(
            std::string(to_string(ErrorCode::kExecutionRejected)),
            "prepared command lost final dispatch admission",
            handover_stale ? "manual_handover_scope_stale" : "motion_admission_closed");
        cancelled_before_provider = true;
      } else if (admission_closed || handover_stale) {
        // Reuse exact Stop settlement for an admitted operation whose final
        // dispatch lost to a cell fault. Unreleased products are cancelled;
        // any already-released coordinated participant is physically stopped.
        admission_stop = StopIntentDTO{
            PublicRequestContextDTO{RequestId("stop.admission-closed." + operation_id.value()),
                                    config_.request_owner_epoch, "kernel.admission", std::nullopt},
            std::vector<StopOperationTargetDTO>{
                {operation_id, operation->state.operation_revision, StopGeneration(0U)}},
            std::string(handover_stale ? "manual_handover_scope_stale"
                                       : "motion_admission_closed")};
      } else if (!cancelled_before_provider) {
        participant->prepared_dispatch_released = true;
        participant->dispatch_effect_in_flight = true;
        attempted_participants.push_back(dispatch_target.participant_id);
      }
    }

    if (admission_stop.has_value()) {
      const auto contained = stop(*admission_stop);
      return contained.ok() ? Result<void>::Ok() : Result<void>::Err(contained.error());
    }

    if (cancelled_before_provider) {
      std::shared_ptr<std::mutex> resident;
      std::optional<ErrorDTO> cause;
      bool own_claim = false;
      {
        std::lock_guard lock(state_mutex_);
        auto* operation = find_active_operation_locked(operation_id);
        auto* participant = operation ? operation->participant_for(dispatch_target.engine) : nullptr;
        if (!participant || participant->preparation_id != dispatch_target.preparation_id ||
            participant->dispatch_effect_in_flight || participant->prepare_in_flight ||
            (participant->prepared_advance_in_flight && dispatch_target.engine != engine)) continue;
        resident = operation->commit_mutex;
        cause = participant->last_subordinate_error;
        own_claim = !participant->prepared_advance_in_flight;
        participant->prepared_advance_in_flight = true;
      }
      const ScopeExit release_cancel([&] {
        if (!own_claim) return;
        std::lock_guard lock(state_mutex_);
        auto* operation = find_active_operation_locked(operation_id);
        auto* participant = operation ? operation->participant_for(dispatch_target.engine) : nullptr;
        if (participant && operation->commit_mutex == resident &&
            participant->preparation_id == dispatch_target.preparation_id) {
          participant->prepared_advance_in_flight = false;
          settle_completed_control_locked(operation_id);
          wake_stop_after_preparation_borrow_locked(operation_id);
        }
      });
      const auto cancelled = dispatch_target.prepared_engine->CancelPreparedCapabilityCommand(
          dispatch_target.preparation_id);
      if (!cancelled.ok()) continue;
      const auto proof = confirm_prepared_command_no_effect(operation_id, dispatch_target.engine,
                                                            dispatch_target.preparation_id);
      if (!proof.ok() || !proof.value()) continue;
      const auto applied = apply_subordinate_state(operation_id, dispatch_target.engine,
          dispatch_target.preparation_id,
          cause ? OperationStateKind::kInterventionRequired : OperationStateKind::kCancelledPreDispatch,
          0.0, cause);
      if (!applied.ok()) return applied;
      continue;
    }

    Result<void> dispatched = Result<void>::Ok();
    std::optional<GripperCommandResultDTO> command_result;
    if (dispatch_target.payload_kind == OperationParticipantPayloadKind::kMotion) {
      dispatched = dispatch_target.prepared_engine->DispatchPreparedMotion(
          dispatch_target.preparation_id, dispatch_target.contract_digest);
    } else {
      auto command = dispatch_target.prepared_engine->DispatchPreparedCapabilityCommand(
          dispatch_target.preparation_id, dispatch_target.contract_digest,
          dispatch_target.controller_goal_id, dispatch_target.controller_goal_generation);
      if (!command.ok()) {
        dispatched = Result<void>::Err(command.error());
      } else if (!command.value().accepted || command.value().error.has_value() ||
                 command.value().command_id == 0U || command.value().generation == 0U) {
        dispatched = Result<void>::Err(command.value().error.value_or(
            error(std::string(to_string(ErrorCode::kExecutionRejected)),
                  "prepared capability command returned an invalid acceptance",
                  "subordinate_command_result_invalid")));
      } else {
        command_result = command.value();
      }
    }
    {
      // Returning from the provider ends the dispatch borrow, independently of
      // ordinary journal publication. Stop must be able to settle even when a
      // sibling participant's ordinary capture is blocked in storage.
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(dispatch_target.engine);
      if (participant != nullptr && participant->subordinate_id == dispatch_target.preparation_id) {
        participant->dispatch_effect_in_flight = false;
        if (participant->deferred_stop_settlement.has_value()) {
          deferred_settlement = std::move(participant->deferred_stop_settlement);
          participant->deferred_stop_settlement.reset();
          deferred_settlement_engine = participant->engine;
        }
        if (!dispatched.ok()) {
          participant->last_subordinate_error = dispatched.error();
        } else if (command_result.has_value()) {
          participant->subordinate_id = command_result->command_id;
          participant->subordinate_generation = command_result->generation;
        }
        if (participant->stop_owns_reconciliation() && operation->state.active_stop_id.has_value()) {
          const auto transaction = std::find_if(
              stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
                return candidate && candidate->active &&
                       candidate->stop_id == operation->state.active_stop_id &&
                       candidate->stop_generation == operation->state.active_stop_generation;
              });
          if (transaction != stop_transactions_.end()) {
            stop_wakeup = (*transaction)->token;
            stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
          }
        }
      }
    }
    if (stop_wakeup.has_value()) {
      stop_control_executor_->Publish(*stop_wakeup);
    }
    if (deferred_settlement.has_value()) {
      settle_deferred_capability_stop(operation_id, dispatch_target.engine,
                                      deferred_settlement_engine, *deferred_settlement);
    }
    if (command_result.has_value()) {
      OperationCommitGuard commit(*this, operation_id);
      std::unique_lock state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(dispatch_target.engine);
      if (participant != nullptr && !participant->stop_owns_reconciliation() &&
          participant->preparation_id == dispatch_target.preparation_id &&
          participant->subordinate_id == command_result->command_id &&
          participant->subordinate_generation == command_result->generation) {
        auto executing_state = operation->state;
        const auto public_row = std::find_if(
            executing_state.participants.begin(), executing_state.participants.end(),
            [&](const auto& row) { return row.participant_id == dispatch_target.participant_id; });
        if (public_row == executing_state.participants.end()) {
          dispatched =
              reject<void>("dispatched command lost its public participant row",
                           "operation_participant_state_missing", ErrorCode::kInvariantViolation);
        } else if (public_row->state == OperationStateKind::kPrepared ||
                   public_row->state == OperationStateKind::kDispatching) {
          // Reconciliation or Stop may already have advanced this participant
          // while we waited for the ordinary gate. Never restore an older state.
          public_row->state = OperationStateKind::kExecuting;
          if (operation->state.state != OperationStateKind::kInterventionRequired)
            executing_state.state = aggregate_participant_state(executing_state.participants);
          executing_state.updated_at_ns = clock_.now();
          auto durable = record_operation_state_locked(state_lock, executing_state,
                                                       operation->runtime_scale_status.has_value()
                                                           ? &*operation->runtime_scale_status
                                                           : nullptr,
                                                       "subordinate_command_admitted");
          if (!durable.ok()) {
            dispatched = Result<void>::Err(durable.error());
          } else if (durable.value()) {
            operation->state = std::move(executing_state);
          }
        }
        if (!dispatched.ok()) participant->last_subordinate_error = dispatched.error();
      }
    }
    if (dispatched.ok()) {
      continue;
    }
    bool held_no_effect = false;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation ? operation->participant_for(dispatch_target.engine) : nullptr;
      held_no_effect = participant && participant->non_motion_transaction() &&
                       participant->stop_cancelled_pre_dispatch;
      // prepared_dispatch_released is historical Kernel permission, not engine commitment.
    }
    if (held_no_effect) {
      const auto proof = confirm_prepared_command_no_effect(operation_id, dispatch_target.engine,
                                                            dispatch_target.preparation_id);
      if (!proof.ok()) continue;
      if (!proof.value()) continue;  // Unknown owner outcome retains its actual reservation.
      const auto applied = apply_subordinate_state(operation_id, dispatch_target.engine,
          dispatch_target.preparation_id, OperationStateKind::kCancelledPreDispatch, 0.0, std::nullopt);
      if (!applied.ok()) return applied;
      continue;
    }
    bool pneumatic_failure = false;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto* current = find_active_operation_locked(operation_id);
      const auto* participant = current ? current->participant_for(dispatch_target.engine) : nullptr;
      pneumatic_failure = participant && participant->finishes_current_gripper_transaction();
    }
    if (pneumatic_failure) {
      auto failed = apply_subordinate_state(operation_id, dispatch_target.engine,
          dispatch_target.preparation_id, OperationStateKind::kFailed, 0.0, dispatched.error());
      if (!failed.ok()) return failed;
      continue;  // DX778: ordinary final Failure, not coordinated containment/fault.
    }
    if (dispatch_targets.size() == 1U) {
      return apply_subordinate_state(
          operation_id, dispatch_target.engine, dispatch_target.preparation_id,
          OperationStateKind::kInterventionRequired, 0.0, dispatched.error());
    }

    const ErrorDTO partial_failure{
        std::string(to_string(ErrorCode::kExecutionRejected)),
        "coordinated dispatch failed after the barrier released physical work",
        {{"reason", "operation_partial_dispatch"},
         {"backend_reason", dispatched.error().details.contains("reason")
                                ? dispatched.error().details.at("reason")
                                : dispatched.error().code}}};
    std::vector<CancellationTarget> cancellations;
    std::vector<ResourceSlotReservationOwnerDTO> undispatched_rows;
    OperationRevision current_revision(0U);
    std::optional<OperationStateDTO> partial_state;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      if (operation == nullptr || operation->state.active_stop_id.has_value()) {
        return Result<void>::Ok();
      }
      current_revision = operation->state.operation_revision;
      for (auto& candidate : operation->participants) {
        const bool attempted =
            std::find(attempted_participants.begin(), attempted_participants.end(),
                      candidate.slot_owner.participant_id) != attempted_participants.end();
        auto state_row =
            std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                         [&](const auto& row) {
                           return row.participant_id == candidate.slot_owner.participant_id;
                         });
        if (state_row == operation->state.participants.end()) {
          motion_admission_closed_ = true;
          continue;
        }
        if (attempted) {
          state_row->state = OperationStateKind::kDispatching;
          if (candidate.slot_owner.participant_id == dispatch_target.participant_id) {
            state_row->terminal_error = partial_failure;
          }
          continue;
        }
        candidate.stop_cancelled_pre_dispatch = true;
        state_row->state = OperationStateKind::kCancelledPreDispatch;
        state_row->terminal_error.reset();
        undispatched_rows.push_back({candidate.slot_owner, candidate.resources});
        if (candidate.prepared_motion_engine != nullptr && candidate.subordinate_id != 0U) {
          cancellations.push_back(CancellationTarget{
              candidate.prepared_motion_engine, candidate.payload_kind, candidate.subordinate_id});
        }
      }
      operation->state.state = OperationStateKind::kDispatching;
      operation->state.updated_at_ns = clock_.now();
      operation->coordinated_cancellation_in_flight = !cancellations.empty();
      partial_state = operation->state;
    }
    std::sort(attempted_participants.begin(), attempted_participants.end());
    const PartialDispatchStopScope partial_scope{operation_id, current_revision,
                                                 attempted_participants};
    StopIntentDTO containment{
        PublicRequestContextDTO{RequestId("stop.partial-dispatch." + operation_id.value()),
                                config_.request_owner_epoch, "kernel.partial_dispatch",
                                std::nullopt},
        std::vector<StopOperationTargetDTO>{
            StopOperationTargetDTO{operation_id, current_revision, StopGeneration(0U)}},
        std::string("operation_partial_dispatch")};
    auto stopped =
        accept_stop(EmergencyStopProjectedIntentDTO{std::move(containment)}, &partial_scope);
    if (!stopped.ok()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
    }

    std::sort(undispatched_rows.begin(), undispatched_rows.end(),
              [](const auto& left, const auto& right) {
                return std::tie(left.owner.operation_id, left.owner.participant_id) <
                       std::tie(right.owner.operation_id, right.owner.participant_id);
              });
    if (!undispatched_rows.empty()) {
      std::lock_guard state_lock(state_mutex_);
      auto terminalized =
          resource_slots_->transition_batch(undispatched_rows, ResourceSlotState::kTerminal);
      auto released = terminalized.ok() ? resource_slots_->release_batch(undispatched_rows)
                                        : Result<void>::Err(terminalized.error());
      if (!released.ok()) {
        motion_admission_closed_ = true;
      } else if (auto* operation = find_active_operation_locked(operation_id);
                 operation != nullptr) {
        for (auto& candidate : operation->participants) {
          if (std::find(attempted_participants.begin(), attempted_participants.end(),
                        candidate.slot_owner.participant_id) == attempted_participants.end()) {
            candidate.slot_state = ResourceSlotState::kTerminal;
          }
        }
      }
    }
    for (const auto& cancellation : cancellations) {
      if (!cancel_prepared_participant(*cancellation.prepared_engine, cancellation.payload_kind,
                                       cancellation.preparation_id)
               .ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    }
    {
      std::lock_guard state_lock(state_mutex_);
      if (auto* operation = find_active_operation_locked(operation_id); operation != nullptr) {
        stamp_operation_capture_locked(*operation, operation->state);
        partial_state = operation->state;
      }
    }
    if (!partial_state.has_value()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return Result<void>::Ok();
    }
    const auto durable = attempt_journal_write([&] {
      return journal_.RecordOperationState(config_.request_owner_epoch, *partial_state, nullptr,
                                           "operation_partial_dispatch");
    });
    {
      std::lock_guard state_lock(state_mutex_);
      if (!durable.ok()) motion_admission_closed_ = true;
      if (auto* operation = find_active_operation_locked(operation_id); operation != nullptr) {
        operation->coordinated_cancellation_in_flight = false;
        settle_completed_control_locked(operation_id);
      }
    }
    return Result<void>::Ok();
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::advance_prepared_capability_command(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine) {
  std::optional<std::uint64_t> cancelled_preparation;
  IResolvedPreparedParticipantExecutionEngine* cancelled_engine{};
  std::shared_ptr<std::mutex> cancelled_resident;
  std::optional<ErrorDTO> cancellation_cause;
  {
    std::lock_guard lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation ? operation->participant_for(engine) : nullptr;
    if (participant && participant->non_motion_transaction() && participant->stop_cancelled_pre_dispatch &&
        participant->preparation_id && !participant->prepare_in_flight && !participant->dispatch_effect_in_flight &&
        !participant->prepared_advance_in_flight && !participant->reconciliation_in_flight &&
        !operation->pending_control && !operation->recovery_effect_in_flight &&
        participant->prepared_motion_engine) {
      cancelled_preparation = participant->preparation_id;
      cancelled_engine = participant->prepared_motion_engine;
      cancelled_resident = operation->commit_mutex;
      participant->prepared_advance_in_flight = true;
    }
  }
  if (cancelled_preparation) {
    const auto cancelled = cancelled_engine->CancelPreparedCapabilityCommand(*cancelled_preparation);
    const auto proof = cancelled.ok()
        ? confirm_prepared_command_no_effect(operation_id, engine, *cancelled_preparation)
        : Result<bool>::Ok(false);
    bool current = false;
    {
      std::lock_guard lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation ? operation->participant_for(engine) : nullptr;
      current = participant && operation->commit_mutex == cancelled_resident &&
                participant->preparation_id == *cancelled_preparation &&
                participant->prepared_motion_engine == cancelled_engine;
      if (current) {
        participant->prepared_advance_in_flight = false;
        if (!cancelled.ok()) participant->last_subordinate_error = cancelled.error();
        cancellation_cause = participant->last_subordinate_error;
        settle_completed_control_locked(operation_id);
        wake_stop_after_preparation_borrow_locked(operation_id);
      }
    }
    if (!current || !cancelled.ok() || !proof.ok() || !proof.value()) return Result<void>::Ok();
    return apply_subordinate_state(operation_id, engine, *cancelled_preparation,
        cancellation_cause ? OperationStateKind::kInterventionRequired : OperationStateKind::kCancelledPreDispatch,
        0.0, cancellation_cause);
  }
  struct Target final {
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t preparation_id{};
    RuntimeParticipantBinding binding;
    RuntimeParticipantStopReadiness readiness;
    std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
    MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
    std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
  };

  std::optional<Target> target;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant == nullptr ||
        participant->payload_kind != OperationParticipantPayloadKind::kCapabilityCommand ||
        operation->recovery_effect_in_flight || participant->prepared_motion_engine == nullptr ||
        participant->subordinate_id == 0U || participant->prepared_dispatch_released ||
        participant->prepared_advance_in_flight || participant->prepared_participant.has_value() ||
        operation->coordinated_dispatch_barrier_released ||
        (terminal(operation->state.state) && !operation->state.active_stop_id.has_value())) {
      return Result<void>::Ok();
    }
    const auto state_row =
        std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                     [&](const auto& row) {
                       return row.participant_id == participant->slot_owner.participant_id;
                     });
    if (state_row == operation->state.participants.end()) {
      return reject<void>("prepared command has no public participant row",
                          "operation_participant_state_missing", ErrorCode::kInvariantViolation);
    }
    participant->prepared_advance_in_flight = true;
    target.emplace(Target{participant->prepared_motion_engine, participant->engine,
                          participant->subordinate_id, participant->binding,
                          participant->stop_readiness, participant->retained_default_stop_limits,
                          participant->stop_node_category, participant->ordinary_stop_plan});
  }

  const auto release_claim = [&]() {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(target->engine);
    if (participant != nullptr && participant->subordinate_id == target->preparation_id) {
      participant->prepared_advance_in_flight = false;
      settle_completed_control_locked(operation_id);
      wake_stop_after_preparation_borrow_locked(operation_id);
    }
  };

  auto snapshot = target->prepared_engine->ObservePreparedCapabilityCommand(target->preparation_id);
  if (!snapshot.ok()) {
    release_claim();
    return Result<void>::Err(snapshot.error());
  }
  if (snapshot.value().preparation_id != target->preparation_id) {
    release_claim();
    return reject<void>("prepared command observation changed identity",
                        "prepared_command_observation_identity_mismatch",
                        ErrorCode::kInvariantViolation);
  }
  if (snapshot.value().state == ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed ||
      snapshot.value().state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch) {
    const bool cancelled =
        snapshot.value().state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
    const auto failure = snapshot.value().error.value_or(error(
        std::string(to_string(cancelled ? ErrorCode::kInvalidStateTransition
                                        : ErrorCode::kExecutionRejected)),
        cancelled ? "prepared command was cancelled before dispatch"
                  : "prepared command failed before dispatch",
        cancelled ? "prepared_command_cancelled_pre_dispatch" : "prepared_command_prepare_failed"));
    bool stop_owns_terminal = false;
    bool coordinated = false;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(target->engine);
      if (participant != nullptr) {
        participant->prepared_advance_in_flight = false;
        participant->last_subordinate_error = failure;
        stop_owns_terminal = participant->stop_owns_reconciliation();
        coordinated =
            operation->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll;
        if (stop_owns_terminal) {
          settle_completed_control_locked(operation_id);
          wake_stop_after_preparation_borrow_locked(operation_id);
        }
      }
    }
    if (stop_owns_terminal) {
      return Result<void>::Ok();
    }
    if (coordinated) {
      return fail_pre_dispatch(operation_id, target->engine, failure);
    }
    if (cancelled && target->readiness.value().resolved_stop_base_plan.gripper_actuation_kind ==
                         GripperActuationKind::kPneumatic) {
      const auto proof = confirm_prepared_command_no_effect(operation_id, target->engine, target->preparation_id);
      if (!proof.ok()) return Result<void>::Ok();
      if (!proof.value()) return Result<void>::Ok();
    }
    return apply_subordinate_state(
        operation_id, target->engine, target->preparation_id,
        cancelled ? (target->readiness.value().resolved_stop_base_plan.gripper_actuation_kind ==
                         GripperActuationKind::kPneumatic
                         ? OperationStateKind::kCancelledPreDispatch : OperationStateKind::kStopped)
                  : OperationStateKind::kInterventionRequired, 0.0,
        cancelled ? std::optional<ErrorDTO>{} : std::optional(failure));
  }
  if (snapshot.value().state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared ||
      !snapshot.value().participant || !snapshot.value().backend_prepare_result) {
    release_claim();
    return reject<void>("prepared command observation has an invalid shape",
                        "prepared_command_observation_invalid", ErrorCode::kInvariantViolation);
  }
  const auto verified = VerifyPreparedOperationParticipant(
      *snapshot.value().participant, target->binding, target->readiness,
      target->retained_default_stop_limits, target->stop_node_category);
  if (!verified.ok() ||
      snapshot.value().participant->ordinary_stop_plan != *target->ordinary_stop_plan ||
      prepare_participant_result_field_error(*snapshot.value().backend_prepare_result,
                                             *snapshot.value().participant)
          .has_value()) {
    release_claim();
    return verified.ok()
               ? reject<void>("prepared command differs from owner authority",
                              "prepared_command_product_mismatch", ErrorCode::kInvariantViolation)
               : Result<void>::Err(verified.error());
  }

  return release_prepared_dispatch_barrier(operation_id, target->engine, target->preparation_id,
                                           *snapshot.value().participant);
}

Result<void> ResourceAddressedExecutionKernel::apply_subordinate_state(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine,
    std::uint64_t subordinate_id, OperationStateKind state, double progress,
    std::optional<ErrorDTO> terminal_error, const bool observe_current_motion) {
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant != nullptr && participant->stop_owns_reconciliation()) {
      // The exact Stop certificate owns this participant's terminal. A
      // synchronous Stop callback must never wait behind ordinary storage.
      // Unaffected siblings of a partial Stop still follow the ordinary path.
      if (terminal_error.has_value() && participant->subordinate_id == subordinate_id)
        participant->last_subordinate_error = terminal_error;
      return Result<void>::Ok();
    }
  }
  OperationCommitGuard commit(*this, operation_id);
  std::optional<std::uint64_t> sampled_command_generation;
  std::optional<std::string> sampled_command_target;
  if (observe_current_motion) {
    // Callbacks and polling share this observation path under one writer gate.
    OperationParticipantPayloadKind payload_kind;
    std::uint64_t expected_generation{};
    std::string command_target;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto* operation = find_active_operation_locked(operation_id);
      const auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
      if (participant == nullptr || participant->stop_owns_reconciliation() ||
          (participant->payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
           participant->dispatch_effect_in_flight) || participant->subordinate_id != subordinate_id)
        return Result<void>::Ok();
      payload_kind = participant->payload_kind;
      expected_generation = participant->subordinate_generation;
      command_target = participant->binding.value().command_target_id.value();
    }
    if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
      const auto current = engine->get_gripper_status(command_target);
      if (current.gripper_id != command_target ||
          (current.state != GripperCommandState::kIdle &&
           (current.command_id != subordinate_id || current.generation != expected_generation)))
        return Result<void>::Ok();
      sampled_command_generation = expected_generation;
      sampled_command_target = command_target;
      progress = current.progress;
      terminal_error.reset();
      switch (current.state) {
        case GripperCommandState::kIdle:
          state = OperationStateKind::kInterventionRequired;
          terminal_error = error("invariant_violation",
              "accepted gripper command became idle without terminal evidence", "gripper_command_lost");
          break;
        case GripperCommandState::kExecuting: state = OperationStateKind::kExecuting; break;
        case GripperCommandState::kCompleted: state = OperationStateKind::kCompleted; break;
        case GripperCommandState::kFailed:
          state = OperationStateKind::kFailed;
          terminal_error = current.error.value_or(error("execution_aborted", "gripper command failed",
                                                       "gripper_command_failed_without_error"));
          break;
      }
    } else {
      const auto current = engine->get_status();
      if (current.execution_id != subordinate_id) return Result<void>::Ok();
      const auto observed = participant_operation_state(current);
      if (observed != state) terminal_error.reset();
      state = observed;
      progress = current.progress;
      if (state != OperationStateKind::kFailed && state != OperationStateKind::kStopped &&
          state != OperationStateKind::kInterventionRequired && state != OperationStateKind::kInterrupted) {
        terminal_error.reset();
      } else if (const auto fact_error = participant_fact_error(current); fact_error.has_value()) {
        terminal_error = fact_error;
      } else if (!terminal_error) {
        terminal_error = status_error(current.last_error);
      }
    }
  }
  std::unique_lock state_lock(state_mutex_);
  auto* active_operation = find_active_operation_locked(operation_id);
  auto* active_participant =
      active_operation == nullptr ? nullptr : active_operation->participant_for(engine);
  if (active_participant == nullptr || active_operation->recovery_effect_in_flight ||
      active_operation->coordinated_cancellation_in_flight) {
    return Result<void>::Ok();
  }
  if (sampled_command_generation.has_value() &&
      (active_participant->subordinate_id != subordinate_id ||
       active_participant->subordinate_generation != *sampled_command_generation ||
       active_participant->binding.value().command_target_id.value() != *sampled_command_target ||
       active_participant->dispatch_effect_in_flight)) return Result<void>::Ok();
  // Stop may have won while this ordinary callback waited for its writer
  // gate or sampled the subordinate. Its certificate still owns settlement.
  if (active_participant->stop_owns_reconciliation()) {
    return Result<void>::Ok();
  }
  if (state == OperationStateKind::kPrepared && active_operation->pending_control.has_value() &&
      (active_operation->pending_control->accepted.control == OperationControlKind::kReset ||
       active_operation->pending_control->accepted.control ==
           OperationControlKind::kAcknowledgeCompleted)) {
    // The subordinate Idle transition confirms the settling effect. The
    // durable owner retains the terminal state named by the accepted control;
    // it does not republish that private FSM reset as a new active operation.
    return Result<void>::Ok();
  }
  if (active_participant->payload_kind == OperationParticipantPayloadKind::kMotion) {
    if (subordinate_id == 0U) {
      return reject<void>("subordinate execution identity is empty",
                          "subordinate_execution_id_empty", ErrorCode::kInvariantViolation);
    }
    // The execute result establishes event ownership. Events observed before
    // that result are ignored rather than guessed from an engine-global slot;
    // reconcile_subordinate_state() closes the resulting observation window.
    if (active_participant->subordinate_id == 0U ||
        active_participant->subordinate_id != subordinate_id) {
      return Result<void>::Ok();
    }
  }

  auto desired_slot = slot_state_for(state);
  if (active_participant->finishes_current_gripper_transaction() &&
      active_participant->slot_state == ResourceSlotState::kStopping &&
      desired_slot == ResourceSlotState::kDispatched) desired_slot.reset();
  const auto desired_slot_value = desired_slot.value_or(active_participant->slot_state);
  const bool slot_changed =
      desired_slot.has_value() && active_participant->slot_state != desired_slot_value;
  const auto participant_iterator = std::find_if(
      active_operation->state.participants.begin(), active_operation->state.participants.end(),
      [&](const auto& participant) {
        return participant.participant_id == active_participant->slot_owner.participant_id;
      });
  if (participant_iterator == active_operation->state.participants.end()) {
    return reject<void>("active participant has no public operation-state row",
                        "operation_participant_state_missing", ErrorCode::kInvariantViolation);
  }
  const auto& participant = *participant_iterator;
  if (active_participant->non_motion_transaction() && terminal(participant.state) &&
      state != participant.state) return Result<void>::Ok();
  if (terminal(active_operation->state.state) && state != active_operation->state.state &&
      !(active_participant->non_motion_transaction() && !terminal(participant.state) &&
        active_participant->prepared_dispatch_released && !active_participant->dispatch_effect_in_flight &&
        (state == OperationStateKind::kCompleted || state == OperationStateKind::kFailed)) &&
      !(state == OperationStateKind::kCancelledPreDispatch && !terminal(participant.state) &&
        active_participant->stop_cancelled_pre_dispatch && active_participant->subordinate_generation == 0U &&
        !active_participant->dispatch_effect_in_flight &&
        (!active_participant->prepared_dispatch_released || active_participant->prepared_engine_no_effect_confirmed)))
    return Result<void>::Ok();
  if (active_participant->finishes_current_gripper_transaction() &&
      participant.state == OperationStateKind::kFailed && participant.terminal_error &&
      participant.terminal_error->details.contains("pneumatic_transaction_final") &&
      participant.terminal_error->details.at("pneumatic_transaction_final") == "true") {
    // Finality is per owner row even while a coordinated sibling is active.
    // Late provider Success cannot rewrite the first terminal transaction.
    return Result<void>::Ok();
  }
  if (!std::isfinite(progress)) {
    return reject<void>("subordinate progress is not finite", "subordinate_progress_invalid",
                        ErrorCode::kInvariantViolation);
  }
  const double bounded_progress = std::clamp(progress, 0.0, 1.0);
  const bool failure_terminal = state == OperationStateKind::kFailed ||
                                state == OperationStateKind::kStopped ||
                                state == OperationStateKind::kInterrupted ||
                                state == OperationStateKind::kInterventionRequired;
  if (failure_terminal) {
    const bool generic_subordinate_error =
        terminal_error.has_value() && terminal_error->details.contains("reason") &&
        terminal_error->details.at("reason") == "subordinate_terminal_error";
    if (!active_participant->finishes_current_gripper_transaction() &&
        active_operation->state.active_stop_id.has_value() &&
        participant.terminal_error.has_value()) {
      // Stop feedback cannot erase a cleanup fault already owned by this
      // participant. Its containment evidence has a separate certificate.
      terminal_error = participant.terminal_error;
    } else if ((!terminal_error.has_value() || generic_subordinate_error) &&
               active_participant->last_subordinate_error.has_value()) {
      terminal_error = active_participant->last_subordinate_error;
    }
  }
  if (state == OperationStateKind::kFailed &&
      active_participant->finishes_current_gripper_transaction()) {
    if (!terminal_error) terminal_error = error("execution_aborted", "Pneumatic delivery failed",
                                                "pneumatic_delivery_not_confirmed");
    // Normalize the FINAL selected error after retained/provider fallbacks.
    // Only the captured typed target mode grants this Kernel owner fact.
    terminal_error->details["pneumatic_transaction_final"] = "true";
  } else if (terminal_error) {
    // This is an owner fact, never authority supplied by another provider.
    terminal_error->details.erase("pneumatic_transaction_final");
  }
  const bool same_terminal_error =
      participant.terminal_error.has_value() == terminal_error.has_value() &&
      (!terminal_error.has_value() ||
       (participant.terminal_error->code == terminal_error->code &&
        participant.terminal_error->message == terminal_error->message &&
        participant.terminal_error->details == terminal_error->details));
  if (!slot_changed && participant.state == state && (!terminal(state) || same_terminal_error)) {
    // Progress is current presentation, not a durable lifecycle transition.
    // Retain the latest scalar without writing a full operation snapshot on
    // every sample. A later transition records its complete current state.
    if (participant.progress != bounded_progress) {
      participant_iterator->progress = bounded_progress;
      active_operation->state.updated_at_ns = clock_.now();
    }
    return Result<void>::Ok();
  }

  auto observed_state = active_operation->state;
  auto& observed_participant = *std::find_if(
      observed_state.participants.begin(), observed_state.participants.end(),
      [&](const auto& candidate) {
        return candidate.participant_id == active_participant->slot_owner.participant_id;
      });
  observed_participant.state = state;
  observed_participant.progress = bounded_progress;
  if (terminal(state)) {
    observed_participant.terminal_error = std::move(terminal_error);
  }
  // An owed ordinary result changes its row, not an already latched
  // operation-level invariant fault which has no participant reducer cause.
  if (active_operation->state.state != OperationStateKind::kInterventionRequired)
    observed_state.state = aggregate_participant_state(observed_state.participants);
  observed_state.updated_at_ns = clock_.now();
  auto durable_state = record_operation_state_locked(
      state_lock, observed_state,
      active_operation->runtime_scale_status.has_value() ? &*active_operation->runtime_scale_status
                                                         : nullptr,
      "subordinate_state_observed");
  if (!durable_state.ok()) {
    return Result<void>::Err(durable_state.error());
  }
  if (!durable_state.value()) return Result<void>::Ok();
  if (slot_changed) {
    auto transition = resource_slots_->transition(
        active_participant->slot_owner, active_participant->resources, desired_slot_value);
    if (!transition.ok()) {
      auto intervention = observed_state;
      intervention.state = OperationStateKind::kInterventionRequired;
      auto& failed_participant = *std::find_if(
          intervention.participants.begin(), intervention.participants.end(),
          [&](const auto& candidate) {
            return candidate.participant_id == active_participant->slot_owner.participant_id;
          });
      failed_participant.state = OperationStateKind::kInterventionRequired;
      failed_participant.terminal_error = transition.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          record_operation_state_locked(state_lock, intervention,
                                        active_operation->runtime_scale_status.has_value()
                                            ? &*active_operation->runtime_scale_status
                                            : nullptr,
                                        "resource_transition_failed_after_state_observation");
      if (durable_failure.ok() && durable_failure.value()) {
        active_operation->state = std::move(intervention);
      }
      return transition;
    }
    active_participant->slot_state = desired_slot_value;
  }
  active_operation->state = std::move(observed_state);
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::on_engine_event(IResolvedParticipantExecutionEngine* engine,
                                                       const KernelEvent& event) {
  if (event.type == KernelEventType::kCollisionDetected) {
    std::optional<StopIntentDTO> containment;
    std::optional<StopControlWorkToken> escalation_wakeup;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto operation_id = find_event_operation_locked(engine, event.execution_id);
      auto* operation =
          operation_id.has_value() ? find_active_operation_locked(*operation_id) : nullptr;
      auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
      if (operation != nullptr && participant != nullptr && !terminal(operation->state.state)) {
        auto collision_details = event.details;
        if (const auto reason = collision_details.find("reason"); reason != collision_details.end()) {
          collision_details["runtime_collision_reason"] = reason->second;
        }
        collision_details["reason"] = "runtime_collision_fault";
        ErrorDTO collision_error{std::string(to_string(ErrorCode::kExecutionAborted)),
                                 "Runtime collision containment stopped the operation",
                                 std::move(collision_details)};
        // A collision fact during an active Stop is escalation input for that
        // Stop's participant work, never dropped and never a second Stop.
        bool stop_owns_collision = false;
        if (operation->state.active_stop_id.has_value() &&
            operation->state.active_stop_generation.has_value()) {
          const auto transaction = std::find_if(
              stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
                return candidate != nullptr && candidate->active && !candidate->terminal &&
                       candidate->stop_id == operation->state.active_stop_id &&
                       candidate->stop_generation == *operation->state.active_stop_generation;
              });
          if (transaction != stop_transactions_.end()) {
            const auto work = std::find_if(
                (*transaction)->participants.begin(), (*transaction)->participants.end(),
                [&](const auto& candidate) {
                  return candidate.operation_id == operation->state.operation_id &&
                         candidate.plan.participant_id == participant->slot_owner.participant_id;
                });
            const auto contact = event.details.find("in_collision");
            if (contact != event.details.end() && contact->second == "true") {
              auto stop_error = std::move(collision_error);
              stop_error.message = "Collision detected on the Stop path; strongest Stop stage engaged";
              stop_error.details["reason"] = "stop_path_collision";
              stop_error.details["stop_id"] = operation->state.active_stop_id->value();
              if (work != (*transaction)->participants.end()) {
                if (!work->terminal) {
                  if (!work->collision_escalation_error.has_value()) {
                    work->collision_escalation_error = std::move(stop_error);
                  }
                  work->collision_escalation_pending = true;
                  escalation_wakeup = (*transaction)->token;
                }
              } else if (!(*transaction)->participant_work_initialized) {
                (*transaction)->pending_stop_path_collisions.emplace_back(
                    operation->state.operation_id, participant->slot_owner.participant_id,
                    std::move(stop_error));
                escalation_wakeup = (*transaction)->token;
              }
              if (escalation_wakeup.has_value()) {
                stop_control_executor_->ArmDeadline(*escalation_wakeup, clock_.now());
              }
            }
          }
          // During an active Stop a collision fact is escalation input (on
          // contact) or evidence; it never opens a second Stop, and after the
          // participant's containment is proven it cannot reopen it.
          stop_owns_collision = true;
        }
        if (!stop_owns_collision) {
          participant->last_subordinate_error = collision_error;
          participant->runtime_collision_error = collision_error;
          const auto material =
              config_.request_owner_epoch.value() + "\nruntime-collision\n" +
              operation->state.operation_id.value() + "\n" +
              std::to_string(operation->state.operation_revision.value()) + "\n" +
              participant->slot_owner.participant_id.value();
          containment = StopIntentDTO{
              PublicRequestContextDTO{
                  RequestId(manyforge::content_identity::sha256_digest(material)),
                  config_.request_owner_epoch, "kernel.runtime_collision",
                  participant->slot_owner.participant_id.value()},
              std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
                  operation->state.operation_id, operation->state.operation_revision,
                  operation->state.active_stop_generation.value_or(StopGeneration(0U))}},
              std::string("runtime_collision_fault"),
              RequestAttributionDTO{RequestTransportDTO::kInternal,
                                    RequestAssuranceDTO::kUnverified, std::nullopt}};
        }
      }
    }
    if (escalation_wakeup.has_value()) {
      stop_control_executor_->Publish(*escalation_wakeup);
    }
    if (containment.has_value()) {
      auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(*containment)});
      if (!stopped.ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    }
    return;
  }
  if (event.type == KernelEventType::kStopRequested) {
    // The participant held its motion at zero speed for the user-set hold
    // timeout (item 40). Like an operator Stop, the Kernel ends the operation
    // Stopped (no error) through its own Stop, which issues the lane's hard
    // stop; an operation already owned by a Stop or ended is left alone.
    const auto reason = event.details.find("reason");
    if (reason == event.details.end() || reason->second != kZeroSpeedHoldStopReason) {
      return;
    }
    std::optional<StopIntentDTO> hold_stop;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto operation_id = find_event_operation_locked(engine, event.execution_id);
      auto* operation =
          operation_id.has_value() ? find_active_operation_locked(*operation_id) : nullptr;
      auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
      if (operation != nullptr && participant != nullptr && !terminal(operation->state.state) &&
          !operation->state.active_stop_id.has_value()) {
        const auto material = config_.request_owner_epoch.value() + "\nzero-speed-hold\n" +
                              operation->state.operation_id.value() + "\n" +
                              std::to_string(operation->state.operation_revision.value()) + "\n" +
                              participant->slot_owner.participant_id.value();
        hold_stop = StopIntentDTO{
            PublicRequestContextDTO{
                RequestId(manyforge::content_identity::sha256_digest(material)),
                config_.request_owner_epoch, std::string(kZeroSpeedHoldStopSource),
                participant->slot_owner.participant_id.value()},
            std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
                operation->state.operation_id, operation->state.operation_revision,
                operation->state.active_stop_generation.value_or(StopGeneration(0U))}},
            std::string(kZeroSpeedHoldStopReason),
            RequestAttributionDTO{RequestTransportDTO::kInternal,
                                  RequestAssuranceDTO::kUnverified, std::nullopt}};
      }
    }
    if (hold_stop.has_value()) {
      auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(*hold_stop)});
      if (!stopped.ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    }
    return;
  }
  if (event.type == KernelEventType::kParticipantPrepared) {
    std::optional<OperationId> operation_id;
    {
      std::lock_guard state_lock(state_mutex_);
      operation_id = find_event_operation_locked(engine, event.execution_id);
    }
    if (operation_id.has_value()) {
      (void)advance_prepared_motion(*operation_id, engine);
    }
    return;
  }
  if (event.type == KernelEventType::kErrorRaised) {
    if (!event.error.has_value()) {
      return;
    }
    std::optional<OperationId> operation_id;
    std::optional<StopIntentDTO> containment;
    {
      std::lock_guard state_lock(state_mutex_);
      operation_id = find_event_operation_locked(engine, event.execution_id);
      if (operation_id.has_value()) {
        auto* active_operation = find_active_operation_locked(*operation_id);
        if (active_operation != nullptr) {
          auto* participant = active_operation->participant_for(engine);
          if (participant != nullptr) {
            participant->last_subordinate_error = event.error;
            const auto reason = event.error->details.find("reason");
            const bool invalid_prepared_dispatch =
                reason != event.error->details.end() &&
                reason->second.starts_with("prepared_execution_handle_");
            if (invalid_prepared_dispatch && participant->prepared_dispatch_released &&
                !active_operation->state.active_stop_id.has_value()) {
              const auto material =
                  config_.request_owner_epoch.value() + "\nprepared-dispatch-handle\n" +
                  active_operation->state.operation_id.value() + "\n" +
                  std::to_string(active_operation->state.operation_revision.value());
              containment = StopIntentDTO{
                  PublicRequestContextDTO{
                      RequestId(manyforge::content_identity::sha256_digest(material)),
                      config_.request_owner_epoch, "kernel.prepared_dispatch_validation",
                      std::nullopt},
                  std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
                      active_operation->state.operation_id,
                      active_operation->state.operation_revision,
                      active_operation->state.active_stop_generation.value_or(StopGeneration(0U))}},
                  std::string("prepared_dispatch_membership_invalid"),
                  RequestAttributionDTO{RequestTransportDTO::kInternal,
                                        RequestAssuranceDTO::kUnverified, std::nullopt}};
            }
          }
        }
      }
    }
    if (containment.has_value()) {
      auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(*containment)});
      if (!stopped.ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    }
    if (operation_id.has_value()) {
      (void)advance_prepared_motion(*operation_id, engine);
    }
    return;
  }
  if (event.type != KernelEventType::kStateTransition &&
      event.type != KernelEventType::kExecuteStarted &&
      event.type != KernelEventType::kParticipantSettled) {
    return;
  }
  if (event.type == KernelEventType::kParticipantSettled && !event.participant_fact.has_value()) {
    return;
  }
  // Participants report working phases and facts; the outcome of a fact is
  // committed here, never by the participant.
  auto state = event.type == KernelEventType::kExecuteStarted ? OperationStateKind::kExecuting
               : event.type == KernelEventType::kParticipantSettled
                   ? participant_outcome(*event.participant_fact)
                   : operation_state(event.to_state);
  std::optional<OperationId> operation_id;
  std::optional<StopControlWorkToken> stop_wakeup;
  bool aggregate_stop_owns_terminal = false;
  std::optional<ErrorDTO> coordinated_pre_dispatch_failure;
  {
    std::lock_guard state_lock(state_mutex_);
    operation_id = find_event_operation_locked(engine, event.execution_id);
    auto* operation =
        operation_id.has_value() ? find_active_operation_locked(*operation_id) : nullptr;
    if (auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
        participant != nullptr && participant->deferred_cancel_settlement.has_value() &&
        participant->deferred_cancel_settlement->request.execution_id == event.execution_id) {
      // Q60: the late cancellation reply of a never-dispatched participant
      // whose Stop already closed. The closed Stop keeps owning this
      // participant's terminal; only its deferred settlement is tidied up, on
      // the Stop lane (never inside the subordinate's callback).
      auto& deferred = *participant->deferred_cancel_settlement;
      if (terminal(state) && !deferred.returned_state.has_value()) {
        deferred.returned_state = state;
        deferred.returned_error = event.error;
        auto wake = deferred.stop_token;
        if (wake.slot < stop_transactions_.size() && stop_transactions_[wake.slot] != nullptr &&
            stop_transactions_[wake.slot]->token.generation > wake.generation) {
          wake = stop_transactions_[wake.slot]->token;
        }
        stop_control_executor_->ArmDeadline(wake, clock_.now());
      }
      return;
    }
    if (operation != nullptr && terminal(state) && state != OperationStateKind::kCompleted &&
        operation->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll &&
        !operation->state.active_stop_id.has_value() &&
        !operation->coordinated_dispatch_barrier_released &&
        std::none_of(
            operation->participants.begin(), operation->participants.end(),
            [](const auto& participant) { return participant.prepared_dispatch_released; })) {
      const auto* participant = operation->participant_for(engine);
      coordinated_pre_dispatch_failure =
          event.error.has_value() ? event.error : participant->last_subordinate_error;
      if (!coordinated_pre_dispatch_failure.has_value()) {
        coordinated_pre_dispatch_failure =
            error(std::string(to_string(ErrorCode::kExecutionRejected)),
                  "coordinated participant terminated before dispatch",
                  "coordinated_pre_dispatch_terminal_failure");
      }
    }
    const auto* stop_participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (stop_participant != nullptr && stop_participant->stop_owns_reconciliation() &&
        operation->state.active_stop_id.has_value() &&
        operation->state.active_stop_generation.has_value() && operation->stop_effect_in_flight) {
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && !candidate->terminal &&
                   candidate->stop_id == operation->state.active_stop_id &&
                   candidate->stop_generation == *operation->state.active_stop_generation;
          });
      if (transaction == stop_transactions_.end()) {
        motion_admission_closed_ = true;
      } else {
        stop_wakeup = (*transaction)->token;
        const auto* participant = operation->participant_for(engine);
        if (participant != nullptr) {
          const auto work = std::find_if(
              (*transaction)->participants.begin(), (*transaction)->participants.end(),
              [&](const auto& candidate) {
                return candidate.operation_id == operation->state.operation_id &&
                       candidate.plan.participant_id == participant->slot_owner.participant_id;
              });
          aggregate_stop_owns_terminal = participant->stop_owns_reconciliation();
          if (work != (*transaction)->participants.end()) {
            observe_prepared_cancellation_terminal(*work, event.execution_id, state, event.error);
            if (work->terminal_error.has_value()) motion_admission_closed_ = true;
          }
        }
        stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
      }
    }
  }
  if (stop_wakeup.has_value()) {
    stop_control_executor_->Publish(*stop_wakeup);
  }
  if (aggregate_stop_owns_terminal) {
    // The aggregate Stop reducer is the only public terminal owner while its
    // exact participant settlements are in flight. Subordinate feedback is a
    // wake-up/evidence fact, not a competing operation transition.
    return;
  }
  if (coordinated_pre_dispatch_failure.has_value()) {
    // The execution owner's terminal event is decisive even if its private
    // prepared-session cleanup follows that event. An earlier error alone
    // was not decisive. Reuse the abort claim, including recursive callbacks.
    (void)fail_pre_dispatch(*operation_id, engine,
                                        std::move(*coordinated_pre_dispatch_failure));
    return;
  }
  if (!operation_id.has_value()) {
    return;
  }
  (void)apply_subordinate_state(*operation_id, engine, event.execution_id, state, 0.0, event.error,
                                true);
}

Result<void> ResourceAddressedExecutionKernel::reconcile_subordinate_state() {
  struct ReconcileTarget final {
    OperationId operation_id;
    IResolvedParticipantExecutionEngine* engine{};
    IResolvedPreparedParticipantExecutionEngine* prepared_motion_engine{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::uint64_t subordinate_id{};
    std::uint64_t subordinate_generation{};
    std::string command_target;
    OperationStateKind public_state{OperationStateKind::kPrepared};
    bool prepared_dispatch_released{};
    bool cancelled_preparation_pending{};
    std::optional<ControlBindingId> runtime_scale_control_binding_id;
  };
  std::vector<OperationId> interrupted_barriers;
  {
    std::lock_guard lock(state_mutex_);
    for (const auto& [id, operation] : active_operations_)
      if (operation->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll &&
          !operation->coordinated_dispatch_barrier_released &&
          std::any_of(operation->participants.begin(), operation->participants.end(),
              [](const auto& row) { return row.stop_owns_reconciliation() || row.stop_cancelled_pre_dispatch; }))
        interrupted_barriers.push_back(id);
  }
  for (const auto& id : interrupted_barriers) {
    const auto discarded = discard_uncommitted_ordinary_preparations(id);
    if (!discarded.ok()) return discarded;
  }
  std::vector<ReconcileTarget> targets;
  {
    std::lock_guard state_lock(state_mutex_);
    targets.reserve(active_operations_.size() * 2U);
    for (const auto& [operation_id, operation] : active_operations_) {
      // Operation controls own their subordinate transition until their
      // accepted effect returns. A scheduler pass skips, rather than waits on,
      // that exact operation; disjoint operations still reconcile. The claim
      // also coalesces concurrent scheduler calls without holding a lock over
      // subordinate I/O.
      if (operation->pending_control.has_value() || operation->runtime_scale_effect_in_flight ||
          operation->recovery_effect_in_flight ||
          operation->coordinated_cancellation_in_flight) {
        continue;
      }
      for (auto& participant : operation->participants) {
        if (participant.stop_owns_reconciliation() || participant.reconciliation_in_flight || participant.prepare_in_flight ||
            participant.dispatch_effect_in_flight || participant.subordinate_id == 0U) {
          continue;
        }
        const auto public_participant =
            std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                         [&](const auto& candidate) {
                           return candidate.participant_id == participant.slot_owner.participant_id;
                         });
        if (public_participant == operation->state.participants.end()) {
          continue;
        }
        participant.reconciliation_in_flight = true;
        std::optional<ControlBindingId> runtime_scale_control_binding_id;
        if (operation->runtime_scale_status.has_value()) {
          runtime_scale_control_binding_id =
              participant.binding.value().control_path->control_binding_id;
        }
        targets.push_back(ReconcileTarget{
            operation_id, participant.engine, participant.prepared_motion_engine,
            participant.payload_kind, participant.subordinate_id,
            participant.subordinate_generation,
            participant.binding.value().command_target_id.value(), public_participant->state,
            participant.prepared_dispatch_released,
            participant.non_motion_transaction() && participant.stop_cancelled_pre_dispatch,
            std::move(runtime_scale_control_binding_id)});
      }
    }
  }

  std::optional<ErrorDTO> first_error;
  for (const auto& target : targets) {
    Result<void> applied = Result<void>::Ok();
    if (target.public_state == OperationStateKind::kStopping || terminal(target.public_state)) {
      // StopControl and terminal settlement own these states. Nominal
      // subordinate polling must not reinterpret the backend's deliberate
      // cancellation/idle transition before the Stop certificate commits.
    } else if (target.payload_kind == OperationParticipantPayloadKind::kMotion) {
      bool dispatch_released = target.prepared_dispatch_released;
      if (target.prepared_motion_engine != nullptr && !dispatch_released) {
        applied = advance_prepared_motion(target.operation_id, target.engine);
        std::lock_guard state_lock(state_mutex_);
        const auto* operation = find_active_operation_locked(target.operation_id);
        const auto* participant =
            operation == nullptr ? nullptr : operation->participant_for(target.engine);
        dispatch_released = participant != nullptr && participant->prepared_dispatch_released;
      }
      if (!applied.ok() || !dispatch_released) {
        // Preparation is effect-free. Do not reinterpret the subordinate's
        // private Planning/Validating state as public execution progress while
        // the owner still holds the coordinated dispatch barrier.
      } else {
        applied = apply_subordinate_state(target.operation_id, target.engine, target.subordinate_id,
                                          target.public_state, 0.0, std::nullopt, true);
        if (applied.ok() && target.runtime_scale_control_binding_id.has_value()) {
          const auto scale_status = target.engine->runtime_scale_status(
              target.operation_id, *target.runtime_scale_control_binding_id);
          if (scale_status.ok()) {
            applied = apply_runtime_scale_status(target.engine, scale_status.value());
          } else {
            OperationCommitGuard commit(*this, target.operation_id);
            std::unique_lock state_lock(state_mutex_);
            auto* active_operation = find_active_operation_locked(target.operation_id);
            if (active_operation != nullptr && active_operation->runtime_scale_status.has_value()) {
              auto public_status = *active_operation->runtime_scale_status;
              public_status.state = RuntimeScaleApplicationState::kFailed;
              public_status.updated_at_ns = clock_.now();
              const auto* participant = active_operation->participant_for(target.engine);
              const auto row =
                  std::find_if(public_status.participants.begin(), public_status.participants.end(),
                               [&](const auto& item) {
                                 return participant && item.participant_id ==
                                                           participant->slot_owner.participant_id;
                               });
              if (row != public_status.participants.end()) {
                row->state = RuntimeScaleApplicationState::kFailed;
                row->error = scale_status.error();
              }
              applied = retain_runtime_scale_observation_locked(
                  state_lock, *active_operation, std::move(public_status),
                  "runtime_scale_reconcile_unavailable");
            }
          }
        }
      }
    } else if (!target.prepared_dispatch_released || target.cancelled_preparation_pending) {
      // Drop only this polling claim while the same prepared-owner observer
      // obtains its per-operation writer gate. No provider effect is active.
      if (target.cancelled_preparation_pending) {
        std::lock_guard lock(state_mutex_);
        auto* operation = find_active_operation_locked(target.operation_id);
        if (auto* participant = operation ? operation->participant_for(target.engine) : nullptr)
          participant->reconciliation_in_flight = false;
      }
      applied = advance_prepared_capability_command(target.operation_id, target.engine);
    } else {
      applied = apply_subordinate_state(target.operation_id, target.engine,
          target.subordinate_id, target.public_state, 0.0, std::nullopt, true);
    }
    if (!applied.ok() && !first_error.has_value()) {
      first_error = applied.error();
    }
    {
      std::lock_guard state_lock(state_mutex_);
      auto* active_operation = find_active_operation_locked(target.operation_id);
      if (active_operation != nullptr) {
        auto* participant = active_operation->participant_for(target.engine);
        if (participant != nullptr) {
          participant->reconciliation_in_flight = false;
          settle_completed_control_locked(target.operation_id);
        }
      }
    }
  }
  return first_error.has_value() ? Result<void>::Err(*first_error) : Result<void>::Ok();
}

std::vector<ResourceSlotSnapshotDTO> ResourceAddressedExecutionKernel::resource_snapshot() const {
  return resource_slots_->snapshot();
}

ResourceSlotTableSnapshotDTO ResourceAddressedExecutionKernel::resource_table_snapshot() const {
  return resource_slots_->coherent_snapshot();
}

Result<OrphanResourceReconciliationResultDTO>
ResourceAddressedExecutionKernel::reconcile_orphaned_resources(
    const OrphanResourceReconciliationFactsDTO& facts) {
  if (orphan_resource_reconciliation_facts_field_error(facts).has_value()) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation facts are invalid",
        "orphan_resource_reconciliation_facts_invalid", ErrorCode::kInvalidRequest);
  }

  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  if (facts.request_owner_epoch != config_.request_owner_epoch ||
      facts.cell_id != config_.cell_id || !stop_retained_bundle_id_.has_value() ||
      !stop_graph_revision_.has_value() || facts.retained_bundle_id != *stop_retained_bundle_id_ ||
      facts.graph_revision != *stop_graph_revision_) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation crosses current Kernel identity",
        "orphan_resource_reconciliation_identity_mismatch", ErrorCode::kInvalidStateTransition);
  }

  const auto snapshot = resource_slots_->coherent_snapshot();
  if (snapshot.operation_resource_generation != facts.expected_operation_resource_generation) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation names a stale slot generation",
        "orphan_resource_reconciliation_generation_stale", ErrorCode::kInvalidStateTransition);
  }
  const auto configurations = resource_slots_->admission_snapshot({}).active_control_configurations;
  const auto now_ns = clock_.now();
  if (now_ns < facts.assessed_at_ns) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation was assessed in the future",
        "orphan_resource_reconciliation_time_invalid", ErrorCode::kInvalidRequest);
  }
  const auto elapsed_ns = now_ns - facts.assessed_at_ns;
  std::vector<ResourceId> resources;
  resources.reserve(facts.resources.size());
  for (const auto& fact : facts.resources) {
    const auto slot = std::lower_bound(
        snapshot.slots.begin(), snapshot.slots.end(), fact.resource_id,
        [](const auto& row, const ResourceId& resource) { return row.resource_id < resource; });
    if (slot == snapshot.slots.end() || slot->resource_id != fact.resource_id ||
        slot->state != ResourceSlotState::kUnreconciledExternalOrOrphanGoal ||
        slot->owner.has_value() || !fact.settlement.source_age_ns.has_value() ||
        !fact.settlement.freshness_budget_ns.has_value() ||
        elapsed_ns > *fact.settlement.freshness_budget_ns - *fact.settlement.source_age_ns) {
      return reject<OrphanResourceReconciliationResultDTO>(
          "orphan-resource reconciliation is stale or does not name a current orphan row",
          "orphan_resource_reconciliation_scope_or_freshness_changed",
          ErrorCode::kInvalidStateTransition);
    }

    const ActiveControlConfigurationDTO* active_configuration = nullptr;
    const auto readback = runtime_route_configuration_readbacks_.find(fact.control_domain_id);
    const bool has_domain_readback = readback != runtime_route_configuration_readbacks_.end();
    if (has_domain_readback &&
        readback->second.active_configuration.active_control_configuration_id ==
            fact.active_control_configuration_id &&
        readback->second.active_configuration.active_control_configuration_generation ==
            fact.active_control_configuration_generation &&
        readback->second.active_configuration.state ==
            ActiveControlConfigurationStateDTO::kReconciled) {
      active_configuration = &readback->second.active_configuration;
    }
    const std::vector<DesiredControlBindingRouteDTO>* retained_binding_routes = nullptr;
    if (!has_domain_readback) {
      for (const auto& registration : registrations_) {
        if (!registration.stop_readiness.has_value()) {
          continue;
        }
        const auto& readiness = registration.stop_readiness->value();
        const auto& expected = readiness.expected_controller_domain;
        const auto& plan = readiness.resolved_stop_base_plan;
        if (expected.control_domain_id == fact.control_domain_id &&
            plan.active_control_configuration_id == fact.active_control_configuration_id &&
            plan.active_control_configuration_generation ==
                fact.active_control_configuration_generation) {
          retained_binding_routes = &expected.desired_configuration.binding_routes;
          break;
        }
      }
    }

    bool matched_source = false;
    bool found_containing_registration = false;
    for (const auto& registration : registrations_) {
      const auto& binding = registration.binding.value();
      const bool contains = std::any_of(
          binding.resources.begin(), binding.resources.end(),
          [&](const auto& resource) { return resource.resource_id == fact.resource_id; });
      if (!contains) {
        continue;
      }
      found_containing_registration = true;
      if (!binding.control_path.has_value()) {
        return reject<OrphanResourceReconciliationResultDTO>(
            "orphan resource has no exact current control path",
            "orphan_resource_reconciliation_binding_missing", ErrorCode::kInvariantViolation);
      }
      const auto& path = *binding.control_path;
      const bool same_physical_owner =
          binding.controllable_component_id == fact.controllable_component_id &&
          binding.command_target_id == fact.command_target_id &&
          path.control_domain_id == fact.control_domain_id &&
          path.active_control_configuration_id == fact.active_control_configuration_id &&
          path.active_control_configuration_generation ==
              fact.active_control_configuration_generation &&
          path.provider_runtime_unit_id == fact.provider_runtime_unit_id &&
          path.provider_runtime_epoch == fact.provider_runtime_epoch &&
          fact.settlement.source_epoch == std::optional<RuntimeEpoch>(path.provider_runtime_epoch);
      if (!same_physical_owner) {
        return reject<OrphanResourceReconciliationResultDTO>(
            "orphan-resource reconciliation differs from a current binding",
            "orphan_resource_reconciliation_binding_mismatch", ErrorCode::kInvalidStateTransition);
      }

      const auto route_matches = [&](const auto& routes) {
        const auto route =
            std::lower_bound(routes.begin(), routes.end(), path.control_binding_id,
                             [](const auto& row, const ControlBindingId& binding_id) {
                               return row.control_binding_id < binding_id;
                             });
        return route != routes.end() && route->control_binding_id == path.control_binding_id &&
               route->control_binding_revision == path.control_binding_revision &&
               route->control_route_id == path.control_route_id;
      };
      const bool current_route =
          active_configuration != nullptr
              ? route_matches(active_configuration->binding_routes)
              : retained_binding_routes != nullptr && route_matches(*retained_binding_routes);
      if (!current_route) {
        return reject<OrphanResourceReconciliationResultDTO>(
            "orphan-resource reconciliation differs from current binding-route truth",
            "orphan_resource_reconciliation_configuration_mismatch",
            ErrorCode::kInvalidStateTransition);
      }
      matched_source =
          matched_source || (path.control_binding_id == fact.control_binding_id &&
                             path.control_binding_revision == fact.control_binding_revision);
    }
    const auto configuration =
        std::lower_bound(configurations.begin(), configurations.end(), fact.control_domain_id,
                         [](const auto& row, const ControlDomainId& domain) {
                           return row.control_domain_id < domain;
                         });
    if (!found_containing_registration || !matched_source ||
        configuration == configurations.end() ||
        configuration->control_domain_id != fact.control_domain_id ||
        configuration->active_control_configuration_id != fact.active_control_configuration_id ||
        configuration->active_control_configuration_generation !=
            fact.active_control_configuration_generation) {
      return reject<OrphanResourceReconciliationResultDTO>(
          "orphan-resource reconciliation differs from current route truth",
          "orphan_resource_reconciliation_configuration_mismatch",
          ErrorCode::kInvalidStateTransition);
    }
    resources.push_back(fact.resource_id);
  }

  auto reconciled = resource_slots_->reconcile_unreconciled_external_or_orphan_goal(
      facts.expected_operation_resource_generation, resources);
  if (!reconciled.ok()) {
    return Result<OrphanResourceReconciliationResultDTO>::Err(reconciled.error());
  }
  std::vector<ResourceId> remaining;
  for (const auto& slot : resource_slots_->snapshot()) {
    if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
      remaining.push_back(slot.resource_id);
    }
  }
  motion_admission_unreconciled_ = !remaining.empty();
  OrphanResourceReconciliationResultDTO result{std::move(resources), std::move(remaining),
                                               reconciled.value()};
  if (orphan_resource_reconciliation_result_field_error(result).has_value()) {
    std::terminate();
  }
  return Result<OrphanResourceReconciliationResultDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
