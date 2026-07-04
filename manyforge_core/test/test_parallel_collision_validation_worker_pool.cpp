// Chunk 7 unit tests for ParallelCollisionValidationWorkerPool:
// deterministic merge, serial fallback for support-contact / short
// trajectories / worker_count_one, equivalence with serial coordinator on
// fixture trajectories, and telemetry shape.

#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/types/reason_codes.hpp>

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
  ASSERT_FALSE(bundle.waypoint_records.empty());
  EXPECT_EQ(bundle.waypoint_records.front().result.engine_status,
            CollisionEngineStatus::kEngineError);
  EXPECT_EQ(bundle.waypoint_records.front().result.raw_diagnostics.at("reason_code"),
            reason_codes::kCollisionValidationSerialFallbackException);
  EXPECT_EQ(bundle.waypoint_records.front().result.raw_diagnostics.at("exception"),
            "mock engine failure");
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
