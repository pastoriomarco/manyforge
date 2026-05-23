// Chunk 4 unit tests for DirectCollisionQueryEngine: independent
// per-instance scenes, concurrent queries on separate instances,
// agreement with the RoboPlan path on a primitive fixture, and
// no-scene-lock-contention against the planning context.

#include <manyforge_roboplan/direct_collision_query_engine.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using manyforge::core::CollisionQuery;
using manyforge::roboplan_backend::DirectCollisionQueryEngine;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::RoboPlanPlanningContext;
using manyforge::roboplan_backend::create_native_kernel;

std::shared_ptr<DirectCollisionQueryEngine> make_engine(
    const std::string& model_name = "ur5_gripper",
    const std::string& group_name = "arm") {
  return DirectCollisionQueryEngine::create_from_example_model(model_name,
                                                                group_name);
}

CollisionQuery make_query(const std::vector<double>& positions) {
  CollisionQuery q;
  q.query_id = "direct-engine-test";
  q.robot_state.joint_names = {};
  q.robot_state.positions_rad = positions;
  return q;
}

// --- Independence ---------------------------------------------------------

TEST(DirectCollisionQueryEngineTest,
     TwoInstancesHaveDistinctInternalScenes) {
  auto engine_a = make_engine();
  auto engine_b = make_engine();
  EXPECT_NE(engine_a->internal_scene_identity(),
            engine_b->internal_scene_identity());
}

TEST(DirectCollisionQueryEngineTest, EngineExposesStableIdAndVersion) {
  auto engine = make_engine();
  EXPECT_FALSE(engine->engine_id().empty());
  EXPECT_FALSE(engine->engine_version().empty());
}

// --- Concurrent queries on separate instances -----------------------------

TEST(DirectCollisionQueryEngineTest,
     ConcurrentQueriesOnDifferentInstancesComplete) {
  auto engine_a = make_engine();
  auto engine_b = make_engine();

  std::atomic<int> done{0};
  std::atomic<bool> any_failed{false};

  auto worker = [&done, &any_failed](DirectCollisionQueryEngine& eng) {
    try {
      for (int i = 0; i < 10; ++i) {
        auto result = eng.check(make_query({0.0, 0.0, 0.0, 0.0, 0.0, 0.0}));
        (void)result;
      }
    } catch (...) {
      any_failed.store(true);
    }
    done.fetch_add(1);
  };

  std::thread t_a(worker, std::ref(*engine_a));
  std::thread t_b(worker, std::ref(*engine_b));
  t_a.join();
  t_b.join();

  EXPECT_EQ(done.load(), 2);
  EXPECT_FALSE(any_failed.load());
}

// --- Compare-mode parity on a primitive fixture ---------------------------

TEST(DirectCollisionQueryEngineTest,
     AgreesWithLegacyOnFreeJointStateAgainstSameModel) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);

  auto engine = make_engine("ur5_gripper", "arm");

  // Use the current robot state from the kernel bundle as the input.
  auto current = context->get_current_state();
  CollisionQuery q;
  q.query_id = "parity";
  q.robot_state = current;

  auto direct = engine->check(q);
  auto legacy = context->collision_check_normalized(q);
  EXPECT_EQ(direct.in_collision, legacy.in_collision);
}

TEST(DirectCollisionQueryEngineTest,
     RequiredUnsupportedDiagnosticCapabilityFailsClosed) {
  auto engine = make_engine("ur5_gripper", "arm");

  CollisionQuery q = make_query({0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
  q.required_capabilities = {"distance"};
  auto result = engine->check(q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision,
            manyforge::core::CollisionDecisionResult::kInconclusiveFail);
}

TEST(DirectCollisionQueryEngineTest, ZeroTimeoutFailsClosedWithoutQueryPass) {
  auto engine = make_engine("ur5_gripper", "arm");

  CollisionQuery q = make_query({0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
  q.timeout_budget_ns = 0;
  auto result = engine->check(q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kTimeout);
  EXPECT_EQ(result.decision,
            manyforge::core::CollisionDecisionResult::kInconclusiveFail);
}

TEST(DirectCollisionQueryEngineTest, MalformedQueryFailsClosedWithoutThrowing) {
  auto engine = make_engine("ur5_gripper", "definitely_not_a_group");

  CollisionQuery q = make_query({0.0});
  auto result = engine->check(q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kEngineError);
  EXPECT_EQ(result.decision,
            manyforge::core::CollisionDecisionResult::kInconclusiveFail);
}

// --- No scene-lock contention --------------------------------------------

TEST(DirectCollisionQueryEngineTest,
     QueryProceedsEvenWhilePlanningContextSceneIsHeavilyBusy) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);

  auto engine = make_engine("ur5_gripper", "arm");

  // Background thread hammers the planning context's scene_mutex_
  // via repeated get_current_state() / collision_check_normalized().
  // The direct engine should not be blocked by that load.
  std::atomic<bool> stop{false};
  auto thrasher = std::thread([&] {
    while (!stop.load()) {
      CollisionQuery q;
      q.robot_state = context->get_current_state();
      auto r = context->collision_check_normalized(q);
      (void)r;
    }
  });

  // Run direct queries; measure that they all complete in bounded time.
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    auto r = engine->check(make_query({0.0, 0.0, 0.0, 0.0, 0.0, 0.0}));
    (void)r;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  stop.store(true);
  thrasher.join();

  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
            5);
}

// --- Compare-mode mismatch emission --------------------------------------

TEST(DirectCollisionQueryEngineTest,
     CompareModeEmitsMismatchEventWhenEnginesDisagree) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);

  // Install a direct engine and then require stable body-pair diagnostics. The
  // normalized RoboPlan adapter supports that capability, while the robot-only
  // Chunk-4 direct engine intentionally remains binary-only. Compare mode must
  // report the capability/status mismatch.
  auto direct = make_engine("ur5_gripper", "arm");
  context->set_direct_engine_for_compare(direct);

  std::atomic<int> mismatch_count{0};
  std::atomic<bool> saw_direct_incomplete{false};
  std::atomic<bool> saw_timestamp{false};
  context->set_engine_mismatch_emitter(
      [&mismatch_count, &saw_direct_incomplete](
          const std::map<std::string, std::string>& details) {
        auto status_it = details.find("direct_engine_status");
        if (status_it != details.end() &&
            status_it->second == "diagnostic_incomplete") {
          saw_direct_incomplete.store(true);
        }
        mismatch_count.fetch_add(1);
      });
  context->set_engine_mismatch_record_emitter(
      [&saw_timestamp](manyforge::core::CollisionEngineMismatchRecord record) {
        if (record.emitted_at_ns > 0) {
          saw_timestamp.store(true);
        }
      });

  CollisionQuery q;
  q.robot_state = context->get_current_state();
  q.required_capabilities = {"body_pair_only"};
  context->collision_check_normalized(q);

  EXPECT_GT(mismatch_count.load(), 0);
  EXPECT_TRUE(saw_direct_incomplete.load());
  EXPECT_TRUE(saw_timestamp.load());

  // Reset to disabled.
  context->set_direct_engine_for_compare(nullptr);
  context->set_engine_mismatch_record_emitter({});
}

}  // namespace
