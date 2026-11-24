#include <algorithm>
#include <chrono>
#include <cmath>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/ordered_contact_validation.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace manyforge::core {

namespace {

TimePoint default_steady_now() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
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
  return CollisionDecisionAction::kReject;
}

void mark_cancelled(CollisionValidationDecisionBundle& bundle,
                    std::int64_t waypoint_index,
                    std::optional<std::int64_t> time_from_start_ns,
                    std::string reason) {
  bundle.cancelled = true;
  bundle.cancellation_reason = std::move(reason);
  bundle.decision.accepted = false;
  bundle.cancelled_at_waypoint_index = waypoint_index;
  const bool earlier_reject =
      bundle.decision.action != CollisionDecisionAction::kDispatch &&
      bundle.decision.first_failing_waypoint &&
      *bundle.decision.first_failing_waypoint < waypoint_index;
  if (!earlier_reject) {
    bundle.decision.severity = CollisionDecisionSeverity::kBlock;
    bundle.decision.action = CollisionDecisionAction::kInconclusive;
    bundle.decision.first_failing_waypoint = waypoint_index;
    bundle.decision.first_failing_time_ns = time_from_start_ns;
  }
}

std::optional<std::int64_t> waypoint_time_from_start_ns(
    const TrajectoryDTO& trajectory,
    std::size_t index) {
  if (index >= trajectory.waypoints.size()) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(
      trajectory.waypoints[index].time_from_start_s * 1e9);
}

CollisionPairId canonicalize(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
}

struct CollisionDistanceSelection {
  CollisionDistanceRequestMode mode = CollisionDistanceRequestMode::kNone;
  std::vector<CollisionPairId> selected_pairs;
};

CollisionDistanceSelection distance_selection_for(
    const CollisionPolicySnapshot& snapshot) {
  CollisionDistanceSelection selection;
  selection.selected_pairs = build_selected_collision_distance_pairs(snapshot);
  if (!selection.selected_pairs.empty()) {
    selection.mode = CollisionDistanceRequestMode::kSelectedCollidingPairs;
  }
  return selection;
}

std::vector<std::string> required_capabilities_for(
    const CollisionPolicySnapshot& snapshot,
    CollisionDistanceRequestMode distance_request_mode) {
  std::vector<std::string> capabilities;
  const bool has_contact_rules = !snapshot.contact_rules.empty();
  if (!snapshot.require_full_diagnostics && !has_contact_rules) {
    return capabilities;
  }
  // An allowed static pair needs two static bodies.
  if (snapshot.static_body_ids.size() > 1U ||
      !snapshot.allowed_touch_link_pairs.empty() || has_contact_rules) {
    capabilities.push_back("body_pair_only");
  }
  if (distance_request_mode != CollisionDistanceRequestMode::kNone) {
    capabilities.push_back("penetration_depth");
  }
  return capabilities;
}

std::optional<std::string_view> missing_query_capability(
    const CollisionEngineCapabilityDescriptor& capability,
    const std::vector<std::string>& required_capabilities) {
  const auto requires_capability = [&](const char* name) {
    return std::find(required_capabilities.begin(),
                     required_capabilities.end(), name) !=
           required_capabilities.end();
  };
  if (requires_capability("distance") &&
      !capability.supports_distance_query) {
    return "distance";
  }
  if (requires_capability("penetration_depth") &&
      !capability.supports_penetration_depth) {
    return "penetration_depth";
  }
  if (requires_capability("contact_points") &&
      !capability.supports_contact_points) {
    return "contact_points";
  }
  return std::nullopt;
}

bool engine_admits_query_capabilities(
    const CollisionEngineCapabilityDescriptor& capability,
    CollisionDistanceRequestMode distance_request_mode,
    const std::vector<std::string>& required_capabilities) {
  if (!collision_engine_supports_distance_request_mode(
          capability, distance_request_mode)) {
    return false;
  }
  return !missing_query_capability(capability, required_capabilities);
}

CollisionResult capability_admission_failure(
    const CollisionEngineCapabilityDescriptor& capability,
    const CollisionQuery& query,
    const CollisionWorldRevision& borrowed_revision) {
  CollisionResult result;
  result.query_id = query.query_id;
  result.surface = query.surface;
  result.state_time_ns = query.state_time_ns;
  result.collision_world_revision = borrowed_revision;
  result.engine_id = capability.engine_id;
  result.engine_version = capability.engine_version;
  result.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  result.decision = CollisionDecisionResult::kInconclusiveFail;
  result.distance_request_mode = query.distance_request_mode;
  const bool request_mode_supported =
      collision_engine_supports_distance_request_mode(
          capability, query.distance_request_mode);
  const auto missing_capability =
      missing_query_capability(capability, query.required_capabilities);
  result.raw_diagnostics["reason"] =
      request_mode_supported && missing_capability
          ? "collision_required_capability_unsupported"
          : "collision_distance_request_mode_unsupported";
  if (request_mode_supported && missing_capability) {
    result.raw_diagnostics["required_capability"] =
        std::string(*missing_capability);
  }
  result.raw_diagnostics["requested_distance_request_mode"] =
      collision_distance_request_mode_name(query.distance_request_mode);
  result.raw_diagnostics["distance_request_modes_valid"] =
      collision_distance_request_modes_valid(capability) ? "true" : "false";
  return result;
}

