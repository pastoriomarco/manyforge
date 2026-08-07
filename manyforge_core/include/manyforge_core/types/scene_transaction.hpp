#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Public request to add one portable scene resource.
struct AddObjectRequestDTO final {
  SceneResourceId scene_resource_id;
  CollisionShapeDTO shape;
  Pose3DTO pose;
  SceneFrameRefDTO frame_ref;
  /// Backend-neutral object-to-object support declarations. The scene owner
  /// resolves these portable resource ids to instance-scoped collision bodies;
  /// callers never author planner-native body names here.
  std::vector<SupportContactDTO> support_contacts{};
};

struct RemoveObjectRequestDTO final {
  SceneResourceId scene_resource_id;
};

/// Public or trusted-provider pose update. Provider evidence is one atomic
/// optional record, preventing partially attributed observations.
struct UpdateObjectPoseRequestDTO final {
  SceneResourceId scene_resource_id;
  Pose3DTO pose;
  SceneFrameRefDTO frame_ref;
  std::optional<SceneObjectProviderEvidenceDTO> provider_evidence;
  std::optional<bool> expected_attached;
};

struct AttachCarriedObjectRequestDTO final {
  CarriedObjectAttachmentId carried_object_attachment_id;
  SceneResourceId scene_resource_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  FrameRoleId frame_role_id;
  std::vector<CollisionBodyRoleId> touch_body_roles;
  std::vector<ComponentAttachmentRevision> expected_component_attachment_revisions;
};

struct DetachCarriedObjectRequestDTO final {
  CarriedObjectAttachmentId carried_object_attachment_id;
  CarriedObjectAttachmentRevision expected_carried_object_attachment_revision;
  Pose3DTO pose;
  SceneFrameRefDTO frame_ref;
};

struct SetCollisionPolicyRequestDTO final {
  CollisionPolicySnapshot configured_policy;
  CollisionPolicyRevision expected_configured_policy_revision;
};

using SceneOperationRequestDTO =
    std::variant<AddObjectRequestDTO, RemoveObjectRequestDTO, UpdateObjectPoseRequestDTO,
                 AttachCarriedObjectRequestDTO, DetachCarriedObjectRequestDTO,
                 SetCollisionPolicyRequestDTO>;

struct SceneTransactionRequestDTO final {
  PublicRequestContextDTO request_context;
  RequestAttributionDTO attribution{};
  ProjectionRevisionDTO expected_projection_revision;
  SceneOperationRequestDTO operation;
};

/// Projection-ready add/update/detach value. The object placement is already
/// normalized into the semantic cell frame and contains no authored frame.
struct ResolvedUpsertSceneObjectDTO final {
  SceneObjectSnapshotDTO object;

  friend bool operator==(const ResolvedUpsertSceneObjectDTO&,
                         const ResolvedUpsertSceneObjectDTO&) = default;
};

struct ResolvedRemoveSceneObjectDTO final {
  SceneResourceId scene_resource_id;

  friend bool operator==(const ResolvedRemoveSceneObjectDTO&,
                         const ResolvedRemoveSceneObjectDTO&) = default;
};

struct ResolvedAttachCarriedObjectDTO final {
  SceneObjectSnapshotDTO object;
  CarriedObjectAttachmentDTO attachment;

  friend bool operator==(const ResolvedAttachCarriedObjectDTO&,
                         const ResolvedAttachCarriedObjectDTO&) = default;
};

struct ResolvedDetachCarriedObjectDTO final {
  SceneObjectSnapshotDTO object;
  CarriedObjectAttachmentId removed_attachment_id;
  CarriedObjectAttachmentRevision removed_attachment_revision;

  friend bool operator==(const ResolvedDetachCarriedObjectDTO&,
                         const ResolvedDetachCarriedObjectDTO&) = default;
};

struct ResolvedCollisionPolicyDTO final {
  /// The exact caller-configured policy retained in logical scene truth.
  CollisionPolicySnapshot configured_policy;
  /// The projection-ready policy after owner-derived touch/support allowances.
  CollisionPolicySnapshot effective_policy;

  friend bool operator==(const ResolvedCollisionPolicyDTO&,
                         const ResolvedCollisionPolicyDTO&) = default;
};

using ResolvedSceneOperationDTO =
    std::variant<ResolvedUpsertSceneObjectDTO, ResolvedRemoveSceneObjectDTO,
                 ResolvedAttachCarriedObjectDTO, ResolvedDetachCarriedObjectDTO,
                 ResolvedCollisionPolicyDTO>;

/// Immutable owner-produced transaction passed to native projections.
struct SceneTransactionDTO final {
  RequestId request_id;
  RequestAttributionDTO attribution{};
  std::uint64_t transaction_sequence{};
  ProjectionRevisionDTO prior_projection_revision;
  ProjectionRevisionDTO proposed_projection_revision;
  ContentDigest original_request_digest;
  ResolvedSceneOperationDTO resolved_operation;
  std::optional<ContentDigest> resolution_evidence_digest;

  friend bool operator==(const SceneTransactionDTO&,
                         const SceneTransactionDTO&) = default;
};

