#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <set>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Identity>
[[nodiscard]] bool bounded_identity(const Identity& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes;
}

[[nodiscard]] bool digest_valid(const ContentDigest& digest) noexcept {
  return manyforge::content_identity::is_sha256_digest(digest.value());
}

[[nodiscard]] bool error_valid(const ErrorDTO& error) noexcept {
  if (error.code.empty() || error.code.size() > 128U || error.message.size() > 1024U ||
      error.details.size() > 8U) {
    return false;
  }
  return std::all_of(error.details.begin(), error.details.end(), [](const auto& item) {
    return !item.first.empty() && item.first.size() <= 64U && item.second.size() <= 256U;
  });
}

[[nodiscard]] std::optional<std::uint8_t> granularity_rank(
    const ContainmentGranularity granularity) noexcept {
  switch (granularity) {
    case ContainmentGranularity::kResource:
      return 0U;
    case ContainmentGranularity::kControllerSet:
      return 1U;
    case ContainmentGranularity::kControlDomain:
      return 2U;
  }
  return std::nullopt;
}

template <typename Row, typename Less>
[[nodiscard]] bool canonical_rows(const std::vector<Row>& rows, Less less) noexcept {
  for (std::size_t index = 1U; index < rows.size(); ++index) {
    if (!less(rows[index - 1U], rows[index])) {
      return false;
    }
  }
  return true;
}

