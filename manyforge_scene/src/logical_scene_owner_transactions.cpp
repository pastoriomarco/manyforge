#include "logical_scene_owner_impl.hpp"
#include <manyforge_scene/scene_candidate_preview.hpp>

namespace manyforge::scene::logical_scene_owner_detail {

ErrorDTO SceneError(const std::string_view code, const std::string_view message,
                    std::map<std::string, std::string> details) {
  return {std::string(code), std::string(message), std::move(details)};
}

bool ExpectedProjectionAdmitsActive(const SceneTransactionRequestDTO &request,
                                    const ProjectionRevisionDTO &active) {
  const auto &expected = request.expected_projection_revision;
  return core::SameActiveDeployment(active, expected) &&
         active.scene_revision.scene_id == expected.scene_revision.scene_id;
}

SceneCommitResultDTO
FailedSceneResult(const RequestKey &request_key,
                  const core::RequestReplayStatusDTO replay, ErrorDTO error,
                  const SceneProjectionCommitStatusDTO projection_status) {
  SceneCommitResultDTO result{
      request_key,       replay,       false,
      std::move(error),  std::nullopt, std::nullopt,
      projection_status, std::nullopt,
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "cannot construct a valid failed scene result");
  }
  return result;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

void ValidateBoundedText(const std::string_view value,
                         const std::string_view label) {
  if (value.empty() ||
      value.size() > kMaximumSceneStoreIdentifierBytes ||
      !manyforge::content_identity::is_utf8_nfc(value)) {
    throw std::invalid_argument(std::string(label) +
                                " must be bounded NFC text");
  }
}

// The admission call did not reach the Kernel: refused by the Manager's
// execution-host channel. The Kernel itself answers none of these codes.
[[nodiscard]] bool AdmissionOwnerUnreachable(const std::string_view code) {
  return code == "kernel_owner_unavailable" ||
         code == "runtime_unit_launch_in_progress" ||
         code == "execution_host_late" || code == "execution_host_exited" ||
         code == "application_stopping";
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

bool SamePose(const core::Pose3DTO &left,
              const core::Pose3DTO &right) noexcept {
  return left == right;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

struct ResolvedSceneCandidate final {
  LogicalSceneSnapshotDTO snapshot;
  core::ResolvedSceneOperationDTO operation;
  SceneUpdateStatusDTO update_status;
  std::optional<ContentDigest> resolution_evidence_digest;
};

ContentDigest CellResolutionEvidence(const ContentDigest &request_digest,
                                     const ProjectionRevisionDTO &projection) {
  return ContentDigest(manyforge::content_identity::sha256_digest(
      "manyforge.scene-frame-resolution.cell.v1\n" + request_digest.value() +
      "\n" + projection.retained_bundle_id.value() + "\n" +
      projection.graph_revision.value() + "\n" +
      projection.scene_revision.content_hash.value()));
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

const core::CellSceneFrameRefDTO &
RequireCellFrame(const core::SceneTransactionFrameRefDTO &frame,
                 const CellId &cell_id) {
  const auto *cell = std::get_if<core::CellSceneFrameRefDTO>(&frame);
  if (cell == nullptr) {
    throw LogicalSceneOwnerIntegrityError(
        "cell-frame normalizer received another frame-reference kind");
  }
  if (cell->cell_id != cell_id) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "scene frame does not name the active semantic cell",
        {{"reference_type", "cell_frame"},
         {"reference_id", cell->cell_id.value()}});
  }
  return *cell;
}

[[nodiscard]] bool
FrameReferenceMatchesResolved(const core::SceneTransactionFrameRefDTO &reference,
                              const core::ResolvedFrameDTO &resolved) noexcept {
  return std::visit(
      [&](const auto &selected) {
        using T = std::remove_cvref_t<decltype(selected)>;
        if constexpr (std::is_same_v<T, core::CellSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedCellFrameDTO>(&resolved);
          return frame != nullptr && frame->cell_id == selected.cell_id;
        } else if constexpr (std::is_same_v<T,
                                            core::ComponentSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedComponentModelFrameDTO>(&resolved);
          return frame != nullptr &&
                 frame->controllable_component_id ==
                     selected.controllable_component_id &&
                 frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        } else if constexpr (std::is_same_v<
                                 T, core::PassiveModelSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedPassiveModelFrameDTO>(&resolved);
          return frame != nullptr &&
                 frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        } else {
          return false;
        }
      },
      reference);
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] NormalizedScenePose
NormalizeScenePose(const core::SceneTransactionFrameRefDTO &frame_ref,
                   const core::Pose3DTO &pose_in_frame,
                   const LogicalSceneSnapshotDTO &current,
                   const ProjectionRevisionDTO &expected_projection,
                   const ContentDigest &request_digest,
                   const std::optional<core::SceneFrameResolutionReceiptDTO>
                       &frame_resolution) {
  if (std::holds_alternative<core::CellSceneFrameRefDTO>(frame_ref)) {
    static_cast<void>(RequireCellFrame(frame_ref, current.cell_id));
    if (frame_resolution.has_value()) {
      throw LogicalSceneOwnerIntegrityError(
          "cell-frame normalization unexpectedly carries native evidence");
    }
    return {pose_in_frame,
            CellResolutionEvidence(request_digest, expected_projection)};
  }
  if (const auto *scene_resource =
          std::get_if<core::SceneResourceSceneFrameRefDTO>(&frame_ref)) {
    ValidateBoundedIdentity(scene_resource->scene_resource_id.value(),
                            "scene_resource_id");
    const auto reference =
        std::lower_bound(current.objects.begin(), current.objects.end(),
                         scene_resource->scene_resource_id,
                         [](const SceneObjectSnapshotDTO &object,
                            const SceneResourceId &id) {
                           return object.scene_resource_id < id;
                         });
    if (reference == current.objects.end() ||
        reference->scene_resource_id != scene_resource->scene_resource_id) {
      throw SceneRequestRejectionError(
          "missing_reference", "scene-resource frame does not exist",
          {{"reference_type", "scene_resource"},
           {"reference_id", scene_resource->scene_resource_id.value()}});
    }
    const auto *fixed =
        std::get_if<core::CellFixedSceneObjectPlacementDTO>(
            &reference->placement);
    if (fixed != nullptr) {
      if (frame_resolution.has_value()) {
        throw LogicalSceneOwnerIntegrityError(
            "fixed scene-resource normalization unexpectedly carries native "
            "evidence");
      }
      return {core::multiply_poses(fixed->pose_in_cell, pose_in_frame),
              CellResolutionEvidence(request_digest, expected_projection)};
    }
    const auto &carried =
        std::get<core::CarriedSceneObjectPlacementDTO>(reference->placement);
    const auto attachment = std::lower_bound(
        current.carried_object_attachments.begin(),
        current.carried_object_attachments.end(),
        carried.carried_object_attachment_id,
        [](const core::CarriedObjectAttachmentDTO &entry,
           const core::CarriedObjectAttachmentId &id) {
          return entry.carried_object_attachment_id < id;
        });
    if (attachment == current.carried_object_attachments.end() ||
        attachment->carried_object_attachment_id !=
            carried.carried_object_attachment_id ||
        attachment->carried_object_attachment_revision !=
            carried.carried_object_attachment_revision) {
      throw LogicalSceneOwnerIntegrityError(
          "carried scene-resource frame has no exact attachment row");
    }
    if (!frame_resolution.has_value() || !frame_resolution->valid() ||
        frame_resolution->projection_binding.projection_revision !=
            expected_projection ||
        !FrameReferenceMatchesResolved(
            core::ToSceneTransactionFrameRef(
                attachment->carrier_frame_ref),
            frame_resolution->resolved_frame)) {
      throw SceneRequestRejectionError(
          "missing_reference",
          "carried scene-resource frame has no exact immutable retained "
          "resolution",
          {{"reason", "carried_scene_resource_resolution_missing_or_foreign"},
           {"reference_id", scene_resource->scene_resource_id.value()}});
    }
    const auto resource_pose_in_cell = core::multiply_poses(
        frame_resolution->frame_pose_in_cell,
        attachment->pose_in_carrier_frame);
    return {core::multiply_poses(resource_pose_in_cell, pose_in_frame),
            frame_resolution->resolution_evidence_digest};
  }
  if (!frame_resolution.has_value() || !frame_resolution->valid() ||
      frame_resolution->projection_binding.projection_revision !=
          expected_projection ||
      !FrameReferenceMatchesResolved(frame_ref,
                                     frame_resolution->resolved_frame)) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "scene frame has no exact immutable retained resolution",
        {{"reason", "scene_frame_resolution_missing_or_foreign"}});
  }
  return {
      core::multiply_poses(frame_resolution->frame_pose_in_cell, pose_in_frame),
      frame_resolution->resolution_evidence_digest};
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

std::string CurrentFrameId(const LogicalSceneSnapshotDTO &snapshot,
                           const SceneObjectSnapshotDTO &object) {
  if (const auto *cell = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
          &object.placement)) {
    return cell->cell_id.value();
  }
  const auto &carried =
      std::get<core::CarriedSceneObjectPlacementDTO>(object.placement);
  const auto attachment =
      std::lower_bound(snapshot.carried_object_attachments.begin(),
                       snapshot.carried_object_attachments.end(),
                       carried.carried_object_attachment_id,
                       [](const core::CarriedObjectAttachmentDTO &entry,
                          const core::CarriedObjectAttachmentId &id) {
                         return entry.carried_object_attachment_id < id;
                       });
  if (attachment == snapshot.carried_object_attachments.end() ||
      attachment->carried_object_attachment_id !=
          carried.carried_object_attachment_id ||
      attachment->carried_object_attachment_revision !=
          carried.carried_object_attachment_revision) {
    throw LogicalSceneOwnerIntegrityError(
        "carried scene object has no exact attachment row");
  }
  const auto *frame = std::get_if<core::ComponentSceneFrameRefDTO>(
      &attachment->carrier_frame_ref);
  if (frame == nullptr) {
    throw LogicalSceneOwnerIntegrityError(
        "carried scene object attachment has no component frame");
  }
  return frame->frame_role_id.value();
}

