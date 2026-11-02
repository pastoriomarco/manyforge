#include "prepared_command_session_internal.hpp"
#include <algorithm>
#include <limits>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/types/error_code_mapping.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <stdexcept>

namespace manyforge::core {
namespace {
[[nodiscard]] bool prepared_command_matches_private_request(
    const PreparedOperationParticipantDTO& participant, const RuntimeParticipantBindingDTO& binding,
    const GripperCommandRequestDTO& request, GripperActuationKind actuation, GripperOutputMode output) noexcept {
  const auto* command = std::get_if<PreparedCapabilityCommandDTO>(&participant.payload);
  if (command == nullptr || binding.resources.size() != 1U || !binding.control_path.has_value() ||
      participant.command_target_id != binding.command_target_id) {
    return false;
  }
  const auto& resource = binding.resources.front();
  const auto& value = command->command_value;
  const auto kind = coordinate_kind(value.position);
  const auto target = kind == CoordinateKind::kAngular
                          ? std::get<AngularPositionDTO>(value.position).value_rad
                          : std::get<LinearPositionDTO>(value.position).value_m;
  std::optional<double> effort;
  if (value.max_effort.has_value()) {
    effort = kind == CoordinateKind::kAngular
                 ? std::get<AngularEffortDTO>(*value.max_effort).value_nm
                 : std::get<LinearEffortDTO>(*value.max_effort).value_n;
  }
  double duration = 0.0;
  bool wait_for_duration = false;
  for (const auto& option : command->validated_options) {
    const auto* value_s = std::get_if<double>(&option.value);
    const auto* value_bool = std::get_if<bool>(&option.value);
    if (option.option_id.value() == "command_duration_s" && value_s != nullptr) {
      duration = *value_s;
    } else if (option.option_id.value() == "wait_for_duration" && value_bool != nullptr) {
      wait_for_duration = *value_bool;
    } else {
      return false;
    }
  }
  const bool model_backed = resource.source_joint_name.has_value();
  const auto expected_group = model_backed && binding.model_groups.size() == 1U
                                  ? binding.model_groups.front().compiled_group_name
                                  : std::string{};
  return request.gripper_id == binding.command_target_id.value() &&
         request.group_id == expected_group &&
         request.joint_name == resource.compiled_resource_name &&
         request.target_position == target && request.duration_s == duration &&
         request.wait_for_duration == wait_for_duration &&
         request.max_effort == effort && request.commanded_state == "position" &&
         request.actuation_kind == (kind == CoordinateKind::kAngular ? "angular" : "linear") &&
         request.state_authority == binding.control_path->state_authority_id.value() &&
         request.completion_policy == (actuation == GripperActuationKind::kPneumatic
                                         ? (output == GripperOutputMode::kNone ? "no_output" : "output_delivery")
                                         : "goal_fenced_target_reached") &&
         request.grasp_confirmation_policy == (actuation == GripperActuationKind::kPneumatic
                                                  ? "not_required" : "binding_feedback_required") &&
         request.transport_kind == "resolved_binding" &&
         request.evidence == std::vector<std::string>{"command_terminal"} &&
         request.evidence_state == "required_pending" &&
         request.completion_timeout_s == command->completion_timeout_s;
}
}  // namespace
struct GripperParticipantEngine::PreparedCapabilityCommandSession final
    : detail::PreparedCommandSessionLifecycle {
  explicit PreparedCapabilityCommandSession(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO supplied)
      : authority(std::move(supplied)) {}

  ResolvedPreparedCapabilityCommandExecutionRequestDTO authority;
  GripperActuationKind actuation{GripperActuationKind::kPositionCommanded};
  std::uint64_t owner_lifecycle_generation{};
  std::optional<PrepareParticipantResultDTO> backend_prepare_result;
  std::optional<AddressedParticipantStopSettlementDTO> stop_settlement;
  bool stop_recovery_consumed{};
  std::optional<ParticipantStopStageResultDTO> translated_stop;
  mutable std::optional<KernelRecoveryAssessmentDTO> close_assessment;

  KernelRecoveryAssessmentDTO assess_close(const std::optional<GripperCommandStatusDTO>& observed,
                                           const std::int64_t now,
                                           const std::uint64_t epoch) const {
    KernelRecoveryAssessmentDTO assessment;
    assessment.assessed_at_ns = now;
    assessment.control_command_epoch = epoch;
    assessment.execution_id = preparation_id;
    bool no_effect = state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch ||
                     state == ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed;
    bool completed = false;
    bool settled_failure = false;
    if ((state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched ||
         state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed) &&
        dispatch_result && dispatch_result->command_id != 0U && observed) {
      const auto& status = *observed;
      const bool exact = status.command_id == dispatch_result->command_id &&
                         status.generation == dispatch_result->generation;
      completed = exact && dispatch_result->accepted && !dispatch_result->error &&
                  status.state == GripperCommandState::kCompleted;
      // Pneumatic finality settles its delivery transaction. A position-command
      // Failure alone does not prove containment of physical gripper motion.
      settled_failure = exact && actuation == GripperActuationKind::kPneumatic &&
                        status.state == GripperCommandState::kFailed;
      assessment.execution_id = dispatch_result->command_id;
    }
    assessment.execution_state = ExecutionState::kInterventionRequired;
    if (completed) {
      assessment.execution_state = ExecutionState::kCompleted;
    } else if (settled_failure ||
               state == ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed) {
      assessment.execution_state = ExecutionState::kFailed;
    } else if (no_effect) {
      assessment.execution_state = ExecutionState::kStopped;
    } else if (state == ResolvedPreparedCapabilityCommandStateDTO::kPrepared) {
      assessment.execution_state = ExecutionState::kPlanning;
    } else if (state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested ||
               state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched) {
      assessment.execution_state = ExecutionState::kExecuting;
    }
    assessment.eligible_close =
        !ordinary_result_acknowledged && (no_effect || completed || settled_failure);
    assessment.reason_code =
        assessment.eligible_close ? "operation_close_ready" : "operation_close_unavailable";
    assessment.assessment_revision = close_assessment ? close_assessment->assessment_revision : 1U;
    if (close_assessment && recovery_assessment_semantically_differs(*close_assessment, assessment))
      ++assessment.assessment_revision;
    close_assessment = assessment;
    return assessment;
  }
};

GripperParticipantEngine::GripperParticipantEngine(IGripperCommandBackend& backend, IClock& clock)
    : backend_(backend), clock_(clock) {}
GripperParticipantEngine::~GripperParticipantEngine() = default;

ErrorDTO GripperParticipantEngine::unsupported_error() {
  return {kErrorUnsupportedCapability,
          "Motion control is unavailable on a gripper participant",
          {{"reason", "gripper_motion_control_unsupported"}}};
}
std::uint64_t GripperParticipantEngine::bump_control_command_epoch() {
  auto current = control_command_epoch_.load(std::memory_order_acquire);
  do {
    if (current == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("gripper control generation exhausted");
  } while (!control_command_epoch_.compare_exchange_weak(current, current + 1U));
  return current + 1U;
}
void GripperParticipantEngine::emit_event(KernelEvent event) {
  (void)event_publisher_.dispatch(event);
}
SubscriptionHandle GripperParticipantEngine::subscribe_events(EventCallback callback) {
  return event_publisher_.subscribe(std::move(callback));
}
void GripperParticipantEngine::unsubscribe_events(SubscriptionHandle handle) {
  event_publisher_.unsubscribe(handle);
}
GripperActuationKind GripperParticipantEngine::gripper_actuation_kind(
    const std::string& target) const {
  return backend_.gripper_actuation_kind(target);
}
GripperOutputMode GripperParticipantEngine::gripper_output_mode(const std::string& target) const {
  return backend_.gripper_output_mode(target);
}
AcknowledgeCompletedResultDTO GripperParticipantEngine::acknowledge_completed(
    const std::uint64_t preparation_id, const std::optional<std::uint64_t> expected_epoch) {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_.load();
  }
  if (!session) return {false, unsupported_error()};
  std::lock_guard lock(session->mutex);
  if (session->actuation != GripperActuationKind::kPneumatic ||
      session->preparation_id != preparation_id ||
      (expected_epoch && *expected_epoch != control_command_epoch_.load()))
    return {false, unsupported_error()};
  if (session->ordinary_result_acknowledged) return {true, std::nullopt};
  bool final = session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed;
  if (session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched &&
      session->dispatch_result && session->dispatch_result->accepted &&
      !session->dispatch_result->error) {
    const auto status = backend_.get_gripper_status(session->authority.gripper().command_request.gripper_id);
    final = status.command_id == session->dispatch_result->command_id &&
            status.generation == session->dispatch_result->generation &&
            (status.state == GripperCommandState::kCompleted || status.state == GripperCommandState::kFailed);
  }
  if (!final) return {false, ErrorDTO{kErrorInvalidRequest,
      "Pneumatic command has not reached its final exit condition",
      {{"reason", "pneumatic_command_not_final"}}}};
  // Consume the ordinary result only. Admission closed by a shared Stop
  // remains closed; only the established Reset/recovery path may reopen it.
  // Pneumatic delivery Failure itself never closes backend admission.
  session->ordinary_result_acknowledged = true;
  return {true, std::nullopt};
}
GripperCommandResultDTO GripperParticipantEngine::command_gripper(
    const GripperCommandRequestDTO& request) {
  return backend_.command_gripper(request);
}
Result<void> GripperParticipantEngine::validate_gripper_command(
    const GripperCommandRequestDTO& request) const {
  return backend_.validate_gripper_command(request);
}
GripperCommandStatusDTO GripperParticipantEngine::get_gripper_status(const std::string& id) const {
  return backend_.get_gripper_status(id);
}
Result<std::uint64_t> GripperParticipantEngine::stop_gripper(const std::string& id,
                                                             const std::uint64_t command_id,
                                                             const std::uint64_t generation) {
  if (backend_.gripper_actuation_kind(id) == GripperActuationKind::kPneumatic)
    return Result<std::uint64_t>::Err(ErrorDTO{kErrorInvalidRequest,
        "Pneumatic output transactions do not participate in Stop containment",
        {{"reason", "pneumatic_stop_not_applicable"}}});
  const auto before = backend_.get_gripper_status(id);
  if (before.gripper_id != id || command_id == 0U || before.command_id != command_id ||
      before.generation != generation || before.state != GripperCommandState::kExecuting)
    return Result<std::uint64_t>::Err(ErrorDTO{"stale_gripper_state",
                                               "Gripper changed before scoped Stop",
                                               {{"reason", "stale_gripper_state"}}});
  backend_.invalidate_gripper_commands(GripperInvalidationReason::kStop);
  const auto after = backend_.get_gripper_status(id);
  if (after.gripper_id != id || after.command_id != command_id || after.generation == generation ||
      after.state != GripperCommandState::kIdle)
    return Result<std::uint64_t>::Err(ErrorDTO{"gripper_stop_unconfirmed",
                                               "Gripper Stop has no new idle generation",
                                               {{"reason", "gripper_stop_unconfirmed"}}});
  return Result<std::uint64_t>::Ok(after.generation);
}
Result<void> GripperParticipantEngine::reset_gripper(const std::string& id,
                                                     const std::uint64_t command_id,
                                                     const std::uint64_t generation) {
  auto closed = close_gripper_generation(id, command_id, generation);
  if (!closed.ok()) return closed;
  return backend_.try_authorize_gripper_commands();
}
Result<void> GripperParticipantEngine::close_gripper_generation(const std::string& id,
                                                                const std::uint64_t command_id,
                                                                const std::uint64_t generation) {
  const auto before = backend_.get_gripper_status(id);
  if (before.gripper_id != id || command_id == 0U || before.command_id != command_id ||
      before.generation < generation || before.state == GripperCommandState::kExecuting ||
      (before.generation != generation && before.state != GripperCommandState::kIdle))
    return Result<void>::Err(ErrorDTO{"stale_gripper_state",
                                      "Gripper changed before scoped Reset",
                                      {{"reason", "stale_gripper_state"}}});
  backend_.invalidate_gripper_commands(GripperInvalidationReason::kReset);
  return Result<void>::Ok();
}
AddressedParticipantStopFenceDisposition GripperParticipantEngine::fence_addressed_participant_stop(
    const std::uint64_t execution_id) noexcept {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_.load();
  }
  if (!session || execution_id == 0U) return AddressedParticipantStopFenceDisposition::kUnknown;
  std::lock_guard lock(session->mutex);
  if (session->state == ResolvedPreparedCapabilityCommandStateDTO::kPrepared &&
      session->preparation_id == execution_id)
    session->state = ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
  if (session->state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch &&
      session->preparation_id == execution_id)
    return AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch;
  // DispatchRequested is the linearization point, before calling the backend.
  if (session->preparation_id == execution_id ||
      (session->dispatch_result && session->dispatch_result->command_id == execution_id))
    return AddressedParticipantStopFenceDisposition::kDispatchCommitted;
  return AddressedParticipantStopFenceDisposition::kUnknown;
}

