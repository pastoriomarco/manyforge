#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace manyforge::core {

/// Telemetry payload produced by one parallel-validation run. The kernel /
/// observer surface can copy this into `manyforge.validation.collision_parallel_*`
/// events.
///
/// All fields are populated unconditionally except `serial_reference_wall_ns`,
/// which is opt-in (qualification / shadow runs only, see
/// `Config::serial_reference_mode`). Hot-path runs MUST NOT pay the cost of
/// the serial reference; the field stays `std::nullopt`.
struct ParallelValidationTelemetry {
  /// Deployment-requested and admitted worker counts. They are equal on the
  /// production path; a mismatch is observable and cannot silently weaken a
  /// declared deployment.
  int workers_requested = 1;
  int workers_effective = 1;

  /// Number of workers that actually executed validation jobs. 1 means the
  /// pool took the serial path; values above 1 mean parallel work ran.
  int workers_actually_used = 1;

  /// Reason the pool fell back to serial. Empty when parallel ran. Values are
  /// stable structured reason codes documented by the parallelism plan.
  std::string fallback_reason;

  /// Total wall time of the call (ns).
  std::int64_t total_wall_ns = 0;

  /// Summed wall time of each worker, useful for parallelism factor calculations.
  std::int64_t total_worker_wall_ns = 0;

  /// How many waypoints each worker actually classified, in original
  /// trajectory order (worker 0 first).
  std::vector<int> per_worker_waypoint_counts;

  /// True when the active policy snapshot declared one or more
  /// `support_contacts` entries, regardless of whether that caused a
  /// serial fallback. Useful for distinguishing "support_contact present
  /// but pool ran parallel because the refusal flag is off" from "no
  /// support_contact at all" in dashboards.
  bool support_contact_active = false;

  bool contact_release_active = false;
  bool ordered_contact_summary_used = false;

  /// Collision geometry count sourced from `CollisionWorldSnapshot`
  /// metadata when available; 0 means "not sourced" — callers MUST NOT
  /// interpret 0 as "no geometry". This field
  /// MUST be populated from snapshot metadata only; the pool MUST NOT
  /// scan the scene to compute it (per the telemetry-overhead avoidance
  /// rule).
  std::int64_t collision_geometry_count = 0;

  /// p95 of the per-worker wall times for this single validation call,
  /// in ns. Computed over `per_worker_waypoint_counts.size()` samples.
  /// 0 when fewer than two workers actually ran (single-sample p95 is
  /// undefined). This is per-call, not a cross-call rolling stat; the
  /// kernel observer is the layer that aggregates over time.
  std::int64_t validation_wall_time_p95_ns = 0;

  /// Serial reference wall time, populated ONLY when
  /// `Config::serial_reference_mode != kDisabled` and the pool ran
  /// parallel for this call. The serial reference is run *after* the
  /// parallel decision is produced; it is for measurement, not
  /// authority. Production / hot-path runs MUST leave this unset.
  std::optional<std::int64_t> serial_reference_wall_ns;
};

/// Structured fallback-log event emitted by the pool when it takes a
/// serial fallback. Sinks are responsible for rendering to whichever
/// logging surface they prefer (rosout, structured-log file, telemetry
/// event bus). The pool internally rate-limits the sink to at most one
/// emit per cause per window (see `Config::fallback_log_window`).
struct FallbackLogEvent {
  std::string cause;                /// canonical reason (matches `fallback_reason`)
  int worker_count_requested = 0;
  int worker_count_available = 0;
  int waypoint_count = 0;
  bool support_contact_active = false;
};

/// Lock-free diagnostic snapshot for the persistent validation worker pool.
/// Counters are monotonic for one pool lifetime. Queue depth and active workers
/// are instantaneous and may change immediately after this value is returned.
struct ParallelValidationPoolStatus {
  std::uint64_t queued_jobs = 0;
  std::uint64_t active_workers = 0;
  std::uint64_t completed_jobs_total = 0;
  std::uint64_t cancelled_jobs_total = 0;
  std::uint64_t dropped_jobs_total = 0;
  std::uint64_t abandoned_jobs_total = 0;
};

/// ParallelCollisionValidationWorkerPool.
///
/// Splits a trajectory into contiguous waypoint chunks, runs each chunk on a
/// dedicated worker with its own `ICollisionQueryEngine` against a shared
/// immutable `CollisionWorldSnapshot`, and merges the per-chunk results
/// deterministically by lowest failing waypoint index.
///
/// Workers always see waypoints in their original trajectory order within
/// each chunk; the merge is order-invariant w.r.t. worker completion time. This
/// preserves the determinism contract from the parallelism plan.
///
/// Falls back to serial validation when:
///   - a stateful policy is not represented by the qualified bounded summary;
///   - worker_count <= 1;
///   - the trajectory is shorter than `min_waypoints_for_parallel`;
///   - the caller did not provide enough independent worker engines to back
///     the requested worker count;
///   - engine mutable-context identity is absent or duplicated;
///   - the stateful policy exceeds its admitted identity bound.
///
/// The pool owns persistent worker threads and fallback-log rate limiter state.
/// Reuse one instance per validation call site to avoid per-call thread spawn
/// overhead. Calls on one pool are serialized internally; the parallelism is
/// inside one validation call, across worker contexts.
class ParallelCollisionValidationWorkerPool {
 public:
  /// Whether the pool should also run a serial-reference pass for
  /// measurement. The serial reference is run AFTER the parallel
  /// decision; it never becomes the authoritative result. Production /
  /// hot paths MUST use `kDisabled`. Qualification, shadow and
  /// commissioning paths MAY enable `kQualification` to populate
  /// `ParallelValidationTelemetry::serial_reference_wall_ns`.
  enum class SerialReferenceMode {
    kDisabled,
    kQualification,
  };