template <typename Identity>
[[nodiscard]] bool canonical_identities(const std::vector<Identity>& values) noexcept {
  return canonical_rows(values,
                        [](const Identity& left, const Identity& right) { return left < right; }) &&
         std::all_of(values.begin(), values.end(), bounded_identity<Identity>);
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

[[nodiscard]] bool key_valid(const StopScopeEntryKeyDTO& key) noexcept {
  if (!bounded_identity(key.origin_stop_id) || !key.origin_stop_generation.nonzero()) {
    return false;
  }
  if (const auto* participant = std::get_if<StopParticipantScopeKeyDTO>(&key.subject)) {
    return bounded_identity(participant->operation_id) &&
           bounded_identity(participant->participant_id);
  }
  return bounded_identity(std::get<ResourceId>(key.subject));
}

[[nodiscard]] bool predecessor_less(const StopPredecessorAcceptanceRefDTO& left,
                                    const StopPredecessorAcceptanceRefDTO& right) noexcept {
  return left.predecessor_stop_id < right.predecessor_stop_id;
}

[[nodiscard]] bool predecessor_valid(const StopPredecessorAcceptanceRefDTO& predecessor,
                                     const StopId& current_stop) noexcept {
  return bounded_identity(predecessor.predecessor_stop_id) &&
         predecessor.predecessor_stop_id != current_stop &&
         predecessor.predecessor_stop_generation.nonzero() &&
         digest_valid(predecessor.captured_scope_digest) &&
         predecessor.remaining_outer_duration_ns_at_join > 0 &&
         predecessor.remaining_outer_duration_ns_at_join <= kMaximumStopDurationNs;
}

[[nodiscard]] bool reason_matches_disposition(StopResolutionDispositionDTO disposition,
                                              StopScopeResolutionReasonDTO reason) noexcept {
  switch (reason) {
    case StopScopeResolutionReasonDTO::kExactCurrentScope:
      return disposition == StopResolutionDispositionDTO::kExact;
    case StopScopeResolutionReasonDTO::kExplicitFullStop:
      return disposition == StopResolutionDispositionDTO::kExact ||
             disposition == StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
    case StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope:
      return disposition == StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
    case StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect:
      return disposition == StopResolutionDispositionDTO::kAlreadyTerminalNoEffect;
    case StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent:
    case StopScopeResolutionReasonDTO::kUnknownOperation:
    case StopScopeResolutionReasonDTO::kUnknownProgramRun:
    case StopScopeResolutionReasonDTO::kUnknownComponent:
    case StopScopeResolutionReasonDTO::kUnknownResource:
    case StopScopeResolutionReasonDTO::kRequestIdentityConflict:
    case StopScopeResolutionReasonDTO::kOverlappingStopTransactions:
    case StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced:
      return disposition == StopResolutionDispositionDTO::kPromotedToFull;
  }
  return false;
}

[[nodiscard]] bool selector_snapshot_valid(
    const StopRequestedSelectorSnapshotDTO& snapshot) noexcept {
  if (!digest_valid(snapshot.requested_selector_digest)) {
    return false;
  }
  const auto ordinary = [&](std::size_t alternative) {
    if (!snapshot.selector.has_value() || snapshot.selector->index() != alternative ||
        snapshot.malformed_ingress_reason.has_value()) {
      return false;
    }
    return std::visit(
        [](const auto& rows) {
          return !rows.empty() && rows.size() <= kMaximumStopSelectorTargets;
        },
        *snapshot.selector);
  };
  switch (snapshot.kind) {
    case StopRequestedSelectorKindDTO::kOrdinaryOperations:
      return ordinary(0U);
    case StopRequestedSelectorKindDTO::kOrdinaryComponents:
      return ordinary(1U);
    case StopRequestedSelectorKindDTO::kOrdinaryResources:
      return ordinary(2U);
    case StopRequestedSelectorKindDTO::kInternalProgramRun:
      return ordinary(3U);
    case StopRequestedSelectorKindDTO::kExplicitFull:
      return !snapshot.selector.has_value() && !snapshot.malformed_ingress_reason.has_value();
    case StopRequestedSelectorKindDTO::kMalformedIngress:
      return !snapshot.selector.has_value() && snapshot.malformed_ingress_reason.has_value() &&
             !snapshot.malformed_ingress_reason->empty() &&
             snapshot.malformed_ingress_reason->size() <= kMaximumStopReasonBytes;
  }
  return false;
}

[[nodiscard]] bool selector_matches_reason(const StopRequestedSelectorSnapshotDTO& snapshot,
                                           StopScopeResolutionReasonDTO reason) noexcept {
  if (snapshot.kind == StopRequestedSelectorKindDTO::kExplicitFull) {
    return reason == StopScopeResolutionReasonDTO::kExplicitFullStop;
  }
  if (snapshot.kind == StopRequestedSelectorKindDTO::kMalformedIngress) {
    return reason == StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent;
  }
  return reason != StopScopeResolutionReasonDTO::kExplicitFullStop &&
         reason != StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent;
}

[[nodiscard]] bool phase_rows_valid(
    const std::vector<StopAcceptedPhaseBudgetDTO>& phases) noexcept {
  if (phases.empty() || phases.size() > kMaximumStopAcceptedPhases) {
    return false;
  }
  std::int64_t prior_fixed = 0;
  for (std::size_t index = 0U; index < phases.size(); ++index) {
    const auto& phase = phases[index];
    if (!bounded_identity(phase.phase_id) || phase.fixed_deadline_offset_ns <= prior_fixed ||
        phase.fixed_deadline_offset_ns > kMaximumStopDurationNs ||
        phase.remaining_deadline_offset_ns < 0 ||
        phase.remaining_deadline_offset_ns > phase.fixed_deadline_offset_ns) {
      return false;
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (phases[prior].phase_id == phase.phase_id) {
        return false;
      }
    }
    prior_fixed = phase.fixed_deadline_offset_ns;
  }
  return true;
}

[[nodiscard]] bool participant_facts_valid(const StopParticipantScopeFactsDTO& facts) noexcept {
  return facts.operation_revision_after_acceptance.nonzero() &&
         bounded_identity(facts.controllable_component_id) &&
         bounded_identity(facts.command_target_id) && bounded_identity(facts.capability_id) &&
         bounded_identity(facts.control_domain_id) &&
         bounded_identity(facts.control_domain_revision) &&
         bounded_identity(facts.control_binding_id) &&
         bounded_identity(facts.control_binding_revision) &&
         bounded_identity(facts.controller_goal_id) && facts.controller_goal_generation.nonzero() &&
         bounded_identity(facts.active_control_configuration_id) &&
         facts.active_control_configuration_generation.nonzero() &&
         bounded_identity(facts.controller_topology_id) &&
         bounded_identity(facts.topology_revision) && bounded_identity(facts.control_route_id) &&
         bounded_identity(facts.provider_runtime_epoch) && !facts.resources.empty() &&
         facts.resources.size() <= kMaximumStopSelectorTargets &&
         granularity_rank(facts.containment_granularity).has_value() &&
         static_cast<std::uint8_t>(facts.terminal_settlement) <=
             static_cast<std::uint8_t>(
                 StopParticipantScopeFactsDTO::TerminalSettlementState::kContainedUnsettled) &&
         canonical_identities(facts.resources);
}

[[nodiscard]] bool scope_entry_valid(const StopScopeEntryDTO& entry) noexcept {
  if (!key_valid(entry.key) || entry.accepted_at_ns < 0 || entry.remaining_outer_duration_ns < 0 ||
      entry.remaining_outer_duration_ns > kMaximumStopDurationNs ||
      (entry.stop_evidence_policy_id.has_value() !=
       entry.stop_evidence_policy_revision.has_value())) {
    return false;
  }

  const auto* participant_key = std::get_if<StopParticipantScopeKeyDTO>(&entry.key.subject);
  const auto* participant_facts = std::get_if<StopParticipantScopeFactsDTO>(&entry.facts);
  if ((participant_key != nullptr) != (participant_facts != nullptr)) {
    return false;
  }
  if (participant_facts != nullptr) {
    if (!participant_facts_valid(*participant_facts) || !phase_rows_valid(entry.phases)) {
      return false;
    }
  } else {
    if (std::get<ResourceId>(entry.key.subject) != std::get<ResourceId>(entry.facts) ||
        entry.disposition != StopScopeEntryDispositionDTO::kNotStoppable || !entry.phases.empty()) {
      return false;
    }
  }

  if (entry.stop_evidence_policy_id.has_value() &&
      (!bounded_identity(*entry.stop_evidence_policy_id) ||
       !bounded_identity(*entry.stop_evidence_policy_revision))) {
    return false;
  }
  if (entry.disposition == StopScopeEntryDispositionDTO::kStoppable) {
    return participant_key != nullptr && entry.stop_evidence_policy_id.has_value() &&
           entry.participant_stop_invocation_plan_digest.has_value() &&
           digest_valid(*entry.participant_stop_invocation_plan_digest);
  }
  return !entry.participant_stop_invocation_plan_digest.has_value();
}

[[nodiscard]] std::optional<std::size_t> stage_index_of(
    const StopAttemptedStageResultDTO& attempt) noexcept {
  const auto stage_index = attempt.result.request.stage_index;
  if (stage_index >= attempt.result.request.participant_stop_plan.stage_commands.size()) {
    return std::nullopt;
  }
  return stage_index;
}

[[nodiscard]] bool attempt_less(const StopAttemptedStageResultDTO& left,
                                const StopAttemptedStageResultDTO& right) noexcept {
  if (key_less(left.scope_key, right.scope_key)) {
    return true;
  }
  if (key_less(right.scope_key, left.scope_key)) {
    return false;
  }
  const auto left_stage = left.result.request.stage_index;
  const auto right_stage = right.result.request.stage_index;
  return std::tie(left_stage, left.result_sequence) < std::tie(right_stage, right.result_sequence);
}

[[nodiscard]] bool attempt_matches_entry(const StopAttemptedStageResultDTO& attempt,
                                         const StopScopeEntryDTO& entry) noexcept {
  const auto* key = std::get_if<StopParticipantScopeKeyDTO>(&entry.key.subject);
  const auto* facts = std::get_if<StopParticipantScopeFactsDTO>(&entry.facts);
  const auto stage_index = stage_index_of(attempt);
  const auto stage_granularity = granularity_rank(attempt.result.request.containment_granularity);
  const auto entry_granularity =
      facts == nullptr ? std::nullopt : granularity_rank(facts->containment_granularity);
  if (key == nullptr || facts == nullptr || !stage_index.has_value() ||
      !stage_granularity.has_value() || !entry_granularity.has_value() ||
      attempt.result_sequence == 0U || attempt.completed_at_ns < 0 ||
      attempt.result.request.stop_id != entry.key.origin_stop_id ||
      attempt.result.request.stop_generation != entry.key.origin_stop_generation ||
      attempt.result.request.operation_id != key->operation_id ||
      attempt.result.request.participant_id != key->participant_id ||
      attempt.result.request.operation_revision >= facts->operation_revision_after_acceptance ||
      attempt.result.request.controllable_component_id != facts->controllable_component_id ||
      attempt.result.request.command_target_id != facts->command_target_id ||
      attempt.result.request.capability_id != facts->capability_id ||
      attempt.result.request.control_domain_id != facts->control_domain_id ||
      attempt.result.request.control_domain_revision != facts->control_domain_revision ||
      attempt.result.request.control_binding_id != facts->control_binding_id ||
      attempt.result.request.control_binding_revision != facts->control_binding_revision ||
      attempt.result.request.controller_goal_id != facts->controller_goal_id ||
      attempt.result.request.controller_goal_generation != facts->controller_goal_generation ||
      attempt.result.request.active_control_configuration_id !=
          facts->active_control_configuration_id ||
      attempt.result.request.active_control_configuration_generation !=
          facts->active_control_configuration_generation ||
      attempt.result.request.controller_topology_id != facts->controller_topology_id ||
      attempt.result.request.topology_revision != facts->topology_revision ||
      attempt.result.request.control_route_id != facts->control_route_id ||
      attempt.result.request.provider_runtime_epoch != facts->provider_runtime_epoch ||
      attempt.result.request.resources != facts->resources ||
      *stage_granularity > *entry_granularity ||
      !entry.participant_stop_invocation_plan_digest.has_value() ||
      attempt.result.request.participant_stop_plan.participant_stop_invocation_plan_digest !=
          *entry.participant_stop_invocation_plan_digest) {
    return false;
  }
  const auto& stage =
      attempt.result.request.participant_stop_plan.stage_commands[*stage_index].resolved_stage;
  if (stage.stage_index != *stage_index ||
      attempt.result.request.stop_evidence_policy_id != *entry.stop_evidence_policy_id ||
      attempt.result.request.stop_evidence_policy_revision !=
          *entry.stop_evidence_policy_revision ||
      attempt.result.evidence_items.size() > kMaxStopEvidencePolicyItems ||
      (attempt.result.error.has_value() && !error_valid(*attempt.result.error))) {
    return false;
  }
  if (!std::all_of(attempt.result.evidence_items.begin(), attempt.result.evidence_items.end(),
                   [](const auto& item) {
                     return item.state != StopEvidenceItemStateDTO::kRequiredPending &&
                            (!item.error.has_value() || error_valid(*item.error));
                   })) {
    return false;
  }
  if (attempt.result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
    return !attempt.result.error.has_value() &&
           std::all_of(
               attempt.result.evidence_items.begin(), attempt.result.evidence_items.end(),
               [](const auto& item) { return item.state == StopEvidenceItemStateDTO::kSatisfied; });
  }
  if (attempt.result.outcome == ParticipantStopStageOutcomeDTO::kSupersededNoEffect) {
    return attempt.result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched &&
           attempt.result.error.has_value() &&
           std::all_of(attempt.result.evidence_items.begin(), attempt.result.evidence_items.end(),
                       [](const auto& item) {
                         return item.state == StopEvidenceItemStateDTO::kInapplicable;
                       });
  }
  return attempt.result.error.has_value() &&
         std::none_of(attempt.result.evidence_items.begin(), attempt.result.evidence_items.end(),
                      [](const auto& item) {
                        return item.state == StopEvidenceItemStateDTO::kInapplicable;
                      });
}

[[nodiscard]] bool evidence_less(const StopCertificateEvidenceItemDTO& left,
                                 const StopCertificateEvidenceItemDTO& right) noexcept {
  if (key_less(left.scope_key, right.scope_key)) {
    return true;
  }
  if (key_less(right.scope_key, left.scope_key)) {
    return false;
  }
  return std::tie(left.stage_index, left.result_sequence, left.evidence_item.evidence_item_id) <
         std::tie(right.stage_index, right.result_sequence, right.evidence_item.evidence_item_id);
}

[[nodiscard]] bool certificate_ref_less(const StopCertificateRefDTO& left,
                                        const StopCertificateRefDTO& right) noexcept {
  return left.stop_id < right.stop_id;
}

[[nodiscard]] bool origin_allowed(const StopScopeEntryKeyDTO& key, const StopId& current_stop,
                                  StopGeneration current_generation,
                                  const std::vector<StopCertificateRefDTO>& predecessors) noexcept {
  if (key.origin_stop_id == current_stop && key.origin_stop_generation == current_generation) {
    return true;
  }
  const auto found = std::lower_bound(
      predecessors.begin(), predecessors.end(), key.origin_stop_id,
      [](const auto& reference, const StopId& stop_id) { return reference.stop_id < stop_id; });
  return found != predecessors.end() && found->stop_id == key.origin_stop_id &&
         found->stop_generation == key.origin_stop_generation;
}

[[nodiscard]] bool capture_predecessors_match(
    const std::vector<StopScopeCaptureHistoryDTO>& history,
    const std::vector<StopCertificateRefDTO>& predecessors) noexcept {
  if (history.empty()) {
    return false;
  }
  const auto& captured = history.back().predecessor_stops;
  if (captured.size() != predecessors.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < captured.size(); ++index) {
    if (captured[index].predecessor_stop_id != predecessors[index].stop_id ||
        captured[index].predecessor_stop_generation != predecessors[index].stop_generation) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool capture_history_valid(const std::vector<StopScopeCaptureHistoryDTO>& history,
                                         StopResolutionDispositionDTO final_disposition,
                                         StopScopeResolutionReasonDTO final_reason,
                                         const StopId& current_stop) noexcept {
  if (history.empty() || history.size() > kMaximumStopScopeCaptureHistory) {
    return false;
  }
  for (std::size_t index = 0U; index < history.size(); ++index) {
    const auto& row = history[index];
    if (row.capture_sequence != index + 1U ||
        !reason_matches_disposition(row.resolution_disposition, row.resolution_reason) ||
        !digest_valid(row.captured_scope_digest) ||
        row.predecessor_stops.size() > kMaximumStopTerminalPredecessors ||
        !canonical_rows(row.predecessor_stops, predecessor_less) ||
        !std::all_of(row.predecessor_stops.begin(), row.predecessor_stops.end(),
                     [&](const auto& predecessor) {
                       return predecessor_valid(predecessor, current_stop);
                     }) ||
        row.newly_fenced_operation_revisions.size() > kMaximumStopTerminalScopeEntries ||
        !canonical_rows(row.newly_fenced_operation_revisions,
                        [](const auto& left, const auto& right) {
                          return left.operation_id < right.operation_id;
                        }) ||
        !std::all_of(row.newly_fenced_operation_revisions.begin(),
                     row.newly_fenced_operation_revisions.end(),
                     [](const auto& operation) {
                       return bounded_identity(operation.operation_id) &&
                              operation.operation_revision_after_acceptance.nonzero();
                     }) ||
        row.newly_captured_resources.size() > kMaximumStopTerminalScopeEntries ||
        !canonical_identities(row.newly_captured_resources)) {
      return false;
    }
  }
  return history.back().resolution_disposition == final_disposition &&
         history.back().resolution_reason == final_reason;
}

[[nodiscard]] bool correlation_valid(const StopInterruptCorrelationDTO& correlation) noexcept {
  return bounded_identity(correlation.interrupt_source_id) &&
         bounded_identity(correlation.interrupt_policy_id) &&
         bounded_identity(correlation.motion_interrupt_scope_id) &&
         (!correlation.interrupt_latch_id.has_value() ||
          bounded_identity(*correlation.interrupt_latch_id));
}

struct DerivedTerminal final {
  StopCertificateResultDTO result{StopCertificateResultDTO::kSucceededNominal};
  std::vector<StopScopeEntryKeyDTO> pre_dispatch_cancelled;
  std::vector<StopScopeEntryKeyDTO> dispatched_stopped;
  std::vector<StopScopeEntryKeyDTO> unsettled;
  std::vector<StopScopeEntryKeyDTO> failed;
  std::vector<StopCertificateEvidenceItemDTO> evidence;
};

[[nodiscard]] Result<DerivedTerminal> derive_terminal(
    const std::vector<StopScopeEntryDTO>& entries,
    const std::vector<StopAttemptedStageResultDTO>& attempts) {
  DerivedTerminal derived;
  bool used_stronger_stage = false;

  for (const auto& attempt : attempts) {
    const auto found = std::lower_bound(
        entries.begin(), entries.end(), attempt.scope_key,
        [](const auto& entry, const auto& key) { return key_less(entry.key, key); });
    if (found == entries.end() || found->key != attempt.scope_key ||
        !attempt_matches_entry(attempt, *found)) {
      return Result<DerivedTerminal>::Err(
          ErrorDTO{"stop_terminal_attempt_join_invalid",
                   "Stop stage result does not join its immutable terminal scope entry",
                   {}});
    }
    const auto stage_index = *stage_index_of(attempt);
    const auto& stage =
        attempt.result.request.participant_stop_plan.stage_commands[stage_index].resolved_stage;
    for (const auto& evidence : attempt.result.evidence_items) {
      derived.evidence.push_back(
          StopCertificateEvidenceItemDTO{attempt.scope_key, stage.stop_escalation_stage_id,
                                         stage_index, attempt.result_sequence, evidence});
    }
  }
  if (derived.evidence.size() > kMaximumStopTerminalEvidenceItems) {
    return Result<DerivedTerminal>::Err(
        ErrorDTO{"stop_terminal_evidence_capacity_exceeded",
                 "Stop terminal evidence exceeds its preflighted bound",
                 {}});
  }
  std::sort(derived.evidence.begin(), derived.evidence.end(), evidence_less);
  if (!canonical_rows(derived.evidence, evidence_less)) {
    return Result<DerivedTerminal>::Err(
        ErrorDTO{"stop_terminal_evidence_conflict",
                 "Stop terminal evidence contains a conflicting duplicate",
                 {}});
  }

  for (const auto& entry : entries) {
    if (entry.dispatch_state == StopScopeDispatchStateDTO::kNeverDispatchedCancelled) {
      const auto first = std::lower_bound(
          attempts.begin(), attempts.end(), entry.key,
          [](const auto& attempt, const auto& key) { return key_less(attempt.scope_key, key); });
      if (first != attempts.end() && first->scope_key == entry.key) {
        return Result<DerivedTerminal>::Err(
            ErrorDTO{"stop_terminal_cancelled_entry_has_attempt",
                     "A pre-dispatch-cancelled scope entry cannot carry a stage attempt",
                     {}});
      }
      derived.pre_dispatch_cancelled.push_back(entry.key);
      const auto* participant_facts = std::get_if<StopParticipantScopeFactsDTO>(&entry.facts);
      const bool participant_settled_or_absent =
          participant_facts != nullptr &&
          (participant_facts->terminal_settlement ==
               StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled ||
           participant_facts->terminal_settlement ==
               StopParticipantScopeFactsDTO::TerminalSettlementState::kNotRequired);
      if (!participant_settled_or_absent) {
        // Closing the dispatch barrier proves absence of a physical effect;
        // it does not let the addressed owner invent the subordinate's local
        // terminal/recovery fact. Aggregate success waits for that exact
        // process-local settlement just as it does after physical containment.
        // kNotRequired is reserved for the narrower case where no subordinate
        // execution identity was ever created.
        derived.unsettled.push_back(entry.key);
      }
      continue;
    }

    const StopAttemptedStageResultDTO* latest = nullptr;
    bool duplicate_authoritative_stage = false;
    std::optional<std::size_t> prior_authoritative_stage;
    for (const auto& attempt : attempts) {
      if (attempt.scope_key != entry.key ||
          attempt.authority != StopStageResultAuthorityDTO::kAuthoritative) {
        continue;
      }
      const auto stage_index = *stage_index_of(attempt);
      if (prior_authoritative_stage.has_value() && *prior_authoritative_stage == stage_index) {
        duplicate_authoritative_stage = true;
      }
      prior_authoritative_stage = stage_index;
      if (latest == nullptr || latest->result.request.stage_index < stage_index) {
        latest = &attempt;
      }
    }
    if (duplicate_authoritative_stage) {
      return Result<DerivedTerminal>::Err(
          ErrorDTO{"stop_terminal_authoritative_stage_conflict",
                   "A scope stage has more than one authoritative result",
                   {}});
    }

    bool physically_contained = false;
    bool explicit_failure = entry.disposition == StopScopeEntryDispositionDTO::kNotStoppable;
    std::optional<std::size_t> prior_superseded_late_stage;
    for (const auto& attempt : attempts) {
      if (attempt.scope_key != entry.key ||
          attempt.authority != StopStageResultAuthorityDTO::kSupersededLate) {
        continue;
      }
      const auto stage_index = *stage_index_of(attempt);
      if (prior_superseded_late_stage.has_value() && *prior_superseded_late_stage == stage_index) {
        return Result<DerivedTerminal>::Err(
            ErrorDTO{"stop_terminal_superseded_stage_conflict",
                     "A scope stage has more than one superseded-late result",
                     {}});
      }
      prior_superseded_late_stage = stage_index;
    }
    if (entry.disposition == StopScopeEntryDispositionDTO::kStoppable && latest != nullptr) {
      physically_contained =
          latest->result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
          !latest->result.error.has_value() &&
          std::all_of(latest->result.evidence_items.begin(), latest->result.evidence_items.end(),
                      [](const auto& evidence) {
                        return evidence.state == StopEvidenceItemStateDTO::kSatisfied;
                      });
      explicit_failure = !physically_contained &&
                         (latest->result.outcome == ParticipantStopStageOutcomeDTO::kPartial ||
                          latest->result.outcome == ParticipantStopStageOutcomeDTO::kFailed);
      if (physically_contained && latest->result.request.stage_index > 0U) {
        used_stronger_stage = true;
      }
    }
    const auto* participant_facts = std::get_if<StopParticipantScopeFactsDTO>(&entry.facts);
    const bool participant_settled =
        participant_facts == nullptr ||
        participant_facts->terminal_settlement ==
            StopParticipantScopeFactsDTO::TerminalSettlementState::kNotRequired ||
        participant_facts->terminal_settlement ==
            StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled;
    if (physically_contained) {
      derived.dispatched_stopped.push_back(entry.key);
    }
    if (!physically_contained || !participant_settled) {
      derived.unsettled.push_back(entry.key);
      if (explicit_failure) {
        derived.failed.push_back(entry.key);
      }
    }
  }

  if (derived.unsettled.empty()) {
    derived.result = used_stronger_stage ? StopCertificateResultDTO::kSucceededEscalated
                                         : StopCertificateResultDTO::kSucceededNominal;
  } else if (!derived.pre_dispatch_cancelled.empty() || !derived.dispatched_stopped.empty()) {
    derived.result = StopCertificateResultDTO::kPartial;
  } else {
    derived.result = StopCertificateResultDTO::kFailedUncontained;
  }
  return Result<DerivedTerminal>::Ok(std::move(derived));
}

[[nodiscard]] bool latch_presence_valid(const StopCertificateDTO& certificate) noexcept {
  if (certificate.fault_latch_id.has_value() != certificate.fault_latch_generation.has_value()) {
    return false;
  }
  const bool required =
      certificate.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull ||
      certificate.result != StopCertificateResultDTO::kSucceededNominal;
  return !required ||
         (certificate.fault_latch_id.has_value() && bounded_identity(*certificate.fault_latch_id) &&
          certificate.fault_latch_generation->nonzero());
}

[[nodiscard]] std::optional<std::string_view> certificate_shape_error(
    const StopCertificateDTO& certificate, bool compare_derived) noexcept {
  if (!bounded_identity(certificate.stop_certificate_id) ||
      !certificate.stop_certificate_revision.nonzero() || !bounded_identity(certificate.stop_id) ||
      !certificate.stop_generation.nonzero() || certificate.completed_at_ns < 0 ||
      !selector_snapshot_valid(certificate.requested_selector) ||
      !selector_matches_reason(certificate.requested_selector, certificate.resolution_reason) ||
      !reason_matches_disposition(certificate.resolution_disposition,
                                  certificate.resolution_reason) ||
      certificate.stale_diagnostics.size() > kMaximumStopStaleDiagnostics ||
      ((certificate.resolution_disposition == StopResolutionDispositionDTO::kExact &&
        !certificate.stale_diagnostics.empty()) ||
       (certificate.resolution_disposition ==
            StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved &&
        certificate.stale_diagnostics.empty())) ||
      !capture_history_valid(certificate.capture_history, certificate.resolution_disposition,
                             certificate.resolution_reason, certificate.stop_id) ||
      (certificate.interrupt_correlation.has_value() &&
       !correlation_valid(*certificate.interrupt_correlation)) ||
      (certificate.terminal_error.has_value() && !error_valid(*certificate.terminal_error)) ||
      !latch_presence_valid(certificate)) {
    return "stop_certificate_identity_or_presence_invalid";
  }
  if (certificate.predecessor_certificates.size() > kMaximumStopTerminalPredecessors ||
      !canonical_rows(certificate.predecessor_certificates, certificate_ref_less) ||
      !std::all_of(certificate.predecessor_certificates.begin(),
                   certificate.predecessor_certificates.end(), [&](const auto& predecessor) {
                     return predecessor.locally_valid() &&
                            predecessor.stop_id != certificate.stop_id;
                   })) {
    return "stop_certificate_predecessors_invalid";
  }
  if (!certificate.predecessor_certificates.empty() &&
      certificate.resolution_disposition != StopResolutionDispositionDTO::kPromotedToFull &&
      !((certificate.resolution_disposition == StopResolutionDispositionDTO::kExact ||
         certificate.resolution_disposition == StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved) &&
        certificate.resolution_reason == StopScopeResolutionReasonDTO::kExplicitFullStop)) {
    return "stop_certificate_predecessor_resolution_invalid";
  }
  if (certificate.scope_entries.size() > kMaximumStopTerminalScopeEntries ||
      !canonical_rows(
          certificate.scope_entries,
          [](const auto& left, const auto& right) { return key_less(left.key, right.key); }) ||
      !std::all_of(certificate.scope_entries.begin(), certificate.scope_entries.end(),
                   scope_entry_valid)) {
    return "stop_certificate_scope_invalid";
  }
  if (!capture_predecessors_match(certificate.capture_history,
                                  certificate.predecessor_certificates) ||
      !std::all_of(certificate.scope_entries.begin(), certificate.scope_entries.end(),
                   [&](const auto& entry) {
                     return origin_allowed(entry.key, certificate.stop_id,
                                           certificate.stop_generation,
                                           certificate.predecessor_certificates);
                   })) {
    return "stop_certificate_origin_join_invalid";
  }
  if (certificate.attempted_stage_results.size() > kMaximumStopTerminalAttemptResults ||
      !canonical_rows(certificate.attempted_stage_results, attempt_less)) {
    return "stop_certificate_attempts_invalid";
  }
  if (certificate.evidence_items.size() > kMaximumStopTerminalEvidenceItems ||
      !canonical_rows(certificate.evidence_items, evidence_less)) {
    return "stop_certificate_evidence_invalid";
  }
  if (!canonical_rows(certificate.pre_dispatch_cancelled, key_less) ||
      !canonical_rows(certificate.dispatched_stopped, key_less) ||
      !canonical_rows(certificate.unsettled, key_less) ||
      !canonical_rows(certificate.failed, key_less)) {
    return "stop_certificate_subsets_invalid";
  }
  if (compare_derived) {
    auto derived = derive_terminal(certificate.scope_entries, certificate.attempted_stage_results);
    if (!derived.ok() || certificate.result != derived.value().result ||
        certificate.pre_dispatch_cancelled != derived.value().pre_dispatch_cancelled ||
        certificate.dispatched_stopped != derived.value().dispatched_stopped ||
        certificate.unsettled != derived.value().unsettled ||
        certificate.failed != derived.value().failed ||
        certificate.evidence_items != derived.value().evidence) {
      return "stop_certificate_reduction_mismatch";
    }
  }
  return std::nullopt;
}

[[nodiscard]] ErrorDTO terminal_input_error(std::string code, std::string message) {
  return ErrorDTO{std::move(code), std::move(message), {}};
}

}  // namespace

std::optional<std::string_view> stop_terminal_reduction_input_field_error(
    const StopTerminalReductionInputDTO& input) noexcept {
  if (!bounded_identity(input.stop_certificate_id) || !input.stop_certificate_revision.nonzero() ||
      !bounded_identity(input.stop_id) || !input.stop_generation.nonzero() ||
      input.completed_at_ns < 0 || !selector_snapshot_valid(input.requested_selector) ||
      !selector_matches_reason(input.requested_selector, input.resolution_reason) ||
      !reason_matches_disposition(input.resolution_disposition, input.resolution_reason) ||
      input.stale_diagnostics.size() > kMaximumStopStaleDiagnostics ||
      ((input.resolution_disposition == StopResolutionDispositionDTO::kExact &&
        !input.stale_diagnostics.empty()) ||
       (input.resolution_disposition ==
            StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved &&
        input.stale_diagnostics.empty())) ||
      !capture_history_valid(input.capture_history, input.resolution_disposition,
                             input.resolution_reason, input.stop_id) ||
      input.predecessor_certificates.size() > kMaximumStopTerminalPredecessors ||
      input.newly_captured_scope_entries.size() > kMaximumStopTerminalScopeEntries ||
      input.newly_completed_stage_results.size() > kMaximumStopTerminalAttemptResults ||
      (input.interrupt_correlation.has_value() &&
       !correlation_valid(*input.interrupt_correlation)) ||
      (input.fault_latch_id.has_value() != input.fault_latch_generation.has_value()) ||
      (input.fault_latch_id.has_value() &&
       (!bounded_identity(*input.fault_latch_id) || !input.fault_latch_generation->nonzero())) ||
      (input.terminal_error.has_value() && !error_valid(*input.terminal_error))) {
    return "stop_terminal_reduction_input_invalid";
  }
  if (!canonical_rows(
          input.predecessor_certificates,
          [](const auto& left, const auto& right) { return left.stop_id < right.stop_id; }) ||
      !std::all_of(input.predecessor_certificates.begin(), input.predecessor_certificates.end(),
                   [&](const auto& predecessor) {
                     return predecessor.stop_id != input.stop_id &&
                            !stop_certificate_field_error(predecessor).has_value();
                   })) {
    return "stop_terminal_predecessor_certificate_invalid";
  }
  if (!input.predecessor_certificates.empty() &&
      input.resolution_disposition != StopResolutionDispositionDTO::kPromotedToFull &&
      !((input.resolution_disposition == StopResolutionDispositionDTO::kExact ||
         input.resolution_disposition == StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved) &&
        input.resolution_reason == StopScopeResolutionReasonDTO::kExplicitFullStop)) {
    return "stop_terminal_predecessor_resolution_invalid";
  }
  if (!canonical_rows(
          input.newly_captured_scope_entries,
          [](const auto& left, const auto& right) { return key_less(left.key, right.key); }) ||
      !std::all_of(input.newly_captured_scope_entries.begin(),
                   input.newly_captured_scope_entries.end(),
                   [&](const auto& entry) {
                     return scope_entry_valid(entry) && entry.key.origin_stop_id == input.stop_id &&
                            entry.key.origin_stop_generation == input.stop_generation;
                   }) ||
      !canonical_rows(input.newly_completed_stage_results, attempt_less)) {
    return "stop_terminal_new_scope_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_certificate_field_error(
    const StopCertificateDTO& certificate) noexcept {
  return certificate_shape_error(certificate, true);
}

Result<StopCertificateDTO> ReduceStopTerminal(const StopTerminalReductionInputDTO& input) {
  if (const auto field = stop_terminal_reduction_input_field_error(input); field.has_value()) {
    return Result<StopCertificateDTO>::Err(
        terminal_input_error(std::string(*field), "Stop terminal reduction input is invalid"));
  }

  std::vector<StopCertificateRefDTO> predecessor_refs;
  std::vector<StopScopeEntryDTO> scope_entries;
  std::vector<StopAttemptedStageResultDTO> attempts;
  for (const auto& predecessor : input.predecessor_certificates) {
    predecessor_refs.push_back(StopCertificateRefDTO{
        predecessor.stop_certificate_id, predecessor.stop_certificate_revision, predecessor.stop_id,
        predecessor.stop_generation});
    predecessor_refs.insert(predecessor_refs.end(), predecessor.predecessor_certificates.begin(),
                            predecessor.predecessor_certificates.end());
    scope_entries.insert(scope_entries.end(), predecessor.scope_entries.begin(),
                         predecessor.scope_entries.end());
    attempts.insert(attempts.end(), predecessor.attempted_stage_results.begin(),
                    predecessor.attempted_stage_results.end());
  }
  predecessor_refs.erase(
      std::remove_if(predecessor_refs.begin(), predecessor_refs.end(),
                     [&](const auto& reference) { return reference.stop_id == input.stop_id; }),
      predecessor_refs.end());
  std::sort(predecessor_refs.begin(), predecessor_refs.end(), certificate_ref_less);
  predecessor_refs.erase(
      std::unique(predecessor_refs.begin(), predecessor_refs.end(),
                  [](const auto& left, const auto& right) {
                    return left.stop_id == right.stop_id &&
                           left.stop_generation == right.stop_generation &&
                           left.stop_certificate_id == right.stop_certificate_id &&
                           left.stop_certificate_revision == right.stop_certificate_revision;
                  }),
      predecessor_refs.end());
  if (predecessor_refs.size() > kMaximumStopTerminalPredecessors ||
      !canonical_rows(predecessor_refs, certificate_ref_less)) {
    return Result<StopCertificateDTO>::Err(
        terminal_input_error("stop_terminal_predecessor_conflict",
                             "Stop predecessor certificates conflict or exceed capacity"));
  }
  if (!capture_predecessors_match(input.capture_history, predecessor_refs)) {
    return Result<StopCertificateDTO>::Err(
        terminal_input_error("stop_terminal_capture_predecessor_mismatch",
                             "Stop capture history does not name every exact predecessor"));
  }

  scope_entries.insert(scope_entries.end(), input.newly_captured_scope_entries.begin(),
                       input.newly_captured_scope_entries.end());
  attempts.insert(attempts.end(), input.newly_completed_stage_results.begin(),
                  input.newly_completed_stage_results.end());
  std::sort(scope_entries.begin(), scope_entries.end(),
            [](const auto& left, const auto& right) { return key_less(left.key, right.key); });
  std::sort(attempts.begin(), attempts.end(), attempt_less);
  if (scope_entries.size() > kMaximumStopTerminalScopeEntries ||
      !canonical_rows(
          scope_entries,
          [](const auto& left, const auto& right) { return key_less(left.key, right.key); }) ||
      attempts.size() > kMaximumStopTerminalAttemptResults ||
      !canonical_rows(attempts, attempt_less)) {
    return Result<StopCertificateDTO>::Err(
        terminal_input_error("stop_terminal_merged_scope_conflict",
                             "Merged Stop predecessor and fresh scope truth conflicts"));
  }
  if (!std::all_of(scope_entries.begin(), scope_entries.end(), [&](const auto& entry) {
        return origin_allowed(entry.key, input.stop_id, input.stop_generation, predecessor_refs);
      })) {
    return Result<StopCertificateDTO>::Err(terminal_input_error(
        "stop_terminal_scope_origin_invalid", "Merged Stop scope has an unlisted origin Stop"));
  }

  auto derived = derive_terminal(scope_entries, attempts);
  if (!derived.ok()) {
    return Result<StopCertificateDTO>::Err(derived.error());
  }
  const bool latch_required =
      input.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull ||
      derived.value().result != StopCertificateResultDTO::kSucceededNominal;
  if (latch_required && !input.fault_latch_id.has_value()) {
    return Result<StopCertificateDTO>::Err(
        terminal_input_error("stop_terminal_fault_latch_required",
                             "Stop terminal outcome requires an exact containment-fault latch"));
  }

  StopCertificateDTO certificate{input.stop_certificate_id,
                                 input.stop_certificate_revision,
                                 input.stop_id,
                                 input.stop_generation,
                                 input.requested_selector,
                                 input.resolution_disposition,
                                 input.resolution_reason,
                                 input.stale_diagnostics,
                                 input.capture_history,
                                 std::move(predecessor_refs),
                                 std::move(scope_entries),
                                 std::move(attempts),
                                 std::move(derived.value().evidence),
                                 derived.value().result,
                                 std::move(derived.value().pre_dispatch_cancelled),
                                 std::move(derived.value().dispatched_stopped),
                                 std::move(derived.value().unsettled),
                                 std::move(derived.value().failed),
                                 input.interrupt_correlation,
                                 input.fault_latch_id,
                                 input.fault_latch_generation,
                                 input.completed_at_ns,
                                 input.terminal_error};
  if (const auto field = stop_certificate_field_error(certificate); field.has_value()) {
    return Result<StopCertificateDTO>::Err(
        terminal_input_error(std::string(*field), "Reduced Stop certificate is invalid"));
  }
  return Result<StopCertificateDTO>::Ok(std::move(certificate));
}

}  // namespace manyforge::core