Result<std::uint64_t> GripperParticipantEngine::PrepareCapabilityCommand(
    ResolvedPreparedCapabilityCommandExecutionRequestDTO request) {
  if (!request.participant) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared capability command requires one immutable participant",
                 {{"reason", "prepared_command_participant_missing"}}});
  }
  const auto verified = VerifyPreparedOperationParticipant(
      *request.participant, request.binding, request.gripper().stop_readiness,
      request.gripper().retained_default_stop_limits, request.gripper().stop_node_category);
  if (!verified.ok()) {
    return Result<std::uint64_t>::Err(verified.error());
  }
  const auto actuation = gripper_actuation_kind(request.binding.value().command_target_id.value());
  const auto output = gripper_output_mode(request.binding.value().command_target_id.value());
  if (output == GripperOutputMode::kNone && actuation != GripperActuationKind::kPneumatic)
    return Result<std::uint64_t>::Err(ErrorDTO{kErrorInvalidRequest,
        "No output is only supported for a strict pneumatic target",
        {{"reason", "gripper_output_mode_invalid"}}});
  if (!std::holds_alternative<PreparedCapabilityCommandDTO>(request.participant->payload) ||
      !prepared_command_matches_private_request(*request.participant, request.binding.value(),
                                                request.gripper().command_request, actuation, output)) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared capability command differs from its compiled private request",
                 {{"reason", "prepared_command_authority_mismatch"}}});
  }
  auto validated = validate_gripper_command(request.gripper().command_request);
  if (!validated.ok()) {
    return Result<std::uint64_t>::Err(validated.error());
  }
  auto prepared = MakePrepareParticipantResult(*request.participant,
                                               PrepareParticipantResultStateDTO::kPrepared);
  if (!prepared.ok()) {
    return Result<std::uint64_t>::Err(prepared.error());
  }

  std::shared_ptr<PreparedCapabilityCommandSession> session;
  try {
    session = std::make_shared<PreparedCapabilityCommandSession>(std::move(request));
  } catch (...) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInternalError,
                 "Prepared capability-command session allocation failed",
                 {{"reason", "prepared_command_session_allocation_failed"}}});
  }
  session->actuation = actuation;
  session->state = ResolvedPreparedCapabilityCommandStateDTO::kPrepared;
  session->backend_prepare_result = std::move(prepared.value());

  return detail::AdmitPreparedCommandSession(
      std::move(session), active_prepared_command_session_, prepared_command_session_mutex_,
      next_execution_id_,
      [](const auto& value) -> const ContentDigest* {
        return value.authority.participant
                   ? &value.authority.participant->prepared_contract_digest : nullptr;
      },
      [&](const auto& prior) {
        if (prior.state != ResolvedPreparedCapabilityCommandStateDTO::kDispatched ||
            !prior.dispatch_result || !prior.dispatch_result->accepted) return false;
        const auto status = get_gripper_status(prior.authority.participant->command_target_id.value());
        return status.state == GripperCommandState::kExecuting &&
               status.command_id == prior.dispatch_result->command_id &&
               status.generation == prior.dispatch_result->generation;
      });
}

