#include <algorithm>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <set>
#include <utility>

namespace manyforge::core {
namespace {

template <typename T>
[[nodiscard]] Result<T> planning_error(std::string reason,
                                       std::string message,
                                       ErrorCode code =
                                           ErrorCode::kInvalidRequest) {
  return Result<T>::Err(ErrorDTO{std::string(to_string(code)),
                                 std::move(message),
                                 {{"reason", std::move(reason)}}});
}

[[nodiscard]] const MotionParticipantIntentDTO* motion_payload(
    const OperationIntentDTO& operation) noexcept {
  if (operation.participants.size() != 1U) {
    return nullptr;
  }
  return std::get_if<MotionParticipantIntentDTO>(
      &operation.participants.front().payload);
}

[[nodiscard]] bool command_payload(
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
             right.expected_retained_bundle_id &&
         left.expected_scene_revision == right.expected_scene_revision &&
         left.request_context.request_owner_epoch ==
             right.request_context.request_owner_epoch;
}

[[nodiscard]] bool same_motion_target(const OperationIntentDTO& left,
                                      const OperationIntentDTO& right) {
  if (left.participants.size() != 1U || right.participants.size() != 1U) {
    return false;
  }
  const auto* lhs = motion_payload(left);
  const auto* rhs = motion_payload(right);
  return lhs != nullptr && rhs != nullptr &&
         left.participants.front().component_role_id ==
             right.participants.front().component_role_id &&
         left.participants.front().target_role_id ==
             right.participants.front().target_role_id &&
         left.participants.front().required_capability_id ==
             right.participants.front().required_capability_id &&
         lhs->base_frame_role_id == rhs->base_frame_role_id &&
         lhs->tcp_frame_role_id == rhs->tcp_frame_role_id;
}

[[nodiscard]] Result<void> validate_common(
    const std::string& sequence_id,
    const SceneResourceId& selected_object_id,
    const FrameRoleId& carrier_frame_role_id,
    const OperationIntentDTO& approach,
    const OperationIntentDTO& descend,
    const OperationIntentDTO& tool,
    const OperationIntentDTO& retreat,
    const ProjectionRevisionDTO& current_projection) {
  if (sequence_id.empty() ||
      sequence_id.size() > kMaxPickPlaceSequenceIdBytes ||
      selected_object_id.empty() || carrier_frame_role_id.empty() ||
      motion_payload(approach) == nullptr ||
      motion_payload(descend) == nullptr || !command_payload(tool) ||
      motion_payload(retreat) == nullptr ||
      !same_program_context(approach, descend) ||
      !same_program_context(approach, tool) ||
      !same_program_context(approach, retreat) ||
      !same_motion_target(approach, descend) ||
      !same_motion_target(approach, retreat) ||
      !approach.expected_scene_revision.has_value() ||
      *approach.expected_scene_revision != current_projection.scene_revision ||
      approach.expected_graph_revision != current_projection.graph_revision ||
      approach.expected_retained_bundle_id !=
          current_projection.retained_bundle_id) {
    return planning_error<void>(
        "pick_place_public_intent_invalid",
        "pick/place planning intent is incomplete, stale, or crosses its "
        "Program, arm, scene, or deployment context");
  }
  std::set<RequestId> request_ids{
      approach.request_context.request_id,
      descend.request_context.request_id,
      tool.request_context.request_id,
      retreat.request_context.request_id};
  if (request_ids.size() != 4U) {
    return planning_error<void>(
        "pick_place_request_id_duplicate",
        "pick/place phase request identities must be distinct");
  }
  return Result<void>::Ok();
}

[[nodiscard]] const SceneObjectSnapshotDTO* find_object(
    const LogicalSceneSnapshotDTO& scene,
    const SceneResourceId& id) noexcept {
  const auto found = std::lower_bound(
      scene.objects.begin(), scene.objects.end(), id,
      [](const SceneObjectSnapshotDTO& candidate,
         const SceneResourceId& selected) {
        return candidate.scene_resource_id < selected;
      });
  return found == scene.objects.end() || found->scene_resource_id != id
             ? nullptr
             : &*found;
}

[[nodiscard]] const CarriedObjectAttachmentDTO* find_attachment(
    const LogicalSceneSnapshotDTO& scene,
    const SceneResourceId& id) noexcept {
  const auto found = std::find_if(
      scene.carried_object_attachments.begin(),
      scene.carried_object_attachments.end(), [&](const auto& candidate) {
        return candidate.scene_resource_id == id;
      });
  return found == scene.carried_object_attachments.end() ? nullptr : &*found;
}

[[nodiscard]] const ProgramFrameRoleBindingDTO* find_frame(
    const RuntimeParticipantBindingDTO& binding,
    const FrameRoleId& role) noexcept {
  const auto found = std::lower_bound(
      binding.frame_bindings.begin(), binding.frame_bindings.end(), role,
      [](const ProgramFrameRoleBindingDTO& candidate,
         const FrameRoleId& selected) {
        return candidate.frame_role_id < selected;
      });
  return found == binding.frame_bindings.end() ||
                 found->frame_role_id != role
             ? nullptr
             : &*found;
}

[[nodiscard]] bool exact_frame_receipt(
    const SceneFrameResolutionReceiptDTO& receipt,
    const ProjectionLeaseBindingDTO& projection_binding,
    const RuntimeParticipantBindingDTO& binding,
    const FrameRoleId& frame_role_id) noexcept {
  const auto* expected = find_frame(binding, frame_role_id);
  const auto* resolved = std::get_if<ResolvedComponentModelFrameDTO>(
      &receipt.resolved_frame);
  return expected != nullptr && resolved != nullptr && receipt.valid() &&
         receipt.projection_binding == projection_binding &&
         resolved->controllable_component_id ==
             binding.controllable_component_id &&
         resolved->model_instance_id == expected->model_instance_id &&
         resolved->frame_role_id == expected->frame_role_id &&
         resolved->compiled_frame_name == expected->compiled_frame_name &&
         resolved->model_revision == expected->model_revision;
}

[[nodiscard]] bool exact_predecessor(
    const CollisionWorldLeaseInventory& predecessor,
    const ProjectionRevisionDTO& revision,
    const ProjectionLeaseId& lease_id) noexcept {
  return predecessor.projection_binding ==
             ProjectionLeaseBindingDTO{revision, lease_id} &&
         predecessor.logical_scene &&
         predecessor.logical_scene->locally_valid() &&
         predecessor.logical_scene->revision == revision.scene_revision &&
         predecessor.bundle.world && predecessor.bundle.policy.has_value();
}

[[nodiscard]] Result<std::vector<CollisionBodyRoleId>> touch_roles(
    const RuntimeParticipantBindingDTO& arm_binding,
    const RuntimeParticipantBindingDTO& carrier_binding,
    const ContactPolicyId& policy_id,
    const RuntimeObjectId& runtime_object_id) {
  const auto policy = std::lower_bound(
      arm_binding.contact_grasp_bindings.begin(),
      arm_binding.contact_grasp_bindings.end(), policy_id,
      [](const ProgramContactGraspBindingDTO& candidate,
         const ContactPolicyId& selected) {
        return candidate.contact_policy_id < selected;
      });
  if (policy == arm_binding.contact_grasp_bindings.end() ||
      policy->contact_policy_id != policy_id ||
      policy->runtime_object_id != runtime_object_id) {
    return planning_error<std::vector<CollisionBodyRoleId>>(
        "pick_contact_grasp_template_unresolved",
        "pick contact policy does not resolve the selected Program runtime "
        "object", ErrorCode::kMissingReference);
  }
  std::vector<CollisionBodyRoleId> roles;
  for (const auto& body : policy->allowed_body_bindings) {
    if (body.controllable_component_id !=
            carrier_binding.controllable_component_id ||
        body.command_target_id != carrier_binding.command_target_id) {
      continue;
    }
    const auto exact = std::lower_bound(
        carrier_binding.collision_body_bindings.begin(),
        carrier_binding.collision_body_bindings.end(),
        body.collision_body_role_id,
        [](const ProgramCollisionBodyRoleBindingDTO& candidate,
           const CollisionBodyRoleId& selected) {
          return candidate.collision_body_role_id < selected;
        });
    if (exact == carrier_binding.collision_body_bindings.end() ||
        exact->collision_body_role_id != body.collision_body_role_id ||
        exact->model_instance_id != body.model_instance_id ||
        exact->model_revision != body.model_revision) {
      return planning_error<std::vector<CollisionBodyRoleId>>(
          "pick_touch_body_binding_stale",
          "pick touch-body role differs from the exact carrier binding",
          ErrorCode::kInvariantViolation);
    }
    roles.push_back(body.collision_body_role_id);
  }
  std::sort(roles.begin(), roles.end());
  roles.erase(std::unique(roles.begin(), roles.end()), roles.end());
  if (roles.empty()) {
    return planning_error<std::vector<CollisionBodyRoleId>>(
        "pick_carrier_touch_roles_unresolved",
        "pick contact policy has no touch body on the exact carrier target",
        ErrorCode::kMissingReference);
  }
  return Result<std::vector<CollisionBodyRoleId>>::Ok(std::move(roles));
}

struct ProjectionLeaseGuard final {
  IPlannerSceneProjection& projection;
  ProjectionLeaseId lease_id;
  ~ProjectionLeaseGuard() { projection.ReleaseProjection(lease_id); }
};

[[nodiscard]] SceneTransactionRequestDTO attach_request(
    const PickSequencePlanningIntentDTO& intent,
    const ProjectionRevisionDTO& projection_revision,
    const RuntimeParticipantBindingDTO& carrier_binding,
    std::vector<CollisionBodyRoleId> roles,
    std::vector<ComponentAttachmentRevision> structural_revisions,
    const RequestOwnerEpoch& scene_owner_epoch) {
  return SceneTransactionRequestDTO{
      PublicRequestContextDTO{
          RequestId(intent.sequence_id + ".scene.attach"), scene_owner_epoch,
          "pick_place_planning_owner",
          intent.approach_operation.request_context.correlation_id},
      intent.approach_operation.attribution,
      projection_revision,
      AttachCarriedObjectRequestDTO{
          CarriedObjectAttachmentId("carried:" + intent.sequence_id),
          intent.selected_object_id, carrier_binding.controllable_component_id,
          carrier_binding.command_target_id, intent.carrier_frame_role_id,
          std::move(roles), std::move(structural_revisions)}};
}

[[nodiscard]] SceneTransactionRequestDTO detach_request(
    const PlaceSequencePlanningIntentDTO& intent,
    const ProjectionRevisionDTO& projection_revision,
    const CarriedObjectAttachmentDTO& attachment,
    const RequestOwnerEpoch& scene_owner_epoch) {
  return SceneTransactionRequestDTO{
      PublicRequestContextDTO{
          RequestId(intent.sequence_id + ".scene.detach"), scene_owner_epoch,
          "pick_place_planning_owner",
          intent.approach_operation.request_context.correlation_id},
      intent.approach_operation.attribution,
      projection_revision,
      DetachCarriedObjectRequestDTO{
          attachment.carried_object_attachment_id,
          attachment.carried_object_attachment_revision, Pose3DTO{},
          attachment.carrier_frame_ref}};
}

}  // namespace

Result<PickPlacePlanningResultDTO> AddressedPickPlacePlanningOwner::PlanPick(
    PickSequencePlanningIntentDTO intent,
    const std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return planning_error<PickPlacePlanningResultDTO>(
        "pick_planning_cancelled", "pick planning was cancelled",
        ErrorCode::kExecutionAborted);
  }
  const auto current_projection = scene_.CurrentProjectionRevision();
  auto common = validate_common(
      intent.sequence_id, intent.selected_object_id,
      intent.carrier_frame_role_id, intent.approach_operation,
      intent.descend_operation, intent.tool_operation,
      intent.retreat_operation, current_projection);
  const auto* approach_motion = motion_payload(intent.approach_operation);
  const auto* descend_motion = motion_payload(intent.descend_operation);
  const auto* retreat_motion = motion_payload(intent.retreat_operation);
  if (!common.ok() || intent.selected_runtime_object_id.empty() ||
      approach_motion == nullptr || descend_motion == nullptr ||
      retreat_motion == nullptr ||
      approach_motion->contact_policy_id.has_value() ||
      !approach_motion->runtime_object_bindings.empty() ||
      !descend_motion->contact_policy_id.has_value() ||
      descend_motion->runtime_object_bindings !=
          std::vector<RuntimeObjectReferenceBindingDTO>{
              {intent.selected_runtime_object_id,
               intent.selected_object_id}} ||
      retreat_motion->contact_policy_id.has_value() ||
      !retreat_motion->runtime_object_bindings.empty()) {
    return common.ok()
               ? planning_error<PickPlacePlanningResultDTO>(
                     "pick_public_phase_contract_invalid",
                     "pick requires contact only on descend and one exact "
                     "selected runtime-object binding")
               : Result<PickPlacePlanningResultDTO>::Err(common.error());
  }

