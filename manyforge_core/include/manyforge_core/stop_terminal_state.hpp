#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

enum class StopForensicProjectionStateDTO : std::uint8_t {
  kRecorded = 0,
  kFailed,
};

struct StopForensicProjectionResultDTO final {
  StopForensicProjectionStateDTO state{StopForensicProjectionStateDTO::kFailed};
  std::optional<std::uint64_t> journal_sequence;
  std::optional<ErrorDTO> error;

  friend bool operator==(const StopForensicProjectionResultDTO&,
                         const StopForensicProjectionResultDTO&) = default;
};

enum class StopStatusStateDTO : std::uint8_t {
  kAccepted = 0,
  kStopping,
  kCertifying,
  kSucceeded,
  kPartial,
  kFailed,
};

struct StopCurrentStageDTO final {
  StopScopeEntryKeyDTO scope_key;
  StopEscalationStageId stop_escalation_stage_id;
  StopEscalationStageKind stage_kind{StopEscalationStageKind::kNominal};
  std::size_t stage_index{};
  RemainingStopBudgetDTO remaining_budget;

  friend bool operator==(const StopCurrentStageDTO&, const StopCurrentStageDTO&) = default;
};

struct StopStatusDTO final {
  StopAcceptedDTO accepted;
  StopStatusStateDTO state{StopStatusStateDTO::kAccepted};
  std::vector<StopScopeEntryDTO> scope_entries;
  std::vector<StopCurrentStageDTO> current_stages;
  std::vector<StopAttemptedStageResultDTO> attempted_stage_results;
  std::vector<StopCertificateEvidenceItemDTO> evidence_items;
  std::vector<StopScopeEntryKeyDTO> unsettled;
  std::vector<StopScopeEntryKeyDTO> failed;
  StopDurableProjectionStatusDTO acceptance_projection;
  StopDurableProjectionStatusDTO terminal_projection;
  std::optional<StopCertificateRefDTO> stop_certificate;

  friend bool operator==(const StopStatusDTO&, const StopStatusDTO&) = default;
};

/// The StopStatusDTO fields of a terminal status that its certificate cannot
/// rebuild. The lean terminal record (wire v4 status_rest)
/// stores exactly these, in this order.
struct StopTerminalStatusRestDTO final {
  StopAcceptedDTO accepted;
  std::vector<StopCurrentStageDTO> current_stages;
  StopDurableProjectionStatusDTO acceptance_projection;
  StopDurableProjectionStatusDTO terminal_projection;

  friend bool operator==(const StopTerminalStatusRestDTO&,
                         const StopTerminalStatusRestDTO&) = default;
};

/// Copies the stored-only fields out of a terminal status.
[[nodiscard]] StopTerminalStatusRestDTO TerminalStopStatusRest(const StopStatusDTO& status);

/// Rebuilds the full terminal status: `rest` fields verbatim; state from
/// certificate.result (succeeded nominal/escalated -> kSucceeded, partial ->
/// kPartial, failed uncontained -> kFailed); scope_entries,
/// attempted_stage_results, evidence_items, unsettled and failed from the
/// certificate; stop_certificate = {id, revision, stop_id, stop_generation} of
/// the certificate. Pure; no validation (callers run
/// stop_terminal_projection_field_error). For any status S that passes that
/// validator with certificate C:
///   TerminalStopStatusFromCertificate(TerminalStopStatusRest(S), C) == S.
/// The same rule as the Kernel recovery adoption, which may keep its own copy.
[[nodiscard]] StopStatusDTO TerminalStopStatusFromCertificate(
    const StopTerminalStatusRestDTO& rest, const StopCertificateDTO& certificate);

[[nodiscard]] std::optional<std::string_view> stop_forensic_projection_result_field_error(
    const StopForensicProjectionResultDTO& result) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_status_field_error(
    const StopStatusDTO& status) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_terminal_projection_field_error(
    const StopStatusDTO& status, const StopCertificateDTO& certificate) noexcept;

}  // namespace manyforge::core