ResolvedSceneCandidate
ResolveSceneCandidate(const SceneTransactionRequestDTO &request,
                      const LogicalSceneSnapshotDTO &current,
                      const std::uint64_t transaction_sequence,
                      const ContentDigest &request_digest,
                      const std::optional<core::SceneFrameResolutionReceiptDTO>
                          &frame_resolution) {
  ResolvedSceneCandidate candidate{
      current,
      core::ResolvedRemoveSceneObjectDTO{SceneResourceId(std::string{})},
      SceneUpdateStatusDTO::kGeometryChanged,
      std::nullopt,
  };
  candidate.snapshot.transaction_sequence = transaction_sequence;
  bool content_expected_to_change = true;
  bool topology_changed = true;
  bool policy_changed = false;

  std::visit(
      [&](const auto &operation) {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, core::AddObjectRequestDTO> ||
                      std::is_same_v<T, core::UpsertObjectRequestDTO>) {
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          // The Manager fills the captured mesh content before the owner sees
          // the request; an empty digest means it did not.
          if (const auto *mesh =
                  std::get_if<core::MeshShapeDTO>(&operation.shape);
              mesh != nullptr && mesh->content_digest.empty()) {
            throw SceneRequestRejectionError(
                "scene_mesh_unavailable",
                "scene mesh " + mesh->resource_uri + " was not captured",
                {{"resource_uri", mesh->resource_uri},
                 {"scene_resource_id", operation.scene_resource_id.value()}});
          }
          const auto normalized =
              NormalizeScenePose(operation.frame_ref, operation.pose, current,
                                 request.expected_projection_revision,
                                 request_digest, frame_resolution);
          const auto found = std::lower_bound(
              candidate.snapshot.objects.begin(),
              candidate.snapshot.objects.end(), operation.scene_resource_id,
              [](const SceneObjectSnapshotDTO &object,
                 const SceneResourceId &id) {
                return object.scene_resource_id < id;
              });
          const bool exists =
              found != candidate.snapshot.objects.end() &&
              found->scene_resource_id == operation.scene_resource_id;
          if constexpr (std::is_same_v<T, core::AddObjectRequestDTO>) {
            if (exists) {
              throw SceneRequestRejectionError(
                  "object_already_exists", "scene resource already exists",
                  {{"scene_resource_id", operation.scene_resource_id.value()}});
            }
          } else if (exists && std::holds_alternative<
                                   core::CarriedSceneObjectPlacementDTO>(
                                   found->placement)) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "a carried object cannot be replaced",
                {{"reason", "scene_object_is_attached"},
                 {"scene_resource_id", operation.scene_resource_id.value()}});
          }
          SceneObjectSnapshotDTO object{
              operation.scene_resource_id,
              {operation.shape},
              core::CellFixedSceneObjectPlacementDTO{current.cell_id,
                                                     normalized.pose_in_cell},
              std::nullopt,
              operation.mass_kg,
          };
          const bool unchanged = exists && *found == object;
          if (exists) {
            *found = object;
          } else {
            candidate.snapshot.objects.insert(found, object);
          }
          candidate.operation = core::ResolvedUpsertSceneObjectDTO{object};
          candidate.resolution_evidence_digest =
              normalized.resolution_evidence_digest;
          if (unchanged) {
            candidate.update_status = SceneUpdateStatusDTO::kNoOp;
            content_expected_to_change = false;
            topology_changed = false;
          }
        } else if constexpr (std::is_same_v<T, core::RemoveObjectRequestDTO>) {
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          const auto found = std::lower_bound(
              candidate.snapshot.objects.begin(),
              candidate.snapshot.objects.end(), operation.scene_resource_id,
              [](const SceneObjectSnapshotDTO &object,
                 const SceneResourceId &id) {
                return object.scene_resource_id < id;
              });
          if (found == candidate.snapshot.objects.end() ||
              found->scene_resource_id != operation.scene_resource_id) {
            throw SceneRequestRejectionError(
                "object_not_found", "scene resource does not exist",
                {{"scene_resource_id", operation.scene_resource_id.value()}});
          }
          if (std::holds_alternative<core::CarriedSceneObjectPlacementDTO>(
                  found->placement)) {
            throw LogicalSceneOwnerConflictError(
                "a carried object must be detached before removal");
          }
          candidate.snapshot.objects.erase(found);
          candidate.operation =
              core::ResolvedRemoveSceneObjectDTO{operation.scene_resource_id};
        } else if constexpr (std::is_same_v<T,
                                            core::UpdateObjectPoseRequestDTO>) {
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          const auto normalized = NormalizeScenePose(
              operation.frame_ref, operation.pose, current,
              request.expected_projection_revision, request_digest,
              std::holds_alternative<core::CellSceneFrameRefDTO>(
                  operation.frame_ref)
                  ? std::nullopt
                  : frame_resolution);
          auto found = std::lower_bound(candidate.snapshot.objects.begin(),
                                        candidate.snapshot.objects.end(),
                                        operation.scene_resource_id,
                                        [](const SceneObjectSnapshotDTO &object,
                                           const SceneResourceId &id) {
                                          return object.scene_resource_id < id;
                                        });
          if (found == candidate.snapshot.objects.end() ||
              found->scene_resource_id != operation.scene_resource_id) {
            throw SceneRequestRejectionError(
                "object_not_found", "scene resource does not exist",
                {{"scene_resource_id", operation.scene_resource_id.value()}});
          }
          const bool attached =
              std::holds_alternative<core::CarriedSceneObjectPlacementDTO>(
                  found->placement);
          if (operation.expected_attached.has_value() &&
              *operation.expected_attached != attached) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "scene object attachment precondition failed",
                {{"reason", "scene_object_attachment_precondition_failed"},
                 {"scene_resource_id", operation.scene_resource_id.value()},
                 {"expected_attached",
                  *operation.expected_attached ? "true" : "false"},
                 {"current_attached", attached ? "true" : "false"},
                 {"current_frame_id", CurrentFrameId(current, *found)}});
          }
          if (attached) {
            if (!operation.provider_evidence)
              throw SceneRequestRejectionError(
                  "scene_object_state_conflict",
                  "a carried object pose is updated only by its runtime "
                  "provider",
                  {{"reason", "scene_object_is_attached"}});
            const auto &placement =
                std::get<core::CarriedSceneObjectPlacementDTO>(
                    found->placement);
            auto attachment = std::lower_bound(
                candidate.snapshot.carried_object_attachments.begin(),
                candidate.snapshot.carried_object_attachments.end(),
                placement.carried_object_attachment_id,
                [](const auto &entry, const auto &id) {
                  return entry.carried_object_attachment_id < id;
                });
            if (attachment ==
                    candidate.snapshot.carried_object_attachments.end() ||
                attachment->carried_object_attachment_id !=
                    placement.carried_object_attachment_id ||
                attachment->carried_object_attachment_revision !=
                    placement.carried_object_attachment_revision)
              throw LogicalSceneOwnerIntegrityError(
                  "attached pose update has no exact attachment");
            // A pose in the attachment's own carrier frame is the carried
            // offset itself; a cell-frame pose is expressed on the carrier.
            const bool carrier_frame_pose =
                operation.frame_ref ==
                core::ToSceneTransactionFrameRef(attachment->carrier_frame_ref);
            if ((!carrier_frame_pose &&
                 !std::holds_alternative<core::CellSceneFrameRefDTO>(
                     operation.frame_ref)) ||
                !frame_resolution || !frame_resolution->valid())
              throw SceneRequestRejectionError(
                  "missing_reference",
                  "attached pose update requires a cell-frame or carrier-frame "
                  "pose and exact carrier resolution");
            const auto next_pose =
                carrier_frame_pose
                    ? operation.pose
                    : core::multiply_poses(
                          core::inverse_pose(
                              frame_resolution->frame_pose_in_cell),
                          normalized.pose_in_cell);
            const bool pose_changed =
                !SamePose(attachment->pose_in_carrier_frame, next_pose);
            const bool evidence_changed =
                found->provider_evidence != operation.provider_evidence;
            attachment->pose_in_carrier_frame = next_pose;
            attachment->carried_object_attachment_revision =
                core::logical_scene_identity::
                    ComputeCarriedObjectAttachmentRevision(*attachment);
            found->placement = core::CarriedSceneObjectPlacementDTO{
                attachment->carried_object_attachment_id,
                attachment->carried_object_attachment_revision};
            found->provider_evidence = operation.provider_evidence;
            candidate.operation =
                core::ResolvedUpsertSceneObjectDTO{*found, *attachment};
            candidate.resolution_evidence_digest =
                frame_resolution->resolution_evidence_digest;
            if (!pose_changed) {
              topology_changed = false;
              candidate.update_status =
                  evidence_changed ? SceneUpdateStatusDTO::kFreshnessOnly
                                   : SceneUpdateStatusDTO::kNoOp;
              content_expected_to_change = evidence_changed;
            }
            return;
          }
          const auto previous_pose =
              std::get<core::CellFixedSceneObjectPlacementDTO>(found->placement)
                  .pose_in_cell;
          const bool pose_changed =
              !SamePose(previous_pose, normalized.pose_in_cell);
          const bool evidence_changed =
              found->provider_evidence != operation.provider_evidence;
          found->placement = core::CellFixedSceneObjectPlacementDTO{
              current.cell_id, normalized.pose_in_cell};
          found->provider_evidence = operation.provider_evidence;
          candidate.operation = core::ResolvedUpsertSceneObjectDTO{*found};
          candidate.resolution_evidence_digest =
              normalized.resolution_evidence_digest;
          if (!pose_changed && !evidence_changed) {
            candidate.update_status = SceneUpdateStatusDTO::kNoOp;
            content_expected_to_change = false;
            topology_changed = false;
          } else if (!pose_changed) {
            candidate.update_status = SceneUpdateStatusDTO::kFreshnessOnly;
            topology_changed = false;
          }
        } else if constexpr (std::is_same_v<
                                 T, core::AttachCarriedObjectRequestDTO>) {
          ValidateBoundedIdentity(
              operation.carried_object_attachment_id.value(),
              "carried_object_attachment_id");
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          if (!frame_resolution.has_value() || !frame_resolution->valid() ||
              frame_resolution->projection_binding.projection_revision !=
                  request.expected_projection_revision) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "carried-object attach has no exact carrier-frame resolution",
                {{"reason", "carrier_frame_resolution_missing"}});
          }
          const auto *carrier =
              std::get_if<core::ResolvedComponentModelFrameDTO>(
                  &frame_resolution->resolved_frame);
          if (carrier == nullptr ||
              carrier->controllable_component_id !=
                  operation.carrier_component_id.value_or(
                      operation.controllable_component_id)) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "carrier target resolved to another component",
                {{"reason", "carrier_frame_resolution_foreign"}});
          }
          // The carrier frame resolves against the current structural
          // attachments; expected_component_attachment_revisions is ignored.
          auto touch_roles = operation.touch_body_roles;
          if (std::any_of(touch_roles.begin(), touch_roles.end(),
                          [](const auto &role) { return role.empty(); }) ||
              !std::is_sorted(touch_roles.begin(), touch_roles.end()) ||
              std::adjacent_find(touch_roles.begin(), touch_roles.end()) !=
                  touch_roles.end()) {
            throw std::invalid_argument(
                "touch_body_roles must be sorted, unique, and non-empty "
                "when present");
          }
          const auto duplicate_attachment = std::lower_bound(
              candidate.snapshot.carried_object_attachments.begin(),
              candidate.snapshot.carried_object_attachments.end(),
              operation.carried_object_attachment_id,
              [](const core::CarriedObjectAttachmentDTO &entry,
                 const core::CarriedObjectAttachmentId &id) {
                return entry.carried_object_attachment_id < id;
              });
          if (duplicate_attachment !=
                  candidate.snapshot.carried_object_attachments.end() &&
              duplicate_attachment->carried_object_attachment_id ==
                  operation.carried_object_attachment_id) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "carried-object attachment ID already exists",
                {{"reason", "carried_attachment_already_exists"}});
          }
          auto object = std::lower_bound(candidate.snapshot.objects.begin(),
                                         candidate.snapshot.objects.end(),
                                         operation.scene_resource_id,
                                         [](const SceneObjectSnapshotDTO &entry,
                                            const SceneResourceId &id) {
                                           return entry.scene_resource_id < id;
                                         });
          if (object == candidate.snapshot.objects.end() ||
              object->scene_resource_id != operation.scene_resource_id) {
            throw SceneRequestRejectionError(
                "object_not_found", "scene resource does not exist",
                {{"scene_resource_id", operation.scene_resource_id.value()}});
          }
          const auto *fixed =
              std::get_if<core::CellFixedSceneObjectPlacementDTO>(
                  &object->placement);
          if (fixed == nullptr) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "scene resource is already carried",
                {{"reason", "scene_object_is_attached"},
                 {"scene_resource_id", operation.scene_resource_id.value()}});
          }
          if (operation.grasp_lost_bound &&
              !operation.grasp_lost_bound->valid()) {
            throw std::invalid_argument(
                "grasp_lost_bound must be finite and positive");
          }
          // The defaults are kept as absent: one form per attachment.
          const auto on_grasp_lost =
              operation.on_grasp_lost && !operation.on_grasp_lost->is_default()
                  ? operation.on_grasp_lost
                  : std::nullopt;
          if (on_grasp_lost && !on_grasp_lost->valid()) {
            throw std::invalid_argument(
                "on_grasp_lost raise_alarm requires stop_motion and set_key "
                "must be a non-empty key name");
          }
          if (!operation.grasp_lost_bound && on_grasp_lost) {
            throw std::invalid_argument(
                "on_grasp_lost requires grasp_lost_bound");
          }
          const auto pose_in_carrier = core::multiply_poses(
              core::inverse_pose(frame_resolution->frame_pose_in_cell),
              fixed->pose_in_cell);
          core::CarriedObjectAttachmentDTO attachment{
              operation.carried_object_attachment_id,
              core::CarriedObjectAttachmentRevision(std::string{}),
              operation.scene_resource_id,
              operation.controllable_component_id,
              operation.command_target_id,
              core::ComponentSceneFrameRefDTO{
                  carrier->controllable_component_id,
                  carrier->model_instance_id, carrier->frame_role_id},
              pose_in_carrier,
              std::move(touch_roles),
              operation.grasp_lost_bound
                  ? std::optional(core::CarriedObjectGraspLostCheckDTO{
                        *operation.grasp_lost_bound, on_grasp_lost})
                  : std::nullopt};
          attachment.carried_object_attachment_revision =
              core::logical_scene_identity::
                  ComputeCarriedObjectAttachmentRevision(attachment);
          object->placement = core::CarriedSceneObjectPlacementDTO{
              attachment.carried_object_attachment_id,
              attachment.carried_object_attachment_revision};
          object->provider_evidence = std::nullopt;
          candidate.snapshot.carried_object_attachments.insert(
              duplicate_attachment, attachment);
          candidate.operation = core::ResolvedAttachCarriedObjectDTO{
              *object, std::move(attachment)};
          candidate.resolution_evidence_digest =
              frame_resolution->resolution_evidence_digest;
          policy_changed = true;
        } else if constexpr (std::is_same_v<
                                 T, core::DetachCarriedObjectRequestDTO>) {
          const auto normalized =
              NormalizeScenePose(operation.frame_ref, operation.pose, current,
                                 request.expected_projection_revision,
                                 request_digest, frame_resolution);
          const auto attachment = std::lower_bound(
              candidate.snapshot.carried_object_attachments.begin(),
              candidate.snapshot.carried_object_attachments.end(),
              operation.carried_object_attachment_id,
              [](const core::CarriedObjectAttachmentDTO &entry,
                 const core::CarriedObjectAttachmentId &id) {
                return entry.carried_object_attachment_id < id;
              });
          // As for detach at the current pose: the attachment id identifies
          // the attachment; its revision may have advanced with the carried
          // offset since the caller read it.
          if (attachment ==
                  candidate.snapshot.carried_object_attachments.end() ||
              attachment->carried_object_attachment_id !=
                  operation.carried_object_attachment_id) {
            throw LogicalSceneOwnerConflictError(
                "carried-object attachment precondition failed");
          }
          const auto object = std::lower_bound(
              candidate.snapshot.objects.begin(),
              candidate.snapshot.objects.end(), attachment->scene_resource_id,
              [](const SceneObjectSnapshotDTO &entry,
                 const SceneResourceId &id) {
                return entry.scene_resource_id < id;
              });
          if (object == candidate.snapshot.objects.end() ||
              object->scene_resource_id != attachment->scene_resource_id) {
            throw LogicalSceneOwnerIntegrityError(
                "carried attachment has no matching scene object");
          }
          const auto removed_id = attachment->carried_object_attachment_id;
          const auto removed_revision =
              attachment->carried_object_attachment_revision;
          object->placement = core::CellFixedSceneObjectPlacementDTO{
              current.cell_id, normalized.pose_in_cell};
          candidate.snapshot.carried_object_attachments.erase(attachment);
          candidate.operation = core::ResolvedDetachCarriedObjectDTO{
              *object, removed_id, removed_revision};
          candidate.resolution_evidence_digest =
              normalized.resolution_evidence_digest;
          policy_changed = true;
        } else if constexpr (
            std::is_same_v<T,
                           core::DetachCarriedObjectAtCurrentPoseRequestDTO>) {
          ValidateBoundedIdentity(
              operation.carried_object_attachment_id.value(),
              "carried_object_attachment_id");
          ValidateBoundedIdentity(operation.expected_scene_resource_id.value(),
                                  "expected_scene_resource_id");
          ValidateBoundedIdentity(
              operation.expected_controllable_component_id.value(),
              "expected_controllable_component_id");
          ValidateBoundedIdentity(operation.expected_command_target_id.value(),
                                  "expected_command_target_id");
          const auto attachment = std::lower_bound(
              candidate.snapshot.carried_object_attachments.begin(),
              candidate.snapshot.carried_object_attachments.end(),
              operation.carried_object_attachment_id,
              [](const core::CarriedObjectAttachmentDTO &entry,
                 const core::CarriedObjectAttachmentId &id) {
                return entry.carried_object_attachment_id < id;
              });
          // The attachment is identified by its id, object and carrier. Its
          // revision also covers the carried offset, which a continuous pose
          // source may advance between the caller's read and this commit, so
          // the detach applies to the latest offset rather than refusing.
          if (attachment ==
                  candidate.snapshot.carried_object_attachments.end() ||
              attachment->carried_object_attachment_id !=
                  operation.carried_object_attachment_id ||
              attachment->scene_resource_id !=
                  operation.expected_scene_resource_id ||
              attachment->controllable_component_id !=
                  operation.expected_controllable_component_id ||
              attachment->command_target_id !=
                  operation.expected_command_target_id) {
            throw LogicalSceneOwnerConflictError(
                "carried-object attachment precondition failed");
          }
          if (!frame_resolution.has_value() || !frame_resolution->valid() ||
              frame_resolution->projection_binding.projection_revision !=
                  request.expected_projection_revision) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "current carried-object pose has no exact carrier-frame "
                "resolution",
                {{"reason", "carrier_frame_resolution_missing"}});
          }
          const auto *expected_frame =
              std::get_if<core::ComponentSceneFrameRefDTO>(
                  &attachment->carrier_frame_ref);
          const auto *resolved_frame =
              std::get_if<core::ResolvedComponentModelFrameDTO>(
                  &frame_resolution->resolved_frame);
          if (expected_frame == nullptr || resolved_frame == nullptr ||
              resolved_frame->controllable_component_id !=
                  expected_frame->controllable_component_id ||
              resolved_frame->model_instance_id !=
                  expected_frame->model_instance_id ||
              resolved_frame->frame_role_id != expected_frame->frame_role_id) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "current carrier frame resolved to another component, model, "
                "or frame role",
                {{"reason", "carrier_frame_resolution_foreign"}});
          }
          auto object = std::lower_bound(candidate.snapshot.objects.begin(),
                                         candidate.snapshot.objects.end(),
                                         attachment->scene_resource_id,
                                         [](const SceneObjectSnapshotDTO &entry,
                                            const SceneResourceId &id) {
                                           return entry.scene_resource_id < id;
                                         });
          if (object == candidate.snapshot.objects.end() ||
              object->scene_resource_id != attachment->scene_resource_id) {
            throw LogicalSceneOwnerIntegrityError(
                "carried attachment has no matching scene object");
          }
          const auto *carried =
              std::get_if<core::CarriedSceneObjectPlacementDTO>(
                  &object->placement);
          if (carried == nullptr ||
              carried->carried_object_attachment_id !=
                  attachment->carried_object_attachment_id ||
              carried->carried_object_attachment_revision !=
                  attachment->carried_object_attachment_revision) {
            throw LogicalSceneOwnerIntegrityError(
                "carried scene object does not identify its attachment");
          }
          const auto removed_id = attachment->carried_object_attachment_id;
          const auto removed_revision =
              attachment->carried_object_attachment_revision;
          object->placement = core::CellFixedSceneObjectPlacementDTO{
              current.cell_id,
              core::multiply_poses(frame_resolution->frame_pose_in_cell,
                                   attachment->pose_in_carrier_frame)};
          candidate.snapshot.carried_object_attachments.erase(attachment);
          candidate.operation = core::ResolvedDetachCarriedObjectDTO{
              *object, removed_id, removed_revision};
          candidate.resolution_evidence_digest =
              frame_resolution->resolution_evidence_digest;
          policy_changed = true;
        } else {
          topology_changed = false;
          // Last arrival wins: expected_configured_policy_revision is
          // ignored and the edit replaces the current configured policy.
          const auto &current_policy = current.configured_collision_policy;
          if (!operation.configured_policy.static_body_ids.empty() ||
              !operation.configured_policy.allowed_touch_link_pairs.empty() ||
              !operation.configured_policy.contact_rules.empty()) {
            throw SceneRequestRejectionError(
                "invalid_request",
                "configured collision policy cannot contain owner-derived "
                "static bodies or touch allowances, or collision rules of a "
                "move",
                {{"reason", "derived_collision_policy_fields_are_read_only"}});
          }
          auto next_policy = operation.configured_policy;
          const auto digest =
              core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
                  next_policy);
          if (digest.value() == current_policy.revision.content_hash) {
            next_policy.revision = current_policy.revision;
            candidate.update_status = SceneUpdateStatusDTO::kNoOp;
            content_expected_to_change = false;
          } else {
            if (current_policy.revision.revision_id >= kMaximumSafeIntegral) {
              throw LogicalSceneOwnerLimitError(
                  "collision-policy revision is exhausted");
            }
            next_policy.revision = {
                current_policy.revision.revision_id + 1U,
                digest.value(),
            };
            policy_changed = true;
          }
          candidate.snapshot.configured_collision_policy = next_policy;
          candidate.operation = core::ResolvedCollisionPolicyDTO{
              next_policy,
              std::move(next_policy),
          };
        }
      },
      request.operation);

  if (content_expected_to_change) {
    if (candidate.snapshot.revision.revision_id >= kMaximumSafeIntegral) {
      throw LogicalSceneOwnerLimitError(
          "logical-scene revision is exhausted");
    }
    candidate.snapshot.revision.revision_id += 1U;
    if (topology_changed) {
      if (candidate.snapshot.topology_epoch >= kMaximumSafeIntegral) {
        throw LogicalSceneOwnerLimitError(
            "logical-scene topology epoch is exhausted");
      }
      candidate.snapshot.topology_epoch += 1U;
    }
    if (policy_changed) {
      if (candidate.snapshot.policy_epoch >= kMaximumSafeIntegral) {
        throw LogicalSceneOwnerLimitError(
            "logical-scene policy epoch is exhausted");
      }
      candidate.snapshot.policy_epoch += 1U;
    }
    candidate.snapshot.revision.content_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(
            candidate.snapshot);
  }
  if (!candidate.snapshot.locally_valid()) {
    throw std::invalid_argument(
        "scene operation does not form a valid logical snapshot");
  }
  return candidate;
}

