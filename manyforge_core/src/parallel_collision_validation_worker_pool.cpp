#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <manyforge_core/types/reason_codes.hpp>

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

CollisionValidationDecisionBundle exception_bundle(
    const CollisionWorldSnapshot& snapshot,
    const char* reason_code,
    const char* exception_type,
    const char* exception_message) {
  CollisionValidationDecisionBundle bundle = inconclusive_bundle();
  bundle.borrowed_world_revision = snapshot.revision;
  bundle.final_world_revision = snapshot.revision;
  bundle.decision.first_failing_waypoint = 0;
  bundle.decision.collision_world_revision = snapshot.revision;

  CollisionValidationWaypointRecord record;
  record.waypoint_index = 0;
  record.produced_decision_change = true;
  record.result.engine_status = CollisionEngineStatus::kEngineError;
  record.result.decision = CollisionDecisionResult::kInconclusiveFail;
  record.result.engine_id = "parallel_collision_validation_worker_pool";
  record.result.raw_diagnostics["reason_code"] = reason_code;
  record.result.raw_diagnostics["exception_type"] = exception_type;
  record.result.raw_diagnostics["exception"] = exception_message;
  record.result.collision_world_revision = snapshot.revision;
  bundle.waypoint_records.push_back(std::move(record));
  return bundle;
}

}  // namespace

struct ParallelCollisionValidationWorkerPool::WorkerSlot {
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
  std::function<void()> job;
  std::thread thread;
};

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

ParallelCollisionValidationWorkerPool::~ParallelCollisionValidationWorkerPool() {
  stop_worker_threads();
}

void ParallelCollisionValidationWorkerPool::ensure_worker_threads(int count) {
  if (count <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(worker_slots_mutex_);
  while (static_cast<int>(worker_slots_.size()) < count) {
    auto slot = std::make_unique<WorkerSlot>();
    WorkerSlot* slot_ptr = slot.get();
    slot->thread =
        std::thread([this, slot_ptr]() { worker_loop(*slot_ptr); });
    worker_slots_.push_back(std::move(slot));
  }
}

void ParallelCollisionValidationWorkerPool::stop_worker_threads() {
  std::vector<std::unique_ptr<WorkerSlot>> slots;
  {
    std::lock_guard<std::mutex> lock(worker_slots_mutex_);
    slots.swap(worker_slots_);
  }
  for (auto& slot : slots) {
    {
      std::lock_guard<std::mutex> lock(slot->mutex);
      slot->stop = true;
      slot->job = nullptr;
    }
    slot->cv.notify_one();
  }
  for (auto& slot : slots) {
    if (slot->thread.joinable()) {
      slot->thread.join();
    }
  }
}

void ParallelCollisionValidationWorkerPool::worker_loop(WorkerSlot& slot) {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(slot.mutex);
      slot.cv.wait(lock, [&slot]() {
        return slot.stop || static_cast<bool>(slot.job);
      });
      if (slot.stop && !slot.job) {
        return;
      }
      job = std::move(slot.job);
      slot.job = nullptr;
    }
    try {
      if (job) {
        job();
      }
    } catch (...) {
      // Worker jobs catch engine/coordinator failures and encode them in their
      // output bundle. This guard preserves the pool's liveness if a future
      // bug lets an exception escape.
    }
    signal_job_complete();
  }
}

