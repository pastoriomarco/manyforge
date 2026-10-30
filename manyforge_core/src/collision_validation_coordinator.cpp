#include <algorithm>
#include <chrono>
#include <cmath>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
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

bool has_active_contact_grasp(const CollisionPolicySnapshot& snapshot,
                              const std::string& active_contact_policy_id) {
  return !active_contact_policy_id.empty() &&
         std::any_of(snapshot.contact_grasps.begin(),
                     snapshot.contact_grasps.end(),
                     [&](const ContactGraspPolicyEntry& entry) {
                       return entry.policy_id == active_contact_policy_id;
                     });
}

CollisionDistanceSelection distance_selection_for(
    const CollisionPolicySnapshot& snapshot,
    const CollisionWorldSnapshot& world,
    const std::string& active_contact_policy_id) {
  CollisionDistanceSelection selection;
  selection.selected_pairs = build_selected_collision_distance_pairs(
      snapshot, world, active_contact_policy_id);
  if (!selection.selected_pairs.empty()) {
    selection.mode = CollisionDistanceRequestMode::kSelectedCollidingPairs;
  }
  return selection;
}

std::vector<std::string> required_capabilities_for(
    const CollisionPolicySnapshot& snapshot,
    const std::string& active_contact_policy_id,
    CollisionDistanceRequestMode distance_request_mode) {
  std::vector<std::string> capabilities;
  const bool has_support_contacts = !snapshot.support_contacts.empty();
  const bool has_contact_grasps =
      has_active_contact_grasp(snapshot, active_contact_policy_id);
  if (!snapshot.require_full_diagnostics && !has_support_contacts &&
      !has_contact_grasps) {
    return capabilities;
  }
  if (!snapshot.allowed_static_static_pairs.empty() ||
      !snapshot.allowed_touch_link_pairs.empty() ||
      has_support_contacts || has_contact_grasps) {
    capabilities.push_back("body_pair_only");
  }
  if (distance_request_mode != CollisionDistanceRequestMode::kNone) {
    capabilities.push_back("penetration_depth");
  }
  if (has_support_contacts) {
    for (const auto& entry : snapshot.support_contacts) {
      if (entry.mode == CollisionContactPolicyMode::kContactProcess) {
        capabilities.push_back("contact_process");
      }
    }
  }
  std::sort(capabilities.begin(), capabilities.end());
  capabilities.erase(std::unique(capabilities.begin(), capabilities.end()),
                     capabilities.end());
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
                                    const SupportContactPolicyEntry& entry,
                                    CollisionDecisionResult decision,
                                    std::string reason,
                                    std::optional<double> initial_depth,
                                    std::optional<double> current_depth) {
  record.result.raw_diagnostics["contact_release_violation"] =
      std::move(reason);
  record.result.raw_diagnostics["contact_release_pair"] = pair_text(entry.pair);
  record.result.raw_diagnostics["contact_release_mode"] =
      entry.mode == CollisionContactPolicyMode::kContactProcess
          ? "contact_process"
          : entry.mode == CollisionContactPolicyMode::kBoundedPenetration
                ? "bounded_penetration"
                : "contact_release";
  if (entry.mode == CollisionContactPolicyMode::kBoundedPenetration) {
    record.result.raw_diagnostics[
        "contact_release_max_contact_penetration_m"] =
        std::to_string(entry.max_contact_penetration_m);
  } else if (entry.mode == CollisionContactPolicyMode::kContactRelease) {
    const bool signed_penetration =
        entry.release_evidence_mode ==
        ContactReleaseEvidenceMode::kSignedPenetration;
    record.result.raw_diagnostics["contact_release_evidence_mode"] =
        signed_penetration ? "signed_penetration" : "binary_contact";
    if (signed_penetration) {
      record.result.raw_diagnostics["contact_release_tolerance_m"] =
          std::to_string(entry.release_tolerance_m);
      record.result.raw_diagnostics[
          "contact_release_enforce_max_start_penetration"] =
          entry.enforce_max_start_penetration ? "true" : "false";
      record.result.raw_diagnostics[
          "contact_release_max_start_penetration_m"] =
          std::to_string(entry.max_start_penetration_m);
      record.result.raw_diagnostics["contact_release_min_progress_m"] =
          std::to_string(entry.min_release_progress_m);
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
      } else if (entry.mode ==
                 CollisionContactPolicyMode::kBoundedPenetration) {
        classified.classification =
            CollisionPairClassification::kBlockingCollision;
      }
      classified.is_first_failure = true;
      break;
    }
  }
}

