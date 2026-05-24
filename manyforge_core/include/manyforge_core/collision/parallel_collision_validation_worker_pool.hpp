#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Telemetry payload produced by one parallel-validation run. The kernel /
/// observer surface can copy this into `manyforge.validation.collision_parallel_*`
/// events as described by TRAJECTORY_VALIDATION_PARALLELISM_PLAN §"Observability".
///
/// All fields are populated unconditionally except `serial_reference_wall_ns`,
/// which is opt-in (qualification / shadow runs only, see
/// `Config::serial_reference_mode`). Hot-path runs MUST NOT pay the cost of
/// the serial reference; the field stays `std::nullopt`.
struct ParallelValidationTelemetry {
  /// Number of workers that actually executed in parallel. 1 means the pool
  /// took the serial fallback.
  int workers_actually_used = 1;

  /// Reason the pool fell back to serial. Empty when parallel ran. Use the
  /// canonical reasons from the parallelism plan. Current worker-pool causes
  /// are "support_contact_policy_active", "worker_count_one",
  /// "trajectory_too_short", and "no_worker_contexts"; scene-aware
  /// engine-specific causes land with the manyforge_collision chunks.
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

  /// Collision geometry count sourced from `CollisionWorldSnapshot`
  /// metadata when available; 0 means "not sourced" (Phase 2 not yet
  /// landed) — callers MUST NOT interpret 0 as "no geometry". This field
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

/// ParallelCollisionValidationWorkerPool (spec COLLISION_CHECKING_OWNERSHIP_PLAN
/// Chunk 7, TRAJECTORY_VALIDATION_PARALLELISM_PLAN Chunks 1-3).
///
/// Splits a trajectory into contiguous waypoint chunks, runs each chunk on a
/// dedicated worker with its own IPlanningContext, and merges the per-chunk
/// results deterministically by lowest failing waypoint index.
///
/// Workers always see waypoints in their original trajectory order within
/// each chunk; the merge is order-invariant w.r.t. worker completion time. This
/// preserves the determinism contract from the parallelism plan.
///
/// Falls back to serial validation when:
///   - the policy snapshot declares any support_contacts (state-machine
///     ordering between chunks not yet specified — Chunk 4 of the parallelism
///     plan);
///   - worker_count <= 1;
///   - the trajectory is shorter than `min_waypoints_for_parallel`;
///   - the caller did not provide enough worker_contexts to back the requested
///     worker count.
///
/// The pool is stateless across calls EXCEPT for the fallback-log rate
/// limiter (a small map of last-emit timestamps per cause). Instantiate
/// once per call site if you want isolated telemetry; reuse and read
/// last_telemetry() after each run. A pool instance is not a concurrent
/// multi-caller object: validation calls on the same instance MUST be
/// serialized by the caller. The parallelism is inside one validation
/// call, across worker contexts.
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

    /// Desired worker count. Pool clamps this to [1, worker_contexts.size()].
    int worker_count = 1;

    /// Minimum trajectory length to bother parallelizing. Defaults to 8 to
    /// avoid sub-millisecond contention overheads.
    int min_waypoints_for_parallel = 8;

    /// When true, the pool will refuse to parallelize trajectories whose
    /// policy snapshot has any support_contacts entries. The plan's Chunk 4
    /// (parallelism) defines the chunk-summary merge semantics that would
    /// remove this restriction; until that is implemented, leave this true.
    bool refuse_parallel_with_support_contacts = true;

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
  };

  explicit ParallelCollisionValidationWorkerPool(Config config);

  /// Run validation. `worker_contexts` is the caller-provided pool of
  /// independent planning contexts. Null entries are ignored; if no
  /// non-null context remains the pool fails closed with
  /// `fallback_reason = "no_worker_contexts"`. The first non-null context
  /// is also used for the serial fallback. The pool MUST NOT mutate the
  /// contexts; threading discipline is the caller's responsibility (each
  /// worker uses exactly one context for the duration of the call).
  CollisionValidationDecisionBundle validate_trajectory(
      const std::vector<IPlanningContext*>& worker_contexts,
      const TrajectoryDTO& trajectory);

  /// Production engine-backed validation path. `worker_engines` must contain
  /// one independent engine per worker for engines that report
  /// kSingleContextExclusive. Null entries are ignored. The pool routes every
  /// waypoint through ICollisionQueryEngine::check(snapshot, query, policy,
  /// cancel) through the coordinator's engine-native path; this overload never
  /// adapts engines into `IPlanningContext` and never touches planner-owned
  /// scene state.
  ///
  /// The snapshot is immutable and shared across all workers. Telemetry
  /// collision geometry count is sourced directly from
  /// `snapshot.geometry_count`; the pool never scans `snapshot.bodies`.
  CollisionValidationDecisionBundle validate_trajectory(
      const CollisionWorldSnapshot& snapshot,
      const std::vector<ICollisionQueryEngine*>& worker_engines,
      const TrajectoryDTO& trajectory,
      const CollisionCancellationToken& cancel);

  const ParallelValidationTelemetry& last_telemetry() const {
    return last_telemetry_;
  }

 private:
  CollisionValidationDecisionBundle run_serial_fallback(
      IPlanningContext& context, const TrajectoryDTO& trajectory,
      std::string fallback_reason,
      int worker_count_requested,
      int worker_count_available,
      bool support_contact_active);

  /// Emit a fallback-log event through the sink, rate-limited.
  void maybe_emit_fallback_log(const FallbackLogEvent& event);

  Config config_;
  ParallelValidationTelemetry last_telemetry_{};
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      last_fallback_log_at_;
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

}  // namespace manyforge::core