Result<ResolvedPreparedCapabilityCommandSnapshotDTO>
GripperParticipantEngine::ObservePreparedCapabilityCommand(
    const std::uint64_t preparation_id) const {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_.load();
  }
  if (!session) {
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared capability-command session is not resident",
                 {{"reason", "prepared_command_session_missing"}}});
  }
  std::lock_guard lock(session->mutex);
  if (session->preparation_id != preparation_id) {
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared capability-command identity is stale",
                 {{"reason", "prepared_command_identity_mismatch"}}});
  }
  return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Ok(
      ResolvedPreparedCapabilityCommandSnapshotDTO{
          session->preparation_id, session->state, session->authority.participant,
          session->backend_prepare_result, session->controller_goal_id,
          session->controller_goal_generation, session->dispatch_result, session->error});
}

Result<GripperCommandResultDTO> GripperParticipantEngine::DispatchPreparedCapabilityCommand(
    const std::uint64_t preparation_id, const ContentDigest& prepared_contract_digest,
    const ControllerGoalId& controller_goal_id,
    const ControllerGoalGeneration controller_goal_generation) {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_.load();
  }
  if (!session) {
    return Result<GripperCommandResultDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared capability-command session is not resident",
                 {{"reason", "prepared_command_session_missing"}}});
  }
  {
    std::lock_guard lock(session->mutex);
    const auto begun = session->BeginDispatch(
        preparation_id,
        session->authority.participant ? &session->authority.participant->prepared_contract_digest : nullptr,
        prepared_contract_digest, controller_goal_id, controller_goal_generation,
        [&] {
          if (session->actuation == GripperActuationKind::kPneumatic)
            session->owner_lifecycle_generation = backend_.get_gripper_status(
                session->authority.gripper().command_request.gripper_id).lifecycle_generation;
        });
    if (!begun.ok()) return Result<GripperCommandResultDTO>::Err(begun.error());
    if (begun.value()) return Result<GripperCommandResultDTO>::Ok(*begun.value());
  }

  GripperCommandResultDTO dispatched;
  try {
    auto command_request = session->authority.gripper().command_request;
    command_request.addressed_identity =
        AddressedGripperCommandIdentityDTO{session->authority.participant->operation_id,
                                           session->authority.participant->operation_revision,
                                           session->authority.participant->participant_id,
                                           controller_goal_id, controller_goal_generation};
    if (session->actuation == GripperActuationKind::kPneumatic) {
      command_request.owner_lifecycle_generation = session->owner_lifecycle_generation;
      command_request.committed_owner_is_current = [this, session](const auto& identity) {
        // Provider calls this only during the same synchronous command call.
        // No engine/session mutex is acquired under backend authority; the
        // atomic resident/phase reads avoid the inverse lock order.
        // The immutable goal/authority fields were published by the release
        // transition; no provider may retain this closure after the call.
        return active_prepared_command_session_.load() == session &&
            session->state.load() == ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested &&
            session->authority.participant &&
            session->authority.participant->operation_id == identity.operation_id &&
            session->authority.participant->operation_revision == identity.operation_revision &&
            session->authority.participant->participant_id == identity.participant_id &&
            session->controller_goal_id == identity.command_id &&
            session->controller_goal_generation == identity.command_generation;
      };
    }
    dispatched = command_gripper(command_request);
  } catch (const std::exception& exception) {
    dispatched = GripperCommandResultDTO{
        false, 0U, 0U,
        ErrorDTO{
            kErrorExecutionRejected,
            "Capability-command backend threw during dispatch",
            {{"reason", "prepared_command_dispatch_exception"}, {"exception", exception.what()}}}};
  } catch (...) {
    dispatched =
        GripperCommandResultDTO{false, 0U, 0U,
                                ErrorDTO{kErrorExecutionRejected,
                                         "Capability-command backend threw during dispatch",
                                         {{"reason", "prepared_command_dispatch_exception"},
                                          {"exception", reason_codes::kUnknownNonStdException}}}};
  }
  {
    std::lock_guard lock(session->mutex);
    session->RecordDispatch(dispatched);
  }
  return Result<GripperCommandResultDTO>::Ok(std::move(dispatched));
}

