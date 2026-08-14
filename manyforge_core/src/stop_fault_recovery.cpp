#include <algorithm>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/stop_fault_recovery.hpp>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Identity>
[[nodiscard]] bool identity(const Identity& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes;
}

[[nodiscard]] bool digest(const ContentDigest& value) noexcept {
  return content_identity::is_sha256_digest(value.value());
}

[[nodiscard]] bool error_valid(const ErrorDTO& error) noexcept {
  return !error.code.empty() && error.code.size() <= 128U && error.message.size() <= 1024U &&
         error.details.size() <= 8U &&
         std::all_of(error.details.begin(), error.details.end(), [](const auto& detail) {
           return !detail.first.empty() && detail.first.size() <= 64U &&
                  detail.second.size() <= 256U;
         });
}

[[nodiscard]] bool request_context_valid(const PublicRequestContextDTO& context) noexcept {
  return identity(context.request_id) && identity(context.request_owner_epoch) &&
         !context.source_label.empty() && context.source_label.size() <= 256U &&
         (!context.correlation_id.has_value() ||
          (!context.correlation_id->empty() && context.correlation_id->size() <= 256U));
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

template <typename Value, typename Less>
[[nodiscard]] bool canonical(const std::vector<Value>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(), [&](const auto& left, const auto& right) {
           return !less(left, right) && !less(right, left);
         }) == values.end();
}

template <typename Identity>
[[nodiscard]] bool canonical_identities(const std::vector<Identity>& values) noexcept {
  return canonical(values, [](const auto& left, const auto& right) { return left < right; }) &&
         std::all_of(values.begin(), values.end(), identity<Identity>);
}

[[nodiscard]] bool configuration_less(
    const StopFaultRecoveryConfigurationExpectationDTO& left,
    const StopFaultRecoveryConfigurationExpectationDTO& right) noexcept {
  return left.control_domain_id < right.control_domain_id;
}

[[nodiscard]] bool configuration_fact_less(
    const StopFaultRecoveryConfigurationFactDTO& left,
    const StopFaultRecoveryConfigurationFactDTO& right) noexcept {
  return configuration_less(left.configuration, right.configuration);
}

[[nodiscard]] bool reset_fact_less(const StopFaultRecoveryResetFactDTO& left,
                                   const StopFaultRecoveryResetFactDTO& right) noexcept {
  return left.requirement_digest < right.requirement_digest;
}

[[nodiscard]] bool certificate_ref_less(const StopCertificateRefDTO& left,
                                        const StopCertificateRefDTO& right) noexcept {
  return std::tie(left.stop_certificate_id, left.stop_certificate_revision, left.stop_id,
                  left.stop_generation) < std::tie(right.stop_certificate_id,
                                                   right.stop_certificate_revision, right.stop_id,
                                                   right.stop_generation);
}

[[nodiscard]] bool recovery_evidence_less(const StopFaultRecoveryEvidenceRefDTO& left,
                                          const StopFaultRecoveryEvidenceRefDTO& right) noexcept {
  return std::tie(left.kind, left.evidence_digest) < std::tie(right.kind, right.evidence_digest);
}

[[nodiscard]] bool configuration_valid(
    const StopFaultRecoveryConfigurationExpectationDTO& configuration) noexcept {
  return identity(configuration.control_domain_id) &&
         identity(configuration.active_control_configuration_id) &&
         configuration.active_control_configuration_generation.nonzero();
}

[[nodiscard]] bool resource_fact_valid(const StopFaultRecoveryResourceFactDTO& fact,
                                       const bool external) noexcept {
  if (!identity(fact.resource_id)) return false;
  switch (fact.state) {
    case StopFaultRecoveryResourceStateDTO::kSettled:
    case StopFaultRecoveryResourceStateDTO::kUnsettled:
      return (!external || fact.state == StopFaultRecoveryResourceStateDTO::kSettled) &&
             fact.source_epoch.has_value() && identity(*fact.source_epoch) &&
             fact.source_sequence.has_value() && *fact.source_sequence > 0U &&
             fact.source_age_ns.has_value() && *fact.source_age_ns >= 0 &&
             fact.freshness_budget_ns.has_value() && *fact.freshness_budget_ns > 0 &&
             *fact.source_age_ns <= *fact.freshness_budget_ns && fact.evidence_digest.has_value() &&
             digest(*fact.evidence_digest) && !fact.error.has_value();
    case StopFaultRecoveryResourceStateDTO::kUnavailable:
      return !external && !fact.source_epoch.has_value() && !fact.source_sequence.has_value() &&
             !fact.source_age_ns.has_value() && !fact.freshness_budget_ns.has_value() &&
             !fact.evidence_digest.has_value() && fact.error.has_value() &&
             error_valid(*fact.error);
  }
  return false;
}