  auto session =
      motion_.begin_motion_preview_session(intent.approach_operation);
  if (!session.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(session.error());
  }
  auto prepared = session.value()->prepare_motion(intent.approach_operation);
  auto arm = session.value()->resolve_binding(intent.approach_operation);
  auto carrier = session.value()->resolve_binding(intent.tool_operation);
  if (!prepared.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(prepared.error());
  }
  if (!arm.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(arm.error());
  }
  if (!carrier.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(carrier.error());
  }
  if (find_frame(carrier.value(), intent.carrier_frame_role_id) == nullptr) {
    return planning_error<PickPlacePlanningResultDTO>(
        "pick_carrier_frame_unresolved",
        "carrier frame role is absent from the exact tool binding",
        ErrorCode::kMissingReference);
  }
  auto roles = touch_roles(
      arm.value(), carrier.value(), *descend_motion->contact_policy_id,
      intent.selected_runtime_object_id);
  if (!roles.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(roles.error());
  }

  const ProjectionLeaseId predecessor_lease("pick-place:" +
                                             intent.sequence_id +
                                             ":predecessor");
  auto predecessor =
      projection_.RetainProjection(current_projection, predecessor_lease);
  if (!predecessor.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(predecessor.error());
  }
  ProjectionLeaseGuard release{projection_, predecessor_lease};
  if (!exact_predecessor(predecessor.value(), current_projection,
                         predecessor_lease)) {
    return planning_error<PickPlacePlanningResultDTO>(
        "pick_predecessor_projection_stale",
        "retained pick projection differs from current scene truth",
        ErrorCode::kSceneChangedDuringValidation);
  }
  const auto* selected =
      find_object(*predecessor.value().logical_scene,
                  intent.selected_object_id);
  if (selected == nullptr ||
      !std::holds_alternative<CellFixedSceneObjectPlacementDTO>(
          selected->placement)) {
    return planning_error<PickPlacePlanningResultDTO>(
        "pick_selected_object_not_detached",
        "pick requires one detached selected scene object",
        ErrorCode::kSceneObjectStateConflict);
  }

