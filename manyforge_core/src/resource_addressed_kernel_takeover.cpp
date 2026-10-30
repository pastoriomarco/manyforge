#include "resource_addressed_execution_kernel_internal.hpp"

#include <cstdio>

namespace manyforge::core {
namespace {
// Unpublished capture storage. Required strong identities are filled from the
// current operation before this value can be recorded or published.
[[nodiscard]] OperationStateDTO empty_takeover_capture_storage() {
  return {OperationId(std::string{}),
          OperationRevision(0U),
          ProgramBindingId(std::string{}),
          ProgramRunId(std::string{}),
          OperationCoordinationPolicy::kIndependent,
          OperationStateKind::kResolving,
          {},
          0,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}
}  // namespace

Result<std::optional<OperationAcceptedDTO>>
ResourceAddressedExecutionKernel::try_accept_motion_takeover(
    ActiveOperationNode& prepared_operation, ReplayEntry& prepared_replay,
    const RuntimeParticipantExecutionRegistration& registration) {
  using Outcome = Result<std::optional<OperationAcceptedDTO>>;
  if (prepared_operation.empty())
    return reject<std::optional<OperationAcceptedDTO>>(
        "takeover admission lost its prepared operation", "active_operation_lost",
        ErrorCode::kInvariantViolation);
  auto& candidate = *prepared_operation.mapped();
  if (candidate.participants.size() != 1U ||
      candidate.participants.front().payload_kind != OperationParticipantPayloadKind::kMotion)
    return Outcome::Ok(std::nullopt);
  auto* engine = candidate.participants.front().engine;
  OperationId incumbent_id{std::string{}};
  {
    std::lock_guard lock(state_mutex_);
    for (const auto& [id, operation] : active_operations_)
      if (operation->participant_for(engine)) {
        incumbent_id = id;
        break;
      }
  }
  if (incumbent_id.empty()) return Outcome::Ok(std::nullopt);

  // Ordinary operation writers serialize only capture/publication. Stop
  // continues to bypass this guard, including during durable acceptance.
  OperationCommitGuard incumbent_commit(*this, incumbent_id);
  OperationStateDTO acceptance_state = empty_takeover_capture_storage();
  OperationStateDTO logical_state = empty_takeover_capture_storage();
  std::vector<OperationId> protected_ids;
  std::optional<std::uint64_t> cancel_unsent;
  IResolvedPreparedParticipantExecutionEngine* cancel_engine{};
  OperationAcceptedDTO accepted{RequestId(std::string{}),
                                RequestReplayStatusDTO::kAcceptedNew,
                                OperationId(std::string{}),
                                OperationRevision(0U),
                                {},
                                0};
  OperationId candidate_id{std::string{}};
  {
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard lock(state_mutex_);
    auto* incumbent = find_active_operation_locked(incumbent_id);
    auto* physical = incumbent ? incumbent->participant_for(engine) : nullptr;
    const auto busy = [&]() {
      return reject<std::optional<OperationAcceptedDTO>>(
          "the resolved participant engine owns an unsettled operation", "participant_engine_busy");
    };
    if (!physical) return Outcome::Ok(std::nullopt);
    if (candidate.manual || incumbent->manual || incumbent->participants.size() != 1U ||
        candidate.intent.program_run_id != incumbent->intent.program_run_id ||
        !program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
        program_cycle_claim_run_->program_run_id != candidate.intent.program_run_id ||
        !candidate.intent.participants.front().execution_scope || !physical->motion_request ||
        !physical->prepared_motion_engine || !candidate.participants.front().takeover_preparation ||
        physical->resources != candidate.participants.front().resources ||
        incumbent->state.active_stop_id || incumbent->pending_control ||
        incumbent->stop_effect_in_flight || incumbent->recovery_effect_in_flight ||
        incumbent->coordinated_cancellation_in_flight || physical->stop_owns_reconciliation() ||
        terminal(incumbent->state.state))
      return busy();
    auto target_physical = candidate.participants.front().slot_owner;
    target_physical.operation_id = physical->slot_owner.operation_id;
    target_physical.operation_revision = physical->slot_owner.operation_revision;
    target_physical.participant_id = physical->slot_owner.participant_id;
    if (target_physical != physical->slot_owner) return busy();
    if (auto refusal = component_command_refusal_locked(registration))
      return Outcome::Err(std::move(*refusal));
    if (motion_admission_closed_)
      return reject<std::optional<OperationAcceptedDTO>>("new motion admission is closed",
                                                         "motion_admission_closed",
                                                         ErrorCode::kInvalidStateTransition);
    const auto scope = validate_program_execution_scope_locked(candidate.intent, false, false);
    if (!scope.ok()) return Outcome::Err(scope.error());
    if (auto refusal = operator_alarm_attention_refusal_locked(physical->resources))
      return Outcome::Err(std::move(*refusal));

    auto* winner = !incumbent->next_motion_takeover.empty()
                       ? incumbent->next_motion_takeover.mapped().get()
                   : !incumbent->pending_motion_takeover.empty()
                       ? incumbent->pending_motion_takeover.mapped().get()
                       : incumbent;
    const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
    const auto node_for = [&](const ActiveOperation& operation) -> const ProgramExecutionNodeDTO* {
      const auto row = std::find_if(
          operation.state.participants.begin(), operation.state.participants.end(),
          [](const auto& participant) {
            return participant.payload_kind == OperationParticipantPayloadKind::kMotion;
          });
      if (row == operation.state.participants.end() || !row->execution_scope) return nullptr;
      const auto node = std::lower_bound(
          nodes.begin(), nodes.end(), row->execution_scope->node_instance_id,
          [](const auto& value, const auto& id) { return value.instance_id < id; });
      return node != nodes.end() && node->instance_id == row->execution_scope->node_instance_id &&
                     node->owner_api == "kernel.operation"
                 ? &*node
                 : nullptr;
    };
    const auto* candidate_node = node_for(candidate);
    const auto* winner_node = node_for(*winner);
    if (!candidate_node || !winner_node) return busy();
    if (candidate_node->preemption_priority < winner_node->preemption_priority) {
      const auto& origin = *winner->state.participants.front().execution_scope;
      return Outcome::Err(ErrorDTO{"refused_by_priority",
                                   "a higher-priority Program motion owns this robot",
                                   {{"winner_operation_id", winner->state.operation_id.value()},
                                    {"winner_node_instance_id", origin.node_instance_id.value()},
                                    {"winner_node_attempt_id", origin.node_attempt_id}}});
    }
    // Active TCP (M7, T-MQ2, T-C2c): a preempt keeps the running move's TCP
    // and never changes the Active TCP; one resolving to another TCP is
    // refused on every lane, before anything is prepared or sent.
    // The requests' TCP frames carry their owners (B7), so equal frames are
    // equal TCP entries.
    if (candidate.participants.front().motion_request->binding.tcp_frame !=
        winner->participants.front().motion_request->binding.tcp_frame) {
      return Outcome::Err(ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                   "a preempt cannot change the robot's TCP during motion",
                                   {{"reason", "tool_context_change_during_motion"},
                                    {"cause", "tcp_differs"}}});
    }
    // The Deployment policy is not defaulted here. Final absence handling is
    // selected with the measured default before this feature is qualified.
    if (!candidate.participants.front().takeover_preparation_budget_ns) return busy();
    if (emergency_stop_reserve_.find(candidate.intent.request_context.request_id.value()))
      return Outcome::Err(
          error("request_id_conflict", "request became retained by Stop", "request_id_conflict"));
    const auto& origin = *candidate.intent.participants.front().execution_scope;
    const auto node_index = static_cast<std::size_t>(candidate_node - nodes.data());
    if (node_index < program_cycle_claim_run_->motion_continuations.size()) {
      const auto& continuation = program_cycle_claim_run_->motion_continuations[node_index];
      if (continuation) {
        auto& row = candidate.participants.front();
        const auto& request = row.takeover_preparation->execution_request;
        if (continuation->origin.node_attempt_id == origin.node_attempt_id ||
            continuation->binding.value() != row.binding.value() ||
            continuation->base_frame != request.binding.base_frame ||
            continuation->tcp_frame != request.binding.tcp_frame ||
            continuation->planner_capability != request.binding.planner_capability_binding ||
            continuation->authored_motion_kind != request.motion.index())
          return reject<std::optional<OperationAcceptedDTO>>(
              "new attempt differs from its retained continuation",
              "program_motion_continuation_mismatch", ErrorCode::kInvalidRequest);
        row.continuation = continuation->intent;
        row.continuation_node_index = node_index;
        row.takeover_preparation->continuation = continuation->intent;
      }
    }

    // All variable-size state is built before publishing either identity.
    const bool unsent = winner != incumbent
                            ? !winner->participants.front().prepared_dispatch_released
                            : !physical->prepared_dispatch_released;
    logical_state = winner->state;
    logical_state.preemption = OperationPreemptionDTO{
        unsent ? OperationPreemptionKind::kSupersededUnsent : OperationPreemptionKind::kPreempted,
        {candidate.state.operation_id, origin.node_instance_id, origin.node_attempt_id}};
    logical_state.updated_at_ns = clock_.now();
    stamp_operation_capture_locked(*winner, logical_state);
    acceptance_state = candidate.state;
    accepted = candidate.accepted;
    candidate_id = candidate.state.operation_id;
    protected_ids = durable_operation_retention_ids_locked();
    protected_ids.push_back(candidate_id);
    std::sort(protected_ids.begin(), protected_ids.end());
    protected_ids.erase(std::unique(protected_ids.begin(), protected_ids.end()),
                        protected_ids.end());
    if (!incumbent->next_motion_takeover.empty()) {
      // A next-slot candidate has never borrowed an engine or crossed a gate.
      auto retired = logical_state;
      retired.state = OperationStateKind::kStopped;
      for (auto& row : retired.participants) {
        row.state = OperationStateKind::kStopped;
        row.terminal_fact =
            ParticipantExecutionFact{ParticipantExecutionFactKind::kCancelledBeforeDispatch,
                                     ParticipantEffectKnowledge::kNoEffect, std::nullopt};
      }
      logical_state = retired;
      retain_historical_operation_locked(std::move(retired));
      incumbent->next_motion_takeover = std::move(prepared_operation);
    } else if (!incumbent->pending_motion_takeover.empty()) {
      winner->state = logical_state;
      incumbent->next_motion_takeover = std::move(prepared_operation);
      if (unsent && winner->participants.front().subordinate_id) {
        cancel_unsent = winner->participants.front().subordinate_id;
        cancel_engine = winner->participants.front().prepared_motion_engine;
      }
    } else {
      winner->state = logical_state;
      incumbent->pending_motion_takeover = std::move(prepared_operation);
    }
    // Item 37: a new operation ends the operator's Force close arm.
    force_close_stop_generation_.reset();
  }
  const auto accepted_write = attempt_journal_write([&] {
    return journal_.RecordOperationAccepted(config_.request_owner_epoch, prepared_replay.intent,
                                            accepted, acceptance_state, protected_ids);
  });
  const auto logical_write =
      accepted_write.ok() ? attempt_journal_write([&] {
        return journal_.RecordOperationState(config_.request_owner_epoch, logical_state, nullptr,
                                             "motion_preempted");
      })
                          : Result<std::uint64_t>::Err(accepted_write.error());
  std::optional<ErrorDTO> publication_failure;
  {
    std::lock_guard lock(state_mutex_);
    auto* pending = find_pending_motion_operation_locked(candidate_id);
    if (!accepted_write.ok() || !logical_write.ok()) {
      publication_failure = !accepted_write.ok() ? accepted_write.error() : logical_write.error();
      retain_operation_journal_write_fault_locked("motion_preemption", *publication_failure,
                                                  &acceptance_state, accepted.request_id);
    }
    if (!publication_failure) {
      if (pending) {
        pending->ordinary_acceptance_pending = false;
        pending->ordinary_acceptance_durable = true;
      }
      replay_ledger_.push_back(std::move(prepared_replay));
    }
  }
  incumbent_commit.release();
  const auto contain_accepted_failure = [&](const ErrorDTO& failure) -> Result<void> {
    OperationId current_owner{std::string{}};
    OperationId first_pending{std::string{}};
    {
      std::lock_guard lock(state_mutex_);
      for (const auto& [id, operation] : active_operations_) {
        if ((!operation->pending_motion_takeover.empty() &&
             operation->pending_motion_takeover.key() == candidate_id) ||
            (!operation->next_motion_takeover.empty() &&
             operation->next_motion_takeover.key() == candidate_id)) {
          if (operation->state.active_stop_id) return Result<void>::Ok();
          current_owner = id;
          first_pending = operation->pending_motion_takeover.key();
          break;
        }
      }
    }
    if (current_owner.empty()) return Result<void>::Ok();
    return begin_motion_takeover_fallback(current_owner, first_pending, failure);
  };
  if (publication_failure) {
    // Acceptance already logically preempted the source. A failed journal
    // closes successor admission, but cannot leave that source moving forever.
    const auto contained = contain_accepted_failure(*publication_failure);
    if (!contained.ok()) return Outcome::Err(contained.error());
    return Outcome::Err(std::move(*publication_failure));
  }
  if (cancel_engine && cancel_unsent) {
    const auto cancelled = cancel_engine->CancelPreparedMotion(*cancel_unsent);
    if (!cancelled.ok()) {
      const auto contained = contain_accepted_failure(cancelled.error());
      if (!contained.ok()) return Outcome::Err(contained.error());
    }
  }
  const auto advanced = advance_motion_takeover(incumbent_id);
  if (!advanced.ok()) return Outcome::Err(advanced.error());
  return Outcome::Ok(accepted);
}

Result<void> ResourceAddressedExecutionKernel::reserve_cold_motion_takeover_goal_locked(
    ActiveOperation& candidate) {
  if (candidate.participants.size() != 1U || candidate.state.participants.size() != 1U ||
      !candidate.participants.front().takeover_preparation)
    return reject<void>("Cold takeover lost its exact future authority",
                        "motion_takeover_authority_missing", ErrorCode::kInvariantViolation);
  auto& row = candidate.participants.front();
  if (!row.prepared_dispatch_released) return Result<void>::Ok();
  const auto& current = candidate.state.participants.front();
  auto& future = row.takeover_preparation->execution_request.binding;
  if (future.operation_id != candidate.state.operation_id ||
      future.participant_id != current.participant_id ||
      future.controller_goal_id != current.command_id)
    return reject<void>("Cold takeover changed its physical goal family",
                        "motion_takeover_goal_identity_mismatch", ErrorCode::kInvariantViolation);
  if (current.command_generation.value() >= kMaximumKernelJsonInteger)
    return reject<void>("Controller goal generation space is exhausted",
                        "controller_goal_generation_exhausted", ErrorCode::kInternalError);
  const ControllerGoalGeneration next(current.command_generation.value() + 1U);
  if (future.controller_goal_generation != next &&
      future.controller_goal_generation != current.command_generation)
    return reject<void>("Cold takeover future goal generation is stale",
                        "motion_takeover_goal_identity_mismatch", ErrorCode::kInvariantViolation);
  // Only private, not-yet-computed authority changes here. The sent goal and
  // public operation retain their exact key until the settled owner transfer.
  future.controller_goal_generation = next;
  // The initial scale command is addressed to this same reserved future goal,
  // including when a repeated reservation already observes the next key.
  if (auto& scale = row.takeover_preparation->execution_request.initial_runtime_scale; scale)
    scale->controller_goal_generation = next;
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::commit_motion_takeover_owner_locked(
    ActiveOperation& incumbent, ActiveOperation& candidate,
    const ResourceSlotOwnerTransferMode mode,
    std::pair<OperationStateDTO, OperationStateDTO>& captures) {
  auto* old_row = incumbent.participants.empty() ? nullptr : &incumbent.participants.front();
  auto* next_row = candidate.participants.empty() ? nullptr : &candidate.participants.front();
  if (!old_row || !next_row || incumbent.pending_motion_takeover.empty() ||
      incumbent.pending_motion_takeover.mapped().get() != &candidate ||
      active_operations_.contains(candidate.state.operation_id) || incumbent.pending_control ||
      incumbent.recovery_effect_in_flight || incumbent.stop_effect_in_flight ||
      old_row->dispatch_effect_in_flight || next_row->prepare_in_flight ||
      (mode == ResourceSlotOwnerTransferMode::kMoving && !next_row->prepared_participant) ||
      !candidate.ordinary_acceptance_durable || motion_admission_closed_ ||
      is_active_stop_fault_latch(active_stop_fault_latch_))
    return reject<void>("takeover ownership is no longer current", "motion_takeover_owner_stale");
  const auto scope = validate_program_execution_scope_locked(candidate.intent, false, false);
  if (!scope.ok()) return scope;
  if (auto attention = operator_alarm_attention_refusal_locked(next_row->resources))
    return Result<void>::Err(std::move(*attention));
  if (mode == ResourceSlotOwnerTransferMode::kMoving &&
      (incumbent.state.active_stop_id || old_row->stop_owns_reconciliation() ||
       old_row->slot_state != ResourceSlotState::kDispatched || !incumbent.state.preemption ||
       incumbent.state.preemption->kind != OperationPreemptionKind::kPreempted))
    return reject<void>("moving takeover source is fenced", "motion_takeover_source_stale");
  const auto& old_public = incumbent.state.participants.front();
  const auto* old_fact = selected_cause_terminal_fact(old_public);
  const bool cold_settled =
      incumbent.state.active_stop_id
          ? incumbent.terminal_stop_result() == StopCertificateResultDTO::kSucceededNominal
          : old_fact && old_fact->effect == ParticipantEffectKnowledge::kNoEffect;
  if (mode == ResourceSlotOwnerTransferMode::kSettled &&
      (old_row->slot_state != ResourceSlotState::kTerminal || !cold_settled ||
       !terminal(old_public.state)))
    return reject<void>("cold takeover source is not settled",
                        "motion_takeover_source_not_settled");

  auto old_state = incumbent.state;
  if (mode == ResourceSlotOwnerTransferMode::kMoving) {
    auto& public_row = old_state.participants.front();
    public_row.state = OperationStateKind::kStopped;
    public_row.terminal_error.reset();
    public_row.terminal_fact =
        ParticipantExecutionFact{ParticipantExecutionFactKind::kPreemptedByReplacement,
                                 ParticipantEffectKnowledge::kEffectSettled, std::nullopt};
    old_state.state = OperationStateKind::kStopped;
    old_state.updated_at_ns = clock_.now();
  }
  // Settled handoffs also emit a new journal record, even though the source
  // state is already terminal. Its new record contents need their own capture.
  stamp_operation_capture_locked(incumbent, old_state);
  auto next_state = candidate.state;
  auto& next_public = next_state.participants.front();
  if (mode == ResourceSlotOwnerTransferMode::kSettled &&
      candidate.motion_takeover_spring_fallback) {
    if (!next_row->takeover_preparation || !next_row->motion_request)
      return reject<void>("Cold takeover lost its physical goal authority",
                          "motion_takeover_authority_missing", ErrorCode::kInvariantViolation);
    const auto& future = next_row->takeover_preparation->execution_request.binding;
    const auto& current_request = next_row->motion_request->binding;
    const auto current_generation = next_public.command_generation.value();
    const auto future_generation = future.controller_goal_generation.value();
    if (future.operation_id != candidate.state.operation_id ||
        future.participant_id != next_public.participant_id ||
        future.controller_goal_id != next_public.command_id || future_generation == 0U ||
        future_generation > kMaximumKernelJsonInteger ||
        (future_generation != current_generation &&
         (current_generation >= kMaximumKernelJsonInteger ||
          future_generation != current_generation + 1U)) ||
        current_request.operation_id != candidate.state.operation_id ||
        current_request.participant_id != next_public.participant_id ||
        current_request.controller_goal_id != next_public.command_id ||
        current_request.controller_goal_generation != next_public.command_generation)
      return reject<void>("Cold takeover physical goal authority changed",
                          "motion_takeover_goal_identity_mismatch", ErrorCode::kInvariantViolation);
    next_public.command_generation = future.controller_goal_generation;
  }
  next_public.state = mode == ResourceSlotOwnerTransferMode::kMoving
                          ? OperationStateKind::kExecuting
                      : next_row->prepared_participant ? OperationStateKind::kPrepared
                                                       : OperationStateKind::kPlanning;
  next_public.terminal_error.reset();
  next_public.terminal_fact.reset();
  next_state.state = next_public.state;
  next_state.updated_at_ns = clock_.now();
  if (mode == ResourceSlotOwnerTransferMode::kMoving) {
    next_state.motion_takeover = MotionTakeoverDecisionDTO{
        incumbent.state.operation_id, MotionTakeoverPathDTO::kMovingReplacement, std::nullopt,
        next_state.updated_at_ns,
        std::max<std::int64_t>(0, next_state.updated_at_ns - candidate.accepted.accepted_at_ns),
        next_row->takeover_preparation_budget_ns.value_or(0), std::nullopt};
  }
  stamp_operation_capture_locked(candidate, next_state);
  captures = {old_state, next_state};
  const auto snapshot = resource_slots_->coherent_snapshot();
  const std::vector<ResourceSlotOwnerTransferDTO> rows{
      {old_row->slot_owner, next_row->slot_owner, old_row->resources}};
  // This is the final fallible action. The existing worker/backend has already
  // fenced its exact source and prebuilt its adoption context before invoking us.
  auto transferred = resource_slots_->transfer_owners(snapshot, rows, mode);
  if (!transferred.ok()) return Result<void>::Err(std::move(transferred.error()));

  static_assert(std::is_nothrow_move_assignable_v<OperationStateDTO>);
  incumbent.state = std::move(old_state);
  if (mode == ResourceSlotOwnerTransferMode::kSettled &&
      candidate.motion_takeover_spring_fallback) {
    // The family string was checked before CAS; publish only its nonthrowing
    // generation value alongside the already-built current operation capture.
    next_row->motion_request->binding.controller_goal_generation =
        next_state.participants.front().command_generation;
  }
  old_row->ownership_transferred = true;
  // Move the now-consumed preparation authority to the retiring source. Its
  // ordinary closure reclaims it, outside this handover's critical section.
  static_assert(
      std::is_nothrow_move_assignable_v<std::optional<ResolvedPreparedMotionExecutionRequestDTO>>);
  old_row->takeover_preparation = std::move(next_row->takeover_preparation);
  next_row->takeover_preparation.reset();
  old_row->resources.clear();
  old_row->engine = nullptr;
  old_row->prepared_motion_engine = nullptr;
  old_row->stop_execution_port = nullptr;
  old_row->subordinate_id = 0U;
  old_row->subordinate_generation = 0U;
  old_row->preparation_id = 0U;
  old_row->reconciliation_in_flight = false;
  old_row->prepared_advance_in_flight = false;
  old_row->slot_state = ResourceSlotState::kTerminal;
  candidate.state = std::move(next_state);
  next_row->slot_state = mode == ResourceSlotOwnerTransferMode::kMoving
                             ? ResourceSlotState::kDispatched
                             : ResourceSlotState::kReserved;
  next_row->prepared_dispatch_released = mode == ResourceSlotOwnerTransferMode::kMoving;
  candidate.coordinated_dispatch_barrier_released = next_row->prepared_dispatch_released;
  candidate.takeover_source.reset();
  candidate.motion_takeover_spring_fallback.reset();
  candidate.pending_motion_takeover = std::move(incumbent.next_motion_takeover);
  // Insertion of this preallocated node cannot allocate. OperationId's
  // comparison is nonthrowing; uniqueness was checked before the table CAS.
  active_operations_.insert(std::move(incumbent.pending_motion_takeover));
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::advance_motion_takeover_impl(
    const OperationId& incumbent_id) {
  OperationId candidate_id{std::string{}};
  IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
  std::uint64_t source_id{};
  std::uint64_t candidate_execution_id{};
  bool original_unsent{};
  bool cancelled_by_stop{};
  bool superseded_unsent{};
  bool spring_fallback{};
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> request;
  std::optional<PreparedMotionReplacementSourceDTO> source;
  std::shared_ptr<std::mutex> identity;
  {
    std::lock_guard lock(state_mutex_);
    auto* incumbent = find_active_operation_locked(incumbent_id);
    if (!incumbent || incumbent->pending_motion_takeover.empty() ||
        incumbent->takeover_advance_in_flight)
      return Result<void>::Ok();
    auto& candidate = *incumbent->pending_motion_takeover.mapped();
    if ((!candidate.ordinary_acceptance_durable && !candidate.motion_takeover_spring_fallback) ||
        candidate.participants.empty() || incumbent->participants.empty())
      return Result<void>::Ok();
    auto& next = candidate.participants.front();
    const auto& old = incumbent->participants.front();
    const bool reset = incumbent->pending_control &&
                       incumbent->pending_control->accepted.control == OperationControlKind::kReset;
    if (reset &&
        (!incumbent->pending_control->durable || !incumbent->pending_control->effects_dispatched ||
         incumbent->pending_control->first_failure ||
         incumbent->pending_control->completed_effects !=
             incumbent->pending_control->expected_effects))
      return Result<void>::Ok();
    incumbent->takeover_advance_in_flight = true;
    identity = incumbent->commit_mutex;
    candidate_id = candidate.state.operation_id;
    prepared_engine = next.prepared_motion_engine;
    source_id = old.subordinate_id;
    candidate_execution_id = next.subordinate_id;
    const auto* old_fact = selected_cause_terminal_fact(incumbent->state.participants.front());
    original_unsent = !old.prepared_dispatch_released || old.prepared_engine_no_effect_confirmed ||
                      (old_fact && old_fact->effect == ParticipantEffectKnowledge::kNoEffect);
    superseded_unsent =
        candidate.state.preemption &&
        candidate.state.preemption->kind == OperationPreemptionKind::kSupersededUnsent;
    spring_fallback = candidate.motion_takeover_spring_fallback.has_value();
    cancelled_by_stop =
        reset ||
        (incumbent->state.active_stop_id &&
         (!spring_fallback ||
          *incumbent->state.active_stop_id != candidate.motion_takeover_spring_fallback->stop_id ||
          incumbent->state.active_stop_generation !=
              candidate.motion_takeover_spring_fallback->stop_generation));
    source = candidate.takeover_source;
    if (!spring_fallback && candidate_execution_id == 0U && next.takeover_preparation) {
      request = *next.takeover_preparation;
      next.prepare_in_flight = !cancelled_by_stop && !superseded_unsent && !original_unsent;
    }
  }
  const ScopeExit release([&] {
    std::lock_guard lock(state_mutex_);
    auto* incumbent = find_active_operation_locked(incumbent_id);
    if (incumbent && incumbent->commit_mutex == identity) {
      incumbent->takeover_advance_in_flight = false;
      if (auto* pending = find_pending_motion_operation_locked(candidate_id))
        pending->participants.front().prepare_in_flight = false;
      settle_completed_control_locked(incumbent_id);
    }
  });
  if (cancelled_by_stop || superseded_unsent) {
    const auto ended = end_pending_motion_takeover(incumbent_id, candidate_id, std::nullopt);
    return ended.ok() ? Result<void>::Ok() : Result<void>::Err(ended.error());
  }
  if (spring_fallback) return advance_motion_takeover_fallback(incumbent_id, candidate_id);
  if (original_unsent) return advance_unsent_motion_takeover(incumbent_id);
  if (!prepared_engine)
    return begin_motion_takeover_fallback(
        incumbent_id, candidate_id,
        error("motion_takeover_owner_missing", "Takeover lost its preparation owner",
              "motion_takeover_owner_missing"));

  if (candidate_execution_id == 0U) {
    auto captured = prepared_engine->CaptureMotionReplacementSource(source_id);
    if (!captured.ok()) {
      {
        std::lock_guard lock(state_mutex_);
        if (auto* pending = find_pending_motion_operation_locked(candidate_id))
          pending->participants.front().prepare_in_flight = false;
      }
      return begin_motion_takeover_fallback(incumbent_id, candidate_id, captured.error());
    }
    if (!request)
      return begin_motion_takeover_fallback(
          incumbent_id, candidate_id,
          error("motion_takeover_authority_missing", "Takeover lost its sealed authority",
                "motion_takeover_authority_missing"));
    {
      std::lock_guard lock(state_mutex_);
      auto* incumbent = find_active_operation_locked(incumbent_id);
      auto* candidate = find_pending_motion_operation_locked(candidate_id);
      if (!incumbent || !candidate || incumbent->pending_motion_takeover.empty() ||
          incumbent->pending_motion_takeover.key() != candidate_id ||
          incumbent->state.active_stop_id || candidate->state.preemption ||
          !candidate->participants.front().takeover_preparation_budget_ns) {
        if (candidate) candidate->participants.front().prepare_in_flight = false;
        return Result<void>::Ok();
      }
      candidate->takeover_source = captured.value();
      request->replacement = ResolvedPreparedMotionReplacementContextDTO{
          captured.value(), *candidate->participants.front().takeover_preparation_budget_ns,
          std::nullopt, candidate->participants.front().takeover_planning_budget_ns};
    }
    const auto prepared = [&]() -> Result<std::uint64_t> {
      try {
        return prepared_engine->PrepareMotion(std::move(*request));
      } catch (...) {
        return reject<std::uint64_t>("Takeover preparation raised before dispatch",
                                     "motion_takeover_prepare_exception");
      }
    }();
    {
      std::lock_guard lock(state_mutex_);
      if (auto* candidate = find_pending_motion_operation_locked(candidate_id)) {
        auto& row = candidate->participants.front();
        row.prepare_in_flight = false;
        if (prepared.ok()) row.subordinate_id = row.preparation_id = prepared.value();
      }
    }
    if (!prepared.ok())
      return begin_motion_takeover_fallback(incumbent_id, candidate_id, prepared.error());
    return Result<void>::Ok();
  }

  const auto observe_started = clock_.now();
  const auto observed = prepared_engine->ObservePreparedMotion(candidate_execution_id);
  const auto observe_finished = clock_.now();
  if (!observed.ok())
    return begin_motion_takeover_fallback(incumbent_id, candidate_id, observed.error());
  const auto& snapshot = observed.value();
  if (snapshot.execution_id != candidate_execution_id)
    return begin_motion_takeover_fallback(
        incumbent_id, candidate_id,
        error("motion_takeover_observation_stale", "Takeover observation changed identity",
              "motion_takeover_observation_stale"));
  if (snapshot.state == ResolvedPreparedMotionExecutionStateDTO::kPreparing)
    return Result<void>::Ok();
  if (snapshot.state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
      snapshot.state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch)
    return begin_motion_takeover_fallback(
        incumbent_id, candidate_id,
        snapshot.error.value_or(error("motion_takeover_preparation_failed",
                                      "Takeover preparation did not produce a dispatchable curve",
                                      "motion_takeover_preparation_failed")));

  if (snapshot.state == ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
    const auto commit_started = clock_.now();
    OperationCommitGuard commit(*this, incumbent_id);
    const auto commit_acquired = clock_.now();
    std::int64_t state_acquired{};
    OperationStateDTO dispatch_state = empty_takeover_capture_storage();
    ContentDigest digest{std::string{}};
    const auto state_lock_started = clock_.now();
    {
      std::lock_guard lock(state_mutex_);
      state_acquired = clock_.now();
      auto* incumbent = find_active_operation_locked(incumbent_id);
      auto* candidate = find_pending_motion_operation_locked(candidate_id);
      if (!incumbent || !candidate || incumbent->state.active_stop_id ||
          incumbent->pending_motion_takeover.empty() ||
          incumbent->pending_motion_takeover.key() != candidate_id || candidate->state.preemption ||
          motion_admission_closed_)
        return Result<void>::Ok();
      auto& row = candidate->participants.front();
      if (!snapshot.participant || !snapshot.backend_prepare_result)
        return reject<void>("takeover preparation has no exact product",
                            "prepared_motion_observation_invalid", ErrorCode::kInvariantViolation);
      if (snapshot.participant->controller().ordinary_stop_plan != *row.ordinary_stop_plan ||
          !prepared_fault_stop_plan_matches(snapshot.participant->controller().fault_stop_plan,
                                            row.fault_stop_plan) ||
          prepare_participant_result_field_error(*snapshot.backend_prepare_result,
                                                 *snapshot.participant))
        return reject<void>("takeover product differs from its resolved authority",
                            "prepared_motion_product_mismatch", ErrorCode::kInvariantViolation);
      row.prepared_participant = *snapshot.participant;
      digest = snapshot.participant->prepared_contract_digest;
      dispatch_state = candidate->state;
      dispatch_state.state = dispatch_state.participants.front().state =
          OperationStateKind::kDispatching;
      dispatch_state.updated_at_ns = clock_.now();
      stamp_operation_capture_locked(*candidate, dispatch_state);
      candidate->state = dispatch_state;
    }
    const auto durable = attempt_journal_write([&] {
      return journal_.RecordOperationState(config_.request_owner_epoch, dispatch_state, nullptr,
                                           "motion_takeover_dispatch_committed");
    });
    if (!durable.ok()) {
      {
        std::lock_guard lock(state_mutex_);
        retain_operation_journal_write_fault_locked("motion_takeover_dispatch_committed",
                                                    durable.error(), &dispatch_state);
      }
      commit.release();
      return begin_motion_takeover_fallback(incumbent_id, candidate_id, durable.error());
    }
    {
      std::lock_guard lock(state_mutex_);
      auto* incumbent = find_active_operation_locked(incumbent_id);
      auto* candidate = find_pending_motion_operation_locked(candidate_id);
      if (!incumbent || !candidate || incumbent->state.active_stop_id ||
          candidate->state.preemption || motion_admission_closed_)
        return Result<void>::Ok();
      const auto scope = validate_program_execution_scope_locked(candidate->intent, false, false);
      if (!scope.ok()) return scope;
      if (auto attention =
              operator_alarm_attention_refusal_locked(candidate->participants.front().resources))
        return Result<void>::Err(std::move(*attention));
      candidate->participants.front().prepared_dispatch_released = true;
    }
    commit.release();
    const auto dispatched = prepared_engine->DispatchPreparedMotion(candidate_execution_id, digest);
    (void)std::fprintf(stderr,
        "manyforge_moving_owner_phase operation_id=%.160s execution_id=%llu "
        "observe_started_ns=%lld state_updated_ns=%lld observe_wall_ns=%lld "
        "commit_wait_wall_ns=%lld state_lock_wall_ns=%lld authority_wall_ns=%lld\n",
        candidate_id.value().c_str(), static_cast<unsigned long long>(candidate_execution_id),
        static_cast<long long>(observe_started), static_cast<long long>(dispatch_state.updated_at_ns),
        static_cast<long long>(observe_finished - observe_started),
        static_cast<long long>(commit_acquired - commit_started),
        static_cast<long long>(state_acquired - state_lock_started),
        static_cast<long long>(dispatch_state.updated_at_ns - state_acquired));
    if (!dispatched.ok())
      return begin_motion_takeover_fallback(incumbent_id, candidate_id, dispatched.error());
    return Result<void>::Ok();
  }
  if (!snapshot.replacement) return Result<void>::Ok();
  if (snapshot.replacement->phase == PreparedMotionReplacementPhaseDTO::kNoEffect ||
      snapshot.replacement->phase == PreparedMotionReplacementPhaseDTO::kEffectUncertain)
    return begin_motion_takeover_fallback(
        incumbent_id, candidate_id,
        snapshot.replacement->error.value_or(
            error("motion_takeover_not_committed",
                  "Takeover could not establish the exact physical handover",
                  "motion_takeover_not_committed")));
  if (snapshot.replacement->phase != PreparedMotionReplacementPhaseDTO::kReadyToCommit || !source)
    return Result<void>::Ok();

  struct Commit final {
    ResourceAddressedExecutionKernel* owner;
    ActiveOperation* incumbent;
    ActiveOperation* candidate;
    std::pair<OperationStateDTO, OperationStateDTO> captures;
    Result<void> allocation_failure;
  };
  OperationCommitGuard commit(*this, incumbent_id);
  std::unique_lock lock(state_mutex_);
  auto* incumbent = find_active_operation_locked(incumbent_id);
  auto* candidate = find_pending_motion_operation_locked(candidate_id);
  if (!incumbent || !candidate || incumbent->pending_motion_takeover.empty() ||
      incumbent->pending_motion_takeover.key() != candidate_id || incumbent->state.active_stop_id)
    return Result<void>::Ok();
  Commit context{this,
                 incumbent,
                 candidate,
                 {empty_takeover_capture_storage(), empty_takeover_capture_storage()},
                 reject<void>("takeover commit could not preallocate its state",
                              "motion_takeover_commit_capacity")};
  const PreparedMotionReplacementCommitDTO callback{
      &context, [](void* value) noexcept -> Result<void> {
        auto& state = *static_cast<Commit*>(value);
        try {
          return state.owner->commit_motion_takeover_owner_locked(
              *state.incumbent, *state.candidate, ResourceSlotOwnerTransferMode::kMoving,
              state.captures);
        } catch (...) {
          return std::move(state.allocation_failure);
        }
      }};
  const auto adopted =
      prepared_engine->CommitPreparedMotionReplacement(candidate_execution_id, *source, callback);
  lock.unlock();
  if (!adopted.ok()) {
    commit.release();
    return begin_motion_takeover_fallback(incumbent_id, candidate_id, adopted.error());
  }
  if (!adopted.value()) return Result<void>::Ok();
  for (const auto* state : {&context.captures.first, &context.captures.second}) {
    const auto written = attempt_journal_write([&] {
      return journal_.RecordOperationState(config_.request_owner_epoch, *state, nullptr,
                                           "motion_takeover_committed");
    });
    if (!written.ok()) {
      OperationId next_id{std::string{}};
      {
        std::lock_guard state_lock(state_mutex_);
        retain_operation_journal_write_fault_locked("motion_takeover_committed", written.error(),
                                                    state);
        if (const auto* current = find_active_operation_locked(candidate_id);
            current && !current->pending_motion_takeover.empty())
          next_id = current->pending_motion_takeover.key();
      }
      commit.release();
      if (!next_id.empty()) {
        const auto contained =
            begin_motion_takeover_fallback(candidate_id, next_id, written.error());
        if (!contained.ok()) return contained;
      }
      return Result<void>::Err(written.error());
    }
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::advance_motion_takeover(
    const OperationId& incumbent_id) {
  const auto advanced = advance_motion_takeover_impl(incumbent_id);
  if (advanced.ok()) return advanced;
  OperationId candidate_id{std::string{}};
  bool fallback_owned{};
  bool operator_stop{};
  {
    std::lock_guard lock(state_mutex_);
    const auto* incumbent = find_active_operation_locked(incumbent_id);
    if (!incumbent || incumbent->pending_motion_takeover.empty()) return advanced;
    const auto& candidate = *incumbent->pending_motion_takeover.mapped();
    candidate_id = candidate.state.operation_id;
    fallback_owned = candidate.motion_takeover_spring_fallback.has_value();
    operator_stop = incumbent->state.active_stop_id &&
                    (!fallback_owned || *incumbent->state.active_stop_id !=
                                            candidate.motion_takeover_spring_fallback->stop_id);
  }
  if (operator_stop) {
    const auto ended = end_pending_motion_takeover(incumbent_id, candidate_id, std::nullopt);
    return ended.ok() ? Result<void>::Ok() : Result<void>::Err(ended.error());
  }
  if (fallback_owned) {
    const auto ended = fail_pending_motion_takeover(incumbent_id, candidate_id, advanced.error());
    return ended.ok() ? Result<void>::Ok() : Result<void>::Err(ended.error());
  }
  return begin_motion_takeover_fallback(incumbent_id, candidate_id, advanced.error());
}

Result<bool> ResourceAddressedExecutionKernel::fail_pending_motion_takeover(
    const OperationId& incumbent_id, const OperationId& candidate_id, ErrorDTO failure) {
  return end_pending_motion_takeover(incumbent_id, candidate_id, std::move(failure));
}

Result<bool> ResourceAddressedExecutionKernel::end_pending_motion_takeover(
    const OperationId& incumbent_id, const OperationId& candidate_id,
    std::optional<ErrorDTO> failure) {
  IResolvedPreparedParticipantExecutionEngine* engine{};
  IResolvedParticipantExecutionEngine* local_engine{};
  std::uint64_t preparation_id{};
  bool no_effect{};
  bool source_settled{};
  bool reset_cleanup{};
  {
    std::lock_guard lock(state_mutex_);
    const auto* source = find_active_operation_locked(incumbent_id);
    if (!source || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != candidate_id)
      return Result<bool>::Ok(false);
    const auto& candidate = *source->pending_motion_takeover.mapped();
    const auto& row = candidate.participants.front();
    if (row.prepare_in_flight || row.dispatch_effect_in_flight) return Result<bool>::Ok(false);
    engine = row.prepared_motion_engine;
    local_engine = row.engine;
    preparation_id = row.preparation_id;
    reset_cleanup =
        source->pending_control &&
        source->pending_control->accepted.control == OperationControlKind::kReset &&
        source->pending_control->durable && source->pending_control->effects_dispatched &&
        source->pending_control->expected_effects > 0U && !source->pending_control->first_failure &&
        source->pending_control->completed_effects == source->pending_control->expected_effects;
    no_effect = row.prepared_engine_no_effect_confirmed ||
                (preparation_id == 0U && !row.prepared_dispatch_released);
    const auto* source_fact = selected_cause_terminal_fact(source->state.participants.front());
    source_settled = source->state.active_stop_id && source->settled_stop &&
                     source->settled_stop->stop_id == source->state.active_stop_id &&
                     source->settled_stop->generation == source->state.active_stop_generation &&
                     source->settled_stop->certificate && source_fact &&
                     source_fact->effect == ParticipantEffectKnowledge::kEffectSettled &&
                     terminal(source->state.participants.front().state);
    // Accepted Reset completion already retired pending membership first in
    // the existing Kernel cleanup. Its explicit effect result is authority;
    // a subsequent Missing observation is deliberately not consulted.
    source_settled = source_settled || reset_cleanup;
    // A previous successful cleanup remains represented by the terminal fact,
    // even if its journal publication subsequently faulted. Do not recreate a
    // retired membership or reinterpret Missing as cancellation evidence.
    const auto* candidate_fact = selected_cause_terminal_fact(candidate.state.participants.front());
    if (preparation_id == 0U && candidate_fact)
      source_settled =
          source_settled || candidate_fact->effect == ParticipantEffectKnowledge::kEffectSettled;
    if (!source_settled && candidate_fact &&
        candidate_fact->effect == ParticipantEffectKnowledge::kEffectUncertain &&
        source->next_motion_takeover.empty())
      return Result<bool>::Ok(false);
  }
  if (preparation_id != 0U && !reset_cleanup) {
    if (!engine)
      return reject<bool>("pending cleanup lost its exact engine", "motion_takeover_owner_missing",
                          ErrorCode::kInvariantViolation);
    // Sent replacements refuse pre-dispatch cancellation. Their exact Stop
    // owner still runs independently; retirement below requires its proof.
    const auto cancelled = engine->CancelPreparedMotion(preparation_id);
    if (!cancelled.ok() && !source_settled && !failure) return Result<bool>::Ok(false);
    const auto observed = engine->ObservePreparedMotion(preparation_id);
    if (!observed.ok()) return Result<bool>::Err(observed.error());
    if (observed.value().execution_id != preparation_id)
      return reject<bool>("pending cleanup observation changed identity",
                          "motion_takeover_observation_stale");
    // Only this owner's physical observation can prove NoEffect. An attached
    // spring_cold_result describes unsent computation while the abandoned
    // presentation may still owe exact Stop/backend retirement.
    no_effect =
        no_effect ||
        observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch ||
        observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
        (observed.value().replacement &&
         observed.value().replacement->phase == PreparedMotionReplacementPhaseDTO::kNoEffect);
    if (no_effect || source_settled) {
      // May join a completed computation. Never hold the addressed writer or
      // state mutex over this call; an on-stack candidate callback returns false.
      bool local_closed = false;
      if (no_effect && local_engine) {
        const auto assessment = local_engine->get_recovery_assessment();
        if (assessment.execution_id == preparation_id) {
          // Cold activation may already have made Y the ordinary local
          // session before the parent's settled owner CAS. Reuse its exact
          // motion-free close; pending-only retirement cannot close it.
          if (!assessment.eligible_close) return Result<bool>::Ok(false);
          KernelRecoveryActionRequestDTO request;
          request.request_id = content_identity::sha256_digest(
              config_.request_owner_epoch.value() + "\npending-takeover-close\n" +
              candidate_id.value() + "\n" + std::to_string(preparation_id));
          request.action = RecoveryAction::kCloseOperation;
          request.principal_id = "kernel.motion_takeover";
          request.reason = "cancelled_before_owner_transfer";
          bind_recovery_request(request, assessment);
          const auto closed = local_engine->apply_recovery_action(request);
          if (closed.request_id != request.request_id || closed.action != request.action)
            return reject<bool>("candidate local close changed identity",
                                "motion_takeover_close_identity_mismatch",
                                ErrorCode::kInvariantViolation);
          if (!closed.accepted) return Result<bool>::Ok(false);
          local_closed = true;
        }
      }
      if (!local_closed) {
        const auto retired = engine->RetireCancelledPreparedMotion(preparation_id);
        if (!retired.ok()) return Result<bool>::Err(retired.error());
        if (!retired.value()) return Result<bool>::Ok(false);
      }
    }
  }

  OperationCommitGuard commit(*this, incumbent_id);
  std::vector<OperationStateDTO> captures;
  bool terminal_cleanup = no_effect || source_settled;
  bool promote_next{};
  {
    std::lock_guard lock(state_mutex_);
    auto* source = find_active_operation_locked(incumbent_id);
    if (!source || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != candidate_id)
      return Result<bool>::Ok(false);
    auto& candidate = *source->pending_motion_takeover.mapped();
    auto& row = candidate.participants.front();
    if (reset_cleanup &&
        (!source->pending_control ||
         source->pending_control->accepted.control != OperationControlKind::kReset ||
         !source->pending_control->durable || !source->pending_control->effects_dispatched ||
         source->pending_control->expected_effects == 0U ||
         source->pending_control->first_failure ||
         source->pending_control->completed_effects != source->pending_control->expected_effects))
      return Result<bool>::Ok(false);
    if (row.preparation_id != preparation_id || row.prepare_in_flight)
      return reject<bool>("pending owner changed during cleanup",
                          "motion_takeover_candidate_stale");
    if (operation_journal_write_fault_ && terminal(candidate.state.state))
      return Result<bool>::Ok(false);  // A failed capture is never appended again.
    auto state = candidate.state;
    auto& participant = state.participants.front();
    const auto effect = no_effect        ? ParticipantEffectKnowledge::kNoEffect
                        : source_settled ? ParticipantEffectKnowledge::kEffectSettled
                                         : ParticipantEffectKnowledge::kEffectUncertain;
    state.state = participant.state =
        terminal_cleanup ? (failure ? OperationStateKind::kFailed : OperationStateKind::kStopped)
                         : OperationStateKind::kInterventionRequired;
    participant.terminal_error = failure;
    if (!terminal_cleanup && !participant.terminal_error)
      participant.terminal_error =
          error("motion_takeover_cleanup_unsettled", "The pending goal has not been proved ended",
                "motion_takeover_cleanup_unsettled");
    participant.terminal_fact = ParticipantExecutionFact{
        participant.terminal_error ? ParticipantExecutionFactKind::kLocalFailure
        : no_effect                ? ParticipantExecutionFactKind::kCancelledBeforeDispatch
                                   : ParticipantExecutionFactKind::kPreemptedByAddressedStop,
        effect, participant.terminal_error};
    state.active_stop_id = source->state.active_stop_id;
    state.active_stop_generation = source->state.active_stop_generation;
    state.updated_at_ns = clock_.now();
    stamp_operation_capture_locked(candidate, state);
    captures.push_back(state);
    // A failed predecessor never prepares its next slot. The next identity has
    // no engine membership and receives its own factual NoEffect failure.
    const bool ordinary_stop =
        source->state.active_stop_id &&
        (!candidate.motion_takeover_spring_fallback ||
         source->state.active_stop_id != candidate.motion_takeover_spring_fallback->stop_id);
    const bool end_next = failure.has_value() || ordinary_stop || reset_cleanup;
    if (end_next && !source->next_motion_takeover.empty()) {
      auto& next = *source->next_motion_takeover.mapped();
      auto next_state = next.state;
      auto& next_row = next_state.participants.front();
      next_state.state = next_row.state =
          failure ? OperationStateKind::kFailed : OperationStateKind::kStopped;
      next_row.terminal_error =
          failure ? std::optional(error("predecessor_handover_failed",
                                        "The preceding accepted handover did not commit",
                                        "predecessor_handover_failed"))
                  : std::nullopt;
      next_row.terminal_fact =
          ParticipantExecutionFact{failure ? ParticipantExecutionFactKind::kLocalFailure
                                           : ParticipantExecutionFactKind::kCancelledBeforeDispatch,
                                   ParticipantEffectKnowledge::kNoEffect, next_row.terminal_error};
      next_state.active_stop_id = source->state.active_stop_id;
      next_state.active_stop_generation = source->state.active_stop_generation;
      next_state.updated_at_ns = clock_.now();
      stamp_operation_capture_locked(next, next_state);
      captures.push_back(next_state);
      next.state = std::move(next_state);
    }
    candidate.state = std::move(state);
    if (terminal_cleanup) {
      row.prepared_engine_no_effect_confirmed = no_effect;
      row.preparation_id = row.subordinate_id = 0U;
      row.prepared_participant.reset();
    }
    promote_next = terminal_cleanup && !failure && !reset_cleanup &&
                   !source->state.active_stop_id && !source->next_motion_takeover.empty();
  }
  for (const auto& state : captures) {
    const auto written = attempt_journal_write([&] {
      return journal_.RecordOperationState(config_.request_owner_epoch, state, nullptr,
                                           "motion_takeover_ended");
    });
    if (!written.ok()) {
      std::lock_guard lock(state_mutex_);
      retain_operation_journal_write_fault_locked("motion_takeover_ended", written.error(), &state);
      return Result<bool>::Err(written.error());
    }
  }
  {
    std::lock_guard lock(state_mutex_);
    auto* source = find_active_operation_locked(incumbent_id);
    if (!source || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != candidate_id)
      return Result<bool>::Ok(false);
    if (captures.size() == 2U && !source->next_motion_takeover.empty()) {
      retain_historical_operation_locked(source->next_motion_takeover.mapped()->state);
      source->next_motion_takeover = {};
    }
    if (!terminal_cleanup) return Result<bool>::Ok(false);
    retain_historical_operation_locked(source->pending_motion_takeover.mapped()->state);
    source->pending_motion_takeover = {};
    if (promote_next) source->pending_motion_takeover = std::move(source->next_motion_takeover);
  }
  return Result<bool>::Ok(true);
}

Result<bool> ResourceAddressedExecutionKernel::commit_settled_motion_takeover(
    const OperationId& incumbent_id, const OperationId& candidate_id,
    const StopCertificateDTO& certificate) {
  IResolvedPreparedParticipantExecutionEngine* engine{};
  std::uint64_t preparation_id{};
  {
    std::lock_guard lock(state_mutex_);
    const auto* source = find_active_operation_locked(incumbent_id);
    if (!source || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != candidate_id)
      return Result<bool>::Ok(false);
    const auto& candidate = *source->pending_motion_takeover.mapped();
    engine = candidate.participants.front().prepared_motion_engine;
    preparation_id = candidate.participants.front().preparation_id;
  }
  if (!engine || preparation_id == 0U)
    return reject<bool>("cold takeover has no prepared membership",
                        "motion_takeover_owner_missing");
  const auto observed = engine->ObservePreparedMotion(preparation_id);
  if (!observed.ok()) return Result<bool>::Err(observed.error());
  if (observed.value().execution_id != preparation_id ||
      observed.value().state != ResolvedPreparedMotionExecutionStateDTO::kPrepared ||
      !observed.value().participant || !observed.value().backend_prepare_result)
    return Result<bool>::Ok(false);
  OperationCommitGuard commit(*this, incumbent_id);
  std::pair<OperationStateDTO, OperationStateDTO> captures{empty_takeover_capture_storage(),
                                                           empty_takeover_capture_storage()};
  IResolvedParticipantExecutionEngine* ordinary_engine{};
  {
    std::lock_guard lock(state_mutex_);
    auto* source = find_active_operation_locked(incumbent_id);
    auto* candidate = find_pending_motion_operation_locked(candidate_id);
    if (!source || !candidate || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != candidate_id || !source->settled_stop ||
        !source->settled_stop->certificate ||
        source->settled_stop->certificate->stop_certificate_id != certificate.stop_certificate_id ||
        source->settled_stop->certificate->stop_certificate_revision !=
            certificate.stop_certificate_revision ||
        source->state.active_stop_id != certificate.stop_id ||
        source->state.active_stop_generation != certificate.stop_generation ||
        certificate.result != StopCertificateResultDTO::kSucceededNominal ||
        !candidate->motion_takeover_spring_fallback ||
        candidate->motion_takeover_spring_fallback->stop_id != certificate.stop_id ||
        !candidate->motion_takeover_spring_fallback->stage ||
        !candidate->motion_takeover_spring_fallback->return_point ||
        !candidate->motion_takeover_spring_fallback->certificate ||
        *candidate->motion_takeover_spring_fallback->certificate != certificate)
      return reject<bool>("cold takeover lost its exact spring settlement",
                          "motion_takeover_source_not_settled");
    auto& row = candidate->participants.front();
    const auto& product = *observed.value().participant;
    if (product.controller().ordinary_stop_plan != *row.ordinary_stop_plan ||
        !prepared_fault_stop_plan_matches(product.controller().fault_stop_plan,
                                          row.fault_stop_plan) ||
        prepare_participant_result_field_error(*observed.value().backend_prepare_result, product))
      return reject<bool>("cold product differs from its resolved authority",
                          "prepared_motion_product_mismatch", ErrorCode::kInvariantViolation);
    row.prepared_participant = product;
    ordinary_engine = row.engine;
    const auto transferred = commit_motion_takeover_owner_locked(
        *source, *candidate, ResourceSlotOwnerTransferMode::kSettled, captures);
    if (!transferred.ok()) return Result<bool>::Err(transferred.error());
    row.prepared_advance_in_flight = true;
  }
  const ScopeExit release_candidate_borrow([&] {
    std::lock_guard lock(state_mutex_);
    auto* candidate = find_active_operation_locked(candidate_id);
    auto* row = candidate ? candidate->participant_for(ordinary_engine) : nullptr;
    if (row && row->preparation_id == preparation_id) {
      row->prepared_advance_in_flight = false;
      settle_completed_control_locked(candidate_id);
      wake_stop_after_preparation_borrow_locked(candidate_id);
    }
  });
  for (const auto* state : {&captures.first, &captures.second}) {
    const auto written = attempt_journal_write([&] {
      return journal_.RecordOperationState(config_.request_owner_epoch, *state, nullptr,
                                           "motion_takeover_cold_committed");
    });
    if (!written.ok()) {
      std::lock_guard lock(state_mutex_);
      retain_operation_journal_write_fault_locked("motion_takeover_cold_committed", written.error(),
                                                  state);
      return Result<bool>::Err(written.error());
    }
  }
  commit.release();
  const auto advanced = release_prepared_dispatch_barrier(
      candidate_id, ordinary_engine, preparation_id, *observed.value().participant);
  return advanced.ok() ? Result<bool>::Ok(true) : Result<bool>::Err(advanced.error());
}

Result<void> ResourceAddressedExecutionKernel::advance_unsent_motion_takeover(
    const OperationId& incumbent_id) {
  IResolvedParticipantExecutionEngine* engine{};
  IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
  std::uint64_t source_id{};
  OperationId candidate_id{std::string{}};
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> authority;
  bool source_closed{};
  {
    std::lock_guard lock(state_mutex_);
    const auto* source = find_active_operation_locked(incumbent_id);
    if (!source || source->pending_motion_takeover.empty()) return Result<void>::Ok();
    const auto& old = source->participants.front();
    const auto& candidate = *source->pending_motion_takeover.mapped();
    const auto* old_fact = selected_cause_terminal_fact(source->state.participants.front());
    const bool known_no_effect =
        old.prepared_engine_no_effect_confirmed ||
        (old_fact && old_fact->effect == ParticipantEffectKnowledge::kNoEffect);
    if (source->state.active_stop_id || (old.prepared_dispatch_released && !known_no_effect) ||
        old.prepare_in_flight || old.prepared_advance_in_flight || old.reconciliation_in_flight)
      return Result<void>::Ok();
    engine = old.engine;
    prepared_engine = old.prepared_motion_engine;
    source_id = old.preparation_id;
    source_closed = old.prepared_engine_no_effect_confirmed && source_id == 0U;
    candidate_id = candidate.state.operation_id;
    authority = candidate.participants.front().takeover_preparation;
  }
  if (!engine || !prepared_engine || !authority)
    return reject<void>("unsent takeover lost its local authority",
                        "motion_takeover_owner_missing");
  std::optional<ParticipantExecutionFact> source_fact;
  if (!source_closed) {
    if (source_id == 0U)
      return Result<void>::Ok();  // The original Prepare borrow has not returned.
    auto status = engine->get_status();
    if (status.execution_id != source_id || !status.fact ||
        status.fact->effect != ParticipantEffectKnowledge::kNoEffect) {
      const auto cancelled = prepared_engine->CancelPreparedMotion(source_id);
      if (!cancelled.ok()) return cancelled;
      status = engine->get_status();
    }
    if (status.execution_id != source_id || !status.fact ||
        status.fact->effect != ParticipantEffectKnowledge::kNoEffect)
      return Result<void>::Ok();
    source_fact = status.fact;
    const auto assessment = engine->get_recovery_assessment();
    if (assessment.execution_id != source_id || !assessment.eligible_close)
      return Result<void>::Ok();
    KernelRecoveryActionRequestDTO request;
    request.request_id = content_identity::sha256_digest(
        config_.request_owner_epoch.value() + "\nunsent-takeover-close\n" + incumbent_id.value());
    request.action = RecoveryAction::kCloseOperation;
    request.principal_id = "kernel.motion_takeover";
    request.reason = "superseded_before_dispatch";
    bind_recovery_request(request, assessment);
    // Common local motion-free closure owns joins and membership cleanup. It
    // may return not eligible while the worker's terminal publication unwinds.
    const auto closed = engine->apply_recovery_action(request);
    if (closed.request_id != request.request_id || closed.action != request.action)
      return reject<void>("unsent local closure changed request identity",
                          "motion_takeover_close_identity_mismatch",
                          ErrorCode::kInvariantViolation);
    if (!closed.accepted) return Result<void>::Ok();
  }

  OperationCommitGuard commit(*this, incumbent_id);
  std::pair<OperationStateDTO, OperationStateDTO> captures{empty_takeover_capture_storage(),
                                                           empty_takeover_capture_storage()};
  {
    std::lock_guard lock(state_mutex_);
    auto* source = find_active_operation_locked(incumbent_id);
    auto* candidate = find_pending_motion_operation_locked(candidate_id);
    if (!source || !candidate || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != candidate_id)
      return Result<void>::Ok();
    auto& old = source->participants.front();
    if (old.preparation_id != source_id || old.prepare_in_flight ||
        (old.prepared_dispatch_released && !old.prepared_engine_no_effect_confirmed &&
         (!source_fact || source_fact->effect != ParticipantEffectKnowledge::kNoEffect)))
      return reject<void>("unsent source changed during local closure",
                          "motion_takeover_source_stale");
    // Preserve the actual terminal reason reported before local cleanup.
    // Later passes use this retained fact, never an Idle/Missing inference.
    if (source_fact) {
      auto& row = source->state.participants.front();
      row.state = source->state.state =
          source_fact->error ? OperationStateKind::kFailed : OperationStateKind::kStopped;
      row.terminal_fact = *source_fact;
      row.terminal_error = source_fact->error;
      source->state.updated_at_ns = clock_.now();
      stamp_operation_capture_locked(*source, source->state);
    }
    old.prepared_engine_no_effect_confirmed = true;
    old.preparation_id = old.subordinate_id = 0U;
    if (source->state.active_stop_id) {
      old.stop_cancelled_pre_dispatch = true;
      wake_stop_after_preparation_borrow_locked(incumbent_id);
      return Result<void>::Ok();
    }
    if (old.slot_state != ResourceSlotState::kTerminal) {
      const auto terminal_slot =
          resource_slots_->transition(old.slot_owner, old.resources, ResourceSlotState::kTerminal);
      if (!terminal_slot.ok()) return terminal_slot;
      old.slot_state = ResourceSlotState::kTerminal;
    }
    const auto transferred = commit_motion_takeover_owner_locked(
        *source, *candidate, ResourceSlotOwnerTransferMode::kSettled, captures);
    if (!transferred.ok()) return transferred;
    candidate->participants.front().prepare_in_flight = true;
  }
  const ScopeExit release_candidate_borrow([&] {
    std::lock_guard lock(state_mutex_);
    if (auto* candidate = find_active_operation_locked(candidate_id)) {
      candidate->participants.front().prepare_in_flight = false;
      wake_stop_after_preparation_borrow_locked(candidate_id);
      settle_completed_control_locked(candidate_id);
    }
  });
  for (const auto* state : {&captures.first, &captures.second}) {
    const auto written = attempt_journal_write([&] {
      return journal_.RecordOperationState(config_.request_owner_epoch, *state, nullptr,
                                           "motion_takeover_unsent_committed");
    });
    if (!written.ok()) {
      std::lock_guard lock(state_mutex_);
      if (auto* candidate = find_active_operation_locked(candidate_id))
        candidate->participants.front().prepare_in_flight = false;
      retain_operation_journal_write_fault_locked("motion_takeover_unsent_committed",
                                                  written.error(), state);
      return Result<void>::Err(written.error());
    }
  }
  commit.release();
  authority->replacement.reset();
  const auto prepared = [&]() -> Result<std::uint64_t> {
    try {
      return prepared_engine->PrepareMotion(std::move(*authority));
    } catch (...) {
      return reject<std::uint64_t>("Ordinary successor preparation raised before dispatch",
                                   "motion_takeover_prepare_exception");
    }
  }();
  bool cancel_late_preparation{};
  {
    std::lock_guard lock(state_mutex_);
    auto* candidate = find_active_operation_locked(candidate_id);
    if (!candidate)
      return reject<void>("borrowed ordinary preparation lost its owner",
                          "motion_takeover_candidate_stale", ErrorCode::kInvariantViolation);
    auto& row = candidate->participants.front();
    cancel_late_preparation = candidate->terminal_stop_result().has_value();
    row.prepare_in_flight = cancel_late_preparation;
    if (prepared.ok()) {
      if (row.subordinate_id != 0U && row.subordinate_id != prepared.value())
        return reject<void>("ordinary candidate event changed preparation identity",
                            "subordinate_execution_id_mismatch", ErrorCode::kInvariantViolation);
      row.preparation_id = row.subordinate_id = prepared.value();
    }
    if (!cancel_late_preparation) wake_stop_after_preparation_borrow_locked(candidate_id);
  }
  if (cancel_late_preparation && prepared.ok()) {
    const auto cancelled = prepared_engine->CancelPreparedMotion(prepared.value());
    if (!cancelled.ok()) return cancelled;
  }
  {
    std::lock_guard lock(state_mutex_);
    if (auto* candidate = find_active_operation_locked(candidate_id)) {
      candidate->participants.front().prepare_in_flight = false;
      wake_stop_after_preparation_borrow_locked(candidate_id);
    }
  }
  if (!prepared.ok()) {
    OperationId next_id{std::string{}};
    {
      std::lock_guard lock(state_mutex_);
      if (const auto* candidate = find_active_operation_locked(candidate_id);
          candidate && !candidate->pending_motion_takeover.empty())
        next_id = candidate->pending_motion_takeover.key();
    }
    if (!next_id.empty()) {
      const auto failed = fail_pending_motion_takeover(
          candidate_id, next_id,
          error("predecessor_handover_failed", "The preceding handover could not prepare",
                "predecessor_handover_failed"));
      if (!failed.ok()) return Result<void>::Err(failed.error());
      if (!failed.value()) return Result<void>::Ok();
    }
    return fail_pre_dispatch(candidate_id, nullptr, prepared.error());
  }
  return advance_prepared_motion(candidate_id, engine);
}

}  // namespace manyforge::core