ParticipantStopStageResultDTO GripperParticipantEngine::TranslateStopStage(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation) {
  ParticipantStopStageResultDTO result{
      stage,
      StopStageAttemptDispositionDTO::kNotDispatched,
      ParticipantStopStageOutcomeDTO::kFailed,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      0,
      {},
      std::nullopt};
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_.load();
  }
  const auto refuse = [&](const std::string& reason, const std::string& message) {
    result.error = ErrorDTO{kErrorExecutionRejected, message, {{"reason", reason}}};
    if (stage.stage_index < stage.participant_stop_plan.stage_commands.size()) {
      for (const auto& expected : stage.participant_stop_plan.stage_commands[stage.stage_index]
                                       .resolved_stage.evidence_requirements) {
        StopEvidenceItemDTO item{
            expected.requirement.evidence_item_id,
            expected.requirement.evidence_kind,
            StopEvidenceItemStateDTO::kMissing,
            expected.resolved_source,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            result.error};
        result.evidence_items.push_back(std::move(item));
      }
    }
    return result;
  };
  if (!session)
    return refuse("gripper_stop_session_missing", "Gripper Stop has no resident preparation");
  std::lock_guard lock(session->mutex);
  const auto& authority = session->authority;
  const auto& participant = authority.participant;
  if (!participant ||
      participant->operation_id != stage.operation_id ||
      participant->operation_revision != stage.operation_revision ||
      participant->participant_id != stage.participant_id ||
      session->controller_goal_id != stage.controller_goal_id ||
      session->controller_goal_generation != stage.controller_goal_generation)
    return refuse("gripper_stop_command_mismatch", "Gripper Stop names another prepared command");
  const auto valid = ValidateResolvedParticipantStopStage(
      stage, budget, authority.gripper().stop_readiness.value().resolved_stop_base_plan, authority.binding,
      authority.gripper().retained_default_stop_limits, authority.gripper().stop_node_category, participant->resources);
  if (!valid.ok()) {
    return refuse("gripper_stop_stage_invalid", valid.error().message);
  }
  if (session->translated_stop) {
    if (session->translated_stop->request == stage) return *session->translated_stop;
    const auto& prior = *session->translated_stop;
    if (prior.request.stop_id != stage.stop_id || prior.request.stop_generation != stage.stop_generation ||
        prior.outcome == ParticipantStopStageOutcomeDTO::kSucceeded ||
        prior.request.stage_index >= stage.stage_index)
      return refuse("gripper_stop_identity_conflict", "Another Stop already fenced this command");
  }
  if (cancellation.stop_requested())
    return refuse("gripper_stop_cancelled_before_effect", "Gripper Stop was cancelled before effect");
  const bool pneumatic = session->actuation == GripperActuationKind::kPneumatic;
  if (pneumatic)
    return refuse("pneumatic_stop_not_applicable",
                  "Pneumatic output transactions do not participate in Stop containment");
  if (session->state != ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested &&
      session->state != ResolvedPreparedCapabilityCommandStateDTO::kDispatched &&
      !(pneumatic && session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed))
    return refuse("gripper_stop_command_not_dispatched", "Gripper Stop has no dispatched command");
  const auto& command = stage.participant_stop_plan.stage_commands.at(stage.stage_index).resolved_stage;
  const bool logical = command.containment_command.command_kind == StopContainmentCommandKind::kLogicalCancel;
  const bool position_hard = !pneumatic &&
      command.stage_kind == StopEscalationStageKind::kSoftwareHard &&
      command.containment_command.command_kind == StopContainmentCommandKind::kRegistered &&
      command.containment_command.registered_command_kind_id ==
          RegisteredStopCommandKindId("manyforge.stop.software_hard");
  if ((!logical && !position_hard) || command.evidence_requirements.empty() ||
      std::any_of(command.evidence_requirements.begin(), command.evidence_requirements.end(),
                  [](const auto& item) {
                    return item.requirement.evidence_kind !=
                               StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect ||
                           item.requirement.source_requirement.source_kind !=
                               StopEvidenceSourceKind::kCommandTargetStatus ||
                           item.requirement.observation_semantics.observation_kind !=
                               StopEvidenceObservationKind::kCommandResult;
                  }))
    return refuse("gripper_stop_policy_mismatch", "Gripper Stop requires its logical-cancellation policy");
  const auto& id = authority.gripper().command_request.gripper_id;
  const auto started = clock_.now();
  const auto before = backend_.get_gripper_status(id);
  if (before.gripper_id != id)
    return refuse("gripper_stop_target_unavailable", "Gripper Stop target status is unavailable");
  if (!pneumatic && session->dispatch_result &&
      (!session->dispatch_result->accepted || session->dispatch_result->error ||
       before.command_id != session->dispatch_result->command_id ||
       before.generation != session->dispatch_result->generation))
    return refuse("gripper_stop_backend_command_mismatch", "Gripper Stop target names another backend command");

  // The maintained ROS backend's status locks are leaf map/runtime locks;
  // invalidation drops them before scheduler/cancel calls, whose callbacks
  // do not enter this engine. No arbitrary backend implementation is assumed.
  // Q58/Q70: close the genuine command generation and request cancel, without
  // awaiting its transport reply or the still-returning command dispatch.
  backend_.invalidate_gripper_commands(GripperInvalidationReason::kStop);
  result.attempt_disposition = StopStageAttemptDispositionDTO::kDispatched;
  const auto after = backend_.get_gripper_status(id);
  const auto observed = clock_.now();
  if (observed >= started) result.receiver_elapsed_duration_ns = observed - started;
  const bool fenced = pneumatic
      ? after.admission_closed && after.admission_generation > before.admission_generation &&
            after.command_id == before.command_id && after.generation == before.generation
      : after.command_id == before.command_id && after.generation > before.generation &&
            after.state != GripperCommandState::kExecuting;
  if (after.gripper_id != id || !fenced || after.revision == 0U || observed < started) {
    auto failed = refuse("gripper_stop_generation_unavailable",
                         "Gripper command target did not publish its generation fence");
    session->translated_stop = failed;
    return failed;
  }
  JsonObjectDTO facts{
      {"controller_goal_id", JsonValueDTO{stage.controller_goal_id.value()}},
      {"controller_goal_generation", JsonValueDTO{std::to_string(stage.controller_goal_generation.value())}},
      {"gripper_id", JsonValueDTO{id}},
      {"backend_command_id", JsonValueDTO{std::to_string(after.command_id)}},
      {"previous_generation", JsonValueDTO{std::to_string(before.generation)}},
      {"fenced_generation", JsonValueDTO{std::to_string(after.generation)}},
      {"admission_generation", JsonValueDTO{std::to_string(after.admission_generation)}},
      {"no_further_commands", JsonValueDTO{true}},
      {"admitted_transaction_preserved", JsonValueDTO{pneumatic}},
      {"dispatch_result_available", JsonValueDTO{session->dispatch_result.has_value()}},
      {"cancel_dispatch", JsonValueDTO{std::string("unavailable")}},
      {"cancel_dispatch_reason", JsonValueDTO{std::string("backend_does_not_publish_cancel_dispatch_status")}},
  };
  for (const auto& expected : command.evidence_requirements) {
    StopEvidenceItemDTO item{
        expected.requirement.evidence_item_id,
        expected.requirement.evidence_kind,
        StopEvidenceItemStateDTO::kSatisfied,
        expected.resolved_source,
        stage.provider_runtime_epoch,
        after.revision,
        0,  // This owner's direct status observation above.
        JsonValueDTO{facts},
        std::nullopt};
    result.evidence_items.push_back(std::move(item));
  }
  // Deliberately no controller ACK or motion-rest projection: this is only
  // the actual logical command fence. Kernel retains the dispatch-return hold.
  result.outcome = ParticipantStopStageOutcomeDTO::kSucceeded;
  const auto checked = ValidateParticipantStopStageResult(
      result, authority.gripper().stop_readiness.value().resolved_stop_base_plan, authority.binding,
      authority.gripper().retained_default_stop_limits, authority.gripper().stop_node_category, participant->resources);
  if (!checked.ok()) {
    result.outcome = ParticipantStopStageOutcomeDTO::kFailed;
    result.error = checked.error();
  }
  session->translated_stop = result;
  return result;
}

