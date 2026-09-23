#include "manyforge_component_manager/selected_bundle_transition.hpp"
#include "manyforge_component_manager/selected_runtime_composition.hpp"

#include <algorithm>
#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::component_manager {

bool ManagerSelectedRuntimeCompositionRequestDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
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
         configuration_fence_scope.valid();
}

bool ManagerSelectedRuntimeCompositionResultDTO::valid() const noexcept {
  return request.valid() &&
         (state == ManagerSelectedRuntimeCompositionState::kPreparing ||
          state == ManagerSelectedRuntimeCompositionState::kInstalled);
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