void ParallelCollisionValidationWorkerPool::signal_job_complete() {
  std::lock_guard<std::mutex> lock(completion_mutex_);
  if (pending_jobs_ > 0) {
    --pending_jobs_;
  }
  if (pending_jobs_ == 0) {
    completion_cv_.notify_all();
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
  return validate_trajectory(snapshot,
                             worker_engines,
                             trajectory,
                             cancel,
                             config_.pass_config);
}

CollisionValidationDecisionBundle
ParallelCollisionValidationWorkerPool::validate_trajectory(
    const CollisionWorldSnapshot& snapshot,
    const std::vector<ICollisionQueryEngine*>& worker_engines,
    const TrajectoryDTO& trajectory,
    const CollisionCancellationToken& cancel,
    CollisionValidationPassConfig pass_config) {
  std::lock_guard<std::mutex> call_lock(validation_call_mutex_);

  std::vector<ICollisionQueryEngine*> available_engines;
  available_engines.reserve(worker_engines.size());
  bool skipped_duplicate_single_context_engine = false;
  for (ICollisionQueryEngine* engine : worker_engines) {
    if (engine == nullptr) {
      continue;
    }
    if (engine->capability().thread_safety ==
            CollisionEngineThreadSafetyClass::kSingleContextExclusive &&
        std::find(available_engines.begin(), available_engines.end(),
                  engine) != available_engines.end()) {
      skipped_duplicate_single_context_engine = true;
      continue;
    }
    available_engines.push_back(engine);
  }

  const auto previous_geometry_count = config_.collision_geometry_count;
  struct GeometryCountRestore {
    Config& config;
    std::int64_t previous;
    ~GeometryCountRestore() { config.collision_geometry_count = previous; }
  } restore{config_, previous_geometry_count};
  config_.collision_geometry_count = snapshot.geometry_count;

  const auto& effective_policy = pass_config.effective_policy_snapshot();
  const bool support_contact_present =
      !effective_policy.support_contacts.empty();
  const bool contact_grasp_present =
      !pass_config.contact_policy_id.empty() &&
      !effective_policy.contact_grasps.empty();
  const int waypoint_count = static_cast<int>(trajectory.waypoints.size());
  const int worker_count_requested = config_.worker_count;
  const int worker_count_available = static_cast<int>(available_engines.size());

  auto run_engine_serial_fallback =
      [&](ICollisionQueryEngine& engine, std::string fallback_reason)
      -> CollisionValidationDecisionBundle {
    const auto t0 = steady_now_ns();
    CollisionValidationDecisionBundle bundle;
    try {
      CollisionValidationCoordinator coord(pass_config);
      bundle = coord.validate_trajectory(snapshot, engine, trajectory, cancel);
    } catch (const std::exception& e) {
      bundle = exception_bundle(
          snapshot,
          reason_codes::kCollisionValidationSerialFallbackException,
          "std_exception",
          e.what());
    } catch (...) {
      bundle = exception_bundle(
          snapshot,
          reason_codes::kCollisionValidationSerialFallbackException,
          reason_codes::kUnknownNonStdException,
          reason_codes::kUnknownNonStdException);
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
    return run_engine_serial_fallback(
        *available_engines[0],
        skipped_duplicate_single_context_engine
            ? "single_context_duplicate_engine"
            : "worker_count_one");
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

  auto run_worker = [&snapshot, &trajectory, &cancel, &pass_config](
                        int worker_index,
                        ICollisionQueryEngine* engine,
                        int range_begin,
                        int range_end) -> WorkerOut {
    WorkerOut out;
    out.worker_index = worker_index;
    out.range_begin = range_begin;
    const auto t0 = steady_now_ns();
    try {
      CollisionValidationPassConfig per_worker_cfg = pass_config;
      CollisionValidationCoordinator coord(std::move(per_worker_cfg));
      out.bundle = coord.validate_trajectory_range(
          snapshot,
          *engine,
          trajectory,
          static_cast<std::size_t>(range_begin),
          static_cast<std::size_t>(range_end),
          cancel);
    } catch (const std::exception& e) {
      out.bundle =
          exception_bundle(snapshot,
                           reason_codes::kCollisionValidationWorkerException,
                           "std_exception",
                           e.what());
    } catch (...) {
      out.bundle =
          exception_bundle(snapshot,
                           reason_codes::kCollisionValidationWorkerException,
                           reason_codes::kUnknownNonStdException,
                           reason_codes::kUnknownNonStdException);
    }
    out.wall_ns = steady_now_ns() - t0;
    return out;
  };

  const auto pool_t0 = steady_now_ns();
  std::vector<WorkerOut> outputs(static_cast<std::size_t>(desired_workers));
  ensure_worker_threads(desired_workers);
  {
    std::lock_guard<std::mutex> lock(completion_mutex_);
    pending_jobs_ = desired_workers;
  }
  {
    std::lock_guard<std::mutex> slots_lock(worker_slots_mutex_);
    for (int w = 0; w < desired_workers; ++w) {
      auto& slot = *worker_slots_[static_cast<std::size_t>(w)];
      const auto range_begin = ranges[static_cast<std::size_t>(w)].first;
      const auto range_end = ranges[static_cast<std::size_t>(w)].second;
      {
        std::lock_guard<std::mutex> slot_lock(slot.mutex);
        slot.job = [&, w, range_begin, range_end]() {
          outputs[static_cast<std::size_t>(w)] =
              run_worker(w, available_engines[static_cast<std::size_t>(w)],
                         range_begin, range_end);
        };
      }
      slot.cv.notify_one();
    }
  }
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completion_cv_.wait(lock, [this]() { return pending_jobs_ == 0; });
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
  std::optional<std::int64_t> cancelled_at_time_ns;
  std::optional<std::string> cancellation_reason;

  auto waypoint_time_ns =
      [&trajectory](std::int64_t waypoint_index)
      -> std::optional<std::int64_t> {
    if (waypoint_index < 0 ||
        static_cast<std::size_t>(waypoint_index) >=
            trajectory.waypoints.size()) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(
        trajectory.waypoints[static_cast<std::size_t>(waypoint_index)]
            .time_from_start_s *
        1e9);
  };

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
        cancelled_at_time_ns = waypoint_time_ns(candidate);
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
        first_failing_time_ns =
            out.bundle.decision.first_failing_time_ns
                ? out.bundle.decision.first_failing_time_ns
                : waypoint_time_ns(*first_failing_wp);
      }
    }
    for (auto& rec : out.bundle.waypoint_records) {
      rec.waypoint_index += out.range_begin;
      const auto rec_waypoint_index = rec.waypoint_index;
      const auto rec_time_from_start_ns = rec.time_from_start_ns;
      const auto rec_decision = rec.result.decision;
      merged.waypoint_records.push_back(std::move(rec));
      if (rec_decision != CollisionDecisionResult::kPass &&
          !first_failing_wp.has_value()) {
        first_failing_wp = rec_waypoint_index;
        first_failing_time_ns = rec_time_from_start_ns;
      }
      const auto rec_sev = severity_for(rec_decision);
      if (rec_sev > worst_sev ||
          (rec_sev == worst_sev &&
           rec_decision != CollisionDecisionResult::kPass &&
           worst == CollisionDecisionResult::kPass)) {
        worst = rec_decision;
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
  if (!cancelled && cancel.is_cancel_requested()) {
    cancelled = true;
    const auto late_cancel_index =
        trajectory.waypoints.empty()
            ? std::int64_t{0}
            : static_cast<std::int64_t>(trajectory.waypoints.size() - 1);
    cancelled_at_waypoint_index = late_cancel_index;
    cancelled_at_time_ns = waypoint_time_ns(late_cancel_index);
    cancellation_reason = cancel.reason();
  }
  if (cancelled) {
    merged.cancelled = true;
    merged.cancellation_reason = cancellation_reason;
    merged.cancelled_at_waypoint_index = cancelled_at_waypoint_index;
    merged.decision.accepted = false;
    const bool earlier_reject =
        merged.decision.action != CollisionDecisionAction::kDispatch &&
        first_failing_wp && cancelled_at_waypoint_index &&
        *first_failing_wp < *cancelled_at_waypoint_index;
    if (!earlier_reject) {
      merged.decision.severity = CollisionDecisionSeverity::kBlock;
      merged.decision.action = CollisionDecisionAction::kInconclusive;
    }
    if (!earlier_reject && cancelled_at_waypoint_index) {
      merged.decision.first_failing_waypoint = cancelled_at_waypoint_index;
      merged.decision.first_failing_time_ns = cancelled_at_time_ns;
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
    const auto ref_t0 = steady_now_ns();
    try {
      CollisionValidationCoordinator ref_coord(pass_config);
      (void)ref_coord.validate_trajectory(snapshot, *available_engines[0],
                                          trajectory, cancel);
    } catch (const std::exception&) {
      last_telemetry_.serial_reference_wall_ns.reset();
      return merged;
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
  } catch (const std::exception&) {
    return clamp(default_value);
  }
}

}  // namespace manyforge::core
