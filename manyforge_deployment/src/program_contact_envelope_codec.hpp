#pragma once

#include <manyforge_core/types/program_contact_envelope.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <stdexcept>

namespace manyforge::deployment::detail {

inline core::ContactZoneEnvelopeDTO parse_program_contact_envelope(
    const CanonicalValue::Object& fields, bool release, bool normalized) {
  const auto member = [&](std::string_view name) -> const CanonicalValue* {
    for (const auto& [key, value] : fields) if (key == name) return &value;
    return nullptr;
  };
  const auto number = [&](std::string_view name, double fallback) {
    const auto* value = member(name);
    if (!value && !normalized) return fallback;
    if (value) {
      if (const auto* v = std::get_if<double>(&value->storage())) return *v;
    }
    throw std::invalid_argument("contact envelope requires numeric " + std::string(name));
  };
  const auto boolean = [&](std::string_view name, bool fallback) {
    const auto* value = member(name);
    if (!value && !normalized) return fallback;
    if (value) if (const auto* v = std::get_if<bool>(&value->storage())) return *v;
    throw std::invalid_argument("contact envelope requires boolean " + std::string(name));
  };
  core::ContactZoneEnvelopeDTO envelope;
  if (!release) {
    const auto depth = number("max_penetration_m", 0.0);
    const auto start = number("terminal_window_start_fraction", 0.0);
    envelope = core::ContactGraspEnvelopeDTO{depth, start};
  } else {
    const auto* evidence = member("release_evidence_mode");
    const auto* text = evidence ? std::get_if<std::string>(&evidence->storage()) : nullptr;
    if (!text || (*text != "signed_penetration" && *text != "binary_contact"))
      throw std::invalid_argument("contact release evidence mode is invalid");
    const bool binary = *text == "binary_contact";
    core::ContactReleaseEnvelopeDTO value;
    value.release_evidence_mode = binary ? core::ContactReleaseEvidenceModeDTO::kBinaryContact
                                        : core::ContactReleaseEvidenceModeDTO::kSignedPenetration;
    value.enforce_max_start_penetration = boolean("enforce_max_start_penetration", !binary);
    value.max_start_penetration_m = number("max_start_penetration_m", 0.0);
    value.min_release_progress_m = number("min_release_progress_m", 0.0);
    value.release_tolerance_m = number("release_tolerance_m", binary ? 0.0 : 1e-5);
    value.require_strictly_decreasing_penetration = boolean("require_strictly_decreasing_penetration", !binary);
    value.require_separation_by_end = boolean("require_separation_by_end", true);
    value.allow_recontact = boolean("allow_recontact", false);
    envelope = value;
  }
  if (!core::valid_program_contact_envelope(envelope))
    throw std::invalid_argument("contact envelope bounds or mode are invalid");
  return envelope;
}

inline void append_program_contact_envelope(CanonicalValue::Object& fields,
                                            const core::ContactZoneEnvelopeDTO& envelope) {
  if (const auto* grasp = std::get_if<core::ContactGraspEnvelopeDTO>(&envelope)) {
    fields.emplace_back("max_penetration_m", CanonicalValue(grasp->max_penetration_m));
    fields.emplace_back("terminal_window_start_fraction", CanonicalValue(grasp->terminal_window_start_fraction));
    return;
  }
  const auto& release = std::get<core::ContactReleaseEnvelopeDTO>(envelope);
  fields.emplace_back("mode", CanonicalValue("contact_release"));
  fields.emplace_back("release_evidence_mode", CanonicalValue(
      release.release_evidence_mode == core::ContactReleaseEvidenceModeDTO::kBinaryContact
          ? "binary_contact" : "signed_penetration"));
  fields.emplace_back("enforce_max_start_penetration", CanonicalValue(release.enforce_max_start_penetration));
  fields.emplace_back("max_start_penetration_m", CanonicalValue(release.max_start_penetration_m));
  fields.emplace_back("min_release_progress_m", CanonicalValue(release.min_release_progress_m));
  fields.emplace_back("release_tolerance_m", CanonicalValue(release.release_tolerance_m));
  fields.emplace_back("require_strictly_decreasing_penetration", CanonicalValue(release.require_strictly_decreasing_penetration));
  fields.emplace_back("require_separation_by_end", CanonicalValue(release.require_separation_by_end));
  fields.emplace_back("allow_recontact", CanonicalValue(release.allow_recontact));
}
}  // namespace manyforge::deployment::detail
