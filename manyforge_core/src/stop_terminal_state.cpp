#include <algorithm>
#include <manyforge_core/stop_terminal_state.hpp>
#include <tuple>

namespace manyforge::core {
namespace {

template <typename Identity>
[[nodiscard]] bool identity(const Identity& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes;
}

[[nodiscard]] bool error_valid(const ErrorDTO& error) noexcept {
  return !error.code.empty() && error.code.size() <= 128U && error.message.size() <= 1024U &&
         error.details.size() <= 8U &&
         std::all_of(error.details.begin(), error.details.end(), [](const auto& detail) {
           return !detail.first.empty() && detail.first.size() <= 64U &&
                  detail.second.size() <= 256U;
         });
}

template <typename Row, typename Less>
[[nodiscard]] bool canonical(const std::vector<Row>& rows, Less less) noexcept {
  for (std::size_t index = 1U; index < rows.size(); ++index) {
    if (!less(rows[index - 1U], rows[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool key_less(const StopScopeEntryKeyDTO& left,
                            const StopScopeEntryKeyDTO& right) noexcept {
  if (left.origin_stop_id != right.origin_stop_id) {
    return left.origin_stop_id < right.origin_stop_id;
  }
  if (left.origin_stop_generation != right.origin_stop_generation) {
    return left.origin_stop_generation < right.origin_stop_generation;
  }
  if (left.subject.index() != right.subject.index()) {
    return left.subject.index() < right.subject.index();
  }
  if (const auto* left_participant = std::get_if<StopParticipantScopeKeyDTO>(&left.subject)) {
    const auto& right_participant = std::get<StopParticipantScopeKeyDTO>(right.subject);
    return std::tie(left_participant->operation_id, left_participant->participant_id) <
           std::tie(right_participant.operation_id, right_participant.participant_id);
  }
  return std::get<ResourceId>(left.subject) < std::get<ResourceId>(right.subject);
}

[[nodiscard]] bool projection_status_valid(const StopDurableProjectionStatusDTO& status) noexcept {
  switch (status.state) {
    case StopDurableProjectionStateDTO::kNotApplicable:
    case StopDurableProjectionStateDTO::kPending:
    case StopDurableProjectionStateDTO::kRecorded:
      return !status.error.has_value() && !status.retry_generation.has_value();
    case StopDurableProjectionStateDTO::kFailed:
      return status.error.has_value() && error_valid(*status.error) &&
             status.retry_generation.has_value() && *status.retry_generation > 0U;
  }
  return false;
}

[[nodiscard]] bool status_terminal(const StopStatusStateDTO state) noexcept {
  return state == StopStatusStateDTO::kSucceeded || state == StopStatusStateDTO::kPartial ||
         state == StopStatusStateDTO::kFailed;
}

[[nodiscard]] bool status_state_valid(const StopStatusStateDTO state) noexcept {
  switch (state) {
    case StopStatusStateDTO::kAccepted:
    case StopStatusStateDTO::kStopping:
    case StopStatusStateDTO::kCertifying:
    case StopStatusStateDTO::kSucceeded:
    case StopStatusStateDTO::kPartial:
    case StopStatusStateDTO::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool current_stage_less(const StopCurrentStageDTO& left,
                                      const StopCurrentStageDTO& right) noexcept {
  return key_less(left.scope_key, right.scope_key);
}

[[nodiscard]] bool current_stage_valid(const StopCurrentStageDTO& stage,
                                       const StopAcceptedDTO& accepted) noexcept {
  if (!identity(stage.stop_escalation_stage_id) || stage.stage_index >= kMaxStopEscalationStages ||
      stage.remaining_budget.stop_id != accepted.stop_id ||
      stage.remaining_budget.stop_generation != accepted.stop_generation ||
      stage.remaining_budget.elapsed_since_stop_ns < 0 ||
      stage.remaining_budget.elapsed_since_stop_ns > kMaximumStopDurationNs ||
      (stage.remaining_budget.stop_ingress_age_ns &&
       (*stage.remaining_budget.stop_ingress_age_ns < 0 ||
        *stage.remaining_budget.stop_ingress_age_ns > kMaximumStopDurationNs)) ||
      (stage.remaining_budget.remaining_controlled_deadline_ns.has_value() &&
       (*stage.remaining_budget.remaining_controlled_deadline_ns < 0 ||
        *stage.remaining_budget.remaining_controlled_deadline_ns > kMaximumStopDurationNs))) {
    return false;
  }
  switch (stage.stage_kind) {
    case StopEscalationStageKind::kNominal:
    case StopEscalationStageKind::kControlledFallback:
    case StopEscalationStageKind::kSoftwareHard:
    case StopEscalationStageKind::kSafetySystemRequest:
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace

std::optional<std::string_view> stop_forensic_projection_result_field_error(
    const StopForensicProjectionResultDTO& result) noexcept {
  switch (result.state) {
    case StopForensicProjectionStateDTO::kRecorded:
      return result.journal_sequence.has_value() && *result.journal_sequence > 0U &&
                     !result.error.has_value()
                 ? std::nullopt
                 : std::optional<std::string_view>("stop_forensic_projection_result_invalid");
    case StopForensicProjectionStateDTO::kFailed:
      return !result.journal_sequence.has_value() && result.error.has_value() &&
                     error_valid(*result.error)
                 ? std::nullopt
                 : std::optional<std::string_view>("stop_forensic_projection_result_invalid");
  }
  return "stop_forensic_projection_result_invalid";
}

std::optional<std::string_view> stop_status_field_error(const StopStatusDTO& status) noexcept {
  if (stop_accepted_field_error(status.accepted).has_value() || !status_state_valid(status.state) ||
      status.scope_entries.size() > kMaximumStopTerminalScopeEntries ||
      status.current_stages.size() > kMaximumStopTerminalScopeEntries ||
      status.attempted_stage_results.size() > kMaximumStopTerminalAttemptResults ||
      status.evidence_items.size() > kMaximumStopTerminalEvidenceItems ||
      !canonical(status.current_stages, current_stage_less) ||
      !std::all_of(
          status.current_stages.begin(), status.current_stages.end(),
          [&](const auto& stage) { return current_stage_valid(stage, status.accepted); }) ||
      !projection_status_valid(status.acceptance_projection) ||
      !projection_status_valid(status.terminal_projection) ||
      (status.stop_certificate.has_value() &&
       (!status.stop_certificate->locally_valid() ||
        status.stop_certificate->stop_id != status.accepted.stop_id ||
        status.stop_certificate->stop_generation != status.accepted.stop_generation))) {
    return "stop_status_shape_invalid";
  }
  const bool terminal = status_terminal(status.state);
  if (terminal != status.stop_certificate.has_value() ||
      (terminal && !status.current_stages.empty())) {
    return "stop_status_presence_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_terminal_projection_field_error(
    const StopStatusDTO& status, const StopCertificateDTO& certificate) noexcept {
  if (stop_status_field_error(status).has_value() ||
      stop_certificate_field_error(certificate).has_value() || !status_terminal(status.state) ||
      !status.stop_certificate.has_value() ||
      status.stop_certificate->stop_certificate_id != certificate.stop_certificate_id ||
      status.stop_certificate->stop_certificate_revision != certificate.stop_certificate_revision ||
      status.accepted.stop_id != certificate.stop_id ||
      status.accepted.stop_generation != certificate.stop_generation ||
      status.accepted.requested_selector_digest !=
          certificate.requested_selector.requested_selector_digest ||
      status.accepted.resolution_disposition != certificate.resolution_disposition ||
      status.accepted.resolution_reason != certificate.resolution_reason ||
      status.scope_entries != certificate.scope_entries ||
      status.attempted_stage_results != certificate.attempted_stage_results ||
      status.evidence_items != certificate.evidence_items ||
      status.unsettled != certificate.unsettled || status.failed != certificate.failed) {
    return "stop_terminal_projection_join_invalid";
  }
  const auto expected_status = [&] {
    switch (certificate.result) {
      case StopCertificateResultDTO::kSucceededNominal:
      case StopCertificateResultDTO::kSucceededEscalated:
        return StopStatusStateDTO::kSucceeded;
      case StopCertificateResultDTO::kPartial:
        return StopStatusStateDTO::kPartial;
      case StopCertificateResultDTO::kFailedUncontained:
        return StopStatusStateDTO::kFailed;
    }
    return StopStatusStateDTO::kFailed;
  }();
  if (status.state != expected_status) {
    return "stop_terminal_projection_outcome_invalid";
  }
  return std::nullopt;
}

StopTerminalStatusRestDTO TerminalStopStatusRest(const StopStatusDTO& status) {
  return StopTerminalStatusRestDTO{status.accepted,
                                   status.current_stages,
                                   status.acceptance_projection,
                                   status.terminal_projection};
}

// Lean Stop record: the terminal Stop status from its certificate.
StopStatusDTO TerminalStopStatusFromCertificate(const StopTerminalStatusRestDTO& rest,
                                                const StopCertificateDTO& certificate) {
  const auto state = [&] {
    switch (certificate.result) {
      case StopCertificateResultDTO::kSucceededNominal:
      case StopCertificateResultDTO::kSucceededEscalated:
        return StopStatusStateDTO::kSucceeded;
      case StopCertificateResultDTO::kPartial:
        return StopStatusStateDTO::kPartial;
      case StopCertificateResultDTO::kFailedUncontained:
        return StopStatusStateDTO::kFailed;
    }
    return StopStatusStateDTO::kFailed;
  }();
  return StopStatusDTO{rest.accepted,
                       state,
                       certificate.scope_entries,
                       rest.current_stages,
                       certificate.attempted_stage_results,
                       certificate.evidence_items,
                       certificate.unsettled,
                       certificate.failed,
                       rest.acceptance_projection,
                       rest.terminal_projection,
                       StopCertificateRefDTO{certificate.stop_certificate_id,
                                             certificate.stop_certificate_revision,
                                             certificate.stop_id, certificate.stop_generation}};
}

}  // namespace manyforge::core
