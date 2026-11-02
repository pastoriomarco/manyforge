#pragma once

#include <cmath>
#include <manyforge_core/types/contact_policy.hpp>

namespace manyforge::core {

// The portable single-zone profile admits grasp and release, using the same
// envelopes as Core validation. Bounded penetration is not admitted here.
inline bool valid_program_contact_envelope(const ContactZoneEnvelopeDTO& envelope) noexcept {
  if (const auto* grasp = std::get_if<ContactGraspEnvelopeDTO>(&envelope))
    return std::isfinite(grasp->max_penetration_m) && grasp->max_penetration_m > 0.0 &&
           std::isfinite(grasp->terminal_window_start_fraction) &&
           grasp->terminal_window_start_fraction >= 0.0 &&
           grasp->terminal_window_start_fraction <= 1.0;
  const auto* release = std::get_if<ContactReleaseEnvelopeDTO>(&envelope);
  if (!release) return false;
  const auto metric = [](double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1000.0;
  };
  if (!metric(release->max_start_penetration_m) || !metric(release->min_release_progress_m) ||
      !metric(release->release_tolerance_m)) return false;
  if (release->release_evidence_mode == ContactReleaseEvidenceModeDTO::kSignedPenetration)
    return true;
  return release->release_evidence_mode == ContactReleaseEvidenceModeDTO::kBinaryContact &&
         !release->enforce_max_start_penetration && release->max_start_penetration_m == 0.0 &&
         release->min_release_progress_m == 0.0 && release->release_tolerance_m == 0.0 &&
         !release->require_strictly_decreasing_penetration;
}

}  // namespace manyforge::core
