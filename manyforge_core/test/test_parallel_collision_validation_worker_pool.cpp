// Unit tests for ParallelCollisionValidationWorkerPool:
// deterministic stateless and ordered-contact merge, named serial fallbacks,
// equivalence with the serial coordinator, timeout ownership, and telemetry.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/ordered_contact_validation.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {


class MockCollisionQueryEngine : public ICollisionQueryEngine {
 public:
  CollisionResult default_result;
  std::map<std::int64_t, CollisionResult> result_by_waypoint;
  std::atomic<int> call_count{0};
  bool throw_on_check = false;
  std::optional<std::int64_t> throw_on_waypoint;
  bool record_thread_ids = false;
  bool block_on_check = false;
  std::chrono::microseconds check_delay{0};
  std::function<void(const CollisionQuery&)> on_check;
  std::mutex thread_ids_mutex;
  std::set<std::thread::id> thread_ids;
  std::mutex block_mutex;
  std::condition_variable block_cv;
  bool release_blocked_checks = false;
  std::atomic<int> blocked_check_count{0};
  std::atomic<const CollisionWorldSnapshot*> observed_snapshot{nullptr};

  MockCollisionQueryEngine() {
    static std::atomic<std::uint64_t> next_context_id{1};
    default_result.in_collision = false;
    default_result.engine_status = CollisionEngineStatus::kOk;
    default_result.decision = CollisionDecisionResult::kPass;
    capability_.engine_id = "mock_engine";
    capability_.engine_version = "test";
    capability_.library_lineage = "unit_test";
    capability_.mutable_context_id = "mock_context_" + std::to_string(next_context_id.fetch_add(1));
    capability_.supported_geometry_kinds = {"box"};
    capability_.supports_distance_query = true;
    capability_.supported_distance_request_modes = {
        CollisionDistanceRequestMode::kNone,
        CollisionDistanceRequestMode::kSelectedCollidingPairs,
        CollisionDistanceRequestMode::kAllCollidingPairs,
        CollisionDistanceRequestMode::kAllActivePairs};
    capability_.supports_penetration_depth = true;
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

  void set_mutable_context_id(std::string id) { capability_.mutable_context_id = std::move(id); }

  std::set<std::thread::id> observed_thread_ids() {
    std::lock_guard<std::mutex> lock(thread_ids_mutex);
    return thread_ids;
  }

  CollisionResult check(const CollisionWorldSnapshot& snapshot,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot&,
                        const CollisionCancellationToken& cancel) override {
    call_count.fetch_add(1);
    observed_snapshot.store(&snapshot, std::memory_order_relaxed);
    if (record_thread_ids) {
      std::lock_guard<std::mutex> lock(thread_ids_mutex);
      thread_ids.insert(std::this_thread::get_id());
    }
    if (check_delay > std::chrono::microseconds::zero()) {
      std::this_thread::sleep_for(check_delay);
    }
    if (on_check) {
      on_check(query);
    }
    if (throw_on_check || (throw_on_waypoint && query.waypoint_index == throw_on_waypoint)) {
      throw std::runtime_error("mock engine failure");
    }
    if (block_on_check) {
      std::unique_lock<std::mutex> lock(block_mutex);
      blocked_check_count.fetch_add(1);
      block_cv.notify_all();
      block_cv.wait(lock, [this]() { return release_blocked_checks; });
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

  void release_blocked() {
    {
      std::lock_guard<std::mutex> lock(block_mutex);
      release_blocked_checks = true;
    }
    block_cv.notify_all();
  }

  void wait_until_blocked(int expected_count = 1) {
    std::unique_lock<std::mutex> lock(block_mutex);
    block_cv.wait(
        lock, [this, expected_count]() { return blocked_check_count.load() >= expected_count; });
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

CollisionResult contact_result(const CollisionPairId& pair, double signed_clearance_m) {
  CollisionResult result;
  result.in_collision = signed_clearance_m < 0.0;
  if (result.in_collision) {
    result.normalized_pairs = {pair};
  }
  result.pair_clearances = {CollisionPairClearance{pair, signed_clearance_m}};
  result.min_clearance_m = signed_clearance_m;
  result.min_clearance_pair = pair;
  result.engine_status = CollisionEngineStatus::kOk;
  result.decision =
      result.in_collision ? CollisionDecisionResult::kFail : CollisionDecisionResult::kPass;
  return result;
}

CollisionResult clear_result() {
  CollisionResult result;
  result.engine_status = CollisionEngineStatus::kOk;
  result.decision = CollisionDecisionResult::kPass;
  return result;
}

CollisionPolicySnapshot contact_release_snapshot(const CollisionPairId& pair) {
  CollisionPolicySnapshot policy;
  policy.policy_id = "contact_release";
  policy.revision.revision_id = 3;
  policy.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.max_start_penetration_m = 0.10;
  entry.release_tolerance_m = 1e-5;
  entry.min_release_progress_m = 0.001;
  entry.require_separation_by_end = true;
  entry.require_strictly_decreasing_penetration = true;
  policy.support_contacts.push_back(entry);
  return policy;
}

CollisionPolicySnapshot bounded_penetration_snapshot(
    const CollisionPairId& pair, double max_contact_penetration_m) {
  auto policy = contact_release_snapshot(pair);
  auto& entry = policy.support_contacts.front();
  entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
  entry.max_contact_penetration_m = max_contact_penetration_m;
  entry.enforce_max_start_penetration = false;
  entry.max_start_penetration_m = 0.0;
  entry.release_tolerance_m = 0.0;
  entry.min_release_progress_m = 0.0;
  entry.require_separation_by_end = false;
  entry.require_strictly_decreasing_penetration = false;
  entry.allow_recontact = false;
  return policy;
}

void assign_parallel_results(const std::vector<MockCollisionQueryEngine*>& engines,
                             const std::vector<CollisionResult>& results) {
  for (auto* engine : engines) {
    engine->result_by_waypoint.clear();
  }
  const int worker_count = static_cast<int>(engines.size());
  const int waypoint_count = static_cast<int>(results.size());
  const int per_worker = waypoint_count / worker_count;
  const int leftover = waypoint_count % worker_count;
  int cursor = 0;
  for (int worker = 0; worker < worker_count; ++worker) {
    const int length = per_worker + (worker < leftover ? 1 : 0);
    for (int local = 0; local < length; ++local) {
      engines[static_cast<std::size_t>(worker)]->result_by_waypoint[local] =
          results[static_cast<std::size_t>(cursor + local)];
    }
    cursor += length;
  }
}

CollisionResult multi_contact_result(
    const std::vector<std::pair<CollisionPairId, double>>& clearances) {
  CollisionResult result;
  result.engine_status = CollisionEngineStatus::kOk;
  result.decision = CollisionDecisionResult::kPass;
  for (const auto& [pair, clearance] : clearances) {
    result.pair_clearances.push_back({pair, clearance});
    if (!result.min_clearance_m || clearance < *result.min_clearance_m) {
      result.min_clearance_m = clearance;
      result.min_clearance_pair = pair;
    }
    if (clearance < 0.0) {
      result.in_collision = true;
      result.normalized_pairs.push_back(pair);
      result.decision = CollisionDecisionResult::kFail;
    }
  }
  return result;
}

void expect_policy_equivalent(const CollisionValidationDecisionBundle& parallel,
                              const CollisionValidationDecisionBundle& serial) {
  EXPECT_EQ(parallel.decision.accepted, serial.decision.accepted);
  EXPECT_EQ(parallel.decision.action, serial.decision.action);
  EXPECT_EQ(parallel.decision.severity, serial.decision.severity);
  EXPECT_EQ(parallel.decision.first_failing_waypoint, serial.decision.first_failing_waypoint);
  EXPECT_EQ(parallel.decision.first_failing_time_ns, serial.decision.first_failing_time_ns);
  ASSERT_EQ(parallel.waypoint_records.size(), serial.waypoint_records.size());
  for (std::size_t i = 0; i < parallel.waypoint_records.size(); ++i) {
    const auto& left = parallel.waypoint_records[i];
    const auto& right = serial.waypoint_records[i];
    EXPECT_EQ(left.waypoint_index, right.waypoint_index) << i;
    EXPECT_EQ(left.result.decision, right.result.decision) << i;
    ASSERT_EQ(left.result.normalized_pairs_classified.size(),
              right.result.normalized_pairs_classified.size())
        << i;
    for (std::size_t pair_index = 0; pair_index < left.result.normalized_pairs_classified.size();
         ++pair_index) {
      EXPECT_EQ(left.result.normalized_pairs_classified[pair_index].classification,
                right.result.normalized_pairs_classified[pair_index].classification)
          << i << ":" << pair_index;
      EXPECT_EQ(left.result.normalized_pairs_classified[pair_index].is_first_failure,
                right.result.normalized_pairs_classified[pair_index].is_first_failure)
          << i << ":" << pair_index;
    }
    EXPECT_EQ(left.result.raw_diagnostics, right.result.raw_diagnostics) << i;
  }
}

TrajectoryDTO long_trajectory(int n) {
  TrajectoryDTO t;
  t.joint_names = {"j0"};
  for (int i = 0; i < n; ++i) {
    TrajectoryWaypointDTO wp;
    wp.positions_rad.push_back(0.01 * i);
    wp.time_from_start_s = 0.01 * i;
    t.waypoints.push_back(std::move(wp));
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

// Call-local results become available just before the worker tail publishes
// its instantaneous accounting. Tests that inspect settled counters therefore
// wait boundedly for that separate diagnostic boundary.
template <typename Predicate>
ParallelValidationPoolStatus wait_for_pool_status(
    const ParallelCollisionValidationWorkerPool& pool, Predicate predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto status = pool.status();
  while (!predicate(status) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    status = pool.status();
  }
  return status;
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
  const auto status = wait_for_pool_status(pool, [](const auto& current) {
    return current.queued_jobs == 0 && current.active_workers == 0 &&
           current.completed_jobs_total >= 2;
  });
  EXPECT_EQ(status.queued_jobs, 0u);
  EXPECT_EQ(status.active_workers, 0u);
  EXPECT_EQ(status.completed_jobs_total, 2u);
  EXPECT_EQ(status.cancelled_jobs_total, 0u);
  EXPECT_EQ(status.dropped_jobs_total, 0u);
  EXPECT_EQ(status.abandoned_jobs_total, 0u);
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
     OwnedSnapshotOverloadRetainsPublishedSnapshotWithoutCopying) {
  auto engine_a = std::make_shared<MockCollisionQueryEngine>();
  auto engine_b = std::make_shared<MockCollisionQueryEngine>();
  std::vector<std::shared_ptr<ICollisionQueryEngine>> engines{engine_a, engine_b};
  auto snapshot = std::make_shared<const CollisionWorldSnapshot>(test_snapshot());
  auto cancel = std::make_shared<CollisionCancellationToken>();
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);

  const auto bundle =
      pool.validate_trajectory(snapshot, engines, long_trajectory(8), cancel, cfg.pass_config);

  ASSERT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(engine_a->observed_snapshot.load(std::memory_order_relaxed), snapshot.get());
  EXPECT_EQ(engine_b->observed_snapshot.load(std::memory_order_relaxed), snapshot.get());
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
     DistinctEngineWrappersSharingMutableContextFallBackToSerial) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  engine_a.set_mutable_context_id("shared_context");
  engine_b.set_mutable_context_id("shared_context");
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 2;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  const auto bundle = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b},
      long_trajectory(8), cancel);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().workers_effective, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "single_context_duplicate_engine");
}

TEST(ParallelCollisionValidationWorkerPoolTest, MissingMutableContextIdentityFailsClosedToSerial) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  engine_b.set_mutable_context_id("");
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 2;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  (void)pool.validate_trajectory(test_snapshot(),
                                 std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b},
                                 long_trajectory(8), cancel);

  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().workers_effective, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "single_context_missing_mutable_context_id");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseMatchesSerialAcrossChunkBoundaries) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine engine_c;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{-0.050, -0.045, -0.040, -0.030, -0.020, -0.010,
                                       -0.005, 0.001,  0.004,  0.006,  0.008,  0.010};
  std::vector<CollisionResult> results;
  for (double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  assign_parallel_results({&engine_a, &engine_b, &engine_c}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 3;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{&engine_a, &engine_b, &engine_c};
  const auto trajectory = long_trajectory(12);
  auto parallel = pool.validate_trajectory(test_snapshot(), engines, trajectory, cancel);

  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine, trajectory,
                                                       serial_cancel);

  expect_policy_equivalent(parallel, serial);
  EXPECT_TRUE(parallel.decision.accepted);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 3);
  EXPECT_TRUE(pool.last_telemetry().fallback_reason.empty());
  EXPECT_TRUE(pool.last_telemetry().support_contact_active);
  EXPECT_TRUE(pool.last_telemetry().contact_release_active);
  EXPECT_TRUE(pool.last_telemetry().ordered_contact_summary_used);
  EXPECT_EQ(engine_a.call_count.load() + engine_b.call_count.load() + engine_c.call_count.load(),
            12);
  const auto status = wait_for_pool_status(pool, [](const auto& current) {
    return current.queued_jobs == 0 && current.active_workers == 0 &&
           current.completed_jobs_total >= 6;
  });
  EXPECT_EQ(status.queued_jobs, 0u);
  EXPECT_EQ(status.active_workers, 0u);
  EXPECT_EQ(status.completed_jobs_total, 6u);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseDetectsBoundaryDepthIncreaseWithSerialDiagnostics) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{-0.050, -0.040, -0.030, -0.020,
                                       -0.030, -0.020, -0.010, 0.001};
  std::vector<CollisionResult> results;
  for (double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  assign_parallel_results({&engine_a, &engine_b}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(8);
  CollisionCancellationToken cancel;
  auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
      cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine, trajectory,
                                                       serial_cancel);

  expect_policy_equivalent(parallel, serial);
  ASSERT_TRUE(parallel.decision.first_failing_waypoint);
  EXPECT_EQ(*parallel.decision.first_failing_waypoint, 4);
  EXPECT_EQ(parallel.waypoint_records[4].result.raw_diagnostics.at("contact_release_violation"),
            "penetration_increased");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedBoundedPenetrationMatchesSerialAcrossChunkBoundaries) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  std::vector<CollisionResult> results{
      clear_result(),
      contact_result(pair, -0.004),
      clear_result(),
      clear_result(),
      contact_result(pair, -0.003),
      clear_result(),
      contact_result(pair, -0.002),
      contact_result(pair, -0.005),
  };

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot =
      bounded_penetration_snapshot(pair, 0.005);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));

  auto validate_serial_and_parallel = [&]() {
    assign_parallel_results({&engine_a, &engine_b}, results);
    serial_engine.result_by_waypoint.clear();
    for (std::size_t i = 0; i < results.size(); ++i) {
      serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] =
          results[i];
    }
    CollisionCancellationToken parallel_cancel;
    auto parallel = pool.validate_trajectory(
        test_snapshot(),
        std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
        parallel_cancel);
    CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
    CollisionCancellationToken serial_cancel;
    auto serial = serial_coordinator.validate_trajectory(
        test_snapshot(), serial_engine, trajectory, serial_cancel);
    expect_policy_equivalent(parallel, serial);
    return parallel;
  };

  const auto accepted = validate_serial_and_parallel();
  EXPECT_TRUE(accepted.decision.accepted);
  EXPECT_TRUE(pool.last_telemetry().ordered_contact_summary_used);
  EXPECT_TRUE(pool.last_telemetry().fallback_reason.empty());

  results[6] = contact_result(pair, -0.006);
  const auto rejected = validate_serial_and_parallel();
  ASSERT_FALSE(rejected.decision.accepted);
  ASSERT_TRUE(rejected.decision.first_failing_waypoint);
  EXPECT_EQ(*rejected.decision.first_failing_waypoint, 6);
  EXPECT_EQ(rejected.waypoint_records[6]
                .result.raw_diagnostics.at("contact_release_violation"),
            "contact_penetration_exceeds_policy");
  const auto violation = contact_policy_violation_details(
      rejected, cfg.pass_config.policy_snapshot, "block_dispatch");
  ASSERT_TRUE(violation.has_value());
  EXPECT_EQ(violation->at("policy_mode"), "bounded_penetration");
  EXPECT_EQ(violation->at("initial_contact_pair_count"), "0");
  EXPECT_EQ(violation->at("current_contact_pair_count"), "1");
  EXPECT_EQ(violation->at("distance_request_mode"),
            "selected_colliding_pairs");
  EXPECT_EQ(violation->at("evaluated_distance_pair_count"), "1");
  EXPECT_EQ(violation->find("release_evidence_mode"), violation->end());
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     MixedSignedBinaryAndBoundedContactsMatchSerialAcrossChunkBoundary) {
  const CollisionPairId signed_pair{"component/arm/model/arm_model/body/tool/collision/0", "scene/signed/shape/0"};
  const CollisionPairId binary_pair{"scene/binary/shape/0", "scene/table/shape/0"};
  const CollisionPairId bounded_pair{"scene/insert/shape/0", "scene/socket/shape/0"};

  auto policy = contact_release_snapshot(signed_pair);
  SupportContactPolicyEntry binary;
  binary.pair = binary_pair;
  binary.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  binary.enforce_max_start_penetration = false;
  binary.max_start_penetration_m = 0.0;
  binary.release_tolerance_m = 0.0;
  binary.min_release_progress_m = 0.0;
  binary.require_separation_by_end = true;
  binary.require_strictly_decreasing_penetration = false;
  policy.support_contacts.push_back(binary);
  policy.support_contacts.push_back(
      bounded_penetration_snapshot(bounded_pair, 0.005)
          .support_contacts.front());

  auto sample = [](std::vector<CollisionPairId> colliding,
                   std::vector<CollisionPairClearance> clearances) {
    std::sort(colliding.begin(), colliding.end());
    std::sort(clearances.begin(), clearances.end(),
              [](const auto& lhs, const auto& rhs) {
                return lhs.pair < rhs.pair;
              });
    CollisionResult result;
    result.in_collision = !colliding.empty();
    result.normalized_pairs = std::move(colliding);
    result.pair_clearances = std::move(clearances);
    result.engine_status = CollisionEngineStatus::kOk;
    result.decision = result.in_collision ? CollisionDecisionResult::kFail
                                          : CollisionDecisionResult::kPass;
    return result;
  };
  const std::vector<CollisionResult> results{
      sample({signed_pair, binary_pair}, {{signed_pair, -0.004}}),
      sample({signed_pair, binary_pair}, {{signed_pair, -0.002}}),
      sample({binary_pair, bounded_pair}, {{bounded_pair, -0.003}}),
      sample({bounded_pair}, {{bounded_pair, -0.004}}),
  };

  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  assign_parallel_results({&engine_a, &engine_b}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] =
        results[i];
  }

  std::mutex queries_mutex;
  std::vector<std::vector<CollisionPairId>> selected_pairs_by_query;
  const auto capture_query = [&](const CollisionQuery& query) {
    std::lock_guard<std::mutex> lock(queries_mutex);
    selected_pairs_by_query.push_back(query.selected_distance_pairs);
  };
  engine_a.on_check = capture_query;
  engine_b.on_check = capture_query;

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = policy;
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));
  CollisionCancellationToken parallel_cancel;
  const auto parallel = pool.validate_trajectory(
      test_snapshot(),
      std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
      parallel_cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  const auto serial = serial_coordinator.validate_trajectory(
      test_snapshot(), serial_engine, trajectory, serial_cancel);

  expect_policy_equivalent(parallel, serial);
  ASSERT_TRUE(parallel.decision.accepted);
  EXPECT_TRUE(pool.last_telemetry().ordered_contact_summary_used);
  EXPECT_TRUE(pool.last_telemetry().fallback_reason.empty());

  std::vector<CollisionPairId> expected_selected{bounded_pair, signed_pair};
  std::sort(expected_selected.begin(), expected_selected.end());
  {
    std::lock_guard<std::mutex> lock(queries_mutex);
    ASSERT_EQ(selected_pairs_by_query.size(), results.size());
    for (const auto& selected : selected_pairs_by_query) {
      EXPECT_EQ(selected, expected_selected);
      EXPECT_EQ(std::find(selected.begin(), selected.end(), binary_pair),
                selected.end());
    }
  }
  for (const auto& record : parallel.waypoint_records) {
    EXPECT_EQ(std::find_if(record.result.pair_clearances.begin(),
                           record.result.pair_clearances.end(),
                           [&](const auto& clearance) {
                             return clearance.pair == binary_pair;
                           }),
              record.result.pair_clearances.end());
  }
  ASSERT_TRUE(parallel.waypoint_records.back().result.in_collision);
  EXPECT_NE(std::find(parallel.waypoint_records.back()
                          .result.normalized_pairs.begin(),
                      parallel.waypoint_records.back()
                          .result.normalized_pairs.end(),
                      bounded_pair),
            parallel.waypoint_records.back().result.normalized_pairs.end());
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseDetectsContactBeginningAtChunkBoundary) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{0.010, 0.010, 0.010, 0.010, -0.010, -0.005, 0.001, 0.002};
  std::vector<CollisionResult> results;
  for (const double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  assign_parallel_results({&engine_a, &engine_b}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));
  CollisionCancellationToken parallel_cancel;
  const auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
      parallel_cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  const auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine,
                                                             trajectory, serial_cancel);

  expect_policy_equivalent(parallel, serial);
  ASSERT_TRUE(parallel.decision.first_failing_waypoint);
  EXPECT_EQ(*parallel.decision.first_failing_waypoint, 4);
  EXPECT_EQ(parallel.waypoint_records[4].result.raw_diagnostics.at("contact_release_violation"),
            "new_active_contact_after_clear_start");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseDetectsNoProgressPlateauAcrossChunkBoundary) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{-0.0500, -0.0499, -0.0499, -0.0499,
                                       -0.0499, -0.0499, -0.0499, -0.0499};
  std::vector<CollisionResult> results;
  for (const double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  assign_parallel_results({&engine_a, &engine_b}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  auto& release = cfg.pass_config.policy_snapshot.support_contacts.front();
  release.require_separation_by_end = false;
  release.min_release_progress_m = 0.010;
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));
  CollisionCancellationToken parallel_cancel;
  const auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
      parallel_cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  const auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine,
                                                             trajectory, serial_cancel);

  expect_policy_equivalent(parallel, serial);
  ASSERT_TRUE(parallel.decision.first_failing_waypoint);
  EXPECT_EQ(*parallel.decision.first_failing_waypoint, 7);
  EXPECT_EQ(parallel.waypoint_records[7].result.raw_diagnostics.at("contact_release_violation"),
            "insufficient_release_progress");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseDetectsBoundaryRecontactWithSerialDiagnostics) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{-0.050, -0.030, -0.010, 0.002, -0.005, 0.003, 0.004, 0.005};
  std::vector<CollisionResult> results;
  for (double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  assign_parallel_results({&engine_a, &engine_b}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(8);
  CollisionCancellationToken cancel;
  auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
      cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine, trajectory,
                                                       serial_cancel);

  expect_policy_equivalent(parallel, serial);
  ASSERT_TRUE(parallel.decision.first_failing_waypoint);
  EXPECT_EQ(*parallel.decision.first_failing_waypoint, 4);
  EXPECT_EQ(parallel.waypoint_records[4].result.raw_diagnostics.at("contact_release_violation"),
            "recontact_after_release");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseMissingCollidingMetricAtBoundaryMatchesSerial) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  std::vector<CollisionResult> results{contact_result(pair, -0.050),
                                       contact_result(pair, -0.040),
                                       contact_result(pair, -0.030),
                                       contact_result(pair, -0.020),
                                       CollisionResult{},
                                       contact_result(pair, -0.010),
                                       contact_result(pair, 0.001),
                                       contact_result(pair, 0.002)};
  results[4].engine_status = CollisionEngineStatus::kOk;
  results[4].in_collision = true;
  results[4].normalized_pairs = {pair};
  results[4].decision = CollisionDecisionResult::kFail;
  assign_parallel_results({&engine_a, &engine_b}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(8);
  CollisionCancellationToken cancel;
  auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
      cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine, trajectory,
                                                       serial_cancel);

  expect_policy_equivalent(parallel, serial);
  ASSERT_TRUE(parallel.decision.first_failing_waypoint);
  EXPECT_EQ(*parallel.decision.first_failing_waypoint, 4);
  EXPECT_EQ(parallel.decision.action, CollisionDecisionAction::kInconclusive);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseMatchesSerialForEverySmallPartitionAndPattern) {
  const CollisionPairId pair{"gripper", "object"};
  const std::array<std::optional<double>, 4> sample_kinds{std::nullopt, -0.040, -0.015, 0.002};
  constexpr int waypoint_count = 5;
  constexpr int pattern_count = 4 * 4 * 4 * 4 * 4;

  for (int worker_count = 2; worker_count <= waypoint_count; ++worker_count) {
    std::vector<std::unique_ptr<MockCollisionQueryEngine>> engine_storage;
    std::vector<MockCollisionQueryEngine*> engines;
    for (int i = 0; i < worker_count; ++i) {
      engine_storage.push_back(std::make_unique<MockCollisionQueryEngine>());
      engines.push_back(engine_storage.back().get());
    }
    MockCollisionQueryEngine serial_engine;
    ParallelCollisionValidationWorkerPool::Config cfg;
    cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
    cfg.worker_count = worker_count;
    cfg.min_waypoints_for_parallel = 1;
    ParallelCollisionValidationWorkerPool pool(cfg);
    const auto trajectory = long_trajectory(waypoint_count);

    for (int encoded_pattern = 0; encoded_pattern < pattern_count; ++encoded_pattern) {
      SCOPED_TRACE("workers=" + std::to_string(worker_count) +
                   " pattern=" + std::to_string(encoded_pattern));
      std::vector<CollisionResult> results;
      results.reserve(waypoint_count);
      int cursor = encoded_pattern;
      for (int waypoint = 0; waypoint < waypoint_count; ++waypoint) {
        const auto& selected = sample_kinds[static_cast<std::size_t>(cursor % 4)];
        cursor /= 4;
        if (selected) {
          results.push_back(contact_result(pair, *selected));
        } else {
          CollisionResult missing;
          missing.engine_status = CollisionEngineStatus::kOk;
          missing.decision = CollisionDecisionResult::kPass;
          results.push_back(std::move(missing));
        }
      }
      assign_parallel_results(engines, results);
      serial_engine.result_by_waypoint.clear();
      for (std::size_t i = 0; i < results.size(); ++i) {
        serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
      }

      CollisionCancellationToken parallel_cancel;
      const auto parallel = pool.validate_trajectory(
          test_snapshot(), std::vector<ICollisionQueryEngine*>(engines.begin(), engines.end()),
          trajectory, parallel_cancel);
      CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
      CollisionCancellationToken serial_cancel;
      const auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine,
                                                                 trajectory, serial_cancel);
      expect_policy_equivalent(parallel, serial);
    }
  }
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseMatchesSerialForMultipleContactIdentities) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine engine_c;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair_a{"left_finger", "object"};
  const CollisionPairId pair_b{"right_finger", "object"};
  const std::vector<double> a{-0.05, -0.04, -0.03, -0.02, -0.01, 0.001, 0.002, 0.003, 0.004};
  const std::vector<double> b{-0.04, -0.03, -0.02, -0.01, 0.001, 0.002, 0.003, 0.004, 0.005};
  std::vector<CollisionResult> results;
  for (std::size_t i = 0; i < a.size(); ++i) {
    results.push_back(multi_contact_result({{pair_a, a[i]}, {pair_b, b[i]}}));
  }
  assign_parallel_results({&engine_a, &engine_b, &engine_c}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair_a);
  auto second_entry = cfg.pass_config.policy_snapshot.support_contacts.front();
  second_entry.pair = pair_b;
  cfg.pass_config.policy_snapshot.support_contacts.push_back(second_entry);
  cfg.worker_count = 3;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));
  CollisionCancellationToken parallel_cancel;
  const auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b, &engine_c},
      trajectory, parallel_cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  const auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine,
                                                             trajectory, serial_cancel);

  expect_policy_equivalent(parallel, serial);
  EXPECT_TRUE(parallel.decision.accepted);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedSupportStateHonorsTouchLinkPrecedenceExactlyLikeSerial) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{-0.04, -0.03, 0.002, -0.02, 0.003, 0.004};
  std::vector<CollisionResult> results;
  for (const double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  assign_parallel_results({&engine_a, &engine_b}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.pass_config.policy_snapshot.support_contacts.front().allow_recontact = true;
  cfg.pass_config.policy_snapshot.allowed_touch_link_pairs.push_back(pair);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 2;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));
  CollisionCancellationToken parallel_cancel;
  const auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b}, trajectory,
      parallel_cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  const auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine,
                                                             trajectory, serial_cancel);

  expect_policy_equivalent(parallel, serial);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseCancellationIsFailClosedAndDoesNotRunSummaryMerge) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  const CollisionPairId pair{"gripper", "object"};
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  cancel.request_cancel("operator_stop");
  const auto bundle = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b},
      long_trajectory(8), cancel);

  EXPECT_TRUE(bundle.cancelled);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(bundle.cancellation_reason, std::optional<std::string>{"operator_stop"});
  EXPECT_TRUE(pool.last_telemetry().ordered_contact_summary_used);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseMidChunkCancellationMatchesSerialPrefix) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine engine_c;
  MockCollisionQueryEngine serial_engine;
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{-0.05,  -0.04, -0.03, -0.02, -0.015, -0.01,
                                       -0.005, 0.001, 0.002, 0.003, 0.004,  0.005};
  std::vector<CollisionResult> results;
  for (const double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  results[5] = cancelled_result("operator_stop");
  assign_parallel_results({&engine_a, &engine_b, &engine_c}, results);
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }

  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 3;
  cfg.min_waypoints_for_parallel = 2;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));
  CollisionCancellationToken parallel_cancel;
  const auto parallel = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b, &engine_c},
      trajectory, parallel_cancel);
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  const auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine,
                                                             trajectory, serial_cancel);

  EXPECT_EQ(parallel.cancelled, serial.cancelled);
  EXPECT_EQ(parallel.cancellation_reason, serial.cancellation_reason);
  EXPECT_EQ(parallel.cancelled_at_waypoint_index, serial.cancelled_at_waypoint_index);
  EXPECT_EQ(parallel.decision.action, serial.decision.action);
  EXPECT_EQ(parallel.decision.first_failing_waypoint, serial.decision.first_failing_waypoint);
  ASSERT_EQ(parallel.waypoint_records.size(), serial.waypoint_records.size());
  ASSERT_FALSE(parallel.waypoint_records.empty());
  EXPECT_EQ(parallel.waypoint_records.back().waypoint_index, 5);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseIsIndependentOfWorkerCompletionOrder) {
  const CollisionPairId pair{"gripper", "object"};
  const std::vector<double> clearances{-0.05, -0.04, -0.03, -0.02, -0.03, -0.02,
                                       -0.01, 0.001, 0.002, 0.003, 0.004, 0.005};
  std::vector<CollisionResult> results;
  for (const double clearance : clearances) {
    results.push_back(contact_result(pair, clearance));
  }
  MockCollisionQueryEngine serial_engine;
  for (std::size_t i = 0; i < results.size(); ++i) {
    serial_engine.result_by_waypoint[static_cast<std::int64_t>(i)] = results[i];
  }
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 3;
  cfg.min_waypoints_for_parallel = 2;
  const auto trajectory = long_trajectory(static_cast<int>(results.size()));
  CollisionValidationCoordinator serial_coordinator(cfg.pass_config);
  CollisionCancellationToken serial_cancel;
  const auto serial = serial_coordinator.validate_trajectory(test_snapshot(), serial_engine,
                                                             trajectory, serial_cancel);

  const std::array<std::array<int, 3>, 6> completion_orders{{
      {{0, 1, 2}},
      {{0, 2, 1}},
      {{1, 0, 2}},
      {{1, 2, 0}},
      {{2, 0, 1}},
      {{2, 1, 0}},
  }};
  for (const auto& order : completion_orders) {
    SCOPED_TRACE("completion_order=" + std::to_string(order[0]) + std::to_string(order[1]) +
                 std::to_string(order[2]));
    MockCollisionQueryEngine engine_a;
    MockCollisionQueryEngine engine_b;
    MockCollisionQueryEngine engine_c;
    std::array<MockCollisionQueryEngine*, 3> engines{&engine_a, &engine_b, &engine_c};
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
      engines[static_cast<std::size_t>(order[rank])]->check_delay =
          std::chrono::microseconds(static_cast<int>(rank) * 50);
    }
    assign_parallel_results({engines[0], engines[1], engines[2]}, results);
    ParallelCollisionValidationWorkerPool pool(cfg);
    CollisionCancellationToken cancel;
    const auto parallel = pool.validate_trajectory(
        test_snapshot(), std::vector<ICollisionQueryEngine*>{engines[0], engines[1], engines[2]},
        trajectory, cancel);
    expect_policy_equivalent(parallel, serial);
  }
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     OrderedContactReleaseWorkerExceptionPreservesStructuredEngineFailure) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  const CollisionPairId pair{"gripper", "object"};
  engine_b.throw_on_check = true;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  const auto bundle = pool.validate_trajectory(
      test_snapshot(), std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b},
      long_trajectory(8), cancel);

  ASSERT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  const auto failure =
      std::find_if(bundle.waypoint_records.begin(), bundle.waypoint_records.end(),
                   [](const CollisionValidationWaypointRecord& record) {
                     const auto it = record.result.raw_diagnostics.find("reason_code");
                     return it != record.result.raw_diagnostics.end() &&
                            it->second == reason_codes::kCollisionValidationWorkerException;
                   });
  ASSERT_NE(failure, bundle.waypoint_records.end());
  EXPECT_EQ(failure->waypoint_index, 4);
  EXPECT_EQ(failure->result.raw_diagnostics.count("ordered_contact_summary_error"), 0u);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     StatefulUnsupportedModesAndIdentityBoundsFallBackSerially) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  const CollisionPairId pair{"gripper", "object"};

  auto run_with_policy = [&](CollisionPolicySnapshot policy, bool summaries_enabled) {
    ParallelCollisionValidationWorkerPool::Config cfg;
    cfg.pass_config.policy_snapshot = std::move(policy);
    cfg.worker_count = 2;
    cfg.min_waypoints_for_parallel = 1;
    cfg.enable_ordered_support_contact_summaries = summaries_enabled;
    ParallelCollisionValidationWorkerPool pool(cfg);
    CollisionCancellationToken cancel;
    (void)pool.validate_trajectory(test_snapshot(),
                                   std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b},
                                   long_trajectory(8), cancel);
    return pool.last_telemetry().fallback_reason;
  };

  auto process_policy = contact_release_snapshot(pair);
  process_policy.support_contacts.front().mode = CollisionContactPolicyMode::kContactProcess;
  EXPECT_EQ(run_with_policy(std::move(process_policy), true), "contact_process_policy_active");
  EXPECT_EQ(run_with_policy(contact_release_snapshot(pair), false),
            "support_contact_policy_active");

  auto over_bound = contact_release_snapshot(pair);
  over_bound.support_contacts.clear();
  for (std::size_t i = 0; i <= kMaxOrderedSupportContactIdentities; ++i) {
    SupportContactPolicyEntry entry;
    entry.pair = {"robot_" + std::to_string(i), "object_" + std::to_string(i)};
    over_bound.support_contacts.push_back(std::move(entry));
  }
  EXPECT_EQ(run_with_policy(std::move(over_bound), true),
            "support_contact_identity_bound_exceeded");

  auto duplicate = contact_release_snapshot(pair);
  duplicate.support_contacts.push_back(duplicate.support_contacts.front());
  EXPECT_EQ(run_with_policy(std::move(duplicate), true), "duplicate_support_contact_identity");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     EffectiveCapacityRemainsVisibleWhenTrajectoryUsesFewerWorkers) {
  MockCollisionQueryEngine engine_a;
  MockCollisionQueryEngine engine_b;
  MockCollisionQueryEngine engine_c;
  MockCollisionQueryEngine engine_d;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 4;
  cfg.min_waypoints_for_parallel = 2;
  ParallelCollisionValidationWorkerPool pool(cfg);
  CollisionCancellationToken cancel;
  (void)pool.validate_trajectory(
      test_snapshot(),
      std::vector<ICollisionQueryEngine*>{&engine_a, &engine_b, &engine_c, &engine_d},
      long_trajectory(3), cancel);

  EXPECT_EQ(pool.last_telemetry().workers_requested, 4);
  EXPECT_EQ(pool.last_telemetry().workers_effective, 4);
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 3);
  EXPECT_EQ(pool.last_telemetry().per_worker_waypoint_counts, (std::vector<int>{1, 1, 1}));
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