// The intent keeps the delta and, in its proposed revision, the candidate's
// content hash.
void InsertSceneIntent(SceneStoreMemory &memory,
                       const SceneTransactionRequestDTO &request,
                       const ContentDigest &request_digest,
                       std::string request_json,
                       const core::SceneTransactionDTO &transaction,
                       const ResolvedSceneCandidate &candidate,
                       const std::size_t required_projection_count) {
  memory.transactions.push_back(SceneTransactionRecord{
      transaction.transaction_sequence,
      request.request_context.request_key,
      request_digest,
      std::move(request_json),
      transaction.prior_projection_revision,
      transaction.proposed_projection_revision,
      transaction.resolved_operation,
      candidate.update_status,
      required_projection_count,
      0U,
      {},
  });
}

} // namespace

// Compaction is transparent to callers: it prunes the resident ledger behind
// a checkpoint of the active head, so neither the triggering request nor any
// other caller has to refresh. A commit compacts the ledger when it is full.
// A maintenance fold runs the same compaction before the bounded maintenance
// history evicts a completion the replay would otherwise still consume; it
// needs only a settled head, not a ledger or an open maintenance window.
PreparedSceneHistoryCompaction PrepareSceneHistoryCompaction(
    const SceneStoreMemory &memory, const DurableState &durable_state,
    const SceneTransactionStoreState &transactions,
    const bool maintenance_fold) {
  const auto access = AccessStatus(durable_state);
  if ((!maintenance_fold &&
       (!access.scene_writes_open || !access.projection_leases_open ||
        transactions.resident_transaction_count == 0U ||
        transactions.next_transaction_sequence <= 1U ||
        transactions.next_owner_event_sequence <= 1U)) ||
      durable_state.projection_desynchronized ||
      durable_state.unresolved_scene_transaction ||
      transactions.projection_desynchronized ||
      transactions.unresolved_native_attempt) {
    throw LogicalSceneOwnerConflictError(
        "logical-scene history compaction requires a settled quiescent head");
  }
  const auto &history = memory.history;
  if (history.compaction_generation >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene history compaction generation is exhausted");
  }
  const auto next_compaction_generation = history.compaction_generation + 1U;
  static_cast<void>(
      CheckedSafeCounter(next_compaction_generation, "compaction generation"));
  static_cast<void>(CheckedSafeCounter(
      transactions.active_projection.scene_revision.revision_id,
      "checkpoint scene revision"));
  auto next = history;
  next.compaction_generation = next_compaction_generation;
  // A maintenance fold may run before any Scene transaction (watermark 0).
  next.compacted_through_transaction_sequence =
      transactions.next_transaction_sequence - 1U;
  next.compacted_through_owner_event_sequence =
      transactions.next_owner_event_sequence - 1U;
  next.compacted_maintenance_completion_count =
      static_cast<std::size_t>(std::ranges::count_if(
          durable_state.maintenance_initializations,
          [](const MaintenanceInitializationState &initialization) {
            return initialization.publication_receipt.has_value();
          }));
  next.checkpoint_projection = transactions.active_projection;
  next.checkpoint_snapshot = transactions.active_snapshot;
  return {std::move(next)};
}