std::shared_ptr<const CollisionPolicySnapshot> resolve_policy_snapshot(
    CollisionValidationPassConfig& config) {
  if (config.policy_snapshot_ref) {
    return config.policy_snapshot_ref;
  }
  auto snapshot =
      std::make_shared<CollisionPolicySnapshot>(std::move(config.policy_snapshot));
  config.policy_snapshot_ref = snapshot;
  return snapshot;
}

std::shared_ptr<const CollisionPolicyIndex> resolve_policy_index(
    CollisionValidationPassConfig& config,
    const std::shared_ptr<const CollisionPolicySnapshot>& snapshot) {
  if (config.policy_index) {
    return config.policy_index;
  }
  auto index = build_collision_policy_index(*snapshot);
  config.policy_index = index;
  return index;
}

std::optional<double> clearance_for_pair(const CollisionResult& result,
                                         const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  for (const auto& clearance : result.pair_clearances) {
    if (canonicalize(clearance.pair) == canonical) {
      return clearance.signed_clearance_m;
    }
  }
  if (result.min_clearance_pair &&
      canonicalize(*result.min_clearance_pair) == canonical &&
      result.min_clearance_m) {
    return *result.min_clearance_m;
  }
  return std::nullopt;
}

bool pair_present(const CollisionResult& result, const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  return std::any_of(result.normalized_pairs.begin(),
                     result.normalized_pairs.end(),
                     [&](const CollisionPairId& observed) {
                       return canonicalize(observed) == canonical;
                     });
}

std::string pair_text(const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  return canonical.body_a + " <-> " + canonical.body_b;
}

