// Chunk 7 unit tests for ParallelCollisionValidationWorkerPool:
// deterministic merge, serial fallback for support-contact / short
// trajectories / worker_count_one, equivalence with serial coordinator on
// fixture trajectories, and telemetry shape.

#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

class MockPlanningContext : public IPlanningContext {
 public:
  std::queue<CollisionResult> normalized_sequence;
  CollisionResult next_normalized;
  CollisionWorldRevision world_revision{1, "mock-world"};
  std::atomic<int> normalized_call_count{0};
  bool throw_normalized = false;

  RobotStateDTO get_current_state() override { return {}; }
  ValidationResultDTO validate_trajectory(const TrajectoryDTO&,
                                          const ValidationOptionsDTO&) override {
    ValidationResultDTO r;
    r.valid = true;
    return r;
  }
  CollisionCheckResultDTO collision_check(
      const CollisionCheckRequestDTO&) override {
    return {};
  }
  Pose3DTO forward_kinematics(const FkRequestDTO&) override { return {}; }
  Pose3DTO get_object_pose(const std::string&, const std::string&) override {
    return {};
  }
  ObjectStateDTO get_object_state(const std::string&) override { return {}; }
  Result<Pose3DTO> resolve_reference_pose(const ReferenceDTO&,
                                          const std::string&) override {
    return Result<Pose3DTO>::Ok(Pose3DTO{});
  }
  JointLimitsDTO get_joint_limits(const std::string&,
                                  const std::string&) override {
    return {};
  }
  SceneEditResultDTO add_object(const AddObjectRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO remove_object(const RemoveObjectRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO update_object_pose(
      const UpdateObjectPoseRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO attach_object(const AttachObjectRequestDTO&) override {
    return {};
  }
  SceneEditResultDTO detach_object(const DetachObjectRequestDTO&) override {
    return {};
  }
  CollisionWorldRevision current_collision_world_revision() override {
    return world_revision;
  }
  CollisionResult collision_check_normalized(const CollisionQuery&) override {
    normalized_call_count.fetch_add(1);
    if (throw_normalized) {
      throw std::runtime_error("collision query failed");
    }
    if (!normalized_sequence.empty()) {
      auto r = std::move(normalized_sequence.front());
      normalized_sequence.pop();
      return r;
    }
    return next_normalized;
  }
};

class MockCollisionQueryEngine : public ICollisionQueryEngine {
 public:
  CollisionResult default_result;
  std::map<std::int64_t, CollisionResult> result_by_waypoint;
  std::atomic<int> call_count{0};
  bool throw_on_check = false;

  MockCollisionQueryEngine() {
    default_result.in_collision = false;
    default_result.engine_status = CollisionEngineStatus::kOk;
    default_result.decision = CollisionDecisionResult::kPass;
    capability_.engine_id = "mock_engine";
    capability_.engine_version = "test";
    capability_.library_lineage = "unit_test";
    capability_.supported_geometry_kinds = {"box"};
    capability_.thread_safety =
        CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.deterministic_ordering = true;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return capability_;
  }

  CollisionResult check(const CollisionWorldSnapshot& snapshot,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot&,
                        const CollisionCancellationToken& cancel) override {
    call_count.fetch_add(1);
    if (throw_on_check) {
      throw std::runtime_error("mock engine failure");
    }
    CollisionResult result = default_result;
    if (query.waypoint_index) {
      auto it = result_by_waypoint.find(*query.waypoint_index);
      if (it != result_by_waypoint.end()) {
        result = it->second;
      }
    }
    result.query_id = query.query_id;
    result.surface = query.surface;
    result.state_time_ns = query.state_time_ns;
    result.collision_world_revision = snapshot.revision;
    result.engine_id = capability_.engine_id;
    result.engine_version = capability_.engine_version;
    if (cancel.is_cancel_requested()) {
      result.in_collision = false;
      result.normalized_pairs.clear();
      result.engine_status = CollisionEngineStatus::kCancelled;
      result.decision = CollisionDecisionResult::kInconclusiveFail;
    }
    return result;
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override {
    return capability_.engine_version;
  }

 private:
  CollisionEngineCapabilityDescriptor capability_;
};

CollisionResult clean_result() {
  CollisionResult r;
  r.in_collision = false;
  r.engine_status = CollisionEngineStatus::kOk;
  r.decision = CollisionDecisionResult::kPass;
  return r;
}

CollisionResult colliding_result() {
  CollisionResult r;
  r.in_collision = true;
  r.normalized_pairs = {CollisionPairId{"a", "b"}};
  r.engine_status = CollisionEngineStatus::kOk;
  r.decision = CollisionDecisionResult::kFail;
  return r;
}

CollisionResult cancelled_result(std::string reason) {
  CollisionResult r;
  r.in_collision = false;
  r.engine_status = CollisionEngineStatus::kCancelled;
  r.decision = CollisionDecisionResult::kInconclusiveFail;
  r.raw_diagnostics["cancel_reason"] = std::move(reason);
  return r;
}

TrajectoryDTO long_trajectory(int n) {
  TrajectoryDTO t;
  t.joint_names = {"j0"};
  for (int i = 0; i < n; ++i) {
    TrajectoryWaypointDTO wp;
    wp.positions_rad = {0.01 * i};
    wp.time_from_start_s = 0.01 * i;
    t.waypoints.push_back(wp);
  }
  return t;
}

CollisionPolicySnapshot freespace_snapshot() {
  CollisionPolicySnapshot s;
  s.policy_id = "free_space";
  s.revision.revision_id = 3;
  return s;
}

CollisionWorldSnapshot test_snapshot(std::int64_t geometry_count = 42) {
  CollisionWorldSnapshot snapshot;
  snapshot.publisher_id = "unit_test_snapshot";
  snapshot.revision = CollisionWorldRevision{7, "snapshot-hash"};
  snapshot.snapshot_format_version = 1;
  snapshot.body_count = 2;
  snapshot.geometry_count = geometry_count;
  snapshot.all_bodies_have_geometry = true;
  return snapshot;
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     SerialFallbackWhenWorkerCountIsOne) {
  MockPlanningContext ctx;
  ctx.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(16));
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "worker_count_one");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     SerialFallbackWhenTrajectoryShorterThanMinimum) {
  MockPlanningContext ctx_a;
  ctx_a.next_normalized = clean_result();
  MockPlanningContext ctx_b;
  ctx_b.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 16;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(4));
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "trajectory_too_short");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     SerialFallbackWhenSupportContactsActive) {
  MockPlanningContext ctx_a, ctx_b;
  ctx_a.next_normalized = clean_result();
  ctx_b.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  SupportContactPolicyEntry entry;
  entry.pair = CollisionPairId{"gripper", "object"};
  cfg.pass_config.policy_snapshot.support_contacts.push_back(entry);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(16));
  EXPECT_EQ(pool.last_telemetry().fallback_reason,
            "support_contact_policy_active");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelRunDistributesWaypointsAcrossWorkers) {
  MockPlanningContext ctx_a, ctx_b, ctx_c, ctx_d;
  ctx_a.next_normalized = clean_result();
  ctx_b.next_normalized = clean_result();
  ctx_c.next_normalized = clean_result();
  ctx_d.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 4;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b, &ctx_c, &ctx_d};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(20));
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.waypoint_records.size(), 20u);

  const auto& telemetry = pool.last_telemetry();
  EXPECT_EQ(telemetry.workers_actually_used, 4);
  EXPECT_TRUE(telemetry.fallback_reason.empty());
  ASSERT_EQ(telemetry.per_worker_waypoint_counts.size(), 4u);
  int total = 0;
  for (int n : telemetry.per_worker_waypoint_counts) {
    total += n;
  }
  EXPECT_EQ(total, 20);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     NullWorkerContextsAreIgnoredAndFailClosedWhenNoneRemain) {
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> no_workers{nullptr, nullptr};
  auto bundle = pool.validate_trajectory(no_workers, long_trajectory(8));
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "no_worker_contexts");

  MockPlanningContext ctx;
  ctx.next_normalized = clean_result();
  std::vector<IPlanningContext*> one_worker{nullptr, &ctx, nullptr};
  bundle = pool.validate_trajectory(one_worker, long_trajectory(8));
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "worker_count_one");
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     ConfigSanitizesInvalidWorkerAndThresholdValues) {
  MockPlanningContext ctx_a;
  ctx_a.next_normalized = clean_result();
  MockPlanningContext ctx_b;
  ctx_b.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 0;
  cfg.min_waypoints_for_parallel = -5;
  cfg.fallback_log_window = std::chrono::nanoseconds{-1};
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(16));
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "worker_count_one");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelMergeReportsLowestFailingWaypointAcrossWorkers) {
  // 16 waypoints split into 4 workers of 4 each.
  // Worker 0 (wp 0-3): clean. Worker 1 (wp 4-7): collide at wp 6.
  // Worker 2 (wp 8-11): collide at wp 8. Worker 3 (wp 12-15): clean.
  // Lowest global failing waypoint is index 6, NOT 8.
  MockPlanningContext ctx0, ctx1, ctx2, ctx3;
  for (int i = 0; i < 4; ++i) ctx0.normalized_sequence.push(clean_result());
  ctx1.normalized_sequence.push(clean_result());
  ctx1.normalized_sequence.push(clean_result());
  ctx1.normalized_sequence.push(colliding_result());  // worker-local wp 2 = global 6
  ctx1.normalized_sequence.push(clean_result());
  ctx2.normalized_sequence.push(colliding_result());  // worker-local wp 0 = global 8
  ctx2.normalized_sequence.push(clean_result());
  ctx2.normalized_sequence.push(clean_result());
  ctx2.normalized_sequence.push(clean_result());
  for (int i = 0; i < 4; ++i) ctx3.normalized_sequence.push(clean_result());

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 4;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx0, &ctx1, &ctx2, &ctx3};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(16));
  EXPECT_FALSE(bundle.decision.accepted);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 6);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelAndSerialAgreeOnCleanTrajectory) {
  TrajectoryDTO trajectory = long_trajectory(20);

  // Serial reference.
  MockPlanningContext serial_ctx;
  serial_ctx.next_normalized = clean_result();
  CollisionValidationPassConfig serial_cfg;
  serial_cfg.policy_snapshot = freespace_snapshot();
  CollisionValidationCoordinator serial(serial_cfg);
  auto serial_bundle = serial.validate_trajectory(serial_ctx, trajectory);

  // Parallel pool.
  MockPlanningContext a, b;
  a.next_normalized = clean_result();
  b.next_normalized = clean_result();
  ParallelCollisionValidationWorkerPool::Config par_cfg;
  par_cfg.pass_config.policy_snapshot = freespace_snapshot();
  par_cfg.worker_count = 2;
  par_cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(par_cfg);
  std::vector<IPlanningContext*> workers{&a, &b};
  auto par_bundle = pool.validate_trajectory(workers, trajectory);

  EXPECT_EQ(serial_bundle.decision.accepted, par_bundle.decision.accepted);
  EXPECT_EQ(serial_bundle.waypoint_records.size(),
            par_bundle.waypoint_records.size());
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelRunFailsClosedWhenWorkersBorrowDifferentRevisions) {
  MockPlanningContext ctx_a, ctx_b;
  ctx_a.next_normalized = clean_result();
  ctx_b.next_normalized = clean_result();
  ctx_a.world_revision = CollisionWorldRevision{1, "rev-a"};
  ctx_b.world_revision = CollisionWorldRevision{2, "rev-b"};

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(8));

  EXPECT_TRUE(bundle.scene_changed_during_validation);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelRunFailsClosedWhenWorkerThrows) {
  MockPlanningContext ctx_a, ctx_b;
  ctx_a.next_normalized = clean_result();
  ctx_b.throw_normalized = true;

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
  auto bundle = pool.validate_trajectory(workers, long_trajectory(8));

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 4);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TelemetryRecordsSupportContactActiveEvenWhenParallel) {
  MockPlanningContext ctx_a, ctx_b;
  ctx_a.next_normalized = clean_result();
  ctx_b.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  SupportContactPolicyEntry entry;
  entry.pair = CollisionPairId{"gripper", "object"};
  cfg.pass_config.policy_snapshot.support_contacts.push_back(entry);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  cfg.refuse_parallel_with_support_contacts = false;  // permit parallel
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
  (void)pool.validate_trajectory(workers, long_trajectory(16));
  const auto& telemetry = pool.last_telemetry();
  EXPECT_TRUE(telemetry.fallback_reason.empty());
  EXPECT_TRUE(telemetry.support_contact_active);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TelemetryRecordsCollisionGeometryCountFromConfig) {
  MockPlanningContext ctx_a, ctx_b;
  ctx_a.next_normalized = clean_result();
  ctx_b.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  cfg.collision_geometry_count = 42;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
  (void)pool.validate_trajectory(workers, long_trajectory(16));
  EXPECT_EQ(pool.last_telemetry().collision_geometry_count, 42);

  // Fallback path also carries the count.
  ParallelCollisionValidationWorkerPool::Config cfg2 = cfg;
  cfg2.worker_count = 1;
  ParallelCollisionValidationWorkerPool pool2(cfg2);
  MockPlanningContext ctx_solo;
  ctx_solo.next_normalized = clean_result();
  std::vector<IPlanningContext*> solo{&ctx_solo};
  (void)pool2.validate_trajectory(solo, long_trajectory(16));
  EXPECT_EQ(pool2.last_telemetry().collision_geometry_count, 42);
  EXPECT_EQ(pool2.last_telemetry().fallback_reason, "worker_count_one");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TelemetryComputesValidationWallTimeP95WhenParallel) {
  MockPlanningContext ctx_a, ctx_b, ctx_c, ctx_d;
  ctx_a.next_normalized = clean_result();
  ctx_b.next_normalized = clean_result();
  ctx_c.next_normalized = clean_result();
  ctx_d.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 4;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b, &ctx_c, &ctx_d};
  (void)pool.validate_trajectory(workers, long_trajectory(20));
  EXPECT_GT(pool.last_telemetry().validation_wall_time_p95_ns, 0);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TelemetryLeavesP95UnsetWhenFallbackTakesSerialPath) {
  MockPlanningContext ctx_a;
  ctx_a.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);

  std::vector<IPlanningContext*> workers{&ctx_a};
  (void)pool.validate_trajectory(workers, long_trajectory(16));
  EXPECT_EQ(pool.last_telemetry().validation_wall_time_p95_ns, 0);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     SerialReferencePopulatedOnlyInQualificationMode) {
  MockPlanningContext ctx_a, ctx_b;
  ctx_a.next_normalized = clean_result();
  ctx_b.next_normalized = clean_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;

  // Default: disabled.
  {
    ParallelCollisionValidationWorkerPool pool(cfg);
    std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
    (void)pool.validate_trajectory(workers, long_trajectory(16));
    EXPECT_FALSE(pool.last_telemetry().serial_reference_wall_ns.has_value());
  }
  // Qualification: populated.
  {
    cfg.serial_reference_mode = ParallelCollisionValidationWorkerPool::
        SerialReferenceMode::kQualification;
    // Replenish mock queues — every coordinator call consumes `next_normalized`.
    MockPlanningContext ctx_x, ctx_y;
    ctx_x.next_normalized = clean_result();
    ctx_y.next_normalized = clean_result();
    ParallelCollisionValidationWorkerPool pool(cfg);
    std::vector<IPlanningContext*> workers{&ctx_x, &ctx_y};
    (void)pool.validate_trajectory(workers, long_trajectory(16));
    ASSERT_TRUE(pool.last_telemetry().serial_reference_wall_ns.has_value());
    EXPECT_GE(*pool.last_telemetry().serial_reference_wall_ns, 0);
  }
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     FallbackLogSinkIsRateLimitedPerCause) {
  std::vector<FallbackLogEvent> events;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 1;
  cfg.fallback_log_window = std::chrono::seconds{60};
  cfg.fallback_log_sink = [&events](const FallbackLogEvent& e) {
    events.push_back(e);
  };
  ParallelCollisionValidationWorkerPool pool(cfg);

  for (int i = 0; i < 5; ++i) {
    MockPlanningContext ctx;
    ctx.next_normalized = clean_result();
    std::vector<IPlanningContext*> workers{&ctx};
    (void)pool.validate_trajectory(workers, long_trajectory(16));
  }
  // Five calls, same cause, one window — sink fires once.
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].cause, "worker_count_one");
  EXPECT_EQ(events[0].worker_count_requested, 1);
  EXPECT_EQ(events[0].worker_count_available, 1);
  EXPECT_EQ(events[0].waypoint_count, 16);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     FallbackLogSinkFiresOncePerDistinctCause) {
  // Single pool instance, two distinct causes: each cause fires once.
  std::vector<FallbackLogEvent> events;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 16;
  cfg.fallback_log_window = std::chrono::seconds{60};
  cfg.fallback_log_sink = [&events](const FallbackLogEvent& e) {
    events.push_back(e);
  };
  ParallelCollisionValidationWorkerPool pool(cfg);

  // First call: trajectory_too_short (waypoint_count < min).
  {
    MockPlanningContext ctx_a, ctx_b;
    ctx_a.next_normalized = clean_result();
    ctx_b.next_normalized = clean_result();
    std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
    (void)pool.validate_trajectory(workers, long_trajectory(4));
  }
  // Second call: same cause, rate-limited (window 60s).
  {
    MockPlanningContext ctx_a, ctx_b;
    ctx_a.next_normalized = clean_result();
    ctx_b.next_normalized = clean_result();
    std::vector<IPlanningContext*> workers{&ctx_a, &ctx_b};
    (void)pool.validate_trajectory(workers, long_trajectory(4));
  }
  // Third call: worker_count_one — distinct cause, fires.
  {
    MockPlanningContext ctx_only;
    ctx_only.next_normalized = clean_result();
    std::vector<IPlanningContext*> workers{&ctx_only};
    (void)pool.validate_trajectory(workers, long_trajectory(16));
  }

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].cause, "trajectory_too_short");
  EXPECT_EQ(events[1].cause, "worker_count_one");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EngineBackedPathDistributesWaypointsAndUsesSnapshotGeometryCount) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  auto bundle = pool.validate_trajectory(test_snapshot(/*geometry_count=*/99),
                                         engines, long_trajectory(12), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.waypoint_records.size(), 12u);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 2);
  EXPECT_TRUE(pool.last_telemetry().fallback_reason.empty());
  EXPECT_EQ(pool.last_telemetry().collision_geometry_count, 99);
  EXPECT_EQ(engine_a.call_count.load() + engine_b.call_count.load(), 12);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EngineBackedPathReportsLowestFailingWaypoint) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  engine_a.result_by_waypoint[5] = colliding_result();
  engine_b.result_by_waypoint[2] = colliding_result();

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  auto bundle = pool.validate_trajectory(test_snapshot(), engines,
                                         long_trajectory(12), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 5);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EngineBackedPathFailsClosedOnCancellation) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  cancel.request_cancel("operator_stop");
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  auto bundle = pool.validate_trajectory(test_snapshot(), engines,
                                         long_trajectory(8), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancellation_reason.has_value());
  EXPECT_EQ(*bundle.cancellation_reason, "operator_stop");
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 0);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 0);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EngineBackedPathMergesEngineCancellationAtGlobalWaypoint) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  engine_b.result_by_waypoint[1] = cancelled_result("engine_b_cancelled");

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  auto bundle = pool.validate_trajectory(test_snapshot(), engines,
                                         long_trajectory(12), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancellation_reason.has_value());
  EXPECT_EQ(*bundle.cancellation_reason, "engine_b_cancelled");
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 7);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 7);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EngineBackedPathPreservesEarlierCollisionWhenLaterWorkerCancels) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  engine_a.result_by_waypoint[1] = colliding_result();
  engine_b.result_by_waypoint[1] = cancelled_result("operator_stop");

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  auto bundle = pool.validate_trajectory(test_snapshot(), engines,
                                         long_trajectory(12), cancel);

  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancelled_at_waypoint_index.has_value());
  EXPECT_EQ(*bundle.cancelled_at_waypoint_index, 7);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 1);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EngineBackedPathUsesSerialFallbackForSupportContacts) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  SupportContactPolicyEntry entry;
  entry.pair = CollisionPairId{"gripper", "object"};
  cfg.pass_config.policy_snapshot.support_contacts.push_back(entry);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  (void)pool.validate_trajectory(test_snapshot(), engines,
                                 long_trajectory(8), cancel);

  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason,
            "support_contact_policy_active");
  EXPECT_TRUE(pool.last_telemetry().support_contact_active);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EngineBackedSerialFallbackFailsClosedWhenEngineThrows) {
  MockCollisionQueryEngine engine;
  engine.throw_on_check = true;

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 1;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine};
  const auto snapshot = test_snapshot();
  auto bundle = pool.validate_trajectory(snapshot, engines,
                                         long_trajectory(8), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.severity, CollisionDecisionSeverity::kBlock);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(bundle.borrowed_world_revision.revision_id,
            snapshot.revision.revision_id);
  ASSERT_TRUE(bundle.final_world_revision.has_value());
  EXPECT_EQ(bundle.final_world_revision->revision_id,
            snapshot.revision.revision_id);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "worker_count_one");
}

