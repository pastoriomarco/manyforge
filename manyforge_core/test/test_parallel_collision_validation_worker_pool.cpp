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
#include <mutex>
#include <queue>
#include <set>
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
  bool record_thread_ids = false;
  std::mutex thread_ids_mutex;
  std::set<std::thread::id> thread_ids;

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

  void set_thread_safety(CollisionEngineThreadSafetyClass thread_safety) {
    capability_.thread_safety = thread_safety;
  }

  std::set<std::thread::id> observed_thread_ids() {
    std::lock_guard<std::mutex> lock(thread_ids_mutex);
    return thread_ids;
  }

  CollisionResult check(const CollisionWorldSnapshot& snapshot,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot&,
                        const CollisionCancellationToken& cancel) override {
    call_count.fetch_add(1);
    if (record_thread_ids) {
      std::lock_guard<std::mutex> lock(thread_ids_mutex);
      thread_ids.insert(std::this_thread::get_id());
    }
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
     ReusesPersistentWorkerThreadsAcrossValidationCalls) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  engine_a.record_thread_ids = true;
  engine_b.record_thread_ids = true;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  auto first = pool.validate_trajectory(test_snapshot(), engines,
                                        long_trajectory(12), cancel);
  ASSERT_TRUE(first.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 2);

  auto second = pool.validate_trajectory(test_snapshot(), engines,
                                         long_trajectory(12), cancel);
  ASSERT_TRUE(second.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 2);
  EXPECT_EQ(engine_a.call_count.load() + engine_b.call_count.load(), 24);

  std::set<std::thread::id> observed;
  auto a_threads = engine_a.observed_thread_ids();
  auto b_threads = engine_b.observed_thread_ids();
  observed.insert(a_threads.begin(), a_threads.end());
  observed.insert(b_threads.begin(), b_threads.end());
  EXPECT_GE(observed.size(), 2u);
  EXPECT_LE(observed.size(), 2u);
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
  ASSERT_TRUE(bundle.decision.first_failing_time_ns.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_time_ns, 70'000'000);
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
  ASSERT_TRUE(bundle.decision.first_failing_time_ns.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_time_ns, 10'000'000);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelCancelAfterPriorRejectMatchesSerialMetadata) {
  MockCollisionQueryEngine parallel_a;
  MockCollisionQueryEngine parallel_b;
  parallel_a.result_by_waypoint[1] = colliding_result();
  parallel_b.result_by_waypoint[1] = cancelled_result("operator_stop");

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken parallel_cancel;
  std::vector<ICollisionQueryEngine*> engines{&parallel_a, &parallel_b};
  auto parallel_bundle = pool.validate_trajectory(
      test_snapshot(), engines, long_trajectory(12), parallel_cancel);

  MockCollisionQueryEngine serial_engine;
  serial_engine.result_by_waypoint[1] = colliding_result();
  serial_engine.result_by_waypoint[7] = cancelled_result("operator_stop");
  CollisionValidationCoordinator serial_coord(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  auto serial_bundle = serial_coord.validate_trajectory(
      test_snapshot(), serial_engine, long_trajectory(12), serial_cancel);

  EXPECT_TRUE(parallel_bundle.cancelled);
  EXPECT_TRUE(serial_bundle.cancelled);
  EXPECT_FALSE(parallel_bundle.decision.accepted);
  EXPECT_EQ(parallel_bundle.decision.accepted,
            serial_bundle.decision.accepted);
  EXPECT_EQ(parallel_bundle.decision.action, serial_bundle.decision.action);
  EXPECT_EQ(parallel_bundle.decision.first_failing_waypoint,
            serial_bundle.decision.first_failing_waypoint);
  EXPECT_EQ(parallel_bundle.decision.first_failing_time_ns,
            serial_bundle.decision.first_failing_time_ns);
  EXPECT_EQ(parallel_bundle.cancelled_at_waypoint_index,
            serial_bundle.cancelled_at_waypoint_index);
  EXPECT_EQ(parallel_bundle.cancellation_reason,
            serial_bundle.cancellation_reason);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     SingleContextDuplicateEngineFallsBackToSerial) {
  MockCollisionQueryEngine engine;
  engine.set_thread_safety(
      CollisionEngineThreadSafetyClass::kSingleContextExclusive);

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine, &engine};
  auto bundle = pool.validate_trajectory(test_snapshot(), engines,
                                         long_trajectory(8), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(engine.call_count.load(), 8);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason,
            "single_context_duplicate_engine");
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

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelWorkerExceptionReportsEngineFailureNotSceneChange) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  engine_b.throw_on_check = true;

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b};
  const auto snapshot = test_snapshot();
  auto bundle = pool.validate_trajectory(snapshot, engines,
                                         long_trajectory(12), cancel);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_FALSE(bundle.scene_changed_during_validation);
  EXPECT_EQ(bundle.borrowed_world_revision.revision_id,
            snapshot.revision.revision_id);
  ASSERT_TRUE(bundle.final_world_revision.has_value());
  EXPECT_EQ(bundle.final_world_revision->revision_id,
            snapshot.revision.revision_id);
  ASSERT_TRUE(bundle.decision.first_failing_waypoint.has_value());
  EXPECT_EQ(*bundle.decision.first_failing_waypoint, 6);
  auto it = std::find_if(bundle.waypoint_records.begin(),
                         bundle.waypoint_records.end(),
                         [](const CollisionValidationWaypointRecord& record) {
                           return record.result.engine_status ==
                                  CollisionEngineStatus::kEngineError;
                         });
  ASSERT_NE(it, bundle.waypoint_records.end());
  EXPECT_EQ(it->waypoint_index, 6);
  EXPECT_EQ(it->result.raw_diagnostics.at("reason_code"),
            reason_codes::kCollisionValidationWorkerException);
  EXPECT_EQ(it->result.raw_diagnostics.at("exception"),
            "mock engine failure");
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
