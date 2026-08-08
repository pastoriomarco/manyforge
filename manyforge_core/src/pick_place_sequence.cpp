#include <manyforge_core/pick_place_sequence.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

template <typename T>
[[nodiscard]] Result<T> sequence_error(std::string reason,
                                       std::string message) {
  return Result<T>::Err(
      ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
               std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool same_state(const RobotStateDTO& left,
                              const RobotStateDTO& right) noexcept {
  return left.joint_names == right.joint_names &&
         left.positions_rad == right.positions_rad &&
         left.velocities_rad_s == right.velocities_rad_s;
}

[[nodiscard]] bool exact_preview_chain(
    const MotionPreviewResultDTO& approach,
    const MotionPreviewResultDTO& descend,
    const MotionPreviewResultDTO& retreat,
    const ProjectionRevisionDTO& predecessor,
    const ProjectionRevisionDTO& successor) noexcept {
  return approach.projection_binding.projection_revision == predecessor &&
         descend.projection_binding.projection_revision == predecessor &&
         retreat.projection_binding.projection_revision == successor &&
         same_state(approach.final_state, descend.start_state) &&
         same_state(descend.final_state, retreat.start_state);
}

[[nodiscard]] MotionParticipantIntentDTO* motion_payload(
    OperationIntentDTO& operation) noexcept {
  if (operation.participants.size() != 1U) {
    return nullptr;
  }
  return std::get_if<MotionParticipantIntentDTO>(
      &operation.participants.front().payload);
}

[[nodiscard]] bool capability_command_operation(
    const OperationIntentDTO& operation) noexcept {
  return operation.participants.size() == 1U &&
         std::holds_alternative<CapabilityCommandIntentDTO>(
             operation.participants.front().payload);
}

[[nodiscard]] bool same_program_context(const OperationIntentDTO& left,
                                        const OperationIntentDTO& right) {
  return left.program_id == right.program_id &&
         left.program_revision == right.program_revision &&
         left.program_binding_id == right.program_binding_id &&
         left.program_run_id == right.program_run_id &&
         left.expected_graph_revision == right.expected_graph_revision &&
         left.expected_retained_bundle_id ==
             right.expected_retained_bundle_id;
}

[[nodiscard]] bool terminal(const OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] ErrorDTO terminal_operation_error(
    const OperationStateDTO& state) {
  for (const auto& participant : state.participants) {
    if (participant.terminal_error.has_value()) {
      return *participant.terminal_error;
    }
  }
  return ErrorDTO{
      "execution_failed",
      "pick/place phase operation reached a non-success terminal state",
      {{"reason", "pick_place_phase_operation_failed"},
       {"operation_id", state.operation_id.value()}}};
}

[[nodiscard]] const OperationIntentDTO& phase_operation(
    const PickPlaceSequenceRequestDTO& request,
    const PickPlaceSequencePhaseDTO phase) {
  switch (phase) {
    case PickPlaceSequencePhaseDTO::kApproach:
      return request.approach_operation;
    case PickPlaceSequencePhaseDTO::kDescend:
      return request.descend_operation;
    case PickPlaceSequencePhaseDTO::kToolCommand:
      return request.tool_operation;
    case PickPlaceSequencePhaseDTO::kRetreat:
      return request.retreat_operation;
    case PickPlaceSequencePhaseDTO::kIdle:
    case PickPlaceSequencePhaseDTO::kSceneTransition:
    case PickPlaceSequencePhaseDTO::kCompleted:
    case PickPlaceSequencePhaseDTO::kFailed:
    case PickPlaceSequencePhaseDTO::kInterventionRequired:
      break;
  }
  throw std::logic_error("pick/place phase has no operation");
}

[[nodiscard]] Result<void> validate_sequence_request(
    PickPlaceSequenceRequestDTO& request,
    const RequestOwnerEpoch& operation_owner_epoch,
    const ProjectionRevisionDTO& current_projection,
    const SceneObjectStateDTO& object_state) {
  if (request.sequence_id.empty() || request.sequence_id.size() > 128U ||
      request.plan.selected_object_id.empty() ||
      !request.plan.predecessor_projection_revision.valid() ||
      !request.plan.preview_successor_projection_revision.valid() ||
      request.plan.predecessor_projection_revision != current_projection ||
      request.plan.scene_transition.expected_projection_revision !=
          current_projection ||
      request.plan.predecessor_projection_revision.retained_bundle_id !=
          request.plan.preview_successor_projection_revision
              .retained_bundle_id ||
      request.plan.predecessor_projection_revision.graph_revision !=
          request.plan.preview_successor_projection_revision.graph_revision ||
      request.plan.predecessor_projection_revision.scene_revision.scene_id !=
          request.plan.preview_successor_projection_revision.scene_revision
              .scene_id ||
      request.plan.predecessor_projection_revision.scene_revision ==
          request.plan.preview_successor_projection_revision.scene_revision ||
      !object_state.exists ||
      object_state.scene_resource_id != request.plan.selected_object_id) {
    return sequence_error<void>(
        "pick_place_sequence_plan_stale_or_invalid",
        "pick/place sequence plan is invalid, stale, or names a missing "
        "selected object");
  }

  std::array<OperationIntentDTO*, 4U> operations{
      &request.approach_operation, &request.descend_operation,
      &request.tool_operation, &request.retreat_operation};
  std::set<RequestId> request_ids;
  for (auto* operation : operations) {
    auto valid = validate_operation_intent(*operation);
    if (!valid.ok() ||
        operation->request_context.request_owner_epoch !=
            operation_owner_epoch ||
        operation->expected_graph_revision !=
            current_projection.graph_revision ||
        operation->expected_retained_bundle_id !=
            current_projection.retained_bundle_id ||
        !same_program_context(request.approach_operation, *operation) ||
        !request_ids.insert(operation->request_context.request_id).second) {
      return sequence_error<void>(
          "pick_place_operation_template_invalid",
          "pick/place phase operations are invalid, duplicate, stale, or "
          "cross their exact Program/deployment context");
    }
  }
  if (motion_payload(request.approach_operation) == nullptr ||
      motion_payload(request.descend_operation) == nullptr ||
      !capability_command_operation(request.tool_operation) ||
      motion_payload(request.retreat_operation) == nullptr ||
      request.plan.scene_transition.request_context.request_id.empty() ||
      request_ids.contains(
          request.plan.scene_transition.request_context.request_id)) {
    return sequence_error<void>(
        "pick_place_phase_payload_invalid",
        "pick/place requires three motion phases, one tool command, and one "
        "distinct scene transition");
  }

  auto* approach = motion_payload(request.approach_operation);
  auto* descend = motion_payload(request.descend_operation);
  auto* retreat = motion_payload(request.retreat_operation);
  approach->target_mode = MotionTargetModeDTO::kAbsolute;
  approach->target_reference.reset();
  approach->offset_reference.reset();
  approach->motion = request.plan.approach_motion;
  approach->contact_policy_id.reset();
  approach->runtime_object_bindings.clear();
  descend->target_mode = MotionTargetModeDTO::kAbsolute;
  descend->target_reference.reset();
  descend->offset_reference.reset();
  descend->motion = request.plan.descend_motion;
  retreat->target_mode = MotionTargetModeDTO::kAbsolute;
  retreat->target_reference.reset();
  retreat->offset_reference.reset();
  retreat->motion = request.plan.retreat_motion;
  retreat->contact_policy_id.reset();
  retreat->runtime_object_bindings.clear();

  for (auto* operation : operations) {
    operation->expected_scene_revision =
        current_projection.scene_revision;
  }
  if (request.plan.kind == PickPlaceExecutionKindDTO::kPick) {
    const auto* attach = std::get_if<AttachCarriedObjectRequestDTO>(
        &request.plan.scene_transition.operation);
    const bool selected_runtime_object =
        descend->contact_policy_id.has_value() &&
        std::count_if(
            descend->runtime_object_bindings.begin(),
            descend->runtime_object_bindings.end(), [&](const auto& binding) {
              return binding.scene_resource_id ==
                     request.plan.selected_object_id;
            }) == 1;
    if (object_state.attached || attach == nullptr ||
        attach->scene_resource_id != request.plan.selected_object_id ||
        !selected_runtime_object) {
      return sequence_error<void>(
          "pick_precondition_invalid",
          "pick requires one detached selected object, one concrete grasp "
          "binding, and one matching attach transition");
    }
  } else {
    const auto* detach = std::get_if<DetachCarriedObjectRequestDTO>(
        &request.plan.scene_transition.operation);
    if (!object_state.attached || detach == nullptr ||
        !object_state.carried_object_attachment_id.has_value() ||
        *object_state.carried_object_attachment_id !=
            detach->carried_object_attachment_id ||
        descend->contact_policy_id.has_value() ||
        !descend->runtime_object_bindings.empty()) {
      return sequence_error<void>(
          "place_precondition_invalid",
          "place requires the exact carried selected object, no grasp "
          "allowance, and one matching detach transition");
    }
  }
  return Result<void>::Ok();
}

}  // namespace

Result<PickPlaceExecutionPlanDTO> make_pick_execution_plan(
    const PlannedPickCandidateDTO& planned) {
  const auto* attach = std::get_if<AttachCarriedObjectRequestDTO>(
      &planned.attach_request.operation);
  const auto* resolved = std::get_if<ResolvedAttachCarriedObjectDTO>(
      &planned.attached_scene.resolved_operation);
  const auto& predecessor =
      planned.approach_preview.projection_binding.projection_revision;
  const auto& successor = planned.attached_scene.proposed_projection_revision;
  if (attach == nullptr || resolved == nullptr ||
      !planned.attached_scene.valid() ||
      planned.attached_scene.prior_projection_revision != predecessor ||
      planned.attach_request.expected_projection_revision != predecessor ||
      resolved->object.scene_resource_id != attach->scene_resource_id ||
      resolved->attachment.scene_resource_id != attach->scene_resource_id ||
      !exact_preview_chain(planned.approach_preview,
                           planned.descend_preview,
                           planned.retreat_preview, predecessor, successor)) {
    return sequence_error<PickPlaceExecutionPlanDTO>(
        "pick_execution_plan_invalid",
        "selected pick candidate does not carry one exact chained attach "
        "preview");
  }
  return Result<PickPlaceExecutionPlanDTO>::Ok(PickPlaceExecutionPlanDTO{
      PickPlaceExecutionKindDTO::kPick,
      attach->scene_resource_id,
      predecessor,
      successor,
      PoseGoalDTO{planned.candidate.approach_pose},
      planned.candidate.descend_path,
      planned.candidate.retreat_path,
      planned.attach_request});
}

Result<PickPlaceExecutionPlanDTO> make_place_execution_plan(
    const PlannedPlaceDTO& planned,
    const SceneResourceId& selected_object_id) {
  const auto* detach = std::get_if<DetachCarriedObjectRequestDTO>(
      &planned.detach_request.operation);
  const auto* resolved = std::get_if<ResolvedDetachCarriedObjectDTO>(
      &planned.detached_scene.resolved_operation);
  const auto& predecessor =
      planned.approach_preview.projection_binding.projection_revision;
  const auto& successor = planned.detached_scene.proposed_projection_revision;
  if (selected_object_id.empty() || detach == nullptr || resolved == nullptr ||
      !planned.detached_scene.valid() ||
      planned.detached_scene.prior_projection_revision != predecessor ||
      planned.detach_request.expected_projection_revision != predecessor ||
      resolved->object.scene_resource_id != selected_object_id ||
      !exact_preview_chain(planned.approach_preview,
                           planned.descend_preview,
                           planned.retreat_preview, predecessor, successor)) {
    return sequence_error<PickPlaceExecutionPlanDTO>(
        "place_execution_plan_invalid",
        "selected place plan does not carry one exact chained detach "
        "preview");
  }
  return Result<PickPlaceExecutionPlanDTO>::Ok(PickPlaceExecutionPlanDTO{
      PickPlaceExecutionKindDTO::kPlace,
      selected_object_id,
      predecessor,
      successor,
      planned.approach_motion,
      planned.descend_motion,
      planned.retreat_motion,
      planned.detach_request});
}

Result<PickPlaceSequenceStatusDTO> PickPlaceSequenceCoordinator::Begin(
    PickPlaceSequenceRequestDTO request) {
  std::lock_guard lock(mutex_);
  if (request_.has_value() && request_->sequence_id == request.sequence_id) {
    return sequence_error<PickPlaceSequenceStatusDTO>(
        "pick_place_sequence_id_conflict",
        "pick/place sequence identity was already used");
  }
  if (request_.has_value() &&
      status_.phase != PickPlaceSequencePhaseDTO::kCompleted &&
      status_.phase != PickPlaceSequencePhaseDTO::kFailed &&
      status_.phase != PickPlaceSequencePhaseDTO::kInterventionRequired) {
    return sequence_error<PickPlaceSequenceStatusDTO>(
        "pick_place_sequence_busy",
        "another pick/place sequence is not terminal");
  }
  const auto current_projection = scene_.CurrentProjectionRevision();
  const auto object_state =
      scene_.GetObjectState(request.plan.selected_object_id);
  auto valid = validate_sequence_request(
      request, operations_.request_owner_epoch(), current_projection,
      object_state);
  if (!valid.ok()) {
    return Result<PickPlaceSequenceStatusDTO>::Err(valid.error());
  }
  status_ = PickPlaceSequenceStatusDTO{
      request.sequence_id, request.plan.kind,
      PickPlaceSequencePhaseDTO::kApproach,
      request.plan.selected_object_id, {}, std::nullopt, std::nullopt,
      std::nullopt};
  request_ = std::move(request);
  auto dispatched = DispatchCurrentPhase();
  if (!dispatched.ok()) {
    SetTerminal(PickPlaceSequencePhaseDTO::kFailed, dispatched.error());
    return Result<PickPlaceSequenceStatusDTO>::Err(dispatched.error());
  }
  return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
}

Result<PickPlaceSequenceStatusDTO> PickPlaceSequenceCoordinator::Reconcile() {
  std::lock_guard lock(mutex_);
  if (!request_.has_value() ||
      status_.phase == PickPlaceSequencePhaseDTO::kIdle) {
    return sequence_error<PickPlaceSequenceStatusDTO>(
        "pick_place_sequence_absent", "no pick/place sequence is active");
  }
  if (status_.phase == PickPlaceSequencePhaseDTO::kCompleted ||
      status_.phase == PickPlaceSequencePhaseDTO::kFailed ||
      status_.phase == PickPlaceSequencePhaseDTO::kInterventionRequired) {
    return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
  }
  auto reconciled = operations_.reconcile_subordinate_state();
  if (!reconciled.ok()) {
    SetTerminal(PickPlaceSequencePhaseDTO::kInterventionRequired,
                reconciled.error());
    return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
  }
  if (!status_.active_operation_id.has_value()) {
    SetTerminal(
        PickPlaceSequencePhaseDTO::kInterventionRequired,
        ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
                 "pick/place operation phase lost its active operation",
                 {{"reason", "pick_place_active_operation_missing"}}});
    return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
  }
  auto observed =
      operations_.get_operation_state(*status_.active_operation_id);
  if (!observed.ok()) {
    SetTerminal(PickPlaceSequencePhaseDTO::kInterventionRequired,
                observed.error());
    return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
  }
  if (!terminal(observed.value().state)) {
    return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
  }
  if (observed.value().state != OperationStateKind::kCompleted) {
    const auto phase = observed.value().state ==
                               OperationStateKind::kInterventionRequired
                           ? PickPlaceSequencePhaseDTO::kInterventionRequired
                           : PickPlaceSequencePhaseDTO::kFailed;
    SetTerminal(phase, terminal_operation_error(observed.value()));
    return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
  }
  auto completed = CompleteActiveOperation(observed.value());
  if (!completed.ok() && status_.phase !=
                             PickPlaceSequencePhaseDTO::kFailed &&
      status_.phase != PickPlaceSequencePhaseDTO::kInterventionRequired) {
    SetTerminal(PickPlaceSequencePhaseDTO::kInterventionRequired,
                completed.error());
  }
  return Result<PickPlaceSequenceStatusDTO>::Ok(status_);
}

