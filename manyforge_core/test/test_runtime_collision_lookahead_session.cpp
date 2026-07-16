#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/runtime_collision_lookahead_session.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

class TestPublisher final : public ICollisionWorldSnapshotPublisher {
 public:
  CollisionWorldSnapshotPtr snapshot() const override { return bundle ? bundle->world : nullptr; }
  RuntimeCollisionQueryBundlePtr published_runtime_collision_query_bundle() const override {
    std::lock_guard<std::mutex> lock(mutex);
    return bundle;
  }

  mutable std::mutex mutex;
  RuntimeCollisionQueryBundlePtr bundle;
};

class TestLookaheadEngine final : public ICollisionQueryEngine {
 public:
  explicit TestLookaheadEngine(std::string context_id) {
    capability_.engine_id = "lookahead_test";
    capability_.engine_version = "test";
    capability_.library_lineage = "unit_test";
    capability_.thread_safety = CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.mutable_context_id = std::move(context_id);
    capability_.deterministic_ordering = true;
  }

  const CollisionEngineCapabilityDescriptor& capability() const override { return capability_; }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const CollisionWorldSnapshot&) noexcept override {
    prepare_calls.fetch_add(1, std::memory_order_relaxed);
    return prepare_status;
  }

  CollisionResult check(const CollisionWorldSnapshot& snapshot, const CollisionQuery& query,
                        const CollisionPolicySnapshot&,
                        const CollisionCancellationToken&) override {
    check_calls.fetch_add(1, std::memory_order_relaxed);
    if (clock_to_advance != nullptr && simulated_query_duration_ns > 0) {
      clock_to_advance->advance(simulated_query_duration_ns);
    }
    if (forced_result.has_value()) {
      auto result = *forced_result;
      result.query_id = query.query_id;
      result.surface = query.surface;
      result.state_time_ns = query.state_time_ns;
      result.collision_world_revision = snapshot.revision;
      result.engine_id = capability_.engine_id;
      result.engine_version = capability_.engine_version;
      if (on_check) {
        on_check();
      }
      return result;
    }
    CollisionResult result;
    result.query_id = query.query_id;
    result.surface = query.surface;
    result.state_time_ns = query.state_time_ns;
    result.collision_world_revision = snapshot.revision;
    result.engine_id = capability_.engine_id;
    result.engine_version = capability_.engine_version;
    result.engine_status = CollisionEngineStatus::kOk;
    if (query.timeout_budget_ns && simulated_query_duration_ns > *query.timeout_budget_ns) {
      result.engine_status = CollisionEngineStatus::kTimeout;
      result.decision = CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["simulated_query_duration_ns"] =
          std::to_string(simulated_query_duration_ns);
      if (on_check) {
        on_check();
      }
      return result;
    }
    if (collision_local_waypoint && query.waypoint_index == collision_local_waypoint) {
      result.in_collision = true;
      result.normalized_pairs = {collision_pair};
      result.pair_clearances = {CollisionPairClearance{collision_pair, collision_clearance_m}};
      result.min_clearance_m = collision_clearance_m;
      result.min_clearance_pair = collision_pair;
      result.decision = CollisionDecisionResult::kFail;
    } else {
      result.decision = CollisionDecisionResult::kPass;
    }
    if (on_check) {
      on_check();
    }
    return result;
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override { return capability_.engine_version; }

  CollisionEngineCapabilityDescriptor capability_;
  std::optional<std::int64_t> collision_local_waypoint;
  CollisionPairId collision_pair{"robot", "obstacle"};
  double collision_clearance_m = -0.001;
  std::optional<CollisionResult> forced_result;
  std::function<void()> on_check;
  std::atomic<std::uint64_t> prepare_calls{0};
  std::atomic<std::uint64_t> check_calls{0};
  FakeClock* clock_to_advance = nullptr;
  std::int64_t simulated_query_duration_ns = 0;
  RuntimeCollisionQueryPrepareStatus prepare_status =
      RuntimeCollisionQueryPrepareStatus::kUnsupported;
};

TrajectoryDTO trajectory() {
  TrajectoryDTO value;
  value.joint_names = {"j1"};
  value.waypoints = {
      TrajectoryWaypointDTO{0.0, {0.0}, {}, {}}, TrajectoryWaypointDTO{0.1, {0.1}, {}, {}},
      TrajectoryWaypointDTO{0.2, {0.2}, {}, {}}, TrajectoryWaypointDTO{0.3, {0.3}, {}, {}},
      TrajectoryWaypointDTO{0.4, {0.4}, {}, {}},
  };
  return value;
}

RuntimeCollisionQueryBundlePtr make_bundle(std::uint64_t epoch, std::uint64_t revision,
                                           CollisionPolicySnapshot policy) {
  auto world = std::make_shared<CollisionWorldSnapshot>();
  world->revision = {revision, "world-" + std::to_string(revision)};
  world->snapshot_format_version = 1;
  return build_runtime_collision_query_bundle(
      CollisionWorldPolicySnapshotBundle{world, std::move(policy)}, epoch);
}

