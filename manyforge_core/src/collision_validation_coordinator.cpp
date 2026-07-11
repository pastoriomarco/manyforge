#include <manyforge_core/collision/collision_validation_coordinator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <utility>

#include <manyforge_core/types/reason_codes.hpp>

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

std::vector<std::string> required_capabilities_for(
    const CollisionPolicySnapshot& snapshot,
    const std::string& active_contact_policy_id) {
  std::vector<std::string> capabilities;
  const bool has_support_contacts = !snapshot.support_contacts.empty();
  const bool has_contact_grasps =
      !active_contact_policy_id.empty() && !snapshot.contact_grasps.empty();
  if (!snapshot.require_full_diagnostics && !has_support_contacts &&
      !has_contact_grasps) {
    return capabilities;
  }
  if (!snapshot.allowed_static_static_pairs.empty() ||
      !snapshot.allowed_touch_link_pairs.empty() ||
      has_support_contacts || has_contact_grasps) {
    capabilities.push_back("body_pair_only");
  }
  if (has_support_contacts || has_contact_grasps) {
    capabilities.push_back("distance");
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

std::string pair_text(const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  return canonical.body_a + " <-> " + canonical.body_b;
}

void set_contact_release_diagnostic(CollisionValidationWaypointRecord& record,
                                    const SupportContactPolicyEntry& entry,
                                    std::string reason,
                                    std::optional<double> initial_depth,
                                    std::optional<double> current_depth) {
  record.result.raw_diagnostics["contact_release_violation"] =
      std::move(reason);
  record.result.raw_diagnostics["contact_release_pair"] = pair_text(entry.pair);
  record.result.raw_diagnostics["contact_release_mode"] =
      entry.mode == CollisionContactPolicyMode::kContactProcess
          ? "contact_process"
          : "contact_release";
  record.result.raw_diagnostics["contact_release_tolerance_m"] =
      std::to_string(entry.release_tolerance_m);
  record.result.raw_diagnostics["contact_release_enforce_max_start_penetration"] =
      entry.enforce_max_start_penetration ? "true" : "false";
  record.result.raw_diagnostics["contact_release_max_start_penetration_m"] =
      std::to_string(entry.max_start_penetration_m);
  record.result.raw_diagnostics["contact_release_min_progress_m"] =
      std::to_string(entry.min_release_progress_m);
  record.result.raw_diagnostics["contact_release_min_separation_clearance_m"] =
      std::to_string(entry.min_separation_clearance_m);
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
      classified.is_first_failure = true;
      break;
    }
  }
}

bool contains_segment(const std::string& text, const std::string& segment) {
  return !segment.empty() && text.find(segment) != std::string::npos;
}

bool contact_grasp_pair_matches(const CollisionPairId& pair,
                                const ContactGraspPolicyEntry& entry) {
  auto object_matches = [&](const std::string& body) {
    return contains_segment(body, "object/" + entry.selected_object_id + "/");
  };
  auto link_matches = [&](const std::string& body) {
    for (const auto& link : entry.allowed_robot_links) {
      if (contains_segment(body, "/link/" + link + "/collision/")) {
        return true;
      }
    }
    return false;
  };
  return (object_matches(pair.body_a) && link_matches(pair.body_b)) ||
         (object_matches(pair.body_b) && link_matches(pair.body_a));
}

