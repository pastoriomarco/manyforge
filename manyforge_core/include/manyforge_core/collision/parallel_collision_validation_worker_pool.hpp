#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Telemetry payload produced by one parallel-validation run. The kernel /
/// observer surface can copy this into `manyforge.validation.collision_parallel_*`
/// events as described by TRAJECTORY_VALIDATION_PARALLELISM_PLAN §"Observability".
struct ParallelValidationTelemetry {
  /// Number of workers that actually executed in parallel. 1 means the pool
  /// took the serial fallback.
  int workers_actually_used = 1;

  /// Reason the pool fell back to serial. Empty when parallel ran. Use the
  /// canonical reasons from the parallelism plan ("support_contact_policy_active",
  /// "worker_count_one", "scene_clone_unavailable", "trajectory_too_short",
  /// "debug_serial_validation_enabled").
  std::string fallback_reason;

  /// Total wall time of the call (ns).
  std::int64_t total_wall_ns = 0;

  /// Summed wall time of each worker, useful for parallelism factor calculations.
  std::int64_t total_worker_wall_ns = 0;

  /// How many waypoints each worker actually classified, in original
  /// trajectory order (worker 0 first).
  std::vector<int> per_worker_waypoint_counts;
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
/// The pool is stateless across calls; instantiate once per call site if you
/// want isolated telemetry, or reuse and read last_telemetry() after each run.
class ParallelCollisionValidationWorkerPool {
 public:
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
  };

  explicit ParallelCollisionValidationWorkerPool(Config config);

  /// Run validation. `worker_contexts` is the caller-provided pool of
  /// independent planning contexts. Element 0 is also used for the serial
  /// fallback. The pool MUST NOT mutate the contexts; threading discipline
  /// is the caller's responsibility (each worker uses exactly one context for
  /// the duration of the call).
  CollisionValidationDecisionBundle validate_trajectory(
      const std::vector<IPlanningContext*>& worker_contexts,
      const TrajectoryDTO& trajectory);

  const ParallelValidationTelemetry& last_telemetry() const {
    return last_telemetry_;
  }

 private:
  CollisionValidationDecisionBundle run_serial_fallback(
      IPlanningContext& context, const TrajectoryDTO& trajectory,
      std::string fallback_reason);

  Config config_;
  ParallelValidationTelemetry last_telemetry_{};
};

}  // namespace manyforge::core
