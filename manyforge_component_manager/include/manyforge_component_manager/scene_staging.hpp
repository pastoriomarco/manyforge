#pragma once

#include <cstddef>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/projection_apply.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::size_t kMaximumManagerApplySceneTransitionRows = 256U;

/// Immutable Manager-side named-Apply scene-stage intent.
///
/// The parent staged-ready digest identifies the complete direct runtime
/// evidence. The retained bundles are reloaded from the sealed repository when
/// reconstructing the Core stage request; this record stores only their exact
/// identities and the Manager-derived canonical transition set.
struct ManagerApplySceneStageIntentDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ContentDigest runtime_staged_ready_digest;
  manyforge::core::ActiveBundlePublicationGeneration
      predecessor_publication_generation;
  manyforge::core::ContentDigest predecessor_public_pointer_digest;
  manyforge::core::ProjectionRevisionDTO expected_active_projection_revision;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;
  std::vector<manyforge::core::ProjectionTransitionRowDTO>
      native_transition_rows;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplySceneStageIntentDTO &,
                         const ManagerApplySceneStageIntentDTO &) = default;
};

/// Complete successful scene-owner stage evidence bound to one durable intent.
///
/// Only a `kStaged` Core result is representable here. Quiescing, rejection,
/// absence, and uncertainty remain recovery observations and cannot become a
/// durable success record.
struct ManagerApplySceneStagedDTO final {
  manyforge::core::ContentDigest scene_stage_intent_digest;
  manyforge::core::ProjectionApplyStageResultDTO staged_result;

  [[nodiscard]] bool valid() const noexcept;
};

/// Irreversible Manager-owned semantic selection of one frozen scene stage.
///
/// This record is committed before the first activation/retirement call. Once
/// durable, recovery may only complete this exact transition forward.
struct ManagerApplyActivationSelectionDTO final {
  manyforge::core::ContentDigest scene_staged_digest;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ProjectionRevisionDTO predecessor_projection_revision;
  manyforge::core::ProjectionStageGeneration projection_stage_generation;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyActivationSelectionDTO &,
                         const ManagerApplyActivationSelectionDTO &) = default;
};

/// Complete successful scene-owner activation/retirement evidence bound to
/// the durable semantic selection.
struct ManagerApplySceneActivatedDTO final {
  manyforge::core::ContentDigest activation_selection_digest;
  manyforge::core::ProjectionApplyActivationResultDTO activation_result;

  [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool
SameManagerApplySceneStaged(const ManagerApplySceneStagedDTO &left,
                            const ManagerApplySceneStagedDTO &right) noexcept;

[[nodiscard]] bool SameManagerApplySceneActivated(
    const ManagerApplySceneActivatedDTO &left,
    const ManagerApplySceneActivatedDTO &right) noexcept;

} // namespace manyforge::component_manager
