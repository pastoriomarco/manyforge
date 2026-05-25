#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/collision/collision_policy_evaluator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Pose-source borrow / retry policy (spec §"Pose-Source Apply During
/// Validation", COLLISION_CHECKING_OWNERSHIP_PLAN Chunk 5).
///
/// Validation borrows one immutable collision-world revision at start.
/// Pose-source applies may publish newer revisions while validation runs but
/// they do not mutate the borrowed snapshot. Before dispatch, the coordinator
/// compares the borrowed revision to the active revision. The thresholds here
/// control how aggressively to retry under churn before failing with
/// kSceneChangedDuringValidation.
struct PoseSourceRetryConfig {
  /// Maximum number of fast revalidations after a single observed revision
  /// change. Default 1 — one retry then fail closed.
  int max_count = 1;

  /// Wall-time budget for the retry validation. If the second pass takes
  /// longer than this and the revision is still changing, fail closed with
  /// kSceneChangedDuringValidation rather than livelocking.
  Duration retry_wall_time_ns = 250'000'000;  // 250 ms

  /// Sliding window over which churn is measured. Default 1 s.
  Duration churn_window_ns = 1'000'000'000;

  /// Maximum revisions observed within `churn_window_ns` before declaring the
  /// scene unstable. Three by default.
  int churn_max_revisions = 3;
};

/// Pass-scoped configuration for one CollisionValidationCoordinator
/// invocation. Coordinators are constructed per-pass so support-contact state
/// does not leak across moves.
struct CollisionValidationPassConfig {
  /// Active collision policy snapshot. The coordinator owns the evaluator;
  /// the snapshot is copied into the evaluator at construction.
  CollisionPolicySnapshot policy_snapshot;

  /// Pose-source borrow + retry knobs.
  PoseSourceRetryConfig pose_source_retry;

  /// When true, the coordinator borrows the active world revision at start
  /// and fails closed with kSceneChangedDuringValidation if it differs at
  /// finish (after retry exhaustion). Disabling this disables the pose-source
  /// retry path; the coordinator still classifies waypoints normally.
  bool require_pose_source_borrow = true;

  /// Per-waypoint collision-check timeout budget (ns). Forwarded into the
  /// CollisionQuery. Zero means "no per-state budget enforced".
  std::int64_t per_state_timeout_ns = 0;

  /// Stable identifier for the surface this pass is validating. Echoed into
  /// every CollisionQuery so downstream engines / failure archives can tag
  /// the diagnostic chain. Defaults to kPreExecuteValidation.
  CollisionSurface surface = CollisionSurface::kPreExecutionValidation;
};

/// Per-waypoint diagnostic record produced during a coordinator pass.
/// Populated whether or not the waypoint blocked; the kernel / failure
/// archive may inspect the full sequence after the fact.
struct CollisionValidationWaypointRecord {
  std::int64_t waypoint_index = -1;
  std::optional<std::int64_t> time_from_start_ns;
  CollisionResult result;
  bool produced_decision_change = false;
};

/// Coordinator pass output bundle. The kernel cares about `decision` plus the
/// optional retry / churn metadata. The waypoint records are kept for
/// downstream consumers (failure archive, telemetry).
struct CollisionValidationDecisionBundle {
  CollisionDecision decision;
  CollisionWorldRevision borrowed_world_revision;
  std::optional<CollisionWorldRevision> final_world_revision;
  int pose_source_retries_used = 0;
  bool scene_changed_during_validation = false;
  bool churn_window_exceeded = false;
  bool cancelled = false;
  std::optional<std::string> cancellation_reason;
  std::optional<std::int64_t> cancelled_at_waypoint_index;
  std::vector<CollisionValidationWaypointRecord> waypoint_records;
};

