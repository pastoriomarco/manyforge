#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <limits>
#include <manyforge_core/collision/ordered_contact_validation.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

CollisionPairId canonical_pair(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
}

bool support_contact_identities_are_unique(const CollisionPolicySnapshot& policy) {
  std::vector<CollisionPairId> identities;
  identities.reserve(policy.support_contacts.size());
  for (const auto& entry : policy.support_contacts) {
    const auto pair = canonical_pair(entry.pair);
    if (std::find(identities.begin(), identities.end(), pair) != identities.end()) {
      return false;
    }
    identities.push_back(pair);
  }
  return true;
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

CollisionValidationDecisionBundle timeout_bundle(
    const CollisionWorldSnapshot& snapshot,
    std::int64_t timeout_ns,
    std::int64_t elapsed_ns) {
  CollisionValidationDecisionBundle bundle = inconclusive_bundle();
  bundle.borrowed_world_revision = snapshot.revision;
  bundle.final_world_revision = snapshot.revision;
  bundle.cancelled = true;
  bundle.cancellation_reason = "collision_validation_wall_timeout";
  bundle.cancelled_at_waypoint_index = 0;
  bundle.decision.first_failing_waypoint = 0;
  bundle.decision.collision_world_revision = snapshot.revision;

  CollisionValidationWaypointRecord record;
  record.waypoint_index = 0;
  record.produced_decision_change = true;
  record.result.engine_status = CollisionEngineStatus::kTimeout;
  record.result.decision = CollisionDecisionResult::kInconclusiveFail;
  record.result.engine_id = "parallel_collision_validation_worker_pool";
  record.result.raw_diagnostics["reason_code"] =
      "collision_validation_wall_timeout";
  record.result.raw_diagnostics["timeout_ns"] = std::to_string(timeout_ns);
  record.result.raw_diagnostics["elapsed_ns"] = std::to_string(elapsed_ns);
  record.result.collision_world_revision = snapshot.revision;
  bundle.waypoint_records.push_back(std::move(record));
  return bundle;
}

CollisionValidationDecisionBundle non_owning_timeout_unsupported_bundle(
    const CollisionWorldSnapshot& snapshot) {
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
  record.result.raw_diagnostics["reason_code"] =
      "collision_validation_wall_timeout_requires_owned_lifetimes";
  record.result.collision_world_revision = snapshot.revision;
  bundle.waypoint_records.push_back(std::move(record));
  return bundle;
}

CollisionValidationDecisionBundle workers_draining_after_timeout_bundle(
    const CollisionWorldSnapshot& snapshot, std::uint64_t active_workers) {
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
  record.result.raw_diagnostics["reason_code"] =
      "collision_validation_workers_draining_after_timeout";
  record.result.raw_diagnostics["active_workers"] = std::to_string(active_workers);
  record.result.collision_world_revision = snapshot.revision;
  bundle.waypoint_records.push_back(std::move(record));
  return bundle;
}

struct WorkerOut {
  int worker_index = 0;
  int range_begin = 0;
  CollisionValidationDecisionBundle bundle;
  OrderedContactChunkSummary ordered_contact_summary;
  SupportContactEvaluationStateMap exiting_support_state;
  bool worker_exception = false;
  std::int64_t wall_ns = 0;
};

struct ValidationCallState {
  std::mutex mutex;
  std::condition_variable cv;
  int pending_jobs = 0;
  bool deadline_expired = false;
  std::vector<WorkerOut> outputs;
};

TimePoint saturating_deadline(TimePoint start, Duration timeout) {
  if (timeout > 0 && start > std::numeric_limits<TimePoint>::max() - timeout) {
    return std::numeric_limits<TimePoint>::max();
  }
  return start + timeout;
}

/// Arms one pass phase against the kernel's injected deadline authority. The
/// deadline and job completion linearize under the same call-state mutex: if
/// completion wins, a late watchdog is a no-op; if the watchdog wins, later
/// completion cannot turn the timed-out attempt into success.
class ValidationDeadlineAlarm final {
 public:
  ValidationDeadlineAlarm(IClock& clock, IScheduler& scheduler, TimePoint deadline,
                          std::shared_ptr<ValidationCallState> state,
                          std::shared_ptr<CollisionCancellationToken> cancel)
      : scheduler_(scheduler) {
    auto expire = [state = std::move(state), cancel = std::move(cancel)]() {
      bool expired = false;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->pending_jobs > 0 && !state->deadline_expired) {
          state->deadline_expired = true;
          expired = true;
        }
      }
      if (expired) {
        cancel->request_cancel("collision_validation_wall_timeout");
        state->cv.notify_all();
      }
    };

    if (clock.now() >= deadline) {
      expire();
      return;
    }
    handle_ = scheduler_.schedule(deadline, std::move(expire), SchedulerPriority::Monitoring);
  }

  ~ValidationDeadlineAlarm() {
    if (handle_) {
      scheduler_.cancel(*handle_);
    }
  }

  ValidationDeadlineAlarm(const ValidationDeadlineAlarm&) = delete;
  ValidationDeadlineAlarm& operator=(const ValidationDeadlineAlarm&) = delete;

 private:
  IScheduler& scheduler_;
  std::optional<ScheduleHandle> handle_;
};

/// Every queued job must retire its call-state slot even if a future edit lets
/// an exception escape the job body. Without this guard an unbounded caller
/// could wait forever while the persistent worker itself remained alive.
class PendingJobCompletion final {
 public:
  explicit PendingJobCompletion(std::shared_ptr<ValidationCallState> state)
      : state_(std::move(state)) {}

  PendingJobCompletion(const PendingJobCompletion&) = delete;
  PendingJobCompletion& operator=(const PendingJobCompletion&) = delete;

  ~PendingJobCompletion() {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->pending_jobs > 0) {
        --state_->pending_jobs;
      }
    }
    state_->cv.notify_all();
  }

 private:
  std::shared_ptr<ValidationCallState> state_;
};

}  // namespace