RuntimeCollisionQueryBundlePtr make_bundle(std::uint64_t epoch, std::uint64_t revision) {
  CollisionPolicySnapshot policy;
  policy.policy_id = "free";
  policy.revision = {1, "policy"};
  return make_bundle(epoch, revision, std::move(policy));
}

RuntimeCollisionQueryBundlePtr make_dynamic_bundle(
    std::uint64_t epoch, std::uint64_t revision,
    std::int64_t source_timestamp_ns, std::int64_t freshness_budget_ns) {
  auto base = make_bundle(epoch, revision);
  auto world = std::make_shared<CollisionWorldSnapshot>(*base->world);
  CollisionWorldBody body;
  body.body_id = "object/dynamic_box/shape/0";
  body.owner_kind = CollisionOwnerKind::kDetachedObject;
  body.frame_id = "universe";
  body.pose.orientation.w = 1.0;
  body.source_id = "vision";
  body.source_timestamp_ns = source_timestamp_ns;
  body.source_freshness_budget_ns = freshness_budget_ns;
  world->bodies.push_back(body);
  world->body_count = 1;

  auto bundle = std::make_shared<RuntimeCollisionQueryBundle>(*base);
  bundle->world = std::move(world);
  bundle->body_freshness = {CollisionWorldBodyFreshness{
      body.body_id, body.source_id, source_timestamp_ns,
      freshness_budget_ns}};
  bundle->lookup_order_valid = true;
  return bundle;
}

struct ResultSink {
  void push(RuntimeCollisionLookaheadResult value) {
    std::lock_guard<std::mutex> lock(mutex);
    result = std::move(value);
    cv.notify_all();
  }

  RuntimeCollisionLookaheadResult wait() {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(2), [this]() { return result.has_value(); })) {
      ADD_FAILURE() << "lookahead result callback timed out";
      return {};
    }
    return *result;
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::optional<RuntimeCollisionLookaheadResult> result;
};

struct ResultHistory {
  void push(RuntimeCollisionLookaheadResult value) {
    std::lock_guard<std::mutex> lock(mutex);
    results.push_back(std::move(value));
    cv.notify_all();
  }

  bool wait_for_size(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2),
                       [&]() { return results.size() >= count; });
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<RuntimeCollisionLookaheadResult> results;
};

RuntimeCollisionLookaheadSession::AuthorityCallback accept_test_authority() {
  return [](const RuntimeCollisionLookaheadSession*,
            RuntimeCollisionLookaheadResult&) {
    return RuntimeCollisionLookaheadAuthorityDisposition::kAccepted;
  };
}

TrajectoryDTO dense_trajectory(std::size_t waypoint_count,
                               double step_s = 0.01) {
  TrajectoryDTO value;
  value.joint_names = {"j1"};
  value.waypoints.reserve(waypoint_count);
  for (std::size_t index = 0; index < waypoint_count; ++index) {
    const auto time_s = static_cast<double>(index) * step_s;
    value.waypoints.push_back(
        TrajectoryWaypointDTO{time_s, {time_s}, {}, {}});
  }
  return value;
}

bool request_until_started(RuntimeCollisionLookaheadSession& session, FakeClock& clock,
                           TestPublisher& publisher) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status = session.status();
    if (status.jobs_started_total > 0) {
      return true;
    }
    if (!status.permit_pending) {
      session.request(clock.now(), publisher.published_runtime_collision_query_bundle(), {});
    }
    // request() is non-blocking by contract. If its try-lock races the worker
    // parking, the next immediate clear tick retries the latest request instead
    // of making the runtime-safety thread wait.
    std::this_thread::yield();
  }
  return false;
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     PreservesFittingSourceKnotsAndSamplesTheExactTimeHorizon) {
  const auto fitting =
      select_runtime_collision_lookahead_samples(trajectory(), 50'000'000, 200'000'000, 3);
  ASSERT_EQ(fitting.termination, RuntimeCollisionLookaheadTermination::kTimeHorizon);
  EXPECT_FALSE(fitting.sampling_reduced);
  EXPECT_EQ(fitting.eligible_source_waypoints, 2u);
  EXPECT_EQ(fitting.sample_times_ns,
            (std::vector<std::int64_t>{100'000'000, 200'000'000, 250'000'000}));
  EXPECT_EQ(fitting.source_waypoint_indices,
            (std::vector<std::int64_t>{1, 2, -1}));
  ASSERT_EQ(fitting.original_trajectory_sample_fractions.size(), 3u);
  EXPECT_DOUBLE_EQ(fitting.original_trajectory_sample_fractions[0], 0.25);
  EXPECT_DOUBLE_EQ(fitting.original_trajectory_sample_fractions[1], 0.5);
  EXPECT_DOUBLE_EQ(fitting.original_trajectory_sample_fractions[2], 0.625);
  EXPECT_EQ(fitting.covered_until_time_ns, 250'000'000);

  const auto reduced =
      select_runtime_collision_lookahead_samples(trajectory(), 50'000'000, 300'000'000, 2);
  EXPECT_EQ(reduced.termination, RuntimeCollisionLookaheadTermination::kTimeHorizon);
  EXPECT_TRUE(reduced.sampling_reduced);
  EXPECT_EQ(reduced.sample_times_ns,
            (std::vector<std::int64_t>{125'000'000, 350'000'000}));
  EXPECT_EQ(reduced.source_waypoint_indices,
            (std::vector<std::int64_t>{-1, -1}));
  EXPECT_EQ(reduced.covered_until_time_ns, 350'000'000);
}