PickPlaceSequenceStatusDTO PickPlaceSequenceCoordinator::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

Result<void> PickPlaceSequenceCoordinator::DispatchCurrentPhase() {
  if (!request_.has_value()) {
    return sequence_error<void>("pick_place_sequence_absent",
                                "pick/place sequence request is absent");
  }
  auto operation = phase_operation(*request_, status_.phase);
  if (status_.phase == PickPlaceSequencePhaseDTO::kRetreat) {
    if (!status_.committed_projection_revision.has_value()) {
      return sequence_error<void>(
          "pick_place_scene_transition_missing",
          "retreat cannot dispatch before an exact scene commit");
    }
    operation.expected_scene_revision =
        status_.committed_projection_revision->scene_revision;
  }
  auto accepted = operations_.execute(operation);
  if (!accepted.ok()) {
    return Result<void>::Err(accepted.error());
  }
  status_.active_operation_id = accepted.value().operation_id;
  return Result<void>::Ok();
}

Result<void> PickPlaceSequenceCoordinator::CompleteActiveOperation(
    const OperationStateDTO& operation) {
  if (!request_.has_value() || !status_.active_operation_id.has_value() ||
      operation.operation_id != *status_.active_operation_id) {
    return sequence_error<void>(
        "pick_place_operation_completion_mismatch",
        "pick/place completion differs from its active operation");
  }
  const auto& source = phase_operation(*request_, status_.phase);
  auto acknowledged = operations_.acknowledge_completed(
      OperationControlRequestDTO{
          PublicRequestContextDTO{
              RequestId(request_->sequence_id + ".ack." +
                        std::string(to_string(status_.phase))),
              operations_.request_owner_epoch(),
              "pick_place_sequence_acknowledge", request_->sequence_id},
          operation.operation_id, operation.operation_revision,
          std::optional<std::string>("pick/place phase completed"),
          source.attribution});
  if (!acknowledged.ok()) {
    SetTerminal(PickPlaceSequencePhaseDTO::kInterventionRequired,
                acknowledged.error());
    return Result<void>::Err(acknowledged.error());
  }
  status_.completed_operations.push_back(operation.operation_id);
  status_.active_operation_id.reset();

  switch (status_.phase) {
    case PickPlaceSequencePhaseDTO::kApproach:
      status_.phase = PickPlaceSequencePhaseDTO::kDescend;
      break;
    case PickPlaceSequencePhaseDTO::kDescend:
      status_.phase = PickPlaceSequencePhaseDTO::kToolCommand;
      break;
    case PickPlaceSequencePhaseDTO::kToolCommand: {
      status_.phase = PickPlaceSequencePhaseDTO::kSceneTransition;
      auto committed = CommitSceneTransition();
      if (!committed.ok()) {
        SetTerminal(PickPlaceSequencePhaseDTO::kInterventionRequired,
                    committed.error());
        return committed;
      }
      status_.phase = PickPlaceSequencePhaseDTO::kRetreat;
      break;
    }
    case PickPlaceSequencePhaseDTO::kRetreat:
      status_.phase = PickPlaceSequencePhaseDTO::kCompleted;
      return Result<void>::Ok();
    case PickPlaceSequencePhaseDTO::kIdle:
    case PickPlaceSequencePhaseDTO::kSceneTransition:
    case PickPlaceSequencePhaseDTO::kCompleted:
    case PickPlaceSequencePhaseDTO::kFailed:
    case PickPlaceSequencePhaseDTO::kInterventionRequired:
      return sequence_error<void>(
          "pick_place_phase_transition_invalid",
          "pick/place completion occurred outside an operation phase");
  }
  auto dispatched = DispatchCurrentPhase();
  if (!dispatched.ok()) {
    SetTerminal(PickPlaceSequencePhaseDTO::kFailed, dispatched.error());
  }
  return dispatched;
}

