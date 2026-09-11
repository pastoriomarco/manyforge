#include "manyforge_component_manager/scene_staging.hpp"

#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::component_manager {
namespace {

[[nodiscard]] bool
SameError(const std::optional<manyforge::core::ErrorDTO> &left,
          const std::optional<manyforge::core::ErrorDTO> &right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left.has_value() ||
         (left->code == right->code && left->message == right->message &&
          left->details == right->details);
}

[[nodiscard]] bool
SameHealth(const manyforge::core::ProjectionHealthDTO &left,
           const manyforge::core::ProjectionHealthDTO &right) {
  return left.native_runtime_binding == right.native_runtime_binding &&
         left.source_sequence == right.source_sequence &&
         left.state == right.state &&
         left.active_projection_revision == right.active_projection_revision &&
         left.staged_candidate == right.staged_candidate &&
         left.native_projection_generation ==
             right.native_projection_generation &&
         left.projection_content_digest == right.projection_content_digest &&
         SameError(left.error, right.error);
}

[[nodiscard]] bool SameTransitionEvidence(
    const manyforge::core::ProjectionTransitionEvidenceDTO &left,
    const manyforge::core::ProjectionTransitionEvidenceDTO &right) {
  if (left.apply_id != right.apply_id ||
      left.apply_generation != right.apply_generation ||
      left.projection_stage_generation != right.projection_stage_generation ||
      left.predecessor_projection_revision !=
          right.predecessor_projection_revision ||
      left.candidate_projection_revision !=
          right.candidate_projection_revision ||
      left.state != right.state ||
      left.transition_rows.size() != right.transition_rows.size() ||
      !SameError(left.error, right.error)) {
    return false;
  }
  for (std::size_t index = 0U; index < left.transition_rows.size(); ++index) {
    const auto &lhs = left.transition_rows[index];
    const auto &rhs = right.transition_rows[index];
    if (lhs.transition_row != rhs.transition_row || lhs.state != rhs.state ||
        lhs.acknowledged_staged_candidate_revision !=
            rhs.acknowledged_staged_candidate_revision ||
        lhs.observed_local_active != rhs.observed_local_active ||
        lhs.resulting_health.has_value() != rhs.resulting_health.has_value() ||
        (lhs.resulting_health &&
         !SameHealth(*lhs.resulting_health, *rhs.resulting_health)) ||
        !SameError(lhs.error, rhs.error)) {
      return false;
    }
  }
  return true;
}

} // namespace

bool ManagerApplySceneStageIntentDTO::valid() const noexcept {
  if (apply_id.empty() || !apply_generation.nonzero() ||
      !manyforge::core::IsExactLowercaseSha256Digest(
          runtime_staged_ready_digest.value()) ||
      !predecessor_publication_generation.nonzero() ||
      !manyforge::core::IsExactLowercaseSha256Digest(
          predecessor_public_pointer_digest.value()) ||
      !expected_active_projection_revision.valid() ||
      candidate_retained_bundle_id.empty() ||
      candidate_graph_revision.empty() ||
      native_transition_rows.size() > kMaximumManagerApplySceneTransitionRows ||
      !manyforge::core::CanonicalProjectionTransitionRows(
          native_transition_rows)) {
    return false;
  }
  for (const auto &row : native_transition_rows) {
    if (!row.matches_predecessor(expected_active_projection_revision) ||
        (row.managed_withdrawal &&
         (row.managed_withdrawal->apply_id != apply_id ||
          row.managed_withdrawal->apply_generation != apply_generation))) {
      return false;
    }
  }
  return true;
}

bool ManagerApplySceneStagedDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             scene_stage_intent_digest.value()) &&
         staged_result.state ==
             manyforge::core::ProjectionApplyStageResultState::kStaged &&
         staged_result.presence_valid();
}

bool ManagerApplyActivationSelectionDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             scene_staged_digest.value()) &&
         !apply_id.empty() && apply_generation.nonzero() &&
         predecessor_projection_revision.valid() &&
         projection_stage_generation.nonzero() &&
         candidate_projection_revision.valid();
}

bool ManagerApplySceneActivatedDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             activation_selection_digest.value()) &&
         activation_result.state ==
             manyforge::core::ProjectionApplyActivationResultState::
                 kActivated &&
         activation_result.presence_valid();
}

bool SameManagerApplySceneStaged(
    const ManagerApplySceneStagedDTO &left,
    const ManagerApplySceneStagedDTO &right) noexcept {
  const auto &lhs = left.staged_result;
  const auto &rhs = right.staged_result;
  return left.scene_stage_intent_digest == right.scene_stage_intent_digest &&
         lhs.apply_id == rhs.apply_id &&
         lhs.apply_generation == rhs.apply_generation &&
         lhs.expected_active_projection_revision ==
             rhs.expected_active_projection_revision &&
         lhs.candidate_projection_revision ==
             rhs.candidate_projection_revision &&
         lhs.projection_stage_generation == rhs.projection_stage_generation &&
         lhs.state == rhs.state && SameError(lhs.error, rhs.error) &&
         lhs.transition_evidence.has_value() ==
             rhs.transition_evidence.has_value() &&
         (!lhs.transition_evidence.has_value() ||
          SameTransitionEvidence(*lhs.transition_evidence,
                                 *rhs.transition_evidence));
}

bool SameManagerApplySceneActivated(
    const ManagerApplySceneActivatedDTO &left,
    const ManagerApplySceneActivatedDTO &right) noexcept {
  const auto &lhs = left.activation_result;
  const auto &rhs = right.activation_result;
  return left.activation_selection_digest ==
             right.activation_selection_digest &&
         lhs.apply_id == rhs.apply_id &&
         lhs.apply_generation == rhs.apply_generation &&
         lhs.expected_active_projection_revision ==
             rhs.expected_active_projection_revision &&
         lhs.projection_stage_generation == rhs.projection_stage_generation &&
         lhs.candidate_projection_revision ==
             rhs.candidate_projection_revision &&
         lhs.state == rhs.state &&
         lhs.acknowledged_candidate_projection_revision ==
             rhs.acknowledged_candidate_projection_revision &&
         SameTransitionEvidence(lhs.transition_evidence,
                                rhs.transition_evidence) &&
         SameError(lhs.error, rhs.error);
}

} // namespace manyforge::component_manager
