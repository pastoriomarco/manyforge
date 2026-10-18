#include <cmath>
#include <limits>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/io_output_participant_engine.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/types/error_code_mapping.hpp>
#include <stdexcept>

#include "prepared_command_session_internal.hpp"

namespace manyforge::core {
namespace {
ErrorDTO output_error(std::string reason) {
  return {
      reason, "The output transaction did not establish source acceptance", {{"reason", reason}}};
}
bool final_evidence(IoOutputEvidence value, IoOutputEvidence strongest) {
  return value == IoOutputEvidence::kNotSubmitted || value == IoOutputEvidence::kSourceAccepted ||
         value == IoOutputEvidence::kSourceRejected ||
         value == IoOutputEvidence::kAcceptanceUnknown ||
         (value == IoOutputEvidence::kSubmitted && strongest == IoOutputEvidence::kSubmitted);
}
bool evidence_matches_capability(IoOutputEvidence value, IoOutputEvidence strongest) {
  return strongest != IoOutputEvidence::kSubmitted ||
         (value != IoOutputEvidence::kSourceAccepted && value != IoOutputEvidence::kSourceRejected);
}
}  // namespace
struct IoOutputParticipantEngine::Session final : detail::PreparedCommandSessionLifecycle {
  explicit Session(ResolvedPreparedCapabilityCommandExecutionRequestDTO request)
      : authority(std::move(request)) {}
  ResolvedPreparedCapabilityCommandExecutionRequestDTO authority;
  std::optional<IoOutputWriteRequestDTO> submitted;
  std::optional<IoOutputObservationDTO> observation;
  std::optional<IoOutputObservationDTO> uncertainty_fallback;
  bool submission_in_flight{};
  bool unknown_model_excluded{};
  std::optional<ErrorDTO> model_exclusion_error;
  mutable std::optional<KernelRecoveryAssessmentDTO> assessment;
  bool retired{};
  std::optional<IoOutputResetAuthorityDTO> reset_authority;
};

IoOutputParticipantEngine::IoOutputParticipantEngine(IIoOutputProvider& provider, IClock& clock)
    : provider_(provider), clock_(clock), strongest_(provider.StrongestOutputEvidence()) {
  if (strongest_ != IoOutputEvidence::kSubmitted && strongest_ != IoOutputEvidence::kSourceAccepted)
    throw std::invalid_argument("output provider exposes an unsupported completion capability");
}
IoOutputParticipantEngine::~IoOutputParticipantEngine() = default;
ErrorDTO IoOutputParticipantEngine::unsupported_error() {
  return {"unsupported_capability",
          "An output transaction has no physical motion authority",
          {{"reason", "io_output_motion_unsupported"}}};
}
Result<std::uint64_t> IoOutputParticipantEngine::PrepareCapabilityCommand(
    ResolvedPreparedCapabilityCommandExecutionRequestDTO request) {
  const auto* output = request.output();
  if (!request.participant || !output ||
      !VerifyPreparedIoOutputParticipant(*request.participant, request.binding).ok())
    return Result<std::uint64_t>::Err(output_error("io_output_prepared_authority_invalid"));
  const auto& payload = std::get<PreparedIoOutputCommandDTO>(request.participant->payload);
  if (payload.completion_policy.submitted_only() != (strongest_ == IoOutputEvidence::kSubmitted))
    return Result<std::uint64_t>::Err(output_error("io_output_mechanism_capability_mismatch"));
  if (payload.signal_id != output->signal_id ||
      payload.output_resource != output->output_resource ||
      payload.command_value.value != output->value ||
      payload.completion_policy != output->completion_policy ||
      request.participant->provider()->lifecycle_provider != output->provider)
    return Result<std::uint64_t>::Err(output_error("io_output_private_request_mismatch"));
  auto session = std::make_shared<Session>(std::move(request));
  session->state = ResolvedPreparedCapabilityCommandStateDTO::kPrepared;
  return detail::AdmitPreparedCommandSession(
      std::move(session), resident_, resident_mutex_, next_id_,
      [](const Session& value) { return &value.authority.participant->prepared_contract_digest; },
      [](const Session& prior) {
        // A final transport fact remains resident until the actual owner has
        // consumed its result and retired the correlation record.
        return prior.state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched &&
               !prior.retired;
      });
}

void IoOutputParticipantEngine::exclude_unknown_group_model_locked(Session& session) const {
  if (strongest_ != IoOutputEvidence::kSubmitted || !session.submitted || session.retired ||
      session.unknown_model_excluded || !session.observation ||
      session.observation->evidence != IoOutputEvidence::kAcceptanceUnknown)
    return;
  try {
    const auto excluded = provider_.ExcludeFromGroupModel(session.submitted->identity);
    if (!excluded.ok()) {
      session.model_exclusion_error = excluded.error();
      return;
    }
  } catch (...) {
    session.model_exclusion_error = output_error("io_output_group_model_exclusion_failed");
    return;
  }
  session.unknown_model_excluded = true;
  session.model_exclusion_error.reset();
}

void IoOutputParticipantEngine::refresh_locked(Session& session) const {
  // The session mutex keeps this channel's uncertain result private until the
  // provider's shared-writer exclusion has completed. It is the provider's
  // group mutex, not this session mutex, that serializes other channels.
  exclude_unknown_group_model_locked(session);
  if (!session.submitted || session.retired ||
      (session.observation && final_evidence(session.observation->evidence, strongest_)))
    return;
  const auto now = clock_.now();
  const auto deadline = session.submitted->absolute_deadline_ns;
  if (session.submission_in_flight) {
    if (deadline && now >= *deadline) {
      session.observation = std::move(session.uncertainty_fallback);
      session.observation->trusted_received_at_ns = now;
    }
    return;
  }
  // Read the retained source fact before expiring the owner budget. A timely
  // source receipt remains timely when the next reconciliation runs later.
  Result<IoOutputObservationDTO> result =
      Result<IoOutputObservationDTO>::Err(output_error("io_output_observation_failed"));
  try {
    result = provider_.ObserveOutputResult(session.submitted->identity);
  } catch (...) {
  }
  const auto received_now = clock_.now();
  if (!result.ok() || result.value().identity != session.submitted->identity ||
      result.value().trusted_received_at_ns < 0 ||
      result.value().trusted_received_at_ns > received_now ||
      (deadline && result.value().trusted_received_at_ns >= *deadline &&
       result.value().evidence != IoOutputEvidence::kNotSubmitted) ||
      ((!deadline || received_now >= *deadline) &&
       !final_evidence(result.value().evidence, strongest_)) ||
      (session.observation && session.observation->evidence == IoOutputEvidence::kSubmitted &&
       result.value().evidence == IoOutputEvidence::kNotSubmitted) ||
      to_string(result.value().evidence).empty() ||
      !evidence_matches_capability(result.value().evidence, strongest_) ||
      ((result.value().evidence == IoOutputEvidence::kSubmitted ||
        result.value().evidence == IoOutputEvidence::kSourceAccepted) &&
       result.value().error)) {
    session.observation = std::move(session.uncertainty_fallback);
    session.observation->trusted_received_at_ns = received_now;
  } else {
    session.observation = std::move(result.value());
  }
  exclude_unknown_group_model_locked(session);
}
Result<ResolvedPreparedCapabilityCommandSnapshotDTO>
IoOutputParticipantEngine::ObservePreparedCapabilityCommand(std::uint64_t id) const {
  const auto session = resident_.load();
  if (!session)
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
        output_error("prepared_command_session_missing"));
  std::lock_guard lock(session->mutex);
  if (id != session->preparation_id)
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
        output_error("prepared_command_identity_mismatch"));
  refresh_locked(*session);
  if (session->model_exclusion_error)
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
        *session->model_exclusion_error);
  return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Ok(
      {id, session->state, session->authority.participant, std::nullopt,
       session->controller_goal_id, session->controller_goal_generation, session->dispatch_result,
       session->error, session->observation, status_locked(*session)});
}
Result<GripperCommandResultDTO> IoOutputParticipantEngine::DispatchPreparedCapabilityCommand(
    std::uint64_t id, const ContentDigest& digest, const ControllerGoalId& goal,
    ControllerGoalGeneration generation) {
  const auto session = resident_.load();
  if (!session)
    return Result<GripperCommandResultDTO>::Err(output_error("prepared_command_session_missing"));
  std::unique_lock lock(session->mutex);
  const auto& participant = *session->authority.participant;
  // Exact committed replay is independent of the elapsed command budget.
  if (session->dispatch_result && session->preparation_id == id &&
      participant.prepared_contract_digest == digest && session->controller_goal_id == goal &&
      session->controller_goal_generation == generation)
    return Result<GripperCommandResultDTO>::Ok(*session->dispatch_result);
  const auto before = clock_.now();
  const auto& policy = session->authority.output()->completion_policy;
  if (before < 0 || !policy.locally_valid())
    return Result<GripperCommandResultDTO>::Err(output_error("io_output_command_budget_invalid"));
  std::optional<TimePoint> deadline;
  if (const auto* budget = policy.source_ack_budget()) {
    if (budget->timeout_ns > std::numeric_limits<TimePoint>::max() - before)
      return Result<GripperCommandResultDTO>::Err(output_error("io_output_command_budget_invalid"));
    deadline = before + budget->timeout_ns;
  }
  const auto dispatch = session->BeginDispatch(
      id, &participant.prepared_contract_digest, digest, goal, generation, [&] {
        session->submitted =
            IoOutputWriteRequestDTO{{{participant.operation_id, participant.operation_revision,
                                      participant.participant_id, goal, generation},
                                     digest,
                                     participant.component_lifecycle_generation,
                                     participant.provider()->lifecycle_provider},
                                    session->authority.output()->signal_id,
                                    session->authority.output()->value,
                                    deadline};
        // Allocate the uncertainty fallback before committing the effect gate.
        session->uncertainty_fallback = IoOutputObservationDTO{
            session->submitted->identity, IoOutputEvidence::kAcceptanceUnknown, before,
            output_error("io_output_acceptance_unknown")};
        session->submission_in_flight = true;
      });
  if (!dispatch.ok()) return Result<GripperCommandResultDTO>::Err(dispatch.error());
  if (dispatch.value()) return Result<GripperCommandResultDTO>::Ok(*dispatch.value());
  // Submitted-only publication is local and has no ACK/deadline wait. Keep
  // the same session lock through that call so owner exclusion cannot precede
  // creation of this command's actual transport correlation record.
  const bool lock_local_publication = policy.submitted_only();
  if (!lock_local_publication) lock.unlock();
  std::optional<IoOutputObservationDTO> returned;
  // This is the only effect boundary. An exception after entry is uncertainty;
  // the resident request/digest can never authorize a second Submit.
  try {
    auto response = provider_.SubmitOutput(*session->submitted);
    if (response.ok() && response.value().identity == session->submitted->identity &&
        response.value().trusted_received_at_ns >= 0 &&
        response.value().trusted_received_at_ns <= clock_.now() &&
        (!deadline || response.value().trusted_received_at_ns < *deadline ||
         response.value().evidence == IoOutputEvidence::kNotSubmitted) &&
        !to_string(response.value().evidence).empty() &&
        evidence_matches_capability(response.value().evidence, strongest_) &&
        !((response.value().evidence == IoOutputEvidence::kSourceAccepted ||
           response.value().evidence == IoOutputEvidence::kSubmitted) &&
          response.value().error)) {
      returned = std::move(response.value());
    }
  } catch (...) {
  }
  if (!lock_local_publication) lock.lock();
  session->submission_in_flight = false;
  // A timeout observed while Submit was returning is already terminal.
  if (!session->observation || !final_evidence(session->observation->evidence, strongest_)) {
    session->observation =
        returned ? std::move(returned) : std::move(session->uncertainty_fallback);
    if (session->observation->evidence == IoOutputEvidence::kAcceptanceUnknown)
      session->observation->trusted_received_at_ns = clock_.now();
  }
  // A positive local receipt identifies the committed subordinate transaction;
  // source ACK/rejection/unknown are retained separately as typed evidence.
  session->RecordDispatch({true, id, generation.value(), std::nullopt});
  refresh_locked(*session);
  return Result<GripperCommandResultDTO>::Ok(*session->dispatch_result);
}
Result<void> IoOutputParticipantEngine::CancelPreparedCapabilityCommand(std::uint64_t id) {
  const auto session = resident_.load();
  if (!session) return Result<void>::Err(output_error("prepared_command_session_missing"));
  std::lock_guard lock(session->mutex);
  return session->CancelBeforeDispatch(id);
}
AddressedParticipantStopFenceDisposition
IoOutputParticipantEngine::fence_addressed_participant_stop(std::uint64_t id) noexcept {
  try {
    const auto session = resident_.load();
    if (!session) return AddressedParticipantStopFenceDisposition::kUnknown;
    std::lock_guard lock(session->mutex);
    if (session->preparation_id != id) return AddressedParticipantStopFenceDisposition::kUnknown;
    if (session->state == ResolvedPreparedCapabilityCommandStateDTO::kPrepared ||
        session->state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch) {
      static_cast<void>(session->CancelBeforeDispatch(id));
      return AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch;
    }
    return AddressedParticipantStopFenceDisposition::kDispatchCommitted;
  } catch (...) {
    return AddressedParticipantStopFenceDisposition::kUnknown;
  }
}
ExecutionStatus IoOutputParticipantEngine::status_locked(const Session& session) const {
  ExecutionStatus status;
  if (session.retired) return status;
  if (session.model_exclusion_error) {
    // No terminal fact/settlement permission escapes a failed model barrier.
    // Observe reports the exact error and retries the same bounded exclusion.
    status.execution_id = session.preparation_id;
    status.state = ExecutionState::kExecuting;
    return status;
  }
  status.execution_id = session.preparation_id;
  status.last_transition_ns = clock_.now();
  status.state = ExecutionState::kExecuting;
  if (session.state == ResolvedPreparedCapabilityCommandStateDTO::kPrepared) {
    status.state = ExecutionState::kPlanning;
  } else if (session.state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch) {
    status.fact = ParticipantExecutionFact{ParticipantExecutionFactKind::kCancelledBeforeDispatch,
                                           ParticipantEffectKnowledge::kNoEffect,
                                           {}};
  } else if (session.observation && final_evidence(session.observation->evidence, strongest_)) {
    const auto evidence = session.observation->evidence;
    const bool success =
        evidence == IoOutputEvidence::kSourceAccepted ||
        (evidence == IoOutputEvidence::kSubmitted && strongest_ == IoOutputEvidence::kSubmitted);
    const bool unknown = evidence == IoOutputEvidence::kAcceptanceUnknown;
    status.progress = success ? 1.0 : 0.0;
    status.fact = ParticipantExecutionFact{
        success ? ParticipantExecutionFactKind::kComputedCompletion
                : ParticipantExecutionFactKind::kLocalFailure,
        evidence == IoOutputEvidence::kNotSubmitted ? ParticipantEffectKnowledge::kNoEffect
        : unknown                                   ? ParticipantEffectKnowledge::kEffectUncertain
                                                    : ParticipantEffectKnowledge::kEffectSettled,
        success ? std::nullopt
                : std::optional(session.observation->error.value_or(output_error(
                      unknown ? "io_output_acceptance_unknown" : "io_output_source_rejected")))};
  }
  return status;
}
ExecutionStatus IoOutputParticipantEngine::get_status() const {
  const auto session = resident_.load();
  if (!session) return {};
  std::lock_guard lock(session->mutex);
  refresh_locked(*session);
  return status_locked(*session);
}
KernelRecoveryAssessmentDTO IoOutputParticipantEngine::assessment_locked(Session& session) const {
  refresh_locked(session);
  const auto status = status_locked(session);
  KernelRecoveryAssessmentDTO result;
  result.assessed_at_ns = clock_.now();
  result.control_command_epoch = command_epoch_.load();
  result.execution_id = session.preparation_id;
  result.execution_state = status.state;
  if (status.fact) {
    result.execution_state =
        status.fact->effect == ParticipantEffectKnowledge::kEffectUncertain
            ? ExecutionState::kInterventionRequired
        : status.fact->kind == ParticipantExecutionFactKind::kComputedCompletion
            ? ExecutionState::kCompleted
        : status.fact->kind == ParticipantExecutionFactKind::kCancelledBeforeDispatch
            ? ExecutionState::kStopped
            : ExecutionState::kFailed;
  }
  result.eligible_close = !session.retired && status.fact &&
                          status.fact->effect != ParticipantEffectKnowledge::kEffectUncertain;
  result.reason_code =
      result.eligible_close ? "operation_close_ready" : "operation_close_unavailable";
  result.assessment_revision = session.assessment ? session.assessment->assessment_revision : 1U;
  if (session.assessment && recovery_assessment_semantically_differs(*session.assessment, result))
    ++result.assessment_revision;
  session.assessment = result;
  return result;
}
KernelRecoveryAssessmentDTO IoOutputParticipantEngine::get_recovery_assessment() const {
  const auto session = resident_.load();
  if (!session) return {};
  std::lock_guard lock(session->mutex);
  return assessment_locked(*session);
}
Result<void> IoOutputParticipantEngine::retire_locked(Session& session) {
  if (session.retired) return Result<void>::Ok();
  if (command_epoch_ == std::numeric_limits<std::uint64_t>::max())
    return Result<void>::Err(output_error("io_output_command_epoch_exhausted"));
  if (session.submitted) {
    try {
      const auto retired = provider_.RetireOutput(session.submitted->identity);
      if (!retired.ok()) return retired;
    } catch (...) {
      return Result<void>::Err(output_error("io_output_retirement_failed"));
    }
  }
  session.retired = true;
  session.ordinary_result_acknowledged = true;
  ++command_epoch_;
  return Result<void>::Ok();
}
AcknowledgeCompletedResultDTO IoOutputParticipantEngine::acknowledge_completed(
    std::uint64_t id, std::optional<std::uint64_t> epoch) {
  const auto session = resident_.load();
  if (!session) return {false, unsupported_error()};
  std::lock_guard lock(session->mutex);
  if (id != session->preparation_id || (epoch && *epoch != command_epoch_.load()))
    return {false, output_error("io_output_retirement_identity_mismatch")};
  if (session->retired) return {true, {}};
  const auto assessment = assessment_locked(*session);
  const auto status = status_locked(*session);
  if (!assessment.eligible_close || !status.fact ||
      status.fact->kind != ParticipantExecutionFactKind::kComputedCompletion)
    return {false, output_error("io_output_result_not_completed")};
  const auto retired = retire_locked(*session);
  return {retired.ok(), retired.ok() ? std::nullopt : std::optional(retired.error())};
}
Result<void> IoOutputParticipantEngine::reset_capability_command(
    const std::string& target, const std::uint64_t id, const std::uint64_t generation,
    const std::optional<IoOutputResetAuthorityDTO>& authority) {
  const auto session = resident_.load();
  if (!session) return Result<void>::Err(output_error("prepared_command_session_missing"));
  std::lock_guard lock(session->mutex);
  if (target != session->authority.binding.value().command_target_id.value() ||
      id != session->preparation_id || !session->controller_goal_generation ||
      generation != session->controller_goal_generation->value())
    return Result<void>::Err(output_error("io_output_retirement_identity_mismatch"));
  if (session->retired) {
    return session->reset_authority == authority
               ? Result<void>::Ok()
               : Result<void>::Err(output_error("io_output_retirement_authority_conflict"));
  }
  refresh_locked(*session);
  const auto status = status_locked(*session);
  const bool unknown =
      status.fact && status.fact->effect == ParticipantEffectKnowledge::kEffectUncertain;
  if (unknown) {
    if (!session->submitted || !authority || authority->command != session->submitted->identity ||
        authority->acknowledgement_request_id.empty() ||
        !authority->acknowledged_operation_revision.nonzero() ||
        authority->acknowledged_operation_revision < authority->command.command.operation_revision)
      return Result<void>::Err(
          output_error("io_output_explicit_uncertainty_acknowledgement_required"));
    const auto& proof = authority->local_resynchronization;
    const auto& binding = session->authority.binding.value();
    if (proof.retained_bundle_id != binding.retained_bundle_id ||
        proof.graph_revision != binding.graph_revision ||
        proof.controllable_component_id != binding.controllable_component_id ||
        proof.provider != session->submitted->identity.provider ||
        proof.disable_transaction_id.empty() || proof.enable_transaction_id.empty() ||
        proof.disable_transaction_id == proof.enable_transaction_id ||
        proof.disabled_generation <= session->submitted->identity.component_lifecycle_generation ||
        proof.enabled_generation <= proof.disabled_generation)
      return Result<void>::Err(output_error("io_output_local_resynchronization_not_proved"));
  } else {
    if (authority || !status.fact ||
        status.fact->kind == ParticipantExecutionFactKind::kComputedCompletion ||
        status.fact->effect == ParticipantEffectKnowledge::kEffectUncertain)
      return Result<void>::Err(output_error("io_output_result_not_resettable"));
  }
  // Capture the bounded replay proof before the local transport retirement.
  // Neither this proof nor RetireOutput asserts remote queue drain/effect undo.
  auto retained_authority = authority;
  const auto retired = retire_locked(*session);
  if (!retired.ok()) return retired;
  session->reset_authority = std::move(retained_authority);
  return Result<void>::Ok();
}

KernelRecoveryActionResultDTO IoOutputParticipantEngine::apply_recovery_action(
    const KernelRecoveryActionRequestDTO& request) {
  const auto session = resident_.load();
  if (!session) return {request.request_id, request.action, false, {}, unsupported_error()};
  std::lock_guard lock(session->mutex);
  auto assessment = assessment_locked(*session);
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = assessment.control_command_epoch;
  const auto decision = evaluate_recovery_admission(request, capture, assessment);
  if (request.action != RecoveryAction::kCloseOperation || !decision.admitted)
    return {request.request_id, request.action, false, assessment,
            recovery_action_rejection_error(request, assessment, decision.rejection_reason)};
  const auto retired = retire_locked(*session);
  if (!retired.ok())
    return {request.request_id, request.action, false, assessment, retired.error()};
  assessment = assessment_locked(*session);
  return {request.request_id, request.action, true, assessment, {}};
}
}  // namespace manyforge::core