Result<void> GripperParticipantEngine::CancelPreparedCapabilityCommand(
    const std::uint64_t preparation_id) {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_.load();
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared capability-command session is not resident",
                                      {{"reason", "prepared_command_session_missing"}}});
  }
  std::lock_guard lock(session->mutex);
  return session->CancelBeforeDispatch(preparation_id);
}

Result<AddressedParticipantStopSettlementDTO>
GripperParticipantEngine::settle_addressed_participant_stop(
    const AddressedParticipantStopSettlementRequestDTO& request) {
  const auto& containment = request.containment;
  const auto& stop = request.aggregate_stop_certificate;
  const auto dispatched_containment_complete =
      !request.effect_prevented_before_dispatch &&
      containment.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
      !containment.error.has_value() &&
      containment.attempt_disposition != StopStageAttemptDispositionDTO::kNotDispatched &&
      std::all_of(
          containment.evidence_items.begin(), containment.evidence_items.end(),
          [](const auto& item) { return item.state == StopEvidenceItemStateDTO::kSatisfied; });
  const auto no_effect_cancellation_complete =
      request.effect_prevented_before_dispatch &&
      containment.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
      !containment.error.has_value() &&
      containment.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched &&
      !containment.actual_provider_contract_mode.has_value() &&
      !containment.recoverable_substitution_reason.has_value() &&
      !containment.controller_acknowledgement_sequence.has_value() &&
      !containment.source_epoch.has_value() && !containment.source_sequence.has_value() &&
      containment.evidence_items.empty();
  if (request.execution_id == 0U || !stop.locally_valid() ||
      (!dispatched_containment_complete && !no_effect_cancellation_complete) ||
      containment.request.stop_id != stop.stop_id ||
      containment.request.stop_generation != stop.stop_generation) {
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Addressed Stop settlement handoff is incomplete",
                 {{"reason", "addressed_participant_stop_settlement_handoff_invalid"}}});
  }

  // Capability commands share the addressed owner's physical Stop and
  // certificate, but own a distinct local command lifecycle. Settle that
  // lifecycle directly instead of routing it through the motion FSM.
  std::shared_ptr<PreparedCapabilityCommandSession> command_session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    command_session = active_prepared_command_session_.load();
  }
  if (request.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
      command_session) {
    std::lock_guard session_lock(command_session->mutex);
    const auto& participant = command_session->authority.participant;
    const auto& stage = containment.request;
    // The addressed handoff names this resident preparation/session. Backend
    // command identity is separate and is used only for exact backend cleanup.
    const bool exact_execution = command_session->preparation_id == request.execution_id;
    const bool exact_participant = exact_execution && participant &&
                                   participant->operation_id == stage.operation_id &&
                                   participant->participant_id == stage.participant_id &&
                                   participant->operation_revision == stage.operation_revision;
    const bool exact_dispatch =
        !request.effect_prevented_before_dispatch && exact_participant &&
        command_session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched &&
        command_session->dispatch_result.has_value() &&
        command_session->dispatch_result->accepted &&
        !command_session->dispatch_result->error.has_value() &&
        command_session->controller_goal_id == stage.controller_goal_id &&
        command_session->controller_goal_generation == stage.controller_goal_generation;
    const bool exact_no_effect =
        request.effect_prevented_before_dispatch && exact_participant &&
        command_session->state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
    if (!exact_dispatch && !exact_no_effect) {
      return Result<AddressedParticipantStopSettlementDTO>::Err(
          ErrorDTO{kErrorMissingReference,
                   "Addressed Stop names a stale prepared capability command",
                   {{"reason", "addressed_participant_stop_execution_mismatch"},
                    {"requested_execution_id", std::to_string(request.execution_id)},
                    {"current_execution_id", std::to_string(command_session->preparation_id)}}});
    }
    if (!request.effect_prevented_before_dispatch) {
      const auto valid_containment = ValidateParticipantStopStageResult(
          containment, command_session->authority.gripper().stop_readiness.value().resolved_stop_base_plan,
          command_session->authority.binding,
          command_session->authority.gripper().retained_default_stop_limits,
          command_session->authority.gripper().stop_node_category, participant->resources);
      if (!valid_containment.ok()) {
        return Result<AddressedParticipantStopSettlementDTO>::Err(valid_containment.error());
      }
    }
    if (command_session->stop_settlement.has_value()) {
      if (command_session->stop_settlement->aggregate_stop_certificate == stop &&
          command_session->stop_settlement->operation_id == stage.operation_id &&
          command_session->stop_settlement->participant_id == stage.participant_id &&
          command_session->stop_settlement->controller_goal_id == stage.controller_goal_id &&
          command_session->stop_settlement->controller_goal_generation ==
              stage.controller_goal_generation) {
        return Result<AddressedParticipantStopSettlementDTO>::Ok(*command_session->stop_settlement);
      }
      return Result<AddressedParticipantStopSettlementDTO>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "A different Stop already owns the capability-command terminal",
                   {{"reason", "addressed_participant_stop_identity_conflict"}}});
    }

    if (!request.effect_prevented_before_dispatch) {
      const auto& dispatched = *command_session->dispatch_result;
      auto closed = close_gripper_generation(
          command_session->authority.gripper().command_request.gripper_id,
          dispatched.command_id, dispatched.generation);
      if (!closed.ok()) {
        return Result<AddressedParticipantStopSettlementDTO>::Err(closed.error());
      }
      // Q58: the Stop's cancel is fire-and-forget, so a ROS action goal's
      // cancel terminal normally arrives after this settlement. The command
      // is already fenced (new idle generation), so the Stop is settled.
      // Until that terminal the backend refuses to reopen admission; the
      // Stop's recovery (apply_recovery_action) or the operation Reset
      // (reset_capability_command -> reset_gripper) reopens it, or reports
      // that typed refusal if it still applies.
      (void)backend_.try_authorize_gripper_commands();
    }

    KernelRecoveryAssessmentDTO assessment;
    assessment.assessment_revision = 1U;
    assessment.assessed_at_ns = clock_.now();
    assessment.control_command_epoch = bump_control_command_epoch();
    assessment.execution_id = request.execution_id;
    assessment.execution_state = ExecutionState::kStopped;
    assessment.recovery_class = RecoveryClass::kOrdinaryOperatorStop;
    assessment.status = RecoveryAssessmentStatus::kRetryReady;
    assessment.source_id = request.effect_prevented_before_dispatch
                               ? "addressed_participant_cancelled_before_dispatch"
                               : "addressed_participant_stop";
    assessment.stop_owner_generation = stop.stop_generation.value();
    assessment.backend_stop_confirmed = true;
    assessment.stop_certificate_id = stop.stop_certificate_id.value();
    assessment.stop_certificate_revision = stop.stop_certificate_revision.value();
    assessment.retry_admissible = true;
    assessment.allowed_actions = {RecoveryAction::kReplanFromStoppedState,
                                  RecoveryAction::kResetCycle};
    assessment.reason_code = reason_codes::kRecoveryStopCertified;
    assessment.evidence_refs = {stop.stop_certificate_id.value(), stop.stop_id.value()};
    command_session->stop_recovery_consumed = false;
    command_session->stop_settlement = AddressedParticipantStopSettlementDTO{
        request.execution_id,       stop,
        stage.operation_id,         stage.participant_id,
        stage.controller_goal_id,   stage.controller_goal_generation,
        std::move(assessment)};
    return Result<AddressedParticipantStopSettlementDTO>::Ok(*command_session->stop_settlement);
  }
  if (request.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Addressed Stop names no resident prepared capability command",
                 {{"reason", "addressed_participant_stop_command_session_missing"}}});
  }

  return Result<AddressedParticipantStopSettlementDTO>::Err(unsupported_error());
}

