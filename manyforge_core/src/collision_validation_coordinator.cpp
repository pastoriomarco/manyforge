#include <manyforge_core/collision/collision_validation_coordinator.hpp>

#include <algorithm>
#include <chrono>
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

bool revisions_equal(const CollisionWorldRevision& a,
                     const CollisionWorldRevision& b) {
  return a.revision_id == b.revision_id && a.content_hash == b.content_hash;
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
  if (!snapshot.require_full_diagnostics) {
    return capabilities;
  }
  if (!snapshot.allowed_static_static_pairs.empty() ||
      !snapshot.allowed_touch_link_pairs.empty() ||
      !snapshot.support_contacts.empty()) {
    capabilities.push_back("body_pair_only");
  }
  if (!snapshot.support_contacts.empty()) {
    // Support-contact envelopes include max_start_penetration_m. Until a
    // coordinator receives per-pair penetration diagnostics, a full-diagnostic
    // support-contact policy must fail closed on engines that cannot provide
    // this capability.
    capabilities.push_back("penetration_depth");
  }
  std::sort(capabilities.begin(), capabilities.end());
  capabilities.erase(std::unique(capabilities.begin(), capabilities.end()),
                     capabilities.end());
  return capabilities;
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
    IPlanningContext& context, const RobotStateDTO& state,
    std::int64_t waypoint_index, std::optional<std::int64_t> time_from_start_ns,
    const CollisionWorldRevision& borrowed_revision) {
  auto query =
      make_query(state, waypoint_index, time_from_start_ns, borrowed_revision);
  return classify_result(context.collision_check_normalized(query),
                         waypoint_index, time_from_start_ns,
                         borrowed_revision);
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
    IPlanningContext& context, const TrajectoryDTO& trajectory) {
  CollisionValidationDecisionBundle bundle;
  bundle.borrowed_world_revision = context.current_collision_world_revision();

  evaluator_.reset();

  CollisionDecisionResult worst_decision = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_severity = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;

  for (std::size_t i = 0; i < trajectory.waypoints.size(); ++i) {
    const auto& wp = trajectory.waypoints[i];
    RobotStateDTO state;
    state.joint_names = trajectory.joint_names;
    state.positions_rad = wp.positions_rad;

    std::optional<std::int64_t> time_from_start_ns =
        static_cast<std::int64_t>(wp.time_from_start_s * 1e9);
    auto record = classify_one(context, state, static_cast<std::int64_t>(i),
                               time_from_start_ns,
                               bundle.borrowed_world_revision);
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
    bundle.waypoint_records.push_back(std::move(record));
  }

  for (const auto& support_entry : config_.policy_snapshot.support_contacts) {
    if (!support_entry.require_separation_by_end) {
      continue;
    }
    const auto required_pair = canonicalize(support_entry.pair);
    bool contact_started = false;
    bool separated = false;
    for (const auto& record : bundle.waypoint_records) {
      for (const auto& classified :
           record.result.normalized_pairs_classified) {
        if (canonicalize(classified.pair) != required_pair) {
          continue;
        }
        switch (classified.classification) {
          case CollisionPairClassification::kSupportContactStart:
          case CollisionPairClassification::kSupportContactContinued:
          case CollisionPairClassification::kSupportContactRecontactViolation:
            contact_started = true;
            break;
          case CollisionPairClassification::kSupportContactSeparated:
            separated = true;
            break;
          default:
            break;
        }
      }
    }
    if (contact_started && !separated) {
      worst_decision = CollisionDecisionResult::kFail;
      worst_severity = CollisionDecisionSeverity::kBlock;
      if (!first_failing_waypoint.has_value() && !trajectory.waypoints.empty()) {
        first_failing_waypoint =
            static_cast<std::int64_t>(trajectory.waypoints.size() - 1);
        const auto& final_wp = trajectory.waypoints.back();
        first_failing_time_ns =
            static_cast<std::int64_t>(final_wp.time_from_start_s * 1e9);
      }
    }
  }

  bundle.decision.accepted = (worst_decision == CollisionDecisionResult::kPass);
  bundle.decision.severity = worst_severity;
  bundle.decision.action = action_for(worst_decision);
  bundle.decision.first_failing_waypoint = first_failing_waypoint;
  bundle.decision.first_failing_time_ns = first_failing_time_ns;
  bundle.decision.collision_world_revision = bundle.borrowed_world_revision;

  bundle.final_world_revision = context.current_collision_world_revision();
  return bundle;
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

  for (const auto& support_entry : config_.policy_snapshot.support_contacts) {
    if (!support_entry.require_separation_by_end) {
      continue;
    }
    const auto required_pair = canonicalize(support_entry.pair);
    bool contact_started = false;
    bool separated = false;
    for (const auto& record : bundle.waypoint_records) {
      for (const auto& classified :
           record.result.normalized_pairs_classified) {
        if (canonicalize(classified.pair) != required_pair) {
          continue;
        }
        switch (classified.classification) {
          case CollisionPairClassification::kSupportContactStart:
          case CollisionPairClassification::kSupportContactContinued:
          case CollisionPairClassification::kSupportContactRecontactViolation:
            contact_started = true;
            break;
          case CollisionPairClassification::kSupportContactSeparated:
            separated = true;
            break;
          default:
            break;
        }
      }
    }
    if (contact_started && !separated) {
      worst_decision = CollisionDecisionResult::kFail;
      worst_severity = CollisionDecisionSeverity::kBlock;
      if (!first_failing_waypoint.has_value() && !trajectory.waypoints.empty()) {
        first_failing_waypoint =
            static_cast<std::int64_t>(trajectory.waypoints.size() - 1);
        const auto& final_wp = trajectory.waypoints.back();
        first_failing_time_ns =
            static_cast<std::int64_t>(final_wp.time_from_start_s * 1e9);
      }
    }
  }

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
CollisionValidationCoordinator::validate_state(IPlanningContext& context,
                                                const RobotStateDTO& state) {
  TrajectoryDTO single;
  single.joint_names = state.joint_names;
  TrajectoryWaypointDTO wp;
  wp.positions_rad = state.positions_rad;
  wp.time_from_start_s = 0.0;
  single.waypoints.push_back(std::move(wp));
  return validate_trajectory(context, single);
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::validate_trajectory(
    const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
    const TrajectoryDTO& trajectory,
    const CollisionCancellationToken& cancel) {
  return run_pass_once(snapshot, engine, trajectory, cancel);
}

CollisionValidationDecisionBundle
CollisionValidationCoordinator::validate_trajectory(
    IPlanningContext& context, const TrajectoryDTO& trajectory) {
  auto bundle = run_pass_once(context, trajectory);

  if (!config_.require_pose_source_borrow) {
    return bundle;
  }

  const auto final_rev =
      bundle.final_world_revision.value_or(CollisionWorldRevision{});
  if (revisions_equal(bundle.borrowed_world_revision, final_rev)) {
    return bundle;
  }

  // Revision changed mid-pass. Retry up to max_count times within
  // retry_wall_time_ns wall budget. Surface scene_changed_during_validation
  // and downgrade the decision to inconclusive if all retries are exhausted.
  const auto retry_start_ns = clock_now_();
  std::set<std::pair<std::uint64_t, std::string>> observed_revisions;
  auto record_revisions = [&observed_revisions](
                              const CollisionValidationDecisionBundle& b) {
    observed_revisions.insert({b.borrowed_world_revision.revision_id,
                               b.borrowed_world_revision.content_hash});
    if (b.final_world_revision) {
      observed_revisions.insert({b.final_world_revision->revision_id,
                                 b.final_world_revision->content_hash});
    }
  };
  record_revisions(bundle);
  bool churn_window_exceeded = false;
  for (int retry_idx = 0;
       retry_idx < config_.pose_source_retry.max_count; ++retry_idx) {
    const auto elapsed_ns = clock_now_() - retry_start_ns;
    if (elapsed_ns > config_.pose_source_retry.retry_wall_time_ns) {
      break;
    }
    if (config_.pose_source_retry.churn_max_revisions > 0 &&
        elapsed_ns <= config_.pose_source_retry.churn_window_ns &&
        static_cast<int>(observed_revisions.size()) >
            config_.pose_source_retry.churn_max_revisions) {
      churn_window_exceeded = true;
      break;
    }
    bundle = run_pass_once(context, trajectory);
    bundle.pose_source_retries_used = retry_idx + 1;
    record_revisions(bundle);
    const auto post_retry_elapsed_ns = clock_now_() - retry_start_ns;
    if (config_.pose_source_retry.churn_max_revisions > 0 &&
        post_retry_elapsed_ns <= config_.pose_source_retry.churn_window_ns &&
        static_cast<int>(observed_revisions.size()) >
            config_.pose_source_retry.churn_max_revisions) {
      churn_window_exceeded = true;
      break;
    }
    const auto rev_after =
        bundle.final_world_revision.value_or(CollisionWorldRevision{});
    if (revisions_equal(bundle.borrowed_world_revision, rev_after)) {
      return bundle;
    }
  }

  bundle.scene_changed_during_validation = true;
  bundle.churn_window_exceeded = churn_window_exceeded;
  bundle.decision.accepted = false;
  bundle.decision.severity = CollisionDecisionSeverity::kBlock;
  bundle.decision.action = CollisionDecisionAction::kInconclusive;
  return bundle;
}

}  // namespace manyforge::core