  const auto& arm_binding = prepared.value().binding;
  const TargetCarrierSceneFrameRefDTO base_selector{
      arm_binding.controllable_component_id, arm_binding.command_target_id,
      approach_motion->base_frame_role_id};
  auto base = projection_.ResolveSceneFrame(
      SceneFrameResolutionQueryDTO{base_selector,
                                   predecessor.value().projection_binding});
  const TargetCarrierSceneFrameRefDTO carrier_selector{
      carrier.value().controllable_component_id,
      carrier.value().command_target_id, intent.carrier_frame_role_id};
  auto carrier_frame = projection_.ResolveSceneFrame(
      SceneFrameResolutionQueryDTO{carrier_selector,
                                   predecessor.value().projection_binding});
  if (!base.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(base.error());
  }
  if (!carrier_frame.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(carrier_frame.error());
  }
  if (!exact_frame_receipt(base.value(),
                           predecessor.value().projection_binding,
                           arm.value(), approach_motion->base_frame_role_id) ||
      !exact_frame_receipt(carrier_frame.value(),
                           predecessor.value().projection_binding,
                           carrier.value(), intent.carrier_frame_role_id)) {
    return planning_error<PickPlacePlanningResultDTO>(
        "pick_frame_resolution_receipt_invalid",
        "pick frame resolution differs from the exact retained arm or "
        "carrier binding", ErrorCode::kInvariantViolation);
  }
  const auto& fixed =
      std::get<CellFixedSceneObjectPlacementDTO>(selected->placement);
  const Pose3DTO object_in_base = multiply_poses(
      inverse_pose(base.value().frame_pose_in_cell), fixed.pose_in_cell);

