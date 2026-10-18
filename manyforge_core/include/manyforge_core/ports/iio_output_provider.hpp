#pragma once

#include <manyforge_core/types/io_output.hpp>

namespace manyforge::core {

/// Thin, transport-neutral output boundary subordinate to the existing
/// prepared command session. No command admission, resource ledger, settlement
/// owner, retry, physical cancellation or readback policy lives here.
class IIoOutputProvider {
 public:
  virtual ~IIoOutputProvider() = default;

  /// Installed transport capability, never an authored completion criterion.
  /// The closed values are kSourceAccepted for an acknowledged source and
  /// kSubmitted for a publish-only source. It makes no physical-effect claim.
  [[nodiscard]] virtual IoOutputEvidence StrongestOutputEvidence() const noexcept = 0;

  /// Submit at most one transport request for the exact committed identity.
  /// A possible send must be represented as submitted/unknown evidence rather
  /// than a refusal claiming that no command reached the source.
  [[nodiscard]] virtual Result<IoOutputObservationDTO> SubmitOutput(
      const IoOutputWriteRequestDTO& request) = 0;

  /// Read retained correlation evidence only. Observation neither resends nor
  /// advances a timeout; the existing command owner enforces its budget.
  [[nodiscard]] virtual Result<IoOutputObservationDTO> ObserveOutputResult(
      const IoOutputCommandIdentityDTO& identity) const = 0;

  /// A grouped publisher must exclude this exact current command's channel
  /// before the owner exposes a retained uncertain result. Serialize against
  /// every shared group writer, keep factual source evidence unchanged, and
  /// retain exclusion until authorized retirement. No cancellation or retry.
  /// Providers without grouped publication have no model to exclude.
  [[nodiscard]] virtual Result<void> ExcludeFromGroupModel(const IoOutputCommandIdentityDTO&) {
    return Result<void>::Ok();
  }

  /// Called only when the existing session owner authorizes exact retirement.
  /// Fence late replies and release local correlation; this is not remote
  /// cancellation, undo, or evidence that a submitted write had no effect.
  [[nodiscard]] virtual Result<void> RetireOutput(const IoOutputCommandIdentityDTO& identity) = 0;
};

}  // namespace manyforge::core