struct ParallelCollisionValidationWorkerPool::PoolCounters {
  std::atomic<std::uint64_t> queued_jobs{0};
  std::atomic<std::uint64_t> active_workers{0};
  std::atomic<std::uint64_t> completed_jobs_total{0};
  std::atomic<std::uint64_t> cancelled_jobs_total{0};
  std::atomic<std::uint64_t> dropped_jobs_total{0};
  std::atomic<std::uint64_t> abandoned_jobs_total{0};
};

struct ParallelCollisionValidationWorkerPool::WorkerSlot {
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
  bool abandoned = false;
  bool running = false;
  std::function<void()> job;
  std::thread thread;
  std::shared_ptr<PoolCounters> counters;
};

ParallelCollisionValidationWorkerPool::ParallelCollisionValidationWorkerPool(Config config)
    : config_(std::move(config)), counters_(std::make_shared<PoolCounters>()) {
  config_.worker_count = std::max(1, config_.worker_count);
  config_.min_waypoints_for_parallel =
      std::max(1, config_.min_waypoints_for_parallel);
  if (config_.fallback_log_window < std::chrono::nanoseconds::zero()) {
    config_.fallback_log_window = std::chrono::nanoseconds::zero();
  }
  if (config_.validation_wall_timeout < std::chrono::nanoseconds::zero()) {
    config_.validation_wall_timeout = std::chrono::nanoseconds::zero();
  }
  if (config_.validation_wall_timeout > std::chrono::nanoseconds::zero() &&
      (config_.clock == nullptr || config_.scheduler == nullptr)) {
    throw std::invalid_argument(
        "positive validation_wall_timeout requires injected clock and scheduler");
  }
  try {
    ensure_worker_threads(config_.worker_count);
  } catch (...) {
    stop_worker_threads();
    throw;
  }
}

ParallelCollisionValidationWorkerPool::~ParallelCollisionValidationWorkerPool() {
  stop_worker_threads();
  release_abandoned_worker_threads();
}

void ParallelCollisionValidationWorkerPool::ensure_worker_threads(int count) {
  if (count <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(worker_slots_mutex_);
  while (static_cast<int>(worker_slots_.size()) < count) {
    auto slot = std::make_shared<WorkerSlot>();
    slot->counters = counters_;
    slot->thread = std::thread([slot]() { worker_loop(slot); });
    worker_slots_.push_back(std::move(slot));
  }
}

void ParallelCollisionValidationWorkerPool::stop_worker_threads() {
  std::vector<std::shared_ptr<WorkerSlot>> slots;
  {
    std::lock_guard<std::mutex> lock(worker_slots_mutex_);
    slots.swap(worker_slots_);
  }
  for (auto& slot : slots) {
    {
      std::lock_guard<std::mutex> lock(slot->mutex);
      slot->stop = true;
      if (slot->job) {
        slot->counters->queued_jobs.fetch_sub(1, std::memory_order_relaxed);
        slot->counters->dropped_jobs_total.fetch_add(1, std::memory_order_relaxed);
      }
      slot->job = nullptr;
    }
    slot->cv.notify_one();
  }
  for (auto& slot : slots) {
    if (slot->thread.joinable()) {
      if (slot->abandoned) {
        slot->thread.detach();
      } else {
        slot->thread.join();
      }
    }
  }
}

void ParallelCollisionValidationWorkerPool::abandon_worker_threads() {
  std::vector<std::shared_ptr<WorkerSlot>> slots;
  {
    std::lock_guard<std::mutex> lock(worker_slots_mutex_);
    slots.swap(worker_slots_);
  }
  for (auto& slot : slots) {
    {
      std::lock_guard<std::mutex> lock(slot->mutex);
      slot->stop = true;
      slot->abandoned = true;
      if (slot->running) {
        slot->counters->abandoned_jobs_total.fetch_add(1, std::memory_order_relaxed);
      }
      if (slot->job) {
        slot->counters->queued_jobs.fetch_sub(1, std::memory_order_relaxed);
        slot->counters->dropped_jobs_total.fetch_add(1, std::memory_order_relaxed);
      }
      slot->job = nullptr;
    }
    slot->cv.notify_one();
  }
  {
    std::lock_guard<std::mutex> lock(abandoned_worker_slots_mutex_);
    abandoned_worker_slots_.insert(abandoned_worker_slots_.end(), slots.begin(), slots.end());
  }
}

bool ParallelCollisionValidationWorkerPool::reap_abandoned_worker_threads_if_drained() {
  std::vector<std::shared_ptr<WorkerSlot>> drained;
  {
    std::lock_guard<std::mutex> abandoned_lock(abandoned_worker_slots_mutex_);
    if (abandoned_worker_slots_.empty()) {
      return true;
    }
    for (const auto& slot : abandoned_worker_slots_) {
      std::lock_guard<std::mutex> slot_lock(slot->mutex);
      if (slot->running || slot->job) {
        return false;
      }
    }
    drained.swap(abandoned_worker_slots_);
  }
  for (auto& slot : drained) {
    slot->cv.notify_one();
    if (slot->thread.joinable()) {
      slot->thread.join();
    }
  }
  return true;
}

void ParallelCollisionValidationWorkerPool::release_abandoned_worker_threads() {
  std::vector<std::shared_ptr<WorkerSlot>> slots;
  {
    std::lock_guard<std::mutex> lock(abandoned_worker_slots_mutex_);
    slots.swap(abandoned_worker_slots_);
  }
  for (auto& slot : slots) {
    bool running = false;
    {
      std::lock_guard<std::mutex> lock(slot->mutex);
      slot->stop = true;
      running = slot->running;
    }
    slot->cv.notify_one();
    if (!slot->thread.joinable()) {
      continue;
    }
    if (running) {
      // A timed-out engine may ignore cancellation indefinitely. The worker
      // captures every object it uses by shared ownership, so teardown can
      // remain bounded without exposing that context to another validation.
      slot->thread.detach();
    } else {
      slot->thread.join();
    }
  }
}

void ParallelCollisionValidationWorkerPool::worker_loop(
    std::shared_ptr<WorkerSlot> slot) {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(slot->mutex);
      slot->cv.wait(lock, [&slot]() {
        return slot->stop || static_cast<bool>(slot->job);
      });
      if (slot->stop && !slot->job) {
        return;
      }
      job = std::move(slot->job);
      slot->job = nullptr;
      if (job) {
        slot->running = true;
        slot->counters->queued_jobs.fetch_sub(1, std::memory_order_relaxed);
        slot->counters->active_workers.fetch_add(1, std::memory_order_relaxed);
      }
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
    if (job) {
      slot->counters->active_workers.fetch_sub(1, std::memory_order_relaxed);
      slot->counters->completed_jobs_total.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(slot->mutex);
      slot->running = false;
    }
  }
}

