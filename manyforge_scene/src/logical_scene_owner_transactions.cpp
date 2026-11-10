#include "logical_scene_owner_impl.hpp"
#include <manyforge_scene/scene_candidate_preview.hpp>

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

class CellAdmissionClaimRejected final : public std::runtime_error {
public:
  explicit CellAdmissionClaimRejected(core::ErrorDTO error)
      : std::runtime_error(error.message), error_(std::move(error)) {}
  const core::ErrorDTO &error() const noexcept { return error_; }

private:
  core::ErrorDTO error_;
};

[[nodiscard]] core::CellAdmissionClaimDTO RequireAuthoredSceneClaim(
    core::IKernelCellAdmissionPort &admission,
    const core::CellAdmissionClaimIntentDTO &intent) {
  const auto receipt = admission.GetCellAdmissionRecoveryReceipt(
      {intent, std::nullopt, std::nullopt});
  if (!receipt.ok()) {
    ThrowCellAdmissionFailure("authored scene admission lookup failed",
                              receipt.error());
  }
  if (receipt.value().status ==
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
      receipt.value().matched_claim.has_value()) {
    return *receipt.value().matched_claim;
  }
  if (receipt.value().status !=
      core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown) {
    throw LogicalSceneOwnerConflictError(
        "authored scene admission identity conflicts or is already settled");
  }
  const auto acquired = admission.AcquireCellAdmissionClaim(intent);
  if (!acquired.ok()) {
    throw CellAdmissionClaimRejected(acquired.error());
  }
  return acquired.value();
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

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
FailedSceneResult(const RequestId &request_id,
                  const core::RequestReplayStatusDTO replay, ErrorDTO error,
                  const SceneProjectionCommitStatusDTO projection_status) {
  SceneCommitResultDTO result{
      request_id,        replay,       false,
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

bool ReleaseFieldsMatchDefaults(const core::SupportContactDTO &contact) {
  return contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 1e-5 &&
         contact.min_release_progress_m == 0.0 &&
         contact.require_separation_by_end &&
         contact.require_strictly_decreasing_penetration &&
         !contact.allow_recontact;
}

bool ReleaseFieldsAreNeutral(const core::SupportContactDTO &contact) {
  return !contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 0.0 &&
         contact.min_release_progress_m == 0.0 &&
         !contact.require_separation_by_end &&
         !contact.require_strictly_decreasing_penetration &&
         !contact.allow_recontact;
}

bool BinaryFieldsMatchDefaults(const core::SupportContactDTO &contact) {
  return contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 1e-5 &&
         contact.min_release_progress_m == 0.0 &&
         contact.require_strictly_decreasing_penetration;
}

bool BinaryFieldsAreNeutral(const core::SupportContactDTO &contact) {
  return !contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 0.0 &&
         contact.min_release_progress_m == 0.0 &&
         !contact.require_strictly_decreasing_penetration;
}

[[noreturn]] void RejectSupportContact(const std::size_t index,
                                       const std::string_view reason,
                                       const std::string_view field,
                                       const std::string_view message) {
  throw SceneRequestRejectionError(
      "invalid_contact_policy", std::string(message),
      {{"support_contact_index", std::to_string(index)},
       {"reason", std::string(reason)},
       {"field", std::string(field)}});
}

void ValidateAndNormalizeSupportContact(core::SupportContactDTO &contact,
                                        const std::size_t index) {
  try {
    ValidateBoundedIdentity(contact.support_object_id, "support_object_id");
  } catch (const std::invalid_argument &error) {
    RejectSupportContact(index, "invalid_support_contact_reference",
                         "support_object_id", error.what());
  }
  const auto nonnegative_finite = [](const double value) {
    return std::isfinite(value) && value >= 0.0;
  };
  if (!nonnegative_finite(contact.max_start_penetration_m) ||
      !nonnegative_finite(contact.release_tolerance_m) ||
      !nonnegative_finite(contact.min_release_progress_m) ||
      !nonnegative_finite(contact.max_contact_penetration_m)) {
    RejectSupportContact(index, "support_contact_metric_invalid", "metrics",
                         "support-contact metrics must be finite and "
                         "non-negative");
  }

  switch (contact.mode) {
  case core::CollisionContactPolicyMode::kBoundedPenetration:
    if (contact.release_evidence_mode !=
        core::ContactReleaseEvidenceMode::kSignedPenetration) {
      RejectSupportContact(index, "release_evidence_mode_not_applicable",
                           "release_evidence_mode",
                           "binary release evidence is not applicable to "
                           "bounded penetration");
    }
    if (!ReleaseFieldsMatchDefaults(contact) &&
        !ReleaseFieldsAreNeutral(contact)) {
      RejectSupportContact(index, "release_field_for_bounded_penetration",
                           "release_fields",
                           "release-only fields are not applicable to "
                           "bounded penetration");
    }
    break;
  case core::CollisionContactPolicyMode::kContactProcess:
    if (contact.max_contact_penetration_m != 0.0) {
      RejectSupportContact(index, "bounded_penetration_field_not_applicable",
                           "max_contact_penetration_m",
                           "max_contact_penetration_m is valid only for "
                           "bounded penetration");
    }
    if (contact.release_evidence_mode !=
            core::ContactReleaseEvidenceMode::kSignedPenetration ||
        (!ReleaseFieldsMatchDefaults(contact) &&
         !ReleaseFieldsAreNeutral(contact))) {
      RejectSupportContact(index, "release_field_not_applicable",
                           "release_fields",
                           "release-only fields are not applicable to "
                           "contact process");
    }
    break;
  case core::CollisionContactPolicyMode::kContactRelease:
    if (contact.max_contact_penetration_m != 0.0) {
      RejectSupportContact(index, "bounded_penetration_field_not_applicable",
                           "max_contact_penetration_m",
                           "max_contact_penetration_m is valid only for "
                           "bounded penetration");
    }
    switch (contact.release_evidence_mode) {
    case core::ContactReleaseEvidenceMode::kSignedPenetration:
      break;
    case core::ContactReleaseEvidenceMode::kBinaryContact:
      if (!BinaryFieldsMatchDefaults(contact) &&
          !BinaryFieldsAreNeutral(contact)) {
        RejectSupportContact(index, "metric_field_for_binary_contact",
                             "metric_fields",
                             "signed-distance metric fields are not valid "
                             "for binary contact evidence");
      }
      break;
    default:
      RejectSupportContact(index, "unknown_release_evidence_mode",
                           "release_evidence_mode",
                           "support-contact release evidence is unsupported");
    }
    break;
  default:
    RejectSupportContact(index, "unknown_contact_policy_mode", "mode",
                         "support-contact mode is unsupported");
  }

  if (contact.mode == core::CollisionContactPolicyMode::kContactRelease) {
    contact.max_contact_penetration_m = 0.0;
    if (contact.release_evidence_mode ==
        core::ContactReleaseEvidenceMode::kBinaryContact) {
      contact.enforce_max_start_penetration = false;
      contact.max_start_penetration_m = 0.0;
      contact.release_tolerance_m = 0.0;
      contact.min_release_progress_m = 0.0;
      contact.require_strictly_decreasing_penetration = false;
    }
    return;
  }

  contact.release_evidence_mode =
      core::ContactReleaseEvidenceMode::kSignedPenetration;
  contact.enforce_max_start_penetration = false;
  contact.max_start_penetration_m = 0.0;
  contact.release_tolerance_m = 0.0;
  contact.min_release_progress_m = 0.0;
  contact.require_separation_by_end = false;
  contact.require_strictly_decreasing_penetration = false;
  contact.allow_recontact = false;
  if (contact.mode == core::CollisionContactPolicyMode::kContactProcess) {
    contact.max_contact_penetration_m = 0.0;
  }
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

std::vector<core::SupportContactDTO>
NormalizeSupportContacts(const core::SceneResourceId &scene_resource_id,
                         std::vector<core::SupportContactDTO> support_contacts,
                         const core::LogicalSceneSnapshotDTO &current) {
  for (std::size_t index = 0U; index < support_contacts.size(); ++index) {
    ValidateAndNormalizeSupportContact(support_contacts[index], index);
  }
  std::sort(support_contacts.begin(), support_contacts.end(),
            [](const auto &left, const auto &right) {
              return left.support_object_id < right.support_object_id;
            });
  for (std::size_t index = 0U; index < support_contacts.size(); ++index) {
    const auto &contact = support_contacts[index];
    if (index != 0U && support_contacts[index - 1U].support_object_id ==
                           contact.support_object_id) {
      RejectSupportContact(index, "duplicate_support_contact_pair",
                           "support_object_id",
                           "support object is declared more than once");
    }
    if (contact.support_object_id == scene_resource_id.value()) {
      RejectSupportContact(index, "self_support_contact", "support_object_id",
                           "scene resource cannot support itself");
    }
    const auto support =
        std::lower_bound(current.objects.begin(), current.objects.end(),
                         contact.support_object_id,
                         [](const SceneObjectSnapshotDTO &object,
                            const std::string &support_id) {
                           return object.scene_resource_id.value() < support_id;
                         });
    if (support == current.objects.end() ||
        support->scene_resource_id.value() != contact.support_object_id) {
      throw SceneRequestRejectionError(
          "missing_reference",
          "support contact names an unknown scene resource",
          {{"reference_type", "support_scene_resource"},
           {"reference_id", contact.support_object_id},
           {"scene_resource_id", scene_resource_id.value()}});
    }
  }
  return support_contacts;
}

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
          auto support_contacts = NormalizeSupportContacts(
              operation.scene_resource_id, operation.support_contacts, current);
          SceneObjectSnapshotDTO object{
              operation.scene_resource_id,
              {operation.shape},
              core::CellFixedSceneObjectPlacementDTO{current.cell_id,
                                                     normalized.pose_in_cell},
              std::nullopt,
              std::move(support_contacts),
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
          for (const auto &object : candidate.snapshot.objects) {
            if (object.scene_resource_id == operation.scene_resource_id) {
              continue;
            }
            const auto dependent = std::find_if(
                object.support_contacts.begin(), object.support_contacts.end(),
                [&](const core::SupportContactDTO &contact) {
                  return contact.support_object_id ==
                         operation.scene_resource_id.value();
                });
            if (dependent != object.support_contacts.end()) {
              throw SceneRequestRejectionError(
                  "scene_object_state_conflict",
                  "scene resource is still referenced as a support",
                  {{"reason", "support_contact_reference_exists"},
                   {"scene_resource_id", operation.scene_resource_id.value()},
                   {"dependent_scene_resource_id",
                    object.scene_resource_id.value()}});
            }
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
                  operation.controllable_component_id) {
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
          if (!operation.configured_policy.allowed_static_static_pairs
                   .empty() ||
              !operation.configured_policy.allowed_touch_link_pairs.empty() ||
              !operation.configured_policy.support_contacts.empty()) {
            throw SceneRequestRejectionError(
                "invalid_request",
                "configured collision policy cannot contain owner-derived "
                "static, touch, or support allowances",
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

void InsertSceneIntent(sqlite3 *database,
                       const SceneTransactionRequestDTO &request,
                       const ContentDigest &request_digest,
                       const std::string_view request_json,
                       const core::SceneTransactionDTO &transaction,
                       const ResolvedSceneCandidate &candidate,
                       const std::size_t required_projection_count) {
  const auto resolved_json =
      logical_scene_codec_internal::EncodeResolvedOperation(
          transaction.resolved_operation);
  const auto snapshot_json =
      logical_scene_codec_internal::EncodeSnapshot(candidate.snapshot);
  if (request_json.size() > kMaximumLogicalSceneTransactionBytes ||
      resolved_json.size() > kMaximumLogicalSceneTransactionBytes ||
      snapshot_json.size() > kMaximumLogicalSceneTransactionBytes) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene transaction exceeds its durable byte bound");
  }
  Statement statement(
      database,
      "INSERT INTO logical_scene_transaction_intents("
      "transaction_sequence,request_owner_epoch,request_id,request_digest,"
      "request_json,prior_retained_bundle_id,prior_graph_revision,"
      "prior_scene_id,prior_scene_revision_id,prior_scene_content_hash,"
      "proposed_retained_bundle_id,proposed_graph_revision,"
      "proposed_scene_id,proposed_scene_revision_id,"
      "proposed_scene_content_hash,resolved_operation_json,"
      "resolution_evidence_digest,candidate_snapshot_json,update_status,"
      "required_projection_count) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(1, CheckedSafeCounter(transaction.transaction_sequence,
                                       "transaction_sequence"));
  statement.Bind(2, request.request_context.request_owner_epoch.value());
  statement.Bind(3, request.request_context.request_id.value());
  statement.Bind(4, request_digest.value());
  statement.Bind(5, request_json);
  statement.Bind(
      6, transaction.prior_projection_revision.retained_bundle_id.value());
  statement.Bind(7,
                 transaction.prior_projection_revision.graph_revision.value());
  statement.Bind(
      8, transaction.prior_projection_revision.scene_revision.scene_id.value());
  statement.Bind(
      9, CheckedSafeCounter(
             transaction.prior_projection_revision.scene_revision.revision_id,
             "prior scene revision"));
  statement.Bind(10, transaction.prior_projection_revision.scene_revision
                         .content_hash.value());
  statement.Bind(
      11, transaction.proposed_projection_revision.retained_bundle_id.value());
  statement.Bind(
      12, transaction.proposed_projection_revision.graph_revision.value());
  statement.Bind(
      13,
      transaction.proposed_projection_revision.scene_revision.scene_id.value());
  statement.Bind(
      14,
      CheckedSafeCounter(
          transaction.proposed_projection_revision.scene_revision.revision_id,
          "proposed scene revision"));
  statement.Bind(15, transaction.proposed_projection_revision.scene_revision
                         .content_hash.value());
  statement.Bind(16, resolved_json);
  if (transaction.resolution_evidence_digest.has_value()) {
    statement.Bind(17, transaction.resolution_evidence_digest->value());
  } else {
    statement.BindNull(17);
  }
  statement.Bind(18, snapshot_json);
  statement.Bind(19, core::to_string(candidate.update_status));
  statement.Bind(20, static_cast<std::int64_t>(required_projection_count));
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene intent insert unexpectedly returned a row");
  }
}

std::uint64_t NextSceneOwnerEventSequence(sqlite3 *database) {
  const auto history = ReadBoundedHistoryState(database);
  Statement statement(
      database, "SELECT count(*),coalesce(max(owner_event_sequence),0) FROM "
                "logical_scene_owner_events");
  if (!statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene owner event aggregate is missing");
  }
  const auto count = statement.Integer(0);
  const auto maximum = statement.Integer(1);
  if (statement.Step() || count < 0 ||
      count >= static_cast<std::int64_t>(kMaximumLogicalSceneOwnerEvents) ||
      maximum < 0) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene owner event ledger is exhausted or invalid");
  }
  const auto watermark =
      std::max(static_cast<std::uint64_t>(maximum),
               history.compacted_through_owner_event_sequence);
  if (watermark >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene owner event sequence is exhausted");
  }
  return watermark + 1U;
}

} // namespace

// Compaction is transparent to callers: it prunes the resident ledger behind
// a checkpoint of the active head and keeps the request owner epoch in force,
// so neither the triggering request nor any other caller has to refresh.
// A maintenance fold runs the same compaction before the bounded maintenance
// history evicts a completion the replay would otherwise still consume; it
// needs only a settled head, not a full ledger or an open maintenance window.
void CompactSceneTransactionHistory(
    sqlite3 *database, const DurableState &durable_state,
    const SceneTransactionStoreState &transactions,
    const bool maintenance_fold) {
  const auto access = AccessStatus(durable_state);
  if ((!maintenance_fold &&
       (!access.scene_writes_open || !access.projection_leases_open ||
        transactions.resident_transaction_count <
            kMaximumLogicalSceneTransactions ||
        transactions.next_transaction_sequence <= 1U ||
        transactions.next_owner_event_sequence <= 1U)) ||
      durable_state.projection_desynchronized ||
      durable_state.unresolved_scene_transaction ||
      transactions.projection_desynchronized ||
      transactions.unresolved_native_attempt) {
    throw LogicalSceneOwnerConflictError(
        "logical-scene history compaction requires a settled quiescent head");
  }
  const auto history = ReadBoundedHistoryState(database);
  if (history.compaction_generation >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene history compaction generation is exhausted");
  }
  const auto next_compaction_generation = history.compaction_generation + 1U;
  // The epoch never advances here. The first compaction records the bootstrap
  // epoch (generation 2 is the schema's first compacted generation); a store
  // that rotated before this rule keeps its last rotated epoch.
  const auto request_epoch_generation =
      history.request_epoch_generation == 0U
          ? 2U
          : history.request_epoch_generation;
  const auto &request_owner_epoch = transactions.request_owner_epoch;
  const auto snapshot_json = logical_scene_codec_internal::EncodeSnapshot(
      transactions.active_snapshot);
  if (snapshot_json.empty() ||
      snapshot_json.size() > kMaximumLogicalSceneTransactionBytes) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene checkpoint exceeds its canonical byte bound");
  }

  Statement update(
      database,
      "UPDATE logical_scene_history_state SET compaction_generation=?,"
      "request_epoch_generation=?,request_owner_epoch=?,"
      "compacted_through_transaction_sequence=?,"
      "compacted_through_owner_event_sequence=?,"
      "compacted_structural_completion_count=?,"
      "compacted_maintenance_completion_count=?,"
      "checkpoint_retained_bundle_id=?,checkpoint_graph_revision=?,"
      "checkpoint_scene_id=?,checkpoint_scene_revision_id=?,"
      "checkpoint_scene_content_hash=?,checkpoint_snapshot_json=?,"
      "compaction_in_progress=1 WHERE singleton=1 AND "
      "compaction_in_progress=0");
  update.Bind(1, CheckedSafeCounter(next_compaction_generation,
                                    "compaction generation"));
  update.Bind(2, CheckedSafeCounter(request_epoch_generation,
                                    "request epoch generation"));
  update.Bind(3, request_owner_epoch.value());
  // A maintenance fold may run before any Scene transaction (watermark 0).
  update.Bind(4, static_cast<std::int64_t>(
                     transactions.next_transaction_sequence - 1U));
  update.Bind(5, static_cast<std::int64_t>(
                     transactions.next_owner_event_sequence - 1U));
  update.Bind(6, static_cast<std::int64_t>(
                     durable_state.migrated_structural_completions.size()));
  update.Bind(7, static_cast<std::int64_t>(std::ranges::count_if(
                     durable_state.maintenance_initializations,
                     [](const MaintenanceInitializationState &initialization) {
                       return initialization.publication_receipt.has_value();
                     })));
  update.Bind(8, transactions.active_projection.retained_bundle_id.value());
  update.Bind(9, transactions.active_projection.graph_revision.value());
  update.Bind(10,
              transactions.active_projection.scene_revision.scene_id.value());
  update.Bind(11, CheckedSafeCounter(
                      transactions.active_projection.scene_revision.revision_id,
                      "checkpoint scene revision"));
  update.Bind(
      12, transactions.active_projection.scene_revision.content_hash.value());
  update.Bind(13, snapshot_json);
  if (update.Step() || sqlite3_changes(database) != 1) {
    throw LogicalSceneOwnerConflictError(
        "logical-scene history compaction state changed concurrently");
  }

  Exec(database, "DELETE FROM logical_scene_projection_results");
  Exec(database, "DELETE FROM logical_scene_projection_attempts");
  Exec(database, "DELETE FROM logical_scene_request_outcomes");
  Exec(database, "DELETE FROM logical_scene_owner_events");
  Exec(database, "DELETE FROM logical_scene_transaction_intents");
  Exec(database, "UPDATE logical_scene_history_state SET "
                 "compaction_in_progress=0 WHERE singleton=1");

  const auto compacted =
      ReadSceneTransactionStoreState(database, durable_state);
  if (compacted.active_projection != transactions.active_projection ||
      compacted.active_snapshot != transactions.active_snapshot ||
      compacted.request_owner_epoch != request_owner_epoch ||
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
    sqlite3 *database, const std::uint64_t owner_event_sequence,
    const std::uint64_t transaction_sequence) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_owner_events(owner_event_sequence,"
      "event_kind,transaction_sequence,structural_completion_sequence) "
      "VALUES(?,'scene_transaction_intent',?,NULL)");
  statement.Bind(
      1, CheckedSafeCounter(owner_event_sequence, "owner_event_sequence"));
  statement.Bind(
      2, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene transaction owner event insert unexpectedly returned a row");
  }
}

void InsertProjectionAttempt(
    sqlite3 *database, const std::uint64_t transaction_sequence,
    const std::size_t ordinal,
    const core::ProjectionExecutionHostBindingDTO &binding) {
  Statement statement(
      database, "INSERT INTO logical_scene_projection_attempts("
                "transaction_sequence,projection_ordinal,provider_binding_id,"
                "provider_binding_revision,runtime_unit_id,runtime_epoch) "
                "VALUES(?,?,?,?,?,?)");
  statement.Bind(
      1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  statement.Bind(2, static_cast<std::int64_t>(ordinal));
  statement.Bind(3, binding.provider_binding_id.value());
  statement.Bind(4, binding.provider_binding_revision.value());
  statement.Bind(5, binding.runtime_unit_id.value());
  statement.Bind(6, binding.runtime_epoch.value());
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "projection attempt insert unexpectedly returned a row");
  }
}

std::string_view
ProjectionResultState(const core::ProjectionCommitResultStateDTO state) {
  switch (state) {
  case core::ProjectionCommitResultStateDTO::kAccepted:
    return "accepted";
  case core::ProjectionCommitResultStateDTO::kCommitted:
    return "committed";
  case core::ProjectionCommitResultStateDTO::kRejectedRollbackProven:
    return "rejected_rollback_proven";
  case core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain:
    return "failed_projection_uncertain";
  }
  return {};
}

void InsertProjectionResult(sqlite3 *database,
                            const std::uint64_t transaction_sequence,
                            const std::size_t ordinal,
                            const core::ProjectionCommitResultDTO &result) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_projection_results("
      "transaction_sequence,projection_ordinal,result_state,"
      "acknowledged_retained_bundle_id,acknowledged_graph_revision,"
      "acknowledged_scene_id,acknowledged_scene_revision_id,"
      "acknowledged_scene_content_hash,rollback_proven,"
      "resulting_health_state,error_code,error_message,error_details_json) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(
      1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  statement.Bind(2, static_cast<std::int64_t>(ordinal));
  statement.Bind(3, ProjectionResultState(result.result));
  if (result.acknowledged_projection_revision.has_value()) {
    const auto &ack = *result.acknowledged_projection_revision;
    statement.Bind(4, ack.retained_bundle_id.value());
    statement.Bind(5, ack.graph_revision.value());
    statement.Bind(6, ack.scene_revision.scene_id.value());
    statement.Bind(7, CheckedSafeCounter(ack.scene_revision.revision_id,
                                         "acknowledged scene revision"));
    statement.Bind(8, ack.scene_revision.content_hash.value());
  } else {
    for (int index = 4; index <= 8; ++index) {
      statement.BindNull(index);
    }
  }
  statement.Bind(9, result.rollback_proven ? 1 : 0);
  statement.Bind(10, core::to_string(result.resulting_health.state));
  if (result.error.has_value()) {
    statement.Bind(11, result.error->code);
    statement.Bind(12, result.error->message);
    statement.Bind(13, logical_scene_codec_internal::EncodeErrorDetails(
                           result.error->details));
  } else {
    statement.BindNull(11);
    statement.BindNull(12);
    statement.BindNull(13);
  }
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "projection result insert unexpectedly returned a row");
  }
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

