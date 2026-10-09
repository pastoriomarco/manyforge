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
    const GripperCommandRequestDTO& request) noexcept {
  const auto* command = std::get_if<PreparedCapabilityCommandDTO>(&participant.payload);
  if (command == nullptr || binding.resources.size() != 1U || !binding.control_path.has_value() ||
      participant.command_target_id != binding.command_target_id) {
    return false;
  }
  const auto& resource = binding.resources.front();
  const auto& value = std::get<GripperPositionCommandValueDTO>(command->command_value);
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
  for (const auto& option : command->validated_options) {
    if (option.option_id.value() != "command_duration_s") {
      return false;
    }
    duration = option.value_s;
  }
  const bool model_backed = resource.source_joint_name.has_value();
  const auto expected_group = model_backed && binding.model_groups.size() == 1U
                                  ? binding.model_groups.front().compiled_group_name
                                  : std::string{};
  return request.gripper_id == binding.command_target_id.value() &&
         request.group_id == expected_group &&
         request.joint_name == resource.compiled_resource_name &&
         request.target_position == target && request.duration_s == duration &&
         request.max_effort == effort && request.commanded_state == "position" &&
         request.actuation_kind == (kind == CoordinateKind::kAngular ? "angular" : "linear") &&
         request.state_authority == binding.control_path->state_authority_id.value() &&
         request.completion_policy == "goal_fenced_target_reached" &&
         request.grasp_confirmation_policy == "binding_feedback_required" &&
         request.transport_kind == "resolved_binding" &&
         request.evidence == std::vector<std::string>{"command_terminal"} &&
         request.evidence_state == "required_pending" &&
         request.completion_timeout_s == command->completion_timeout_s;
}
}  // namespace
struct GripperParticipantEngine::PreparedCapabilityCommandSession final {
  explicit PreparedCapabilityCommandSession(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO supplied)
      : authority(std::move(supplied)) {}

  ResolvedPreparedCapabilityCommandExecutionRequestDTO authority;
  mutable std::mutex mutex;
  std::uint64_t preparation_id{};
  ResolvedPreparedCapabilityCommandStateDTO state{
      ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed};
  std::optional<PrepareParticipantResultDTO> backend_prepare_result;
  std::optional<ControllerGoalId> controller_goal_id;
  std::optional<ControllerGoalGeneration> controller_goal_generation;
  std::optional<GripperCommandResultDTO> dispatch_result;
  std::optional<AddressedParticipantStopSettlementDTO> stop_settlement;
  bool stop_recovery_consumed{};
  std::optional<ErrorDTO> error;
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
  const auto before = backend_.get_gripper_status(id);
  if (before.gripper_id != id || command_id == 0U || before.command_id != command_id ||
      before.generation < generation || before.state == GripperCommandState::kExecuting ||
      (before.generation != generation && before.state != GripperCommandState::kIdle))
    return Result<void>::Err(ErrorDTO{"stale_gripper_state",
                                      "Gripper changed before scoped Reset",
                                      {{"reason", "stale_gripper_state"}}});
  backend_.invalidate_gripper_commands(GripperInvalidationReason::kReset);
  backend_.authorize_gripper_commands();
  return Result<void>::Ok();
}
AddressedParticipantStopFenceDisposition GripperParticipantEngine::fence_addressed_participant_stop(
    const std::uint64_t execution_id) noexcept {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_;
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
      *request.participant, request.binding, request.stop_readiness,
      request.retained_default_stop_limits, request.stop_node_category);
  if (!verified.ok()) {
    return Result<std::uint64_t>::Err(verified.error());
  }
  if (!std::holds_alternative<PreparedCapabilityCommandDTO>(request.participant->payload) ||
      !prepared_command_matches_private_request(*request.participant, request.binding.value(),
                                                request.command_request)) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared capability command differs from its compiled private request",
                 {{"reason", "prepared_command_authority_mismatch"}}});
  }
  auto validated = validate_gripper_command(request.command_request);
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
  session->state = ResolvedPreparedCapabilityCommandStateDTO::kPrepared;
  session->backend_prepare_result = std::move(prepared.value());

  std::lock_guard active_lock(prepared_command_session_mutex_);
  if (active_prepared_command_session_) {
    std::lock_guard prior_lock(active_prepared_command_session_->mutex);
    const auto& prior = *active_prepared_command_session_;
    if (prior.authority.participant &&
        prior.authority.participant->prepared_contract_digest ==
            session->authority.participant->prepared_contract_digest) {
      return Result<std::uint64_t>::Ok(prior.preparation_id);
    }
    bool prior_effect_active =
        prior.state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested;
    if (prior.state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched &&
        prior.dispatch_result.has_value() && prior.dispatch_result->accepted) {
      const auto status =
          get_gripper_status(prior.authority.participant->command_target_id.value());
      prior_effect_active = status.state == GripperCommandState::kExecuting &&
                            status.command_id == prior.dispatch_result->command_id &&
                            status.generation == prior.dispatch_result->generation;
    }
    if (prior.state == ResolvedPreparedCapabilityCommandStateDTO::kPrepared ||
        prior_effect_active) {
      return Result<std::uint64_t>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Another prepared capability command is still resident",
                   {{"reason", "prepared_command_session_busy"}}});
    }
  }
  if (next_execution_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInternalError,
                 "Prepared command identity space is exhausted",
                 {{"reason", "prepared_command_identity_exhausted"}}});
  }
  const auto preparation_id = next_execution_id_++;
  session->preparation_id = preparation_id;
  active_prepared_command_session_ = std::move(session);
  return Result<std::uint64_t>::Ok(preparation_id);
}

