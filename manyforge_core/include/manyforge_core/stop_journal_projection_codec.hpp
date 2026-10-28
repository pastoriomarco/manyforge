#pragma once

#include <manyforge_core/ports/ioperation_journal.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {

// Lean Stop record (operation journal v17). Only these versions decode; every
// earlier version (acceptance v4-v11, terminal v3, fault clear v2) is refused
// with "... API version is unsupported", never normalized or converted.
inline constexpr std::string_view kStopJournalAcceptanceProjectionApiVersion =
    "manyforge.internal.stop-journal-acceptance-projection.v12";
inline constexpr std::string_view kStopJournalTerminalProjectionApiVersion =
    "manyforge.internal.stop-journal-terminal-projection.v4";
inline constexpr std::string_view kStopJournalSurveillanceProjectionApiVersion =
    "manyforge.internal.stop-journal-surveillance-projection.v1";
inline constexpr std::string_view kStopJournalFaultClearProjectionApiVersion =
    "manyforge.internal.stop-journal-fault-clear-projection.v3";

/// Error code returned by EncodeStopJournalTerminalProjection when two attempts
/// name one participant-plan digest but carry unequal plans. The writer is
/// in-process, so this signals a bug; the terminal projection then fails
/// through the existing persistence-degraded path. Every other codec failure
/// keeps the code "invalid_stop_journal_projection".
inline constexpr std::string_view kStopJournalProjectionPlanConflictCode =
    "stop_journal_projection_plan_conflict";

/// Positional `accepted_payload` of the `stop_projections` index row:
///   ["...acceptance-projection.v12", accepted_value(accepted)]
/// The affected-state capture is not repeated here; it stays only in the
/// `stop_acceptance_projected` journal row (history payload v8, unchanged).
/// Recovery of an unterminated Stop and the joined-receipt scope check need
/// only `accepted`.
struct StopJournalAcceptanceProjectionDTO final {
  StopAcceptedDTO accepted;

  friend bool operator==(const StopJournalAcceptanceProjectionDTO&,
                         const StopJournalAcceptanceProjectionDTO&) = default;
};

/// Terminal projection; the in-memory DTO is unchanged and decoding rebuilds
/// it completely. Wire v4 (the `stop_terminal_projected` journal row, the
/// only stored copy):
///   [ version,
///     status_rest = [accepted, current_stages[], acceptance_projection,
///                    terminal_projection, surveillance_projection,
///                    surveillance|null],
///     certificate = 23 fixed slots: the v3 certificate slots 0-11 and 13-22
///                   (slot 12, evidence_items, is omitted) followed by
///                   lane_hard_stop_requested[] (always present, may be []);
///                   inside each attempt, resolved participant stage slot 26
///                   (participant_stop_plan) holds the plan's
///                   participant_stop_invocation_plan_digest string,
///     fault_latch|null,
///     plans = [prepared_plan_value...] sorted by digest, unique ]
/// Decode: each attempt's plan comes from `plans` by digest; the certificate
/// evidence comes from DeriveStopCertificateEvidence; the status comes from
/// TerminalStopStatusFromCertificate; then stop_terminal_projection_field_error
/// and the canonical re-encode check run. Every referenced digest must be in
/// `plans` and every plan must be referenced. Plan digests are not
/// recomputed. Encode runs stop_terminal_projection_field_error and refuses
/// unequal plans under one digest (kStopJournalProjectionPlanConflictCode).
/// Referent re-verification of embedded participant plans/evidence remains a
/// separate owner-adoption gate; decoding is never renewed qualification.
struct StopJournalTerminalProjectionDTO final {
  StopStatusDTO stop_status;
  StopCertificateDTO certificate;
  std::optional<StopFaultLatchDTO> fault_latch;

  friend bool operator==(const StopJournalTerminalProjectionDTO&,
                         const StopJournalTerminalProjectionDTO&) = default;
};

/// Closed structural payload for the latest durable surveillance checkpoint
/// and its exact current non-cleared latch. Wire v1, unchanged.
struct StopJournalSurveillanceProjectionDTO final {
  StopPostTerminalSurveillanceDTO checkpoint;
  StopFaultLatchDTO fault_latch;

  friend bool operator==(const StopJournalSurveillanceProjectionDTO&,
                         const StopJournalSurveillanceProjectionDTO&) = default;
};

/// Lean durable fault-clear record. Wire v3:
///   ["...fault-clear-projection.v3", recovery_intent, recovery_result]
/// The resulting status and certificate are not stored: the writer still
/// derives and checks the exact successor status in-process, and recovery
/// takes the certificate from the terminal row, the latch from
/// recovery_result.latch, and the surveillance from the latest checkpoint
/// (else the terminal status).
struct StopJournalFaultClearProjectionDTO final {
  StopFaultRecoveryIntentDTO recovery_intent;
  StopFaultRecoveryResultDTO recovery_result;

  friend bool operator==(const StopJournalFaultClearProjectionDTO&,
                         const StopJournalFaultClearProjectionDTO&) = default;
};

/// Entry points: names and signatures are unchanged from v16. Each decoder
/// accepts only canonical bytes of its current version.
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
