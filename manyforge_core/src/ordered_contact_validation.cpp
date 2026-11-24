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
    const CollisionPairContactRule& policy, SupportContactEvaluationState entry_state) {
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
          if (policy.release() == nullptr || policy.release()->allow_recontact) {
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
                                           const CollisionPairContactRule& policy,
                                           const CollisionValidationPassConfig* pass_config) {
  ContactReleaseSampleSummary sample;
  sample.waypoint_index = static_cast<std::int64_t>(global_offset) + record.waypoint_index;
  if (pass_config != nullptr && sample.waypoint_index >= 0) {
    sample.path_steps =
        pass_config->path_steps_at(static_cast<std::size_t>(sample.waypoint_index));
  }
  sample.time_from_start_ns = record.time_from_start_ns;
  sample.in_contact = pair_present(record, policy.pair);
  sample.separated = !sample.in_contact;
  const bool metric_required = contact_rule_needs_signed_distance(policy);
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
    const CollisionPairContactRule& policy) {
  ContactReleaseSegmentSummary summary;
  if (begin >= samples.size()) {
    return summary;
  }
  summary.empty = false;
  summary.first_sample = samples[begin];
  const auto* release = policy.release();
  const bool bounded_penetration = release == nullptr;
  const bool signed_penetration =
      release != nullptr && release->release_evidence_mode ==
                                ContactReleaseEvidenceMode::kSignedPenetration;
  const bool metric_required = bounded_penetration || signed_penetration;
  const double tolerance =
      release != nullptr ? std::max(0.0, release->release_tolerance_m) : 0.0;
  const double max_start =
      release != nullptr ? std::max(0.0, release->max_start_penetration_m) : 0.0;
  const double max_contact =
      bounded_penetration ? std::max(0.0, *policy.contact()->max_penetration_m) : 0.0;
  bool saw_separation = false;
  std::optional<double> previous_depth;
  const auto* spring = bounded_penetration && policy.contact()->max_compression_step_m &&
                               !policy.contact()->spring_group
                           ? policy.contact()
                           : nullptr;

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
        sample.penetration_depth_m > max_contact &&
        !summary.first_active_contact_exceeding_contact_bound) {
      summary.first_active_contact_exceeding_contact_bound = sample;
    }
    // A spring: the depth against the segment's previous sample (0 when that
    // one was not in contact). The segment's first sample is checked against
    // the merge state.
    if (spring != nullptr && i > begin && !summary.first_compression_step_exceeded) {
      const auto& before = samples[i - 1U];
      const double previous = before.in_contact ? before.penetration_depth_m : 0.0;
      const double current = active ? sample.penetration_depth_m : 0.0;
      const auto limit = compression_step_limit_m(*spring, sample.path_steps);
      if (limit && current > previous + *limit + 1e-9) {
        summary.first_compression_step_exceeded = sample;
      }
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
    std::size_t policy_entry_index, const CollisionPairContactRule& rule,
    const ContactReleaseSegmentSummary& segment, ReleaseMergeState& state) {
  if (segment.empty) {
    return std::nullopt;
  }
  // A tracked rule is a release or a whole-move contact with a depth limit;
  // the release knobs below are read only on the release paths.
  const auto* release = rule.release();
  const CollisionReleaseRule policy = release != nullptr ? *release : CollisionReleaseRule{};
  const double tolerance =
      release != nullptr ? std::max(0.0, policy.release_tolerance_m) : 0.0;
  const bool bounded_penetration = release == nullptr;
  const bool signed_penetration =
      release != nullptr && policy.release_evidence_mode ==
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
            "contact_penetration_exceeded", state),
        1);
  }

  if (bounded_penetration && rule.contact()->max_compression_step_m &&
      !rule.contact()->spring_group) {
    const auto& spring = *rule.contact();
    // A colliding sample without a depth is reported as missing clearance.
    if (segment.first_sample &&
        (!segment.first_sample->in_contact || segment.first_sample->clearance_valid)) {
      const auto& first = *segment.first_sample;
      const double current = first.in_contact ? first.penetration_depth_m : 0.0;
      const auto limit = compression_step_limit_m(spring, first.path_steps);
      if (state.initialized && limit && current > state.previous_depth_m + *limit + 1e-9) {
        consider_violation(selected,
                           make_release_violation(policy_entry_index, first,
                                                  CollisionDecisionResult::kFail,
                                                  "contact_compression_step_exceeded", state),
                           2);
      }
    }
    if (segment.first_compression_step_exceeded) {
      consider_violation(selected,
                         make_release_violation(policy_entry_index,
                                                *segment.first_compression_step_exceeded,
                                                CollisionDecisionResult::kFail,
                                                "contact_compression_step_exceeded", state),
                         2);
    }
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
      state.previous_depth_m =
          segment.last_sample->in_contact ? segment.last_sample->penetration_depth_m : 0.0;
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
                                    const CollisionPairContactRule& policy,
                                    const ContactReleaseViolation& violation) {
  const auto* release = policy.release();
  record.result.raw_diagnostics["contact_release_violation"] = violation.reason;
  record.result.raw_diagnostics["contact_release_rule_id"] = policy.rule_id;
  record.result.raw_diagnostics["contact_release_pair"] = pair_text(policy.pair);
  record.result.raw_diagnostics["contact_release_mode"] =
      release == nullptr ? "contact" : "release";
  if (release == nullptr) {
    if (const auto* contact = policy.contact();
        contact != nullptr && contact->max_penetration_m) {
      record.result.raw_diagnostics["contact_release_max_penetration_m"] =
          std::to_string(*contact->max_penetration_m);
    }
  } else {
    const bool signed_penetration =
        release->release_evidence_mode ==
        ContactReleaseEvidenceMode::kSignedPenetration;
    record.result.raw_diagnostics["contact_release_evidence_mode"] =
        signed_penetration ? "signed_penetration" : "binary_contact";
    if (signed_penetration) {
      record.result.raw_diagnostics["contact_release_tolerance_m"] =
          std::to_string(release->release_tolerance_m);
      record.result.raw_diagnostics[
          "contact_release_enforce_max_start_penetration"] =
          release->enforce_max_start_penetration ? "true" : "false";
      record.result.raw_diagnostics[
          "contact_release_max_start_penetration_m"] =
          std::to_string(release->max_start_penetration_m);
      record.result.raw_diagnostics["contact_release_min_progress_m"] =
          std::to_string(release->min_release_progress_m);
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
      } else if (release == nullptr) {
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
    std::size_t range_end, const CollisionValidationPassConfig* pass_config) {
  OrderedContactChunkSummary summary;
  summary.range_begin = global_offset;
  summary.range_end = range_end;
  summary.policy_revision = policy.revision;

  if (tracked_contact_rule_count(policy) > kMaxOrderedSupportContactIdentities) {
    summary.valid = false;
    summary.invalid_reason = "contact_rule_pair_bound_exceeded";
    return summary;
  }
  std::set<CollisionPairId> identities;
  for (const auto& entry : policy.contact_rules) {
    if (!contact_rule_is_tracked(entry)) {
      continue;
    }
    const auto pair = canonicalize(entry.pair);
    if (!identities.insert(pair).second) {
      summary.valid = false;
      summary.invalid_reason = "duplicate_contact_rule_pair";
      return summary;
    }

    SupportContactPairChunkSummary support_summary;
    support_summary.pair = pair;
    const bool support_state_shadowed =
        pair_list_contains(policy.allowed_touch_link_pairs, pair) ||
        policy.static_pair(pair);
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
      samples.push_back(release_sample(record, global_offset, entry, pass_config));
    }
    ContactReleasePairChunkSummary release_summary;
    release_summary.pair = pair;
    release_summary.full_segment = summarize_release_segment(samples, 0, entry);
    release_summary.segment_after_first_sample = summarize_release_segment(samples, 1, entry);
    summary.contact_release_summaries.emplace(pair, std::move(release_summary));
  }
  return summary;
}

std::vector<SpringGroupStepViolation> spring_group_step_violations(
    const CollisionPolicySnapshot& policy,
    const std::vector<CollisionValidationWaypointRecord>& records,
    const CollisionValidationPassConfig& pass_config, const ContactRuleKind kind) {
  struct Group {
    const CollisionPairContactRule* rule = nullptr;
    double previous_depth_m = 0.0;
    double depth_m = 0.0;
    std::size_t deepest = 0;
    bool reported = false;
  };
  std::vector<Group> groups;
  for (const auto& entry : policy.contact_rules) {
    const auto* contact = entry.contact();
    if (contact != nullptr && contact->spring_group && contact_rule_kind(entry) == kind &&
        std::none_of(groups.begin(), groups.end(), [&](const Group& group) {
          return group.rule->rule_id == entry.rule_id;
        })) {
      groups.push_back(Group{&entry, 0.0, 0.0, 0U, false});
    }
  }
  std::vector<SpringGroupStepViolation> violations;
  if (groups.empty()) {
    return violations;
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    for (auto& group : groups) {
      group.depth_m = 0.0;
    }
    for (const auto& observed : record.result.normalized_pairs) {
      const auto* entry = policy.contact_rule_for(canonicalize(observed));
      if (entry == nullptr || entry->contact() == nullptr || !entry->contact()->spring_group) {
        continue;
      }
      const auto group = std::find_if(groups.begin(), groups.end(), [&](const Group& candidate) {
        return candidate.rule->rule_id == entry->rule_id;
      });
      const auto clearance = clearance_for_pair(record.result, entry->pair);
      if (group == groups.end() || !clearance || !std::isfinite(*clearance) || *clearance > 0.0 ||
          !(-*clearance > group->depth_m)) {
        continue;
      }
      group->depth_m = -*clearance;
      group->deepest = static_cast<std::size_t>(entry - policy.contact_rules.data());
    }
    const auto path_steps =
        record.waypoint_index >= 0
            ? pass_config.path_steps_at(static_cast<std::size_t>(record.waypoint_index))
            : std::nullopt;
    for (auto& group : groups) {
      const auto limit = compression_step_limit_m(*group.rule->contact(), path_steps);
      if (index > 0U && !group.reported && limit &&
          group.depth_m > group.previous_depth_m + *limit + 1e-9) {
        violations.push_back(SpringGroupStepViolation{index, group.deepest, group.previous_depth_m,
                                                      group.depth_m, *limit});
        group.reported = true;
      }
      group.previous_depth_m = group.depth_m;
    }
  }
  return violations;
}

std::vector<ContactReleaseViolation> spring_group_release_violations(
    const CollisionPolicySnapshot& policy,
    const std::vector<CollisionValidationWaypointRecord>& records,
    const CollisionValidationPassConfig& pass_config) {
  std::vector<ContactReleaseViolation> violations;
  for (const auto& found : spring_group_step_violations(policy, records, pass_config,
                                                        ContactRuleKind::kBoundedContact)) {
    const auto& record = records[found.record_index];
    ContactReleaseViolation violation;
    violation.policy_entry_index = found.policy_entry_index;
    violation.waypoint_index = record.waypoint_index;
    violation.time_from_start_ns = record.time_from_start_ns;
    violation.decision = CollisionDecisionResult::kFail;
    violation.reason = "contact_compression_step_exceeded";
    violation.current_depth_m = found.depth_m;
    violations.push_back(std::move(violation));
  }
  return violations;
}

SupportContactEvaluationStateMap initial_support_contact_state(
    const CollisionPolicySnapshot& policy) {
  SupportContactEvaluationStateMap state;
  for (const auto& entry : policy.contact_rules) {
    if (contact_rule_is_tracked(entry)) {
      state[canonicalize(entry.pair)] = SupportContactEvaluationState::kNotInContact;
    }
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
      *error_reason = "contact_rule_summary_identity_mismatch";
    }
    return false;
  }
  for (auto& [pair, current] : state) {
    const auto it = summary.support_contact_transitions.find(pair);
    if (it == summary.support_contact_transitions.end()) {
      if (error_reason) {
        *error_reason = "contact_rule_summary_identity_mismatch";
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
  const std::size_t tracked_count = tracked_contact_rule_count(policy);
  if (trajectory_waypoint_count == 0 || tracked_count == 0U) {
    return violations;
  }
  if (tracked_count > kMaxOrderedSupportContactIdentities) {
    if (error_reason) {
      *error_reason = "contact_rule_pair_bound_exceeded";
    }
    return violations;
  }
  if (summaries.empty()) {
    if (error_reason) {
      *error_reason = "ordered_contact_summary_missing";
    }
    return violations;
  }
  if (entering_state != nullptr && entering_state->size() != tracked_count) {
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

  for (std::size_t policy_index = 0; policy_index < policy.contact_rules.size();
       ++policy_index) {
    const auto& entry = policy.contact_rules[policy_index];
    if (!contact_rule_is_tracked(entry)) {
      continue;
    }
    const auto* release = entry.release();
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
      if (state.initialized && state.started_in_contact && release != nullptr &&
          release->release_evidence_mode ==
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
        evidence.path_steps = first->path_steps;
        const auto transition =
            advance_contact_release_sample(entry, evidence, state);
        if (!transition.accepted()) {
          const bool inconclusive =
              transition.failure == ContactReleaseTransitionFailure::
                                        kInvalidPolicy ||
              transition.failure == ContactReleaseTransitionFailure::
                                        kMissingOrInvalidSignedPenetration;
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

    if (!violation && state.started_in_contact && release != nullptr &&
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
      if (release->require_separation_by_end && state.in_contact) {
        violation =
            make_release_violation(policy_index, final_sample, CollisionDecisionResult::kFail,
                                   "final_sample_not_separated", state);
      } else if (release->release_evidence_mode ==
                     ContactReleaseEvidenceMode::kSignedPenetration &&
                 release->require_strictly_decreasing_penetration &&
                 state.in_contact) {
        const double tolerance = std::max(0.0, release->release_tolerance_m);
        const double required_progress =
            std::max(std::max(0.0, release->min_release_progress_m), tolerance);
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
    if (violation.policy_entry_index >= policy.contact_rules.size() ||
        !contact_rule_is_tracked(policy.contact_rules[violation.policy_entry_index])) {
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
        *record_it, policy.contact_rules[violation.policy_entry_index], violation);

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