TEST(EnvValidationCollisionWorkers, ReturnsDefaultWhenUnset) {
  ::unsetenv("MANYFORGE_VALIDATION_COLLISION_WORKERS");
  EXPECT_EQ(env_validation_collision_workers(3), 3);
}

TEST(EnvValidationCollisionWorkers, ClampsDefaultIntoSafeRange) {
  ::unsetenv("MANYFORGE_VALIDATION_COLLISION_WORKERS");
  EXPECT_EQ(env_validation_collision_workers(0), 1);
  EXPECT_EQ(env_validation_collision_workers(-4), 1);
  EXPECT_EQ(env_validation_collision_workers(128, /*max_workers=*/8), 8);
  EXPECT_EQ(env_validation_collision_workers(3, /*max_workers=*/0), 1);
}

TEST(EnvValidationCollisionWorkers, ParsesPositiveInteger) {
  ::setenv("MANYFORGE_VALIDATION_COLLISION_WORKERS", "5", 1);
  EXPECT_EQ(env_validation_collision_workers(1), 5);
  ::unsetenv("MANYFORGE_VALIDATION_COLLISION_WORKERS");
}

TEST(EnvValidationCollisionWorkers, ClampsToMax) {
  ::setenv("MANYFORGE_VALIDATION_COLLISION_WORKERS", "999", 1);
  EXPECT_EQ(env_validation_collision_workers(1, /*max_workers=*/8), 8);
  ::unsetenv("MANYFORGE_VALIDATION_COLLISION_WORKERS");
}

