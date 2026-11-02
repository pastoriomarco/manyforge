#pragma once

#include <cstdint>
#include <manyforge_core/types/kinematic_state_view.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Attach-time selector whose model occurrence is resolved from the exact
/// retained command target rather than supplied by a public caller.
struct TargetCarrierSceneFrameRefDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  /// Portable command-target frame role. The retained target map may resolve
  /// this to a differently named model-asset frame role.
  FrameRoleId frame_role_id;

  [[nodiscard]] bool valid() const noexcept {
    return !controllable_component_id.empty() && !command_target_id.empty() &&
           !frame_role_id.empty();
  }

  friend bool operator==(const TargetCarrierSceneFrameRefDTO&,
                         const TargetCarrierSceneFrameRefDTO&) = default;
};

using SceneFrameResolutionSelectorDTO =
    std::variant<SceneFrameRefDTO, TargetCarrierSceneFrameRefDTO>;

/// One exact, already-retained native-projection query. The lease is retained
/// by the caller before this request and released after the immutable result is
/// captured.
struct SceneFrameResolutionQueryDTO final {
  SceneFrameResolutionSelectorDTO selector;
  ProjectionLeaseBindingDTO projection_binding;

  [[nodiscard]] bool valid() const noexcept;
};

/// Candidate-only carrier-frame query. The predicted state is the exact
/// terminal state returned by the shared non-dispatching motion primitive; it
/// overrides only its own retained control binding while all other path state
/// remains owner-issued. It is never an observed-state publication.
struct CandidateSceneFrameResolutionQueryDTO final {
  SceneFrameResolutionSelectorDTO selector;
  ProjectionLeaseBindingDTO projection_binding;
  ControlledStateDTO predicted_state;

  [[nodiscard]] bool valid() const noexcept;
};

/// Pure resolver input. The verified compiler map remains adapter-private;
/// its selected terminal identity and path are carried here explicitly.
struct SceneFrameResolutionContextDTO final {
  ProjectionLeaseBindingDTO projection_binding;
  std::shared_ptr<const LogicalSceneSnapshotDTO> logical_scene;
  KinematicFramePathDTO kinematic_path;
  KinematicStatePathViewDTO kinematic_state_path_view;

  [[nodiscard]] bool valid() const noexcept;
};

/// Complete immutable scene-frame answer captured before scene journaling.
struct ResolvedSceneFramePoseDTO final {
  ResolvedFrameDTO resolved_frame;
  Pose3DTO frame_pose_in_cell;
  /// Exact structural attachment revisions crossed by the selected retained
  /// path. Attach callers compare this canonical set with their precondition.
  std::vector<ComponentAttachmentRevision> component_attachment_revisions;
  ProjectionLeaseBindingDTO projection_binding;
  KinematicFramePathDTO kinematic_path;
  KinematicStatePathViewDTO kinematic_state_path_view;
  /// Present only when one exact candidate terminal state replaced its own
  /// retained control-binding rows during FK. The digest is evidence of a
  /// prediction and must not be interpreted as measured runtime truth.
  std::optional<ContentDigest> predicted_state_digest;
  ContentDigest resolution_evidence_digest;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ResolvedSceneFramePoseDTO&,
                         const ResolvedSceneFramePoseDTO&) = default;
};

/// Transport-neutral receipt returned by one native projection participant.
/// The complete compiler path and immutable state view remain inside the
/// native resolver; their deterministic evidence digest makes this compact
/// value indivisible without exporting adapter-private state.
struct SceneFrameResolutionReceiptDTO final {
  ResolvedFrameDTO resolved_frame;
  Pose3DTO frame_pose_in_cell;
  std::vector<ComponentAttachmentRevision> component_attachment_revisions;
  ProjectionLeaseBindingDTO projection_binding;
  std::optional<ContentDigest> predicted_state_digest;
  ContentDigest resolution_evidence_digest;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const SceneFrameResolutionReceiptDTO&,
                         const SceneFrameResolutionReceiptDTO&) = default;
};

[[nodiscard]] SceneFrameResolutionReceiptDTO MakeSceneFrameResolutionReceipt(
    const ResolvedSceneFramePoseDTO& resolution);

/// Qualified deterministic evidence over the complete immutable resolution
/// result except for its own digest field.
[[nodiscard]] ContentDigest ComputeSceneFrameResolutionEvidenceDigest(
    const ResolvedSceneFramePoseDTO& resolution);

/// Deterministic identity for one exact typed candidate state. This is used
/// only as evidence inside candidate frame receipts; it does not publish or
/// confer observed-state authority.
[[nodiscard]] ContentDigest ComputeCandidateStateEvidenceDigest(
    const ControlledStateDTO& state);

}  // namespace manyforge::core