void set_contact_grasp_diagnostic(CollisionValidationWaypointRecord& record,
                                  const ContactGraspPolicyEntry& entry,
                                  const CollisionPairId& pair,
                                  std::string reason,
                                  std::optional<double> clearance,
                                  double sample_fraction,
                                  bool inside_terminal_window) {
  record.result.raw_diagnostics["contact_grasp_violation"] = reason;
  record.result.raw_diagnostics["contact_grasp_policy_id"] = entry.policy_id;
  record.result.raw_diagnostics["contact_grasp_selected_object_id"] =
      entry.selected_object_id;
  record.result.raw_diagnostics["contact_grasp_pair"] = pair_text(pair);
  record.result.raw_diagnostics["contact_grasp_max_penetration_m"] =
      std::to_string(entry.max_penetration_m);
  record.result.raw_diagnostics["contact_grasp_terminal_window_start_fraction"] =
      std::to_string(entry.terminal_window_start_fraction);
  record.result.raw_diagnostics["contact_grasp_sample_fraction"] =
      std::to_string(sample_fraction);
  record.result.raw_diagnostics["contact_grasp_inside_terminal_window"] =
      inside_terminal_window ? "true" : "false";
  record.result.raw_diagnostics["contact_grasp_allowed_contact"] = "false";
  if (clearance) {
    record.result.raw_diagnostics["contact_grasp_signed_clearance_m"] =
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
    case CollisionPairClassification::kAllowedContactGrasp:
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

void CollisionValidationCoordinator::prepare_distance_selection(
    const CollisionWorldSnapshot& snapshot) {
  auto selection = distance_selection_for(
      *policy_snapshot_, snapshot, config_.contact_policy_id);
  distance_request_mode_ = selection.mode;
  selected_distance_pairs_ = std::move(selection.selected_pairs);
  required_capabilities_ = required_capabilities_for(
      *policy_snapshot_, config_.contact_policy_id, distance_request_mode_);
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
                              const SupportContactPolicyEntry& entry,
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

  for (const auto& support_entry : policy_snapshot_->support_contacts) {
    const double tolerance = std::max(0.0, support_entry.release_tolerance_m);
    const double min_progress =
        std::max(0.0, support_entry.min_release_progress_m);
    ContactReleaseEvaluationState state;
    std::optional<double> current_depth;
    bool sample_violation = false;

    for (auto& record : bundle.waypoint_records) {
      ContactReleaseSampleEvidence evidence;
      evidence.in_contact = pair_present(record.result, support_entry.pair);
      if (evidence.in_contact &&
          (support_entry.mode ==
               CollisionContactPolicyMode::kBoundedPenetration ||
           (support_entry.mode ==
                CollisionContactPolicyMode::kContactRelease &&
            support_entry.release_evidence_mode ==
                ContactReleaseEvidenceMode::kSignedPenetration))) {
        evidence.signed_clearance_m =
            clearance_for_pair(record.result, support_entry.pair);
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
                                    kMissingOrInvalidSignedPenetration ||
          transition.failure == ContactReleaseTransitionFailure::
                                    kContactProcessNotImplemented;
      record_violation(
          record, support_entry,
          inconclusive ? CollisionDecisionResult::kInconclusiveFail
                       : CollisionDecisionResult::kFail,
          contact_release_transition_failure_name(transition.failure),
          state.initial_depth_m, transition.penetration_depth_m);
      sample_violation = true;
      break;
    }

    if (sample_violation ||
        support_entry.mode ==
            CollisionContactPolicyMode::kBoundedPenetration ||
        !state.started_in_contact ||
        first_failing_waypoint.has_value() ||
        !config_.enforce_contact_release_trajectory_end) {
      continue;
    }

    auto& final_record = bundle.waypoint_records.back();
    if (support_entry.require_separation_by_end && state.in_contact) {
      record_violation(final_record,
                       support_entry,
                       CollisionDecisionResult::kFail,
                       "final_sample_not_separated",
                       state.initial_depth_m,
                       current_depth);
      continue;
    }
    if (support_entry.release_evidence_mode ==
            ContactReleaseEvidenceMode::kSignedPenetration &&
        support_entry.require_strictly_decreasing_penetration &&
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

  (void)trajectory;
}

void CollisionValidationCoordinator::apply_contact_grasp_constraints(
    const TrajectoryDTO& trajectory,
    CollisionValidationDecisionBundle& bundle) const {
  if (config_.contact_policy_id.empty() || bundle.waypoint_records.empty()) {
    return;
  }

  const ContactGraspPolicyEntry* active = nullptr;
  for (const auto& entry : policy_snapshot_->contact_grasps) {
    if (entry.policy_id == config_.contact_policy_id) {
      active = &entry;
      break;
    }
  }
  if (active == nullptr) {
    auto& first = bundle.waypoint_records.front();
    first.result.decision = CollisionDecisionResult::kInconclusiveFail;
    first.produced_decision_change = true;
    first.result.raw_diagnostics["contact_grasp_violation"] =
        reason_codes::kUnknownContactPolicyId;
    first.result.raw_diagnostics["contact_grasp_policy_id"] =
        config_.contact_policy_id;
    return;
  }
  if (active->selected_object_id.empty() || active->allowed_robot_links.empty() ||
      !std::isfinite(active->max_penetration_m) ||
      active->max_penetration_m < 0.0 ||
      !std::isfinite(active->terminal_window_start_fraction) ||
      active->terminal_window_start_fraction < 0.0 ||
      active->terminal_window_start_fraction > 1.0) {
    auto& first = bundle.waypoint_records.front();
    first.result.decision = CollisionDecisionResult::kInconclusiveFail;
    first.produced_decision_change = true;
    first.result.raw_diagnostics["contact_grasp_violation"] =
        reason_codes::kInvalidContactGraspPolicy;
    first.result.raw_diagnostics["contact_grasp_policy_id"] = active->policy_id;
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
      first.result.raw_diagnostics["contact_grasp_violation"] =
          reason_codes::kInvalidContactGraspSampleFractions;
      first.result.raw_diagnostics["contact_grasp_policy_id"] = active->policy_id;
      return;
    }
  }

  const auto last_index =
      bundle.waypoint_records.size() > 1 ? bundle.waypoint_records.size() - 1 : 1;
  for (std::size_t i = 0; i < bundle.waypoint_records.size(); ++i) {
    auto& record = bundle.waypoint_records[i];
    if (record.result.engine_status != CollisionEngineStatus::kOk &&
        record.result.engine_status !=
            CollisionEngineStatus::kDiagnosticIncomplete) {
      continue;
    }
    const double fraction = original_fractions != nullptr ? (*original_fractions)[i]
                            : bundle.waypoint_records.size() <= 1
                                ? 1.0
                                : static_cast<double>(i) / static_cast<double>(last_index);
    const bool is_plan_initial_sample = original_fractions != nullptr ? fraction <= 1e-12 : i == 0;
    const bool inside_terminal_window =
        fraction + 1e-12 >= active->terminal_window_start_fraction;
    bool changed = false;
    for (auto& classified : record.result.normalized_pairs_classified) {
      if (classified.classification !=
              CollisionPairClassification::kBlockingCollision &&
          classified.classification !=
              CollisionPairClassification::kDiagnosticIncomplete) {
        continue;
      }
      if (!contact_grasp_pair_matches(classified.pair, *active)) {
        continue;
      }
      const auto clearance = clearance_for_pair(record.result, classified.pair);
      if (!clearance || !std::isfinite(*clearance) || *clearance > 0.0) {
        record.result.engine_status =
            CollisionEngineStatus::kDiagnosticIncomplete;
        record.result.decision =
            CollisionDecisionResult::kInconclusiveFail;
        record.produced_decision_change = true;
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kMissingSignedClearance,
                                     std::nullopt,
                                     fraction,
                                     inside_terminal_window);
        continue;
      }
      // A finite metric cannot make an engine-inconclusive sample safe. Keep
      // the engine failure authoritative and do not relabel it as a grasp
      // envelope violation when the selected metric itself was complete.
      if (record.result.engine_status != CollisionEngineStatus::kOk) {
        continue;
      }
      const double depth = std::max(0.0, -*clearance);
      if (is_plan_initial_sample) {
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kInitialSampleInContact,
                                     clearance,
                                     fraction,
                                     inside_terminal_window);
        continue;
      }
      if (!inside_terminal_window) {
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kContactBeforeTerminalWindow,
                                     clearance,
                                     fraction,
                                     inside_terminal_window);
        continue;
      }
      if (depth > active->max_penetration_m + 1e-9) {
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kPenetrationExceedsPolicy,
                                     clearance,
                                     fraction,
                                     inside_terminal_window);
        continue;
      }
      classified.classification =
          CollisionPairClassification::kAllowedContactGrasp;
      classified.is_first_failure = false;
      record.result.raw_diagnostics["contact_grasp_policy_id"] =
          active->policy_id;
      record.result.raw_diagnostics["contact_grasp_allowed_pair"] =
          pair_text(classified.pair);
      changed = true;
    }
    if (changed) {
      record.result.decision = decision_from_classifications(record.result);
      record.produced_decision_change =
          record.result.decision != CollisionDecisionResult::kPass;
    }
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
  prepare_distance_selection(snapshot);
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

  apply_contact_grasp_constraints(trajectory, bundle);

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
  prepare_distance_selection(snapshot);
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
  prepare_distance_selection(snapshot);
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

  // Contact-grasp validation remains an explicit serial shape. Keeping this
  // call here makes the primitive complete and prevents a future caller from
  // accidentally omitting it when using prequeried records serially.
  apply_contact_grasp_constraints(trajectory, queried);

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
