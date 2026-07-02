#include <manyforge_core/collision/collision_validation_coordinator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
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
                    std::string reason) {
  bundle.cancelled = true;
  bundle.cancellation_reason = std::move(reason);
  bundle.decision.accepted = false;
  bundle.cancelled_at_waypoint_index = waypoint_index;
  const bool earlier_reject =
      bundle.decision.action == CollisionDecisionAction::kReject &&
      bundle.decision.first_failing_waypoint &&
      *bundle.decision.first_failing_waypoint < waypoint_index;
  if (!earlier_reject) {
    bundle.decision.severity = CollisionDecisionSeverity::kBlock;
    bundle.decision.action = CollisionDecisionAction::kInconclusive;
    bundle.decision.first_failing_waypoint = waypoint_index;
  }
}

CollisionPairId canonicalize(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
}

std::vector<std::string> required_capabilities_for(
    const CollisionPolicySnapshot& snapshot) {
  std::vector<std::string> capabilities;
  const bool has_support_contacts = !snapshot.support_contacts.empty();
  if (!snapshot.require_full_diagnostics && !has_support_contacts) {
    return capabilities;
  }
  if (!snapshot.allowed_static_static_pairs.empty() ||
      !snapshot.allowed_touch_link_pairs.empty() ||
      has_support_contacts) {
    capabilities.push_back("body_pair_only");
  }
  if (has_support_contacts) {
    capabilities.push_back("distance");
    capabilities.push_back("penetration_depth");
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

}  // namespace

CollisionValidationCoordinator::CollisionValidationCoordinator(
    CollisionValidationPassConfig config, ClockNow clock_now)
    : config_(std::move(config)),
      evaluator_(config_.policy_snapshot),
      clock_now_(clock_now ? std::move(clock_now) : &default_steady_now) {}

CollisionDecisionResult
CollisionValidationCoordinator::merge_engine_and_policy_decision(
    CollisionDecisionResult engine_decision,
    CollisionDecisionResult policy_decision) {
  // Fail-closed engine status cannot be downgraded by an allow / dispatch
  // policy result. Mirrors merge_policy_collision_decision in the RoboPlan
  // planning context so the two surfaces stay coherent.
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
  record.result.policy_revision_used =
      std::to_string(config_.policy_snapshot.revision.revision_id);

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

  for (const auto& support_entry : config_.policy_snapshot.support_contacts) {
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

CollisionQuery CollisionValidationCoordinator::make_query(
    const RobotStateDTO& state, std::int64_t waypoint_index,
    std::optional<std::int64_t> time_from_start_ns,
    const CollisionWorldRevision& borrowed_revision) const {
  CollisionQuery query;
  query.robot_state = state;
  query.surface = config_.surface;
  query.collision_world_revision = borrowed_revision;
  query.policy_revision =
      std::to_string(config_.policy_snapshot.revision.revision_id);
  query.state_time_ns = time_from_start_ns;
  query.waypoint_index = waypoint_index;
  query.query_id = "validation_coordinator_w" + std::to_string(waypoint_index);
  query.required_capabilities =
      required_capabilities_for(config_.policy_snapshot);
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
  return classify_result(engine.check(snapshot, query, config_.policy_snapshot,
                                      cancel),
                         waypoint_index, time_from_start_ns,
                         borrowed_revision);
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::run_pass_once(
    const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
    const TrajectoryDTO& trajectory,
    const CollisionCancellationToken& cancel) {
  CollisionValidationDecisionBundle bundle;
  bundle.borrowed_world_revision = snapshot.revision;

  evaluator_.reset();

  CollisionDecisionResult worst_decision = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_severity = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;

  for (std::size_t i = 0; i < trajectory.waypoints.size(); ++i) {
    if (cancel.is_cancel_requested()) {
      mark_cancelled(bundle, static_cast<std::int64_t>(i), cancel.reason());
      bundle.final_world_revision = snapshot.revision;
      return bundle;
    }
    const auto& wp = trajectory.waypoints[i];
    RobotStateDTO state;
    state.joint_names = trajectory.joint_names;
    state.positions_rad = wp.positions_rad;

    std::optional<std::int64_t> time_from_start_ns =
        static_cast<std::int64_t>(wp.time_from_start_s * 1e9);
    auto record = classify_one(snapshot, engine, state,
                               static_cast<std::int64_t>(i),
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
    const auto cancelled_reason =
        record.result.raw_diagnostics.contains("cancel_reason")
            ? record.result.raw_diagnostics.at("cancel_reason")
            : cancel.reason();
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
      mark_cancelled(bundle, cancelled_index, cancelled_reason);
      bundle.final_world_revision = snapshot.revision;
      return bundle;
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
  return bundle;
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::validate_trajectory(
    const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
    const TrajectoryDTO& trajectory,
    const CollisionCancellationToken& cancel) {
  return run_pass_once(snapshot, engine, trajectory, cancel);
}

}  // namespace manyforge::core
