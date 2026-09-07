#pragma once

#include <manyforge_core/ports/ioperation_journal.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {

inline constexpr std::string_view kStopJournalAcceptanceProjectionApiVersion =
    "manyforge.internal.stop-journal-acceptance-projection.v2";
inline constexpr std::string_view kStopJournalTerminalProjectionApiVersion =
    "manyforge.internal.stop-journal-terminal-projection.v2";
inline constexpr std::string_view kStopJournalSurveillanceProjectionApiVersion =
    "manyforge.internal.stop-journal-surveillance-projection.v1";
inline constexpr std::string_view kStopJournalFaultClearProjectionApiVersion =
    "manyforge.internal.stop-journal-fault-clear-projection.v2";

/// Closed structural payload retained beside a Stop-acceptance journal row.
/// It is deliberately independent of live repositories and process memory so
/// restart recovery can reconstruct the exact accepted and affected truth.
struct StopJournalAcceptanceProjectionDTO final {
  StopAcceptedDTO accepted;
  EmergencyStopAffectedStateSnapshotDTO affected_state;

  friend bool operator==(const StopJournalAcceptanceProjectionDTO&,
                         const StopJournalAcceptanceProjectionDTO&) = default;
};

/// Closed structural payload retained by one atomic terminal projection.
/// Referent re-verification of embedded participant plans/evidence remains a
/// separate owner-adoption gate; this codec never treats decoding as renewed
/// qualification.
struct StopJournalTerminalProjectionDTO final {
  StopStatusDTO stop_status;
  StopCertificateDTO certificate;
  std::optional<StopFaultLatchDTO> fault_latch;

  friend bool operator==(const StopJournalTerminalProjectionDTO&,
                         const StopJournalTerminalProjectionDTO&) = default;
};

/// Closed structural payload for the latest durable surveillance checkpoint
/// and its exact current non-cleared latch.
struct StopJournalSurveillanceProjectionDTO final {
  StopPostTerminalSurveillanceDTO checkpoint;
  StopFaultLatchDTO fault_latch;

  friend bool operator==(const StopJournalSurveillanceProjectionDTO&,
                         const StopJournalSurveillanceProjectionDTO&) = default;
};

/// Self-contained durable fault-clear record. The journal stores the exact
/// revision-fenced public intent and owner-issued result beside the resulting
/// terminal truth; restart adoption never reconstructs a clear from a request
/// ID or from the latest in-memory assessment.
struct StopJournalFaultClearProjectionDTO final {
  StopFaultRecoveryIntentDTO recovery_intent;
  StopFaultRecoveryResultDTO recovery_result;
  StopStatusDTO resulting_stop_status;
  StopCertificateDTO certificate;

  friend bool operator==(const StopJournalFaultClearProjectionDTO&,
                         const StopJournalFaultClearProjectionDTO&) = default;
};

[[nodiscard]] Result<std::string> EncodeStopJournalAcceptanceProjection(
    const StopJournalAcceptanceProjectionDTO& projection);
[[nodiscard]] Result<StopJournalAcceptanceProjectionDTO> DecodeStopJournalAcceptanceProjection(
    std::string_view canonical_bytes);

[[nodiscard]] Result<std::string> EncodeStopJournalTerminalProjection(
    const StopJournalTerminalProjectionDTO& projection);
[[nodiscard]] Result<StopJournalTerminalProjectionDTO> DecodeStopJournalTerminalProjection(
    std::string_view canonical_bytes);

[[nodiscard]] Result<std::string> EncodeStopJournalSurveillanceProjection(
    const StopJournalSurveillanceProjectionDTO& projection);
[[nodiscard]] Result<StopJournalSurveillanceProjectionDTO> DecodeStopJournalSurveillanceProjection(
    std::string_view canonical_bytes);

[[nodiscard]] Result<std::string> EncodeStopJournalFaultClearProjection(
    const StopJournalFaultClearProjectionDTO& projection);
[[nodiscard]] Result<StopJournalFaultClearProjectionDTO> DecodeStopJournalFaultClearProjection(
    std::string_view canonical_bytes);

}  // namespace manyforge::core
