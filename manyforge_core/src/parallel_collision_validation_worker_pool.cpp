#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
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
    : config_(std::move(config)) {
  config_.worker_count = std::max(1, config_.worker_count);
  config_.min_waypoints_for_parallel =
      std::max(1, config_.min_waypoints_for_parallel);
  if (config_.fallback_log_window < std::chrono::nanoseconds::zero()) {
    config_.fallback_log_window = std::chrono::nanoseconds::zero();
  }
}

void ParallelCollisionValidationWorkerPool::maybe_emit_fallback_log(
    const FallbackLogEvent& event) {
  if (!config_.fallback_log_sink) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  auto it = last_fallback_log_at_.find(event.cause);
  if (it != last_fallback_log_at_.end() &&
      (now - it->second) < config_.fallback_log_window) {
    return;  // Rate limited.
  }
  last_fallback_log_at_[event.cause] = now;
  config_.fallback_log_sink(event);
}

CollisionValidationDecisionBundle
ParallelCollisionValidationWorkerPool::validate_trajectory(
    const CollisionWorldSnapshot& snapshot,
    const std::vector<ICollisionQueryEngine*>& worker_engines,
    const TrajectoryDTO& trajectory,
    const CollisionCancellationToken& cancel) {
  std::vector<ICollisionQueryEngine*> available_engines;
  available_engines.reserve(worker_engines.size());
  for (ICollisionQueryEngine* engine : worker_engines) {
    if (engine != nullptr) {
      available_engines.push_back(engine);
    }
  }

  const auto previous_geometry_count = config_.collision_geometry_count;
  struct GeometryCountRestore {
    Config& config;
    std::int64_t previous;
    ~GeometryCountRestore() { config.collision_geometry_count = previous; }
  } restore{config_, previous_geometry_count};
  config_.collision_geometry_count = snapshot.geometry_count;

  const bool support_contact_present =
      !config_.pass_config.policy_snapshot.support_contacts.empty();
  const bool contact_grasp_present =
      !config_.pass_config.contact_policy_id.empty() &&
      !config_.pass_config.policy_snapshot.contact_grasps.empty();
  const int waypoint_count = static_cast<int>(trajectory.waypoints.size());
  const int worker_count_requested = config_.worker_count;
  const int worker_count_available = static_cast<int>(available_engines.size());

  auto run_engine_serial_fallback =
      [&](ICollisionQueryEngine& engine, std::string fallback_reason)
      -> CollisionValidationDecisionBundle {
    CollisionValidationCoordinator coord(config_.pass_config);
    const auto t0 = steady_now_ns();
    CollisionValidationDecisionBundle bundle;
    try {
      bundle = coord.validate_trajectory(snapshot, engine, trajectory, cancel);
    } catch (...) {
      bundle = inconclusive_bundle();
      bundle.borrowed_world_revision = snapshot.revision;
      bundle.final_world_revision = snapshot.revision;
    }
    const auto t1 = steady_now_ns();
    last_telemetry_.workers_actually_used = 1;
    last_telemetry_.fallback_reason = fallback_reason;
    last_telemetry_.total_wall_ns = t1 - t0;
    last_telemetry_.total_worker_wall_ns = t1 - t0;
    last_telemetry_.per_worker_waypoint_counts = {waypoint_count};
    last_telemetry_.support_contact_active = support_contact_present;
    last_telemetry_.collision_geometry_count = snapshot.geometry_count;
    last_telemetry_.validation_wall_time_p95_ns = 0;
    last_telemetry_.serial_reference_wall_ns.reset();

    FallbackLogEvent event;
    event.cause = std::move(fallback_reason);
    event.worker_count_requested = worker_count_requested;
    event.worker_count_available = worker_count_available;
    event.waypoint_count = waypoint_count;
    event.support_contact_active = support_contact_present;
    maybe_emit_fallback_log(event);
    return bundle;
  };

  if (available_engines.empty()) {
    CollisionValidationDecisionBundle bundle;
    bundle.decision.accepted = false;
    bundle.decision.severity = CollisionDecisionSeverity::kBlock;
    bundle.decision.action = CollisionDecisionAction::kInconclusive;
    last_telemetry_ = {};
    last_telemetry_.fallback_reason = "no_worker_contexts";
    last_telemetry_.support_contact_active = support_contact_present;
    last_telemetry_.collision_geometry_count = snapshot.geometry_count;
    FallbackLogEvent event;
    event.cause = "no_worker_contexts";
    event.worker_count_requested = worker_count_requested;
    event.worker_count_available = 0;
    event.waypoint_count = waypoint_count;
    event.support_contact_active = support_contact_present;
    maybe_emit_fallback_log(event);
    return bundle;
  }

  const bool has_support_contacts =
      support_contact_present && config_.refuse_parallel_with_support_contacts;
  int desired_workers = std::max(1, config_.worker_count);
  desired_workers =
      std::min(desired_workers, static_cast<int>(available_engines.size()));

  if (desired_workers <= 1) {
    return run_engine_serial_fallback(*available_engines[0],
                                      "worker_count_one");
  }
  if (waypoint_count < config_.min_waypoints_for_parallel) {
    return run_engine_serial_fallback(*available_engines[0],
                                      "trajectory_too_short");
  }
  if (has_support_contacts) {
    return run_engine_serial_fallback(*available_engines[0],
                                      "support_contact_policy_active");
  }
  if (contact_grasp_present) {
    return run_engine_serial_fallback(*available_engines[0],
                                      "contact_grasp_policy_active");
  }

  const int per_worker = waypoint_count / desired_workers;
  const int leftover = waypoint_count % desired_workers;
  std::vector<std::pair<int, int>> ranges;
  int cursor = 0;
  for (int w = 0; w < desired_workers; ++w) {
    const int len = per_worker + (w < leftover ? 1 : 0);
    ranges.emplace_back(cursor, cursor + len);
    cursor += len;
  }

  struct WorkerOut {
    int worker_index = 0;
    int range_begin = 0;
    CollisionValidationDecisionBundle bundle;
    std::int64_t wall_ns = 0;
  };

  auto run_worker = [this, &snapshot, &cancel](
                        int worker_index,
                        ICollisionQueryEngine* engine,
                        TrajectoryDTO sub_traj,
                        int range_begin) -> WorkerOut {
    WorkerOut out;
    out.worker_index = worker_index;
    out.range_begin = range_begin;
    CollisionValidationPassConfig per_worker_cfg = config_.pass_config;
    CollisionValidationCoordinator coord(per_worker_cfg);
    const auto t0 = steady_now_ns();
    try {
      out.bundle =
          coord.validate_trajectory(snapshot, *engine, sub_traj, cancel);
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
                                  available_engines[w], std::move(sub),
                                  ranges[w].first));
  }

  std::vector<WorkerOut> outputs;
  outputs.reserve(desired_workers);
  for (auto& f : futures) {
    outputs.push_back(f.get());
  }
  const auto pool_t1 = steady_now_ns();

  CollisionValidationDecisionBundle merged;
  merged.borrowed_world_revision = snapshot.revision;
  merged.final_world_revision = snapshot.revision;

  bool revision_mismatch = false;
  for (const auto& out : outputs) {
    if (out.bundle.scene_changed_during_validation) {
      revision_mismatch = true;
    }
    if (!revisions_equal(out.bundle.borrowed_world_revision, snapshot.revision)) {
      revision_mismatch = true;
    }
    const auto final_revision =
        out.bundle.final_world_revision.value_or(CollisionWorldRevision{});
    if (!revisions_equal(final_revision, snapshot.revision)) {
      revision_mismatch = true;
    }
  }

  CollisionDecisionResult worst = CollisionDecisionResult::kPass;
  CollisionDecisionSeverity worst_sev = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_wp;
  std::optional<std::int64_t> first_failing_time_ns;
  bool cancelled = false;
  std::optional<std::int64_t> cancelled_at_waypoint_index;
  std::optional<std::string> cancellation_reason;

  std::sort(outputs.begin(), outputs.end(),
            [](const WorkerOut& a, const WorkerOut& b) {
              return a.worker_index < b.worker_index;
            });

  for (auto& out : outputs) {
    if (out.bundle.cancelled) {
      cancelled = true;
      const auto candidate =
          out.bundle.cancelled_at_waypoint_index
              ? (*out.bundle.cancelled_at_waypoint_index + out.range_begin)
              : static_cast<std::int64_t>(out.range_begin);
      if (!cancelled_at_waypoint_index ||
          candidate < *cancelled_at_waypoint_index) {
        cancelled_at_waypoint_index = candidate;
        cancellation_reason = out.bundle.cancellation_reason;
      }
    }
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
  merged.decision.collision_world_revision = snapshot.revision;
  if (cancelled) {
    merged.cancelled = true;
    merged.cancellation_reason = cancellation_reason;
    merged.cancelled_at_waypoint_index = cancelled_at_waypoint_index;
    merged.decision.accepted = false;
    const bool earlier_reject =
        merged.decision.action == CollisionDecisionAction::kReject &&
        first_failing_wp && cancelled_at_waypoint_index &&
        *first_failing_wp < *cancelled_at_waypoint_index;
    if (!earlier_reject) {
      merged.decision.severity = CollisionDecisionSeverity::kBlock;
      merged.decision.action = CollisionDecisionAction::kInconclusive;
    }
    if (!earlier_reject && cancelled_at_waypoint_index) {
      merged.decision.first_failing_waypoint = cancelled_at_waypoint_index;
    }
  }
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
  last_telemetry_.support_contact_active = support_contact_present;
  last_telemetry_.collision_geometry_count = snapshot.geometry_count;
  last_telemetry_.serial_reference_wall_ns.reset();

  std::vector<std::int64_t> per_worker_walls;
  per_worker_walls.reserve(outputs.size());
  for (auto& out : outputs) {
    last_telemetry_.total_worker_wall_ns += out.wall_ns;
    last_telemetry_.per_worker_waypoint_counts.push_back(
        static_cast<int>(out.bundle.waypoint_records.size()));
    per_worker_walls.push_back(out.wall_ns);
  }
  if (per_worker_walls.size() >= 2) {
    std::sort(per_worker_walls.begin(), per_worker_walls.end());
    const std::size_t n = per_worker_walls.size();
    std::size_t idx = static_cast<std::size_t>((95 * n + 99) / 100);
    if (idx == 0) idx = 1;
    if (idx > n) idx = n;
    last_telemetry_.validation_wall_time_p95_ns = per_worker_walls[idx - 1];
  } else {
    last_telemetry_.validation_wall_time_p95_ns = 0;
  }

  if (config_.serial_reference_mode == SerialReferenceMode::kQualification) {
    CollisionValidationCoordinator ref_coord(config_.pass_config);
    const auto ref_t0 = steady_now_ns();
    try {
      (void)ref_coord.validate_trajectory(snapshot, *available_engines[0],
                                          trajectory, cancel);
    } catch (...) {
      last_telemetry_.serial_reference_wall_ns.reset();
      return merged;
    }
    const auto ref_t1 = steady_now_ns();
    last_telemetry_.serial_reference_wall_ns = ref_t1 - ref_t0;
  }

  return merged;
}

int env_validation_collision_workers(int default_value, int max_workers) {
  const int upper_bound = std::max(1, max_workers);
  auto clamp = [upper_bound](int value) {
    if (value < 1) {
      return 1;
    }
    return std::min(value, upper_bound);
  };
  const char* raw = std::getenv("MANYFORGE_VALIDATION_COLLISION_WORKERS");
  if (raw == nullptr || raw[0] == '\0') {
    return clamp(default_value);
  }
  try {
    const std::string s(raw);
    std::size_t consumed = 0;
    const int parsed = std::stoi(s, &consumed);
    if (consumed != s.size() || parsed < 1) {
      return clamp(default_value);
    }
    return clamp(parsed);
  } catch (...) {
    return clamp(default_value);
  }
}

}  // namespace manyforge::core