[[nodiscard]] RequiredSceneProjection ResolveRequiredProjection(
    const core::ProjectionExecutionHostBindingDTO &binding,
    const core::IPlannerSceneProjectionResolver *resolver,
    const ProjectionRevisionDTO &selected_revision) {
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
  auto projection = std::move(resolved.value());
  const auto health = [&]() {
    try {
      return projection->ProjectionHealthForRevision(selected_revision);
    } catch (const std::bad_alloc &) {
      throw;
    } catch (const std::exception &error) {
      throw LogicalSceneOwnerConflictError(
          "native projection is unavailable: " + std::string(error.what()));
    }
  }();
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
  return {std::move(projection), std::move(health)};
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
                  operation.command_target_id, operation.frame_role_id}};
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
    const auto &runtime_unit_id =
        projection.preflight_health.execution_host_binding.runtime_unit_id;
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
      request.request_context.request_id.value());
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] core::CollisionPolicySnapshot ProjectEffectivePolicy(
    const LogicalSceneSnapshotDTO &candidate,
    const std::vector<RequiredSceneProjection> &projections) {
  if (projections.empty()) {
    return candidate.configured_collision_policy;
  }
  // The owner-side resolved policy is evidence for this exact immutable scene
  // candidate, so its nonzero monotonic publisher counter follows the scene
  // revision. The separately retained policy_epoch remains the semantic gate
  // used by operation leases when derived pairs change with stable intent.
  const auto effective_policy_revision_id = candidate.revision.revision_id;
  std::optional<core::CollisionPolicySnapshot> selected;
  for (const auto &projection : projections) {
    auto projected = projection.projection->ProjectEffectiveCollisionPolicy(
        candidate, effective_policy_revision_id);
    if (!projected.ok()) {
      throw SceneRequestRejectionError(projected.error().code,
                                       projected.error().message,
                                       projected.error().details);
    }
    const auto &effective = projected.value();
    if (effective.revision.revision_id != effective_policy_revision_id ||
        effective.policy_id !=
            candidate.configured_collision_policy.policy_id ||
        effective.contact_grasps !=
            candidate.configured_collision_policy.contact_grasps ||
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
    ValidateBoundedIdentity(request.request_context.request_id.value(),
                            "request_id");
    ValidateBoundedIdentity(request.request_context.request_owner_epoch.value(),
                            "request_owner_epoch");
    ValidateBoundedText(request.request_context.source_label, "source_label");
    if (request.request_context.correlation_id.has_value()) {
      ValidateBoundedText(*request.request_context.correlation_id,
                          "correlation_id");
    }
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
      if (!projection.preflight_health.healthy_for_target(
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
  if (request.request_context.request_id.empty()) {
    throw std::invalid_argument("scene request_id must be non-empty");
  }
  const auto reject = [&](const core::RequestReplayStatusDTO replay_status,
                          const std::string_view code,
                          const std::string_view message,
                          std::map<std::string, std::string> details = {}) {
    return FailedSceneResult(request.request_context.request_id, replay_status,
                             SceneError(code, message, std::move(details)),
                             SceneProjectionCommitStatusDTO::kNotAttempted);
  };

  try {
    ValidateBoundedIdentity(request.request_context.request_id.value(),
                            "request_id");
    ValidateBoundedIdentity(request.request_context.request_owner_epoch.value(),
                            "request_owner_epoch");
    ValidateBoundedText(request.request_context.source_label, "source_label");
    if (request.request_context.correlation_id.has_value()) {
      ValidateBoundedText(*request.request_context.correlation_id,
                          "correlation_id");
    }
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
    if (request_json.empty() ||
        request_json.size() > kMaximumLogicalSceneTransactionBytes) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "invalid_request",
                    "scene request exceeds its canonical byte bound");
    }
    request_digest_input =
        logical_scene_codec_internal::EncodeRequestDigestInput(request);
    request_digest = ContentDigest(
        manyforge::content_identity::sha256_digest(request_digest_input));
    // Validate native/private input before it can enter durable Scene state.
    // Reuse this exact derived intent at the single admission boundary.
    if (authored_mutation && !program_reset_mutation) {
      claim_intent = AuthoredSceneClaimIntent(request, request_digest);
    }
  } catch (const std::exception &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
                  error.what());
  }

  std::lock_guard lock(impl_->mutex_);
  auto *database = impl_->database();
  DurableState observed_state;
  std::optional<SceneTransactionStoreState> observed_transactions;
  std::optional<ResolvedSceneCandidate> candidate;
  std::int64_t external_data_version = 0;

  {
    const auto &cached = impl_->ReadSceneCommitState();
    observed_state = cached.durable_state;
    observed_transactions = cached.transaction_state;
    external_data_version = cached.external_data_version;
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
    const auto &owner_epoch =
        observed_transactions->request_owner_epoch.value();
    if (request.request_context.request_owner_epoch.value() != owner_epoch) {
      return reject(core::RequestReplayStatusDTO::kStaleRequestOwnerEpoch,
                    "stale_request_owner_epoch",
                    "scene request names a stale logical-owner epoch",
                    {{"expected_request_owner_epoch", owner_epoch}});
    }
    if (const auto existing = ReadOutcomeForRequest(
            database, owner_epoch, request.request_context.request_id);
        existing.has_value()) {
      if (existing->request_digest != request_digest) {
        return reject(core::RequestReplayStatusDTO::kRequestIdConflict,
                      "request_id_conflict",
                      "scene request ID was reused with different intent");
      }
      if (impl_->cell_admission_ != nullptr) {
        try {
          SettleAuthoredSceneClaimIfHeld(
              *impl_->cell_admission_, request, request_digest,
              existing->success, existing->projection_status);
        } catch (const CellAdmissionTemporarilyUnavailable &) {
          // Exact replay remains deterministic while the retained Kernel
          // claim, if any, continues to keep authoring fail-closed.
        }
      }
      return ResultFromStoredOutcome(
          database, *existing, core::RequestReplayStatusDTO::kReplayedExact);
    }
    if (authored_mutation && impl_->cell_admission_ == nullptr) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "authoring_not_ready",
                    "new authored scene mutation requires owner-local "
                    "admission");
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
      if (impl_->cell_admission_ != nullptr) {
        // A durable Scene outcome can precede its admission release. Preserve
        // that proof until exact settlement succeeds, before pruning it.
        try {
          SettleRecoveredAuthoredSceneClaims(database, *impl_->cell_admission_);
        } catch (const CellAdmissionTemporarilyUnavailable &error) {
          return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                        "kernel_owner_unavailable", error.what());
        }
      }
      Transaction compaction(database, "BEGIN IMMEDIATE",
                              &impl_->maintenance_snapshot_cache_);
      // Commit is the only writer and holds the owner mutex, so another
      // write here is a store integrity failure, not a stale request.
      if (!impl_->SceneCommitStateStillCurrent(external_data_version)) {
        impl_->InvalidateSceneCommitState();
        throw LogicalSceneOwnerIntegrityError(
            "logical-scene database changed outside its owner before history "
            "compaction");
      }
      impl_->InvalidateSceneCommitState();
      CompactSceneTransactionHistory(database, observed_state,
                                     *observed_transactions, false);
      compaction.Commit();
      observed_transactions->resident_transaction_count = 0U;
      impl_->StoreSceneCommitState(observed_state, observed_transactions,
                                   external_data_version);
      // The triggering request continues below in the same epoch.
    }
  }

  std::vector<RequiredSceneProjection> projections;
  const auto required_bindings = ActiveExecutionHostBindings(observed_state);
  projections.reserve(required_bindings.size());
  for (const auto &binding : required_bindings) {
    try {
      auto projection =
          ResolveRequiredProjection(
            binding, impl_->projection_resolver_, *observed_state.active_projection);
      if (!projection.preflight_health.healthy_for_target(
              *observed_state.active_projection)) {
        return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                      "live_scene_snapshot_unavailable",
                      "a required native scene projection is not healthy at "
                      "the exact predecessor revision");
      }
      projections.push_back(std::move(projection));
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

  try {
    // A request may name any revision of this Scene;
    // its frames and candidate always resolve against the active revision.
    auto active_request = request;
    active_request.expected_projection_revision =
        *observed_state.active_projection;
    const auto frame_resolution = ResolveFrameForRequest(
        active_request, *observed_state.active_scene_snapshot, projections);
    candidate = ResolveSceneCandidate(
        active_request, *observed_state.active_scene_snapshot,
        observed_transactions->next_transaction_sequence, request_digest,
        frame_resolution);
    auto effective_policy =
        ProjectEffectivePolicy(candidate->snapshot, projections);
    if (auto *policy = std::get_if<core::ResolvedCollisionPolicyDTO>(
            &candidate->operation)) {
      policy->effective_policy = std::move(effective_policy);
    }
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
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
                  error.what());
  } catch (const std::exception &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                  "live_scene_snapshot_unavailable", error.what());
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

  core::SceneTransactionDTO scene_transaction{
      request.request_context.request_id,
      request.attribution,
      observed_transactions->next_transaction_sequence,
      *observed_state.active_projection,
      ProjectionRevisionDTO{
          observed_state.active_projection->retained_bundle_id,
          observed_state.active_projection->graph_revision,
          candidate->snapshot.revision,
      },
      request_digest,
      candidate->operation,
      candidate->resolution_evidence_digest,
  };

  const auto reconstructed = core::ApplyResolvedSceneTransaction(
      *observed_state.active_scene_snapshot, scene_transaction);
  if (!reconstructed.ok()) {
    throw LogicalSceneOwnerIntegrityError(
        "owner-normalized scene transaction was rejected by the shared "
        "projection reducer: " +
        reconstructed.error().message);
  }
  if (reconstructed.value() != candidate->snapshot) {
    throw LogicalSceneOwnerIntegrityError(
        "owner-normalized scene transaction does not reproduce its exact "
        "candidate snapshot");
  }

  {
    Transaction write(database, "BEGIN IMMEDIATE",
                            &impl_->maintenance_snapshot_cache_);
    if (!impl_->SceneCommitStateStillCurrent(external_data_version) ||
        NextSceneOwnerEventSequence(database) !=
            observed_transactions->next_owner_event_sequence ||
        ReadOutcomeForRequest(
            database, request.request_context.request_owner_epoch.value(),
            request.request_context.request_id)
            .has_value()) {
      impl_->InvalidateSceneCommitState();
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene database changed outside its owner before durable "
          "acceptance");
    }
    impl_->InvalidateSceneCommitState();
    InsertSceneIntent(database, request, request_digest, request_json,
                      scene_transaction, *candidate, projections.size());
    InsertSceneTransactionOwnerEvent(
        database, observed_transactions->next_owner_event_sequence,
        scene_transaction.transaction_sequence);
    write.Commit();
  }

  std::optional<core::CellAdmissionClaimDTO> authored_claim;
  if (authored_mutation && !program_reset_mutation) {
    try {
      authored_claim =
          RequireAuthoredSceneClaim(*impl_->cell_admission_, *claim_intent);
    } catch (const std::exception &error) {
      const auto *rejected =
          dynamic_cast<const CellAdmissionClaimRejected *>(&error);
      const auto rejection =
          rejected != nullptr ? rejected->error()
                              : SceneError("authoring_not_ready", error.what());
      Transaction outcome(database, "BEGIN IMMEDIATE",
                              &impl_->maintenance_snapshot_cache_);
      InsertSceneOutcome(
          database, request.request_context.request_owner_epoch.value(),
          request.request_context.request_id, request_digest,
          scene_transaction.transaction_sequence, false,
          SceneProjectionCommitStatusDTO::kNotAttempted, 0U, rejection);
      outcome.Commit();
      return FailedSceneResult(request.request_context.request_id,
                               core::RequestReplayStatusDTO::kAcceptedNew,
                               rejection,
                               SceneProjectionCommitStatusDTO::kNotAttempted);
    }
  }
  if (!projections.empty()) {
    Transaction attempt(database, "BEGIN IMMEDIATE",
                            &impl_->maintenance_snapshot_cache_);
    InsertProjectionAttempt(
        database, scene_transaction.transaction_sequence, 0U,
        projections.front().preflight_health.execution_host_binding);
    attempt.Commit();
  }

  bool every_attempt_rolled_back = true;
  for (std::size_t ordinal = 0U; ordinal < projections.size(); ++ordinal) {
    const auto &projection = projections[ordinal];
    core::ProjectionCommitResultDTO projection_result = [&] {
      try {
        return NormalizeProjectionResult(
            scene_transaction, projection.preflight_health,
            projection.projection->ApplyTransaction(scene_transaction));
      } catch (const std::exception &error) {
        return UncertainProjectionResult(
            scene_transaction, projection.preflight_health,
            SceneError("scene_projection_call_failed", error.what()));
      } catch (...) {
        return UncertainProjectionResult(
            scene_transaction, projection.preflight_health,
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

    Transaction result_write(database, "BEGIN IMMEDIATE",

                            &impl_->maintenance_snapshot_cache_);
    InsertProjectionResult(database, scene_transaction.transaction_sequence,
                           ordinal, projection_result);
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
      InsertSceneOutcome(database,
                         request.request_context.request_owner_epoch.value(),
                         request.request_context.request_id, request_digest,
                         scene_transaction.transaction_sequence, false, status,
                         ordinal + 1U, error);
      if (status ==
          SceneProjectionCommitStatusDTO::kFailedProjectionUncertain) {
        const auto failed_state =
            ReadCompleteDurableState(database, impl_->fixed_query_);
        if (AccessStatus(failed_state).projection_leases_open) {
          throw LogicalSceneOwnerIntegrityError(
              "uncertain projection result did not close projection leases");
        }
      }
      result_write.Commit();
      if (status == SceneProjectionCommitStatusDTO::kRejectedRollbackProven) {
        if (authored_claim.has_value()) {
          ReleaseAuthoredSceneClaim(
              *impl_->cell_admission_, *authored_claim, request_digest,
              core::CellAdmissionSettlementKindDTO::kProvenNoEffect);
        }
        ++observed_transactions->next_transaction_sequence;
        ++observed_transactions->next_owner_event_sequence;
        ++observed_transactions->resident_transaction_count;
        if (impl_->SceneCommitStateStillCurrent(external_data_version)) {
          impl_->StoreSceneCommitState(observed_state, observed_transactions,
                                       external_data_version);
        }
      }
      return FailedSceneResult(request.request_context.request_id,
                               core::RequestReplayStatusDTO::kAcceptedNew,
                               std::move(error), status);
    }
    if (ordinal + 1U < projections.size()) {
      InsertProjectionAttempt(
          database, scene_transaction.transaction_sequence, ordinal + 1U,
          projections[ordinal + 1U].preflight_health.execution_host_binding);
    } else {
      InsertSceneOutcome(database,
                         request.request_context.request_owner_epoch.value(),
                         request.request_context.request_id, request_digest,
                         scene_transaction.transaction_sequence, true,
                         SceneProjectionCommitStatusDTO::kCommitted,
                         projections.size(), std::nullopt);
    }
    result_write.Commit();
  }

  if (projections.empty()) {
    Transaction outcome(database, "BEGIN IMMEDIATE",
                            &impl_->maintenance_snapshot_cache_);
    InsertSceneOutcome(database,
                       request.request_context.request_owner_epoch.value(),
                       request.request_context.request_id, request_digest,
                       scene_transaction.transaction_sequence, true,
                       SceneProjectionCommitStatusDTO::kCommitted,
                       projections.size(), std::nullopt);
    outcome.Commit();
  }

  if (authored_claim.has_value()) {
    ReleaseAuthoredSceneClaim(
        *impl_->cell_admission_, *authored_claim, request_digest,
        core::CellAdmissionSettlementKindDTO::kOwnerCommit);
  }

  observed_state.active_projection =
      scene_transaction.proposed_projection_revision;
  observed_state.active_scene_snapshot = candidate->snapshot;
  observed_state.projection_desynchronized = false;
  observed_state.unresolved_scene_transaction = false;
  observed_state.latest_uncertain_transaction_sequence.reset();
  observed_transactions->active_projection =
      scene_transaction.proposed_projection_revision;
  observed_transactions->active_snapshot = candidate->snapshot;
  ++observed_transactions->next_transaction_sequence;
  ++observed_transactions->next_owner_event_sequence;
  ++observed_transactions->resident_transaction_count;
  observed_transactions->projection_desynchronized = false;
  observed_transactions->unresolved_native_attempt = false;
  observed_transactions->unresolved_transaction_sequence.reset();
  observed_transactions->latest_uncertain_transaction_sequence.reset();
  if (!impl_->SceneCommitStateStillCurrent(external_data_version)) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene database changed outside its owner while completing a "
        "scene commit");
  }
  impl_->StoreSceneCommitState(observed_state, observed_transactions,
                               external_data_version);

  SceneCommitResultDTO result{
      request.request_context.request_id,
      core::RequestReplayStatusDTO::kAcceptedNew,
      true,
      std::nullopt,
      scene_transaction.transaction_sequence,
      scene_transaction.proposed_projection_revision,
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
