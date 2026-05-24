// Chunk 4 unit tests for ManyForgeCollisionEngine: independent
// per-instance scenes, concurrent queries on separate instances,
// agreement with the RoboPlan path on a primitive fixture, and
// no-scene-lock-contention against the planning context.

#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <roboplan_example_models/resources.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using manyforge::core::AddObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionDecisionResult;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::CollisionWorldSnapshot;
using manyforge::core::ICollisionQueryEngine;
using manyforge::core::MeshShapeDTO;
using manyforge::core::Vector3DTO;
using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::RoboPlanPlanningContext;
using manyforge::roboplan_backend::create_native_kernel;

std::shared_ptr<ManyForgeCollisionEngine> make_engine(
    const std::string& model_name = "ur5_gripper",
    const std::string& group_name = "arm") {
  struct Config {
    std::string urdf_rel;
    std::string srdf_rel;
    std::string default_group;
  };
  const std::map<std::string, Config> configs = {
      {"ur5_gripper",
       {"ur_robot_model/ur5_gripper.urdf", "ur_robot_model/ur5_gripper.srdf",
        "arm"}},
      {"fr3",
       {"franka_robot_model/fr3.urdf", "franka_robot_model/fr3.srdf",
        "fr3_arm"}},
      {"kinova",
       {"kinova_robot_model/kinova_robotiq.urdf",
        "kinova_robot_model/kinova_robotiq.srdf", "kinova_arm"}},
      {"so101",
       {"so101_robot_model/so101.urdf", "so101_robot_model/so101.srdf",
        "arm"}},
  };
  auto it = configs.find(model_name);
  if (it == configs.end()) {
    throw std::runtime_error("unknown example model: " + model_name);
  }
  const auto models_dir = roboplan::example_models::get_package_models_dir();
  const auto pkg_share = roboplan::example_models::get_package_share_dir();
  const std::string actual_group =
      group_name.empty() ? it->second.default_group : group_name;
  return std::make_shared<ManyForgeCollisionEngine>(
      model_name,
      models_dir / it->second.urdf_rel,
      models_dir / it->second.srdf_rel,
      std::vector<std::filesystem::path>{pkg_share},
      actual_group);
}

CollisionQuery make_query(const std::vector<double>& positions) {
  CollisionQuery q;
  q.query_id = "manyforge-collision-engine-test";
  q.robot_state.joint_names = {};
  q.robot_state.positions_rad = positions;
  return q;
}

CollisionResult check_snapshot(ManyForgeCollisionEngine& engine,
                               const CollisionWorldSnapshot& snapshot,
                               CollisionQuery q) {
  q.collision_world_revision = snapshot.revision;
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;
  return engine.check(snapshot, q, policy, cancel);
}

class AlwaysIncompleteCompareEngine final : public ICollisionQueryEngine {
 public:
  CollisionResult check(const CollisionWorldSnapshot& snapshot,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot& policy,
                        const CollisionCancellationToken& cancel) override {
    (void)snapshot;
    (void)policy;
    (void)cancel;
    CollisionResult result;
    result.query_id = query.query_id;
    result.surface = query.surface;
    result.engine_id = engine_id();
    result.engine_version = engine_version();
    result.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
    result.decision = CollisionDecisionResult::kInconclusiveFail;
    return result;
  }

  const manyforge::core::CollisionEngineCapabilityDescriptor& capability()
      const override {
    return capability_;
  }

  std::string engine_id() const override { return "test/incomplete_compare"; }
  std::string engine_version() const override { return "0.0.0-test"; }

 private:
  manyforge::core::CollisionEngineCapabilityDescriptor capability_;
};

AddObjectRequestDTO make_box_object(const std::string& object_id,
                                    double x = 0.7) {
  AddObjectRequestDTO req;
  req.object_id = object_id;
  req.shape = CollisionShapeDTO{BoxShapeDTO{Vector3DTO{0.05, 0.05, 0.05}}};
  req.pose.position_m = Vector3DTO{x, 0.0, 0.25};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  return req;
}

