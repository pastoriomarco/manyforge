#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/stop_terminal_state.hpp>
#include <tuple>

namespace manyforge::core {
namespace {

template <typename Identity>
[[nodiscard]] bool identity(const Identity& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes;
}

[[nodiscard]] bool digest(const ContentDigest& value) noexcept {
  return manyforge::content_identity::is_sha256_digest(value.value());
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

template <typename Identity>
[[nodiscard]] bool canonical_identities(const std::vector<Identity>& rows) noexcept {
  return canonical(rows, [](const auto& left, const auto& right) { return left < right; }) &&
         std::all_of(rows.begin(), rows.end(), identity<Identity>);
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
      stage.remaining_budget.remaining_outer_duration_ns < 0 ||
      stage.remaining_budget.remaining_outer_duration_ns > accepted.fixed_outer_duration_ns ||
      stage.remaining_budget.phases.empty() ||
      stage.remaining_budget.phases.size() > kMaximumStopAcceptedPhases) {
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
  std::int64_t previous = -1;
  for (const auto& phase : stage.remaining_budget.phases) {
    if (!identity(phase.phase_id) || phase.remaining_deadline_offset_ns < 0 ||
        phase.remaining_deadline_offset_ns <= previous ||
        phase.remaining_deadline_offset_ns > stage.remaining_budget.remaining_outer_duration_ns) {
      return false;
    }
    previous = phase.remaining_deadline_offset_ns;
  }
  return true;
}

[[nodiscard]] bool source_status_less(const StopSurveillanceSourceStatusDTO& left,
                                      const StopSurveillanceSourceStatusDTO& right) noexcept {
  if (key_less(left.scope_key, right.scope_key)) return true;
  if (key_less(right.scope_key, left.scope_key)) return false;
  return left.evidence_item_id < right.evidence_item_id;
}

[[nodiscard]] bool recovery_evidence_less(const StopFaultRecoveryEvidenceRefDTO& left,
                                          const StopFaultRecoveryEvidenceRefDTO& right) noexcept {
  return std::tie(left.kind, left.evidence_digest) < std::tie(right.kind, right.evidence_digest);
}

[[nodiscard]] bool recovery_evidence_valid(
    const StopFaultRecoveryEvidenceRefDTO& evidence) noexcept {
  switch (evidence.kind) {
    case StopFaultRecoveryEvidenceKindDTO::kSettlement:
    case StopFaultRecoveryEvidenceKindDTO::kLateQuiescence:
    case StopFaultRecoveryEvidenceKindDTO::kConfiguration:
    case StopFaultRecoveryEvidenceKindDTO::kExternalReconciliation:
    case StopFaultRecoveryEvidenceKindDTO::kBackendOrLifecycleReset:
      return digest(evidence.evidence_digest);
  }
  return false;
}

[[nodiscard]] bool source_status_valid(const StopSurveillanceSourceStatusDTO& source) noexcept {
  if (!identity(source.scope_key.origin_stop_id) ||
      !source.scope_key.origin_stop_generation.nonzero() || !identity(source.evidence_item_id) ||
      (source.source_epoch.has_value() != source.source_sequence.has_value()) ||
      (source.source_epoch.has_value() != source.source_age_ns.has_value())) {
    return false;
  }
  switch (source.availability) {
    case StopSurveillanceSourceAvailabilityDTO::kAvailable:
      return source.source_epoch.has_value() && identity(*source.source_epoch) &&
             *source.source_sequence > 0U && *source.source_age_ns >= 0 &&
             !source.error.has_value();
    case StopSurveillanceSourceAvailabilityDTO::kUnavailable:
    case StopSurveillanceSourceAvailabilityDTO::kInvalid:
      return !source.source_epoch.has_value() && source.error.has_value() &&
             error_valid(*source.error);
  }
  return false;
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

std::optional<std::string_view> stop_fault_latch_field_error(
    const StopFaultLatchDTO& latch) noexcept {
  if (!identity(latch.cell_id) || !identity(latch.stop_id) || !latch.stop_generation.nonzero() ||
      !identity(latch.latch_id) || !latch.latch_generation.nonzero() ||
      !identity(latch.retained_bundle_id) || !identity(latch.graph_revision) ||
      latch.recovery_assessment_revision == 0U || latch.reason_code.empty() ||
      latch.reason_code.size() > kMaximumStopReasonBytes ||
      latch.captured_operation_ids.size() > kMaximumStopTerminalScopeEntries ||
      latch.captured_resources.size() > kMaximumStopTerminalScopeEntries ||
      latch.evidence_refs.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical_identities(latch.captured_operation_ids) ||
      !canonical_identities(latch.captured_resources) ||
      !canonical(latch.evidence_refs, recovery_evidence_less) ||
      !std::all_of(latch.evidence_refs.begin(), latch.evidence_refs.end(),
                   recovery_evidence_valid) ||
      !projection_status_valid(latch.terminal_projection) ||
      !projection_status_valid(latch.clear_projection) ||
      (latch.stop_certificate.has_value() &&
       (!latch.stop_certificate->locally_valid() ||
        latch.stop_certificate->stop_id != latch.stop_id ||
        latch.stop_certificate->stop_generation != latch.stop_generation))) {
    return "stop_fault_latch_shape_invalid";
  }

  switch (latch.trigger) {
    case StopFaultTriggerDTO::kScopePromotion:
    case StopFaultTriggerDTO::kEscalated:
    case StopFaultTriggerDTO::kPartial:
    case StopFaultTriggerDTO::kFailedUncontained:
    case StopFaultTriggerDTO::kPhysicalRestUnproven:
    case StopFaultTriggerDTO::kPersistenceDegraded:
      break;
    default:
      return "stop_fault_latch_trigger_invalid";
  }
  switch (latch.state) {
    case StopFaultLatchStateDTO::kLatched:
    case StopFaultLatchStateDTO::kRecoveryPending:
    case StopFaultLatchStateDTO::kCleared:
      break;
    default:
      return "stop_fault_latch_state_invalid";
  }
  switch (latch.recovery_status) {
    case StopFaultRecoveryStatusDTO::kWaitingForObservation:
    case StopFaultRecoveryStatusDTO::kWaitingForQuiescence:
    case StopFaultRecoveryStatusDTO::kWaitingForConfiguration:
    case StopFaultRecoveryStatusDTO::kWaitingForExternalReconciliation:
    case StopFaultRecoveryStatusDTO::kWaitingForDurableRecord:
    case StopFaultRecoveryStatusDTO::kReady:
    case StopFaultRecoveryStatusDTO::kRecoveryPending:
    case StopFaultRecoveryStatusDTO::kCleared:
      break;
    default:
      return "stop_fault_latch_recovery_status_invalid";
  }
  if ((latch.state == StopFaultLatchStateDTO::kCleared) !=
          (latch.recovery_status == StopFaultRecoveryStatusDTO::kCleared) ||
      (latch.state == StopFaultLatchStateDTO::kCleared &&
       latch.clear_projection.state != StopDurableProjectionStateDTO::kRecorded) ||
      (latch.state != StopFaultLatchStateDTO::kCleared &&
       latch.clear_projection.state == StopDurableProjectionStateDTO::kRecorded) ||
      (latch.trigger == StopFaultTriggerDTO::kPersistenceDegraded && !latch.persistence_degraded)) {
    return "stop_fault_latch_presence_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_post_terminal_surveillance_field_error(
    const StopPostTerminalSurveillanceDTO& surveillance) noexcept {
  if (!identity(surveillance.stop_id) || !surveillance.stop_generation.nonzero() ||
      !surveillance.stop_certificate.locally_valid() ||
      surveillance.stop_certificate.stop_id != surveillance.stop_id ||
      surveillance.stop_certificate.stop_generation != surveillance.stop_generation ||
      !identity(surveillance.latch_id) || !surveillance.latch_generation.nonzero() ||
      !identity(surveillance.owner_epoch) || surveillance.surveillance_generation == 0U ||
      surveillance.resources.empty() ||
      surveillance.resources.size() > kMaximumStopTerminalScopeEntries ||
      !canonical_identities(surveillance.resources) || surveillance.source_statuses.empty() ||
      surveillance.source_statuses.size() > kMaximumStopSurveillanceSources ||
      !canonical(surveillance.source_statuses, source_status_less) ||
      !std::all_of(surveillance.source_statuses.begin(), surveillance.source_statuses.end(),
                   source_status_valid) ||
      surveillance.policy.sample_cadence_ns <= 0 ||
      surveillance.policy.checkpoint_interval_ns < surveillance.policy.sample_cadence_ns ||
      surveillance.policy.retained_checkpoint_limit == 0U ||
      surveillance.policy.retained_checkpoint_limit > kMaximumStopSurveillanceCheckpoints ||
      surveillance.checkpoint_generation == 0U ||
      !projection_status_valid(surveillance.checkpoint_projection)) {
    return "stop_surveillance_shape_invalid";
  }
  const bool continued = surveillance.continued_motion_count > 0U;
  if (continued != surveillance.first_continued_motion_at_ns.has_value() ||
      continued != surveillance.last_continued_motion_at_ns.has_value() ||
      (continued &&
       (*surveillance.first_continued_motion_at_ns < 0 ||
        *surveillance.last_continued_motion_at_ns < *surveillance.first_continued_motion_at_ns))) {
    return "stop_surveillance_motion_presence_invalid";
  }
  const bool quiescent =
      surveillance.state == StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved;
  if ((surveillance.state != StopPostTerminalSurveillanceStateDTO::kMonitoring && !quiescent) ||
      quiescent != surveillance.late_quiescence_evidence_digest.has_value() ||
      quiescent != surveillance.late_quiescence_observed_at_ns.has_value() ||
      (quiescent && (!digest(*surveillance.late_quiescence_evidence_digest) ||
                     *surveillance.late_quiescence_observed_at_ns < 0))) {
    return "stop_surveillance_quiescence_presence_invalid";
  }
  return std::nullopt;
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
      !projection_status_valid(status.surveillance_projection) ||
      (status.stop_certificate.has_value() &&
       (!status.stop_certificate->locally_valid() ||
        status.stop_certificate->stop_id != status.accepted.stop_id ||
        status.stop_certificate->stop_generation != status.accepted.stop_generation)) ||
      (status.fault_latch.has_value() && stop_fault_latch_field_error(*status.fault_latch)) ||
      (status.surveillance.has_value() &&
       stop_post_terminal_surveillance_field_error(*status.surveillance))) {
    return "stop_status_shape_invalid";
  }
  const bool terminal = status_terminal(status.state);
  if (terminal != status.stop_certificate.has_value() ||
      (terminal && !status.current_stages.empty()) ||
      (status.surveillance.has_value() !=
       (status.surveillance_projection.state != StopDurableProjectionStateDTO::kNotApplicable)) ||
      (status.fault_latch.has_value() &&
       (status.fault_latch->stop_id != status.accepted.stop_id ||
        status.fault_latch->stop_generation != status.accepted.stop_generation)) ||
      (status.surveillance.has_value() &&
       (status.surveillance->stop_id != status.accepted.stop_id ||
        status.surveillance->stop_generation != status.accepted.stop_generation))) {
    return "stop_status_presence_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_terminal_projection_field_error(
    const StopStatusDTO& status, const StopCertificateDTO& certificate,
    const std::optional<StopFaultLatchDTO>& fault_latch) noexcept {
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
      status.unsettled != certificate.unsettled || status.failed != certificate.failed ||
      status.fault_latch != fault_latch) {
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
  if (status.state != expected_status ||
      (fault_latch.has_value() && (fault_latch->stop_id != certificate.stop_id ||
                                   fault_latch->stop_generation != certificate.stop_generation ||
                                   !fault_latch->stop_certificate.has_value() ||
                                   *fault_latch->stop_certificate != *status.stop_certificate))) {
    return "stop_terminal_projection_outcome_invalid";
  }
  return std::nullopt;
}

}  // namespace manyforge::core