Result<void> PickPlaceSequenceCoordinator::CommitSceneTransition() {
  if (!request_.has_value() ||
      scene_.CurrentProjectionRevision() !=
          request_->plan.predecessor_projection_revision) {
    return sequence_error<void>(
        "pick_place_scene_changed_before_transition",
        "logical scene changed after tool completion and before the selected "
        "transition");
  }
  const auto committed = scene_.Commit(request_->plan.scene_transition);
  if (!committed.success ||
      !committed.committed_projection_revision.has_value()) {
    return Result<void>::Err(
        committed.error.value_or(ErrorDTO{
            std::string(to_string(ErrorCode::kInvariantViolation)),
            "pick/place scene transition did not produce a committed result",
            {{"reason", "pick_place_scene_transition_failed"}}}));
  }
  status_.committed_projection_revision =
      *committed.committed_projection_revision;
  if (*committed.committed_projection_revision !=
      request_->plan.preview_successor_projection_revision) {
    return sequence_error<void>(
        "pick_place_scene_successor_mismatch",
        "durable scene successor differs from the exact previewed successor");
  }
  return Result<void>::Ok();
}

void PickPlaceSequenceCoordinator::SetTerminal(
    const PickPlaceSequencePhaseDTO phase, ErrorDTO error) {
  status_.phase = phase;
  status_.terminal_error = std::move(error);
}

}  // namespace manyforge::core