void set_contact_grasp_diagnostic(CollisionValidationWaypointRecord& record,
                                  const ContactGraspPolicyEntry& entry,
                                  const CollisionPairId& pair,
                                  std::string reason,
                                  std::optional<double> clearance) {
  record.result.raw_diagnostics["contact_grasp_violation"] = reason;
  record.result.raw_diagnostics["contact_grasp_policy_id"] = entry.policy_id;
  record.result.raw_diagnostics["contact_grasp_selected_object_id"] =
      entry.selected_object_id;
  record.result.raw_diagnostics["contact_grasp_pair"] = pair_text(pair);
  record.result.raw_diagnostics["contact_grasp_max_penetration_m"] =
      std::to_string(entry.max_penetration_m);
  record.result.raw_diagnostics["contact_grasp_terminal_window_start_fraction"] =
      std::to_string(entry.terminal_window_start_fraction);
  if (clearance) {
    record.result.raw_diagnostics["contact_grasp_signed_clearance_m"] =
        std::to_string(*clearance);
  }
  for (auto& classified : record.result.normalized_pairs_classified) {
    if (canonicalize(classified.pair) == canonicalize(pair)) {
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
          std::to_string(policy_snapshot_->revision.revision_id)),
      required_capabilities_(required_capabilities_for(
          *policy_snapshot_, config_.contact_policy_id)) {}

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
                                   std::move(reason),
                                   initial_depth,
                                   current_depth);
    if (!first_failing_waypoint.has_value()) {
      first_failing_waypoint = record.waypoint_index;
      first_failing_time_ns = record.time_from_start_ns;
    }
  };

  if (bundle.waypoint_records.empty()) {
    return;
  }

  for (const auto& support_entry : policy_snapshot_->support_contacts) {
    if (support_entry.mode == CollisionContactPolicyMode::kContactProcess) {
      record_violation(bundle.waypoint_records.front(),
                       support_entry,
                       CollisionDecisionResult::kInconclusiveFail,
                       "contact_process_policy_not_implemented",
                       std::nullopt,
                       std::nullopt);
      continue;
    }

    const double tolerance = std::max(0.0, support_entry.release_tolerance_m);
    const double max_start =
        std::max(0.0, support_entry.max_start_penetration_m);
    const double min_progress =
        std::max(0.0, support_entry.min_release_progress_m);

    bool started_in_contact = false;
    bool separated = false;
    std::optional<double> initial_depth;
    double previous_depth = 0.0;
    double best_depth = std::numeric_limits<double>::infinity();

    for (std::size_t i = 0; i < bundle.waypoint_records.size(); ++i) {
      auto& record = bundle.waypoint_records[i];
      const auto clearance = clearance_for_pair(record.result,
                                                support_entry.pair);
      if (!clearance || !std::isfinite(*clearance)) {
        record_violation(record,
                         support_entry,
                         CollisionDecisionResult::kInconclusiveFail,
                         "missing_contact_release_clearance",
                         initial_depth,
                         std::nullopt);
        break;
      }

      const double depth = std::max(0.0, -*clearance);
      const bool separated_now =
          *clearance >= support_entry.min_separation_clearance_m;

      if (i == 0) {
        if (depth <= tolerance || separated_now) {
          previous_depth = 0.0;
          best_depth = 0.0;
          continue;
        }
        started_in_contact = true;
        initial_depth = depth;
        previous_depth = depth;
        best_depth = depth;
        if (support_entry.enforce_max_start_penetration &&
            depth > max_start + tolerance) {
          record_violation(record,
                           support_entry,
                           CollisionDecisionResult::kFail,
                           "initial_penetration_exceeds_policy",
                           initial_depth,
                           depth);
          break;
        }
        continue;
      }

      if (!started_in_contact) {
        if (depth > tolerance && !separated_now) {
          record_violation(record,
                           support_entry,
                           CollisionDecisionResult::kFail,
                           "new_active_contact_after_clear_start",
                           initial_depth,
                           depth);
          break;
        }
        continue;
      }

      if (separated) {
        if (depth > tolerance && !separated_now) {
          if (!support_entry.allow_recontact) {
            record_violation(record,
                             support_entry,
                             CollisionDecisionResult::kFail,
                             "recontact_after_release",
                             initial_depth,
                             depth);
            break;
          }
          if (support_entry.enforce_max_start_penetration &&
              depth > max_start + tolerance) {
            record_violation(record,
                             support_entry,
                             CollisionDecisionResult::kFail,
                             "recontact_penetration_exceeds_policy",
                             initial_depth,
                             depth);
            break;
          }
        }
      }

      if (separated_now) {
        separated = true;
        previous_depth = 0.0;
        best_depth = std::min(best_depth, 0.0);
        continue;
      }

      if (depth > previous_depth + tolerance) {
        record_violation(record,
                         support_entry,
                         CollisionDecisionResult::kFail,
                         "penetration_increased",
                         initial_depth,
                         depth);
        break;
      }
      previous_depth = depth;
      best_depth = std::min(best_depth, depth);
    }

    if (!started_in_contact || first_failing_waypoint.has_value()) {
      continue;
    }

    auto& final_record = bundle.waypoint_records.back();
    const auto final_clearance =
        clearance_for_pair(final_record.result, support_entry.pair);
    const double final_depth =
        final_clearance ? std::max(0.0, -*final_clearance) : previous_depth;
    if (support_entry.require_separation_by_end && !separated) {
      record_violation(final_record,
                       support_entry,
                       CollisionDecisionResult::kFail,
                       "final_sample_not_separated",
                       initial_depth,
                       final_depth);
      continue;
    }
    if (support_entry.require_strictly_decreasing_penetration && !separated) {
      const double required_progress = std::max(min_progress, tolerance);
      const double progress = *initial_depth - best_depth;
      if (progress + tolerance < required_progress) {
        record_violation(final_record,
                         support_entry,
                         CollisionDecisionResult::kFail,
                         "insufficient_release_progress",
                         initial_depth,
                         final_depth);
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

  const auto last_index =
      bundle.waypoint_records.size() > 1 ? bundle.waypoint_records.size() - 1 : 1;
  for (std::size_t i = 0; i < bundle.waypoint_records.size(); ++i) {
    auto& record = bundle.waypoint_records[i];
    if (record.result.engine_status != CollisionEngineStatus::kOk) {
      continue;
    }
    const double fraction =
        bundle.waypoint_records.size() <= 1
            ? 1.0
            : static_cast<double>(i) / static_cast<double>(last_index);
    bool changed = false;
    for (auto& classified : record.result.normalized_pairs_classified) {
      if (classified.classification !=
          CollisionPairClassification::kBlockingCollision) {
        continue;
      }
      if (!contact_grasp_pair_matches(classified.pair, *active)) {
        continue;
      }
      const auto clearance = clearance_for_pair(record.result, classified.pair);
      if (!clearance || !std::isfinite(*clearance)) {
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kMissingSignedClearance,
                                     std::nullopt);
        continue;
      }
      const double depth = std::max(0.0, -*clearance);
      if (i == 0) {
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kInitialSampleInContact,
                                     clearance);
        continue;
      }
      if (fraction + 1e-12 < active->terminal_window_start_fraction) {
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kContactBeforeTerminalWindow,
                                     clearance);
        continue;
      }
      if (depth > active->max_penetration_m + 1e-9) {
        set_contact_grasp_diagnostic(record,
                                     *active,
                                     classified.pair,
                                     reason_codes::kPenetrationExceedsPolicy,
                                     clearance);
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

}  // namespace manyforge::core
