// Chunk 4 unit tests for ManyForgeCollisionEngine: independent
// per-instance scenes, concurrent queries on separate instances,
// agreement with the RoboPlan path on a primitive fixture, and
// no-scene-lock-contention against the planning context.

#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <roboplan_example_models/resources.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using manyforge::core::AddObjectRequestDTO;
using manyforge::core::AttachObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionDecisionResult;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::CollisionSurface;
using manyforge::core::CollisionValidationCoordinator;
using manyforge::core::CollisionValidationPassConfig;
using manyforge::core::CollisionWorldSnapshot;
using manyforge::core::ICollisionQueryEngine;
using manyforge::core::MeshShapeDTO;
using manyforge::core::Pose3DTO;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectoryWaypointDTO;
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

std::filesystem::path repo_root() {
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

std::shared_ptr<NativeKernelBundle> make_ur10e_bundle() {
  const auto root = repo_root();
  const auto model_dir = root / "manyforge_example_models" / "models" /
                         "ur10e_robotiq_2f_140";
  manyforge::core::ExecutionKernelConfig config;
  return manyforge::roboplan_backend::create_native_kernel(
      "ur10e_robotiq_2f_140",
      "arm",
      (model_dir / "ur10e_robotiq_2f_140.urdf").string(),
      (model_dir / "ur10e_robotiq_2f_140.srdf").string(),
      {root.string()},
      config);
}

std::shared_ptr<ManyForgeCollisionEngine> make_ur10e_engine() {
  const auto root = repo_root();
  const auto model_dir = root / "manyforge_example_models" / "models" /
                         "ur10e_robotiq_2f_140";
  return std::make_shared<ManyForgeCollisionEngine>(
      "ur10e_robotiq_2f_140",
      model_dir / "ur10e_robotiq_2f_140.urdf",
      model_dir / "ur10e_robotiq_2f_140.srdf",
      std::vector<std::filesystem::path>{root},
      "arm");
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

AddObjectRequestDTO make_isaac_mesh_object(const std::string& object_id,
                                           const std::string& rel_mesh_path,
                                           Vector3DTO scale,
                                           Vector3DTO position,
                                           manyforge::core::QuaternionDTO orientation =
                                               {0.0, 0.0, 0.0, 1.0}) {
  MeshShapeDTO mesh;
  mesh.resource_uri = (repo_root() / rel_mesh_path).string();
  mesh.scale = scale;

  AddObjectRequestDTO req;
  req.object_id = object_id;
  req.shape = CollisionShapeDTO{mesh};
  req.pose.position_m = position;
  req.pose.orientation = orientation;
  req.frame_id = "universe";
  return req;
}

Pose3DTO make_pose(Vector3DTO position,
                   manyforge::core::QuaternionDTO orientation) {
  Pose3DTO pose;
  pose.position_m = position;
  pose.orientation = orientation;
  return pose;
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

std::int64_t diagnostic_int64(
    const std::map<std::string, std::string>& diagnostics,
    const std::string& key) {
  auto it = diagnostics.find(key);
  if (it == diagnostics.end()) {
    throw std::runtime_error("missing diagnostic key: " + key);
  }
  return std::stoll(it->second);
}

std::string classified_pairs_text(const CollisionResult& result) {
  std::string out;
  for (const auto& classified : result.normalized_pairs_classified) {
    if (!out.empty()) {
      out += "; ";
    }
    out += classified.pair.body_a + " <-> " + classified.pair.body_b +
           " class=" +
           std::to_string(static_cast<int>(classified.classification));
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
  EXPECT_EQ(diagnostic_int64(result.raw_diagnostics, "timeout_budget_ns"),
            *q.timeout_budget_ns);
  EXPECT_EQ(diagnostic_int64(result.raw_diagnostics,
                             "timeout_observed_duration_ns"),
            *result.query_duration_ns);
  EXPECT_EQ(result.raw_diagnostics.at("timeout_includes_snapshot_sync"),
            "true");
}

TEST(ManyForgeCollisionEngineTest,
     ObservedRuntimeMonitorReportsSnapshotSyncSeparately) {
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
  q.query_id = "observed-runtime-sync-separate";
  q.surface = CollisionSurface::kObservedRuntimeMonitor;
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  CollisionPolicySnapshot policy;
  CollisionCancellationToken cancel;

  auto result = engine->check(*snapshot, q, policy, cancel);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kOk)
      << diagnostics_text(result.raw_diagnostics);
  ASSERT_TRUE(result.query_duration_ns.has_value());
  const auto sync_duration_ns =
      diagnostic_int64(result.raw_diagnostics, "snapshot_sync_duration_ns");
  const auto total_duration_ns =
      diagnostic_int64(result.raw_diagnostics, "total_call_duration_ns");
  EXPECT_GE(sync_duration_ns, 0);
  EXPECT_GE(total_duration_ns, sync_duration_ns);
  EXPECT_GE(total_duration_ns, *result.query_duration_ns);
}

TEST(ManyForgeCollisionEngineTest,
     SnapshotPortLoadsTriangleMeshSceneObject) {
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
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kOk)
      << diagnostics_text(result.raw_diagnostics);
  EXPECT_EQ(result.decision, result.in_collision
                                 ? CollisionDecisionResult::kFail
                                 : CollisionDecisionResult::kPass);
  EXPECT_EQ(result.raw_diagnostics.count("snapshot_sync_error"), 0u)
      << diagnostics_text(result.raw_diagnostics);
}

TEST(ManyForgeCollisionEngineTest,
     PassiveMeshSceneContactDoesNotEnterMotionEnginePairGraph) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(context->add_object(make_mesh_object("mesh_a")).success);
  ASSERT_TRUE(context->add_object(make_box_object("box_a", 0.7)).success);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  ASSERT_TRUE(snapshot->contains_mesh_geometry);
  auto policy = context->collision_policy_snapshot();
  ASSERT_TRUE(policy.has_value());
  ASSERT_FALSE(policy->allowed_static_static_pairs.empty());

  auto engine = make_engine("ur5_gripper", "arm");
  CollisionQuery q;
  q.query_id = "snapshot-mesh-static-contact";
  q.robot_state = context->get_current_state();
  q.collision_world_revision = snapshot->revision;
  CollisionCancellationToken cancel;

  auto direct = engine->check(*snapshot, q, *policy, cancel);
  ASSERT_EQ(direct.engine_status, CollisionEngineStatus::kOk)
      << diagnostics_text(direct.raw_diagnostics);
  ASSERT_FALSE(direct.in_collision)
      << "Passive world-object contacts must remain scene-health data, not "
         "motion-validation contacts.";

  TrajectoryDTO trajectory;
  trajectory.joint_names = q.robot_state.joint_names;
  TrajectoryWaypointDTO waypoint;
  waypoint.positions_rad = q.robot_state.positions_rad;
  waypoint.time_from_start_s = 0.0;
  trajectory.waypoints.push_back(std::move(waypoint));

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = *policy;
  CollisionValidationCoordinator coordinator(std::move(cfg));
  const auto bundle_result =
      coordinator.validate_trajectory(*snapshot, *engine, trajectory, cancel);
  EXPECT_TRUE(bundle_result.decision.accepted);
  ASSERT_FALSE(bundle_result.waypoint_records.empty());
  EXPECT_EQ(bundle_result.waypoint_records.front().result.engine_status,
            CollisionEngineStatus::kOk);
}

TEST(ManyForgeCollisionEngineTest,
     IsaacMeshTableCheeseStartContactDoesNotEnterMotionEnginePairGraph) {
  auto bundle = make_ur10e_bundle();
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  context->set_robot_base_pose(make_pose(
      Vector3DTO{-0.21814000606536865, -0.571590006351471,
                 0.12999999523162842},
      manyforge::core::QuaternionDTO{0.0, 0.0, -0.7071067690849304,
                                     0.7071067932881648}));

  ASSERT_TRUE(context->add_object(make_isaac_mesh_object(
                  "isaac_table_low_327",
                  "examples/isaac_manipulator_scene_light/meshes/"
                  "background__table_low_327_collider_cooked.obj",
                  Vector3DTO{1.0, 1.5, 1.149999976},
                  Vector3DTO{0.0, 0.0, -0.7695999827980996}))
                  .success);
  ASSERT_TRUE(context->add_object(make_isaac_mesh_object(
                  "isaac_cheese_box",
                  "examples/isaac_manipulator_scene_light/meshes/"
                  "mac_n_cheese_centered_collider_cooked.obj",
                  Vector3DTO{1.0, 1.0, 1.0},
                  Vector3DTO{-0.4550379514694214, 0.0765950009226799,
                             0.087826669216156},
                  manyforge::core::QuaternionDTO{
                      0.5020703673362732, -0.518012523651123,
                      -0.4821583032608032, 0.49710360169410706}))
                  .success);
  ASSERT_TRUE(context->add_object(make_isaac_mesh_object(
                  "isaac_soup_can",
                  "examples/isaac_manipulator_scene_light/meshes/"
                  "World__soup_can_collider_cooked.obj",
                  Vector3DTO{1.0, 1.0, 1.0},
                  Vector3DTO{0.085370734333992, -0.004780956543982029,
                             0.05248536914587021},
                  manyforge::core::QuaternionDTO{
                      0.03936762849031647, 0.705935907260489,
                      0.706009404317132, 0.04068667697327009}))
                  .success);
  ASSERT_TRUE(context->add_object(make_isaac_mesh_object(
                  "isaac_small_KLT",
                  "examples/isaac_manipulator_scene_light/meshes/"
                  "World__small_KLT_collider_primitives.obj",
                  Vector3DTO{1.0, 1.0, 1.0},
                  Vector3DTO{-0.561839, -0.367252, 0.073459},
                  manyforge::core::QuaternionDTO{
                      -7.463636797056011e-07, 0.0014223730063974037,
                      5.845196084198735e-05, 0.9999989867184076}))
                  .success);
  ASSERT_TRUE(context->add_object(make_isaac_mesh_object(
                  "isaac_small_KLT_01",
                  "examples/isaac_manipulator_scene_light/meshes/"
                  "World__small_KLT_01_collider_primitives.obj",
                  Vector3DTO{1.0, 1.0, 1.0},
                  Vector3DTO{0.27161648869514465, -0.35853588581085205,
                             0.07468909025192261},
                  manyforge::core::QuaternionDTO{
                      -1.835783623763347e-07, -0.0009134877500924122,
                      -3.4923311013172767e-07, 0.9999995827699003}))
                  .success);

  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  auto policy = context->collision_policy_snapshot();
  ASSERT_TRUE(policy.has_value());
  ASSERT_FALSE(policy->allowed_static_static_pairs.empty());

  CollisionQuery q;
  q.query_id = "isaac-mesh-contact-start";
  q.robot_state.positions_rad = {2.63422544, -0.875980752, 1.551074106,
                                 -2.29231544, -1.560498884, -2.038544566};
  q.collision_world_revision = snapshot->revision;
  CollisionCancellationToken cancel;
  auto engine = make_ur10e_engine();

  auto direct = engine->check(*snapshot, q, *policy, cancel);
  ASSERT_EQ(direct.engine_status, CollisionEngineStatus::kOk)
      << diagnostics_text(direct.raw_diagnostics);
  ASSERT_FALSE(direct.in_collision)
      << "The cheese box resting on the Isaac table is passive scene state; "
         "it must not consume the robot motion engine's runtime-monitor budget.";

  TrajectoryDTO trajectory;
  trajectory.joint_names = {};
  TrajectoryWaypointDTO waypoint;
  waypoint.positions_rad = q.robot_state.positions_rad;
  waypoint.time_from_start_s = 0.0;
  trajectory.waypoints.push_back(std::move(waypoint));

  CollisionValidationPassConfig cfg;
  cfg.policy_snapshot = *policy;
  CollisionValidationCoordinator coordinator(std::move(cfg));
  const auto bundle_result =
      coordinator.validate_trajectory(*snapshot, *engine, trajectory, cancel);
  ASSERT_FALSE(bundle_result.waypoint_records.empty());
  EXPECT_TRUE(bundle_result.decision.accepted)
      << classified_pairs_text(bundle_result.waypoint_records.front().result);
}

TEST(ManyForgeCollisionEngineTest,
     AttachedObjectTouchLinksHonorExplicitGripperCollisionEnvelope) {
  auto bundle = make_ur10e_bundle();
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);

  ASSERT_TRUE(context->add_object(make_box_object("grasped_box")).success);
  AttachObjectRequestDTO attach;
  attach.object_id = "grasped_box";
  attach.frame_id = "grasp_frame";
  attach.touch_links = {"robotiq_base_link",
                        "left_outer_knuckle",
                        "left_outer_finger",
                        "left_inner_knuckle",
                        "left_inner_finger",
                        "right_inner_finger",
                        "left_inner_finger_pad",
                        "right_outer_knuckle",
                        "right_outer_finger",
                        "right_inner_knuckle",
                        "right_inner_finger_pad"};
  const auto attach_result = context->attach_object(attach);
  ASSERT_TRUE(attach_result.success)
      << (attach_result.error ? attach_result.error->message : "");

  auto policy = context->collision_policy_snapshot();
  ASSERT_TRUE(policy.has_value());
  const std::string object_body =
      "robot/robot0/attached/grasped_box/shape/0";
  const std::string left_knuckle_body =
      "robot/robot0/link/left_inner_knuckle/collision/0";
  const std::string right_knuckle_body =
      "robot/robot0/link/right_inner_knuckle/collision/0";
  const std::string gripper_base_body =
      "robot/robot0/link/robotiq_base_link/collision/0";
  const auto has_pair = [&](const std::string& a, const std::string& b) {
    manyforge::core::CollisionPairId expected{a, b};
    if (expected.body_b < expected.body_a) {
      std::swap(expected.body_a, expected.body_b);
    }
    return std::find(policy->allowed_touch_link_pairs.begin(),
                     policy->allowed_touch_link_pairs.end(),
                     expected) != policy->allowed_touch_link_pairs.end();
  };
  EXPECT_TRUE(has_pair(object_body, left_knuckle_body));
  EXPECT_TRUE(has_pair(object_body, right_knuckle_body));
  EXPECT_TRUE(has_pair(object_body, gripper_base_body));
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
  q.required_capabilities = {"contact_points"};
  auto result = check_snapshot(*engine, *snapshot, q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision,
            manyforge::core::CollisionDecisionResult::kInconclusiveFail);
}

