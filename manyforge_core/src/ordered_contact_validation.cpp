#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <manyforge_core/collision/ordered_contact_validation.hpp>
#include <set>
#include <utility>

namespace manyforge::core {

namespace {

CollisionPairId canonicalize(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
}

bool revisions_equal(const CollisionPolicyRevision& left, const CollisionPolicyRevision& right) {
  return left.revision_id == right.revision_id && left.content_hash == right.content_hash;
}

std::optional<double> clearance_for_pair(const CollisionResult& result,
                                         const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  for (const auto& clearance : result.pair_clearances) {
    if (canonicalize(clearance.pair) == canonical) {
      return clearance.signed_clearance_m;
    }
  }
  if (result.min_clearance_pair && result.min_clearance_m &&
      canonicalize(*result.min_clearance_pair) == canonical) {
    return result.min_clearance_m;
  }
  return std::nullopt;
}

bool pair_present(const CollisionValidationWaypointRecord& record, const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  return std::any_of(
      record.result.normalized_pairs.begin(), record.result.normalized_pairs.end(),
      [&](const CollisionPairId& observed) { return canonicalize(observed) == canonical; });
}

bool pair_list_contains(const std::vector<CollisionPairId>& pairs, const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  return std::any_of(pairs.begin(), pairs.end(),
                     [&](const auto& candidate) { return canonicalize(candidate) == canonical; });
}

constexpr std::array<SupportContactEvaluationState, 5> kSupportStates{
    SupportContactEvaluationState::kNotInContact,
    SupportContactEvaluationState::kInitialContact,
    SupportContactEvaluationState::kContactActive,
    SupportContactEvaluationState::kSeparated,
    SupportContactEvaluationState::kRecontactViolation,
};

std::size_t support_state_index(SupportContactEvaluationState state) {
  const auto it = std::find(kSupportStates.begin(), kSupportStates.end(), state);
  return it == kSupportStates.end() ? 0 : static_cast<std::size_t>(it - kSupportStates.begin());
}

SupportContactTransition simulate_support_transition(
    const std::vector<CollisionValidationWaypointRecord>& records, std::size_t global_offset,
    const SupportContactPolicyEntry& policy, SupportContactEvaluationState entry_state) {
  SupportContactTransition transition;
  auto state = entry_state;
  const auto pair = canonicalize(policy.pair);
  for (const auto& record : records) {
    const bool present = pair_present(record, pair);
    const bool diagnostics_complete = record.result.engine_status == CollisionEngineStatus::kOk;
    if (present) {
      // CollisionPolicyEvaluator intentionally does not mutate support state
      // from an incomplete observed pair. The engine failure remains an
      // unconditional fail-closed result.
      if (!diagnostics_complete) {
        continue;
      }
      switch (state) {
        case SupportContactEvaluationState::kNotInContact:
          state = SupportContactEvaluationState::kInitialContact;
          break;
        case SupportContactEvaluationState::kInitialContact:
        case SupportContactEvaluationState::kContactActive:
          state = SupportContactEvaluationState::kContactActive;
          break;
        case SupportContactEvaluationState::kSeparated:
          if (policy.mode ==
                  CollisionContactPolicyMode::kBoundedPenetration ||
              policy.allow_recontact) {
            state = SupportContactEvaluationState::kContactActive;
          } else {
            state = SupportContactEvaluationState::kRecontactViolation;
            if (!transition.first_recontact_violation_waypoint) {
              transition.first_recontact_violation_waypoint =
                  static_cast<std::int64_t>(global_offset) + record.waypoint_index;
            }
          }
          break;
        case SupportContactEvaluationState::kRecontactViolation:
          if (!transition.first_recontact_violation_waypoint) {
            transition.first_recontact_violation_waypoint =
                static_cast<std::int64_t>(global_offset) + record.waypoint_index;
          }
          break;
      }
    } else if (state == SupportContactEvaluationState::kInitialContact ||
               state == SupportContactEvaluationState::kContactActive) {
      state = SupportContactEvaluationState::kSeparated;
    }
  }
  transition.exit_state = state;
  return transition;
}

ContactReleaseSampleSummary release_sample(const CollisionValidationWaypointRecord& record,
                                           std::size_t global_offset,
                                           const SupportContactPolicyEntry& policy) {
  ContactReleaseSampleSummary sample;
  sample.waypoint_index = static_cast<std::int64_t>(global_offset) + record.waypoint_index;
  sample.time_from_start_ns = record.time_from_start_ns;
  sample.in_contact = pair_present(record, policy.pair);
  sample.separated = !sample.in_contact;
  const bool metric_required =
      policy.mode == CollisionContactPolicyMode::kBoundedPenetration ||
      (policy.mode == CollisionContactPolicyMode::kContactRelease &&
       policy.release_evidence_mode ==
           ContactReleaseEvidenceMode::kSignedPenetration);
  if (!sample.in_contact || !metric_required) {
    return sample;
  }
  const auto clearance = clearance_for_pair(record.result, policy.pair);
  if (!clearance || !std::isfinite(*clearance) || *clearance > 0.0) {
    return sample;
  }
  sample.clearance_valid = true;
  sample.signed_clearance_m = *clearance;
  sample.penetration_depth_m = std::max(0.0, -*clearance);
  return sample;
}

ContactReleaseSegmentSummary summarize_release_segment(
    const std::vector<ContactReleaseSampleSummary>& samples, std::size_t begin,
    const SupportContactPolicyEntry& policy) {
  ContactReleaseSegmentSummary summary;
  if (begin >= samples.size()) {
    return summary;
  }
  summary.empty = false;
  summary.first_sample = samples[begin];
  const bool bounded_penetration =
      policy.mode == CollisionContactPolicyMode::kBoundedPenetration;
  const bool signed_penetration =
      policy.mode == CollisionContactPolicyMode::kContactRelease &&
      policy.release_evidence_mode ==
          ContactReleaseEvidenceMode::kSignedPenetration;
  const bool metric_required = bounded_penetration || signed_penetration;
  const double tolerance = std::max(0.0, policy.release_tolerance_m);
  const double max_start = std::max(0.0, policy.max_start_penetration_m);
  bool saw_separation = false;
  std::optional<double> previous_depth;

  for (std::size_t i = begin; i < samples.size(); ++i) {
    const auto& sample = samples[i];
    summary.last_sample = sample;
    if (metric_required && sample.in_contact &&
        !sample.clearance_valid) {
      summary.first_missing_clearance = sample;
      break;
    }

    const bool active = sample.in_contact;
    if (active && !summary.first_active_contact) {
      summary.first_active_contact = sample;
    }
    if (active) {
      summary.last_active_contact = sample;
    }
    if (signed_penetration && active &&
        sample.penetration_depth_m > max_start + tolerance &&
        !summary.first_active_contact_exceeding_max_start) {
      summary.first_active_contact_exceeding_max_start = sample;
    }
    if (bounded_penetration && active &&
        sample.penetration_depth_m >
            std::max(0.0, policy.max_contact_penetration_m) &&
        !summary.first_active_contact_exceeding_contact_bound) {
      summary.first_active_contact_exceeding_contact_bound = sample;
    }
    if (sample.separated) {
      if (!summary.first_separation) {
        summary.first_separation = sample;
      }
      saw_separation = true;
    } else if (active && saw_separation) {
      if (!summary.first_active_contact_after_separation) {
        summary.first_active_contact_after_separation = sample;
      }
      if (signed_penetration &&
          sample.penetration_depth_m > max_start + tolerance &&
          !summary.first_active_contact_exceeding_max_start_after_separation) {
        summary.first_active_contact_exceeding_max_start_after_separation = sample;
      }
    }

    if (signed_penetration) {
      const double effective_depth =
          sample.in_contact ? sample.penetration_depth_m : 0.0;
      if (sample.in_contact) {
        if (previous_depth &&
            sample.penetration_depth_m > *previous_depth + tolerance &&
            !summary.first_internal_depth_increase) {
          summary.first_internal_depth_increase = sample;
        }
        previous_depth = sample.penetration_depth_m;
        summary.last_effective_depth_m = sample.penetration_depth_m;
      }
      summary.minimum_effective_depth_m =
          summary.minimum_effective_depth_m
              ? std::min(*summary.minimum_effective_depth_m,
                         effective_depth)
              : effective_depth;
    }
  }
  summary.saw_separation = saw_separation;
  return summary;
}

CollisionDecisionSeverity severity_for(CollisionDecisionResult decision) {
  switch (decision) {
    case CollisionDecisionResult::kPass:
      return CollisionDecisionSeverity::kNone;
    case CollisionDecisionResult::kWarn:
      return CollisionDecisionSeverity::kWarn;
    case CollisionDecisionResult::kFail:
    case CollisionDecisionResult::kStopRequested:
    case CollisionDecisionResult::kInconclusiveFail:
      return CollisionDecisionSeverity::kBlock;
  }
  return CollisionDecisionSeverity::kBlock;
}

CollisionDecisionAction action_for(CollisionDecisionResult decision) {
  switch (decision) {
    case CollisionDecisionResult::kPass:
      return CollisionDecisionAction::kDispatch;
    case CollisionDecisionResult::kWarn:
      return CollisionDecisionAction::kWarnOnly;
    case CollisionDecisionResult::kFail:
    case CollisionDecisionResult::kStopRequested:
      return CollisionDecisionAction::kReject;
    case CollisionDecisionResult::kInconclusiveFail:
      return CollisionDecisionAction::kInconclusive;
  }
  return CollisionDecisionAction::kInconclusive;
}

std::string pair_text(const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  return canonical.body_a + " <-> " + canonical.body_b;
}

using ReleaseMergeState = ContactReleaseEvaluationState;

struct RankedViolation {
  ContactReleaseViolation violation;
  int tie_priority = 0;
};

void consider_violation(std::optional<RankedViolation>& selected, ContactReleaseViolation candidate,
                        int tie_priority) {
  if (candidate.waypoint_index < 0) {
    return;
  }
  if (!selected || candidate.waypoint_index < selected->violation.waypoint_index ||
      (candidate.waypoint_index == selected->violation.waypoint_index &&
       tie_priority < selected->tie_priority)) {
    selected = RankedViolation{std::move(candidate), tie_priority};
  }
}

ContactReleaseViolation make_release_violation(std::size_t policy_entry_index,
                                               const ContactReleaseSampleSummary& sample,
                                               CollisionDecisionResult decision, std::string reason,
                                               const ReleaseMergeState& state) {
  ContactReleaseViolation violation;
  violation.policy_entry_index = policy_entry_index;
  violation.waypoint_index = sample.waypoint_index;
  violation.time_from_start_ns = sample.time_from_start_ns;
  violation.decision = decision;
  violation.reason = std::move(reason);
  violation.initial_depth_m = state.initial_depth_m;
  if (sample.clearance_valid) {
    violation.current_depth_m = sample.penetration_depth_m;
  }
  return violation;
}

std::optional<ContactReleaseViolation> apply_release_segment(
    std::size_t policy_entry_index, const SupportContactPolicyEntry& policy,
    const ContactReleaseSegmentSummary& segment, ReleaseMergeState& state) {
  if (segment.empty) {
    return std::nullopt;
  }
  const double tolerance = std::max(0.0, policy.release_tolerance_m);
  const bool bounded_penetration =
      policy.mode == CollisionContactPolicyMode::kBoundedPenetration;
  const bool signed_penetration =
      policy.mode == CollisionContactPolicyMode::kContactRelease &&
      policy.release_evidence_mode ==
          ContactReleaseEvidenceMode::kSignedPenetration;
  const bool metric_required = bounded_penetration || signed_penetration;
  std::optional<RankedViolation> selected;

  if (metric_required && segment.first_missing_clearance) {
    consider_violation(selected,
                       make_release_violation(policy_entry_index, *segment.first_missing_clearance,
                                              CollisionDecisionResult::kInconclusiveFail,
                                              "missing_contact_release_clearance", state),
                       0);
  }

  if (bounded_penetration &&
      segment.first_active_contact_exceeding_contact_bound) {
    consider_violation(
        selected,
        make_release_violation(
            policy_entry_index,
            *segment.first_active_contact_exceeding_contact_bound,
            CollisionDecisionResult::kFail,
            "contact_penetration_exceeds_policy", state),
        1);
  }

  if (bounded_penetration) {
    if (selected) {
      return selected->violation;
    }
    if (segment.saw_separation) {
      state.separated = true;
    }
    if (segment.last_sample) {
      state.in_contact = segment.last_sample->in_contact;
    }
    return std::nullopt;
  }

  if (!state.started_in_contact) {
    if (segment.first_active_contact) {
      consider_violation(selected,
                         make_release_violation(policy_entry_index, *segment.first_active_contact,
                                                CollisionDecisionResult::kFail,
                                                "new_active_contact_after_clear_start", state),
                         1);
    }
  } else {
    std::optional<ContactReleaseSampleSummary> first_recontact;
    std::optional<ContactReleaseSampleSummary> first_recontact_over_max;
    if (state.separated) {
      first_recontact = segment.first_active_contact;
      first_recontact_over_max = segment.first_active_contact_exceeding_max_start;
    } else {
      first_recontact = segment.first_active_contact_after_separation;
      first_recontact_over_max = segment.first_active_contact_exceeding_max_start_after_separation;
    }
    if (first_recontact && !policy.allow_recontact) {
      consider_violation(
          selected,
          make_release_violation(policy_entry_index, *first_recontact,
                                 CollisionDecisionResult::kFail, "recontact_after_release", state),
          1);
    }
    if (signed_penetration && first_recontact_over_max &&
        policy.allow_recontact &&
        policy.enforce_max_start_penetration) {
      consider_violation(selected,
                         make_release_violation(policy_entry_index, *first_recontact_over_max,
                                                CollisionDecisionResult::kFail,
                                                "recontact_penetration_exceeds_policy", state),
                         2);
    }

    if (signed_penetration && segment.first_active_contact &&
        segment.first_active_contact->clearance_valid &&
        segment.first_active_contact->penetration_depth_m >
            state.previous_depth_m + tolerance) {
      consider_violation(
          selected,
          make_release_violation(policy_entry_index,
                                 *segment.first_active_contact,
                                 CollisionDecisionResult::kFail, "penetration_increased", state),
          3);
    }
    if (signed_penetration && segment.first_internal_depth_increase) {
      consider_violation(
          selected,
          make_release_violation(policy_entry_index, *segment.first_internal_depth_increase,
                                 CollisionDecisionResult::kFail, "penetration_increased", state),
          3);
    }
  }

  if (selected) {
    return selected->violation;
  }
  if (segment.saw_separation) {
    state.separated = true;
  }
  if (segment.last_sample) {
    state.in_contact = segment.last_sample->in_contact;
  }
  if (signed_penetration && segment.minimum_effective_depth_m) {
    state.best_depth_m = std::min(state.best_depth_m, *segment.minimum_effective_depth_m);
  }
  if (signed_penetration && segment.last_active_contact &&
      segment.last_active_contact->clearance_valid) {
    state.previous_depth_m =
        segment.last_active_contact->penetration_depth_m;
  }
  return std::nullopt;
}

void set_contact_release_diagnostic(CollisionValidationWaypointRecord& record,
                                    const SupportContactPolicyEntry& policy,
                                    const ContactReleaseViolation& violation) {
  record.result.raw_diagnostics["contact_release_violation"] = violation.reason;
  record.result.raw_diagnostics["contact_release_pair"] = pair_text(policy.pair);
  record.result.raw_diagnostics["contact_release_mode"] =
      policy.mode == CollisionContactPolicyMode::kContactProcess
          ? "contact_process"
          : policy.mode == CollisionContactPolicyMode::kBoundedPenetration
                ? "bounded_penetration"
                : "contact_release";
  if (policy.mode == CollisionContactPolicyMode::kBoundedPenetration) {
    record.result.raw_diagnostics[
        "contact_release_max_contact_penetration_m"] =
        std::to_string(policy.max_contact_penetration_m);
  } else if (policy.mode == CollisionContactPolicyMode::kContactRelease) {
    const bool signed_penetration =
        policy.release_evidence_mode ==
        ContactReleaseEvidenceMode::kSignedPenetration;
    record.result.raw_diagnostics["contact_release_evidence_mode"] =
        signed_penetration ? "signed_penetration" : "binary_contact";
    if (signed_penetration) {
      record.result.raw_diagnostics["contact_release_tolerance_m"] =
          std::to_string(policy.release_tolerance_m);
      record.result.raw_diagnostics[
          "contact_release_enforce_max_start_penetration"] =
          policy.enforce_max_start_penetration ? "true" : "false";
      record.result.raw_diagnostics[
          "contact_release_max_start_penetration_m"] =
          std::to_string(policy.max_start_penetration_m);
      record.result.raw_diagnostics["contact_release_min_progress_m"] =
          std::to_string(policy.min_release_progress_m);
    }
  }
  if (violation.initial_depth_m) {
    record.result.raw_diagnostics["contact_release_initial_penetration_m"] =
        std::to_string(*violation.initial_depth_m);
  }
  if (violation.current_depth_m) {
    record.result.raw_diagnostics["contact_release_current_penetration_m"] =
        std::to_string(*violation.current_depth_m);
  }
  for (auto& classified : record.result.normalized_pairs_classified) {
    if (canonicalize(classified.pair) == canonicalize(policy.pair)) {
      if (violation.decision ==
          CollisionDecisionResult::kInconclusiveFail) {
        classified.classification =
            CollisionPairClassification::kDiagnosticIncomplete;
      } else if (policy.mode ==
                 CollisionContactPolicyMode::kBoundedPenetration) {
        classified.classification =
            CollisionPairClassification::kBlockingCollision;
      }
      classified.is_first_failure = true;
      break;
    }
  }
}

}  // namespace

OrderedContactChunkSummary summarize_ordered_contact_chunk(
    const CollisionPolicySnapshot& policy,
    const std::vector<CollisionValidationWaypointRecord>& records, std::size_t global_offset,
    std::size_t range_end) {
  OrderedContactChunkSummary summary;
  summary.range_begin = global_offset;
  summary.range_end = range_end;
  summary.policy_revision = policy.revision;

  if (policy.support_contacts.size() > kMaxOrderedSupportContactIdentities) {
    summary.valid = false;
    summary.invalid_reason = "support_contact_identity_bound_exceeded";
    return summary;
  }
  std::set<CollisionPairId> identities;
  for (const auto& entry : policy.support_contacts) {
    const auto pair = canonicalize(entry.pair);
    if (!identities.insert(pair).second) {
      summary.valid = false;
      summary.invalid_reason = "duplicate_support_contact_identity";
      return summary;
    }

    SupportContactPairChunkSummary support_summary;
    support_summary.pair = pair;
    const bool support_state_shadowed =
        pair_list_contains(policy.allowed_touch_link_pairs, pair) ||
        pair_list_contains(policy.allowed_static_static_pairs, pair);
    for (std::size_t i = 0; i < kSupportStates.size(); ++i) {
      if (support_state_shadowed) {
        support_summary.transition_by_entry_state[i].exit_state = kSupportStates[i];
      } else {
        support_summary.transition_by_entry_state[i] =
            simulate_support_transition(records, global_offset, entry, kSupportStates[i]);
      }
    }
    summary.support_contact_transitions.emplace(pair, std::move(support_summary));

    std::vector<ContactReleaseSampleSummary> samples;
    samples.reserve(records.size());
    for (const auto& record : records) {
      samples.push_back(release_sample(record, global_offset, entry));
    }
    ContactReleasePairChunkSummary release_summary;
    release_summary.pair = pair;
    release_summary.full_segment = summarize_release_segment(samples, 0, entry);
    release_summary.segment_after_first_sample = summarize_release_segment(samples, 1, entry);
    summary.contact_release_summaries.emplace(pair, std::move(release_summary));
  }
  return summary;
}

SupportContactEvaluationStateMap initial_support_contact_state(
    const CollisionPolicySnapshot& policy) {
  SupportContactEvaluationStateMap state;
  for (const auto& entry : policy.support_contacts) {
    state[canonicalize(entry.pair)] = SupportContactEvaluationState::kNotInContact;
  }
  return state;
}

bool advance_support_contact_state(const OrderedContactChunkSummary& summary,
                                   SupportContactEvaluationStateMap& state,
                                   std::string* error_reason) {
  if (!summary.valid) {
    if (error_reason) {
      *error_reason = summary.invalid_reason;
    }
    return false;
  }
  if (summary.support_contact_transitions.size() != state.size()) {
    if (error_reason) {
      *error_reason = "support_contact_summary_identity_mismatch";
    }
    return false;
  }
  for (auto& [pair, current] : state) {
    const auto it = summary.support_contact_transitions.find(pair);
    if (it == summary.support_contact_transitions.end()) {
      if (error_reason) {
        *error_reason = "support_contact_summary_identity_mismatch";
      }
      return false;
    }
    current = it->second.transition_by_entry_state[support_state_index(current)].exit_state;
  }
  return true;
}

std::vector<ContactReleaseViolation> merge_contact_release_summaries(
    const CollisionPolicySnapshot& policy, const std::vector<OrderedContactChunkSummary>& summaries,
    std::size_t trajectory_waypoint_count, std::string* error_reason,
    const ContactReleaseEvaluationStateMap* entering_state, bool enforce_trajectory_end_constraints,
    ContactReleaseEvaluationStateMap* exiting_state) {
  std::vector<ContactReleaseViolation> violations;
  if (trajectory_waypoint_count == 0 || policy.support_contacts.empty()) {
    return violations;
  }
  if (policy.support_contacts.size() > kMaxOrderedSupportContactIdentities) {
    if (error_reason) {
      *error_reason = "support_contact_identity_bound_exceeded";
    }
    return violations;
  }
  if (summaries.empty()) {
    if (error_reason) {
      *error_reason = "ordered_contact_summary_missing";
    }
    return violations;
  }
  if (entering_state != nullptr && entering_state->size() != policy.support_contacts.size()) {
    if (error_reason) {
      *error_reason = "contact_release_entering_state_identity_mismatch";
    }
    return violations;
  }
  if (exiting_state != nullptr) {
    exiting_state->clear();
  }
  std::size_t expected_begin = 0;
  for (const auto& summary : summaries) {
    if (!summary.valid || summary.range_begin != expected_begin ||
        summary.range_end < summary.range_begin ||
        !revisions_equal(summary.policy_revision, policy.revision)) {
      if (error_reason) {
        *error_reason =
            !summary.valid ? summary.invalid_reason : "ordered_contact_summary_sequence_mismatch";
      }
      return {};
    }
    expected_begin = summary.range_end;
  }
  if (expected_begin != trajectory_waypoint_count) {
    if (error_reason) {
      *error_reason = "ordered_contact_summary_coverage_mismatch";
    }
    return {};
  }

  for (std::size_t policy_index = 0; policy_index < policy.support_contacts.size();
       ++policy_index) {
    const auto& entry = policy.support_contacts[policy_index];
    const auto pair = canonicalize(entry.pair);
    ReleaseMergeState state;
    if (entering_state != nullptr) {
      const auto entering = entering_state->find(pair);
      if (entering == entering_state->end()) {
        if (error_reason) {
          *error_reason = "contact_release_entering_state_identity_mismatch";
        }
        return {};
      }
      state = entering->second;
      if (state.initialized && state.started_in_contact &&
          entry.mode == CollisionContactPolicyMode::kContactRelease &&
          entry.release_evidence_mode ==
              ContactReleaseEvidenceMode::kSignedPenetration &&
          !state.initial_depth_m.has_value()) {
        if (error_reason) {
          *error_reason = "contact_release_entering_state_invalid";
        }
        return {};
      }
    }
    std::optional<ContactReleaseViolation> violation;

    for (const auto& chunk : summaries) {
      const auto it = chunk.contact_release_summaries.find(pair);
      if (it == chunk.contact_release_summaries.end()) {
        if (error_reason) {
          *error_reason = "contact_release_summary_identity_mismatch";
        }
        return {};
      }
      const auto& pair_summary = it->second;
      if (!state.initialized) {
        const auto& first = pair_summary.full_segment.first_sample;
        if (!first || first->waypoint_index != 0) {
          if (error_reason) {
            *error_reason = "contact_release_initial_sample_missing";
          }
          return {};
        }
        ContactReleaseSampleEvidence evidence;
        evidence.in_contact = first->in_contact;
        if (first->clearance_valid) {
          evidence.signed_clearance_m = first->signed_clearance_m;
        }
        const auto transition =
            advance_contact_release_sample(entry, evidence, state);
        if (!transition.accepted()) {
          const bool inconclusive =
              transition.failure == ContactReleaseTransitionFailure::
                                        kInvalidPolicy ||
              transition.failure == ContactReleaseTransitionFailure::
                                        kMissingOrInvalidSignedPenetration ||
              transition.failure == ContactReleaseTransitionFailure::
                                        kContactProcessNotImplemented;
          violation = make_release_violation(
              policy_index, *first,
              inconclusive ? CollisionDecisionResult::kInconclusiveFail
                           : CollisionDecisionResult::kFail,
              contact_release_transition_failure_name(transition.failure),
              state);
          break;
        }
        violation = apply_release_segment(policy_index, entry,
                                          pair_summary.segment_after_first_sample, state);
      } else {
        violation = apply_release_segment(policy_index, entry, pair_summary.full_segment, state);
      }
      if (violation) {
        break;
      }
    }

    if (!violation && state.started_in_contact &&
        entry.mode != CollisionContactPolicyMode::kBoundedPenetration &&
        enforce_trajectory_end_constraints) {
      const auto& final_chunk = summaries.back();
      const auto final_it = final_chunk.contact_release_summaries.find(pair);
      if (final_it == final_chunk.contact_release_summaries.end() ||
          !final_it->second.full_segment.last_sample) {
        if (error_reason) {
          *error_reason = "contact_release_final_sample_missing";
        }
        return {};
      }
      const auto& final_sample = *final_it->second.full_segment.last_sample;
      if (final_sample.waypoint_index != static_cast<std::int64_t>(trajectory_waypoint_count - 1)) {
        if (error_reason) {
          *error_reason = "contact_release_final_sample_missing";
        }
        return {};
      }
      if (entry.require_separation_by_end && state.in_contact) {
        violation =
            make_release_violation(policy_index, final_sample, CollisionDecisionResult::kFail,
                                   "final_sample_not_separated", state);
      } else if (entry.release_evidence_mode ==
                     ContactReleaseEvidenceMode::kSignedPenetration &&
                 entry.require_strictly_decreasing_penetration &&
                 state.in_contact) {
        const double tolerance = std::max(0.0, entry.release_tolerance_m);
        const double required_progress =
            std::max(std::max(0.0, entry.min_release_progress_m), tolerance);
        if (!state.initial_depth_m) {
          if (error_reason) {
            *error_reason = "contact_release_state_missing_initial_depth";
          }
          return {};
        }
        const double progress = *state.initial_depth_m - state.best_depth_m;
        if (progress + tolerance < required_progress) {
          violation =
              make_release_violation(policy_index, final_sample, CollisionDecisionResult::kFail,
                                     "insufficient_release_progress", state);
        }
      }
    }
    if (violation) {
      violations.push_back(std::move(*violation));
    }
    if (exiting_state != nullptr) {
      exiting_state->emplace(pair, state);
    }
  }
  return violations;
}

bool apply_contact_release_violations(const CollisionPolicySnapshot& policy,
                                      const std::vector<ContactReleaseViolation>& violations,
                                      CollisionValidationDecisionBundle& bundle,
                                      std::string* error_reason) {
  CollisionDecisionResult worst = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_severity = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;
  for (const auto& record : bundle.waypoint_records) {
    const auto decision = record.result.decision;
    const auto severity = severity_for(decision);
    if (severity > worst_severity ||
        (severity == worst_severity && decision != CollisionDecisionResult::kPass &&
         worst == CollisionDecisionResult::kPass)) {
      worst = decision;
      worst_severity = severity;
    }
    if (decision != CollisionDecisionResult::kPass &&
        (!first_failing_waypoint || record.waypoint_index < *first_failing_waypoint)) {
      first_failing_waypoint = record.waypoint_index;
      first_failing_time_ns = record.time_from_start_ns;
    }
  }

  for (const auto& violation : violations) {
    if (violation.policy_entry_index >= policy.support_contacts.size()) {
      if (error_reason) {
        *error_reason = "contact_release_violation_policy_index_invalid";
      }
      return false;
    }
    const auto record_it =
        std::find_if(bundle.waypoint_records.begin(), bundle.waypoint_records.end(),
                     [&](const CollisionValidationWaypointRecord& record) {
                       return record.waypoint_index == violation.waypoint_index;
                     });
    if (record_it == bundle.waypoint_records.end()) {
      if (error_reason) {
        *error_reason = "contact_release_violation_record_missing";
      }
      return false;
    }
    const bool final_only_violation = violation.reason == "final_sample_not_separated" ||
                                      violation.reason == "insufficient_release_progress";
    // The serial authority stops evaluating end-of-trajectory obligations
    // once an earlier failure exists, while still recording unconditional
    // per-sample violations for later policy identities. Preserve that exact
    // diagnostic contract rather than producing extra parallel-only failures.
    if (final_only_violation && first_failing_waypoint) {
      continue;
    }
    record_it->result.decision = violation.decision;
    record_it->produced_decision_change = true;
    set_contact_release_diagnostic(
        *record_it, policy.support_contacts[violation.policy_entry_index], violation);

    if (violation.decision == CollisionDecisionResult::kInconclusiveFail &&
        worst != CollisionDecisionResult::kStopRequested) {
      worst = CollisionDecisionResult::kInconclusiveFail;
    } else if (worst == CollisionDecisionResult::kPass || worst == CollisionDecisionResult::kWarn) {
      worst = violation.decision;
    }
    worst_severity = CollisionDecisionSeverity::kBlock;
    if (!first_failing_waypoint || violation.waypoint_index < *first_failing_waypoint) {
      first_failing_waypoint = violation.waypoint_index;
      first_failing_time_ns = violation.time_from_start_ns;
    }
  }
  bundle.decision.accepted = worst == CollisionDecisionResult::kPass;
  bundle.decision.severity = worst_severity;
  bundle.decision.action = action_for(worst);
  bundle.decision.first_failing_waypoint = first_failing_waypoint;
  bundle.decision.first_failing_time_ns = first_failing_time_ns;
  return true;
}

}  // namespace manyforge::core