TEST(ParallelCollisionValidationWorkerPoolTest,
     ParallelValidationWallTimeoutReturnsFailClosedAndAbandonsWorkers) {
  auto engine_a = std::make_shared<MockCollisionQueryEngine>();
  auto engine_b = std::make_shared<MockCollisionQueryEngine>();
  engine_a->block_on_check = true;

  ParallelCollisionValidationWorkerPool::Config cfg;
  FakeClock clock;
  ManualScheduler scheduler;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 4;
  cfg.validation_wall_timeout = std::chrono::milliseconds(20);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);

  auto cancel = std::make_shared<CollisionCancellationToken>();
  std::vector<std::shared_ptr<ICollisionQueryEngine>> engines{engine_a, engine_b};
  std::optional<CollisionValidationDecisionBundle> bundle;
  std::thread caller([&]() {
    bundle = pool.validate_trajectory(test_snapshot(), engines, long_trajectory(12), cancel);
  });
  engine_a->wait_until_blocked();
  EXPECT_EQ(scheduler.last_priority(), SchedulerPriority::Monitoring);
  clock.advance(20'000'000);
  scheduler.advance_to(clock.now());
  caller.join();

  ASSERT_TRUE(bundle.has_value());
  EXPECT_FALSE(bundle->decision.accepted);
  EXPECT_EQ(bundle->decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_TRUE(bundle->cancelled);
  ASSERT_TRUE(bundle->cancellation_reason.has_value());
  EXPECT_EQ(*bundle->cancellation_reason, "collision_validation_wall_timeout");
  ASSERT_FALSE(bundle->waypoint_records.empty());
  EXPECT_EQ(bundle->waypoint_records.front().result.raw_diagnostics.at("elapsed_ns"), "20000000");
  EXPECT_TRUE(cancel->is_cancel_requested());
  EXPECT_EQ(cancel->reason(), "collision_validation_wall_timeout");
  EXPECT_EQ(pool.last_telemetry().fallback_reason,
            "validation_wall_timeout");
  const auto status = pool.status();
  EXPECT_EQ(status.queued_jobs, 0u);
  EXPECT_GE(status.abandoned_jobs_total, 1u);

  engine_a->release_blocked();
  pool.detach();
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TimedOneWorkerFallbackHonorsEnteringReleasedState) {
  const CollisionPairId pair{"gripper", "object"};
  auto engine = std::make_shared<MockCollisionQueryEngine>();
  engine->result_by_waypoint[0] = contact_result(pair, -0.01);
  engine->result_by_waypoint[1] = clear_result();

  FakeClock clock;
  ManualScheduler scheduler;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  auto& release = cfg.pass_config.policy_snapshot.support_contacts.front();
  release.require_separation_by_end = false;
  release.require_strictly_decreasing_penetration = false;
  release.min_release_progress_m = 0.0;
  cfg.pass_config.entering_support_contact_state =
      SupportContactEvaluationStateMap{
          {pair, SupportContactEvaluationState::kSeparated}};
  ContactReleaseEvaluationState entering_release;
  entering_release.initialized = true;
  entering_release.started_in_contact = true;
  entering_release.in_contact = false;
  entering_release.separated = true;
  entering_release.initial_depth_m = 0.05;
  entering_release.previous_depth_m = 0.02;
  entering_release.best_depth_m = 0.0;
  cfg.pass_config.entering_contact_release_state =
      ContactReleaseEvaluationStateMap{{pair, entering_release}};
  cfg.worker_count = 1;
  cfg.validation_wall_timeout = std::chrono::milliseconds(100);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);

  auto cancel = std::make_shared<CollisionCancellationToken>();
  const auto bundle = pool.validate_trajectory(
      std::make_shared<const CollisionWorldSnapshot>(test_snapshot()),
      std::vector<std::shared_ptr<ICollisionQueryEngine>>{engine},
      long_trajectory(2), cancel, cfg.pass_config);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_EQ(bundle.decision.first_failing_waypoint,
            std::optional<std::int64_t>{0});
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_release_violation"),
            "recontact_after_release");
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "worker_count_one");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TimedShortPrefixFallbackHonorsEnteringPenetrationHistory) {
  const CollisionPairId pair{"gripper", "object"};
  auto engine_a = std::make_shared<MockCollisionQueryEngine>();
  auto engine_b = std::make_shared<MockCollisionQueryEngine>();
  engine_a->result_by_waypoint[0] = contact_result(pair, -0.04);
  engine_a->result_by_waypoint[1] = contact_result(pair, -0.02);

  FakeClock clock;
  ManualScheduler scheduler;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot = contact_release_snapshot(pair);
  cfg.pass_config.enforce_contact_release_trajectory_end = false;
  cfg.pass_config.entering_support_contact_state =
      SupportContactEvaluationStateMap{
          {pair, SupportContactEvaluationState::kContactActive}};
  ContactReleaseEvaluationState entering_release;
  entering_release.initialized = true;
  entering_release.started_in_contact = true;
  entering_release.in_contact = true;
  entering_release.initial_depth_m = 0.05;
  entering_release.previous_depth_m = 0.03;
  entering_release.best_depth_m = 0.03;
  cfg.pass_config.entering_contact_release_state =
      ContactReleaseEvaluationStateMap{{pair, entering_release}};
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 8;
  cfg.validation_wall_timeout = std::chrono::milliseconds(100);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);

  auto cancel = std::make_shared<CollisionCancellationToken>();
  const auto bundle = pool.validate_trajectory(
      std::make_shared<const CollisionWorldSnapshot>(test_snapshot()),
      std::vector<std::shared_ptr<ICollisionQueryEngine>>{engine_a, engine_b},
      long_trajectory(2), cancel, cfg.pass_config);

  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kReject);
  ASSERT_EQ(bundle.decision.first_failing_waypoint,
            std::optional<std::int64_t>{0});
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.raw_diagnostics.at("contact_release_violation"),
            "penetration_increased");
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "trajectory_too_short");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TimedBoundedFallbackAllowsFirstAppearanceReappearanceAndFinalContact) {
  const CollisionPairId pair{"gripper", "object"};
  auto engine = std::make_shared<MockCollisionQueryEngine>();
  // No entering contact exists. The configured pair first appears after a
  // clear sample, disappears, reappears, and remains colliding at the final
  // sample. Bounded mode permits all four transitions while every collision
  // remains within the authored cap.
  engine->result_by_waypoint[0] = clear_result();
  engine->result_by_waypoint[1] = contact_result(pair, -0.004);
  engine->result_by_waypoint[2] = clear_result();
  engine->result_by_waypoint[3] = contact_result(pair, -0.003);

  FakeClock clock;
  ManualScheduler scheduler;
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.pass_config.policy_snapshot =
      bounded_penetration_snapshot(pair, 0.005);
  cfg.worker_count = 1;
  cfg.validation_wall_timeout = std::chrono::milliseconds(100);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);

  auto cancel = std::make_shared<CollisionCancellationToken>();
  const auto bundle = pool.validate_trajectory(
      std::make_shared<const CollisionWorldSnapshot>(test_snapshot()),
      std::vector<std::shared_ptr<ICollisionQueryEngine>>{engine},
      long_trajectory(4), cancel, cfg.pass_config);

  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "worker_count_one");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     SerialFallbackValidationWallTimeoutReturnsFailClosed) {
  auto engine = std::make_shared<MockCollisionQueryEngine>();
  engine->block_on_check = true;

  ParallelCollisionValidationWorkerPool::Config cfg;
  FakeClock clock;
  ManualScheduler scheduler;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 1;
  cfg.min_waypoints_for_parallel = 4;
  cfg.validation_wall_timeout = std::chrono::milliseconds(20);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);

  auto cancel = std::make_shared<CollisionCancellationToken>();
  std::vector<std::shared_ptr<ICollisionQueryEngine>> engines{engine};
  std::optional<CollisionValidationDecisionBundle> bundle;
  std::thread caller([&]() {
    bundle = pool.validate_trajectory(test_snapshot(), engines, long_trajectory(12), cancel);
  });
  engine->wait_until_blocked();
  clock.advance(20'000'000);
  scheduler.advance_to(clock.now());
  caller.join();

  ASSERT_TRUE(bundle.has_value());
  EXPECT_FALSE(bundle->decision.accepted);
  EXPECT_EQ(bundle->decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_TRUE(bundle->cancelled);
  ASSERT_TRUE(bundle->cancellation_reason.has_value());
  EXPECT_EQ(*bundle->cancellation_reason, "collision_validation_wall_timeout");
  ASSERT_FALSE(bundle->waypoint_records.empty());
  EXPECT_EQ(bundle->waypoint_records.front().result.raw_diagnostics.at("elapsed_ns"), "20000000");
  EXPECT_TRUE(cancel->is_cancel_requested());
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 1);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "validation_wall_timeout");
  EXPECT_EQ(pool.last_telemetry().total_worker_wall_ns, 0);
  EXPECT_EQ(pool.last_telemetry().per_worker_waypoint_counts, (std::vector<int>{0}));
  const auto status = pool.status();
  EXPECT_EQ(status.queued_jobs, 0u);
  EXPECT_EQ(status.abandoned_jobs_total, 1u);

  engine->release_blocked();
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     AbsoluteDeadlineIncludesCallerWorkAndDoesNotStartAQueryWhenExpired) {
  auto engine = std::make_shared<MockCollisionQueryEngine>();

  ParallelCollisionValidationWorkerPool::Config cfg;
  FakeClock clock;
  ManualScheduler scheduler;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 1;
  cfg.min_waypoints_for_parallel = 4;
  cfg.validation_wall_timeout = std::chrono::milliseconds(20);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);

  clock.set(10'000'000);
  auto cancel = std::make_shared<CollisionCancellationToken>();
  const std::vector<std::shared_ptr<ICollisionQueryEngine>> engines{engine};
  const auto snapshot = std::make_shared<const CollisionWorldSnapshot>(test_snapshot());
  const auto bundle =
      pool.validate_trajectory(snapshot, engines, long_trajectory(12), cancel, cfg.pass_config,
                               /*absolute_wall_deadline=*/5'000'000);

  EXPECT_EQ(engine->call_count.load(), 0);
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_TRUE(bundle.cancelled);
  ASSERT_TRUE(bundle.cancellation_reason.has_value());
  EXPECT_EQ(*bundle.cancellation_reason, "collision_validation_wall_timeout");
  ASSERT_FALSE(bundle.waypoint_records.empty());
  EXPECT_EQ(
      bundle.waypoint_records.front().result.raw_diagnostics.at("timeout_ns"),
      "0");
  EXPECT_TRUE(cancel->is_cancel_requested());
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 0);
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "validation_wall_timeout");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     TimeoutDoesNotReuseSingleContextUntilAbandonedWorkerIsJoined) {
  auto engine_a = std::make_shared<MockCollisionQueryEngine>();
  auto engine_b = std::make_shared<MockCollisionQueryEngine>();
  engine_a->block_on_check = true;

  ParallelCollisionValidationWorkerPool::Config cfg;
  FakeClock clock;
  ManualScheduler scheduler;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 2;
  cfg.min_waypoints_for_parallel = 1;
  cfg.validation_wall_timeout = std::chrono::milliseconds(20);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);
  const std::vector<std::shared_ptr<ICollisionQueryEngine>> engines{engine_a, engine_b};
  const auto snapshot = std::make_shared<const CollisionWorldSnapshot>(test_snapshot());

  auto first_cancel = std::make_shared<CollisionCancellationToken>();
  std::optional<CollisionValidationDecisionBundle> first;
  std::thread caller([&]() {
    first = pool.validate_trajectory(snapshot, engines, long_trajectory(12), first_cancel,
                                     cfg.pass_config);
  });
  engine_a->wait_until_blocked();
  clock.advance(20'000'000);
  scheduler.advance_to(clock.now());
  caller.join();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->cancelled);
  const int calls_while_blocked = engine_a->call_count.load() + engine_b->call_count.load();

  auto retry_cancel = std::make_shared<CollisionCancellationToken>();
  const auto retry = pool.validate_trajectory(snapshot, engines, long_trajectory(12), retry_cancel,
                                              cfg.pass_config);
  EXPECT_FALSE(retry.decision.accepted);
  EXPECT_EQ(retry.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_FALSE(retry.waypoint_records.empty());
  EXPECT_EQ(retry.waypoint_records.front().result.raw_diagnostics.at("reason_code"),
            "collision_validation_workers_draining_after_timeout");
  EXPECT_EQ(engine_a->call_count.load() + engine_b->call_count.load(), calls_while_blocked);
  EXPECT_FALSE(retry_cancel->is_cancel_requested());
  EXPECT_EQ(pool.last_telemetry().fallback_reason, "validation_workers_draining_after_timeout");

  engine_a->release_blocked();
  const auto released_status = wait_for_pool_status(pool, [](const auto& current) {
    return current.active_workers == 0;
  });
  ASSERT_EQ(released_status.active_workers, 0u);

  auto recovered_cancel = std::make_shared<CollisionCancellationToken>();
  const auto recovered = pool.validate_trajectory(snapshot, engines, long_trajectory(12),
                                                  recovered_cancel, cfg.pass_config);
  EXPECT_TRUE(recovered.decision.accepted);
  EXPECT_GT(engine_a->call_count.load() + engine_b->call_count.load(), calls_while_blocked);
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     RawPointerWallTimeoutFailsClosedWithoutBorrowedWorker) {
  auto engine = std::make_shared<MockCollisionQueryEngine>();
  engine->block_on_check = true;

  ParallelCollisionValidationWorkerPool::Config cfg;
  FakeClock clock;
  ManualScheduler scheduler;
  cfg.pass_config.policy_snapshot = freespace_snapshot();
  cfg.worker_count = 1;
  cfg.validation_wall_timeout = std::chrono::milliseconds(20);
  cfg.clock = &clock;
  cfg.scheduler = &scheduler;
  ParallelCollisionValidationWorkerPool pool(cfg);

  CollisionCancellationToken cancel;
  std::vector<ICollisionQueryEngine*> engines{engine.get()};
  auto bundle = pool.validate_trajectory(test_snapshot(), engines,
                                         long_trajectory(12), cancel);

  EXPECT_EQ(engine->call_count.load(), 0);
  EXPECT_FALSE(cancel.is_cancel_requested());
  EXPECT_FALSE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kInconclusive);
  ASSERT_FALSE(bundle.waypoint_records.empty());
  EXPECT_EQ(
      bundle.waypoint_records.front().result.raw_diagnostics.at("reason_code"),
      "collision_validation_wall_timeout_requires_owned_lifetimes");
  EXPECT_EQ(pool.last_telemetry().workers_actually_used, 0);
  EXPECT_EQ(pool.last_telemetry().fallback_reason,
            "validation_wall_timeout_requires_owned_lifetimes");
}

