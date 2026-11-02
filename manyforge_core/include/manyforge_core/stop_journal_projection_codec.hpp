#pragma once

#include <manyforge_core/ports/ioperation_journal.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {

// Lean Stop record (operation journal v18). Only these versions decode; every
// earlier version (acceptance v4-v11, terminal v3-v4) is refused with
// "... API version is unsupported", never normalized or converted.
inline constexpr std::string_view kStopJournalAcceptanceProjectionApiVersion =
    "manyforge.internal.stop-journal-acceptance-projection.v12";
inline constexpr std::string_view kStopJournalTerminalProjectionApiVersion =
    "manyforge.internal.stop-journal-terminal-projection.v5";

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
/// it completely. Wire v5 (the `stop_terminal_projected` journal row, the
/// only stored copy):
///   [ version,
///     status_rest = [accepted, current_stages[], acceptance_projection,
///                    terminal_projection],
///     certificate = 21 fixed slots: the certificate fields in declaration
///                   order without evidence_items, ending with
///                   lane_hard_stop_requested[] (always present, may be []);
///                   inside each attempt, resolved participant stage slot 26
///                   (participant_stop_plan) holds the plan's
///                   participant_stop_invocation_plan_digest string,
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

  friend bool operator==(const StopJournalTerminalProjectionDTO&,
                         const StopJournalTerminalProjectionDTO&) = default;
};

/// Entry points. Each decoder accepts only canonical bytes of its current
/// version.
[[nodiscard]] Result<std::string> EncodeStopJournalAcceptanceProjection(
    const StopJournalAcceptanceProjectionDTO& projection);
[[nodiscard]] Result<StopJournalAcceptanceProjectionDTO> DecodeStopJournalAcceptanceProjection(
    std::string_view canonical_bytes);

[[nodiscard]] Result<std::string> EncodeStopJournalTerminalProjection(
    const StopJournalTerminalProjectionDTO& projection);
[[nodiscard]] Result<StopJournalTerminalProjectionDTO> DecodeStopJournalTerminalProjection(
    std::string_view canonical_bytes);

}  // namespace manyforge::core