[[nodiscard]] bool configuration_fact_valid(
    const StopFaultRecoveryConfigurationFactDTO& fact) noexcept {
  return configuration_valid(fact.configuration) &&
         (fact.reconciled == fact.evidence_digest.has_value()) &&
         (!fact.evidence_digest.has_value() || digest(*fact.evidence_digest));
}

[[nodiscard]] bool reset_fact_valid(const StopFaultRecoveryResetFactDTO& fact) noexcept {
  return digest(fact.requirement_digest) && (fact.reconciled == fact.evidence_digest.has_value()) &&
         (!fact.evidence_digest.has_value() || digest(*fact.evidence_digest));
}

[[nodiscard]] bool terminal_state(const StopStatusStateDTO state) noexcept {
  return state == StopStatusStateDTO::kSucceeded || state == StopStatusStateDTO::kPartial ||
         state == StopStatusStateDTO::kFailed;
}

template <typename T>
[[nodiscard]] Result<T> invalid(std::string code, std::string message) {
  return Result<T>::Err(ErrorDTO{std::move(code), std::move(message), {}});
}

[[nodiscard]] ErrorDTO rejection(std::string code, std::string message) {
  return ErrorDTO{std::move(code), std::move(message), {}};
}

[[nodiscard]] bool configurations_match(
    const std::vector<StopFaultRecoveryConfigurationExpectationDTO>& expected,
    const std::vector<StopFaultRecoveryConfigurationFactDTO>& actual) noexcept {
  if (expected.size() != actual.size()) return false;
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    if (expected[index] != actual[index].configuration) return false;
  }
  return true;
}

[[nodiscard]] std::vector<ResourceId> resource_ids(
    const std::vector<StopFaultRecoveryResourceFactDTO>& facts) {
  std::vector<ResourceId> values;
  values.reserve(facts.size());
  for (const auto& fact : facts) values.push_back(fact.resource_id);
  return values;
}

[[nodiscard]] StopFaultRecoveryEvidenceRefDTO evidence_ref(
    const StopFaultRecoveryEvidenceKindDTO kind, const ContentDigest& value) {
  return StopFaultRecoveryEvidenceRefDTO{kind, value};
}