  struct Config {
    /// Pass-scoped configuration shared by every worker's coordinator.
    CollisionValidationPassConfig pass_config;

    /// Deployment-admitted worker capacity. Production startup validates this
    /// value and supplies exactly one independent query context per worker.
    int worker_count = 1;

    /// Minimum trajectory length to bother parallelizing. Defaults to 8 to
    /// avoid sub-millisecond contention overheads.
    int min_waypoints_for_parallel = 8;

    /// Enable the qualified two-phase ordered-summary path for support-contact
    /// and contact-release validation. Every collision query remains
    /// sequential inside one worker chunk; only bounded summaries cross back
    /// to the coordinator. Disable only for qualification/debug comparison.
    bool enable_ordered_support_contact_summaries = true;

    /// Optional sink for structured fallback-log events. When set, the
    /// pool calls the sink at most once per cause per `fallback_log_window`.
    /// The sink MUST be thread-safe; the pool calls it from the calling
    /// thread of `validate_trajectory()` only.
    std::function<void(const FallbackLogEvent&)> fallback_log_sink;

    /// Minimum time between successive fallback-log emits for the same
    /// cause. Default 60s prevents hot-path noise.
    std::chrono::nanoseconds fallback_log_window =
        std::chrono::seconds{60};

    /// Serial-reference measurement mode (see `SerialReferenceMode`).
    /// Default `kDisabled`.
    SerialReferenceMode serial_reference_mode = SerialReferenceMode::kDisabled;

    /// Collision geometry count for telemetry, sourced from
    /// `CollisionWorldSnapshot` metadata by the caller. Pool does not
    /// scan the scene; 0 means "not sourced".
    std::int64_t collision_geometry_count = 0;

    /// Wall-clock liveness budget for one validation call. Zero preserves the
    /// historical cooperative-only behavior. Positive values make the pool
    /// request cancellation and return a structured fail-closed timeout when
    /// workers do not finish in time. Retired workers retain their contexts;
    /// later calls fail closed until every retired worker finishes and is
    /// joined, so a timed-out mutable context is never reused concurrently.
    std::chrono::nanoseconds validation_wall_timeout =
        std::chrono::nanoseconds::zero();

    /// Deadline authority for `validation_wall_timeout`. Production kernel
    /// wiring supplies its injected clock/scheduler pair; deterministic tests
    /// use FakeClock/ManualScheduler. Both are mandatory when the timeout is
    /// positive so core never invents a second wall-clock authority. The
    /// timeout callback runs on the Monitoring watchdog lane, never on
    /// ControllerCommand, RuntimeSafety, or StopControl.
    IClock* clock = nullptr;
    IScheduler* scheduler = nullptr;
  };

  explicit ParallelCollisionValidationWorkerPool(Config config);
  ~ParallelCollisionValidationWorkerPool();

  // Close admission and join all workers before the scheduler is released.
  void detach();

  ParallelCollisionValidationWorkerPool(
      const ParallelCollisionValidationWorkerPool&) = delete;
  ParallelCollisionValidationWorkerPool& operator=(
      const ParallelCollisionValidationWorkerPool&) = delete;
  ParallelCollisionValidationWorkerPool(
      ParallelCollisionValidationWorkerPool&&) = delete;
  ParallelCollisionValidationWorkerPool& operator=(
      ParallelCollisionValidationWorkerPool&&) = delete;

  /// Production engine-backed validation path. `worker_engines` must contain
  /// one independent engine per worker for engines that report
  /// kSingleContextExclusive. Null entries are ignored. The pool routes every
  /// waypoint through ICollisionQueryEngine::check(snapshot, query, policy,
  /// cancel) through the coordinator's engine-native path; this overload never
  /// adapts engines into a combined planner facade and never touches
  /// planner-owned scene state.
  ///
  /// The snapshot is immutable and shared across all workers. Telemetry
  /// collision geometry count is sourced directly from
  /// `snapshot.geometry_count`; the pool never scans `snapshot.bodies`.
  ///
  /// This borrowed-lifetime overload is only valid for cooperative, unbounded
  /// calls (`Config::validation_wall_timeout == 0`). Positive wall budgets must
  /// use the `shared_ptr` overload below so an abandoned worker cannot outlive
  /// its engine or cancellation token.
  CollisionValidationDecisionBundle validate_trajectory(
      const CollisionWorldSnapshot& snapshot,
      const std::vector<ICollisionQueryEngine*>& worker_engines,
      const TrajectoryDTO& trajectory,
      CollisionCancellationToken& cancel);