TEST(RuntimeCollisionLookaheadSelectionTest, ReusedOutputDoesNotRetainSamplesFromThePreviousCursor) {
  RuntimeCollisionLookaheadSelection output;
  select_runtime_collision_lookahead_samples(trajectory(), 50'000'000, 200'000'000, 3, output);
  ASSERT_EQ(output.trajectory.waypoints.size(), 3u);

  select_runtime_collision_lookahead_samples(trajectory(), 350'000'000,
                                             200'000'000, 2, output);

  EXPECT_EQ(output.termination, RuntimeCollisionLookaheadTermination::kTrajectoryEnd);
  EXPECT_EQ(output.source_waypoint_indices,
            (std::vector<std::int64_t>{-1, 4}));
  ASSERT_EQ(output.trajectory.waypoints.size(), 2u);
  EXPECT_DOUBLE_EQ(output.trajectory.waypoints.back().time_from_start_s, 0.4);
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     SparseTrajectoryPreservesEverySourceKnotAndFillsUnusedCapacity) {
  const auto sparse = dense_trajectory(3, 1.0);
  const auto selection =
      select_runtime_collision_lookahead_samples(sparse, 0, 2'000'000'000, 8);

  ASSERT_EQ(selection.termination,
            RuntimeCollisionLookaheadTermination::kTrajectoryEnd);
  EXPECT_FALSE(selection.sampling_reduced);
  EXPECT_EQ(selection.eligible_source_waypoints, 2u);
  ASSERT_EQ(selection.sample_times_ns.size(), 8u);
  EXPECT_TRUE(std::binary_search(selection.sample_times_ns.begin(),
                                 selection.sample_times_ns.end(),
                                 1'000'000'000));
  EXPECT_EQ(selection.sample_times_ns.back(), 2'000'000'000);
  EXPECT_TRUE(std::adjacent_find(selection.sample_times_ns.begin(),
                                 selection.sample_times_ns.end()) ==
              selection.sample_times_ns.end());
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     SparseSourceKnotsOverlappingQuadraticTargetsStillFillTheSampleCap) {
  TrajectoryDTO sparse;
  sparse.joint_names = {"j1"};
  // These knots intentionally overlap the K=8 quadratic schedule that used
  // to supply too few distinct extras after mandatory-knot merging.
  for (const auto time_s : {0.0, 0.03125, 0.125, 0.28125, 2.0}) {
    sparse.waypoints.push_back(
        TrajectoryWaypointDTO{time_s, {time_s}, {}, {}});
  }

  const auto selection = select_runtime_collision_lookahead_samples(
      sparse, 0, 2'000'000'000, 8);

  ASSERT_EQ(selection.termination,
            RuntimeCollisionLookaheadTermination::kTrajectoryEnd);
  ASSERT_EQ(selection.sample_times_ns.size(), 8u);
  EXPECT_TRUE(std::adjacent_find(selection.sample_times_ns.begin(),
                                 selection.sample_times_ns.end()) ==
              selection.sample_times_ns.end());
  for (const auto required_ns :
       {31'250'000LL, 125'000'000LL, 281'250'000LL, 2'000'000'000LL}) {
    EXPECT_TRUE(std::binary_search(selection.sample_times_ns.begin(),
                                   selection.sample_times_ns.end(),
                                   required_ns));
  }
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     DenseTrajectoryUsesSixteenFrontLoadedSamplesThroughExactEndpoint) {
  const auto dense = dense_trajectory(101, 0.01);
  const auto selection =
      select_runtime_collision_lookahead_samples(dense, 0, 1'000'000'000, 16);

  ASSERT_EQ(selection.sample_times_ns.size(), 16u);
  EXPECT_TRUE(selection.sampling_reduced);
  EXPECT_EQ(selection.eligible_source_waypoints, 100u);
  EXPECT_EQ(selection.sample_times_ns.back(), 1'000'000'000);
  EXPECT_LT(selection.sample_times_ns[1] - selection.sample_times_ns[0],
            selection.sample_times_ns[15] - selection.sample_times_ns[14]);
  EXPECT_TRUE(std::is_sorted(selection.sample_times_ns.begin(),
                             selection.sample_times_ns.end()));
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     ExactSourceEndpointAndShortRemainderRetainTimeDefinedExtent) {
  const auto exact =
      select_runtime_collision_lookahead_samples(trajectory(), 0, 200'000'000, 2);
  EXPECT_EQ(exact.sample_times_ns,
            (std::vector<std::int64_t>{100'000'000, 200'000'000}));
  EXPECT_EQ(exact.source_waypoint_indices,
            (std::vector<std::int64_t>{1, 2}));

  const auto short_remainder = select_runtime_collision_lookahead_samples(
      trajectory(), 350'000'000, 2'000'000'000, 2);
  ASSERT_EQ(short_remainder.termination,
            RuntimeCollisionLookaheadTermination::kTrajectoryEnd);
  EXPECT_EQ(short_remainder.sample_times_ns.back(), 400'000'000);
  EXPECT_EQ(short_remainder.covered_until_time_ns, 400'000'000);
  EXPECT_TRUE(short_remainder.reaches_trajectory_end);
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     CursorBetweenKnotsNeverSelectsPastOrDuplicateTimes) {
  const auto selection = select_runtime_collision_lookahead_samples(
      trajectory(), 155'000'000, 175'000'000, 4);
  ASSERT_FALSE(selection.sample_times_ns.empty());
  EXPECT_GT(selection.sample_times_ns.front(), 155'000'000);
  EXPECT_EQ(selection.sample_times_ns.back(), 330'000'000);
  EXPECT_TRUE(std::adjacent_find(selection.sample_times_ns.begin(),
                                 selection.sample_times_ns.end()) ==
              selection.sample_times_ns.end());
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     MalformedAndNonFiniteTrajectoryInputsFailClosed) {
  auto nonfinite = trajectory();
  nonfinite.waypoints[2].positions_rad[0] =
      std::numeric_limits<double>::quiet_NaN();
  const auto invalid = select_runtime_collision_lookahead_samples(
      nonfinite, 0, 200'000'000, 4);
  EXPECT_EQ(invalid.termination,
            RuntimeCollisionLookaheadTermination::kInvalidInput);
  EXPECT_EQ(invalid.error_reason, "lookahead_trajectory_malformed");

  auto regressing = trajectory();
  regressing.waypoints[2].time_from_start_s = 0.05;
  EXPECT_EQ(select_runtime_collision_lookahead_samples(
                regressing, 0, 200'000'000, 4)
                .termination,
            RuntimeCollisionLookaheadTermination::kInvalidInput);

  const auto undersized_cap = select_runtime_collision_lookahead_samples(
      trajectory(), 0, 200'000'000, 1);
  EXPECT_EQ(undersized_cap.termination,
            RuntimeCollisionLookaheadTermination::kInvalidInput);
  EXPECT_EQ(undersized_cap.error_reason,
            "lookahead_selection_invalid_arguments");
}

TEST(RuntimeCollisionLookaheadSelectionTest,
     DenseOracleCanIdentifyAnIntentionallyUnexaminedSourceState) {
  const auto dense = dense_trajectory(101, 0.01);
  const auto selection =
      select_runtime_collision_lookahead_samples(dense, 0, 1'000'000'000, 8);
  ASSERT_TRUE(selection.sampling_reduced);
  const auto oracle_collision_time_ns = 500'000'000;
  EXPECT_FALSE(std::binary_search(selection.sample_times_ns.begin(),
                                  selection.sample_times_ns.end(),
                                  oracle_collision_time_ns));
  EXPECT_EQ(selection.covered_until_time_ns, 1'000'000'000);
  // The endpoint proves the sampled time extent only. This deliberately
  // unexamined dense-oracle state prevents a continuous-clearance claim.
}

TEST(RuntimeCollisionLookaheadSessionTest, MapsEarliestFailureBackToOriginalTrajectoryIndex) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  ASSERT_TRUE(progress.publish(3, 50'000'000, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-1");
  engine->collision_local_waypoint = 1;
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.horizon_ns = 300'000'000;
  config.max_samples = 4;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(42, 3, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kCollision);
  ASSERT_TRUE(result.first_failing_waypoint.has_value());
  EXPECT_EQ(*result.first_failing_waypoint, 2);
  ASSERT_TRUE(result.first_failing_pair.has_value());
  EXPECT_EQ(result.first_failing_pair->body_a, "obstacle");
  EXPECT_EQ(result.first_failing_pair->body_b, "robot");
}

TEST(RuntimeCollisionLookaheadSessionTest, PreparesChangedSnapshotOnceBeforeWorkerFanout) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  ASSERT_TRUE(progress.publish(3, 0, clock.now(), clock.now()).published);
  auto first_engine = std::make_shared<TestLookaheadEngine>("lookahead-prepare-1");
  auto second_engine = std::make_shared<TestLookaheadEngine>("lookahead-prepare-2");
  first_engine->prepare_status = RuntimeCollisionQueryPrepareStatus::kReady;
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 2;
  config.min_waypoints_for_parallel = 1;
  config.horizon_ns = 300'000'000;
  config.max_samples = 4;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {first_engine, second_engine},
      [&sink](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(42, 3, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kClear);
  EXPECT_EQ(result.workers_used, 2);
  EXPECT_EQ(first_engine->prepare_calls.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(second_engine->prepare_calls.load(std::memory_order_relaxed), 0u);
}

TEST(RuntimeCollisionLookaheadSessionTest, FailsClosedWhenSnapshotPreparationFails) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  ASSERT_TRUE(progress.publish(3, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-prepare-failure");
  engine->prepare_status = RuntimeCollisionQueryPrepareStatus::kEngineError;
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(42, 3, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kInconclusive);
  EXPECT_EQ(result.reason, "runtime_lookahead_snapshot_prepare_failed");
  EXPECT_EQ(result.failure_details.at("runtime_lookahead_prepare_status"), "engine_error");
  EXPECT_EQ(engine->prepare_calls.load(std::memory_order_relaxed), 1u);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     QueryTimeoutPreservesBoundedEngineDiagnostics) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(3, 50'000'000, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-timeout-details");
  CollisionResult timeout;
  timeout.engine_status = CollisionEngineStatus::kTimeout;
  timeout.decision = CollisionDecisionResult::kInconclusiveFail;
  timeout.raw_diagnostics = {
      {"snapshot_sync_duration_ns", "61000000"},
      {"timeout_budget_ns", "50000000"},
      {"timeout_includes_snapshot_sync", "false"},
  };
  engine->forced_result = std::move(timeout);
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*,
              RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(42, 3, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kInconclusive);
  EXPECT_EQ(result.reason, "runtime_lookahead_query_timeout");
  EXPECT_EQ(result.failure_details.at("collision_result_engine_status"),
            "timeout");
  EXPECT_EQ(result.failure_details.at(
                "collision_result_raw_timeout_includes_snapshot_sync"),
            "false");
  EXPECT_STREQ(runtime_collision_lookahead_error_message(result.reason),
               "Runtime trajectory lookahead collision query exceeded its time budget");
}

TEST(RuntimeCollisionLookaheadSessionTest,
     ContactGraspWindowRemainsBoundToTheCompleteAcceptedTrajectory) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  CollisionPolicySnapshot policy;
  policy.policy_id = "grasp";
  policy.revision = {1, "grasp-policy"};
  policy.require_full_diagnostics = true;
  ContactGraspPolicyEntry grasp;
  grasp.policy_id = "contact_grasp/part";
  grasp.selected_object_id = "part";
  grasp.allowed_robot_links = {"finger_left"};
  grasp.max_penetration_m = 0.003;
  grasp.terminal_window_start_fraction = 0.8;
  policy.contact_grasps.push_back(std::move(grasp));
  publisher.bundle = make_bundle(1, 10, std::move(policy));
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  ASSERT_TRUE(progress.publish(3, 50'000'000, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-grasp");
  engine->collision_local_waypoint = 2;
  engine->collision_pair = {"object/part/shape/0", "robot/arm/link/finger_left/collision/0"};
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.horizon_ns = 300'000'000;
  config.max_samples = 4;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(42, 3, std::make_shared<const TrajectoryDTO>(trajectory()), "contact_grasp/part");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kCollision);
  ASSERT_TRUE(result.first_failing_waypoint.has_value());
  EXPECT_EQ(*result.first_failing_waypoint, 3);
}

TEST(RuntimeCollisionLookaheadSessionTest, ClearResultCannotCertifyARevisionPublishedDuringTheJob) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-2");
  std::atomic<bool> replaced{false};
  engine->on_check = [&]() {
    if (!replaced.exchange(true)) {
      std::lock_guard<std::mutex> lock(publisher.mutex);
      publisher.bundle = make_bundle(2, 11);
    }
  };
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kSupersededRevision);
  EXPECT_EQ(result.reason, "runtime_lookahead_revision_superseded");
}

TEST(RuntimeCollisionLookaheadSessionTest,
     AuthorityBoundaryRejectsResultFromPreviousArmGeneration) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-generation");
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult) {},
      accept_test_authority());

  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  const auto first = session.status();
  ASSERT_TRUE(first.armed);
  EXPECT_TRUE(session.accept_result_for_current_arm(first.execution_id, first.execution_generation,
                                                    first.arm_generation,
                                                    first.permit_generation));

  session.arm(7, 10, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  const auto second = session.status();
  ASSERT_NE(second.arm_generation, first.arm_generation);
  EXPECT_FALSE(session.accept_result_for_current_arm(first.execution_id, first.execution_generation,
                                                     first.arm_generation,
                                                     first.permit_generation));
  EXPECT_TRUE(session.accept_result_for_current_arm(
      second.execution_id, second.execution_generation, second.arm_generation,
      second.permit_generation));
  EXPECT_EQ(session.status().stale_generation_results_total, 1u);
}

TEST(RuntimeCollisionLookaheadSessionTest, RejectsQueryBudgetLongerThanTheCycleBudget) {
  FakeClock clock;
  ManualScheduler scheduler;
  TestPublisher publisher;
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-timeout");
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.cycle_budget_ns = 49'000'000;
  config.query_budget_ns = 50'000'000;

  EXPECT_THROW(RuntimeCollisionLookaheadSession(
                   config, clock, scheduler, publisher, progress, {engine},
                   [](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult) {},
                   accept_test_authority()),
               std::invalid_argument);
}

TEST(RuntimeCollisionLookaheadSessionTest, RejectsMissingAuthorityCallback) {
  FakeClock clock;
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-no-authority");
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;

  EXPECT_THROW(
      RuntimeCollisionLookaheadSession(
          config, clock, scheduler, publisher, progress, {engine},
          [](const RuntimeCollisionLookaheadSession*,
             RuntimeCollisionLookaheadResult) {},
          {}),
      std::invalid_argument);
}

TEST(RuntimeCollisionLookaheadSessionTest, StatusReportsGenerationMatchedFreshCursorAuthority) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-status");
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult) {},
      accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");

  clock.advance(5'000'000);
  const auto status = session.status();
  EXPECT_TRUE(status.cursor_valid);
  EXPECT_EQ(status.cursor_source_kind, "backend_commanded");
  EXPECT_EQ(status.cursor_authority_profile, "isaac");
  EXPECT_EQ(status.cursor_age_ns, 5'000'000);

  progress.invalidate_generation(9);
  EXPECT_FALSE(session.status().cursor_valid);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     RearmSuppressesInFlightResultAndBusyRequestIsOnlyCoalesced) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(ExecutionProgressSourceKind::kBackendCommanded,
                                        ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                        "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool entered = false;
  bool release = false;
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-race");
  engine->on_check = [&]() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    if (entered) {
      return;
    }
    entered = true;
    gate_cv.notify_all();
    gate_cv.wait(lock, [&]() { return release; });
  };

  std::atomic<std::uint64_t> callbacks{0};
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.cycle_budget_ns = 10'000'000;
  config.query_budget_ns = 5'000'000;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&callbacks](const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2), [&]() { return entered; }));
  }
  const auto coalesced_before_busy_request = session.status().reruns_coalesced_total;
  clock.advance(config.cycle_budget_ns);
  session.request(clock.now(), publisher.bundle, {});
  EXPECT_EQ(session.status().jobs_started_total, 1u);
  EXPECT_EQ(session.status().reruns_coalesced_total, coalesced_before_busy_request + 1);

  ASSERT_TRUE(progress.publish(10, 0, clock.now(), clock.now()).published);
  session.arm(7, 10, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_cv.notify_all();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto status = session.status();
  while (status.job_active && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    status = session.status();
  }
  EXPECT_FALSE(status.job_active);
  EXPECT_GT(status.stale_generation_results_total, 0u);
  EXPECT_EQ(callbacks.load(std::memory_order_relaxed), 0u);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     WorkConservingCompletionImmediatelyConsumesTheNewestClearPermit) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool entered = false;
  bool release = false;
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-latest-permit");
  engine->on_check = [&]() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    if (entered) {
      return;
    }
    entered = true;
    gate_cv.notify_all();
    gate_cv.wait(lock, [&]() { return release; });
  };

  ResultHistory history;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&history](const RuntimeCollisionLookaheadSession*,
                 RuntimeCollisionLookaheadResult result) {
        history.push(std::move(result));
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2),
                                 [&]() { return entered; }));
  }

  {
    std::lock_guard<std::mutex> lock(publisher.mutex);
    publisher.bundle = make_bundle(2, 11);
  }
  clock.advance(1'000'000);
  ASSERT_TRUE(
      progress.publish(9, 100'000'000, clock.now(), clock.now()).published);
  session.request(clock.now(), publisher.published_runtime_collision_query_bundle(), {});
  {
    std::lock_guard<std::mutex> lock(publisher.mutex);
    publisher.bundle = make_bundle(3, 12);
  }
  clock.advance(1'000'000);
  ASSERT_TRUE(
      progress.publish(9, 200'000'000, clock.now(), clock.now()).published);
  session.request(clock.now(), publisher.published_runtime_collision_query_bundle(), {});
  clock.advance(1'000'000);
  ASSERT_TRUE(
      progress.publish(9, 300'000'000, clock.now(), clock.now()).published);
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_cv.notify_all();

  ASSERT_TRUE(history.wait_for_size(2));
  ASSERT_EQ(history.results.size(), 2u);
  EXPECT_EQ(history.results[0].status,
            RuntimeCollisionLookaheadResultStatus::kSupersededRevision);
  EXPECT_EQ(history.results[1].runtime_bundle_epoch, 3u);
  EXPECT_EQ(history.results[1].status,
            RuntimeCollisionLookaheadResultStatus::kClear);
  EXPECT_EQ(history.results[1].cursor_time_ns, 200'000'000);
  EXPECT_EQ(session.status().jobs_started_total, 2u);
  EXPECT_GE(session.status().reruns_coalesced_total, 2u);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     CompletionWaitsWhenNoNewClearPermitExists) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-no-catch-up");
  ResultHistory history;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&history](const RuntimeCollisionLookaheadSession*,
                 RuntimeCollisionLookaheadResult result) {
        history.push(std::move(result));
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));
  ASSERT_TRUE(history.wait_for_size(1));
  EXPECT_EQ(session.status().jobs_started_total, 1u);
  EXPECT_FALSE(session.status().permit_pending);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     ImmediatePermitRevocationSuppressesInFlightAndPendingWork) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool entered = false;
  bool release = false;
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-revoked");
  engine->on_check = [&]() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    if (entered) {
      return;
    }
    entered = true;
    gate_cv.notify_all();
    gate_cv.wait(lock, [&]() { return release; });
  };
  std::atomic<std::uint64_t> callbacks{0};
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&callbacks](const RuntimeCollisionLookaheadSession*,
                   RuntimeCollisionLookaheadResult) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2),
                                 [&]() { return entered; }));
  }
  session.request(clock.now(), publisher.published_runtime_collision_query_bundle(), {});
  ASSERT_TRUE(session.status().permit_pending);
  session.revoke_clear_permits();
  session.revoke_clear_permits();
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_cv.notify_all();

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  while (session.status().job_active &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(session.status().job_active);
  EXPECT_FALSE(session.status().permit_pending);
  EXPECT_FALSE(session.status().accepting_clear_permits);
  EXPECT_EQ(session.status().jobs_started_total, 1u);
  EXPECT_EQ(session.status().clear_permits_invalidated_total, 1u);
  EXPECT_EQ(callbacks.load(std::memory_order_relaxed), 0u);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     SlowerQueryIsAcceptedInsideItsSeparateLookaheadAndCycleBudgets) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-61ms-clear");
  engine->clock_to_advance = &clock;
  engine->simulated_query_duration_ns = 61'000'000;
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  config.horizon_ns = 1;
  config.query_budget_ns = 75'000'000;
  config.cycle_budget_ns = 100'000'000;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*,
              RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kClear);
  EXPECT_EQ(result.cycle_duration_ns, 61'000'000);
  EXPECT_EQ(result.worker_active_duration_ns, 61'000'000);

  const auto status = session.status();
  EXPECT_EQ(status.last_cycle_duration_ns, 61'000'000);
  EXPECT_EQ(status.last_worker_active_duration_ns, 61'000'000);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     DynamicBodyFreshnessMustStillBeValidAfterCollisionWorkCompletes) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle =
      make_dynamic_bundle(1, 10, clock.now(), 50'000'000);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>(
      "lookahead-completion-freshness");
  engine->clock_to_advance = &clock;
  engine->simulated_query_duration_ns = 60'000'000;
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  config.horizon_ns = 1;
  config.query_budget_ns = 75'000'000;
  config.cycle_budget_ns = 100'000'000;
  config.dynamic_body_max_age_cap_ns = 500'000'000;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*,
              RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kInconclusive);
  EXPECT_EQ(result.reason, "stale_dynamic_collision_body");
  EXPECT_EQ(result.cycle_duration_ns, 60'000'000);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     CompleteCycleDurationIncludesAdmissionToWorkerStartJitter) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-cycle-latency");
  engine->clock_to_advance = &clock;
  engine->simulated_query_duration_ns = 10'000'000;
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  config.horizon_ns = 1;
  config.query_budget_ns = 25'000'000;
  config.cycle_budget_ns = 100'000'000;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*,
              RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      }, accept_test_authority());
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");

  const auto admitted_at_ns = clock.now();
  clock.advance(7'000'000);
  session.request(admitted_at_ns,
                  publisher.published_runtime_collision_query_bundle(), {});

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kClear);
  EXPECT_EQ(result.start_jitter_ns, 7'000'000);
  EXPECT_EQ(result.cycle_duration_ns, 17'000'000);
  EXPECT_EQ(result.worker_active_duration_ns, 10'000'000);

  const auto status_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(2);
  while (session.status().jobs_clear_total != 1 &&
         std::chrono::steady_clock::now() < status_deadline) {
    std::this_thread::yield();
  }
  const auto status = session.status();
  ASSERT_EQ(status.jobs_clear_total, 1u);
  EXPECT_EQ(status.max_start_jitter_ns, 7'000'000);
  EXPECT_EQ(status.last_cycle_duration_ns, 17'000'000);
  EXPECT_EQ(status.last_worker_active_duration_ns, 10'000'000);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     RequestedToAuthorityDurationExcludesOnlyObserverEvidence) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-authority-latency");
  engine->clock_to_advance = &clock;
  engine->simulated_query_duration_ns = 10'000'000;
  engine->collision_local_waypoint = 0;
  std::atomic<bool> authority_applied{false};
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  config.horizon_ns = 1;
  config.query_budget_ns = 25'000'000;
  config.cycle_budget_ns = 100'000'000;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink, &authority_applied](const RuntimeCollisionLookaheadSession*,
                                  RuntimeCollisionLookaheadResult result) {
        EXPECT_TRUE(authority_applied.load(std::memory_order_acquire));
        sink.push(std::move(result));
      },
      [&clock, &authority_applied](const RuntimeCollisionLookaheadSession*,
                                   RuntimeCollisionLookaheadResult& result) {
        EXPECT_EQ(result.status,
                  RuntimeCollisionLookaheadResultStatus::kCollision);
        clock.advance(7'000'000);
        result.stop_authority_signalled = true;
        authority_applied.store(true, std::memory_order_release);
        return RuntimeCollisionLookaheadAuthorityDisposition::kAccepted;
      });
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kCollision);
  EXPECT_TRUE(result.stop_authority_signalled);
  EXPECT_EQ(result.cycle_duration_ns, 17'000'000);
  EXPECT_EQ(result.worker_active_duration_ns, 17'000'000);
  EXPECT_FALSE(result.cycle_deadline_exceeded);
  const auto status = session.status();
  EXPECT_EQ(status.last_cycle_duration_ns, 17'000'000);
  EXPECT_EQ(status.jobs_collision_total, 1u);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     CollisionAuthorityOverrunPreservesCollisionAndCountsCycleDeadline) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TestPublisher publisher;
  publisher.bundle = make_bundle(1, 10);
  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
  ASSERT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
  auto engine = std::make_shared<TestLookaheadEngine>("lookahead-authority-overrun");
  engine->clock_to_advance = &clock;
  engine->simulated_query_duration_ns = 10'000'000;
  engine->collision_local_waypoint = 0;
  ResultSink sink;
  RuntimeCollisionLookaheadSession::Config config;
  config.workers = 1;
  config.max_samples = 2;
  config.horizon_ns = 1;
  config.query_budget_ns = 25'000'000;
  config.cycle_budget_ns = 100'000'000;
  RuntimeCollisionLookaheadSession session(
      config, clock, scheduler, publisher, progress, {engine},
      [&sink](const RuntimeCollisionLookaheadSession*,
              RuntimeCollisionLookaheadResult result) {
        sink.push(std::move(result));
      },
      [&clock](const RuntimeCollisionLookaheadSession*,
               RuntimeCollisionLookaheadResult& result) {
        clock.advance(95'000'000);
        result.stop_authority_signalled = true;
        return RuntimeCollisionLookaheadAuthorityDisposition::kAccepted;
      });
  session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
  ASSERT_TRUE(request_until_started(session, clock, publisher));

  const auto result = sink.wait();
  EXPECT_EQ(result.status, RuntimeCollisionLookaheadResultStatus::kCollision);
  EXPECT_EQ(result.reason, "runtime_lookahead_collision");
  EXPECT_EQ(result.cycle_duration_ns, 105'000'000);
  EXPECT_TRUE(result.cycle_deadline_exceeded);
  const auto status = session.status();
  EXPECT_EQ(status.jobs_collision_total, 1u);
  EXPECT_EQ(status.jobs_cycle_timeout_total, 1u);
}