AddObjectRequestDTO make_large_collision_box(const std::string& object_id) {
  AddObjectRequestDTO req;
  req.object_id = object_id;
  req.shape = CollisionShapeDTO{BoxShapeDTO{Vector3DTO{10.0, 10.0, 10.0}}};
  req.pose.position_m = Vector3DTO{0.0, 0.0, 0.5};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  return req;
}

AddObjectRequestDTO make_mesh_object(const std::string& object_id) {
  const auto mesh_path =
      std::filesystem::temp_directory_path() /
      ("manyforge_collision_engine_" + object_id + ".obj");
  std::ofstream mesh_file(mesh_path);
  mesh_file << "v 0 0 0\n";
  mesh_file << "v 0.01 0 0\n";
  mesh_file << "v 0 0.01 0\n";
  mesh_file << "f 1 2 3\n";
  mesh_file.close();

  MeshShapeDTO mesh;
  mesh.resource_uri = mesh_path.string();
  mesh.scale = Vector3DTO{1.0, 1.0, 1.0};

  AddObjectRequestDTO req;
  req.object_id = object_id;
  req.shape = CollisionShapeDTO{mesh};
  req.pose.position_m = Vector3DTO{0.7, 0.0, 0.25};
  req.pose.orientation.w = 1.0;
  req.frame_id = "universe";
  return req;
}

std::string diagnostics_text(
    const std::map<std::string, std::string>& diagnostics) {
  std::string out;
  for (const auto& [key, value] : diagnostics) {
    if (!out.empty()) {
      out += "; ";
    }
    out += key + "=" + value;
  }
  return out;
}

// --- Independence ---------------------------------------------------------

TEST(ManyForgeCollisionEngineTest,
     TwoInstancesHaveDistinctInternalScenes) {
  auto engine_a = make_engine();
  auto engine_b = make_engine();
  EXPECT_NE(engine_a->internal_model_identity(),
            engine_b->internal_model_identity());
}

TEST(ManyForgeCollisionEngineTest, EngineExposesStableIdAndVersion) {
  auto engine = make_engine();
  EXPECT_FALSE(engine->engine_id().empty());
  EXPECT_FALSE(engine->engine_version().empty());
}

// --- Concurrent queries on separate instances -----------------------------