ParallelValidationTelemetry ParallelCollisionValidationWorkerPool::last_telemetry() const {
  std::lock_guard<std::mutex> lock(validation_call_mutex_);
  return last_telemetry_;
}

ParallelValidationPoolStatus ParallelCollisionValidationWorkerPool::status() const {
  ParallelValidationPoolStatus snapshot;
  snapshot.queued_jobs = counters_->queued_jobs.load(std::memory_order_relaxed);
  snapshot.active_workers = counters_->active_workers.load(std::memory_order_relaxed);
  snapshot.completed_jobs_total = counters_->completed_jobs_total.load(std::memory_order_relaxed);
  snapshot.cancelled_jobs_total = counters_->cancelled_jobs_total.load(std::memory_order_relaxed);
  snapshot.dropped_jobs_total = counters_->dropped_jobs_total.load(std::memory_order_relaxed);
  snapshot.abandoned_jobs_total = counters_->abandoned_jobs_total.load(std::memory_order_relaxed);
  return snapshot;
}

void ParallelCollisionValidationWorkerPool::maybe_emit_fallback_log(
    const FallbackLogEvent& event) {
  if (!config_.fallback_log_sink) {
    return;
  }
  const auto now =
      config_.clock != nullptr ? config_.clock->now() : static_cast<TimePoint>(steady_now_ns());
  auto it = last_fallback_log_at_.find(event.cause);
  if (it != last_fallback_log_at_.end() &&
      (now - it->second) < config_.fallback_log_window.count()) {
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
    CollisionCancellationToken& cancel) {
  return validate_trajectory(snapshot,
                             worker_engines,
                             trajectory,
                             cancel,
                             config_.pass_config);
}

CollisionValidationDecisionBundle
ParallelCollisionValidationWorkerPool::validate_trajectory(
    const CollisionWorldSnapshot& snapshot,
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& worker_engines,
    const TrajectoryDTO& trajectory,
    const std::shared_ptr<CollisionCancellationToken>& cancel) {
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
    CollisionCancellationToken& cancel,
    CollisionValidationPassConfig pass_config) {
  if (config_.validation_wall_timeout > std::chrono::nanoseconds::zero()) {
    std::lock_guard<std::mutex> call_lock(validation_call_mutex_);
    last_telemetry_ = {};
    last_telemetry_.workers_actually_used = 0;
    last_telemetry_.workers_requested = config_.worker_count;
    last_telemetry_.workers_effective = 0;
    last_telemetry_.fallback_reason =
        "validation_wall_timeout_requires_owned_lifetimes";
    last_telemetry_.collision_geometry_count = snapshot.geometry_count;
    return non_owning_timeout_unsupported_bundle(snapshot);
  }
  std::vector<EngineRef> engine_refs;
  engine_refs.reserve(worker_engines.size());
  for (ICollisionQueryEngine* engine : worker_engines) {
    engine_refs.push_back(EngineRef{engine, {}});
  }
  auto cancel_ref =
      std::shared_ptr<CollisionCancellationToken>(
          &cancel, [](CollisionCancellationToken*) {});
  return validate_trajectory_impl(snapshot, engine_refs, trajectory, cancel_ref,
                                  std::move(pass_config), nullptr, std::nullopt);
}

CollisionValidationDecisionBundle
ParallelCollisionValidationWorkerPool::validate_trajectory(
    const CollisionWorldSnapshot& snapshot,
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& worker_engines,
    const TrajectoryDTO& trajectory,
    const std::shared_ptr<CollisionCancellationToken>& cancel,
    CollisionValidationPassConfig pass_config) {
  std::vector<EngineRef> engine_refs;
  engine_refs.reserve(worker_engines.size());
  for (const auto& engine : worker_engines) {
    engine_refs.push_back(EngineRef{engine.get(), engine});
  }
  return validate_trajectory_impl(snapshot, engine_refs, trajectory, cancel, std::move(pass_config),
                                  std::make_shared<const CollisionWorldSnapshot>(snapshot),
                                  std::nullopt);
}

CollisionValidationDecisionBundle ParallelCollisionValidationWorkerPool::validate_trajectory(
    CollisionWorldSnapshotPtr snapshot,
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& worker_engines,
    const TrajectoryDTO& trajectory, const std::shared_ptr<CollisionCancellationToken>& cancel,
    CollisionValidationPassConfig pass_config, std::optional<TimePoint> absolute_wall_deadline) {
  if (!snapshot) {
    CollisionValidationDecisionBundle bundle = inconclusive_bundle();
    bundle.decision.first_failing_waypoint = 0;
    return bundle;
  }
  std::vector<EngineRef> engine_refs;
  engine_refs.reserve(worker_engines.size());
  for (const auto& engine : worker_engines) {
    engine_refs.push_back(EngineRef{engine.get(), engine});
  }
  const auto& snapshot_ref = *snapshot;
  return validate_trajectory_impl(snapshot_ref, engine_refs, trajectory, cancel,
                                  std::move(pass_config), std::move(snapshot),
                                  absolute_wall_deadline);
}

CollisionValidationDecisionBundle ParallelCollisionValidationWorkerPool::validate_trajectory_impl(
    const CollisionWorldSnapshot& snapshot,
    const std::vector<ParallelCollisionValidationWorkerPool::EngineRef>& worker_engines,
    const TrajectoryDTO& trajectory, const std::shared_ptr<CollisionCancellationToken>& cancel,
    CollisionValidationPassConfig pass_config, CollisionWorldSnapshotPtr snapshot_owner,
    std::optional<TimePoint> absolute_wall_deadline) {
  std::lock_guard<std::mutex> call_lock(validation_call_mutex_);
  last_telemetry_ = {};
  last_telemetry_.workers_requested = config_.worker_count;
  last_telemetry_.workers_effective = 0;

  if (!cancel) {
    CollisionValidationDecisionBundle bundle = inconclusive_bundle();
    bundle.borrowed_world_revision = snapshot.revision;
    bundle.final_world_revision = snapshot.revision;
    bundle.decision.collision_world_revision = snapshot.revision;
    return bundle;
  }

  const auto wall_timeout = config_.validation_wall_timeout;
  const auto deadline_started_at = config_.clock != nullptr ? config_.clock->now() : TimePoint{0};
  const auto configured_wall_deadline =
      wall_timeout > std::chrono::nanoseconds::zero()
          ? saturating_deadline(deadline_started_at, wall_timeout.count())
          : TimePoint{0};
  const auto wall_deadline =
      absolute_wall_deadline && wall_timeout > std::chrono::nanoseconds::zero()
          ? std::min(configured_wall_deadline, *absolute_wall_deadline)
          : configured_wall_deadline;
  const auto effective_wall_timeout_ns =
      wall_timeout > std::chrono::nanoseconds::zero()
          ? std::max<std::int64_t>(0, wall_deadline - deadline_started_at)
          : std::int64_t{0};

  if (!reap_abandoned_worker_threads_if_drained()) {
    const auto active_workers = counters_->active_workers.load(std::memory_order_acquire);
    last_telemetry_.workers_actually_used = 0;
    last_telemetry_.workers_effective = 0;
    last_telemetry_.fallback_reason = "validation_workers_draining_after_timeout";
    last_telemetry_.collision_geometry_count = snapshot.geometry_count;
    return workers_draining_after_timeout_bundle(snapshot, active_workers);
  }

  std::vector<EngineRef> available_engines;
  available_engines.reserve(worker_engines.size());
  bool skipped_duplicate_single_context_engine = false;
  bool missing_single_context_identity = false;
  std::vector<std::string> admitted_single_context_ids;
  for (const auto& engine_ref : worker_engines) {
    ICollisionQueryEngine* engine = engine_ref.ptr;
    if (engine == nullptr) {
      continue;
    }
    const auto& capability = engine->capability();
    if (capability.thread_safety == CollisionEngineThreadSafetyClass::kSingleContextExclusive) {
      if (capability.mutable_context_id.empty()) {
        missing_single_context_identity = true;
      } else if (std::find(admitted_single_context_ids.begin(), admitted_single_context_ids.end(),
                           capability.mutable_context_id) != admitted_single_context_ids.end()) {
        skipped_duplicate_single_context_engine = true;
        continue;
      } else {
        admitted_single_context_ids.push_back(capability.mutable_context_id);
      }
    }
    available_engines.push_back(engine_ref);
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
  const bool contact_release_present = std::any_of(
      effective_policy.support_contacts.begin(), effective_policy.support_contacts.end(),
      [](const SupportContactPolicyEntry& entry) {
        return entry.mode == CollisionContactPolicyMode::kContactRelease;
      });
  const bool contact_process_present = std::any_of(
      effective_policy.support_contacts.begin(), effective_policy.support_contacts.end(),
      [](const SupportContactPolicyEntry& entry) {
        return entry.mode == CollisionContactPolicyMode::kContactProcess;
      });
  const bool contact_grasp_present =
      !pass_config.contact_policy_id.empty() &&
      !effective_policy.contact_grasps.empty();
  const int waypoint_count = static_cast<int>(trajectory.waypoints.size());
  const int worker_count_requested = config_.worker_count;
  const int worker_count_available = static_cast<int>(available_engines.size());

  if (wall_timeout > std::chrono::nanoseconds::zero() && config_.clock->now() >= wall_deadline) {
    cancel->request_cancel("collision_validation_wall_timeout");
    last_telemetry_.workers_actually_used = 0;
    last_telemetry_.workers_effective = worker_count_available;
    last_telemetry_.fallback_reason = "validation_wall_timeout";
    last_telemetry_.collision_geometry_count = snapshot.geometry_count;
    return timeout_bundle(snapshot, effective_wall_timeout_ns,
                          std::max<std::int64_t>(0, config_.clock->now() - deadline_started_at));
  }

  auto run_engine_serial_fallback =
      [&](EngineRef engine_ref, std::string fallback_reason)
      -> CollisionValidationDecisionBundle {
    const auto t0 = steady_now_ns();
    auto run_serial = [&]() -> CollisionValidationDecisionBundle {
      try {
        CollisionValidationCoordinator coord(pass_config);
        if (!support_contact_present) {
          return coord.validate_trajectory(snapshot, *engine_ref.ptr, trajectory, *cancel);
        }

        auto queried = coord.query_trajectory_range_unclassified(
            snapshot, *engine_ref.ptr, trajectory, 0, trajectory.waypoints.size(), *cancel);
        auto summary = summarize_ordered_contact_chunk(effective_policy, queried.waypoint_records,
                                                       0, trajectory.waypoints.size());
        auto support_state = pass_config.entering_support_contact_state.value_or(
            initial_support_contact_state(effective_policy));
        auto finalized = coord.finalize_prequeried_range(snapshot, trajectory, std::move(queried),
                                                         support_state, nullptr);
        std::string summary_error;
        const std::vector<OrderedContactChunkSummary> summaries{std::move(summary)};
        const auto release_violations = merge_contact_release_summaries(
            effective_policy, summaries, trajectory.waypoints.size(), &summary_error,
            pass_config.entering_contact_release_state
                ? &*pass_config.entering_contact_release_state
                : nullptr,
            pass_config.enforce_contact_release_trajectory_end, nullptr);
        if (summary_error.empty()) {
          (void)apply_contact_release_violations(effective_policy, release_violations, finalized,
                                                 &summary_error);
        }
        if (!summary_error.empty()) {
          return exception_bundle(snapshot, "ordered_contact_summary_invalid", "summary_error",
                                  summary_error.c_str());
        }
        return finalized;
      } catch (const std::exception& e) {
        return exception_bundle(
            snapshot,
            reason_codes::kCollisionValidationSerialFallbackException,
            "std_exception",
            e.what());
      } catch (...) {
        return exception_bundle(
            snapshot,
            reason_codes::kCollisionValidationSerialFallbackException,
            reason_codes::kUnknownNonStdException,
            reason_codes::kUnknownNonStdException);
      }
    };

    CollisionValidationDecisionBundle bundle;
    bool wall_timed_out = false;
    std::int64_t serial_worker_wall_ns = 0;
    int serial_waypoints_classified = 0;
    if (wall_timeout <= std::chrono::nanoseconds::zero()) {
      counters_->active_workers.fetch_add(1, std::memory_order_relaxed);
      const auto worker_t0 = steady_now_ns();
      bundle = run_serial();
      serial_worker_wall_ns = steady_now_ns() - worker_t0;
      serial_waypoints_classified = static_cast<int>(bundle.waypoint_records.size());
      counters_->active_workers.fetch_sub(1, std::memory_order_relaxed);
      counters_->completed_jobs_total.fetch_add(1, std::memory_order_relaxed);
      if (bundle.cancelled) {
        counters_->cancelled_jobs_total.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      auto state = std::make_shared<ValidationCallState>();
      state->pending_jobs = 1;
      state->outputs.resize(1);
      ValidationDeadlineAlarm deadline_alarm(*config_.clock, *config_.scheduler, wall_deadline,
                                             state, cancel);
      auto snapshot_copy = snapshot_owner
                               ? snapshot_owner
                               : std::make_shared<const CollisionWorldSnapshot>(snapshot);
      auto trajectory_copy = std::make_shared<TrajectoryDTO>(trajectory);
      auto pass_config_copy =
          std::make_shared<CollisionValidationPassConfig>(pass_config);
      const auto counters = counters_;
      ensure_worker_threads(1);
      {
        std::lock_guard<std::mutex> slots_lock(worker_slots_mutex_);
        auto slot = worker_slots_.front();
        {
          std::lock_guard<std::mutex> slot_lock(slot->mutex);
          slot->job = [state, snapshot_copy, trajectory_copy, pass_config_copy, engine_ref, cancel,
                       counters]() mutable {
            PendingJobCompletion completion(state);
            WorkerOut out;
            out.worker_index = 0;
            out.range_begin = 0;
            const auto worker_t0 = steady_now_ns();
            try {
              CollisionValidationCoordinator coord(*pass_config_copy);
              out.bundle = coord.validate_trajectory(*snapshot_copy, *engine_ref.ptr,
                                                     *trajectory_copy, *cancel);
            } catch (const std::exception& e) {
              out.bundle = exception_bundle(
                  *snapshot_copy, reason_codes::kCollisionValidationSerialFallbackException,
                  "std_exception", e.what());
            } catch (...) {
              out.bundle = exception_bundle(
                  *snapshot_copy, reason_codes::kCollisionValidationSerialFallbackException,
                  reason_codes::kUnknownNonStdException, reason_codes::kUnknownNonStdException);
            }
            out.wall_ns = steady_now_ns() - worker_t0;
            if (out.bundle.cancelled) {
              counters->cancelled_jobs_total.fetch_add(1, std::memory_order_relaxed);
            }
            {
              std::lock_guard<std::mutex> lock(state->mutex);
              state->outputs[0] = std::move(out);
            }
          };
          counters_->queued_jobs.fetch_add(1, std::memory_order_relaxed);
        }
        slot->cv.notify_one();
      }
      bool completed = false;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock,
                       [&state]() { return state->pending_jobs == 0 || state->deadline_expired; });
        completed = state->pending_jobs == 0 && !state->deadline_expired;
      }
      if (completed) {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto& output = state->outputs[0];
        serial_worker_wall_ns = output.wall_ns;
        serial_waypoints_classified = static_cast<int>(output.bundle.waypoint_records.size());
        bundle = std::move(output.bundle);
      } else {
        wall_timed_out = true;
        abandon_worker_threads();
        const auto elapsed = std::max<std::int64_t>(0, config_.clock->now() - deadline_started_at);
        bundle = timeout_bundle(
            snapshot,
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_timeout)
                .count(),
            elapsed);
      }
    }
    const auto t1 = steady_now_ns();
    last_telemetry_.workers_actually_used = 1;
    last_telemetry_.workers_requested = worker_count_requested;
    if (fallback_reason == "single_context_duplicate_engine" ||
        fallback_reason == "single_context_missing_mutable_context_id") {
      last_telemetry_.workers_effective = 1;
    } else if (fallback_reason == "insufficient_worker_contexts") {
      last_telemetry_.workers_effective = worker_count_available;
    } else {
      last_telemetry_.workers_effective = worker_count_requested;
    }
    const std::string telemetry_reason =
        wall_timed_out ? "validation_wall_timeout" : fallback_reason;
    last_telemetry_.fallback_reason = telemetry_reason;
    last_telemetry_.total_wall_ns = t1 - t0;
    last_telemetry_.total_worker_wall_ns = serial_worker_wall_ns;
    last_telemetry_.per_worker_waypoint_counts = {serial_waypoints_classified};
    last_telemetry_.support_contact_active = support_contact_present;
    last_telemetry_.contact_release_active = contact_release_present;
    last_telemetry_.ordered_contact_summary_used = support_contact_present;
    last_telemetry_.collision_geometry_count = snapshot.geometry_count;
    last_telemetry_.validation_wall_time_p95_ns = 0;
    last_telemetry_.serial_reference_wall_ns.reset();

    FallbackLogEvent event;
    event.cause = telemetry_reason;
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
    last_telemetry_.workers_requested = worker_count_requested;
    last_telemetry_.workers_effective = 0;
    last_telemetry_.support_contact_active = support_contact_present;
    last_telemetry_.contact_release_active = contact_release_present;
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

  const int configured_workers = std::max(1, config_.worker_count);

  if (configured_workers <= 1) {
    return run_engine_serial_fallback(
        available_engines[0],
        skipped_duplicate_single_context_engine
            ? "single_context_duplicate_engine"
            : "worker_count_one");
  }
  if (missing_single_context_identity) {
    return run_engine_serial_fallback(available_engines[0],
                                      "single_context_missing_mutable_context_id");
  }
  if (skipped_duplicate_single_context_engine) {
    return run_engine_serial_fallback(available_engines[0], "single_context_duplicate_engine");
  }
  if (static_cast<int>(available_engines.size()) < configured_workers) {
    return run_engine_serial_fallback(available_engines[0], "insufficient_worker_contexts");
  }
  if (waypoint_count < std::max(2, config_.min_waypoints_for_parallel)) {
    return run_engine_serial_fallback(available_engines[0],
                                      "trajectory_too_short");
  }
  if (contact_process_present) {
    return run_engine_serial_fallback(available_engines[0], "contact_process_policy_active");
  }
  if (support_contact_present && !config_.enable_ordered_support_contact_summaries) {
    return run_engine_serial_fallback(available_engines[0],
                                      "support_contact_policy_active");
  }
  if (effective_policy.support_contacts.size() > kMaxOrderedSupportContactIdentities) {
    return run_engine_serial_fallback(available_engines[0],
                                      "support_contact_identity_bound_exceeded");
  }
  if (!support_contact_identities_are_unique(effective_policy)) {
    return run_engine_serial_fallback(available_engines[0], "duplicate_support_contact_identity");
  }
  if (contact_grasp_present) {
    return run_engine_serial_fallback(available_engines[0],
                                      "contact_grasp_policy_active");
  }

  const int active_workers = std::min(configured_workers, waypoint_count);
  const int per_worker = waypoint_count / active_workers;
  const int leftover = waypoint_count % active_workers;
  std::vector<std::pair<int, int>> ranges;
  int cursor = 0;
  for (int w = 0; w < active_workers; ++w) {
    const int len = per_worker + (w < leftover ? 1 : 0);
    ranges.emplace_back(cursor, cursor + len);
    cursor += len;
  }

  auto snapshot_shared = snapshot_owner ? std::move(snapshot_owner)
                                        : std::make_shared<const CollisionWorldSnapshot>(snapshot);
  auto trajectory_shared = std::make_shared<TrajectoryDTO>(trajectory);
  auto pass_config_shared =
      std::make_shared<CollisionValidationPassConfig>(pass_config);
  const bool ordered_contact_path = support_contact_present;

  auto run_worker = [snapshot_shared, trajectory_shared, pass_config_shared, cancel,
                     ordered_contact_path](int worker_index, EngineRef engine_ref, int range_begin,
                                           int range_end) -> WorkerOut {
    WorkerOut out;
    out.worker_index = worker_index;
    out.range_begin = range_begin;
    const auto t0 = steady_now_ns();
    try {
      CollisionValidationPassConfig per_worker_cfg = *pass_config_shared;
      CollisionValidationCoordinator coord(std::move(per_worker_cfg));
      if (ordered_contact_path) {
        out.bundle = coord.query_trajectory_range_unclassified(
            *snapshot_shared, *engine_ref.ptr, *trajectory_shared,
            static_cast<std::size_t>(range_begin), static_cast<std::size_t>(range_end), *cancel);
        out.ordered_contact_summary = summarize_ordered_contact_chunk(
            pass_config_shared->effective_policy_snapshot(), out.bundle.waypoint_records,
            static_cast<std::size_t>(range_begin), static_cast<std::size_t>(range_end));
      } else {
        out.bundle = coord.validate_trajectory_range(
            *snapshot_shared, *engine_ref.ptr, *trajectory_shared,
            static_cast<std::size_t>(range_begin), static_cast<std::size_t>(range_end), *cancel);
      }
    } catch (const std::exception& e) {
      out.worker_exception = true;
      out.bundle =
          exception_bundle(*snapshot_shared,
                           reason_codes::kCollisionValidationWorkerException,
                           "std_exception",
                           e.what());
    } catch (...) {
      out.worker_exception = true;
      out.bundle =
          exception_bundle(*snapshot_shared,
                           reason_codes::kCollisionValidationWorkerException,
                           reason_codes::kUnknownNonStdException,
                           reason_codes::kUnknownNonStdException);
    }
    out.wall_ns = steady_now_ns() - t0;
    return out;
  };

  const auto pool_t0 = steady_now_ns();
  auto call_state = std::make_shared<ValidationCallState>();
  const auto counters = counters_;
  call_state->pending_jobs = active_workers;
  call_state->outputs.resize(static_cast<std::size_t>(active_workers));
  ensure_worker_threads(active_workers);
  {
    std::lock_guard<std::mutex> slots_lock(worker_slots_mutex_);
    for (int w = 0; w < active_workers; ++w) {
      auto slot = worker_slots_[static_cast<std::size_t>(w)];
      const auto range_begin = ranges[static_cast<std::size_t>(w)].first;
      const auto range_end = ranges[static_cast<std::size_t>(w)].second;
      auto engine_ref = available_engines[static_cast<std::size_t>(w)];
      {
        std::lock_guard<std::mutex> slot_lock(slot->mutex);
        slot->job = [call_state, run_worker, counters, w, engine_ref, range_begin,
                     range_end]() mutable {
          PendingJobCompletion completion(call_state);
          auto out = run_worker(w, engine_ref, range_begin, range_end);
          if (out.bundle.cancelled) {
            counters->cancelled_jobs_total.fetch_add(1, std::memory_order_relaxed);
          }
          {
            std::lock_guard<std::mutex> lock(call_state->mutex);
            call_state->outputs[static_cast<std::size_t>(w)] =
                std::move(out);
          }
        };
        counters_->queued_jobs.fetch_add(1, std::memory_order_relaxed);
      }
      slot->cv.notify_one();
    }
  }
  std::unique_ptr<ValidationDeadlineAlarm> phase_one_deadline;
  if (config_.validation_wall_timeout > std::chrono::nanoseconds::zero()) {
    phase_one_deadline = std::make_unique<ValidationDeadlineAlarm>(
        *config_.clock, *config_.scheduler, wall_deadline, call_state, cancel);
  }
  bool completed = false;
  {
    std::unique_lock<std::mutex> lock(call_state->mutex);
    if (config_.validation_wall_timeout > std::chrono::nanoseconds::zero()) {
      call_state->cv.wait(lock, [&call_state]() {
        return call_state->pending_jobs == 0 || call_state->deadline_expired;
      });
      completed = call_state->pending_jobs == 0 && !call_state->deadline_expired;
    } else {
      call_state->cv.wait(
          lock, [&call_state]() { return call_state->pending_jobs == 0; });
      completed = true;
    }
  }
  phase_one_deadline.reset();
  auto pool_t1 = steady_now_ns();
  if (!completed) {
    abandon_worker_threads();
    last_telemetry_.workers_actually_used = active_workers;
    last_telemetry_.workers_requested = worker_count_requested;
    last_telemetry_.workers_effective = configured_workers;
    last_telemetry_.fallback_reason = "validation_wall_timeout";
    last_telemetry_.total_wall_ns = pool_t1 - pool_t0;
    last_telemetry_.total_worker_wall_ns = 0;
    last_telemetry_.per_worker_waypoint_counts.assign(static_cast<std::size_t>(active_workers), 0);
    last_telemetry_.support_contact_active = support_contact_present;
    last_telemetry_.contact_release_active = contact_release_present;
    last_telemetry_.ordered_contact_summary_used = ordered_contact_path;
    last_telemetry_.collision_geometry_count = snapshot.geometry_count;
    last_telemetry_.validation_wall_time_p95_ns = 0;
    last_telemetry_.serial_reference_wall_ns.reset();
    const auto elapsed_ns = std::max<std::int64_t>(0, config_.clock->now() - deadline_started_at);
    return timeout_bundle(snapshot, effective_wall_timeout_ns, elapsed_ns);
  }
  std::vector<WorkerOut> outputs;
  {
    std::lock_guard<std::mutex> lock(call_state->mutex);
    outputs = std::move(call_state->outputs);
  }

  std::sort(outputs.begin(), outputs.end(),
            [](const WorkerOut& a, const WorkerOut& b) { return a.worker_index < b.worker_index; });

  const bool phase_one_worker_exception = std::any_of(
      outputs.begin(), outputs.end(), [](const WorkerOut& out) { return out.worker_exception; });
  std::vector<OrderedContactChunkSummary> ordered_contact_summaries;
  if (ordered_contact_path && !phase_one_worker_exception) {
    ordered_contact_summaries.reserve(outputs.size());
    std::vector<SupportContactEvaluationStateMap> entering_states(outputs.size());
    std::vector<SupportContactEvaluationStateMap> expected_exit_states(outputs.size());
    auto prefix_state = pass_config.entering_support_contact_state.value_or(
        initial_support_contact_state(effective_policy));
    std::string summary_error;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      entering_states[i] = prefix_state;
      ordered_contact_summaries.push_back(outputs[i].ordered_contact_summary);
      if (!advance_support_contact_state(outputs[i].ordered_contact_summary, prefix_state,
                                         &summary_error)) {
        return exception_bundle(snapshot, "ordered_contact_summary_invalid", "summary_error",
                                summary_error.c_str());
      }
      expected_exit_states[i] = prefix_state;
    }

    auto phase_outputs = std::make_shared<std::vector<WorkerOut>>(std::move(outputs));
    auto finalize_state = std::make_shared<ValidationCallState>();
    finalize_state->pending_jobs = active_workers;
    {
      std::lock_guard<std::mutex> slots_lock(worker_slots_mutex_);
      for (int w = 0; w < active_workers; ++w) {
        auto slot = worker_slots_[static_cast<std::size_t>(w)];
        {
          std::lock_guard<std::mutex> slot_lock(slot->mutex);
          slot->job =
              [phase_outputs, finalize_state, pass_config_shared, snapshot_shared,
               trajectory_shared, entering_state = entering_states[static_cast<std::size_t>(w)],
               expected_exit = expected_exit_states[static_cast<std::size_t>(w)], w]() mutable {
                PendingJobCompletion completion(finalize_state);
                auto& out = (*phase_outputs)[static_cast<std::size_t>(w)];
                const auto finalize_t0 = steady_now_ns();
                try {
                  CollisionValidationCoordinator coordinator(*pass_config_shared);
                  out.bundle = coordinator.finalize_prequeried_range(
                      *snapshot_shared, *trajectory_shared, std::move(out.bundle), entering_state,
                      &out.exiting_support_state);
                  if (out.exiting_support_state != expected_exit) {
                    out.worker_exception = true;
                    out.bundle = exception_bundle(
                        *snapshot_shared, "ordered_contact_summary_state_mismatch", "summary_error",
                        "worker final state differed from bounded summary");
                  }
                } catch (const std::exception& e) {
                  out.worker_exception = true;
                  out.bundle = exception_bundle(*snapshot_shared,
                                                "ordered_contact_policy_finalize_exception",
                                                "std_exception", e.what());
                } catch (...) {
                  out.worker_exception = true;
                  out.bundle = exception_bundle(
                      *snapshot_shared, "ordered_contact_policy_finalize_exception",
                      reason_codes::kUnknownNonStdException, reason_codes::kUnknownNonStdException);
                }
                out.wall_ns += steady_now_ns() - finalize_t0;
              };
          counters_->queued_jobs.fetch_add(1, std::memory_order_relaxed);
        }
        slot->cv.notify_one();
      }
    }

    std::unique_ptr<ValidationDeadlineAlarm> phase_two_deadline;
    if (config_.validation_wall_timeout > std::chrono::nanoseconds::zero()) {
      phase_two_deadline = std::make_unique<ValidationDeadlineAlarm>(
          *config_.clock, *config_.scheduler, wall_deadline, finalize_state, cancel);
    }
    bool finalized = false;
    {
      std::unique_lock<std::mutex> lock(finalize_state->mutex);
      if (config_.validation_wall_timeout > std::chrono::nanoseconds::zero()) {
        finalize_state->cv.wait(lock, [&finalize_state]() {
          return finalize_state->pending_jobs == 0 || finalize_state->deadline_expired;
        });
        finalized = finalize_state->pending_jobs == 0 && !finalize_state->deadline_expired;
      } else {
        finalize_state->cv.wait(lock,
                                [&finalize_state]() { return finalize_state->pending_jobs == 0; });
        finalized = true;
      }
    }
    phase_two_deadline.reset();
    pool_t1 = steady_now_ns();
    if (!finalized) {
      abandon_worker_threads();
      const auto timeout_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(config_.validation_wall_timeout)
              .count();
      last_telemetry_.workers_actually_used = active_workers;
      last_telemetry_.workers_requested = worker_count_requested;
      last_telemetry_.workers_effective = configured_workers;
      last_telemetry_.fallback_reason = "validation_wall_timeout";
      last_telemetry_.total_wall_ns = pool_t1 - pool_t0;
      last_telemetry_.support_contact_active = true;
      last_telemetry_.contact_release_active = contact_release_present;
      last_telemetry_.ordered_contact_summary_used = true;
      last_telemetry_.collision_geometry_count = snapshot.geometry_count;
      const auto elapsed_ns = std::max<std::int64_t>(0, config_.clock->now() - deadline_started_at);
      return timeout_bundle(snapshot, timeout_ns, elapsed_ns);
    }
    outputs = std::move(*phase_outputs);
  }

  CollisionValidationDecisionBundle merged;
  merged.borrowed_world_revision = snapshot.revision;
  merged.final_world_revision = snapshot.revision;

  bool revision_mismatch = false;
  bool ordered_worker_exception = false;
  for (const auto& out : outputs) {
    ordered_worker_exception = ordered_worker_exception || out.worker_exception;
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
  if (ordered_contact_path && !cancelled && !ordered_worker_exception) {
    std::string summary_error;
    const auto release_violations = merge_contact_release_summaries(
        effective_policy, ordered_contact_summaries, trajectory.waypoints.size(), &summary_error,
        pass_config.entering_contact_release_state ? &*pass_config.entering_contact_release_state
                                                   : nullptr,
        pass_config.enforce_contact_release_trajectory_end, nullptr);
    if (summary_error.empty()) {
      (void)apply_contact_release_violations(effective_policy, release_violations, merged,
                                             &summary_error);
    }
    if (!summary_error.empty()) {
      merged.decision.accepted = false;
      merged.decision.severity = CollisionDecisionSeverity::kBlock;
      merged.decision.action = CollisionDecisionAction::kInconclusive;
      if (!merged.waypoint_records.empty()) {
        auto& first = merged.waypoint_records.front();
        first.result.decision = CollisionDecisionResult::kInconclusiveFail;
        first.produced_decision_change = true;
        first.result.raw_diagnostics["ordered_contact_summary_error"] = summary_error;
        merged.decision.first_failing_waypoint = first.waypoint_index;
        merged.decision.first_failing_time_ns = first.time_from_start_ns;
      }
    }
    first_failing_wp = merged.decision.first_failing_waypoint;
    first_failing_time_ns = merged.decision.first_failing_time_ns;
    worst_sev = merged.decision.severity;
    worst = result_for_action(merged.decision.action);
  }
  if (!cancelled && cancel->is_cancel_requested()) {
    cancelled = true;
    const auto late_cancel_index =
        trajectory.waypoints.empty()
            ? std::int64_t{0}
            : static_cast<std::int64_t>(trajectory.waypoints.size() - 1);
    cancelled_at_waypoint_index = late_cancel_index;
    cancelled_at_time_ns = waypoint_time_ns(late_cancel_index);
    cancellation_reason = cancel->reason();
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
    if (cancelled_at_waypoint_index) {
      merged.waypoint_records.erase(
          std::remove_if(merged.waypoint_records.begin(), merged.waypoint_records.end(),
                         [&](const CollisionValidationWaypointRecord& record) {
                           return record.waypoint_index > *cancelled_at_waypoint_index;
                         }),
          merged.waypoint_records.end());
    }
  }
  if (revision_mismatch) {
    merged.scene_changed_during_validation = true;
    merged.decision.accepted = false;
    merged.decision.severity = CollisionDecisionSeverity::kBlock;
    merged.decision.action = CollisionDecisionAction::kInconclusive;
  }

  last_telemetry_.workers_actually_used = active_workers;
  last_telemetry_.workers_requested = worker_count_requested;
  last_telemetry_.workers_effective = configured_workers;
  last_telemetry_.fallback_reason.clear();
  last_telemetry_.total_wall_ns = pool_t1 - pool_t0;
  last_telemetry_.total_worker_wall_ns = 0;
  last_telemetry_.per_worker_waypoint_counts.clear();
  last_telemetry_.support_contact_active = support_contact_present;
  last_telemetry_.contact_release_active = contact_release_present;
  last_telemetry_.ordered_contact_summary_used = ordered_contact_path;
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
      (void)ref_coord.validate_trajectory(snapshot,
                                          *available_engines[0].ptr,
                                          trajectory,
                                          *cancel);
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

int resolve_validation_collision_workers(int configured_value, bool deployment_declared,
                                         int max_workers) {
  const int upper_bound = std::max(1, max_workers);
  if (deployment_declared) {
    if (configured_value < 1 || configured_value > upper_bound) {
      throw std::invalid_argument("declared validation collision worker count is outside capacity");
    }
    return configured_value;
  }
  return env_validation_collision_workers(configured_value, upper_bound);
}

}  // namespace manyforge::core