[[nodiscard]] std::vector<StopFaultRecoveryEvidenceRefDTO> collect_evidence(
    const StopFaultRecoveryFactsDTO& facts) {
  std::vector<StopFaultRecoveryEvidenceRefDTO> values;
  values.reserve(facts.resource_facts.size() + facts.configuration_facts.size() +
                 facts.reset_facts.size() + facts.external_reconciliation_facts.size() + 1U);
  for (const auto& fact : facts.resource_facts) {
    if (fact.state == StopFaultRecoveryResourceStateDTO::kSettled) {
      values.push_back(
          evidence_ref(StopFaultRecoveryEvidenceKindDTO::kSettlement, *fact.evidence_digest));
    }
  }
  if (facts.surveillance.has_value() &&
      facts.surveillance->late_quiescence_evidence_digest.has_value()) {
    values.push_back(evidence_ref(StopFaultRecoveryEvidenceKindDTO::kLateQuiescence,
                                  *facts.surveillance->late_quiescence_evidence_digest));
  }
  for (const auto& fact : facts.configuration_facts) {
    if (fact.reconciled) {
      values.push_back(
          evidence_ref(StopFaultRecoveryEvidenceKindDTO::kConfiguration, *fact.evidence_digest));
    }
  }
  for (const auto& fact : facts.external_reconciliation_facts) {
    values.push_back(evidence_ref(StopFaultRecoveryEvidenceKindDTO::kExternalReconciliation,
                                  *fact.evidence_digest));
  }
  for (const auto& fact : facts.reset_facts) {
    if (fact.reconciled) {
      values.push_back(evidence_ref(StopFaultRecoveryEvidenceKindDTO::kBackendOrLifecycleReset,
                                    *fact.evidence_digest));
    }
  }
  std::sort(values.begin(), values.end(), recovery_evidence_less);
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

[[nodiscard]] bool all_resources_settled(const StopFaultRecoveryFactsDTO& facts) noexcept {
  return std::all_of(
      facts.resource_facts.begin(), facts.resource_facts.end(),
      [](const auto& fact) { return fact.state == StopFaultRecoveryResourceStateDTO::kSettled; });
}

[[nodiscard]] bool any_resource_unavailable(const StopFaultRecoveryFactsDTO& facts) noexcept {
  return std::any_of(facts.resource_facts.begin(), facts.resource_facts.end(),
                     [](const auto& fact) {
                       return fact.state == StopFaultRecoveryResourceStateDTO::kUnavailable;
                     });
}

[[nodiscard]] StopFaultRecoveryStatusDTO derive_status(
    const StopFaultLatchDTO& latch, const StopFaultRecoveryFactsDTO& facts) noexcept {
  if (facts.acceptance_projection.state != StopDurableProjectionStateDTO::kRecorded ||
      facts.terminal_projection.state != StopDurableProjectionStateDTO::kRecorded ||
      (facts.surveillance.has_value() &&
       facts.surveillance_projection.state != StopDurableProjectionStateDTO::kRecorded)) {
    return StopFaultRecoveryStatusDTO::kWaitingForDurableRecord;
  }
  if (any_resource_unavailable(facts)) {
    return StopFaultRecoveryStatusDTO::kWaitingForObservation;
  }
  if (!all_resources_settled(facts) ||
      (facts.surveillance.has_value() &&
       facts.surveillance->state != StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved)) {
    return StopFaultRecoveryStatusDTO::kWaitingForQuiescence;
  }
  if (std::any_of(facts.configuration_facts.begin(), facts.configuration_facts.end(),
                  [](const auto& fact) { return !fact.reconciled; })) {
    return StopFaultRecoveryStatusDTO::kWaitingForConfiguration;
  }
  if (std::any_of(facts.reset_facts.begin(), facts.reset_facts.end(),
                  [](const auto& fact) { return !fact.reconciled; }) ||
      !facts.blocking_stop_certificates.empty() || !facts.effect_uncertain_operation_ids.empty() ||
      !facts.fenced_or_orphan_resources.empty()) {
    return StopFaultRecoveryStatusDTO::kWaitingForObservation;
  }
  if (latch.trigger == StopFaultTriggerDTO::kFailedUncontained &&
      resource_ids(facts.external_reconciliation_facts) != latch.captured_resources) {
    return StopFaultRecoveryStatusDTO::kWaitingForExternalReconciliation;
  }
  return StopFaultRecoveryStatusDTO::kReady;
}

[[nodiscard]] std::string_view reason_for(const StopFaultRecoveryStatusDTO status) noexcept {
  switch (status) {
    case StopFaultRecoveryStatusDTO::kWaitingForObservation:
      return "stop_fault_recovery_waiting_for_observation";
    case StopFaultRecoveryStatusDTO::kWaitingForQuiescence:
      return "stop_fault_recovery_waiting_for_quiescence";
    case StopFaultRecoveryStatusDTO::kWaitingForConfiguration:
      return "stop_fault_recovery_waiting_for_configuration";
    case StopFaultRecoveryStatusDTO::kWaitingForExternalReconciliation:
      return "stop_fault_recovery_waiting_for_external_reconciliation";
    case StopFaultRecoveryStatusDTO::kWaitingForDurableRecord:
      return "stop_fault_recovery_waiting_for_durable_record";
    case StopFaultRecoveryStatusDTO::kReady:
      return "stop_fault_recovery_ready";
    case StopFaultRecoveryStatusDTO::kRecoveryPending:
      return "stop_fault_recovery_pending";
    case StopFaultRecoveryStatusDTO::kCleared:
      return "stop_fault_recovery_cleared";
  }
  return "stop_fault_recovery_invalid";
}

[[nodiscard]] StopFaultRecoveryEvaluationDTO rejected_evaluation(
    const StopFaultRecoveryIntentDTO& intent, StopFaultLatchDTO latch, ErrorDTO error) {
  return StopFaultRecoveryEvaluationDTO{
      StopFaultRecoveryResultDTO{intent.request_context.request_id,
                                 RequestReplayStatusDTO::kAcceptedNew, false, std::move(latch),
                                 std::move(error)},
      std::nullopt};
}

}  // namespace