Result<ResolvedPreparedCapabilityCommandSnapshotDTO>
GripperParticipantEngine::ObservePreparedCapabilityCommand(
    const std::uint64_t preparation_id) const {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_;
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
    session = active_prepared_command_session_;
  }
  if (!session) {
    return Result<GripperCommandResultDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared capability-command session is not resident",
                 {{"reason", "prepared_command_session_missing"}}});
  }
  {
    std::lock_guard lock(session->mutex);
    if (session->preparation_id != preparation_id) {
      return Result<GripperCommandResultDTO>::Err(
          ErrorDTO{kErrorMissingReference,
                   "Prepared capability-command identity is stale",
                   {{"reason", "prepared_command_identity_mismatch"}}});
    }
    const bool exact_dispatch =
        session->authority.participant &&
        session->authority.participant->prepared_contract_digest == prepared_contract_digest &&
        session->controller_goal_id == controller_goal_id &&
        session->controller_goal_generation == controller_goal_generation;
    if ((session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched ||
         session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed) &&
        exact_dispatch && session->dispatch_result.has_value()) {
      return Result<GripperCommandResultDTO>::Ok(*session->dispatch_result);
    }
    if (session->state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared ||
        !session->authority.participant ||
        session->authority.participant->prepared_contract_digest != prepared_contract_digest ||
        controller_goal_id.empty() || !controller_goal_generation.nonzero()) {
      return Result<GripperCommandResultDTO>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Prepared capability-command dispatch identity is invalid",
                   {{"reason", "prepared_command_dispatch_mismatch"}}});
    }
    session->controller_goal_id = controller_goal_id;
    session->controller_goal_generation = controller_goal_generation;
    session->state = ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested;
  }

  GripperCommandResultDTO dispatched;
  try {
    auto command_request = session->authority.command_request;
    command_request.addressed_identity =
        AddressedGripperCommandIdentityDTO{session->authority.participant->operation_id,
                                           session->authority.participant->operation_revision,
                                           session->authority.participant->participant_id,
                                           controller_goal_id, controller_goal_generation};
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
    session->dispatch_result = dispatched;
    session->error = dispatched.error;
    session->state = dispatched.accepted && !dispatched.error.has_value()
                         ? ResolvedPreparedCapabilityCommandStateDTO::kDispatched
                         : ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed;
  }
  return Result<GripperCommandResultDTO>::Ok(std::move(dispatched));
}

Result<void> GripperParticipantEngine::CancelPreparedCapabilityCommand(
    const std::uint64_t preparation_id) {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared capability-command session is not resident",
                                      {{"reason", "prepared_command_session_missing"}}});
  }
  std::lock_guard lock(session->mutex);
  if (session->preparation_id != preparation_id) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared capability-command identity is stale",
                                      {{"reason", "prepared_command_identity_mismatch"}}});
  }
  if (session->state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch) {
    return Result<void>::Ok();
  }
  if (session->state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared &&
      session->state != ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed) {
    return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                                      "A released capability command requires the Stop-stage path",
                                      {{"reason", "prepared_command_already_dispatched"}}});
  }
  session->state = ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
  return Result<void>::Ok();
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
    command_session = active_prepared_command_session_;
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
          containment, command_session->authority.stop_readiness.value().resolved_stop_base_plan,
          command_session->authority.binding,
          command_session->authority.retained_default_stop_limits,
          command_session->authority.stop_node_category, participant->resources);
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
      auto reset = reset_gripper(command_session->authority.command_request.gripper_id,
                                 dispatched.command_id, dispatched.generation);
      if (!reset.ok()) {
        return Result<AddressedParticipantStopSettlementDTO>::Err(reset.error());
      }
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
    command_session = active_prepared_command_session_;
  }
  if (command_session) {
    std::lock_guard session_lock(command_session->mutex);
    if (command_session->stop_settlement.has_value() && !command_session->stop_recovery_consumed) {
      return command_session->stop_settlement->recovery_assessment;
    }
  }
  return {};
}
KernelRecoveryActionResultDTO GripperParticipantEngine::apply_recovery_action(
    const KernelRecoveryActionRequestDTO& request) {
  std::shared_ptr<PreparedCapabilityCommandSession> command_session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    command_session = active_prepared_command_session_;
  }
  if (command_session) {
    std::optional<KernelRecoveryAssessmentDTO> addressed_assessment;
    std::optional<KernelRecoveryAssessmentDTO> resulting_assessment;
    std::optional<ErrorDTO> rejection;
    {
      std::lock_guard session_lock(command_session->mutex);
      if (command_session->stop_settlement.has_value() &&
          !command_session->stop_recovery_consumed) {
        addressed_assessment = command_session->stop_settlement->recovery_assessment;
        RecoveryAuthorityCapture capture;
        capture.control_command_epoch = addressed_assessment->control_command_epoch;
        const auto decision = evaluate_recovery_admission(request, capture, *addressed_assessment);
        if (!decision.admitted) {
          rejection = recovery_action_rejection_error(request, *addressed_assessment,
                                                      decision.rejection_reason);
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