TEST(RuntimeCollisionLookaheadSessionTest,
     QueryAndWholeCycleBudgetOverrunsRemainDistinct) {
  struct CaseOutcome {
    RuntimeCollisionLookaheadResult result;
    RuntimeCollisionLookaheadStatus status;
  };
  auto run_case = [](std::int64_t query_duration_ns, std::size_t max_samples,
                     std::int64_t query_budget_ns,
                     std::int64_t cycle_budget_ns) {
    FakeClock clock;
    clock.set(1'000'000'000);
    ManualScheduler scheduler;
    TestPublisher publisher;
    publisher.bundle = make_bundle(1, 10);
    ExecutionProgressPublication progress(
        ExecutionProgressSourceKind::kBackendCommanded,
        ExecutionProgressAuthorityProfile::kCommandedSimulation, "isaac", 0);
    EXPECT_TRUE(progress.publish(9, 0, clock.now(), clock.now()).published);
    auto engine = std::make_shared<TestLookaheadEngine>("lookahead-budget-case");
    engine->clock_to_advance = &clock;
    engine->simulated_query_duration_ns = query_duration_ns;
    ResultSink sink;
    RuntimeCollisionLookaheadSession::Config config;
    config.workers = 1;
    config.max_samples = std::max<std::size_t>(2, max_samples);
    config.horizon_ns = max_samples == 1 ? 1 : 200'000'000;
    config.query_budget_ns = query_budget_ns;
    config.cycle_budget_ns = cycle_budget_ns;
    RuntimeCollisionLookaheadSession session(
        config, clock, scheduler, publisher, progress, {engine},
        [&sink](const RuntimeCollisionLookaheadSession*,
                RuntimeCollisionLookaheadResult result) {
          sink.push(std::move(result));
        }, accept_test_authority());
    session.arm(7, 9, std::make_shared<const TrajectoryDTO>(trajectory()), "");
    EXPECT_TRUE(request_until_started(session, clock, publisher));
    auto result = sink.wait();
    return CaseOutcome{std::move(result), session.status()};
  };

  const auto query_overrun = run_case(61'000'000, 1, 50'000'000, 100'000'000);
  EXPECT_EQ(query_overrun.result.status,
            RuntimeCollisionLookaheadResultStatus::kInconclusive);
  EXPECT_EQ(query_overrun.result.reason, "runtime_lookahead_query_timeout");
  EXPECT_EQ(query_overrun.status.jobs_inconclusive_total, 1u);
  EXPECT_EQ(query_overrun.status.jobs_query_timeout_total, 1u);
  EXPECT_EQ(query_overrun.status.jobs_cycle_timeout_total, 0u);

  const auto cycle_overrun = run_case(60'000'000, 2, 75'000'000, 100'000'000);
  EXPECT_EQ(cycle_overrun.result.status,
            RuntimeCollisionLookaheadResultStatus::kInconclusive);
  EXPECT_EQ(cycle_overrun.result.reason,
            "runtime_lookahead_deadline_exceeded");
  EXPECT_EQ(cycle_overrun.status.jobs_inconclusive_total, 1u);
  EXPECT_EQ(cycle_overrun.status.jobs_query_timeout_total, 0u);
  EXPECT_EQ(cycle_overrun.status.jobs_cycle_timeout_total, 1u);
}

}  // namespace
}  // namespace manyforge::core