/// CollisionValidationCoordinator (spec/210 §3.8.1,
/// COLLISION_CHECKING_OWNERSHIP_PLAN Chunk 5).
///
/// The coordinator is the authoritative pre-execution validation surface
/// for collision. It:
///
///   - takes an immutable `CollisionWorldSnapshot` reference for the whole
///     pass so concurrent pose-source applies cannot mutate the world
///     under it; the snapshot's revision is the borrowed revision;
///   - owns one CollisionPolicyEvaluator instance with trajectory-scoped
///     support-contact state that evolves across waypoints in trajectory
///     order;
///   - calls `ICollisionQueryEngine::check(snapshot, query, policy, cancel)`
///     per waypoint, so backend choice and engine compare mode remain
///     adapter concerns and the coordinator never reads planner-owned
///     mutable scene state;
///   - merges engine status with the evaluator's classification deterministi-
///     cally, never downgrading kInconclusiveFail / kStopRequested.
///
/// The coordinator is single-threaded by design. Parallelism is added by the
/// ParallelCollisionValidationWorkerPool (Chunk 7), which runs multiple
/// coordinator instances against independent `ICollisionQueryEngine`
/// instances sharing the same snapshot, and merges by lowest failing
/// waypoint index.
class CollisionValidationCoordinator {
 public:
  using ClockNow = std::function<TimePoint()>;

  /// @param config Pass-scoped configuration. Copied into the coordinator.
  /// @param clock_now Optional callable returning a monotonic nanosecond
  ///   timestamp. Defaults to steady_clock. Used only for retry timing.
  explicit CollisionValidationCoordinator(CollisionValidationPassConfig config,
                                          ClockNow clock_now = nullptr);

  /// Validate a full trajectory in waypoint order against a scene-aware
  /// collision-query engine and immutable collision-world snapshot.
  ///
  /// This is the production worker primitive for the manyforge_collision path: it
  /// does not depend on `IPlanningContext`, planner-scene mutexes, or
  /// planner-owned mutable state. The snapshot revision is borrowed once and
  /// remains both the initial and final revision for the pass because the
  /// snapshot is immutable.
  CollisionValidationDecisionBundle validate_trajectory(
      const CollisionWorldSnapshot& snapshot,
      ICollisionQueryEngine& engine,
      const TrajectoryDTO& trajectory,
      const CollisionCancellationToken& cancel);

  /// Snapshot view of the active policy.
  const CollisionPolicySnapshot& policy_snapshot() const {
    return config_.policy_snapshot;
  }

 private:
  CollisionValidationDecisionBundle run_pass_once(
      const CollisionWorldSnapshot& snapshot,
      ICollisionQueryEngine& engine,
      const TrajectoryDTO& trajectory,
      const CollisionCancellationToken& cancel);

  /// Classify a single waypoint and update support-contact state.
  CollisionValidationWaypointRecord classify_one(
      const CollisionWorldSnapshot& snapshot,
      ICollisionQueryEngine& engine,
      const RobotStateDTO& state,
      std::int64_t waypoint_index,
      std::optional<std::int64_t> time_from_start_ns,
      const CollisionWorldRevision& borrowed_revision,
      const CollisionCancellationToken& cancel);

  CollisionQuery make_query(
      const RobotStateDTO& state,
      std::int64_t waypoint_index,
      std::optional<std::int64_t> time_from_start_ns,
      const CollisionWorldRevision& borrowed_revision) const;

  CollisionValidationWaypointRecord classify_result(
      CollisionResult result,
      std::int64_t waypoint_index,
      std::optional<std::int64_t> time_from_start_ns,
      const CollisionWorldRevision& borrowed_revision);

  void apply_contact_release_constraints(
      const TrajectoryDTO& trajectory,
      CollisionValidationDecisionBundle& bundle,
      CollisionDecisionResult& worst_decision,
      CollisionDecisionSeverity& worst_severity,
      std::optional<std::int64_t>& first_failing_waypoint,
      std::optional<std::int64_t>& first_failing_time_ns) const;

  /// Merge an engine-level CollisionDecisionResult with a policy-level one.
  /// kInconclusiveFail / kStopRequested cannot be downgraded.
  static CollisionDecisionResult merge_engine_and_policy_decision(
      CollisionDecisionResult engine_decision,
      CollisionDecisionResult policy_decision);

  CollisionValidationPassConfig config_;
  CollisionPolicyEvaluator evaluator_;
  ClockNow clock_now_;
};

}  // namespace manyforge::core