KernelRecoveryAssessmentDTO GripperParticipantEngine::get_recovery_assessment() const {
  std::shared_ptr<PreparedCapabilityCommandSession> command_session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    command_session = active_prepared_command_session_.load();
  }
  if (!command_session) return {};
  {
    std::lock_guard session_lock(command_session->mutex);
    if (command_session->stop_settlement && !command_session->stop_recovery_consumed) {
      auto assessment = command_session->stop_settlement->recovery_assessment;
      assessment.eligible_close = true;
      return assessment;
    }
  }
  const auto observed =
      backend_.get_gripper_status(command_session->authority.gripper().command_request.gripper_id);
  std::lock_guard session_lock(command_session->mutex);
  // A Stop may have installed exact settlement while the backend was observed.
  if (command_session->stop_settlement && !command_session->stop_recovery_consumed) {
    auto assessment = command_session->stop_settlement->recovery_assessment;
    assessment.eligible_close = true;
    return assessment;
  }
  return command_session->assess_close(observed, clock_.now(), control_command_epoch_.load());
}
KernelRecoveryActionResultDTO GripperParticipantEngine::apply_recovery_action(
    const KernelRecoveryActionRequestDTO& request) {
  std::shared_ptr<PreparedCapabilityCommandSession> command_session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    command_session = active_prepared_command_session_.load();
  }
  if (command_session) {
    std::optional<GripperCommandStatusDTO> observed;
    bool ordinary_close = false;
    if (request.action == RecoveryAction::kCloseOperation) {
      std::lock_guard session_lock(command_session->mutex);
      ordinary_close = !command_session->stop_settlement || command_session->stop_recovery_consumed;
    }
    if (ordinary_close)
      observed = backend_.get_gripper_status(command_session->authority.gripper().command_request.gripper_id);
    std::optional<KernelRecoveryAssessmentDTO> addressed_assessment;
    std::optional<KernelRecoveryAssessmentDTO> resulting_assessment;
    std::optional<ErrorDTO> rejection;
    {
      std::lock_guard session_lock(command_session->mutex);
      if (command_session->stop_settlement.has_value() &&
          !command_session->stop_recovery_consumed) {
        addressed_assessment = command_session->stop_settlement->recovery_assessment;
        addressed_assessment->eligible_close = true;
        RecoveryAuthorityCapture capture;
        capture.control_command_epoch = addressed_assessment->control_command_epoch;
        const auto decision = evaluate_recovery_admission(request, capture, *addressed_assessment);
        if (!decision.admitted) {
          rejection = recovery_action_rejection_error(request, *addressed_assessment,
                                                      decision.rejection_reason);
        } else if (auto reopened =
                       command_session->state !=
                               ResolvedPreparedCapabilityCommandStateDTO::kDispatched
                           ? Result<void>::Ok()
                           : backend_.try_authorize_gripper_commands();
                   !reopened.ok()) {
          // A dispatched command's Stop settlement may not have reopened
          // admission before its cancel terminal (Q58). Consuming that Stop
          // (Resume/replan or close) is the next reopen point; until the
          // terminal, refuse cleanly with the backend's typed reason.
          rejection = reopened.error();
        } else {
          command_session->stop_recovery_consumed = true;
          KernelRecoveryAssessmentDTO next;
          next.assessment_revision = addressed_assessment->assessment_revision + 1U;
          next.assessed_at_ns = clock_.now();
          next.control_command_epoch = bump_control_command_epoch();
          next.execution_state = ExecutionState::kIdle;
          next.recovery_class = RecoveryClass::kNone;
          next.status = RecoveryAssessmentStatus::kUnavailable;
          next.reason_code = reason_codes::kRecoveryNoneActive;
          resulting_assessment = std::move(next);
        }
      } else if (request.action == RecoveryAction::kCloseOperation) {
        addressed_assessment =
            command_session->assess_close(observed, clock_.now(), control_command_epoch_.load());
        RecoveryAuthorityCapture capture;
        capture.control_command_epoch = addressed_assessment->control_command_epoch;
        const auto decision = evaluate_recovery_admission(request, capture, *addressed_assessment);
        if (!decision.admitted) {
          rejection = recovery_action_rejection_error(request, *addressed_assessment,
                                                      decision.rejection_reason);
        } else {
          // Consume local ownership only; backend admission, attachment and faults persist.
          command_session->ordinary_result_acknowledged = true;
          KernelRecoveryAssessmentDTO next;
          next.assessment_revision = addressed_assessment->assessment_revision + 1U;
          next.assessed_at_ns = clock_.now();
          next.control_command_epoch = bump_control_command_epoch();
          next.reason_code = "operation_closed";
          resulting_assessment = std::move(next);
        }
      }
    }
    if (addressed_assessment.has_value()) {
      KernelEvent event;
      event.execution_id = addressed_assessment->execution_id;
      event.timestamp_ns = clock_.now();
      event.details = {
          {"request_id", request.request_id},
          {"action", recovery_action_name(request.action)},
          {"principal_id", request.principal_id},
          {"assessment_revision", std::to_string(addressed_assessment->assessment_revision)},
          {"stop_certificate_id", addressed_assessment->stop_certificate_id}};
      if (rejection.has_value()) {
        event.type = KernelEventType::kRecoveryActionRejected;
        event.error = rejection;
        event.details.insert(rejection->details.begin(), rejection->details.end());
        emit_event(std::move(event));
        return KernelRecoveryActionResultDTO{request.request_id, request.action, false,
                                             *addressed_assessment, std::move(rejection)};
      }
      event.type = KernelEventType::kRecoveryActionAccepted;
      event.from_state = ExecutionState::kStopped;
      event.to_state = ExecutionState::kIdle;
      event.details["backend_reset_performed"] = "false";
      event.details["reason_code"] = "recovery_action_accepted";
      emit_event(std::move(event));
      return KernelRecoveryActionResultDTO{request.request_id, request.action, true,
                                           *resulting_assessment, std::nullopt};
    }
  }
  return KernelRecoveryActionResultDTO{request.request_id,
                                       request.action,
                                       false,
                                       {},
                                       ErrorDTO{"recovery_unavailable",
                                                "No command recovery assessment is active",
                                                {{"reason", "recovery_assessment_unavailable"}}}};
}

}  // namespace manyforge::core