std::optional<std::string_view> stop_fault_recovery_intent_field_error(
    const StopFaultRecoveryIntentDTO& intent) noexcept {
  if (!request_context_valid(intent.request_context) || !identity(intent.expected_latch_id) ||
      !intent.expected_latch_generation.nonzero() ||
      intent.expected_recovery_assessment_revision == 0U ||
      !identity(intent.expected_stop_certificate_id) ||
      !intent.expected_stop_certificate_revision.nonzero() || !identity(intent.expected_cell_id) ||
      !identity(intent.expected_retained_bundle_id) || !identity(intent.expected_graph_revision) ||
      intent.configuration_expectations.empty() ||
      intent.configuration_expectations.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical(intent.configuration_expectations, configuration_less) ||
      !std::all_of(intent.configuration_expectations.begin(),
                   intent.configuration_expectations.end(), configuration_valid) ||
      (intent.reason.has_value() &&
       (intent.reason->empty() || intent.reason->size() > kMaximumStopReasonBytes))) {
    return "stop_fault_recovery_intent_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_fault_recovery_facts_field_error(
    const StopFaultRecoveryFactsDTO& facts) noexcept {
  if (facts.assessed_at_ns < 0 || !identity(facts.cell_id) || !identity(facts.retained_bundle_id) ||
      !identity(facts.graph_revision) || !facts.stop_certificate.locally_valid() ||
      !identity(facts.latch_id) || !facts.latch_generation.nonzero() ||
      !projection_status_valid(facts.acceptance_projection) ||
      !projection_status_valid(facts.terminal_projection) ||
      !projection_status_valid(facts.surveillance_projection) || facts.resource_facts.empty() ||
      facts.resource_facts.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical(facts.resource_facts,
                 [](const auto& left, const auto& right) {
                   return left.resource_id < right.resource_id;
                 }) ||
      !std::all_of(facts.resource_facts.begin(), facts.resource_facts.end(),
                   [](const auto& fact) { return resource_fact_valid(fact, false); }) ||
      facts.configuration_facts.empty() ||
      facts.configuration_facts.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical(facts.configuration_facts, configuration_fact_less) ||
      !std::all_of(facts.configuration_facts.begin(), facts.configuration_facts.end(),
                   configuration_fact_valid) ||
      facts.reset_facts.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical(facts.reset_facts, reset_fact_less) ||
      !std::all_of(facts.reset_facts.begin(), facts.reset_facts.end(), reset_fact_valid) ||
      facts.external_reconciliation_facts.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical(facts.external_reconciliation_facts,
                 [](const auto& left, const auto& right) {
                   return left.resource_id < right.resource_id;
                 }) ||
      !std::all_of(facts.external_reconciliation_facts.begin(),
                   facts.external_reconciliation_facts.end(),
                   [](const auto& fact) { return resource_fact_valid(fact, true); }) ||
      facts.blocking_stop_certificates.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical(facts.blocking_stop_certificates, certificate_ref_less) ||
      !std::all_of(facts.blocking_stop_certificates.begin(), facts.blocking_stop_certificates.end(),
                   [](const auto& reference) { return reference.locally_valid(); }) ||
      facts.effect_uncertain_operation_ids.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical_identities(facts.effect_uncertain_operation_ids) ||
      facts.fenced_or_orphan_resources.size() > kMaximumStopFaultRecoveryEvidenceRefs ||
      !canonical_identities(facts.fenced_or_orphan_resources) ||
      (facts.surveillance.has_value() &&
       stop_post_terminal_surveillance_field_error(*facts.surveillance).has_value())) {
    return "stop_fault_recovery_facts_invalid";
  }
  const bool surveillance_applicable = facts.surveillance.has_value();
  if (surveillance_applicable !=
          (facts.surveillance_projection.state != StopDurableProjectionStateDTO::kNotApplicable) ||
      (facts.surveillance.has_value() &&
       (facts.surveillance->stop_certificate != facts.stop_certificate ||
        facts.surveillance->latch_id != facts.latch_id ||
        facts.surveillance->latch_generation != facts.latch_generation))) {
    return "stop_fault_recovery_facts_join_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_fault_recovery_evaluation_field_error(
    const StopFaultRecoveryEvaluationDTO& evaluation) noexcept {
  if (!identity(evaluation.result.request_id) ||
      evaluation.result.replay_status != RequestReplayStatusDTO::kAcceptedNew ||
      stop_fault_latch_field_error(evaluation.result.latch).has_value() ||
      (evaluation.result.error.has_value() && !error_valid(*evaluation.result.error))) {
    return "stop_fault_recovery_evaluation_invalid";
  }
  if (evaluation.result.accepted) {
    if (evaluation.result.error.has_value() || !evaluation.resulting_stop_status.has_value() ||
        stop_status_field_error(*evaluation.resulting_stop_status).has_value() ||
        !evaluation.resulting_stop_status->fault_latch.has_value() ||
        *evaluation.resulting_stop_status->fault_latch != evaluation.result.latch ||
        evaluation.result.latch.state != StopFaultLatchStateDTO::kCleared ||
        evaluation.result.latch.recovery_status != StopFaultRecoveryStatusDTO::kCleared ||
        evaluation.result.latch.clear_projection.state !=
            StopDurableProjectionStateDTO::kRecorded) {
      return "stop_fault_recovery_evaluation_presence_invalid";
    }
  } else if (!evaluation.result.error.has_value() || evaluation.resulting_stop_status.has_value() ||
             evaluation.result.latch.state == StopFaultLatchStateDTO::kCleared) {
    return "stop_fault_recovery_evaluation_presence_invalid";
  }
  return std::nullopt;
}

Result<StopFaultLatchDTO> AssessStopFaultRecovery(const StopStatusDTO& terminal_status,
                                                  const StopFaultRecoveryFactsDTO& facts) {
  if (stop_status_field_error(terminal_status).has_value() ||
      !terminal_state(terminal_status.state) || !terminal_status.stop_certificate.has_value() ||
      !terminal_status.fault_latch.has_value() ||
      terminal_status.fault_latch->state != StopFaultLatchStateDTO::kLatched ||
      stop_fault_recovery_facts_field_error(facts).has_value()) {
    return invalid<StopFaultLatchDTO>("stop_fault_recovery_assessment_invalid",
                                      "Stop recovery assessment input is invalid");
  }
  const auto& current = *terminal_status.fault_latch;
  if (facts.cell_id != current.cell_id || facts.retained_bundle_id != current.retained_bundle_id ||
      facts.graph_revision != current.graph_revision ||
      facts.stop_certificate != *terminal_status.stop_certificate ||
      facts.stop_certificate != *current.stop_certificate || facts.latch_id != current.latch_id ||
      facts.latch_generation != current.latch_generation ||
      facts.acceptance_projection != terminal_status.acceptance_projection ||
      facts.terminal_projection != terminal_status.terminal_projection ||
      facts.surveillance_projection != terminal_status.surveillance_projection ||
      facts.surveillance != terminal_status.surveillance ||
      resource_ids(facts.resource_facts) != current.captured_resources) {
    return invalid<StopFaultLatchDTO>("stop_fault_recovery_facts_join_invalid",
                                      "Stop recovery facts do not join current terminal truth");
  }

  auto assessed = current;
  const auto next_status = derive_status(current, facts);
  const auto next_reason = std::string(reason_for(next_status));
  const auto next_evidence = collect_evidence(facts);
  // Once the complete predicate set is ready, a newer fresh observation is
  // stronger proof of the same semantic assessment, not a new assessment.
  // Keep the exact evidence snapshot that made the latch ready so a caller can
  // name that stable revision while the physical state source keeps publishing
  // newer sequence/timestamp-bearing samples. A loss of readiness still
  // advances immediately and publishes the changed predicate truth.
  const bool equivalent_ready_refresh =
      assessed.recovery_status == StopFaultRecoveryStatusDTO::kReady &&
      next_status == StopFaultRecoveryStatusDTO::kReady && assessed.reason_code == next_reason;
  if (assessed.recovery_status != next_status || assessed.reason_code != next_reason ||
      (!equivalent_ready_refresh && assessed.evidence_refs != next_evidence)) {
    if (assessed.recovery_assessment_revision == std::numeric_limits<std::uint64_t>::max()) {
      return invalid<StopFaultLatchDTO>("stop_fault_recovery_revision_exhausted",
                                        "Stop recovery assessment revision is exhausted");
    }
    ++assessed.recovery_assessment_revision;
    assessed.recovery_status = next_status;
    assessed.reason_code = next_reason;
    assessed.evidence_refs = next_evidence;
  }
  if (stop_fault_latch_field_error(assessed).has_value()) {
    return invalid<StopFaultLatchDTO>("stop_fault_recovery_assessment_invalid",
                                      "Stop recovery assessment is invalid");
  }
  return Result<StopFaultLatchDTO>::Ok(std::move(assessed));
}

Result<StopFaultRecoveryEvaluationDTO> EvaluateStopFaultRecovery(
    const StopFaultRecoveryIntentDTO& intent, const StopStatusDTO& terminal_status,
    const StopFaultRecoveryFactsDTO& facts) {
  if (stop_fault_recovery_intent_field_error(intent).has_value()) {
    return invalid<StopFaultRecoveryEvaluationDTO>("stop_fault_recovery_intent_invalid",
                                                   "Stop recovery intent is invalid");
  }
  auto assessed_result = AssessStopFaultRecovery(terminal_status, facts);
  if (!assessed_result.ok()) {
    return invalid<StopFaultRecoveryEvaluationDTO>(assessed_result.error().code,
                                                   assessed_result.error().message);
  }
  auto assessed = std::move(assessed_result.value());
  const bool identity_match =
      intent.expected_latch_id == assessed.latch_id &&
      intent.expected_latch_generation == assessed.latch_generation &&
      intent.expected_recovery_assessment_revision == assessed.recovery_assessment_revision &&
      assessed.stop_certificate.has_value() &&
      intent.expected_stop_certificate_id == assessed.stop_certificate->stop_certificate_id &&
      intent.expected_stop_certificate_revision ==
          assessed.stop_certificate->stop_certificate_revision &&
      intent.expected_cell_id == assessed.cell_id &&
      intent.expected_retained_bundle_id == assessed.retained_bundle_id &&
      intent.expected_graph_revision == assessed.graph_revision;
  if (!identity_match) {
    auto evaluation = rejected_evaluation(
        intent, std::move(assessed),
        rejection("stop_fault_recovery_identity_mismatch",
                  "Stop recovery expectations do not match current latch truth"));
    return Result<StopFaultRecoveryEvaluationDTO>::Ok(std::move(evaluation));
  }
  if (!configurations_match(intent.configuration_expectations, facts.configuration_facts)) {
    auto evaluation = rejected_evaluation(
        intent, std::move(assessed),
        rejection("stop_fault_recovery_configuration_mismatch",
                  "Stop recovery configuration expectations do not match current facts"));
    return Result<StopFaultRecoveryEvaluationDTO>::Ok(std::move(evaluation));
  }
  if (assessed.recovery_status != StopFaultRecoveryStatusDTO::kReady) {
    auto evaluation = rejected_evaluation(
        intent, std::move(assessed),
        rejection("stop_fault_recovery_not_ready", "Stop recovery predicates are not ready"));
    return Result<StopFaultRecoveryEvaluationDTO>::Ok(std::move(evaluation));
  }
  if (assessed.recovery_assessment_revision == std::numeric_limits<std::uint64_t>::max()) {
    return invalid<StopFaultRecoveryEvaluationDTO>(
        "stop_fault_recovery_revision_exhausted", "Stop recovery assessment revision is exhausted");
  }

  ++assessed.recovery_assessment_revision;
  assessed.state = StopFaultLatchStateDTO::kCleared;
  assessed.recovery_status = StopFaultRecoveryStatusDTO::kCleared;
  assessed.reason_code = std::string(reason_for(StopFaultRecoveryStatusDTO::kCleared));
  assessed.clear_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  auto resulting_status = terminal_status;
  resulting_status.fault_latch = assessed;
  StopFaultRecoveryEvaluationDTO evaluation{
      StopFaultRecoveryResultDTO{intent.request_context.request_id,
                                 RequestReplayStatusDTO::kAcceptedNew, true, assessed,
                                 std::nullopt},
      std::move(resulting_status)};
  if (stop_fault_recovery_evaluation_field_error(evaluation).has_value()) {
    return invalid<StopFaultRecoveryEvaluationDTO>("stop_fault_recovery_evaluation_invalid",
                                                   "Stop recovery evaluation is invalid");
  }
  return Result<StopFaultRecoveryEvaluationDTO>::Ok(std::move(evaluation));
}

}  // namespace manyforge::core