TEST(ManyForgeCollisionEngineTest,
     RequiredDistanceCapabilityPopulatesSignedClearance) {
  auto bundle = create_native_kernel("ur5_gripper", "arm");
  ASSERT_TRUE(bundle);
  auto* context = dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(context->add_object(make_box_object("distance_box")).success);
  auto snapshot = context->snapshot();
  ASSERT_NE(snapshot, nullptr);
  auto engine = make_engine("ur5_gripper", "arm");

  CollisionQuery q;
  q.robot_state = context->get_current_state();
  q.required_capabilities = {"distance", "penetration_depth"};
  auto result = check_snapshot(*engine, *snapshot, q);

  EXPECT_EQ(result.engine_status,
            manyforge::core::CollisionEngineStatus::kOk);
  ASSERT_TRUE(result.min_clearance_m.has_value());
  ASSERT_TRUE(result.min_clearance_pair.has_value());
  EXPECT_FALSE(result.pair_clearances.empty());
  EXPECT_TRUE(std::isfinite(*result.min_clearance_m));
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
  EXPECT_EQ(diagnostic_int64(result.raw_diagnostics, "timeout_budget_ns"), 0);
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
  context->set_engine_mismatch_record_emitter(
      [&mismatch_count, &saw_secondary_incomplete, &saw_timestamp](
          manyforge::core::CollisionEngineMismatchRecord record) {
        auto status_it = record.raw_diagnostics.find("secondary_engine_status");
        if (status_it != record.raw_diagnostics.end() &&
            status_it->second == "diagnostic_incomplete") {
          saw_secondary_incomplete.store(true);
        }
        if (record.emitted_at_ns > 0) {
          saw_timestamp.store(true);
        }
        mismatch_count.fetch_add(1);
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