TEST(ManyForgeCollisionEngineTest,
     ConcurrentQueriesOnDifferentInstancesComplete) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);

  auto engine_a = make_engine();
  auto engine_b = make_engine();
  CollisionQuery q;
  q.query_id = "concurrent-non-colliding-snapshot";
  q.robot_state = context->get_current_state();

  std::atomic<int> done{0};
  std::atomic<bool> any_failed{false};

  auto worker = [&done, &any_failed, &snapshot, &q](ManyForgeCollisionEngine& eng) {
    try {
      for (int i = 0; i < 10; ++i) {
        auto result = check_snapshot(eng, *snapshot, q);
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

TEST(ManyForgeCollisionEngineTest,
     ConcurrentCollidingSnapshotQueriesDoNotRaceDiagnosticCapture) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(context->add_object(make_large_collision_box("collision_box"))
                  .success);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);

  auto engine_a = make_engine("ur5_gripper", "arm");
  auto engine_b = make_engine("ur5_gripper", "arm");

  CollisionQuery q;
  q.query_id = "concurrent-colliding-snapshot";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  q.required_capabilities = {"binary_collision", "body_pair_only"};
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;

  auto serial = engine_a->check(*snapshot, q, policy, cancel);
  ASSERT_EQ(serial.engine_status, CollisionEngineStatus::kOk)
      << diagnostics_text(serial.raw_diagnostics);
  ASSERT_TRUE(serial.in_collision);
  ASSERT_FALSE(serial.normalized_pairs.empty());

  std::atomic<int> done{0};
  std::atomic<bool> any_failed{false};
  auto worker = [&](ManyForgeCollisionEngine& eng) {
    try {
      for (int i = 0; i < 50; ++i) {
        auto result = eng.check(*snapshot, q, policy, cancel);
        if (result.engine_status != serial.engine_status ||
            result.in_collision != serial.in_collision ||
            result.normalized_pairs != serial.normalized_pairs) {
          any_failed.store(true);
          break;
        }
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

TEST(ManyForgeCollisionEngineTest,
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

  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  auto direct = check_snapshot(*engine, *snapshot, q);
  auto legacy = context->collision_check_normalized(q);
  EXPECT_EQ(direct.in_collision, legacy.in_collision);
}

TEST(ManyForgeCollisionEngineTest,
     SnapshotPortAcceptsPrimitiveSceneSnapshotWithBodyPairCapability) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(context->add_object(make_box_object("box_a")).success);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);

  auto engine = make_engine("ur5_gripper", "arm");
  CollisionQuery q;
  q.query_id = "snapshot-primitive";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  q.required_capabilities = {"binary_collision", "body_pair_only"};
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;

  auto result = engine->check(*snapshot, q, policy, cancel);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kOk)
      << diagnostics_text(result.raw_diagnostics);
  EXPECT_EQ(result.decision, result.in_collision
                                 ? CollisionDecisionResult::kFail
                                 : CollisionDecisionResult::kPass);
  ASSERT_TRUE(result.collision_world_revision.has_value());
  EXPECT_EQ(result.collision_world_revision->revision_id,
            snapshot->revision.revision_id);
}

TEST(ManyForgeCollisionEngineTest,
     SnapshotPortAppliesTimeoutBudgetToFullCall) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(context->add_object(make_box_object("box_a")).success);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);

  auto engine = make_engine("ur5_gripper", "arm");
  CollisionQuery q;
  q.query_id = "snapshot-timeout";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  q.timeout_budget_ns = 1;
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;

  auto result = engine->check(*snapshot, q, policy, cancel);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kTimeout);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  ASSERT_TRUE(result.query_duration_ns.has_value());
  EXPECT_GT(*result.query_duration_ns, *q.timeout_budget_ns);
}

TEST(ManyForgeCollisionEngineTest,
     SnapshotPortFailsClosedOnUnsupportedMeshSceneObject) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(context->add_object(make_mesh_object("mesh_a")).success);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  ASSERT_TRUE(snapshot->contains_mesh_geometry);

  auto engine = make_engine("ur5_gripper", "arm");
  CollisionQuery q;
  q.query_id = "snapshot-mesh";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;

  auto result = engine->check(*snapshot, q, policy, cancel);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kUnsupportedGeometry);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  ASSERT_TRUE(result.raw_diagnostics.count("snapshot_sync_error"));
  EXPECT_NE(result.raw_diagnostics["snapshot_sync_error"].find("mesh_a"),
            std::string::npos)
      << result.raw_diagnostics["snapshot_sync_error"];
}

TEST(ManyForgeCollisionEngineTest, SnapshotPortRejectsStaleRevision) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);

  auto engine = make_engine("ur5_gripper", "arm");
  CollisionQuery q;
  q.query_id = "snapshot-stale";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  q.collision_world_revision.revision_id += 1;
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;

  auto result = engine->check(*snapshot, q, policy, cancel);
  EXPECT_EQ(result.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics["query_revision_id"],
            std::to_string(snapshot->revision.revision_id + 1));
  EXPECT_EQ(result.raw_diagnostics["snapshot_revision_id"],
            std::to_string(snapshot->revision.revision_id));
}

TEST(ManyForgeCollisionEngineTest,
     SnapshotPortRejectsUnknownSnapshotFormatWithDiagnostic) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  auto unsupported_snapshot = *snapshot;
  unsupported_snapshot.snapshot_format_version = 99;

  auto engine = make_engine("ur5_gripper", "arm");
  CollisionQuery q;
  q.query_id = "snapshot-format";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = unsupported_snapshot.revision;
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;

  auto result = engine->check(unsupported_snapshot, q, policy, cancel);
  EXPECT_EQ(result.engine_status,
            CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics["snapshot_format_version"], "99");
}