  PickCandidateSynthesisConfigDTO synthesis{
      intent.synthesis.pick_offset_envelope,
      intent.synthesis.approach_offset_envelope,
      PickOffsetBasisDTO{intent.synthesis.pick_offset_uses_candidate_tcp,
                         QuaternionDTO{}},
      PickOffsetBasisDTO{intent.synthesis.approach_offset_uses_candidate_tcp,
                         QuaternionDTO{}},
      intent.synthesis.base_tool_orientation_rpy_deg,
      intent.synthesis.translation_step_m,
      intent.synthesis.rotation_step_deg,
      intent.synthesis.force_vertical_pick,
      intent.synthesis.min_tcp_z_m,
      intent.synthesis.maximum_candidate_count,
      intent.synthesis.search_strategy};
  auto grid = PickCandidateGrid::Create(*selected, object_in_base,
                                        std::move(synthesis));
  if (!grid.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(grid.error());
  }
  auto state = session.value()->current_state();
  if (!state.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(state.error());
  }
  auto attach = attach_request(
      intent, current_projection, carrier.value(), std::move(roles.value()),
      carrier_frame.value().component_attachment_revisions,
      scene_.CurrentRequestOwnerEpoch());
  AddressedPickCandidatePlanner planner(*session.value(), scene_candidates_,
                                        projection_);
  auto outcome = planner.Search(
      PickCandidatePlanningRequestDTO{
          std::move(grid.value()), intent.approach_operation,
          intent.descend_operation, intent.retreat_operation,
          std::move(state.value()), std::move(predecessor.value()),
          ResolvedSceneCandidatePreviewRequestDTO{std::move(attach),
                                                  std::nullopt},
          carrier_selector, "pick-place:" + intent.sequence_id + ":candidate"},
      stop_token);
  if (!outcome.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(outcome.error());
  }
  PickPlacePlanningResultDTO result{
      PickPlacePlanningStatusDTO::kNoFeasibleCandidate,
      intent.selected_object_id,
      outcome.value().attempted_candidate_count,
      std::move(outcome.value().reports), std::nullopt};
  if (!outcome.value().selected.has_value()) {
    return Result<PickPlacePlanningResultDTO>::Ok(std::move(result));
  }
  auto plan = make_pick_execution_plan(*outcome.value().selected);
  if (!plan.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(plan.error());
  }
  result.status = PickPlacePlanningStatusDTO::kPlanned;
  result.sequence_request = PickPlaceSequenceRequestDTO{
      intent.sequence_id, std::move(plan.value()),
      std::move(intent.approach_operation),
      std::move(intent.descend_operation), std::move(intent.tool_operation),
      std::move(intent.retreat_operation)};
  return Result<PickPlacePlanningResultDTO>::Ok(std::move(result));
}