TEST(EnvValidationCollisionWorkers, FallsBackOnGarbage) {
  ::setenv("MANYFORGE_VALIDATION_COLLISION_WORKERS", "abc", 1);
  EXPECT_EQ(env_validation_collision_workers(2), 2);
  ::setenv("MANYFORGE_VALIDATION_COLLISION_WORKERS", "-3", 1);
  EXPECT_EQ(env_validation_collision_workers(2), 2);
  ::setenv("MANYFORGE_VALIDATION_COLLISION_WORKERS", "0", 1);
  EXPECT_EQ(env_validation_collision_workers(2), 2);
  ::setenv("MANYFORGE_VALIDATION_COLLISION_WORKERS", "4x", 1);
  EXPECT_EQ(env_validation_collision_workers(2), 2);
  ::unsetenv("MANYFORGE_VALIDATION_COLLISION_WORKERS");
}

TEST(CollisionCancellationToken, FirstCancelReasonWins) {
  CollisionCancellationToken token;
  EXPECT_FALSE(token.is_cancel_requested());

  token.request_cancel("first");
  token.request_cancel("second");

  EXPECT_TRUE(token.is_cancel_requested());
  EXPECT_EQ(token.reason(), "first");
}

TEST(CollisionCancellationToken, ConcurrentCancelPublishesACompleteReason) {
  CollisionCancellationToken token;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&token, i]() {
      token.request_cancel("reason-" + std::to_string(i));
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_TRUE(token.is_cancel_requested());
  EXPECT_FALSE(token.reason().empty());
}

}  // namespace
}  // namespace manyforge::core