void CompactSceneTransactionHistory(
    SceneStoreMemory &memory, PreparedSceneHistoryCompaction compaction,
    const DurableState &durable_state,
    const SceneTransactionStoreState &transactions) {
  memory.history = std::move(compaction.history);
  memory.transactions.clear();
  memory.outcomes.clear();
  memory.owner_events.clear();
  memory.settled_head.reset();

  const auto compacted =
      ReadSceneTransactionStoreState(memory, durable_state);
  if (compacted.active_projection != transactions.active_projection ||
      compacted.active_snapshot != transactions.active_snapshot ||
      compacted.next_transaction_sequence !=
          transactions.next_transaction_sequence ||
      compacted.next_owner_event_sequence !=
          transactions.next_owner_event_sequence ||
      compacted.resident_transaction_count != 0U) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene history compaction changed authoritative state");
  }
}

namespace {

void InsertSceneTransactionOwnerEvent(
    SceneStoreMemory &memory, const std::uint64_t owner_event_sequence,
    const std::uint64_t transaction_sequence) {
  memory.owner_events.push_back(
      SceneOwnerEventRecord{owner_event_sequence, transaction_sequence});
}

// The attempt and result rows belong to the newest resident transaction:
// from its intent to its outcome a Commit is the only writer. The Apply-lane
// writers wait until it ends; every other write is refused by its unresolved
// intent (OD-376).
void InsertProjectionAttempt(SceneStoreMemory &memory) {
  ++memory.transactions.back().projection_attempt_count;
}

void InsertProjectionResult(SceneStoreMemory &memory,
                            const core::ProjectionCommitResultDTO &result) {
  memory.transactions.back().projection_results.push_back(result.result);
}

// The provider binding and runtime-unit identity are durable scene identity.
// The runtime epoch is the incarnation of the native child process and is
// therefore allowed to change when Manager is restarted while an Apply is
// being recovered.  Keep this relaxation deliberately narrow: a different
// provider, provider revision, or runtime unit is still a different
// projection and must fail closed.
[[nodiscard]] bool SameExecutionHostBindingExceptEpoch(
    const core::ProjectionExecutionHostBindingDTO &lhs,
    const core::ProjectionExecutionHostBindingDTO &rhs) noexcept {
  return lhs.provider_binding_id == rhs.provider_binding_id &&
         lhs.provider_binding_revision == rhs.provider_binding_revision &&
         lhs.runtime_unit_id == rhs.runtime_unit_id &&
         !lhs.runtime_epoch.empty() && !rhs.runtime_epoch.empty();
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] std::shared_ptr<core::IPlannerSceneProjection>
ResolveProjectionClient(const core::ProjectionExecutionHostBindingDTO &binding,
                        const core::IPlannerSceneProjectionResolver *resolver) {
  if (resolver == nullptr) {
    throw LogicalSceneOwnerConflictError(
        "native projection requires an installed projection resolver");
  }
  core::Result<std::shared_ptr<core::IPlannerSceneProjection>> resolved =
      [&]() {
        try {
          return resolver->ResolveProjection(binding);
        } catch (const std::exception &error) {
          return core::Result<std::shared_ptr<core::IPlannerSceneProjection>>::
              Err(core::ErrorDTO{
                  "native_projection_resolution_failed", error.what(), {}});
        } catch (...) {
          return core::Result<std::shared_ptr<core::IPlannerSceneProjection>>::
              Err(core::ErrorDTO{
                  "native_projection_resolution_failed",
                  "projection resolver threw a non-standard exception",
                  {}});
        }
      }();
  if (!resolved.ok() || !resolved.value()) {
    const std::string detail =
        resolved.ok() ? "projection resolver returned a null client"
                      : resolved.error().message;
    throw LogicalSceneOwnerConflictError(
        "native projection is unavailable: " + detail);
  }
  return std::move(resolved.value());
}

void RequireProjectionHealth(
    const core::ProjectionHealthDTO &health,
    const core::ProjectionExecutionHostBindingDTO &binding) {
  if (!health.valid() || health.source_sequence == 0U) {
    throw LogicalSceneOwnerConflictError(
        "native projection returned malformed health");
  }
  if (health.execution_host_binding != binding &&
      !SameExecutionHostBindingExceptEpoch(health.execution_host_binding,
                                            binding)) {
    throw LogicalSceneOwnerConflictError(
        "resolved native projection differs from the selected host "
        "incarnation");
  }
}

[[nodiscard]] RequiredSceneProjection ResolveRequiredProjection(
    const core::ProjectionExecutionHostBindingDTO &binding,
    const core::IPlannerSceneProjectionResolver *resolver,
    const ProjectionRevisionDTO &selected_revision) {
  auto projection = ResolveProjectionClient(binding, resolver);
  auto health = [&]() {
    try {
      return projection->ProjectionHealthForRevision(selected_revision);
    } catch (const std::bad_alloc &) {
      throw;
    } catch (const std::exception &error) {
      throw LogicalSceneOwnerConflictError(
          "native projection is unavailable: " + std::string(error.what()));
    }
  }();
  RequireProjectionHealth(health, binding);
  return {std::move(projection), std::move(health), binding};
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

class ScopedProjectionLease final {
public:
  ScopedProjectionLease(
      std::shared_ptr<core::IPlannerSceneProjection> projection,
      core::ProjectionLeaseId lease_id)
      : projection_(std::move(projection)), lease_id_(std::move(lease_id)) {}

  ScopedProjectionLease(const ScopedProjectionLease &) = delete;
  ScopedProjectionLease &operator=(const ScopedProjectionLease &) = delete;
  ScopedProjectionLease(ScopedProjectionLease &&other) noexcept
      : projection_(std::move(other.projection_)),
        lease_id_(std::move(other.lease_id_)) {}
  ScopedProjectionLease &operator=(ScopedProjectionLease &&) = delete;

  void Release() {
    if (!projection_ || lease_id_.empty()) {
      return;
    }
    projection_->ReleaseProjection(lease_id_);
    projection_.reset();
    lease_id_ = core::ProjectionLeaseId(std::string{});
  }

  ~ScopedProjectionLease() {
    if (!projection_ || lease_id_.empty()) {
      return;
    }
    try {
      projection_->ReleaseProjection(lease_id_);
    } catch (...) {
      // Resolution already captured an immutable receipt. Release failure is
      // bounded native cleanup and must not forge a different scene result.
    }
  }

private:
  std::shared_ptr<core::IPlannerSceneProjection> projection_;
  core::ProjectionLeaseId lease_id_;
};

[[nodiscard]] std::optional<core::SceneFrameResolutionSelectorDTO>
FrameSelectorForRequest(const SceneTransactionRequestDTO &request,
                        const LogicalSceneSnapshotDTO &current) {
  return std::visit(
      [&current](const auto &operation)
          -> std::optional<core::SceneFrameResolutionSelectorDTO> {
        using T = std::remove_cvref_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, core::AddObjectRequestDTO> ||
                      std::is_same_v<T, core::UpsertObjectRequestDTO> ||
                      std::is_same_v<T, core::UpdateObjectPoseRequestDTO> ||
                      std::is_same_v<T, core::DetachCarriedObjectRequestDTO>) {
          if constexpr (std::is_same_v<T, core::UpdateObjectPoseRequestDTO>) {
            const auto object = std::find_if(
                current.objects.begin(), current.objects.end(),
                [&](const auto &entry) {
                  return entry.scene_resource_id == operation.scene_resource_id;
                });
            if (object != current.objects.end())
              if (const auto *carried =
                      std::get_if<core::CarriedSceneObjectPlacementDTO>(
                          &object->placement)) {
                const auto attachment =
                    std::find_if(current.carried_object_attachments.begin(),
                                 current.carried_object_attachments.end(),
                                 [&](const auto &entry) {
                                   return entry.carried_object_attachment_id ==
                                          carried->carried_object_attachment_id;
                                 });
                if (attachment == current.carried_object_attachments.end())
                  throw LogicalSceneOwnerIntegrityError(
                      "carried pose update has no attachment");
                return core::SceneFrameResolutionSelectorDTO{
                    std::in_place_type<core::SceneFrameRefDTO>,
                    attachment->carrier_frame_ref};
              }
          }
          return std::visit(
              [&current](const auto &frame)
                  -> std::optional<core::SceneFrameResolutionSelectorDTO> {
                using Frame = std::remove_cvref_t<decltype(frame)>;
                if constexpr (std::is_same_v<Frame,
                                             core::CellSceneFrameRefDTO>) {
                  return std::nullopt;
                } else if constexpr (std::is_same_v<
                                         Frame,
                                         core::SceneResourceSceneFrameRefDTO>) {
                  const auto object = std::lower_bound(
                      current.objects.begin(), current.objects.end(),
                      frame.scene_resource_id,
                      [](const core::SceneObjectSnapshotDTO &entry,
                         const core::SceneResourceId &id) {
                        return entry.scene_resource_id < id;
                      });
                  if (object == current.objects.end() ||
                      object->scene_resource_id != frame.scene_resource_id) {
                    return std::nullopt;
                  }
                  const auto *carried =
                      std::get_if<core::CarriedSceneObjectPlacementDTO>(
                          &object->placement);
                  if (carried == nullptr) {
                    return std::nullopt;
                  }
                  const auto attachment = std::lower_bound(
                      current.carried_object_attachments.begin(),
                      current.carried_object_attachments.end(),
                      carried->carried_object_attachment_id,
                      [](const core::CarriedObjectAttachmentDTO &entry,
                         const core::CarriedObjectAttachmentId &id) {
                        return entry.carried_object_attachment_id < id;
                      });
                  if (attachment ==
                          current.carried_object_attachments.end() ||
                      attachment->carried_object_attachment_id !=
                          carried->carried_object_attachment_id ||
                      attachment->carried_object_attachment_revision !=
                          carried->carried_object_attachment_revision) {
                    throw LogicalSceneOwnerIntegrityError(
                        "carried scene-resource selector has no exact "
                        "attachment row");
                  }
                  return core::SceneFrameResolutionSelectorDTO{
                      std::in_place_type<core::SceneFrameRefDTO>,
                      attachment->carrier_frame_ref};
                } else {
                  return core::SceneFrameResolutionSelectorDTO{
                      std::in_place_type<core::SceneFrameRefDTO>,
                      core::SceneFrameRefDTO{frame}};
                }
              },
              operation.frame_ref);
        } else if constexpr (std::is_same_v<
                                 T, core::AttachCarriedObjectRequestDTO>) {
          return core::SceneFrameResolutionSelectorDTO{
              core::TargetCarrierSceneFrameRefDTO{
                  operation.controllable_component_id,
                  operation.command_target_id, operation.frame_role_id,
                  operation.carrier_component_id}};
        } else if constexpr (
            std::is_same_v<T,
                           core::DetachCarriedObjectAtCurrentPoseRequestDTO>) {
          const auto attachment =
              std::lower_bound(current.carried_object_attachments.begin(),
                               current.carried_object_attachments.end(),
                               operation.carried_object_attachment_id,
                               [](const core::CarriedObjectAttachmentDTO &entry,
                                  const core::CarriedObjectAttachmentId &id) {
                                 return entry.carried_object_attachment_id < id;
                               });
          if (attachment == current.carried_object_attachments.end() ||
              attachment->carried_object_attachment_id !=
                  operation.carried_object_attachment_id) {
            return std::nullopt;
          }
          return core::SceneFrameResolutionSelectorDTO{
              std::in_place_type<core::SceneFrameRefDTO>,
              attachment->carrier_frame_ref};
        } else {
          return std::nullopt;
        }
      },
      request.operation);
}

// Static frames carry no sensor noise, so their meaning must agree exactly.
[[nodiscard]] bool SameStaticFrameMeaning(
    const core::SceneFrameResolutionReceiptDTO &left,
    const core::SceneFrameResolutionReceiptDTO &right) noexcept {
  return left.resolved_frame == right.resolved_frame &&
         SamePose(left.frame_pose_in_cell, right.frame_pose_in_cell) &&
         left.component_attachment_revisions ==
             right.component_attachment_revisions;
}

[[nodiscard]] bool ProjectionDoesNotOwnFrame(const ErrorDTO &error) noexcept {
  if (error.code != "missing_reference") {
    return false;
  }
  const auto reason = error.details.find("reason");
  return reason != error.details.end() &&
         (reason->second == "scene_frame_reference_missing" ||
          reason->second == "kinematic_state_source_unavailable");
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] std::optional<core::SceneFrameResolutionReceiptDTO>
ResolveFrameAcrossProjections(
    const core::SceneFrameResolutionSelectorDTO &selector,
    const ProjectionRevisionDTO &projection_revision,
    const std::vector<RequiredSceneProjection> &projections,
    const std::string_view lease_scope) {
  if (projections.empty()) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "scene-frame resolution has no active native projection",
        {{"reason", "scene_frame_projection_unavailable"}});
  }

  // One owner per frame, one answer per owner. The robot's kinematic view
  // belongs to the runtime unit hosting it, and that unit's frame resolver is
  // installed into every native projection it hosts; the unit's first
  // projection (canonical order) that owns the frame answers for it, and its
  // other projections are not asked again. A projection that does not own the
  // frame declines (ProjectionDoesNotOwnFrame). Answers are never compared:
  // a robot frame claimed by two runtime units is a configuration error.
  // Cell and passive fixture frames are Scene-owned static model geometry
  // with no measured state: every unit that answers must agree exactly.
  std::vector<ScopedProjectionLease> leases;
  leases.reserve(projections.size());
  std::set<core::RuntimeUnitId> answered_units;
  std::optional<core::SceneFrameResolutionReceiptDTO> owner_resolution;
  bool robot_frame_claimed_twice = false;
  bool static_frame_disagrees = false;
  std::optional<ErrorDTO> first_missing;
  for (std::size_t ordinal = 0U; ordinal < projections.size(); ++ordinal) {
    const auto &projection = projections[ordinal];
    core::ProjectionLeaseId lease_id("scene-resolve-" +
                                     std::string(lease_scope) + "-" +
                                     std::to_string(ordinal));
    auto retained = projection.projection->RetainProjectionBinding(
        projection_revision, lease_id);
    if (!retained.ok()) {
      throw SceneRequestRejectionError(retained.error().code,
                                       retained.error().message,
                                       retained.error().details);
    }
    const core::ProjectionLeaseBindingDTO expected_binding{projection_revision,
                                                           lease_id};
    if (retained.value() != expected_binding) {
      projection.projection->ReleaseProjection(lease_id);
      throw SceneRequestRejectionError(
          "live_scene_snapshot_unavailable",
          "native projection retained a different scene revision",
          {{"reason", "scene_frame_lease_binding_mismatch"}});
    }
    leases.emplace_back(projection.projection, lease_id);
    const auto &runtime_unit_id = projection.binding.runtime_unit_id;
    if (answered_units.contains(runtime_unit_id)) {
      continue;
    }
    auto resolved = projection.projection->ResolveSceneFrame(
        core::SceneFrameResolutionQueryDTO{selector, expected_binding});
    if (!resolved.ok()) {
      if (!ProjectionDoesNotOwnFrame(resolved.error())) {
        throw SceneRequestRejectionError(resolved.error().code,
                                         resolved.error().message,
                                         resolved.error().details);
      }
      if (!first_missing.has_value()) {
        first_missing = resolved.error();
      }
      continue;
    }
    if (!resolved.value().valid() ||
        resolved.value().projection_binding != expected_binding ||
        resolved.value().predicted_state_digest.has_value()) {
      throw SceneRequestRejectionError(
          "live_scene_snapshot_unavailable",
          "native projection returned a malformed frame receipt",
          {{"reason", "scene_frame_receipt_mismatch"}});
    }
    answered_units.insert(runtime_unit_id);
    if (!owner_resolution.has_value()) {
      owner_resolution = std::move(resolved.value());
    } else if (std::holds_alternative<core::ResolvedComponentModelFrameDTO>(
                   resolved.value().resolved_frame)) {
      robot_frame_claimed_twice = true;
    } else if (!SameStaticFrameMeaning(*owner_resolution, resolved.value())) {
      static_frame_disagrees = true;
    }
  }
  if (robot_frame_claimed_twice) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "more than one execution host unit owns this robot frame",
        {{"reason", "scene_frame_owner_ambiguous"}});
  }
  if (static_frame_disagrees) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "execution host units disagree on a static scene frame",
        {{"reason", "scene_frame_resolution_ambiguous"}});
  }
  if (!owner_resolution.has_value()) {
    if (first_missing.has_value()) {
      throw SceneRequestRejectionError(
          first_missing->code, first_missing->message, first_missing->details);
    }
    throw SceneRequestRejectionError(
        "missing_reference", "scene frame has no retained resolution",
        {{"reason", "scene_frame_reference_missing"}});
  }
  for (auto &lease : leases) {
    lease.Release();
  }
  return owner_resolution;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

