#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <utility>

namespace manyforge::core {

namespace {

std::int64_t steady_now_ns() {
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

CollisionDecisionResult result_for_action(CollisionDecisionAction action) {
  switch (action) {
    case CollisionDecisionAction::kDispatch:
      return CollisionDecisionResult::kPass;
    case CollisionDecisionAction::kWarnOnly:
      return CollisionDecisionResult::kWarn;
    case CollisionDecisionAction::kReject:
      return CollisionDecisionResult::kFail;
    case CollisionDecisionAction::kInconclusive:
      return CollisionDecisionResult::kInconclusiveFail;
  }
  return CollisionDecisionResult::kInconclusiveFail;
}

bool revisions_equal(const CollisionWorldRevision& a,
                     const CollisionWorldRevision& b) {
  return a.revision_id == b.revision_id && a.content_hash == b.content_hash;
}

CollisionValidationDecisionBundle inconclusive_bundle() {
  CollisionValidationDecisionBundle bundle;
  bundle.decision.accepted = false;
  bundle.decision.severity = CollisionDecisionSeverity::kBlock;
  bundle.decision.action = CollisionDecisionAction::kInconclusive;
  return bundle;
}

}  // namespace

ParallelCollisionValidationWorkerPool::ParallelCollisionValidationWorkerPool(
    Config config)
    : config_(std::move(config)) {}

CollisionValidationDecisionBundle
ParallelCollisionValidationWorkerPool::run_serial_fallback(
    IPlanningContext& context, const TrajectoryDTO& trajectory,
    std::string fallback_reason) {
  CollisionValidationCoordinator coord(config_.pass_config);
  const auto t0 = steady_now_ns();
  auto bundle = coord.validate_trajectory(context, trajectory);
  const auto t1 = steady_now_ns();
  last_telemetry_.workers_actually_used = 1;
  last_telemetry_.fallback_reason = std::move(fallback_reason);
  last_telemetry_.total_wall_ns = t1 - t0;
  last_telemetry_.total_worker_wall_ns = t1 - t0;
  last_telemetry_.per_worker_waypoint_counts = {
      static_cast<int>(trajectory.waypoints.size())};
  return bundle;
}

CollisionValidationDecisionBundle
ParallelCollisionValidationWorkerPool::validate_trajectory(
    const std::vector<IPlanningContext*>& worker_contexts,
    const TrajectoryDTO& trajectory) {
  if (worker_contexts.empty()) {
    CollisionValidationDecisionBundle bundle;
    bundle.decision.accepted = false;
    bundle.decision.severity = CollisionDecisionSeverity::kBlock;
    bundle.decision.action = CollisionDecisionAction::kInconclusive;
    last_telemetry_.fallback_reason = "no_worker_contexts";
    return bundle;
  }

  const int waypoint_count = static_cast<int>(trajectory.waypoints.size());
  const bool has_support_contacts =
      !config_.pass_config.policy_snapshot.support_contacts.empty() &&
      config_.refuse_parallel_with_support_contacts;
  int desired_workers = std::max(1, config_.worker_count);
  desired_workers =
      std::min(desired_workers, static_cast<int>(worker_contexts.size()));

  if (desired_workers <= 1) {
    return run_serial_fallback(*worker_contexts[0], trajectory,
                                "worker_count_one");
  }
  if (waypoint_count < config_.min_waypoints_for_parallel) {
    return run_serial_fallback(*worker_contexts[0], trajectory,
                                "trajectory_too_short");
  }
  if (has_support_contacts) {
    return run_serial_fallback(*worker_contexts[0], trajectory,
                                "support_contact_policy_active");
  }

  // Deterministic contiguous chunking.
  const int per_worker = waypoint_count / desired_workers;
  const int leftover = waypoint_count % desired_workers;
  std::vector<std::pair<int, int>> ranges;  // [begin, end)
  int cursor = 0;
  for (int w = 0; w < desired_workers; ++w) {
    const int len = per_worker + (w < leftover ? 1 : 0);
    ranges.emplace_back(cursor, cursor + len);
    cursor += len;
  }

  // Build per-worker sub-trajectories. Each sub-trajectory remaps waypoint
  // indices inside the worker, but we keep the original index alongside so
  // the merge picks the global lowest failing index.
  struct WorkerOut {
    int worker_index = 0;
    int range_begin = 0;
    CollisionValidationDecisionBundle bundle;
    std::int64_t wall_ns = 0;
  };

  auto run_worker = [this](int worker_index, IPlanningContext* ctx,
                           TrajectoryDTO sub_traj,
                           int range_begin) -> WorkerOut {
    WorkerOut out;
    out.worker_index = worker_index;
    out.range_begin = range_begin;
    CollisionValidationPassConfig per_worker_cfg = config_.pass_config;
    // Each worker borrows the revision independently; pool aggregates.
    CollisionValidationCoordinator coord(per_worker_cfg);
    const auto t0 = steady_now_ns();
    try {
      out.bundle = coord.validate_trajectory(*ctx, sub_traj);
    } catch (...) {
      out.bundle = inconclusive_bundle();
    }
    out.wall_ns = steady_now_ns() - t0;
    return out;
  };

  const auto pool_t0 = steady_now_ns();
  std::vector<std::future<WorkerOut>> futures;
  futures.reserve(desired_workers);
  for (int w = 0; w < desired_workers; ++w) {
    TrajectoryDTO sub;
    sub.joint_names = trajectory.joint_names;
    sub.waypoints.assign(trajectory.waypoints.begin() + ranges[w].first,
                         trajectory.waypoints.begin() + ranges[w].second);
    futures.push_back(std::async(std::launch::async, run_worker, w,
                                  worker_contexts[w], std::move(sub),
                                  ranges[w].first));
  }

  std::vector<WorkerOut> outputs;
  outputs.reserve(desired_workers);
  for (auto& f : futures) {
    outputs.push_back(f.get());
  }
  const auto pool_t1 = steady_now_ns();

  // Merge: build a global bundle in original trajectory order.
  CollisionValidationDecisionBundle merged;
  merged.borrowed_world_revision = outputs.front().bundle.borrowed_world_revision;
  merged.final_world_revision = outputs.back().bundle.final_world_revision;

  bool revision_mismatch = false;
  for (const auto& out : outputs) {
    if (out.bundle.scene_changed_during_validation) {
      revision_mismatch = true;
    }
    if (!revisions_equal(out.bundle.borrowed_world_revision,
                         merged.borrowed_world_revision)) {
      revision_mismatch = true;
    }
    const auto final_revision =
        out.bundle.final_world_revision.value_or(CollisionWorldRevision{});
    if (!revisions_equal(final_revision, merged.borrowed_world_revision)) {
      revision_mismatch = true;
    }
  }

  CollisionDecisionResult worst = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_sev = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_wp;
  std::optional<std::int64_t> first_failing_time_ns;

  std::sort(outputs.begin(), outputs.end(),
            [](const WorkerOut& a, const WorkerOut& b) {
              return a.worker_index < b.worker_index;
            });

  for (auto& out : outputs) {
    if (!out.bundle.decision.accepted ||
        out.bundle.decision.action != CollisionDecisionAction::kDispatch) {
      const auto bundle_result =
          (!out.bundle.decision.accepted &&
           out.bundle.decision.action == CollisionDecisionAction::kDispatch)
              ? CollisionDecisionResult::kInconclusiveFail
              : result_for_action(out.bundle.decision.action);
      const auto bundle_sev =
          std::max(out.bundle.decision.severity, severity_for(bundle_result));
      if (bundle_sev > worst_sev ||
          (bundle_sev == worst_sev &&
           bundle_result != CollisionDecisionResult::kPass &&
           worst == CollisionDecisionResult::kPass)) {
        worst = bundle_result;
        worst_sev = bundle_sev;
      }
      if (!first_failing_wp.has_value()) {
        first_failing_wp =
            out.bundle.decision.first_failing_waypoint
                ? (*out.bundle.decision.first_failing_waypoint + out.range_begin)
                : static_cast<std::int64_t>(out.range_begin);
        if (out.bundle.decision.first_failing_time_ns) {
          first_failing_time_ns = out.bundle.decision.first_failing_time_ns;
        }
      }
    }
    for (auto& rec : out.bundle.waypoint_records) {
      rec.waypoint_index += out.range_begin;
      merged.waypoint_records.push_back(rec);
      if (rec.result.decision != CollisionDecisionResult::kPass &&
          !first_failing_wp.has_value()) {
        first_failing_wp = rec.waypoint_index;
        first_failing_time_ns = rec.time_from_start_ns;
      }
      const auto rec_sev = severity_for(rec.result.decision);
      if (rec_sev > worst_sev ||
          (rec_sev == worst_sev &&
           rec.result.decision != CollisionDecisionResult::kPass &&
           worst == CollisionDecisionResult::kPass)) {
        worst = rec.result.decision;
        worst_sev = rec_sev;
      }
    }
  }

  // first_failing_wp is the GLOBAL lowest failing waypoint, not necessarily
  // produced by worker 0. Re-walk in original order to be deterministic.
  // (Loop above already iterates worker 0..N in waypoint order.)

  merged.decision.accepted = (worst == CollisionDecisionResult::kPass);
  merged.decision.severity = worst_sev;
  switch (worst) {
    case CollisionDecisionResult::kPass:
      merged.decision.action = CollisionDecisionAction::kDispatch;
      break;
    case CollisionDecisionResult::kWarn:
      merged.decision.action = CollisionDecisionAction::kWarnOnly;
      break;
    case CollisionDecisionResult::kFail:
    case CollisionDecisionResult::kStopRequested:
      merged.decision.action = CollisionDecisionAction::kReject;
      break;
    case CollisionDecisionResult::kInconclusiveFail:
      merged.decision.action = CollisionDecisionAction::kInconclusive;
      break;
  }
  merged.decision.first_failing_waypoint = first_failing_wp;
  merged.decision.first_failing_time_ns = first_failing_time_ns;
  merged.decision.collision_world_revision = merged.borrowed_world_revision;
  if (revision_mismatch) {
    merged.scene_changed_during_validation = true;
    merged.decision.accepted = false;
    merged.decision.severity = CollisionDecisionSeverity::kBlock;
    merged.decision.action = CollisionDecisionAction::kInconclusive;
  }

  last_telemetry_.workers_actually_used = desired_workers;
  last_telemetry_.fallback_reason.clear();
  last_telemetry_.total_wall_ns = pool_t1 - pool_t0;
  last_telemetry_.total_worker_wall_ns = 0;
  last_telemetry_.per_worker_waypoint_counts.clear();
  for (auto& out : outputs) {
    last_telemetry_.total_worker_wall_ns += out.wall_ns;
    last_telemetry_.per_worker_waypoint_counts.push_back(
        static_cast<int>(out.bundle.waypoint_records.size()));
  }
  return merged;
}

}  // namespace manyforge::core
