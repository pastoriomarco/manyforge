#include "manyforge_component_manager/apply_completion.hpp"

namespace manyforge::component_manager {

bool ManagerApplyFenceReleaseIntentDTO::valid() const noexcept {
  return !scene_completion_digest.empty() && !fence_acceptance_digest.empty() &&
         release_request.valid();
}

bool ManagerApplyFenceReleasedDTO::valid() const noexcept {
  return !fence_release_intent_digest.empty() && released_fence.valid();
}

bool ManagerApplyFailureEvidenceDTO::valid() const noexcept {
  return !intent_digest.empty() && !failure_code.empty() &&
         failure_code.size() <= kMaximumManagerApplyFailureCodeBytes &&
         !failure_message.empty() &&
         failure_message.size() <= kMaximumManagerApplyFailureMessageBytes;
}

bool ManagerApplyTerminalOutcomeDTO::valid() const noexcept {
  if (apply_id.empty() || !apply_generation.nonzero()) {
    return false;
  }
  // "No success evidence" means every success-only optional is absent, not
  // merely that the complete set happens to be invalid or incomplete: a stray
  // publication field on a failure would otherwise be silently tolerated.
  const bool has_any_success_evidence =
      candidate_projection_revision.has_value() ||
      publication_receipt_digest.has_value() ||
      scene_completion_digest.has_value() ||
      fence_released_digest.has_value();
  const bool has_complete_success_evidence =
      candidate_projection_revision.has_value() &&
      candidate_projection_revision->valid() &&
      publication_receipt_digest.has_value() &&
      !publication_receipt_digest->empty() && scene_completion_digest.has_value() &&
      !scene_completion_digest->empty() && fence_released_digest.has_value() &&
      !fence_released_digest->empty();
  if (state == ManagerApplyTerminalOutcomeState::kSucceeded) {
    // A success is published exactly once: it carries the Finalize, publication
    // and Kernel-fence-release evidence and no failure.
    return has_complete_success_evidence && !failure.has_value();
  }
  // A failed or unknown-effect outcome never published a configuration. It
  // carries no success evidence at all, retains exactly one valid first failure
  // and states its effect certainty without inferring clean absence.
  if (has_any_success_evidence || !failure.has_value() || !failure->valid()) {
    return false;
  }
  const auto required_disposition =
      state == ManagerApplyTerminalOutcomeState::kFailed
          ? ManagerApplyEffectDisposition::kKnown
          : ManagerApplyEffectDisposition::kUnknown;
  return failure->effect_disposition == required_disposition;
}

const char* ToString(const ManagerApplyTerminalOutcomeState state) noexcept {
  switch (state) {
    case ManagerApplyTerminalOutcomeState::kSucceeded:
      return "succeeded";
    case ManagerApplyTerminalOutcomeState::kFailed:
      return "failed";
    case ManagerApplyTerminalOutcomeState::kUnknownEffect:
      return "unknown_effect";
  }
  return "unknown";
}

const char* ToString(const ManagerApplyEffectDisposition disposition) noexcept {
  switch (disposition) {
    case ManagerApplyEffectDisposition::kKnown:
      return "known";
    case ManagerApplyEffectDisposition::kUnknown:
      return "unknown";
  }
  return "unknown";
}

}  // namespace manyforge::component_manager