[[nodiscard]] std::optional<core::SceneFrameResolutionReceiptDTO>
ResolveFrameForRequest(
    const SceneTransactionRequestDTO &request,
    const LogicalSceneSnapshotDTO &current,
    const std::vector<RequiredSceneProjection> &projections) {
  const auto selector = FrameSelectorForRequest(request, current);
  if (!selector.has_value()) {
    return std::nullopt;
  }
  return ResolveFrameAcrossProjections(
      *selector, request.expected_projection_revision, projections,
      core::RequestKeyText(request.request_context.request_key));
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] core::CollisionPolicySnapshot SelectEffectivePolicy(
    const LogicalSceneSnapshotDTO &candidate,
    std::vector<std::optional<core::Result<core::CollisionPolicySnapshot>>>
        projections) {
  if (projections.empty()) {
    return candidate.configured_collision_policy;
  }
  // The owner-side resolved policy is evidence for this exact immutable scene
  // candidate, so its nonzero monotonic publisher counter follows the scene
  // revision. The separately retained policy_epoch remains the semantic gate
  // used by operation leases when derived pairs change with stable intent.
  const auto effective_policy_revision_id = candidate.revision.revision_id;
  std::optional<core::CollisionPolicySnapshot> selected;
  for (auto &answer : projections) {
    if (!answer.has_value()) {
      throw SceneRequestRejectionError(
          "live_scene_snapshot_unavailable",
          "a required native projection answered no effective collision "
          "policy",
          {{"reason", "effective_collision_policy_unavailable"}});
    }
    auto &projected = *answer;
    if (!projected.ok()) {
      throw SceneRequestRejectionError(projected.error().code,
                                       projected.error().message,
                                       projected.error().details);
    }
    const auto &effective = projected.value();
    if (effective.revision.revision_id != effective_policy_revision_id ||
        effective.policy_id !=
            candidate.configured_collision_policy.policy_id ||
        !effective.contact_rules.empty() ||
        effective.require_full_diagnostics !=
            candidate.configured_collision_policy.require_full_diagnostics) {
      throw SceneRequestRejectionError(
          "scene_policy_projection_invalid",
          "native projection changed configured collision-policy semantics",
          {{"reason", "effective_collision_policy_contract_mismatch"}});
    }
    try {
      if (core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
              effective)
              .value() != effective.revision.content_hash) {
        throw SceneRequestRejectionError(
            "scene_policy_projection_invalid",
            "native projection returned an inexact collision-policy digest",
            {{"reason", "effective_collision_policy_digest_mismatch"}});
      }
    } catch (const SceneRequestRejectionError &) {
      throw;
    } catch (const std::exception &error) {
      throw SceneRequestRejectionError(
          "scene_policy_projection_invalid", error.what(),
          {{"reason", "effective_collision_policy_not_canonical"}});
    }
    if (!selected.has_value()) {
      selected = std::move(projected.value());
    } else if (*selected != projected.value()) {
      throw SceneRequestRejectionError(
          "scene_policy_projection_ambiguous",
          "active native projections disagree on effective collision policy",
          {{"reason", "effective_collision_policy_mismatch"}});
    }
  }
  return std::move(*selected);
}