TEST(ManyForgeCollisionEngineTest, SnapshotPortHonorsCancellation) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);

  auto engine = make_engine("ur5_gripper", "arm");
  CollisionQuery q;
  q.query_id = "snapshot-cancel";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;
  cancel.request_cancel("test");

  auto result = engine->check(*snapshot, q, policy, cancel);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kCancelled);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics["cancel_reason"], "test");
}

TEST(ManyForgeCollisionEngineTest,
     RequiredUnsupportedDiagnosticCapabilityFailsClosed) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  auto engine = make_engine("ur5_gripper", "arm");

  CollisionQuery q;
  q.robot_state = context->get_current_state();
  q.required_capabilities = {"distance"};
  auto result = check_snapshot(*engine, *snapshot, q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision,
            manyforge::core::CollisionDecisionResult::kInconclusiveFail);
}

TEST(ManyForgeCollisionEngineTest, ZeroTimeoutFailsClosedWithoutQueryPass) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  auto engine = make_engine("ur5_gripper", "arm");

  CollisionQuery q;
  q.robot_state = context->get_current_state();
  q.timeout_budget_ns = 0;
  auto result = check_snapshot(*engine, *snapshot, q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kTimeout);
  EXPECT_EQ(result.decision,
            manyforge::core::CollisionDecisionResult::kInconclusiveFail);
}

TEST(ManyForgeCollisionEngineTest, MalformedQueryFailsClosedWithoutThrowing) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  auto engine = make_engine("ur5_gripper", "definitely_not_a_group");

  CollisionQuery q = make_query({0.0});
  auto result = check_snapshot(*engine, *snapshot, q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision,
            manyforge::core::CollisionDecisionResult::kInconclusiveFail);
  EXPECT_NE(result.raw_diagnostics["robot_state_error"].find(
                "does not match"),
            std::string::npos);
}

// --- No scene-lock contention --------------------------------------------

TEST(ManyForgeCollisionEngineTest,
     QueryProceedsEvenWhilePlanningContextSceneIsHeavilyBusy) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);

  auto engine = make_engine("ur5_gripper", "arm");

  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  CollisionQuery direct_query;
  direct_query.robot_state = context->get_current_state();

  // Background thread hammers the planning context's scene_mutex_
  // via repeated get_current_state() / collision_check_normalized().
  // The ManyForge-owned collision engine should not be blocked by that load.
  std::atomic<bool> stop{false};
  auto thrasher = std::thread([&] {
    while (!stop.load()) {
      CollisionQuery q;
      q.robot_state = context->get_current_state();
      auto r = context->collision_check_normalized(q);
      (void)r;
    }
  });

  // Run neutral-engine queries; measure that they all complete in bounded time.
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    auto r = check_snapshot(*engine, *snapshot, direct_query);
    (void)r;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  stop.store(true);
  thrasher.join();

  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
            5);
}

// --- Compare-mode mismatch emission --------------------------------------

TEST(ManyForgeCollisionEngineTest,
     CompareModeEmitsMismatchEventWhenEnginesDisagree) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);

  // Compare mode is generic: the RoboPlan adapter sees only an
  // ICollisionQueryEngine, not the concrete manyforge_collision type.
  auto secondary = std::make_shared<AlwaysIncompleteCompareEngine>();
  context->set_collision_engine_for_compare(secondary);

  std::atomic<int> mismatch_count{0};
  std::atomic<bool> saw_secondary_incomplete{false};
  std::atomic<bool> saw_timestamp{false};
  context->set_engine_mismatch_emitter(
      [&mismatch_count, &saw_secondary_incomplete](
          const std::map<std::string, std::string>& details) {
        auto status_it = details.find("secondary_engine_status");
        if (status_it != details.end() &&
            status_it->second == "diagnostic_incomplete") {
          saw_secondary_incomplete.store(true);
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
  EXPECT_TRUE(saw_secondary_incomplete.load());
  EXPECT_TRUE(saw_timestamp.load());

  // Reset to disabled.
  context->set_collision_engine_for_compare(nullptr);
  context->set_engine_mismatch_record_emitter({});
}

}  // namespace
