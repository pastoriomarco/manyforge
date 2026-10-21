#include "resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {
namespace {
[[nodiscard]] bool sealed_spring_return_allowed(const ActiveOperationParticipant& participant) {
  if (!participant.ordinary_stop_plan || participant.ordinary_stop_plan->stage_commands.empty())
    return false;
  const auto& command = participant.ordinary_stop_plan->stage_commands.front();
  if (command.resolved_stage.containment_command.command_kind !=
          StopContainmentCommandKind::kMotionControlledStop ||
      !command.validated_contract_parameters)
    return false;
  const auto profile = map_motion_controlled_stop_contract_parameters_to_profile(
      *command.validated_contract_parameters, participant.binding.value().resources,
      "motion_takeover_fallback");
  if (!profile.ok()) return false;
  const auto spring = [](const MotionInterruptStopMode mode) {
    return mode == MotionInterruptStopMode::kSpringBackHold ||
           mode == MotionInterruptStopMode::kSpringBackFast;
  };
  return spring(profile.value().stop_mode) ||
         std::any_of(profile.value().fallback_stop_modes.begin(),
                     profile.value().fallback_stop_modes.end(), spring);
}
}  // namespace

Result<void> ResourceAddressedExecutionKernel::begin_motion_takeover_fallback(
    const OperationId& incumbent_id, const OperationId& candidate_id, ErrorDTO cause) {
  StopWorkHandoff handoff;
  const ScopeExit publish([&] { publish_stop_work(handoff); });
  std::lock_guard lock(state_mutex_);
  auto* source = find_active_operation_locked(incumbent_id);
  if (!source || source->pending_motion_takeover.empty() ||
      source->pending_motion_takeover.key() != candidate_id)
    return reject<void>("Takeover fallback lost its accepted candidate",
                        "motion_takeover_candidate_stale");
  auto& candidate = *source->pending_motion_takeover.mapped();
  if (candidate.motion_takeover_spring_fallback) return Result<void>::Ok();
  if (source->participants.size() != 1U || source->state.active_stop_id ||
      source->stop_effect_in_flight)
    return reject<void>("Another Stop already owns the takeover source",
                        "motion_takeover_stop_selection_stale");
  const auto& participant = source->participants.front();
  // Genuine faults retain their ordinary containment route; no successor may
  // activate while admission is closed. The same Stop owner still stops the
  // logically preempted incumbent.
  const bool spring = !motion_admission_closed_ &&
                      !is_active_stop_fault_latch(active_stop_fault_latch_) &&
                      sealed_spring_return_allowed(participant);
  const MotionTakeoverStopSelection selection{incumbent_id, candidate_id, spring};
  const auto material = config_.request_owner_epoch.value() + "\nmotion-takeover-fallback\n" +
                        incumbent_id.value() + "\n" + candidate_id.value();
  StopIntentDTO intent{PublicRequestContextDTO{RequestId(content_identity::sha256_digest(material)),
                                               config_.request_owner_epoch,
                                               "kernel.motion_takeover", candidate_id.value()},
                       std::vector<StopOperationTargetDTO>{
                           {incumbent_id, source->state.operation_revision,
                            source->state.active_stop_generation.value_or(StopGeneration(0U))}},
                       std::string("motion_takeover_fallback"),
                       RequestAttributionDTO{RequestTransportDTO::kInternal,
                                             RequestAssuranceDTO::kUnverified, std::nullopt}};
  const auto decided_at_ns = clock_.now();
  const auto previous_capture_order = candidate.state.journal_capture_order;
  auto previous_decision = std::move(candidate.state.motion_takeover);
  candidate.state.motion_takeover = MotionTakeoverDecisionDTO{
      incumbent_id,
      spring ? MotionTakeoverPathDTO::kSpringBackHold
             : MotionTakeoverPathDTO::kOrdinaryContainment,
      cause,
      decided_at_ns,
      std::max<std::int64_t>(0, decided_at_ns - candidate.accepted.accepted_at_ns),
      candidate.participants.front().takeover_preparation_budget_ns.value_or(0),
      std::nullopt};
  stamp_operation_capture_locked(candidate, candidate.state);
  bool fallback_accepted = false;
  const ScopeExit restore_unaccepted_decision([&] {
    if (!fallback_accepted) {
      candidate.state.motion_takeover = std::move(previous_decision);
      candidate.state.journal_capture_order = previous_capture_order;
    }
  });
  const auto stopped =
      accept_stop_locked(EmergencyStopProjectedIntentDTO{std::move(intent)}, handoff, decided_at_ns,
                         nullptr, nullptr, std::nullopt, &selection);
  if (!stopped.ok()) return Result<void>::Err(stopped.error());
  fallback_accepted = true;
  candidate.motion_takeover_spring_fallback.emplace(
      MotionTakeoverSpringFallbackDTO{stopped.value().stop_id,
                                      stopped.value().stop_generation,
                                      {},
                                      {},
                                      std::move(cause),
                                      candidate.participants.front().preparation_id});
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::advance_motion_takeover_fallback(
    const OperationId& incumbent_id, const OperationId& candidate_id) {
  IResolvedPreparedParticipantExecutionEngine* engine{};
  std::uint64_t preparation_id{};
  bool retire_previous{};
  bool attach_to_sent{};
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> prepare;
  std::optional<StopCertificateDTO> certificate;
  std::optional<ErrorDTO> refusal;
  {
    std::lock_guard lock(state_mutex_);
    auto* source = find_active_operation_locked(incumbent_id);
    if (!source || source->pending_motion_takeover.empty() ||
        source->pending_motion_takeover.key() != candidate_id)
      return Result<void>::Ok();
    auto& candidate = *source->pending_motion_takeover.mapped();
    if (!candidate.motion_takeover_spring_fallback || candidate.participants.size() != 1U)
      return reject<void>("Takeover fallback lost its immutable context",
                          "motion_takeover_fallback_context_missing");
    const auto& fallback = *candidate.motion_takeover_spring_fallback;
    if (source->state.active_stop_id != fallback.stop_id ||
        source->state.active_stop_generation != fallback.stop_generation)
      return reject<void>("A newer Stop superseded takeover fallback",
                          "motion_takeover_stop_selection_stale");
    auto& participant = candidate.participants.front();
    engine = participant.prepared_motion_engine;
    preparation_id = participant.preparation_id;
    retire_previous = preparation_id != 0U && preparation_id == fallback.abandoned_preparation_id;
    attach_to_sent = retire_previous && participant.prepared_dispatch_released;
    if (source->settled_stop && source->settled_stop->stop_id == fallback.stop_id &&
        source->settled_stop->generation == fallback.stop_generation &&
        source->settled_stop->certificate) {
      if (fallback.certificate && fallback.certificate->stop_certificate_id ==
                                      source->settled_stop->certificate->stop_certificate_id)
        certificate = *fallback.certificate;
    }
    if (certificate && (!fallback.stage || !fallback.return_point || motion_admission_closed_ ||
                        is_active_stop_fault_latch(active_stop_fault_latch_)))
      refusal = fallback.smooth_failure;
    if (engine == nullptr || !participant.takeover_preparation ||
        !participant.takeover_preparation_budget_ns)
      refusal = error(std::string(kErrorExecutionRejected),
                      "Spring return has no admitted successor preparation authority",
                      "motion_takeover_fallback_preparation_unavailable");
    // Reserve while the conservative submission-gate fact still describes the
    // old key. Only the settled owner CAS may publish this future authority.
    // Cleanup can precede point publication; key reservation needs no point.
    if (!refusal) {
      const auto reserved = reserve_cold_motion_takeover_goal_locked(candidate);
      if (!reserved.ok()) refusal = reserved.error();
    }
    if (!refusal && preparation_id == 0U && !participant.prepare_in_flight && fallback.stage &&
        fallback.return_point) {
      prepare = *participant.takeover_preparation;
      prepare->replacement = ResolvedPreparedMotionReplacementContextDTO{
          {},
          *participant.takeover_preparation_budget_ns,
          ResolvedPreparedMotionSpringReturnContextDTO{fallback.stage, fallback.return_point}};
      participant.prepare_in_flight = true;
    }
  }
  if (retire_previous && engine != nullptr && !refusal) {
    const auto old = engine->ObservePreparedMotion(preparation_id);
    if (!old.ok()) return Result<void>::Err(old.error());
    // A proved rejected send can use the existing immediate cleanup path.
    // The fresh key reservation above still follows the crossed parent gate.
    attach_to_sent = attach_to_sent &&
                     !(old.value().replacement && old.value().replacement->phase ==
                                                      PreparedMotionReplacementPhaseDTO::kNoEffect);
    if (old.value().spring_cold_result) {
      const auto& cold = *old.value().spring_cold_result;
      if (cold.state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
          cold.state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch) {
        const auto failed = fail_pending_motion_takeover(
            incumbent_id, candidate_id,
            cold.error.value_or(error(std::string(kErrorExecutionRejected),
                                      "Attached spring successor computation failed",
                                      "motion_takeover_fallback_preparation_failed")));
        return failed.ok() ? Result<void>::Ok() : Result<void>::Err(failed.error());
      }
      if (!certificate || cold.state != ResolvedPreparedMotionExecutionStateDTO::kColdPrepared)
        return Result<void>::Ok();
      {
        std::lock_guard lock(state_mutex_);
        auto* source = find_active_operation_locked(incumbent_id);
        if (!source || source->pending_motion_takeover.empty() ||
            source->pending_motion_takeover.key() != candidate_id)
          return Result<void>::Ok();
        auto& candidate = *source->pending_motion_takeover.mapped();
        const auto& fallback = *candidate.motion_takeover_spring_fallback;
        if (source->state.active_stop_id != fallback.stop_id ||
            source->state.active_stop_generation != fallback.stop_generation)
          return reject<void>("A newer Stop superseded cold retirement",
                              "motion_takeover_stop_selection_stale");
        auto& participant = candidate.participants.front();
        if (participant.prepare_in_flight) return Result<void>::Ok();
        participant.prepare_in_flight = true;
      }
      // Useful work must not be cancelled to dispose of the old sent effect.
      // Retirement itself verifies the source Stop and actual backend debt.
      const auto retired = engine->RetireCancelledPreparedMotion(preparation_id);
      bool superseded = false;
      {
        std::lock_guard lock(state_mutex_);
        auto* source = find_active_operation_locked(incumbent_id);
        if (!source || source->pending_motion_takeover.empty() ||
            source->pending_motion_takeover.key() != candidate_id) {
          motion_admission_closed_ = true;
          return reject<void>("Borrowed spring retirement lost its owner",
                              "motion_takeover_preparation_owner_lost",
                              ErrorCode::kInvariantViolation);
        }
        auto& candidate = *source->pending_motion_takeover.mapped();
        auto& participant = candidate.participants.front();
        participant.prepare_in_flight = false;
        if (retired.ok() && retired.value()) {
          // Publish the actual promoted identity even if operator Stop won
          // during I/O, so ordinary cleanup addresses the real remaining data.
          participant.preparation_id = cold.reserved_execution_id;
          participant.subordinate_id = cold.reserved_execution_id;
          participant.prepared_participant.reset();
          participant.prepared_dispatch_released = false;
          const auto& fallback = *candidate.motion_takeover_spring_fallback;
          superseded = source->state.active_stop_id != fallback.stop_id ||
                       source->state.active_stop_generation != fallback.stop_generation;
        }
      }
      if (!retired.ok()) return Result<void>::Err(retired.error());
      if (!retired.value()) return Result<void>::Ok();
      if (superseded) {
        const auto failed = fail_pending_motion_takeover(
            incumbent_id, candidate_id,
            error(std::string(kErrorExecutionRejected), "Operator Stop superseded cold promotion",
                  "motion_takeover_stop_selection_stale"));
        return failed.ok() ? Result<void>::Ok() : Result<void>::Err(failed.error());
      }
      return advance_motion_takeover_fallback(incumbent_id, candidate_id);
    }
    if (attach_to_sent) {
      std::lock_guard lock(state_mutex_);
      auto* source = find_active_operation_locked(incumbent_id);
      if (!source || source->pending_motion_takeover.empty() ||
          source->pending_motion_takeover.key() != candidate_id)
        return Result<void>::Ok();
      auto& candidate = *source->pending_motion_takeover.mapped();
      auto& participant = candidate.participants.front();
      const auto& fallback = *candidate.motion_takeover_spring_fallback;
      if (!fallback.stage || !fallback.return_point || participant.prepare_in_flight)
        return Result<void>::Ok();
      if (source->state.active_stop_id != fallback.stop_id ||
          source->state.active_stop_generation != fallback.stop_generation)
        return reject<void>("A newer Stop superseded cold admission",
                            "motion_takeover_stop_selection_stale");
      prepare = *participant.takeover_preparation;
      prepare->replacement = ResolvedPreparedMotionReplacementContextDTO{
          {},
          *participant.takeover_preparation_budget_ns,
          ResolvedPreparedMotionSpringReturnContextDTO{fallback.stage, fallback.return_point,
                                                       preparation_id}};
      participant.prepare_in_flight = true;
    } else {
      const auto cancelled = engine->CancelPreparedMotion(preparation_id);
      if (!cancelled.ok()) return cancelled;
      const auto retired = engine->RetireCancelledPreparedMotion(preparation_id);
      if (!retired.ok()) return Result<void>::Err(retired.error());
      if (!retired.value()) return Result<void>::Ok();
      {
        std::lock_guard lock(state_mutex_);
        auto* source = find_active_operation_locked(incumbent_id);
        if (!source || source->pending_motion_takeover.empty() ||
            source->pending_motion_takeover.key() != candidate_id)
          return Result<void>::Ok();
        auto& participant = source->pending_motion_takeover.mapped()->participants.front();
        if (participant.preparation_id != preparation_id)
          return reject<void>("Takeover preparation changed while its cleanup returned",
                              "motion_takeover_preparation_stale");
        participant.preparation_id = 0U;
        participant.subordinate_id = 0U;
        participant.prepared_participant.reset();
        participant.prepared_dispatch_released = false;
      }
      return advance_motion_takeover_fallback(incumbent_id, candidate_id);
    }
  }
  if (refusal) {
    const auto failed = fail_pending_motion_takeover(incumbent_id, candidate_id, *refusal);
    return failed.ok() ? Result<void>::Ok() : Result<void>::Err(failed.error());
  }
  if (prepare) {
    auto prepared = [&]() -> Result<std::uint64_t> {
      try {
        return engine->PrepareMotion(std::move(*prepare));
      } catch (...) {
        return reject<std::uint64_t>("Spring successor preparation raised before dispatch",
                                     "motion_takeover_fallback_prepare_exception");
      }
    }();
    {
      std::lock_guard lock(state_mutex_);
      auto* source = find_active_operation_locked(incumbent_id);
      if (!source || source->pending_motion_takeover.empty() ||
          source->pending_motion_takeover.key() != candidate_id) {
        // The parent advancement borrow must retain this owner until return.
        motion_admission_closed_ = true;
        return reject<void>("Borrowed takeover preparation lost its owner",
                            "motion_takeover_preparation_owner_lost",
                            ErrorCode::kInvariantViolation);
      }
      auto& participant = source->pending_motion_takeover.mapped()->participants.front();
      participant.prepare_in_flight = false;
      if (prepared.ok()) {
        participant.preparation_id = prepared.value();
        participant.subordinate_id = prepared.value();
        preparation_id = prepared.value();
      }
    }
    if (!prepared.ok()) {
      const auto failed =
          fail_pending_motion_takeover(incumbent_id, candidate_id, prepared.error());
      return failed.ok() ? Result<void>::Ok() : Result<void>::Err(failed.error());
    }
    if (attach_to_sent) return advance_motion_takeover_fallback(incumbent_id, candidate_id);
  }
  if (preparation_id == 0U || engine == nullptr) return Result<void>::Ok();
  const auto observed = engine->ObservePreparedMotion(preparation_id);
  if (!observed.ok()) return Result<void>::Err(observed.error());
  if (observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
      observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch) {
    const auto failure = observed.value().error.value_or(
        error(std::string(kErrorExecutionRejected),
              "Spring successor preparation ended without dispatch authority",
              "motion_takeover_fallback_preparation_failed"));
    const auto failed = fail_pending_motion_takeover(incumbent_id, candidate_id, failure);
    return failed.ok() ? Result<void>::Ok() : Result<void>::Err(failed.error());
  }
  if (!certificate) return Result<void>::Ok();
  if (observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kColdPrepared) {
    const auto activated = engine->PrepareColdMotionAfterStop(preparation_id, *certificate);
    if (activated.ok()) return Result<void>::Ok();
    const auto failed = fail_pending_motion_takeover(incumbent_id, candidate_id, activated.error());
    return failed.ok() ? Result<void>::Ok() : Result<void>::Err(failed.error());
  }
  if (observed.value().state == ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
    const auto adopted = commit_settled_motion_takeover(incumbent_id, candidate_id, *certificate);
    return adopted.ok() ? Result<void>::Ok() : Result<void>::Err(adopted.error());
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