[[nodiscard]] core::CollisionPolicySnapshot ProjectEffectivePolicy(
    const LogicalSceneSnapshotDTO &candidate,
    const std::vector<RequiredSceneProjection> &projections) {
  std::vector<std::optional<core::Result<core::CollisionPolicySnapshot>>>
      projected;
  projected.reserve(projections.size());
  for (const auto &projection : projections) {
    projected.emplace_back(
        projection.projection->ProjectEffectiveCollisionPolicy(
            candidate, candidate.revision.revision_id));
  }
  return SelectEffectivePolicy(candidate, std::move(projected));
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

core::ProjectionCommitResultDTO
UncertainProjectionResult(const core::SceneTransactionDTO &transaction,
                          const core::ProjectionHealthDTO &preflight_health,
                          ErrorDTO error) {
  auto unavailable = preflight_health;
  unavailable.state = ProjectionHealthState::kUnavailable;
  unavailable.error = error;
  core::ProjectionCommitResultDTO result{
      transaction.transaction_sequence,
      transaction.proposed_projection_revision,
      core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain,
      std::nullopt,
      unavailable.native_projection_generation,
      false,
      std::move(unavailable),
      std::move(error),
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "cannot construct a fail-closed projection result");
  }
  return result;
}

core::ProjectionCommitResultDTO
NormalizeProjectionResult(const core::SceneTransactionDTO &transaction,
                          const core::ProjectionHealthDTO &preflight_health,
                          core::ProjectionCommitResultDTO result) {
  const auto contract_error = [&] {
    return UncertainProjectionResult(
        transaction, preflight_health,
        SceneError("scene_projection_contract_violation",
                   "native scene projection returned contradictory or "
                   "incomplete acknowledgement evidence"));
  };
  if (!result.presence_valid() ||
      result.transaction_sequence != transaction.transaction_sequence ||
      result.proposed_projection_revision !=
          transaction.proposed_projection_revision ||
      (!SameExecutionHostBindingExceptEpoch(
           result.resulting_health.execution_host_binding,
           preflight_health.execution_host_binding) &&
       result.resulting_health.execution_host_binding !=
           preflight_health.execution_host_binding) ||
      result.resulting_health.source_sequence <
          preflight_health.source_sequence) {
    return contract_error();
  }
  switch (result.result) {
  case core::ProjectionCommitResultStateDTO::kAccepted:
    return contract_error();
  case core::ProjectionCommitResultStateDTO::kCommitted:
    return result;
  case core::ProjectionCommitResultStateDTO::kRejectedRollbackProven:
    if (!result.resulting_health.healthy_for_target(
            transaction.prior_projection_revision) ||
        (result.acknowledged_projection_revision.has_value() &&
         *result.acknowledged_projection_revision !=
             transaction.prior_projection_revision)) {
      return contract_error();
    }
    return result;
  case core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain:
    return result;
  }
  return contract_error();
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

// A pose-only delta: the upsert of an existing cell-fixed object, without an
// attachment, that keeps its collision shapes, mass and placement kind; only
// its pose or provider evidence changes. It changes no topology, policy or
// attachment, so it cannot change the effective collision policy, and the
// Commit preflight asks for health only (OD-376 S5).
[[nodiscard]] bool IsPoseOnlyDelta(
    const LogicalSceneSnapshotDTO &current,
    const core::ResolvedSceneOperationDTO &operation) {
  const auto *upsert =
      std::get_if<core::ResolvedUpsertSceneObjectDTO>(&operation);
  if (upsert == nullptr || upsert->attachment.has_value()) {
    return false;
  }
  const auto &next = upsert->object;
  const auto found = std::lower_bound(
      current.objects.begin(), current.objects.end(), next.scene_resource_id,
      [](const SceneObjectSnapshotDTO &object, const SceneResourceId &id) {
        return object.scene_resource_id < id;
      });
  return found != current.objects.end() &&
         found->scene_resource_id == next.scene_resource_id &&
         std::holds_alternative<core::CellFixedSceneObjectPlacementDTO>(
             found->placement) &&
         std::holds_alternative<core::CellFixedSceneObjectPlacementDTO>(
             next.placement) &&
         found->collision_shapes == next.collision_shapes &&
         found->mass_kg == next.mass_kg;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {

core::Result<core::SceneCandidatePreviewDTO>
BuildResolvedSceneCandidatePreview(
    const core::ResolvedSceneCandidatePreviewRequestDTO &preview,
    const core::ProjectionRevisionDTO &predecessor,
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> snapshot,
    const std::uint64_t transaction_sequence,
    const CandidatePolicyProjector &project_policy) {
  const auto reject = [](std::string code, std::string message,
                         std::map<std::string, std::string> details = {}) {
    return core::Result<core::SceneCandidatePreviewDTO>::Err(
        SceneError(code, message, std::move(details)));
  };
  const auto &request = preview.request;
  if (!std::holds_alternative<core::AttachCarriedObjectRequestDTO>(
          request.operation) &&
      !std::holds_alternative<core::DetachCarriedObjectRequestDTO>(
          request.operation)) {
    return reject("invalid_request",
                  "Candidate scene preview currently admits only carried-"
                  "object attach or detach",
                  {{"reason", "scene_candidate_operation_unsupported"}});
  }
  try {
  if (!snapshot || !snapshot->locally_valid() || !predecessor.valid() ||
      snapshot->revision != predecessor.scene_revision ||
      request.expected_projection_revision != predecessor ||
      snapshot->revision.content_hash !=
          core::logical_scene_identity::ComputeLogicalSceneContentHash(*snapshot)) {
    return reject("invalid_request", "Candidate snapshot does not match its planning projection");
  }
    ValidateBoundedText(request.request_context.source_label, "source_label");
    if (!request.attribution.locally_valid() ||
        !request.expected_projection_revision.valid() ||
        !preview.frame_resolution.has_value() ||
        !preview.frame_resolution->valid() ||
        !preview.frame_resolution->predicted_state_digest.has_value() ||
        preview.frame_resolution->projection_binding.projection_revision !=
            request.expected_projection_revision) {
      throw std::invalid_argument(
          "candidate scene request or frame resolution is invalid");
    }
  } catch (const std::exception &error) {
    return reject("invalid_request", error.what(),
                  {{"reason", "scene_candidate_request_invalid"}});
  }

  std::string request_digest_input;
  ContentDigest request_digest(std::string{});
  try {
    request_digest_input =
        logical_scene_codec_internal::EncodeRequestDigestInput(request);
    request_digest = ContentDigest(
        manyforge::content_identity::sha256_digest(request_digest_input));
  } catch (const std::exception &error) {
    return reject("invalid_request", error.what(),
                  {{"reason", "scene_candidate_request_not_canonical"}});
  }

  try {
    auto candidate =
        ResolveSceneCandidate(request, *snapshot, transaction_sequence,
                              request_digest, preview.frame_resolution);
    auto effective_policy = project_policy(candidate.snapshot);
    if (!effective_policy.ok()) {
      return core::Result<core::SceneCandidatePreviewDTO>::Err(effective_policy.error());
    }
    const core::ProjectionRevisionDTO proposed{
        predecessor.retained_bundle_id,
        predecessor.graph_revision,
        candidate.snapshot.revision};
    auto result = core::SceneCandidatePreviewDTO{
        request_digest,
        predecessor,
        proposed,
        std::make_shared<const LogicalSceneSnapshotDTO>(
            std::move(candidate.snapshot)),
        std::move(candidate.operation),
        std::move(effective_policy.value()),
        candidate.update_status,
        candidate.resolution_evidence_digest};
    if (!result.valid()) {
      return reject("invariant_violation",
                    "Scene candidate reducer produced an invalid successor",
                    {{"reason", "scene_candidate_result_invalid"}});
    }
    return core::Result<core::SceneCandidatePreviewDTO>::Ok(std::move(result));
  } catch (const SceneRequestRejectionError &error) {
    return reject(error.code(), error.what(), error.details());
  } catch (const LogicalSceneOwnerConflictError &error) {
    return reject("scene_object_state_conflict", error.what());
  } catch (const LogicalSceneOwnerLimitError &error) {
    return reject("request_ledger_full", error.what());
  } catch (const std::invalid_argument &error) {
    return reject("invalid_request", error.what());
  } catch (const std::exception &error) {
    return reject("live_scene_snapshot_unavailable", error.what());
  } catch (...) {
    return reject("live_scene_snapshot_unavailable",
                  "Scene candidate preview raised a non-standard exception");
  }
}

core::Result<core::SceneCandidatePreviewDTO>
LogicalSceneOwner::PreviewResolvedSceneCandidate(
    const core::ResolvedSceneCandidatePreviewRequestDTO &preview) const {
  const auto reject = [](std::string code, std::string message,
                         std::map<std::string, std::string> details = {}) {
    return core::Result<core::SceneCandidatePreviewDTO>::Err(
        SceneError(code, message, std::move(details)));
  };
  const auto &request = preview.request;
  DurableState observed_state;
  std::optional<SceneTransactionStoreState> observed_transactions;
  {
    std::lock_guard lock(impl_->mutex_);
    const auto &cached = impl_->ReadSceneCommitState();
    observed_state = cached.durable_state;
    observed_transactions = cached.transaction_state;
    if (!observed_state.active_projection.has_value() ||
        !observed_state.active_scene_snapshot.has_value() ||
        !observed_transactions.has_value()) {
      return reject("live_scene_snapshot_unavailable",
                    "Logical-scene bootstrap is not finalized",
                    {{"reason", "scene_candidate_snapshot_unavailable"}});
    }
    const auto access = AccessStatus(observed_state);
    if (!access.scene_writes_open) {
      return reject("live_scene_snapshot_unavailable",
                    "Logical-scene projection is unavailable or changing",
                    {{"reason", "scene_candidate_writes_fenced"}});
    }
    if (!ExpectedProjectionAdmitsActive(request,
                                        *observed_state.active_projection)) {
      return reject("stale_revision",
                    "Candidate scene request names another Deployment or "
                    "Scene",
                    {{"reason", "scene_candidate_projection_stale"}});
    }
  }
  // A request may name any revision of this Scene; the candidate resolves
  // against the active revision, like Commit.
  auto active_preview = preview;
  active_preview.request.expected_projection_revision =
      *observed_state.active_projection;
  if (active_preview.frame_resolution.has_value()) {
    active_preview.frame_resolution->projection_binding.projection_revision =
        *observed_state.active_projection;
  }

  std::vector<RequiredSceneProjection> projections;
  const auto required_bindings = ActiveExecutionHostBindings(observed_state);
  projections.reserve(required_bindings.size());
  for (const auto &binding : required_bindings) {
    try {
      auto projection =
          ResolveRequiredProjection(
            binding, impl_->projection_resolver_, *observed_state.active_projection);
      if (!projection.preflight_health->healthy_for_target(
              *observed_state.active_projection)) {
        return reject(
            "live_scene_snapshot_unavailable",
            "A required native projection is not healthy at the candidate "
            "predecessor",
            {{"reason", "scene_candidate_native_projection_unhealthy"}});
      }
      projections.push_back(std::move(projection));
    } catch (const std::exception &error) {
      return reject(
          "live_scene_snapshot_unavailable", error.what(),
          {{"reason", "scene_candidate_native_projection_unavailable"}});
    } catch (...) {
      return reject(
          "live_scene_snapshot_unavailable",
          "Native scene projection raised a non-standard exception",
          {{"reason", "scene_candidate_native_projection_unavailable"}});
    }
  }

  return BuildResolvedSceneCandidatePreview(
      active_preview, *observed_state.active_projection,
      std::make_shared<const LogicalSceneSnapshotDTO>(*observed_state.active_scene_snapshot),
      observed_transactions->next_transaction_sequence,
      [&](const LogicalSceneSnapshotDTO &candidate) {
        return core::Result<core::CollisionPolicySnapshot>::Ok(
            ProjectEffectivePolicy(candidate, projections));
      });
}

SceneCommitResultDTO
LogicalSceneOwner::Commit(const SceneTransactionRequestDTO &request) {
  const auto reject = [&](const core::RequestReplayStatusDTO replay_status,
                          const std::string_view code,
                          const std::string_view message,
                          std::map<std::string, std::string> details = {}) {
    return FailedSceneResult(request.request_context.request_key, replay_status,
                             SceneError(code, message, std::move(details)),
                             SceneProjectionCommitStatusDTO::kNotAttempted);
  };

  try {
    ValidateBoundedText(request.request_context.source_label, "source_label");
    if (!request.attribution.locally_valid()) {
      throw std::invalid_argument("request attribution is invalid");
    }
    if (request.attribution.principal_id.has_value()) {
      ValidateBoundedText(*request.attribution.principal_id, "principal_id");
    }
    if (!request.expected_projection_revision.valid()) {
      throw std::invalid_argument("expected projection revision is invalid");
    }
  } catch (const std::invalid_argument &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
                  error.what());
  }

  const bool runtime_node = request.program_node_evidence.has_value();
  if (runtime_node) {
    const bool carried =
        std::holds_alternative<core::AttachCarriedObjectRequestDTO>(
            request.operation) ||
        std::holds_alternative<core::DetachCarriedObjectRequestDTO>(
            request.operation) ||
        std::holds_alternative<
            core::DetachCarriedObjectAtCurrentPoseRequestDTO>(
            request.operation);
    const auto &evidence = *request.program_node_evidence;
    if (request.program_reset_scene_authorization.has_value() ||
        evidence.owner_api !=
            (carried ? "scene.carried_attachment" : "scene.transaction") ||
        (!carried && !IsAuthoredSceneMutation(request))) {
      return reject(
          core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
          "runtime Scene transaction differs from its Program owner API");
    }
  }
  const bool authored_mutation =
      IsAuthoredSceneMutation(request) || runtime_node;
  const bool program_reset_mutation =
      authored_mutation &&
      request.program_reset_scene_authorization.has_value();
  if (authored_mutation &&
      (request.expected_admission_generation.has_value() ==
       program_reset_mutation)) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                  "authoring_not_ready",
                  "authored scene mutation requires exactly one ordinary or "
                  "Program Reset authorization");
  }
  if (!authored_mutation &&
      (request.expected_admission_generation.has_value() ||
       request.program_reset_scene_authorization.has_value())) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                  "invalid_request",
                  "runtime-owned scene mutation cannot carry scene authoring "
                  "authorization");
  }

  std::string request_json;
  std::string request_digest_input;
  ContentDigest request_digest(std::string{});
  std::optional<core::CellAdmissionClaimIntentDTO> claim_intent;
  try {
    request_json = logical_scene_codec_internal::EncodeRequest(request);
    request_digest_input =
        logical_scene_codec_internal::EncodeRequestDigestInput(request);
    request_digest = ContentDigest(
        manyforge::content_identity::sha256_digest(request_digest_input));
    // Validate native/private input before it can enter durable Scene state.
    // Reuse this exact derived intent at the single admission boundary.
    if (authored_mutation && !program_reset_mutation) {
      claim_intent = AuthoredSceneClaimIntent(request);
    }
  } catch (const std::exception &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
                  error.what());
  }

  // OD-376: the owner lock is held only to read and write owner state, never
  // across a host or Kernel call. Phase 1 reads and checks the state; the
  // preflight, resolution and admission run unlocked; phase 2 commits the
  // intent only when the state generation is unchanged (compare-and-commit),
  // else the request is refused as stale. From its intent to its outcome
  // the Commit is in flight: readers see the settled predecessor, every
  // other write sees the unresolved intent and is refused.
  std::unique_lock lock(impl_->mutex_);
  auto &memory = impl_->memory_;
  DurableState observed_state;
  std::optional<SceneTransactionStoreState> observed_transactions;
  bool recovered_claims_settled = false;
  for (;;) {
    const auto &cached = impl_->ReadSceneCommitState();
    observed_state = cached.durable_state;
    observed_transactions = cached.transaction_state;
    if (!observed_state.active_projection.has_value() ||
        !observed_state.active_scene_snapshot.has_value()) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "logical-scene bootstrap is not finalized");
    }
    if (!observed_transactions.has_value()) {
      throw LogicalSceneOwnerIntegrityError(
          "finalized logical scene has no reconstructed transaction state");
    }
    if (const auto existing = ReadOutcomeForRequest(
            memory, request.request_context.request_key);
        existing.has_value()) {
      if (existing->request_digest != request_digest) {
        return reject(core::RequestReplayStatusDTO::kRequestIdConflict,
                      "request_id_conflict",
                      "scene request key was reused with different intent");
      }
      auto replayed = ResultFromStoredOutcome(
          memory, *existing, core::RequestReplayStatusDTO::kReplayedExact);
      const bool success = existing->success;
      const auto projection_status = existing->projection_status;
      lock.unlock();
      if (impl_->cell_admission_ != nullptr) {
        try {
          SettleAuthoredSceneClaimIfHeld(*impl_->cell_admission_, request,
                                         request_digest, success,
                                         projection_status);
        } catch (const CellAdmissionTemporarilyUnavailable &) {
          // Exact replay remains deterministic while the retained Kernel
          // claim, if any, continues to keep authoring fail-closed.
        }
      }
      return replayed;
    }
    if (authored_mutation && impl_->cell_admission_ == nullptr) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "authoring_not_ready",
                    "new authored scene mutation requires owner-local "
                    "admission");
    }
    if (impl_->commit_in_flight_.has_value()) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "stale_revision",
                    "another Scene change is being committed",
                    {{"reason", "scene_commit_in_progress"}});
    }
    const auto access = AccessStatus(observed_state);
    if (!access.scene_writes_open) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "logical-scene projection is unavailable or "
                    "desynchronized");
    }
    if (!ExpectedProjectionAdmitsActive(request,
                                        *observed_state.active_projection)) {
      return reject(
          core::RequestReplayStatusDTO::kAcceptedNew, "stale_revision",
          "expected projection names another Deployment or Scene");
    }
    if (observed_transactions->resident_transaction_count >=
        kMaximumLogicalSceneTransactions) {
      if (impl_->cell_admission_ != nullptr && !recovered_claims_settled) {
        // A recorded Scene outcome can precede its admission release. Keep
        // that proof until exact settlement succeeds, before folding it. The
        // settlement runs unlocked; the state is then read again.
        const auto claims = RecoveredAuthoredSceneClaims(memory);
        lock.unlock();
        try {
          SettleRecoveredAuthoredSceneClaims(claims, *impl_->cell_admission_);
        } catch (const CellAdmissionTemporarilyUnavailable &error) {
          return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                        "kernel_owner_unavailable", error.what());
        }
        recovered_claims_settled = true;
        lock.lock();
        continue;
      }
      impl_->InvalidateSceneCommitState();
      auto compaction = PrepareSceneHistoryCompaction(
          memory, observed_state, *observed_transactions, false);
      CompactSceneTransactionHistory(memory, std::move(compaction),
                                     observed_state, *observed_transactions);
      observed_transactions->resident_transaction_count = 0U;
      impl_->StoreSceneCommitState(observed_state, observed_transactions);
      // The triggering request continues below in the same epoch.
    }
    break;
  }
  const auto observed_generation = impl_->state_generation_;
  lock.unlock();

  const auto active = *observed_state.active_projection;
  const auto required_bindings = ActiveExecutionHostBindings(observed_state);
  std::vector<RequiredSceneProjection> projections;
  projections.reserve(required_bindings.size());
  for (const auto &binding : required_bindings) {
    try {
      // Its health is the preflight's answer below.
      projections.push_back(
          {ResolveProjectionClient(binding, impl_->projection_resolver_),
           std::nullopt, binding});
    } catch (const std::exception &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable", error.what());
    }
  }

  // The typed refusal of a failed frame resolution, candidate or policy.
  const auto refusal = [&]() -> SceneCommitResultDTO {
    try {
      throw;
    } catch (const SceneRequestRejectionError &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew, error.code(),
                    error.what(), error.details());
    } catch (const LogicalSceneOwnerConflictError &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "scene_object_state_conflict", error.what());
    } catch (const LogicalSceneOwnerLimitError &error) {
      return reject(core::RequestReplayStatusDTO::kRequestLedgerFull,
                    "request_ledger_full", error.what());
    } catch (const std::invalid_argument &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "invalid_request", error.what());
    } catch (const std::exception &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable", error.what());
    }
  };

  // The candidate is resolved, validated and digested once, here. Its
  // refusal waits for the preflight: an unhealthy projection is refused
  // first, as before.
  std::optional<ResolvedSceneCandidate> candidate;
  std::optional<core::SceneTransactionDTO> scene_transaction;
  std::optional<SceneCommitResultDTO> candidate_refusal;
  try {
    // A request may name any revision of this Scene;
    // its frames and candidate always resolve against the active revision.
    auto active_request = request;
    active_request.expected_projection_revision = active;
    const auto frame_resolution = ResolveFrameForRequest(
        active_request, *observed_state.active_scene_snapshot, projections);
    candidate = ResolveSceneCandidate(
        active_request, *observed_state.active_scene_snapshot,
        observed_transactions->next_transaction_sequence, request_digest,
        frame_resolution);
    // A Scene that could not be published in the Manager status stream is
    // refused here, before any claim or intent.
    if (candidate->snapshot.revision != active.scene_revision) {
      const auto bytes =
          logical_scene_codec_internal::EncodeSnapshot(candidate->snapshot)
              .size();
      if (bytes > kMaximumLogicalSceneSnapshotBytes) {
        throw SceneRequestRejectionError(
            "scene_too_large",
            "the changed Scene would exceed its size limit",
            {{"scene_bytes", std::to_string(bytes)},
             {"maximum_scene_bytes",
              std::to_string(kMaximumLogicalSceneSnapshotBytes)}});
      }
    }
    scene_transaction = core::SceneTransactionDTO{
        request.request_context.request_key,
        request.attribution,
        observed_transactions->next_transaction_sequence,
        active,
        ProjectionRevisionDTO{
            active.retained_bundle_id,
            active.graph_revision,
            candidate->snapshot.revision,
        },
        request_digest,
        candidate->operation,
        candidate->resolution_evidence_digest,
    };
  } catch (const std::exception &) {
    candidate_refusal = refusal();
  }

  // One preflight call per projection: its health at the predecessor and,
  // unless the delta only moves an object, the effective policy of the
  // candidate.
  const bool project_policy =
      scene_transaction.has_value() &&
      !IsPoseOnlyDelta(*observed_state.active_scene_snapshot,
                       scene_transaction->resolved_operation);
  std::vector<std::optional<core::Result<core::CollisionPolicySnapshot>>>
      effective_policies;
  effective_policies.reserve(projections.size());
  for (auto &projection : projections) {
    try {
      auto answer = [&] {
        try {
          return projection.projection->CommitPreflight(
              active, project_policy ? &*scene_transaction : nullptr,
              project_policy ? &candidate->snapshot : nullptr);
        } catch (const std::bad_alloc &) {
          throw;
        } catch (const std::exception &error) {
          throw LogicalSceneOwnerConflictError(
              "native projection is unavailable: " +
              std::string(error.what()));
        }
      }();
      // A typed refusal of the call itself, e.g. a transaction beyond the
      // transport bound (native_projection_rpc_bound_exceeded).
      if (!answer.ok()) {
        return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                      answer.error().code, answer.error().message,
                      answer.error().details);
      }
      auto &preflight = answer.value();
      RequireProjectionHealth(preflight.health, projection.binding);
      if (!preflight.health.healthy_for_target(active)) {
        return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                      "live_scene_snapshot_unavailable",
                      "a required native scene projection is not healthy at "
                      "the exact predecessor revision");
      }
      projection.preflight_health = std::move(preflight.health);
      effective_policies.push_back(std::move(preflight.effective_policy));
    } catch (const std::exception &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable", error.what());
    } catch (...) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "native scene projection health threw a non-standard "
                    "exception");
    }
  }
  if (candidate_refusal.has_value()) {
    return *candidate_refusal;
  }
  if (project_policy) {
    try {
      auto effective_policy = SelectEffectivePolicy(
          candidate->snapshot, std::move(effective_policies));
      if (auto *policy = std::get_if<core::ResolvedCollisionPolicyDTO>(
              &scene_transaction->resolved_operation)) {
        policy->effective_policy = std::move(effective_policy);
      }
    } catch (const std::exception &) {
      return refusal();
    }
  }

  if (program_reset_mutation) {
    const auto authorized =
        impl_->cell_admission_->ValidateProgramResetSceneAuthorization(
            *request.program_reset_scene_authorization);
    if (!authorized.ok()) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "authoring_not_ready",
                    "Program Reset scene restoration is not current",
                    authorized.error().details);
    }
  }

  lock.lock();
  if (impl_->state_generation_ != observed_generation) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "stale_revision",
                  "the Scene changed while this request was prepared",
                  {{"reason", "scene_commit_concurrent_change"}});
  }
  impl_->InvalidateSceneCommitState();
  InsertSceneIntent(memory, request, request_digest, std::move(request_json),
                    *scene_transaction, *candidate, projections.size());
  InsertSceneTransactionOwnerEvent(
      memory, observed_transactions->next_owner_event_sequence,
      scene_transaction->transaction_sequence);
  // In flight from the intent to the outcome, across the admission claim
  // and the native apply.
  impl_->commit_in_flight_.emplace(SceneCommitStateCache{
      std::move(observed_state), std::move(observed_transactions)});
  // Ends this Commit's in-flight state if an exception leaves it set: the
  // unresolved intent is then visible to readers, closed, as before. Once
  // taken, the state is no longer this Commit's to clear.
  struct InFlight final {
    LogicalSceneOwner::Impl &impl;
    std::unique_lock<std::mutex> &lock;
    bool owned{true};
    ~InFlight() {
      if (!owned) {
        return;
      }
      if (!lock.owns_lock()) {
        lock.lock();
      }
      impl.EndCommitInFlight();
    }
  } in_flight{*impl_, lock, true};
  // Under the lock: the settled predecessor, no longer in flight.
  const auto take_settled = [&] {
    auto settled = std::move(*impl_->commit_in_flight_);
    impl_->EndCommitInFlight();
    in_flight.owned = false;
    return settled;
  };

  // The intent is recorded before the claim is acquired, so no claim is
  // held without one; a refused claim is this request's outcome.
  std::optional<core::CellAdmissionClaimDTO> authored_claim;
  if (authored_mutation && !program_reset_mutation) {
    lock.unlock();
    // An admission owner that cannot be reached refuses authoring
    // (authoring_not_ready), as the receipt lookup that preceded the claim
    // did before OD-376; the Kernel's own refusal is the outcome as it is.
    auto acquired = [&] {
      try {
        auto claim =
            impl_->cell_admission_->AcquireCellAdmissionClaim(*claim_intent);
        if (!claim.ok() && AdmissionOwnerUnreachable(claim.error().code)) {
          return core::Result<core::CellAdmissionClaimDTO>::Err(
              SceneError("authoring_not_ready",
                         "authored scene admission is unavailable: " +
                             claim.error().message));
        }
        return claim;
      } catch (const std::exception &error) {
        return core::Result<core::CellAdmissionClaimDTO>::Err(
            SceneError("authoring_not_ready", error.what()));
      }
    }();
    lock.lock();
    if (!acquired.ok()) {
      InsertSceneOutcome(
          memory, request.request_context.request_key, request_digest,
          scene_transaction->transaction_sequence, false,
          SceneProjectionCommitStatusDTO::kNotAttempted, 0U, acquired.error());
      // Readers replay the store again: the cache was dropped at the intent.
      static_cast<void>(take_settled());
      return FailedSceneResult(request.request_context.request_key,
                               core::RequestReplayStatusDTO::kAcceptedNew,
                               acquired.error(),
                               SceneProjectionCommitStatusDTO::kNotAttempted);
    }
    authored_claim = std::move(acquired.value());
  }
  if (!projections.empty()) {
    InsertProjectionAttempt(memory);
    lock.unlock();
  }

  const auto &transaction = *scene_transaction;
  bool every_attempt_rolled_back = true;
  for (std::size_t ordinal = 0U; ordinal < projections.size(); ++ordinal) {
    const auto &projection = projections[ordinal];
    core::ProjectionCommitResultDTO projection_result = [&] {
      try {
        return NormalizeProjectionResult(
            transaction, *projection.preflight_health,
            projection.projection->ApplyTransaction(transaction));
      } catch (const std::exception &error) {
        return UncertainProjectionResult(
            transaction, *projection.preflight_health,
            SceneError("scene_projection_call_failed", error.what()));
      } catch (...) {
        return UncertainProjectionResult(
            transaction, *projection.preflight_health,
            SceneError("scene_projection_call_failed",
                       "native scene projection threw a non-standard "
                       "exception"));
      }
    }();
    every_attempt_rolled_back =
        every_attempt_rolled_back &&
        projection_result.result ==
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven;
    const bool committed = projection_result.result ==
                           core::ProjectionCommitResultStateDTO::kCommitted;

    lock.lock();
    InsertProjectionResult(memory, projection_result);
    if (!committed) {
      const auto status =
          every_attempt_rolled_back
              ? SceneProjectionCommitStatusDTO::kRejectedRollbackProven
              : SceneProjectionCommitStatusDTO::kFailedProjectionUncertain;
      ErrorDTO error =
          status == SceneProjectionCommitStatusDTO::kRejectedRollbackProven
              ? *projection_result.error
              : SceneError(
                    "scene_projection_commit_uncertain",
                    "at least one invoked native projection cannot prove the "
                    "predecessor state");
      InsertSceneOutcome(memory, request.request_context.request_key,
                         request_digest, transaction.transaction_sequence,
                         false, status, ordinal + 1U, error);
      if (status ==
          SceneProjectionCommitStatusDTO::kFailedProjectionUncertain) {
        const auto failed_state = ReadCompleteDurableState(memory);
        if (AccessStatus(failed_state).projection_leases_open) {
          throw LogicalSceneOwnerIntegrityError(
              "uncertain projection result did not close projection leases");
        }
      }
      // Still in flight while the claim is settled, so no other writer can
      // settle it first.
      if (status == SceneProjectionCommitStatusDTO::kRejectedRollbackProven &&
          authored_claim.has_value()) {
        lock.unlock();
        ReleaseAuthoredSceneClaim(
            *impl_->cell_admission_, *authored_claim, request_digest,
            core::CellAdmissionSettlementKindDTO::kProvenNoEffect);
        lock.lock();
      }
      auto settled = take_settled();
      if (status == SceneProjectionCommitStatusDTO::kRejectedRollbackProven) {
        auto &transactions = *settled.transaction_state;
        ++transactions.next_transaction_sequence;
        ++transactions.next_owner_event_sequence;
        ++transactions.resident_transaction_count;
        impl_->StoreSceneCommitState(std::move(settled.durable_state),
                                     std::move(settled.transaction_state));
      }
      lock.unlock();
      return FailedSceneResult(request.request_context.request_key,
                               core::RequestReplayStatusDTO::kAcceptedNew,
                               std::move(error), status);
    }
    if (ordinal + 1U < projections.size()) {
      InsertProjectionAttempt(memory);
      lock.unlock();
    } else {
      InsertSceneOutcome(memory, request.request_context.request_key,
                         request_digest, transaction.transaction_sequence,
                         true, SceneProjectionCommitStatusDTO::kCommitted,
                         projections.size(), std::nullopt);
    }
  }

  if (projections.empty()) {
    InsertSceneOutcome(memory, request.request_context.request_key,
                       request_digest, transaction.transaction_sequence, true,
                       SceneProjectionCommitStatusDTO::kCommitted,
                       projections.size(), std::nullopt);
  }

  // Still in flight while the claim is settled, so no other writer can
  // settle it first.
  if (authored_claim.has_value()) {
    lock.unlock();
    ReleaseAuthoredSceneClaim(
        *impl_->cell_admission_, *authored_claim, request_digest,
        core::CellAdmissionSettlementKindDTO::kOwnerCommit);
    lock.lock();
  }
  auto settled = take_settled();
  // The settled head: later reads take it instead of replaying the delta.
  memory.settled_head =
      SettledSceneHead{transaction.transaction_sequence, candidate->snapshot};
  auto &settled_state = settled.durable_state;
  auto &settled_transactions = *settled.transaction_state;
  settled_state.active_projection = transaction.proposed_projection_revision;
  settled_state.active_scene_snapshot = candidate->snapshot;
  settled_state.projection_desynchronized = false;
  settled_state.unresolved_scene_transaction = false;
  settled_state.latest_uncertain_transaction_sequence.reset();
  settled_transactions.active_projection =
      transaction.proposed_projection_revision;
  settled_transactions.active_snapshot = std::move(candidate->snapshot);
  ++settled_transactions.next_transaction_sequence;
  ++settled_transactions.next_owner_event_sequence;
  ++settled_transactions.resident_transaction_count;
  settled_transactions.projection_desynchronized = false;
  settled_transactions.unresolved_native_attempt = false;
  settled_transactions.unresolved_transaction_sequence.reset();
  settled_transactions.latest_uncertain_transaction_sequence.reset();
  impl_->StoreSceneCommitState(std::move(settled.durable_state),
                               std::move(settled.transaction_state));
  lock.unlock();

  SceneCommitResultDTO result{
      request.request_context.request_key,
      core::RequestReplayStatusDTO::kAcceptedNew,
      true,
      std::nullopt,
      transaction.transaction_sequence,
      transaction.proposed_projection_revision,
      SceneProjectionCommitStatusDTO::kCommitted,
      candidate->update_status,
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "successful scene commit cannot form a valid public result");
  }
  return result;
}

} // namespace manyforge::scene
