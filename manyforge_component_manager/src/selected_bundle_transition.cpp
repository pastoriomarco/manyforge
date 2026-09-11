#include "manyforge_component_manager/selected_bundle_transition.hpp"
#include "manyforge_component_manager/selected_runtime_composition.hpp"

#include <algorithm>
#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::component_manager {

bool ManagerSelectedRuntimeCompositionRequestDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             scene_activated_digest.value()) &&
         !apply_id.empty() && apply_generation.nonzero() &&
         projection_stage_generation.nonzero() &&
         candidate_projection_revision.valid() &&
         expected_predecessor_publication_generation.nonzero() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             expected_predecessor_public_pointer_digest.value()) &&
         manyforge::core::IsExactLowercaseSha256Digest(
             retained_bundle_canonical_content_digest.value()) &&
         retained_bundle_canonical_byte_size != 0U &&
         !runtime_unit_id.empty() && !runtime_epoch.empty() &&
         !manager_runtime_epoch.empty() && !configuration_fence_id.empty() &&
         configuration_fence_generation.nonzero() &&
         configuration_fence_scope.valid();
}

bool ManagerSelectedRuntimeCompositionResultDTO::valid() const noexcept {
  return request.valid() &&
         (state == ManagerSelectedRuntimeCompositionState::kPreparing ||
          state == ManagerSelectedRuntimeCompositionState::kInstalled);
}

namespace {

[[nodiscard]] bool
SameError(const std::optional<manyforge::core::ErrorDTO> &left,
          const std::optional<manyforge::core::ErrorDTO> &right) noexcept {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left.has_value() ||
         (left->code == right->code && left->message == right->message &&
          left->details == right->details);
}

[[nodiscard]] bool
SameHealth(const manyforge::core::ProjectionHealthDTO &left,
           const manyforge::core::ProjectionHealthDTO &right) noexcept {
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
    const manyforge::core::ProjectionTransitionEvidenceDTO &right) noexcept {
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

[[nodiscard]] bool SameAggregateHealth(
    const manyforge::core::ProjectionAggregateHealthDTO &left,
    const manyforge::core::ProjectionAggregateHealthDTO &right) noexcept {
  if (left.required_native_set_source != right.required_native_set_source ||
      left.state != right.state ||
      left.native_projection_health.size() !=
          right.native_projection_health.size() ||
      !SameError(left.error, right.error)) {
    return false;
  }
  for (std::size_t index = 0U; index < left.native_projection_health.size();
       ++index) {
    if (!SameHealth(left.native_projection_health[index],
                    right.native_projection_health[index])) {
      return false;
    }
  }
  return true;
}

} // namespace

bool ManagerApplyPrivateMaterializationDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             scene_activated_digest.value()) &&
         !apply_id.empty() && apply_generation.nonzero() &&
         projection_stage_generation.nonzero() &&
         candidate_projection_revision.valid() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             retained_bundle_canonical_content_digest.value()) &&
         retained_bundle_canonical_byte_size > 0U;
}

const char *
ToString(const ObsoleteManagedCommandSurfaceDisposition value) noexcept {
  switch (value) {
  case ObsoleteManagedCommandSurfaceDisposition::kRetired:
    return "retired";
  case ObsoleteManagedCommandSurfaceDisposition::kAlreadyAbsent:
    return "already_absent";
  case ObsoleteManagedCommandSurfaceDisposition::kNonselectable:
    return "nonselectable";
  }
  return "";
}

bool ObsoleteManagedCommandSurfaceEvidenceDTO::valid() const noexcept {
  return !runtime_unit_id.empty() && !runtime_epoch.empty() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             product_identity.value()) &&
         manyforge::core::IsExactLowercaseSha256Digest(
             configuration_identity.value()) &&
         source_report_sequence > 0U && ToString(disposition)[0] != '\0';
}

bool ManagerApplyObsoleteCommandSurfacesRetiredDTO::valid() const noexcept {
  if (!manyforge::core::IsExactLowercaseSha256Digest(
          private_materialization_content_digest.value()) ||
      apply_id.empty() || !apply_generation.nonzero() ||
      command_surfaces.size() > kMaximumObsoleteManagedCommandSurfaces ||
      !std::all_of(command_surfaces.begin(), command_surfaces.end(),
                   [](const auto &row) { return row.valid(); })) {
    return false;
  }
  return std::is_sorted(command_surfaces.begin(), command_surfaces.end(),
                        [](const auto &left, const auto &right) {
                          return left.runtime_unit_id < right.runtime_unit_id;
                        }) &&
         std::adjacent_find(command_surfaces.begin(), command_surfaces.end(),
                            [](const auto &left, const auto &right) {
                              return left.runtime_unit_id ==
                                     right.runtime_unit_id;
                            }) == command_surfaces.end();
}

bool ManagerApplyPublicationReceiptDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             obsolete_managed_command_surfaces_retired_evidence_digest
                 .value()) &&
         publication_receipt.valid() &&
         publication_receipt.request
                 .obsolete_managed_command_surfaces_retired_evidence_digest ==
             obsolete_managed_command_surfaces_retired_evidence_digest;
}

bool ManagerApplySceneFinalizedDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             publication_receipt_digest.value()) &&
         finalized_result.state ==
             manyforge::core::ProjectionApplyFinalizeResultState::kFinalized &&
         finalized_result.presence_valid();
}

bool SameManagerApplySceneFinalized(
    const ManagerApplySceneFinalizedDTO &left,
    const ManagerApplySceneFinalizedDTO &right) noexcept {
  const auto &lhs = left.finalized_result;
  const auto &rhs = right.finalized_result;
  return left.publication_receipt_digest == right.publication_receipt_digest &&
         lhs.apply_id == rhs.apply_id &&
         lhs.apply_generation == rhs.apply_generation &&
         lhs.projection_stage_generation == rhs.projection_stage_generation &&
         lhs.candidate_projection_revision ==
             rhs.candidate_projection_revision &&
         lhs.publication_receipt == rhs.publication_receipt &&
         lhs.state == rhs.state &&
         lhs.acknowledged_candidate_projection_revision ==
             rhs.acknowledged_candidate_projection_revision &&
         SameTransitionEvidence(lhs.transition_evidence,
                                rhs.transition_evidence) &&
         SameAggregateHealth(lhs.candidate_aggregate_health,
                             rhs.candidate_aggregate_health) &&
         SameError(lhs.error, rhs.error);
}

} // namespace manyforge::component_manager