TEST(ParallelCollisionValidationWorkerPoolTest,
     PositiveWallTimeoutRequiresInjectedDeadlineAuthority) {
  ParallelCollisionValidationWorkerPool::Config cfg;
  cfg.validation_wall_timeout = std::chrono::milliseconds(20);
  EXPECT_THROW(ParallelCollisionValidationWorkerPool pool(cfg), std::invalid_argument);
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

TEST(EnvValidationCollisionWorkers, ExplicitDeploymentValueCannotBeOverriddenByEnvironment) {
  ::setenv("MANYFORGE_VALIDATION_COLLISION_WORKERS", "7", 1);
  EXPECT_EQ(resolve_validation_collision_workers(
                /*configured_value=*/3,
                /*deployment_declared=*/true),
            3);
  EXPECT_EQ(resolve_validation_collision_workers(
                /*configured_value=*/3,
                /*deployment_declared=*/false),
            7);
  ::unsetenv("MANYFORGE_VALIDATION_COLLISION_WORKERS");
}

TEST(EnvValidationCollisionWorkers, InvalidExplicitDeploymentValueFailsInsteadOfClamping) {
  EXPECT_THROW(resolve_validation_collision_workers(
                   /*configured_value=*/0,
                   /*deployment_declared=*/true),
               std::invalid_argument);
  EXPECT_THROW(resolve_validation_collision_workers(
                   /*configured_value=*/65,
                   /*deployment_declared=*/true),
               std::invalid_argument);
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