void set_contact_release_diagnostic(CollisionValidationWaypointRecord& record,
                                    const CollisionPairContactRule& entry,
                                    CollisionDecisionResult decision,
                                    std::string reason,
                                    std::optional<double> initial_depth,
                                    std::optional<double> current_depth) {
  const auto* release = entry.release();
  record.result.raw_diagnostics["contact_release_violation"] =
      std::move(reason);
  record.result.raw_diagnostics["contact_release_rule_id"] = entry.rule_id;
  record.result.raw_diagnostics["contact_release_pair"] = pair_text(entry.pair);
  record.result.raw_diagnostics["contact_release_mode"] =
      release == nullptr ? "contact" : "release";
  if (release == nullptr) {
    if (const auto* contact = entry.contact();
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
  if (initial_depth) {
    record.result.raw_diagnostics["contact_release_initial_penetration_m"] =
        std::to_string(*initial_depth);
  }
  if (current_depth) {
    record.result.raw_diagnostics["contact_release_current_penetration_m"] =
        std::to_string(*current_depth);
  }
  for (auto& classified : record.result.normalized_pairs_classified) {
    if (canonicalize(classified.pair) == canonicalize(entry.pair)) {
      if (decision == CollisionDecisionResult::kInconclusiveFail) {
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

/// Diagnostic of one refused sample of a pair under a contact rule with
/// an end window.
void set_windowed_contact_diagnostic(CollisionValidationWaypointRecord& record,
                                     const CollisionPairContactRule& rule,
                                     const CollisionPairId& pair,
                                     std::string reason,
                                     std::optional<double> clearance,
                                     double sample_fraction,
                                     bool inside_window) {
  const auto& contact = *rule.contact();
  record.result.raw_diagnostics["contact_rule_violation"] = reason;
  record.result.raw_diagnostics["contact_rule_id"] = rule.rule_id;
  record.result.raw_diagnostics["contact_rule_pair"] = pair_text(pair);
  if (contact.max_penetration_m) {
    record.result.raw_diagnostics["contact_rule_max_penetration_m"] =
        std::to_string(*contact.max_penetration_m);
  }
  record.result.raw_diagnostics["contact_rule_end_from_fraction"] =
      std::to_string(contact.end_from_fraction);
  record.result.raw_diagnostics["contact_rule_sample_fraction"] =
      std::to_string(sample_fraction);
  record.result.raw_diagnostics["contact_rule_inside_window"] =
      inside_window ? "true" : "false";
  record.result.raw_diagnostics["contact_rule_allowed_contact"] = "false";
  if (clearance) {
    record.result.raw_diagnostics["contact_rule_signed_clearance_m"] =
        std::to_string(*clearance);
  }
  for (auto& classified : record.result.normalized_pairs_classified) {
    if (canonicalize(classified.pair) == canonicalize(pair)) {
      if (reason == reason_codes::kMissingSignedClearance) {
        classified.classification =
            CollisionPairClassification::kDiagnosticIncomplete;
      }
      classified.is_first_failure = true;
      break;
    }
  }
}

bool classification_blocks(CollisionPairClassification classification) {
  switch (classification) {
    case CollisionPairClassification::kBlockingCollision:
    case CollisionPairClassification::kSupportContactRecontactViolation:
    case CollisionPairClassification::kDiagnosticIncomplete:
      return true;
    case CollisionPairClassification::kAllowedStaticStatic:
    case CollisionPairClassification::kSupportContactStart:
    case CollisionPairClassification::kSupportContactContinued:
    case CollisionPairClassification::kSupportContactSeparated:
    case CollisionPairClassification::kAllowedTouchLink:
    case CollisionPairClassification::kAllowedContactRule:
      return false;
  }
  return true;
}

CollisionDecisionResult decision_from_classifications(
    const CollisionResult& result) {
  if (result.engine_status != CollisionEngineStatus::kOk) {
    return result.decision;
  }
  for (const auto& classified : result.normalized_pairs_classified) {
    if (classification_blocks(classified.classification)) {
      return CollisionDecisionResult::kFail;
    }
  }
  return CollisionDecisionResult::kPass;
}

}  // namespace

CollisionValidationCoordinator::CollisionValidationCoordinator(
    CollisionValidationPassConfig config, ClockNow clock_now)
    : config_(std::move(config)),
      policy_snapshot_(resolve_policy_snapshot(config_)),
      policy_index_(resolve_policy_index(config_, policy_snapshot_)),
      evaluator_(policy_snapshot_, policy_index_),
      clock_now_(clock_now ? std::move(clock_now) : &default_steady_now),
      policy_revision_string_(
          std::to_string(policy_snapshot_->revision.revision_id)) {}

void CollisionValidationCoordinator::prepare_distance_selection() {
  auto selection = distance_selection_for(*policy_snapshot_);
  distance_request_mode_ = selection.mode;
  selected_distance_pairs_ = std::move(selection.selected_pairs);
  required_capabilities_ =
      required_capabilities_for(*policy_snapshot_, distance_request_mode_);
}

CollisionDecisionResult
CollisionValidationCoordinator::merge_engine_and_policy_decision(
    CollisionDecisionResult engine_decision,
    CollisionDecisionResult policy_decision) {
  // Fail-closed engine status cannot be downgraded by an allow / dispatch
  // policy result. Mirrors merge_policy_collision_decision in the RoboPlan
  // observed-state provider so the two surfaces stay coherent.
  if (engine_decision == CollisionDecisionResult::kInconclusiveFail ||
      engine_decision == CollisionDecisionResult::kStopRequested) {
    return engine_decision;
  }
  return policy_decision;
}

CollisionValidationWaypointRecord
CollisionValidationCoordinator::classify_result(
    CollisionResult result, std::int64_t waypoint_index,
    std::optional<std::int64_t> time_from_start_ns,
    const CollisionWorldRevision& borrowed_revision) {
  result.distance_request_mode = distance_request_mode_;
  result.evaluated_distance_pair_count = result.pair_clearances.size();
  CollisionValidationWaypointRecord record;
  record.waypoint_index = waypoint_index;
  record.time_from_start_ns = time_from_start_ns;
  record.result = std::move(result);

  const bool diagnostics_complete =
      record.result.engine_status == CollisionEngineStatus::kOk;
  auto evaluated = evaluator_.classify(record.result.normalized_pairs,
                                       borrowed_revision, diagnostics_complete);

  record.result.normalized_pairs_classified = std::move(evaluated.pairs);
  record.result.policy_revision_used = policy_revision_string_;

  const auto engine_decision = record.result.decision;
  CollisionDecisionResult policy_decision = CollisionDecisionResult::kPass;
  switch (evaluated.decision.action) {
    case CollisionDecisionAction::kReject:
      policy_decision = CollisionDecisionResult::kFail;
      break;
    case CollisionDecisionAction::kInconclusive:
      policy_decision = CollisionDecisionResult::kInconclusiveFail;
      break;
    case CollisionDecisionAction::kWarnOnly:
      policy_decision = CollisionDecisionResult::kWarn;
      break;
    case CollisionDecisionAction::kDispatch:
      policy_decision = CollisionDecisionResult::kPass;
      break;
  }

  const auto merged =
      merge_engine_and_policy_decision(engine_decision, policy_decision);
  record.produced_decision_change = (merged != CollisionDecisionResult::kPass);
  record.result.decision = merged;
  return record;
}

void CollisionValidationCoordinator::apply_contact_release_constraints(
    const TrajectoryDTO& trajectory,
    CollisionValidationDecisionBundle& bundle,
    CollisionDecisionResult& worst_decision,
    CollisionDecisionSeverity& worst_severity,
    std::optional<std::int64_t>& first_failing_waypoint,
    std::optional<std::int64_t>& first_failing_time_ns) const {
  auto record_violation = [&](CollisionValidationWaypointRecord& record,
                              const CollisionPairContactRule& entry,
                              CollisionDecisionResult decision,
                              std::string reason,
                              std::optional<double> initial_depth,
                              std::optional<double> current_depth) {
    if (decision == CollisionDecisionResult::kInconclusiveFail &&
        worst_decision != CollisionDecisionResult::kStopRequested) {
      worst_decision = CollisionDecisionResult::kInconclusiveFail;
    } else if (worst_decision == CollisionDecisionResult::kPass ||
               worst_decision == CollisionDecisionResult::kWarn) {
      worst_decision = decision;
    }
    worst_severity = CollisionDecisionSeverity::kBlock;
    record.result.decision = decision;
    record.produced_decision_change = true;
    set_contact_release_diagnostic(record,
                                   entry,
                                   decision,
                                   std::move(reason),
                                   initial_depth,
                                   current_depth);
    if (!first_failing_waypoint.has_value() || record.waypoint_index < *first_failing_waypoint) {
      first_failing_waypoint = record.waypoint_index;
      first_failing_time_ns = record.time_from_start_ns;
    }
  };

  if (bundle.waypoint_records.empty()) {
    return;
  }

  for (const auto& support_entry : policy_snapshot_->contact_rules) {
    if (!contact_rule_is_tracked(support_entry)) {
      continue;
    }
    const auto* release = support_entry.release();
    const double tolerance =
        release != nullptr ? std::max(0.0, release->release_tolerance_m) : 0.0;
    const double min_progress =
        release != nullptr ? std::max(0.0, release->min_release_progress_m) : 0.0;
    ContactReleaseEvaluationState state;
    std::optional<double> current_depth;
    bool sample_violation = false;

    for (auto& record : bundle.waypoint_records) {
      ContactReleaseSampleEvidence evidence;
      evidence.in_contact = pair_present(record.result, support_entry.pair);
      if (evidence.in_contact &&
          contact_rule_needs_signed_distance(support_entry)) {
        evidence.signed_clearance_m =
            clearance_for_pair(record.result, support_entry.pair);
      }
      if (record.waypoint_index >= 0) {
        evidence.path_steps =
            config_.path_steps_at(static_cast<std::size_t>(record.waypoint_index));
      }
      const auto transition =
          advance_contact_release_sample(support_entry, evidence, state);
      if (transition.penetration_depth_m) {
        current_depth = transition.penetration_depth_m;
      }
      if (transition.accepted()) {
        continue;
      }
      const bool inconclusive =
          transition.failure == ContactReleaseTransitionFailure::
                                    kInvalidPolicy ||
          transition.failure == ContactReleaseTransitionFailure::
                                    kMissingOrInvalidSignedPenetration;
      record_violation(
          record, support_entry,
          inconclusive ? CollisionDecisionResult::kInconclusiveFail
                       : CollisionDecisionResult::kFail,
          contact_release_transition_failure_name(transition.failure),
          state.initial_depth_m, transition.penetration_depth_m);
      sample_violation = true;
      break;
    }

    if (sample_violation || release == nullptr ||
        !state.started_in_contact ||
        first_failing_waypoint.has_value() ||
        !config_.enforce_contact_release_trajectory_end) {
      continue;
    }

    auto& final_record = bundle.waypoint_records.back();
    if (release->require_separation_by_end && state.in_contact) {
      record_violation(final_record,
                       support_entry,
                       CollisionDecisionResult::kFail,
                       "final_sample_not_separated",
                       state.initial_depth_m,
                       current_depth);
      continue;
    }
    if (release->release_evidence_mode ==
            ContactReleaseEvidenceMode::kSignedPenetration &&
        release->require_strictly_decreasing_penetration &&
        state.in_contact) {
      const double required_progress = std::max(min_progress, tolerance);
      if (!state.initial_depth_m) {
        record_violation(final_record,
                         support_entry,
                         CollisionDecisionResult::kInconclusiveFail,
                         "missing_contact_release_clearance",
                         std::nullopt,
                         current_depth);
        continue;
      }
      const double progress = *state.initial_depth_m - state.best_depth_m;
      if (progress + tolerance < required_progress) {
        record_violation(final_record,
                         support_entry,
                         CollisionDecisionResult::kFail,
                         "insufficient_release_progress",
                         state.initial_depth_m,
                         current_depth);
      }
    }
  }

  // A grouped spring's step limit compares the whole rule across samples
  // (OD-371).
  for (const auto& found :
       spring_group_step_violations(*policy_snapshot_, bundle.waypoint_records, config_,
                                    ContactRuleKind::kBoundedContact)) {
    record_violation(bundle.waypoint_records[found.record_index],
                     policy_snapshot_->contact_rules[found.policy_entry_index],
                     CollisionDecisionResult::kFail,
                     reason_codes::kContactCompressionStepExceeded, std::nullopt,
                     found.depth_m);
  }

  (void)trajectory;
}

void CollisionValidationCoordinator::apply_windowed_contact_constraints(
    const TrajectoryDTO& trajectory,
    CollisionValidationDecisionBundle& bundle) const {
  if (bundle.waypoint_records.empty()) {
    return;
  }
  const CollisionPairContactRule* first_windowed = nullptr;
  const CollisionPairContactRule* first_invalid = nullptr;
  for (const auto& [pair, rule] : policy_index_->contact_rule_index) {
    static_cast<void>(pair);
    if (contact_rule_kind(rule) != ContactRuleKind::kWindowedContact) {
      continue;
    }
    if (first_windowed == nullptr) {
      first_windowed = &rule;
    }
    if (first_invalid == nullptr && !contact_rule_valid(rule)) {
      first_invalid = &rule;
    }
  }
  if (first_windowed == nullptr) {
    return;
  }
  if (first_invalid != nullptr) {
    auto& first = bundle.waypoint_records.front();
    first.result.decision = CollisionDecisionResult::kInconclusiveFail;
    first.produced_decision_change = true;
    first.result.raw_diagnostics["contact_rule_violation"] =
        "invalid_collision_rule";
    first.result.raw_diagnostics["contact_rule_id"] = first_invalid->rule_id;
    return;
  }

  const auto* original_fractions = config_.original_trajectory_sample_fractions
                                       ? &*config_.original_trajectory_sample_fractions
                                       : nullptr;
  if (original_fractions != nullptr) {
    bool valid = original_fractions->size() == bundle.waypoint_records.size();
    double previous = -1.0;
    for (const auto fraction : *original_fractions) {
      if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0 || fraction <= previous) {
        valid = false;
        break;
      }
      previous = fraction;
    }
    if (!valid) {
      auto& first = bundle.waypoint_records.front();
      first.result.decision = CollisionDecisionResult::kInconclusiveFail;
      first.produced_decision_change = true;
      first.result.raw_diagnostics["contact_rule_violation"] =
          "invalid_contact_rule_sample_fractions";
      first.result.raw_diagnostics["contact_rule_id"] = first_windowed->rule_id;
      return;
    }
  }

  const auto last_index =
      bundle.waypoint_records.size() > 1 ? bundle.waypoint_records.size() - 1 : 1;
  // A spring's depth at the previous sample; a pair absent from it was not in
  // contact there (depth 0). A grouped spring is checked as one (OD-371).
  std::map<CollisionPairId, double> spring_depths;
  std::map<CollisionPairId, double> spring_depths_now;
  const auto group_steps = spring_group_step_violations(
      *policy_snapshot_, bundle.waypoint_records, config_, ContactRuleKind::kWindowedContact);
  for (std::size_t i = 0; i < bundle.waypoint_records.size(); ++i) {
    auto& record = bundle.waypoint_records[i];
    if (record.result.engine_status != CollisionEngineStatus::kOk &&
        record.result.engine_status !=
            CollisionEngineStatus::kDiagnosticIncomplete) {
      continue;
    }
    spring_depths_now.clear();
    const double fraction = original_fractions != nullptr ? (*original_fractions)[i]
                            : bundle.waypoint_records.size() <= 1
                                ? 1.0
                                : static_cast<double>(i) / static_cast<double>(last_index);
    const bool is_plan_initial_sample = original_fractions != nullptr ? fraction <= 1e-12 : i == 0;
    bool changed = false;
    for (auto& classified : record.result.normalized_pairs_classified) {
      if (classified.classification !=
              CollisionPairClassification::kBlockingCollision &&
          classified.classification !=
              CollisionPairClassification::kDiagnosticIncomplete) {
        continue;
      }
      const auto found =
          policy_index_->contact_rule_index.find(canonicalize(classified.pair));
      if (found == policy_index_->contact_rule_index.end() ||
          contact_rule_kind(found->second) != ContactRuleKind::kWindowedContact) {
        continue;
      }
      const auto& rule = found->second;
      const auto& contact = *rule.contact();
      const bool inside_window =
          fraction + 1e-12 >= contact.end_from_fraction;
      // The depth is measured only where the rule has a limit.
      std::optional<double> clearance;
      if (contact.max_penetration_m) {
        clearance = clearance_for_pair(record.result, classified.pair);
        if (!clearance || !std::isfinite(*clearance) || *clearance > 0.0) {
          record.result.engine_status =
              CollisionEngineStatus::kDiagnosticIncomplete;
          record.result.decision =
              CollisionDecisionResult::kInconclusiveFail;
          record.produced_decision_change = true;
          set_windowed_contact_diagnostic(record, rule, classified.pair,
                                          reason_codes::kMissingSignedClearance,
                                          std::nullopt, fraction, inside_window);
          continue;
        }
      }
      // A finite metric cannot make an engine-inconclusive sample safe. Keep
      // the engine failure authoritative and do not relabel it as a contact
      // rule violation when the selected metric itself was complete.
      if (record.result.engine_status != CollisionEngineStatus::kOk) {
        continue;
      }
      if (is_plan_initial_sample) {
        set_windowed_contact_diagnostic(record, rule, classified.pair,
                                        reason_codes::kInitialSampleInContact,
                                        clearance, fraction, inside_window);
        continue;
      }
      if (!inside_window) {
        set_windowed_contact_diagnostic(record, rule, classified.pair,
                                        reason_codes::kContactBeforeTerminalWindow,
                                        clearance, fraction, inside_window);
        continue;
      }
      if (contact.max_penetration_m &&
          std::max(0.0, -*clearance) > *contact.max_penetration_m + 1e-9) {
        set_windowed_contact_diagnostic(record, rule, classified.pair,
                                        "contact_penetration_exceeded",
                                        clearance, fraction, inside_window);
        continue;
      }
      if (contact.max_compression_step_m) {
        const auto canonical = canonicalize(classified.pair);
        const double depth = std::max(0.0, -*clearance);
        std::optional<double> previous_depth;
        std::optional<double> step_limit;
        if (contact.spring_group) {
          const auto group_step = std::find_if(
              group_steps.begin(), group_steps.end(), [&](const SpringGroupStepViolation& found) {
                return found.record_index == i &&
                       policy_snapshot_->contact_rules[found.policy_entry_index].pair == canonical;
              });
          if (group_step != group_steps.end()) {
            previous_depth = group_step->previous_depth_m;
            step_limit = group_step->step_limit_m;
          }
        } else {
          const auto previous = spring_depths.find(canonical);
          const double previous_pair_depth =
              previous != spring_depths.end() ? previous->second : 0.0;
          const auto limit = pair_compression_step_limit_m(contact, config_.path_steps_at(i));
          if (limit && depth > previous_pair_depth + *limit + 1e-9) {
            previous_depth = previous_pair_depth;
            step_limit = limit;
          } else {
            spring_depths_now[canonical] = depth;
          }
        }
        if (step_limit) {
          set_windowed_contact_diagnostic(record, rule, classified.pair,
                                          reason_codes::kContactCompressionStepExceeded,
                                          clearance, fraction, inside_window);
          record.result.raw_diagnostics["contact_rule_previous_penetration_m"] =
              std::to_string(*previous_depth);
          record.result.raw_diagnostics["contact_rule_max_compression_step_m"] =
              std::to_string(*step_limit);
          continue;
        }
      }
      classified.classification =
          CollisionPairClassification::kAllowedContactRule;
      classified.is_first_failure = false;
      record.result.raw_diagnostics["contact_rule_id"] = rule.rule_id;
      record.result.raw_diagnostics["contact_rule_allowed_pair"] =
          pair_text(classified.pair);
      changed = true;
    }
    if (changed) {
      record.result.decision = decision_from_classifications(record.result);
      record.produced_decision_change =
          record.result.decision != CollisionDecisionResult::kPass;
    }
    spring_depths.swap(spring_depths_now);
  }
  (void)trajectory;
}

CollisionQuery CollisionValidationCoordinator::make_query(
    const RobotStateDTO& state, std::int64_t waypoint_index,
    std::optional<std::int64_t> time_from_start_ns,
    const CollisionWorldRevision& borrowed_revision) const {
  CollisionQuery query;
  query.robot_state = state;
  query.surface = config_.surface;
  query.collision_world_revision = borrowed_revision;
  query.policy_revision = policy_revision_string_;
  query.state_time_ns = time_from_start_ns;
  query.waypoint_index = waypoint_index;
  query.query_id = "validation_coordinator_w" + std::to_string(waypoint_index);
  query.required_capabilities = required_capabilities_;
  query.distance_request_mode = distance_request_mode_;
  query.selected_distance_pairs = selected_distance_pairs_;
  query.clearance_margins = config_.clearance_margins;
  if (config_.per_state_timeout_ns > 0) {
    query.timeout_budget_ns = config_.per_state_timeout_ns;
  }
  return query;
}

CollisionValidationWaypointRecord
CollisionValidationCoordinator::classify_one(
    const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
    const RobotStateDTO& state, std::int64_t waypoint_index,
    std::optional<std::int64_t> time_from_start_ns,
    const CollisionWorldRevision& borrowed_revision,
    const CollisionCancellationToken& cancel) {
  auto query =
      make_query(state, waypoint_index, time_from_start_ns, borrowed_revision);
  if (!engine_admits_query_capabilities(engine.capability(),
                                        query.distance_request_mode,
                                        query.required_capabilities)) {
    return classify_result(
        capability_admission_failure(engine.capability(), query,
                                     borrowed_revision),
        waypoint_index, time_from_start_ns, borrowed_revision);
  }
  return classify_result(engine.check(snapshot, query, *policy_snapshot_,
                                      cancel),
                         waypoint_index, time_from_start_ns,
                         borrowed_revision);
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::run_pass_once(
    const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
    const TrajectoryDTO& trajectory,
    std::size_t begin_index,
    std::size_t end_index,
    const CollisionCancellationToken& cancel) {
  prepare_distance_selection();
  CollisionValidationDecisionBundle bundle;
  bundle.borrowed_world_revision = snapshot.revision;

  evaluator_.reset();

  CollisionDecisionResult worst_decision = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_severity = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;
  RobotStateDTO state;
  state.joint_names = trajectory.joint_names;

  begin_index = std::min(begin_index, trajectory.waypoints.size());
  end_index = std::min(end_index, trajectory.waypoints.size());
  if (end_index < begin_index) {
    end_index = begin_index;
  }

  for (std::size_t i = begin_index; i < end_index; ++i) {
    const auto local_index = static_cast<std::int64_t>(i - begin_index);
    if (cancel.is_cancel_requested()) {
      mark_cancelled(bundle,
                     local_index,
                     waypoint_time_from_start_ns(trajectory, i),
                     cancel.reason());
      bundle.final_world_revision = snapshot.revision;
      return bundle;
    }
    const auto& wp = trajectory.waypoints[i];
    state.positions_rad = wp.positions_rad;

    std::optional<std::int64_t> time_from_start_ns =
        static_cast<std::int64_t>(wp.time_from_start_s * 1e9);
    auto record = classify_one(snapshot, engine, state,
                               local_index,
                               time_from_start_ns,
                               bundle.borrowed_world_revision, cancel);
    const auto wp_decision = record.result.decision;
    const auto wp_severity = severity_for(wp_decision);
    const bool had_prior_failure = first_failing_waypoint.has_value();

    if (wp_severity > worst_severity ||
        (wp_severity == worst_severity &&
         wp_decision != CollisionDecisionResult::kPass &&
         worst_decision == CollisionDecisionResult::kPass)) {
      worst_decision = wp_decision;
      worst_severity = wp_severity;
    }
    if (wp_decision != CollisionDecisionResult::kPass &&
        !first_failing_waypoint.has_value()) {
      first_failing_waypoint = record.waypoint_index;
      first_failing_time_ns = record.time_from_start_ns;
    }
    const bool record_cancelled =
        record.result.engine_status == CollisionEngineStatus::kCancelled;
    const auto cancelled_index = record.waypoint_index;
    const auto cancelled_time_from_start_ns = record.time_from_start_ns;
    std::string cancelled_reason;
    if (record_cancelled) {
      const auto reason_it =
          record.result.raw_diagnostics.find("cancel_reason");
      cancelled_reason = reason_it != record.result.raw_diagnostics.end()
                             ? reason_it->second
                             : cancel.reason();
    }
    bundle.waypoint_records.push_back(std::move(record));
    if (record_cancelled) {
      if (had_prior_failure) {
        bundle.decision.accepted = false;
        bundle.decision.severity = worst_severity;
        bundle.decision.action = action_for(worst_decision);
        bundle.decision.first_failing_waypoint = first_failing_waypoint;
        bundle.decision.first_failing_time_ns = first_failing_time_ns;
        bundle.decision.collision_world_revision = snapshot.revision;
      }
      mark_cancelled(bundle,
                     cancelled_index,
                     cancelled_time_from_start_ns,
                     cancelled_reason);
      bundle.final_world_revision = snapshot.revision;
      return bundle;
    }
  }

  apply_windowed_contact_constraints(trajectory, bundle);

  worst_decision = CollisionDecisionResult::kPass;
  worst_severity = CollisionDecisionSeverity::kNone;
  first_failing_waypoint.reset();
  first_failing_time_ns.reset();
  for (auto& record : bundle.waypoint_records) {
    const auto wp_decision = record.result.decision;
    const auto wp_severity = severity_for(wp_decision);
    if (wp_severity > worst_severity ||
        (wp_severity == worst_severity &&
         wp_decision != CollisionDecisionResult::kPass &&
         worst_decision == CollisionDecisionResult::kPass)) {
      worst_decision = wp_decision;
      worst_severity = wp_severity;
    }
    if (wp_decision != CollisionDecisionResult::kPass &&
        !first_failing_waypoint.has_value()) {
      first_failing_waypoint = record.waypoint_index;
      first_failing_time_ns = record.time_from_start_ns;
    }
  }

  apply_contact_release_constraints(trajectory,
                                    bundle,
                                    worst_decision,
                                    worst_severity,
                                    first_failing_waypoint,
                                    first_failing_time_ns);

  bundle.decision.accepted = (worst_decision == CollisionDecisionResult::kPass);
  bundle.decision.severity = worst_severity;
  bundle.decision.action = action_for(worst_decision);
  bundle.decision.first_failing_waypoint = first_failing_waypoint;
  bundle.decision.first_failing_time_ns = first_failing_time_ns;
  bundle.decision.collision_world_revision = bundle.borrowed_world_revision;
  bundle.final_world_revision = snapshot.revision;
  if (cancel.is_cancel_requested()) {
    const auto cancel_index =
        begin_index >= end_index
            ? std::int64_t{0}
            : static_cast<std::int64_t>(end_index - begin_index - 1);
    const auto global_cancel_index =
        begin_index >= end_index ? begin_index : end_index - 1;
    mark_cancelled(bundle,
                   cancel_index,
                   waypoint_time_from_start_ns(trajectory,
                                               global_cancel_index),
                   cancel.reason());
  }
  return bundle;
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::validate_trajectory(
    const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
    const TrajectoryDTO& trajectory,
    const CollisionCancellationToken& cancel) {
  return run_pass_once(snapshot,
                       engine,
                       trajectory,
                       0,
                       trajectory.waypoints.size(),
                       cancel);
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::validate_trajectory_range(
    const CollisionWorldSnapshot& snapshot,
    ICollisionQueryEngine& engine,
    const TrajectoryDTO& trajectory,
    std::size_t begin_index,
    std::size_t end_index,
    const CollisionCancellationToken& cancel) {
  return run_pass_once(snapshot,
                       engine,
                       trajectory,
                       begin_index,
                       end_index,
                       cancel);
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::query_trajectory_range_unclassified(
    const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
    const TrajectoryDTO& trajectory, std::size_t begin_index, std::size_t end_index,
    const CollisionCancellationToken& cancel) {
  prepare_distance_selection();
  CollisionValidationDecisionBundle bundle;
  bundle.borrowed_world_revision = snapshot.revision;
  bundle.final_world_revision = snapshot.revision;
  bundle.decision.accepted = true;
  bundle.decision.severity = CollisionDecisionSeverity::kNone;
  bundle.decision.action = CollisionDecisionAction::kDispatch;
  bundle.decision.collision_world_revision = snapshot.revision;

  begin_index = std::min(begin_index, trajectory.waypoints.size());
  end_index = std::min(end_index, trajectory.waypoints.size());
  if (end_index < begin_index) {
    end_index = begin_index;
  }

  RobotStateDTO state;
  state.joint_names = trajectory.joint_names;
  for (std::size_t i = begin_index; i < end_index; ++i) {
    const auto local_index = static_cast<std::int64_t>(i - begin_index);
    const auto time_from_start_ns = waypoint_time_from_start_ns(trajectory, i);
    if (cancel.is_cancel_requested()) {
      mark_cancelled(bundle, local_index, time_from_start_ns, cancel.reason());
      return bundle;
    }

    state.positions_rad = trajectory.waypoints[i].positions_rad;
    auto query = make_query(state, local_index, time_from_start_ns, bundle.borrowed_world_revision);
    CollisionValidationWaypointRecord record;
    record.waypoint_index = local_index;
    record.time_from_start_ns = time_from_start_ns;
    if (!engine_admits_query_capabilities(engine.capability(),
                                          query.distance_request_mode,
                                          query.required_capabilities)) {
      record.result = capability_admission_failure(
          engine.capability(), query, bundle.borrowed_world_revision);
    } else {
      record.result = engine.check(snapshot, query, *policy_snapshot_, cancel);
    }
    record.result.distance_request_mode = query.distance_request_mode;
    record.result.evaluated_distance_pair_count =
        record.result.pair_clearances.size();
    const bool engine_cancelled = record.result.engine_status == CollisionEngineStatus::kCancelled;
    std::string cancellation_reason;
    if (engine_cancelled) {
      const auto reason_it = record.result.raw_diagnostics.find("cancel_reason");
      cancellation_reason =
          reason_it == record.result.raw_diagnostics.end() ? cancel.reason() : reason_it->second;
    }
    bundle.waypoint_records.push_back(std::move(record));
    if (engine_cancelled) {
      mark_cancelled(bundle, local_index, time_from_start_ns, std::move(cancellation_reason));
      return bundle;
    }
  }
  if (cancel.is_cancel_requested()) {
    const auto local_index = begin_index >= end_index
                                 ? std::int64_t{0}
                                 : static_cast<std::int64_t>(end_index - begin_index - 1);
    const auto global_index = begin_index >= end_index ? begin_index : end_index - 1;
    mark_cancelled(bundle, local_index, waypoint_time_from_start_ns(trajectory, global_index),
                   cancel.reason());
  }
  return bundle;
}

CollisionValidationDecisionBundle CollisionValidationCoordinator::finalize_prequeried_range(
    const CollisionWorldSnapshot& snapshot, const TrajectoryDTO& trajectory,
    CollisionValidationDecisionBundle queried,
    const SupportContactEvaluationStateMap& entering_support_state,
    SupportContactEvaluationStateMap* exiting_support_state) {
  // The ordered/parallel path constructs a fresh coordinator for this phase.
  // Restore the same distance-selection contract used by the query phase so
  // classification preserves truthful per-record observability.
  prepare_distance_selection();
  evaluator_.reset();
  if (!evaluator_.restore_support_contact_state(entering_support_state)) {
    throw std::invalid_argument("ordered collision-validation support state does not match policy");
  }

  const bool was_cancelled = queried.cancelled;
  const auto cancellation_reason = queried.cancellation_reason;
  const auto cancelled_at = queried.cancelled_at_waypoint_index;

  for (auto& record : queried.waypoint_records) {
    record = classify_result(std::move(record.result), record.waypoint_index,
                             record.time_from_start_ns, queried.borrowed_world_revision);
  }

  // Windowed-contact validation remains an explicit serial shape. Keeping this
  // call here makes the primitive complete and prevents a future caller from
  // accidentally omitting it when using prequeried records serially.
  apply_windowed_contact_constraints(trajectory, queried);

  CollisionDecisionResult worst = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_severity = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;
  for (const auto& record : queried.waypoint_records) {
    const auto decision = record.result.decision;
    const auto severity = severity_for(decision);
    if (severity > worst_severity ||
        (severity == worst_severity && decision != CollisionDecisionResult::kPass &&
         worst == CollisionDecisionResult::kPass)) {
      worst = decision;
      worst_severity = severity;
    }
    if (decision != CollisionDecisionResult::kPass && !first_failing_waypoint) {
      first_failing_waypoint = record.waypoint_index;
      first_failing_time_ns = record.time_from_start_ns;
    }
  }

  queried.decision.accepted = worst == CollisionDecisionResult::kPass;
  queried.decision.severity = worst_severity;
  queried.decision.action = action_for(worst);
  queried.decision.first_failing_waypoint = first_failing_waypoint;
  queried.decision.first_failing_time_ns = first_failing_time_ns;
  queried.decision.collision_world_revision = snapshot.revision;
  queried.final_world_revision = snapshot.revision;

  if (was_cancelled) {
    queried.cancelled = false;
    queried.cancellation_reason.reset();
    queried.cancelled_at_waypoint_index.reset();
    mark_cancelled(
        queried,
        cancelled_at.value_or(
            queried.waypoint_records.empty() ? 0 : queried.waypoint_records.back().waypoint_index),
        cancelled_at && *cancelled_at >= 0 &&
                static_cast<std::size_t>(*cancelled_at) < queried.waypoint_records.size()
            ? queried.waypoint_records[static_cast<std::size_t>(*cancelled_at)].time_from_start_ns
            : std::nullopt,
        cancellation_reason.value_or("collision_validation_cancelled"));
  }

  if (exiting_support_state != nullptr) {
    *exiting_support_state = evaluator_.support_contact_state();
  }
  return queried;
}

}  // namespace manyforge::core
