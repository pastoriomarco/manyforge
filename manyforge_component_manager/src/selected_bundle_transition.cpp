#include "manyforge_component_manager/selected_bundle_transition.hpp"
#include "manyforge_component_manager/selected_runtime_composition.hpp"

#include <algorithm>
#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::component_manager {

bool ManagerSelectedRuntimeCompositionRequestDTO::valid() const noexcept {
  return (phase == ManagerSelectedRuntimeCompositionPhase::kPrepare ||
          phase == ManagerSelectedRuntimeCompositionPhase::kFinalize) &&
         manyforge::core::IsExactLowercaseSha256Digest(
             authoritative_scene_snapshot_digest.value()) &&
         !apply_id.empty() && apply_generation.nonzero() &&
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
         configuration_fence_scope.valid() && component_delta.valid() &&
         manyforge::deployment::apply_component_delta_is_fenced(
             component_delta, configuration_fence_scope) &&
         predecessor_projection_revision.valid();
}

bool ManagerRuntimeComponentReceiptDTO::valid() const noexcept {
  using manyforge::deployment::ApplyComponentDisposition;
  const auto epochs_sorted = std::adjacent_find(
      runtime_unit_epochs.begin(), runtime_unit_epochs.end(),
      [](const auto &lhs, const auto &rhs) {
        return !(lhs.runtime_unit_id < rhs.runtime_unit_id);
      }) == runtime_unit_epochs.end();
  return !controllable_component_id.empty() &&
         disposition != ApplyComponentDisposition::kRemove &&
         installed_runtime_identity.has_value() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             installed_runtime_identity->value()) &&
         epochs_sorted &&
         std::all_of(runtime_unit_epochs.begin(), runtime_unit_epochs.end(),
                     [](const auto &epoch) {
                       return !epoch.runtime_unit_id.empty() &&
                              !epoch.runtime_epoch.empty();
                     }) &&
         predecessor_runtime_retained ==
             (disposition == ApplyComponentDisposition::kRetain) &&
         (disposition == ApplyComponentDisposition::kRetain ||
          !product_binding_resealed);
}

bool ComponentReceiptsMatchRequest(
    const ManagerSelectedRuntimeCompositionRequestDTO &request,
    const std::vector<ManagerRuntimeComponentReceiptDTO> &receipts) noexcept {
  using manyforge::deployment::ApplyComponentDisposition;
  const auto &delta = request.component_delta;
  if (!request.valid()) return false;
  // Bindings are sealed to one bundle and graph revision; a retained
  // controller is re-sealed exactly when that product identity changes.
  const bool product_changed =
      request.predecessor_projection_revision.retained_bundle_id !=
          request.candidate_projection_revision.retained_bundle_id ||
      request.predecessor_projection_revision.graph_revision !=
          request.candidate_projection_revision.graph_revision;
  std::size_t next = 0U;
  for (const auto &change : delta.components) {
    if (change.disposition == ApplyComponentDisposition::kRemove) continue;
    if (next == receipts.size()) return false;
    const auto &receipt = receipts[next++];
    if (!receipt.valid() ||
        receipt.controllable_component_id != change.controllable_component_id ||
        receipt.disposition != change.disposition ||
        receipt.installed_runtime_identity != change.candidate_runtime_identity ||
        // The sole managed native host owns every component runtime (N7):
        // each receipt names exactly that runtime unit at the request's
        // epoch, never a stale, foreign or additional owner.
        receipt.runtime_unit_epochs !=
            std::vector<manyforge::core::RuntimeUnitEpochBindingDTO>{
                {request.runtime_unit_id, request.runtime_epoch}} ||
        (change.disposition == ApplyComponentDisposition::kRetain &&
         receipt.product_binding_resealed != product_changed)) {
      return false;
    }
  }
  return next == receipts.size();
}

bool ManagerSelectedRuntimeCompositionResultDTO::valid() const noexcept {
  return request.valid() &&
         (state == ManagerSelectedRuntimeCompositionState::kPreparing ||
          state == ManagerSelectedRuntimeCompositionState::kInstalled) &&
         (state == ManagerSelectedRuntimeCompositionState::kInstalled ||
          component_receipts.empty()) &&
         std::all_of(component_receipts.begin(), component_receipts.end(),
                     [](const auto &receipt) { return receipt.valid(); }) &&
         std::adjacent_find(component_receipts.begin(), component_receipts.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return !(lhs.controllable_component_id <
                                       rhs.controllable_component_id);
                            }) == component_receipts.end();
}

bool ManagerApplyPrivateMaterializationDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             authoritative_scene_snapshot_digest.value()) &&
         !apply_id.empty() && apply_generation.nonzero() &&
         candidate_projection_revision.valid() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             retained_bundle_canonical_content_digest.value()) &&
         retained_bundle_canonical_byte_size > 0U;
}

bool ManagerApplyPublicationReceiptDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             private_materialization_content_digest.value()) &&
         publication_receipt.valid() &&
         publication_receipt.request
                 .private_materialization_content_digest ==
             private_materialization_content_digest;
}

} // namespace manyforge::component_manager