/// Exact authoritative input for recovery of one already-selected native
/// projection. The retained product is identity evidence; concrete verified
/// artifact/model handles remain adapter-private.
struct NativeProjectionRebuildDTO final {
  ProjectionNativeRuntimeBindingDTO expected_native_runtime_binding;
  ProjectionRevisionDTO target_projection_revision;
  RetainedProductDTO planning_collision_model;
  std::shared_ptr<const LogicalSceneSnapshotDTO> logical_scene;

  [[nodiscard]] bool valid() const noexcept {
    return expected_native_runtime_binding.valid() &&
           target_projection_revision.valid() &&
           planning_collision_model.product_kind ==
               RetainedProductKind::kPlanningCollisionModel &&
           !planning_collision_model.product_id.empty() &&
           !planning_collision_model.product_contract_id.empty() &&
           !planning_collision_model.product_contract_revision.empty() &&
           logical_scene != nullptr && logical_scene->locally_valid() &&
           logical_scene->revision ==
               target_projection_revision.scene_revision;
  }
};

enum class SceneUpdateStatusDTO {
  kGeometryChanged,
  kFreshnessOnly,
  kNoOp,
};

[[nodiscard]] constexpr std::string_view to_string(SceneUpdateStatusDTO status) noexcept {
  switch (status) {
    case SceneUpdateStatusDTO::kGeometryChanged:
      return "geometry_changed";
    case SceneUpdateStatusDTO::kFreshnessOnly:
      return "freshness_only";
    case SceneUpdateStatusDTO::kNoOp:
      return "no_op";
  }
  return {};
}

enum class SceneProjectionCommitStatusDTO {
  kNotAttempted,
  kCommitted,
  kRejectedRollbackProven,
  kFailedProjectionUncertain,
};

[[nodiscard]] constexpr std::string_view to_string(SceneProjectionCommitStatusDTO status) noexcept {
  switch (status) {
    case SceneProjectionCommitStatusDTO::kNotAttempted:
      return "not_attempted";
    case SceneProjectionCommitStatusDTO::kCommitted:
      return "committed";
    case SceneProjectionCommitStatusDTO::kRejectedRollbackProven:
      return "rejected_rollback_proven";
    case SceneProjectionCommitStatusDTO::kFailedProjectionUncertain:
      return "failed_projection_uncertain";
  }
  return {};
}

enum class ProjectionCommitResultStateDTO {
  kCommitted,
  kRejectedRollbackProven,
  kFailedProjectionUncertain,
};

/// One native projection's exact acknowledgement or rollback evidence.
struct ProjectionCommitResultDTO final {
  std::optional<std::uint64_t> transaction_sequence;
  ProjectionRevisionDTO proposed_projection_revision;
  ProjectionCommitResultStateDTO result;
  std::optional<ProjectionRevisionDTO> acknowledged_projection_revision;
  std::optional<std::uint64_t> native_projection_generation;
  bool rollback_proven{};
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept;
};

struct SceneCommitResultDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  bool success{};
  std::optional<ErrorDTO> error;
  std::optional<std::uint64_t> transaction_sequence;
  std::optional<ProjectionRevisionDTO> committed_projection_revision;
  SceneProjectionCommitStatusDTO projection_status;
  std::optional<SceneUpdateStatusDTO> update_status;

  [[nodiscard]] bool presence_valid() const noexcept;
};

struct SceneObjectStateDTO final {
  SceneResourceId scene_resource_id;
  bool exists{};
  bool attached{};
  std::optional<CarriedObjectAttachmentId> carried_object_attachment_id;
  std::optional<SceneObjectProviderEvidenceDTO> provider_evidence;
};

inline bool ProjectionCommitResultDTO::presence_valid() const noexcept {
  if (!proposed_projection_revision.valid() || !resulting_health.valid() ||
      (transaction_sequence.has_value() && *transaction_sequence == 0U) ||
      (acknowledged_projection_revision.has_value() &&
       !acknowledged_projection_revision->valid()) ||
      (native_projection_generation.has_value() && *native_projection_generation == 0U)) {
    return false;
  }
  switch (result) {
    case ProjectionCommitResultStateDTO::kCommitted:
      return acknowledged_projection_revision.has_value() &&
             *acknowledged_projection_revision == proposed_projection_revision &&
             !rollback_proven && !error.has_value() &&
             resulting_health.healthy_for_target(proposed_projection_revision);
    case ProjectionCommitResultStateDTO::kRejectedRollbackProven:
      return rollback_proven && error.has_value();
    case ProjectionCommitResultStateDTO::kFailedProjectionUncertain:
      return !rollback_proven && error.has_value() &&
             (resulting_health.state == ProjectionHealthState::kDesynchronized ||
              resulting_health.state == ProjectionHealthState::kUnavailable);
  }
  return false;
}

inline bool SceneCommitResultDTO::presence_valid() const noexcept {
  if (request_id.empty()) {
    return false;
  }
  if (success) {
    return IsAcceptedRequestReplayStatus(replay_status) && !error.has_value() &&
           transaction_sequence.has_value() && *transaction_sequence != 0U &&
           committed_projection_revision.has_value() && committed_projection_revision->valid() &&
           projection_status == SceneProjectionCommitStatusDTO::kCommitted &&
           update_status.has_value();
  }
  return error.has_value() && !transaction_sequence.has_value() &&
         !committed_projection_revision.has_value() && !update_status.has_value() &&
         projection_status != SceneProjectionCommitStatusDTO::kCommitted;
}

}  // namespace manyforge::core
