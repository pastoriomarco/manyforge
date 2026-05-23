// Chunk 7 unit tests for ParallelCollisionValidationWorkerPool:
// deterministic merge, serial fallback for support-contact / short
// trajectories / worker_count_one, equivalence with serial coordinator on
// fixture trajectories, and telemetry shape.

#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <queue>
#include <stdexcept>
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

}  // namespace
}  // namespace manyforge::core