  /// Production overload that keeps engine and cancellation-token lifetimes
  /// valid if a validation call times out and a worker has to be abandoned.
  CollisionValidationDecisionBundle validate_trajectory(
      const CollisionWorldSnapshot& snapshot,
      const std::vector<std::shared_ptr<ICollisionQueryEngine>>& worker_engines,
      const TrajectoryDTO& trajectory,
      const std::shared_ptr<CollisionCancellationToken>& cancel);

  /// Same as the primary overload, but with pass-scoped collision policy and
  /// retry settings supplied by the caller for this validation attempt. This
  /// lets long-lived pools keep worker threads while each request still uses
  /// its own immutable policy snapshot/index.
  CollisionValidationDecisionBundle validate_trajectory(
      const CollisionWorldSnapshot& snapshot,
      const std::vector<ICollisionQueryEngine*>& worker_engines,
      const TrajectoryDTO& trajectory,
      CollisionCancellationToken& cancel,
      CollisionValidationPassConfig pass_config);

  CollisionValidationDecisionBundle validate_trajectory(
      const CollisionWorldSnapshot& snapshot,
      const std::vector<std::shared_ptr<ICollisionQueryEngine>>& worker_engines,
      const TrajectoryDTO& trajectory,
      const std::shared_ptr<CollisionCancellationToken>& cancel,
      CollisionValidationPassConfig pass_config);

  /// Ownership-preserving production overload. The immutable snapshot owner is
  /// retained by queued or timed-out jobs, avoiding a deep copy of mesh/octomap
  /// geometry at every validation admission. `absolute_wall_deadline` may only
  /// tighten the configured wall timeout; it lets a caller include bounded
  /// pre-pool work in one end-to-end deadline without creating another clock
  /// authority.
  CollisionValidationDecisionBundle validate_trajectory(
      CollisionWorldSnapshotPtr snapshot,
      const std::vector<std::shared_ptr<ICollisionQueryEngine>>& worker_engines,
      const TrajectoryDTO& trajectory, const std::shared_ptr<CollisionCancellationToken>& cancel,
      CollisionValidationPassConfig pass_config,
      std::optional<TimePoint> absolute_wall_deadline = std::nullopt);

  ParallelValidationTelemetry last_telemetry() const;
  ParallelValidationPoolStatus status() const;

 private:
  struct EngineRef {
    ICollisionQueryEngine* ptr = nullptr;
    std::shared_ptr<ICollisionQueryEngine> owner;
  };
  struct WorkerSlot;
  struct PoolCounters;

  /// Emit a fallback-log event through the sink, rate-limited.
  void maybe_emit_fallback_log(const FallbackLogEvent& event);
  void ensure_worker_threads(int count);
  void stop_worker_threads();
  void abandon_worker_threads();
  bool reap_abandoned_worker_threads_if_drained();
  void release_abandoned_worker_threads();
  static void worker_loop(std::shared_ptr<WorkerSlot> slot);
  CollisionValidationDecisionBundle validate_trajectory_impl(
      const CollisionWorldSnapshot& snapshot, const std::vector<EngineRef>& worker_engines,
      const TrajectoryDTO& trajectory, const std::shared_ptr<CollisionCancellationToken>& cancel,
      CollisionValidationPassConfig pass_config, CollisionWorldSnapshotPtr snapshot_owner = nullptr,
      std::optional<TimePoint> absolute_wall_deadline = std::nullopt);

  Config config_;
  ParallelValidationTelemetry last_telemetry_{};
  std::shared_ptr<PoolCounters> counters_;
  std::unordered_map<std::string, TimePoint> last_fallback_log_at_;
  std::atomic<bool> closing_{false};
  mutable std::mutex validation_call_mutex_;
  std::mutex worker_slots_mutex_;
  std::vector<std::shared_ptr<WorkerSlot>> worker_slots_;
  std::mutex abandoned_worker_slots_mutex_;
  std::vector<std::shared_ptr<WorkerSlot>> abandoned_worker_slots_;
};

/// Read the `MANYFORGE_VALIDATION_COLLISION_WORKERS` env var. Returns the
/// parsed positive integer when set and well-formed; otherwise returns
/// `default_value`. The kernel (or its bootstrap) calls this once at
/// startup and threads the result into the pool's `Config::worker_count`.
///
/// Bounds: the helper clamps the parsed value to `[1, max_workers]` so
/// operators cannot accidentally spin up unbounded thread counts via a
/// runaway env override. Unparseable values fall back to `default_value`.
int env_validation_collision_workers(int default_value, int max_workers = 64);

/// Resolve the startup worker count without allowing a development/CI
/// environment override to replace an explicit production deployment value.
int resolve_validation_collision_workers(int configured_value, bool deployment_declared,
                                         int max_workers = 64);

}  // namespace manyforge::core
