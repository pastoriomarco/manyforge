#include "manyforge_component_manager/apply_completion.hpp"

namespace manyforge::component_manager {

bool ManagerApplyFenceReleaseIntentDTO::valid() const noexcept {
  return !scene_finalized_digest.empty() && !fence_acceptance_digest.empty() &&
         release_request.valid();
}

bool ManagerApplyFenceReleasedDTO::valid() const noexcept {
  return !fence_release_intent_digest.empty() && released_fence.valid();
}

bool ManagerApplyTerminalOutcomeDTO::valid() const noexcept {
  return !apply_id.empty() && apply_generation.nonzero() && candidate_projection_revision.valid() &&
         !publication_receipt_digest.empty() && !scene_finalized_digest.empty() &&
         !fence_released_digest.empty() && state == ManagerApplyTerminalOutcomeState::kSucceeded;
}

const char* ToString(const ManagerApplyTerminalOutcomeState state) noexcept {
  switch (state) {
    case ManagerApplyTerminalOutcomeState::kSucceeded:
      return "succeeded";
  }
  return "unknown";
}

}  // namespace manyforge::component_manager