Result<PickPlacePlanningResultDTO> AddressedPickPlacePlanningOwner::PlanPlace(
    PlaceSequencePlanningIntentDTO intent,
    const std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return planning_error<PickPlacePlanningResultDTO>(
        "place_planning_cancelled", "place planning was cancelled",
        ErrorCode::kExecutionAborted);
  }
  const auto current_projection = scene_.CurrentProjectionRevision();
  auto common = validate_common(
      intent.sequence_id, intent.selected_object_id,
      intent.carrier_frame_role_id, intent.approach_operation,
      intent.descend_operation, intent.tool_operation,
      intent.retreat_operation, current_projection);
  const auto* approach_motion = motion_payload(intent.approach_operation);
  const auto* descend_motion = motion_payload(intent.descend_operation);
  const auto* retreat_motion = motion_payload(intent.retreat_operation);
  if (!common.ok() || approach_motion == nullptr ||
      descend_motion == nullptr || retreat_motion == nullptr ||
      approach_motion->contact_policy_id.has_value() ||
      descend_motion->contact_policy_id.has_value() ||
      retreat_motion->contact_policy_id.has_value() ||
      !approach_motion->runtime_object_bindings.empty() ||
      !descend_motion->runtime_object_bindings.empty() ||
      !retreat_motion->runtime_object_bindings.empty()) {
    return common.ok()
               ? planning_error<PickPlacePlanningResultDTO>(
                     "place_public_phase_contract_invalid",
                     "place motion phases cannot carry contact-grasp or "
                     "runtime-object bindings")
               : Result<PickPlacePlanningResultDTO>::Err(common.error());
  }

  auto session =
      motion_.begin_motion_preview_session(intent.approach_operation);
  if (!session.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(session.error());
  }
  auto carrier = session.value()->resolve_binding(intent.tool_operation);
  if (!carrier.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(carrier.error());
  }
  if (find_frame(carrier.value(), intent.carrier_frame_role_id) == nullptr) {
    return planning_error<PickPlacePlanningResultDTO>(
        "place_carrier_frame_unresolved",
        "carrier frame role is absent from the exact tool binding",
        ErrorCode::kMissingReference);
  }

  const ProjectionLeaseId predecessor_lease("pick-place:" +
                                             intent.sequence_id +
                                             ":predecessor");
  auto predecessor =
      projection_.RetainProjection(current_projection, predecessor_lease);
  if (!predecessor.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(predecessor.error());
  }
  ProjectionLeaseGuard release{projection_, predecessor_lease};
  if (!exact_predecessor(predecessor.value(), current_projection,
                         predecessor_lease)) {
    return planning_error<PickPlacePlanningResultDTO>(
        "place_predecessor_projection_stale",
        "retained place projection differs from current scene truth",
        ErrorCode::kSceneChangedDuringValidation);
  }
  const auto* selected =
      find_object(*predecessor.value().logical_scene,
                  intent.selected_object_id);
  const auto* attachment =
      find_attachment(*predecessor.value().logical_scene,
                      intent.selected_object_id);
  if (selected == nullptr || attachment == nullptr ||
      !std::holds_alternative<CarriedSceneObjectPlacementDTO>(
          selected->placement) ||
      attachment->controllable_component_id !=
          carrier.value().controllable_component_id ||
      attachment->command_target_id != carrier.value().command_target_id) {
    return planning_error<PickPlacePlanningResultDTO>(
        "place_selected_attachment_unresolved",
        "place requires the exact object carried by the selected tool target",
        ErrorCode::kSceneObjectStateConflict);
  }
  auto state = session.value()->current_state();
  if (!state.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(state.error());
  }
  const TargetCarrierSceneFrameRefDTO carrier_selector{
      carrier.value().controllable_component_id,
      carrier.value().command_target_id, intent.carrier_frame_role_id};
  auto detach = detach_request(intent, current_projection, *attachment,
                               scene_.CurrentRequestOwnerEpoch());
  AddressedPlacePlanner planner(*session.value(), scene_candidates_,
                                projection_);
  auto outcome = planner.Plan(
      PlacePlanningRequestDTO{
          intent.selected_object_id, intent.approach_operation,
          intent.descend_operation, intent.retreat_operation,
          std::move(state.value()), std::move(predecessor.value()),
          ResolvedSceneCandidatePreviewRequestDTO{std::move(detach),
                                                  std::nullopt},
          carrier_selector, "pick-place:" + intent.sequence_id + ":candidate"},
      stop_token);
  if (!outcome.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(outcome.error());
  }
  PickPlacePlanningResultDTO result{
      PickPlacePlanningStatusDTO::kNoFeasibleCandidate,
      intent.selected_object_id, 1U, {std::move(outcome.value().report)},
      std::nullopt};
  if (!outcome.value().selected.has_value()) {
    return Result<PickPlacePlanningResultDTO>::Ok(std::move(result));
  }
  auto plan = make_place_execution_plan(*outcome.value().selected,
                                        intent.selected_object_id);
  if (!plan.ok()) {
    return Result<PickPlacePlanningResultDTO>::Err(plan.error());
  }
  result.status = PickPlacePlanningStatusDTO::kPlanned;
  result.sequence_request = PickPlaceSequenceRequestDTO{
      intent.sequence_id, std::move(plan.value()),
      std::move(intent.approach_operation),
      std::move(intent.descend_operation), std::move(intent.tool_operation),
      std::move(intent.retreat_operation)};
  return Result<PickPlacePlanningResultDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
