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
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Pose-source borrow / retry policy.
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
  /// Active collision policy snapshot for simple/legacy call sites.
  /// Production hot paths should prefer `policy_snapshot_ref` +
  /// `policy_index` to share immutable policy data across workers.
  CollisionPolicySnapshot policy_snapshot;

  /// Optional shared immutable policy snapshot and prebuilt index. When set,
  /// coordinators use these instead of copying `policy_snapshot`. The value
  /// member remains for simple tests and legacy call sites.
  std::shared_ptr<const CollisionPolicySnapshot> policy_snapshot_ref;
  std::shared_ptr<const CollisionPolicyIndex> policy_index;

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

  /// Concrete per-request contact policy. Empty means free-space validation.
  std::string contact_policy_id;

  /// Clearance margins carried by every query of this pass. Null (runtime
  /// lookahead, monitor) means margin 0 for every pair.
  std::shared_ptr<const CollisionClearanceMargins> clearance_margins;

  /// Optional plan-global sample fractions for a bounded runtime-lookahead
  /// prefix. Contact-grasp terminal windows are properties of the accepted
  /// expanded trajectory; a truncated prefix must never renormalize itself to
  /// [0, 1] and admit contact early. When present, this vector must correspond
  /// one-to-one with the pass trajectory and be strictly increasing in [0, 1].
  std::optional<std::vector<double>> original_trajectory_sample_fractions;

  /// Optional exact prefix state for runtime-lookahead passes. Ordinary
  /// pre-dispatch validation leaves these empty and starts at trajectory
  /// waypoint zero. Supplying one without the other for a support-contact
  /// policy is invalid and fails closed in the ordered merger.
  std::optional<SupportContactEvaluationStateMap> entering_support_contact_state;
  std::optional<ContactReleaseEvaluationStateMap> entering_contact_release_state;

  /// End-of-trajectory release obligations apply only when the selected pass
  /// reaches the accepted trajectory end. Runtime lookahead clears this for a
  /// time/waypoint-truncated prefix while retaining recontact/depth checks.
  bool enforce_contact_release_trajectory_end = true;

  const CollisionPolicySnapshot& effective_policy_snapshot() const {
    return policy_snapshot_ref ? *policy_snapshot_ref : policy_snapshot;
  }
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

/// CollisionValidationCoordinator.
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
/// ParallelCollisionValidationWorkerPool, which runs multiple
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
  /// does not depend on planner scenes, planner-scene mutexes, or
  /// planner-owned mutable state. The snapshot revision is borrowed once and
  /// remains both the initial and final revision for the pass because the
  /// snapshot is immutable.
  CollisionValidationDecisionBundle validate_trajectory(
      const CollisionWorldSnapshot& snapshot,
      ICollisionQueryEngine& engine,
      const TrajectoryDTO& trajectory,
      const CollisionCancellationToken& cancel);

  /// Validate a contiguous waypoint range against `trajectory` without
  /// copying the waypoint chunk. Waypoint indices in the returned records are
  /// local to the range, matching the chunk semantics used by
  /// ParallelCollisionValidationWorkerPool before it applies global offsets.
  CollisionValidationDecisionBundle validate_trajectory_range(
      const CollisionWorldSnapshot& snapshot,
      ICollisionQueryEngine& engine,
      const TrajectoryDTO& trajectory,
      std::size_t begin_index,
      std::size_t end_index,
      const CollisionCancellationToken& cancel);

  /// Query one contiguous range without applying collision policy. This is the
  /// first phase of ordered stateful parallel validation: workers perform every
  /// expensive engine query exactly once, then publish bounded prefix
  /// summaries before policy finalization. Returned waypoint indices are local
  /// to the range.
  CollisionValidationDecisionBundle query_trajectory_range_unclassified(
      const CollisionWorldSnapshot& snapshot, ICollisionQueryEngine& engine,
      const TrajectoryDTO& trajectory, std::size_t begin_index, std::size_t end_index,
      const CollisionCancellationToken& cancel);

  /// Apply policy classification to an already-queried range using the exact
  /// support-contact state entering that chunk. This never calls the collision
  /// engine. It reclassifies the already-owned worker-local records so the
  /// complete diagnostic stream remains serial-identical; this policy-only
  /// work stays parallel. Contact-release end constraints are left to the
  /// bounded ordered-summary merger, and the global coordinator never replays
  /// trajectory records.
  CollisionValidationDecisionBundle finalize_prequeried_range(
      const CollisionWorldSnapshot& snapshot, const TrajectoryDTO& trajectory,
      CollisionValidationDecisionBundle queried,
      const SupportContactEvaluationStateMap& entering_support_state,
      SupportContactEvaluationStateMap* exiting_support_state);

  /// Snapshot view of the active policy.
  const CollisionPolicySnapshot& policy_snapshot() const {
    return *policy_snapshot_;
  }

 private:
  CollisionValidationDecisionBundle run_pass_once(
      const CollisionWorldSnapshot& snapshot,
      ICollisionQueryEngine& engine,
      const TrajectoryDTO& trajectory,
      std::size_t begin_index,
      std::size_t end_index,
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

  void prepare_distance_selection(const CollisionWorldSnapshot& snapshot);

  void apply_contact_release_constraints(
      const TrajectoryDTO& trajectory,
      CollisionValidationDecisionBundle& bundle,
      CollisionDecisionResult& worst_decision,
      CollisionDecisionSeverity& worst_severity,
      std::optional<std::int64_t>& first_failing_waypoint,
      std::optional<std::int64_t>& first_failing_time_ns) const;

  void apply_contact_grasp_constraints(
      const TrajectoryDTO& trajectory,
      CollisionValidationDecisionBundle& bundle) const;

  /// Merge an engine-level CollisionDecisionResult with a policy-level one.
  /// kInconclusiveFail / kStopRequested cannot be downgraded.
  static CollisionDecisionResult merge_engine_and_policy_decision(
      CollisionDecisionResult engine_decision,
      CollisionDecisionResult policy_decision);

  CollisionValidationPassConfig config_;
  std::shared_ptr<const CollisionPolicySnapshot> policy_snapshot_;
  std::shared_ptr<const CollisionPolicyIndex> policy_index_;
  CollisionPolicyEvaluator evaluator_;
  ClockNow clock_now_;
  std::string policy_revision_string_;
  std::vector<std::string> required_capabilities_;
  CollisionDistanceRequestMode distance_request_mode_ =
      CollisionDistanceRequestMode::kNone;
  std::vector<CollisionPairId> selected_distance_pairs_;
};

}  // namespace manyforge::core
