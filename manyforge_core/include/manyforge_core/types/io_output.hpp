#pragma once

#include <manyforge_core/types/addressed_command_identity.hpp>
#include <manyforge_core/types/lifecycle_provider_occurrence.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <optional>
#include <string_view>

namespace manyforge::core {

/// Private identity supplied by the existing prepared command session. The
/// provider owns transport correlation only; it cannot mint command authority.
struct IoOutputCommandIdentityDTO final {
  AddressedCommandIdentityDTO command;
  ContentDigest prepared_contract_digest;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ResolvedLifecycleProviderOccurrenceDTO provider;

  friend bool operator==(const IoOutputCommandIdentityDTO&,
                         const IoOutputCommandIdentityDTO&) = default;
};

/// Endpoint and resource selection come from the sealed provider binding.
/// The existing command owner resolves any absolute deadline from the sealed
/// mechanism policy; a provider does not create another deadline policy.
struct IoOutputWriteRequestDTO final {
  IoOutputCommandIdentityDTO identity;
  IoSignalId signal_id;
  bool value;
  /// Absent for immediate synthetic and submitted-only mechanisms. For an
  /// acknowledged mechanism the existing command owner supplies its one
  /// absolute source-acceptance deadline.
  std::optional<TimePoint> absolute_deadline_ns;

  friend bool operator==(const IoOutputWriteRequestDTO&,
                         const IoOutputWriteRequestDTO&) = default;
};

enum class IoOutputEvidence {
  kNotSubmitted,
  kSubmitted,
  kSourceAccepted,
  kSourceRejected,
  kAcceptanceUnknown,
};

[[nodiscard]] constexpr std::string_view to_string(IoOutputEvidence evidence) noexcept {
  switch (evidence) {
    case IoOutputEvidence::kNotSubmitted:
      return "not_submitted";
    case IoOutputEvidence::kSubmitted:
      return "submitted";
    case IoOutputEvidence::kSourceAccepted:
      return "source_accepted";
    case IoOutputEvidence::kSourceRejected:
      return "source_rejected";
    case IoOutputEvidence::kAcceptanceUnknown:
      return "acceptance_unknown";
  }
  return {};
}

/// Evidence for this exact transport request, never physical readback. Source
/// acceptance completes an acknowledged output command without claiming the
/// output changed. Submission is the strongest fact of a publish-only source.
struct IoOutputObservationDTO final {
  IoOutputCommandIdentityDTO identity;
  IoOutputEvidence evidence;
  TimePoint trusted_received_at_ns;
  std::optional<ErrorDTO> error;

  friend bool operator==(const IoOutputObservationDTO&,
                         const IoOutputObservationDTO&) = default;
};

}  // namespace manyforge::core
