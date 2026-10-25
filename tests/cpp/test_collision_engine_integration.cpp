// Owner-neutral manyforge_collision integration and relocation acceptance map.
//
// The historical source at manyforge@08222188293df8e12f90dc72e204c21f22f92ea1
// contained 35 tests. It constructed NativeKernelBundle for scene snapshots,
// observed state, planner comparison, and runtime composition. That factory is
// intentionally gone. Nine responsibilities below remain testable directly
// through ManyForgeCollisionEngine; both the retained/adapted cases and every
// unavailable historical case are named exactly once below instead of being
// hidden in a skipped legacy body.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_collision/scene_asset_root.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <memory>
#include <pinocchio/algorithm/joint-configuration.hpp>
#include <pinocchio/collision/collision.hpp>
#include <pinocchio/parsers/srdf.hpp>
#include <pinocchio/parsers/urdf.hpp>
#include <random>
#include <string>
#include <vector>

#include "collision_qualification_support.hpp"

namespace {

using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionDecisionResult;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionWorldSnapshot;
using manyforge::qualification::AcceptanceDisposition;

std::filesystem::path repo_root() {
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

struct ModelPaths {
  std::filesystem::path urdf;
  std::filesystem::path srdf;
};

ModelPaths ur10e_model_paths() {
  const auto model_dir = repo_root() / "manyforge_example_models" / "models" /
                         "ur10e_robotiq_2f_140";
  return {
      model_dir / "ur10e_robotiq_2f_140.urdf",
      model_dir / "ur10e_robotiq_2f_140.srdf",
  };
}

ManyForgeCollisionEngine::SharedModelPtr load_shared_model() {
  const auto paths = ur10e_model_paths();
  return ManyForgeCollisionEngine::load_shared_model(
      "ur10e_robotiq_2f_140", paths.urdf, paths.srdf, {repo_root()}, "arm");
}

std::unique_ptr<ManyForgeCollisionEngine> make_engine(
    const std::string& group = "arm") {
  const auto paths = ur10e_model_paths();
  return std::make_unique<ManyForgeCollisionEngine>(
      "ur10e_robotiq_2f_140", paths.urdf, paths.srdf,
      std::vector<std::filesystem::path>{repo_root()}, group);
}

CollisionWorldSnapshot minimal_snapshot() {
  CollisionWorldSnapshot snapshot;
  snapshot.publisher_id = "owner-neutral-collision-test";
  snapshot.revision.revision_id = 1;
  snapshot.revision.content_hash = "sha256:owner-neutral-collision-test";
  snapshot.all_bodies_have_geometry = true;
  return snapshot;
}

CollisionQuery one_joint_query() {
  CollisionQuery query;
  query.robot_state.joint_names = {"joint"};
  query.robot_state.positions_rad = {0.0};
  return query;
}

CollisionResult check(ManyForgeCollisionEngine& engine,
                      const CollisionWorldSnapshot& snapshot,
                      CollisionQuery query,
                      const CollisionCancellationToken* cancel = nullptr) {
  query.collision_world_revision = snapshot.revision;
  CollisionCancellationToken default_cancel;
  return engine.check(snapshot, query, CollisionPolicySnapshot{},
                      cancel == nullptr ? default_cancel : *cancel);
}

struct TemporaryModel {
  std::filesystem::path root;
  TemporaryModel() {
    auto pattern = (std::filesystem::temp_directory_path() / "collision-bound-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("cannot create test model directory");
    root = pattern;
  }
  ~TemporaryModel() { std::filesystem::remove_all(root); }
};

ManyForgeCollisionEngine::SharedModelPtr load_empty_collision_model(
    const TemporaryModel& temporary) {
  const auto urdf = temporary.root / "empty.urdf";
  const auto srdf = temporary.root / "empty.srdf";
  {
    std::ofstream model(urdf);
    model << R"(<robot name="empty"><link name="base"/><link name="tip"/>
      <joint name="joint" type="revolute"><parent link="base"/><child link="tip"/>
      <axis xyz="0 0 1"/><limit lower="-1" upper="1" effort="1" velocity="1"/>
      </joint></robot>)";
    std::ofstream semantic(srdf);
    semantic << R"(<robot name="empty"><group name="arm"><joint name="joint"/></group></robot>)";
  }
  return ManyForgeCollisionEngine::load_shared_model(
      "empty", urdf, srdf, {}, "arm");
}

TEST(CollisionEngineSelfCoverage, BoundsColdGeometryBeforeQuadraticAllocationOrMeshIo) {
  TemporaryModel temporary;
  const auto path = temporary.root / "model.urdf";
  const auto write_model = [&](std::size_t count, bool missing_mesh) {
    std::ofstream output(path);
    output << "<robot name='bounded'><link name='base'>";
    for (std::size_t index = 0; index < count; ++index) {
      output << "<collision><geometry>"
             << (missing_mesh ? "<mesh filename='/nonexistent-collision-bound-mesh.stl'/>"
                              : "<box size='0.1 0.1 0.1'/>")
             << "</geometry></collision>";
    }
    output << "</link></robot>";
    ASSERT_TRUE(output.good());
  };
  write_model(ManyForgeCollisionEngine::kMaximumRetainedCollisionGeometries, false);
  EXPECT_NO_THROW(ManyForgeCollisionEngine::load_shared_model("bounded", path, {}, {}, {}));
  write_model(ManyForgeCollisionEngine::kMaximumRetainedCollisionGeometries + 1, true);
  try {
    (void)ManyForgeCollisionEngine::load_shared_model("bounded", path, {}, {}, {});
    FAIL() << "oversized collision model was accepted";
  } catch (const std::invalid_argument& error) {
    EXPECT_STREQ(error.what(),
                 "retained collision geometry count exceeds qualified cold-model bound");
  }
}

TEST(CollisionEngineSelfCoverage, MasterOnlyGroupIncludesItsCrossBranchMimic) {
  namespace core = manyforge::core;
  namespace collision = manyforge::collision;
  TemporaryModel temporary;
  const auto urdf = temporary.root / "model.urdf";
  const auto srdf = temporary.root / "model.srdf";
  {
    std::ofstream model(urdf);
    model << R"(<robot name="mimic">
      <link name="base"><collision><geometry><sphere radius="0.05"/></geometry></collision></link>
      <link name="source"><collision><geometry><sphere radius="0.05"/></geometry></collision></link>
      <link name="follower"><collision><geometry><sphere radius="0.05"/></geometry></collision></link>
      <joint name="a_source" type="prismatic"><parent link="base"/><child link="source"/>
        <origin xyz="1 0 0"/><axis xyz="0 1 0"/><limit lower="0" upper="1" effort="10" velocity="1"/></joint>
      <joint name="b_follower" type="prismatic"><parent link="base"/><child link="follower"/>
        <origin xyz="-1 0 0"/><axis xyz="0 1 0"/><limit lower="0" upper="1" effort="10" velocity="1"/>
        <mimic joint="a_source" multiplier="1" offset="0"/></joint></robot>)";
    std::ofstream semantic(srdf);
    semantic
        << R"(<robot name="mimic"><group name="hand"><joint name="a_source"/></group></robot>)";
  }
  const auto shared = ManyForgeCollisionEngine::load_shared_model("mimic", urdf, srdf, {}, "hand");
  ManyForgeCollisionEngine engine(shared,
                                  ManyForgeCollisionEngine::make_shared_scene_cache(shared));
  std::vector<collision::RetainedComponentCollisionBodyProjectionInput> bodies;
  for (const auto* frame : {"base", "source", "follower"})
    bodies.push_back({core::ControllableComponentId("hand"), core::ModelInstanceId("model"),
                      core::ModelRevision("r1"), core::CollisionBodyRoleId(frame), 0, frame});
  const std::vector<collision::RetainedComponentModelFrameProjectionInput> frames{
      {core::ControllableComponentId("hand"), core::ModelInstanceId("model"),
       core::ModelRevision("r1"), core::FrameRoleId("base"), "base"}};
  const auto projection = ManyForgeCollisionEngine::project_retained_component_collision(
      shared, "base", bodies, frames);
  ASSERT_TRUE(projection.ok()) << projection.error().message;
  auto snapshot = minimal_snapshot();
  snapshot.bodies = projection.value().component_bodies;
  snapshot.component_model_frames = projection.value().component_model_frames;
  const auto follower = std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                                     [](const auto& body) { return body.frame_id == "follower"; });
  ASSERT_NE(follower, snapshot.bodies.end());
  const auto follower_id = follower->body_id;
  core::CollisionWorldBody obstacle;
  obstacle.body_id = "scene/obstacle/shape/0";
  obstacle.owner_kind = core::CollisionOwnerKind::kSceneResource;
  obstacle.frame_id = "world";
  obstacle.pose.position_m = {-1, .4, 0};
  core::CollisionGeometryDescriptor shape;
  shape.body_id = obstacle.body_id;
  shape.owner_kind = obstacle.owner_kind;
  shape.geometry_kind = core::CollisionGeometryKind::kBox;
  shape.box_dimensions_m = core::Vector3DTO{.04, .04, .04};
  obstacle.geometries.push_back(shape);
  snapshot.bodies.push_back(obstacle);
  snapshot.body_count = snapshot.bodies.size();
  snapshot.geometry_count = 4;
  const core::CollisionPairId expected{std::min(follower_id, obstacle.body_id),
                                       std::max(follower_id, obstacle.body_id)};
  const std::vector<std::string> names{"a_source"};
  ASSERT_EQ(engine.prepare_runtime_query(names), core::RuntimeCollisionQueryPrepareStatus::kReady);
  EXPECT_TRUE(engine.runtime_query_covers_full_configuration());
  ASSERT_EQ(engine.prepare_runtime_snapshot(snapshot),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  for (const double position : {.1, .4}) {
    CollisionQuery query;
    query.query_id = "master-only";
    query.robot_state.joint_names = names;
    query.robot_state.positions_rad = {position};
    const auto ordinary = check(engine, snapshot, query);
    ASSERT_EQ(ordinary.engine_status, CollisionEngineStatus::kOk);
    const auto runtime = engine.check_runtime(snapshot, {query.robot_state.positions_rad,
                                                         0,
                                                         50'000'000,
                                                         core::CollisionDistanceRequestMode::kNone,
                                                         {}});
    ASSERT_EQ(runtime.engine_status, CollisionEngineStatus::kOk);
    EXPECT_EQ(engine.internal_last_query_mechanism_counters().active_collision_pair_count, 5U);
    if (position == .4) {
      EXPECT_EQ(ordinary.normalized_pairs, std::vector<core::CollisionPairId>{expected});
      ASSERT_EQ(runtime.collision_pairs.size(), 1U);
      ASSERT_NE(runtime.collision_pairs.front().pair, nullptr);
      EXPECT_EQ(*runtime.collision_pairs.front().pair, expected);
    } else {
      EXPECT_FALSE(ordinary.in_collision);
      EXPECT_EQ(runtime.status, core::RuntimeCollisionQueryStatus::kClear);
    }
  }
}

// Coordinates are identified by joint name against the engine's current
// model. A named state whose length equals model.nq is still mapped by name,
// so a reordered (or foreign) state can never be read positionally.
TEST(CollisionEngineSelfCoverage, NamedStateMapsByNameEvenWhenItsCountEqualsTheModel) {
  namespace core = manyforge::core;
  namespace collision = manyforge::collision;
  TemporaryModel temporary;
  const auto urdf = temporary.root / "model.urdf";
  const auto srdf = temporary.root / "model.srdf";
  {
    std::ofstream model(urdf);
    model << R"(<robot name="pair">
      <link name="base"/>
      <link name="near"><collision><geometry><sphere radius="0.05"/></geometry></collision></link>
      <link name="far"><collision><geometry><sphere radius="0.05"/></geometry></collision></link>
      <joint name="a_near" type="prismatic"><parent link="base"/><child link="near"/>
        <origin xyz="1 0 0"/><axis xyz="0 1 0"/><limit lower="0" upper="1" effort="10" velocity="1"/></joint>
      <joint name="b_far" type="prismatic"><parent link="base"/><child link="far"/>
        <origin xyz="-1 0 0"/><axis xyz="0 1 0"/><limit lower="0" upper="1" effort="10" velocity="1"/></joint>
      <link name="idle"/>
      <joint name="c_idle" type="prismatic"><parent link="base"/><child link="idle"/>
        <axis xyz="0 0 1"/><limit lower="0" upper="1" effort="10" velocity="1"/></joint>
      </robot>)";
    std::ofstream semantic(srdf);
    semantic << R"(<robot name="pair"><group name="arm"><joint name="a_near"/><joint name="b_far"/></group></robot>)";
  }
  const auto shared = ManyForgeCollisionEngine::load_shared_model("pair", urdf, srdf, {}, "arm");
  ManyForgeCollisionEngine engine(shared,
                                  ManyForgeCollisionEngine::make_shared_scene_cache(shared));
  std::vector<collision::RetainedComponentCollisionBodyProjectionInput> bodies;
  for (const auto* frame : {"near", "far"})
    bodies.push_back({core::ControllableComponentId("pair"), core::ModelInstanceId("model"),
                      core::ModelRevision("r1"), core::CollisionBodyRoleId(frame), 0, frame});
  const std::vector<collision::RetainedComponentModelFrameProjectionInput> frames{
      {core::ControllableComponentId("pair"), core::ModelInstanceId("model"),
       core::ModelRevision("r1"), core::FrameRoleId("base"), "base"}};
  const auto projection = ManyForgeCollisionEngine::project_retained_component_collision(
      shared, "base", bodies, frames);
  ASSERT_TRUE(projection.ok()) << projection.error().message;
  auto snapshot = minimal_snapshot();
  snapshot.bodies = projection.value().component_bodies;
  snapshot.component_model_frames = projection.value().component_model_frames;
  core::CollisionWorldBody obstacle;
  obstacle.body_id = "scene/obstacle/shape/0";
  obstacle.owner_kind = core::CollisionOwnerKind::kSceneResource;
  obstacle.frame_id = "world";
  obstacle.pose.position_m = {-1, .4, 0};
  core::CollisionGeometryDescriptor shape;
  shape.body_id = obstacle.body_id;
  shape.owner_kind = obstacle.owner_kind;
  shape.geometry_kind = core::CollisionGeometryKind::kBox;
  shape.box_dimensions_m = core::Vector3DTO{.04, .04, .04};
  obstacle.geometries.push_back(shape);
  snapshot.bodies.push_back(obstacle);
  snapshot.body_count = snapshot.bodies.size();
  snapshot.geometry_count = 3;

  const auto query = [](std::vector<std::string> names, std::vector<double> positions) {
    CollisionQuery result;
    result.query_id = "named-coordinates";
    result.robot_state.joint_names = std::move(names);
    result.robot_state.positions_rad = std::move(positions);
    return result;
  };
  // Reordered names whose count equals model.nq: only b_far reaches the
  // obstacle. Read positionally, the same vector would move a_near instead
  // and report no collision.
  const auto reordered =
      check(engine, snapshot, query({"b_far", "c_idle", "a_near"}, {.4, 0., .1}));
  ASSERT_EQ(reordered.engine_status, CollisionEngineStatus::kOk);
  EXPECT_TRUE(reordered.in_collision);
  const auto canonical =
      check(engine, snapshot, query({"a_near", "c_idle", "b_far"}, {.4, 0., .1}));
  ASSERT_EQ(canonical.engine_status, CollisionEngineStatus::kOk);
  EXPECT_FALSE(canonical.in_collision);

  const std::vector<std::string> reordered_runtime_names{
      "b_far", "c_idle", "a_near"};
  ASSERT_EQ(engine.prepare_runtime_query(reordered_runtime_names),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  EXPECT_TRUE(engine.runtime_query_covers_full_configuration());
  const std::vector<std::string> partial_runtime_names{"a_near", "b_far"};
  ASSERT_EQ(engine.prepare_runtime_query(partial_runtime_names),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  EXPECT_FALSE(engine.runtime_query_covers_full_configuration());

  const auto refused = [&](CollisionQuery refused_query, const std::string& reason) {
    const auto result = check(engine, snapshot, std::move(refused_query));
    EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete) << reason;
    EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail) << reason;
    EXPECT_EQ(result.raw_diagnostics.at("robot_state_error_reason"), reason);
  };
  // Equal counts never establish identity: a foreign name, a repeated name, a
  // name/position count mismatch, or an unnamed full vector are refused.
  refused(query({"b_far", "c_foreign", "a_near"}, {.4, 0., .1}), "robot_state_joint_unknown");
  refused(query({"b_far", "b_far", "a_near"}, {.4, 0., .1}), "robot_state_joint_duplicate");
  refused(query({"b_far", "a_near"}, {.4, 0., .1}), "robot_state_joint_name_count_mismatch");
  refused(query({}, {.4, 0., .1}), "robot_state_joint_names_required");
  // An unnamed state is accepted only in the model's own SRDF group order.
  const auto group = check(engine, snapshot, query({}, {.1, .4}));
  EXPECT_EQ(group.engine_status, CollisionEngineStatus::kOk);
  EXPECT_TRUE(group.in_collision);
}

TEST(CollisionEngineSelfCoverage, RetainsEverySrdfEnabledSelfPairAndMatchesIndependentQueries) {
  namespace core = manyforge::core;
  namespace collision = manyforge::collision;
  const auto paths = ur10e_model_paths();
  const auto shared = load_shared_model();
  ManyForgeCollisionEngine engine(shared,
                                  ManyForgeCollisionEngine::make_shared_scene_cache(shared));
  pinocchio::Model model;
  pinocchio::GeometryModel geometry;
  pinocchio::urdf::buildModel(paths.urdf.string(), model, false, true);
  pinocchio::urdf::buildGeom(model, paths.urdf.string(), pinocchio::COLLISION, geometry,
                             std::vector<std::string>{repo_root().string()});
  // Independent expected inventory: first enumerate, then apply SRDF
  // exclusions.
  geometry.addAllCollisionPairs();
  pinocchio::srdf::removeCollisionPairs(model, geometry, paths.srdf.string());
  ASSERT_GT(geometry.collisionPairs.size(), 0U);
  std::set<std::string> frames;
  std::vector<collision::RetainedComponentCollisionBodyProjectionInput> bodies;
  for (const auto& object : geometry.geometryObjects) {
    const auto& frame = model.frames[object.parentFrame].name;
    if (frames.insert(frame).second) {
      bodies.push_back({core::ControllableComponentId("self-test"), core::ModelInstanceId("robot"),
                        core::ModelRevision("model-1"), core::CollisionBodyRoleId(frame), 0,
                        frame});
    }
  }
  const std::vector<collision::RetainedComponentModelFrameProjectionInput> model_frames{
      {core::ControllableComponentId("self-test"), core::ModelInstanceId("robot"),
       core::ModelRevision("model-1"), core::FrameRoleId("base"), "base_link"}};
  const auto projection = ManyForgeCollisionEngine::project_retained_component_collision(
      shared, "base_link", bodies, model_frames);
  ASSERT_TRUE(projection.ok()) << projection.error().message;
  auto snapshot = minimal_snapshot();
  snapshot.bodies = projection.value().component_bodies;
  snapshot.component_model_frames = projection.value().component_model_frames;
  snapshot.body_count = snapshot.bodies.size();
  snapshot.geometry_count = geometry.geometryObjects.size();
  snapshot.contains_mesh_geometry = true;
  const std::vector<std::string> names{"shoulder_pan_joint", "shoulder_lift_joint",
                                       "elbow_joint",        "wrist_1_joint",
                                       "wrist_2_joint",      "wrist_3_joint"};
  ASSERT_EQ(engine.prepare_runtime_query(names), core::RuntimeCollisionQueryPrepareStatus::kReady);
  // The integrated Robotiq coordinates are outside this arm-only view.
  EXPECT_FALSE(engine.runtime_query_covers_full_configuration());
  ASSERT_EQ(engine.prepare_runtime_snapshot(snapshot),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  pinocchio::Data data(model);
  pinocchio::GeometryData geometry_data(geometry);
  std::minstd_rand generator(2718);
  std::uniform_real_distribution<double> angle(-2.5, 2.5);
  unsigned collision_count = 0, clear_count = 0;
  for (unsigned sample = 0; sample < 64; ++sample) {
    if (sample == 32) {
      ++snapshot.revision.revision_id;
      snapshot.revision.content_hash = "same-geometry-next-revision";
      ASSERT_EQ(engine.prepare_runtime_snapshot(snapshot),
                core::RuntimeCollisionQueryPrepareStatus::kReady);
    }
    auto q = pinocchio::neutral(model).eval();
    std::vector<double> values(names.size());
    for (std::size_t index = 0; index < names.size(); ++index) {
      values[index] = sample == 0 ? 0 : angle(generator);
      q[model.idx_qs[model.getJointId(names[index])]] = values[index];
    }
    const bool expected =
        pinocchio::computeCollisions(model, data, geometry, geometry_data, q, false);
    CollisionQuery ordinary;
    ordinary.query_id = "self-pose-" + std::to_string(sample);
    ordinary.robot_state.joint_names = names;
    ordinary.robot_state.positions_rad = values;
    const auto ordinary_result = check(engine, snapshot, ordinary);
    ASSERT_EQ(ordinary_result.engine_status, CollisionEngineStatus::kOk);
    EXPECT_EQ(ordinary_result.in_collision, expected) << "ordinary sample " << sample;
    const auto actual = engine.check_runtime(
        snapshot, {values, 0, 50'000'000, core::CollisionDistanceRequestMode::kNone, {}});
    ASSERT_EQ(actual.engine_status, CollisionEngineStatus::kOk);
    ASSERT_EQ(engine.internal_last_query_mechanism_counters().active_collision_pair_count,
              geometry.collisionPairs.size())
        << "self-collision coverage differs from SRDF";
    EXPECT_EQ(actual.status, expected ? core::RuntimeCollisionQueryStatus::kCollision
                                      : core::RuntimeCollisionQueryStatus::kClear)
        << "sample " << sample;
    if (expected)
      ++collision_count;
    else
      ++clear_count;
  }
  EXPECT_GT(collision_count, 0U);
  EXPECT_GT(clear_count, 0U);
}

constexpr std::array<AcceptanceDisposition, 26>
    kFactoryCoupledAcceptance{{
        {"FactoryValidationAndLookaheadReuseOnePreparedRevision", "I6/N",
         "addressed validation/lookahead contexts reuse one owner-issued revision"},
        {"ConcurrentColdContextsPublishOnePreparedRevision", "I6/N",
         "concurrent consumers publish one immutable prepared revision"},
        {"DedicatedRuntimeSurfacePreparesAndTracksPublishedRevisions", "I6/N+I8/P5",
         "runtime preparation, distance modes, counters, and revision tracking"},
        {"DedicatedRuntimeSurfaceRejectsDuplicateFullConfigurationNames", "I6/N",
         "resource-addressed state rejects duplicate canonical coordinates"},
        {"ConcurrentQueriesOnDifferentInstancesComplete", "I6/N",
         "independent owner-issued query contexts make concurrent progress"},
        {"ConcurrentCollidingSnapshotQueriesDoNotRaceDiagnosticCapture", "I6/N",
         "colliding diagnostics remain isolated across concurrent contexts"},
        {"AgreesWithDiagnosticComparatorOnFreeJointStateAgainstSameModel", "I6/N",
         "new diagnostic observer parity without the removed comparator owner"},
        {"SnapshotPortAcceptsPrimitiveSceneSnapshotWithBodyPairCapability", "I6/N",
         "headless scene publishes exact primitive body/resource identities"},
        {"SnapshotPortAppliesTimeoutBudgetToFullCall", "I6/N+I8/P5",
         "owner-issued query deadline covers preparation and collision work"},
        {"ObservedRuntimeMonitorReportsSnapshotSyncSeparately", "I8/P5",
         "immediate monitor reports changed-snapshot cost separately"},
        {"ObservedRuntimeLookaheadExcludesSnapshotSyncFromPerQueryBudget", "I8/P5",
         "lookahead charges preparation outside each warm query budget"},
        {"SnapshotPortLoadsTriangleMeshSceneObject", "I6/N",
         "retained mesh identity and bytes load through the headless snapshot"},
        {"PassiveMeshSceneContactDoesNotEnterMotionEnginePairGraph", "I6/N+I8/P5",
         "passive contacts cannot acquire motion-policy authority"},
        {"IsaacMeshTableCheeseStartContactDoesNotEnterMotionEnginePairGraph", "I9/I+I8/P5",
         "Isaac observed mesh contacts preserve exact policy classification"},
        {"AttachedObjectTouchLinksHonorExplicitGripperCollisionEnvelope", "I6/N+I8/P5",
         "attachments and touch links use owner-issued resource identities"},
        {"RequiredUnsupportedDiagnosticCapabilityFailsClosed", "I6/N",
         "typed required capabilities fail closed when unavailable"},
        {"RequiredDistanceCapabilityPopulatesSignedClearance", "I6/N+I8/P5",
         "signed-clearance evidence is emitted for an explicit distance scope"},
        {"RequiredDistanceCapabilityWithoutTypedScopeFailsClosed", "I6/N",
         "distance requests require an exact typed scope"},
        {"SelectedCollidingDistancePreservesFullCollisionCoverage", "I6/N+I8/P5",
         "selected distance never narrows collision enumeration"},
        {"SelectedPenetrationIsExactAcrossIndependentWorkerHistories", "I8/P5",
         "worker history cannot alter selected penetration evidence"},
        {"UnrelatedSelectedPairEvaluatesNoDistance", "I6/N",
         "unknown selected resources perform no distance work"},
        {"SelectedCollidingDistanceRequiresANonemptySelection", "I6/N",
         "empty selected-distance scope fails closed"},
        {"AllCollidingDistanceEvaluatesOnlyCollidingGeometryPairs", "I6/N+I8/P5",
         "all-colliding mode excludes unrelated distance work"},
        {"MultipleGeometryPairsReduceToDeepestLogicalPairDistance", "I6/N+I8/P5",
         "logical-pair reduction retains the deepest complete finite evidence"},
        {"QueryProceedsEvenWhilePlannerProjectionIsHeavilyBusy", "I6/N",
         "authoritative collision queries are independent of planner projection"},
        {"CompareModeEmitsMismatchEventWhenEnginesDisagree", "I6/N+I5/H0",
         "source-owned mismatch facts replace the deleted comparator emitter"},
    }};

constexpr std::array<AcceptanceDisposition, 9> kRetainedAcceptance{{
    {"ConvenienceInstancesOwnDistinctInternalAssets", "retained/current",
     "direct engine instances own distinct model, cache, and mutable contexts"},
    {"FactoryContextsShareImmutableAssetsButIsolateRuntimePreparation",
     "adapted/current",
     "explicit contexts share immutable model assets and choose cache domains"},
    {"EngineExposesStableIdAndVersion", "retained/current",
     "the direct engine exposes stable identity, version, and capability"},
    {"MixedFiniteAndNonFiniteComponentsRemainIncomplete", "retained/current",
     "owner-neutral distance evidence remains incomplete around non-finite data"},
    {"SnapshotPortRejectsStaleRevision", "retained/current",
     "the direct engine rejects a query for a different snapshot revision"},
    {"SnapshotPortRejectsUnknownSnapshotFormatWithDiagnostic", "retained/current",
     "the direct engine rejects an unknown snapshot format with diagnostics"},
    {"SnapshotPortHonorsCancellation", "retained/current",
     "the direct engine fails closed on a pre-requested cancellation"},
    {"ZeroTimeoutFailsClosedWithoutQueryPass", "retained/current",
     "the direct engine fails closed before work when no deadline remains"},
    {"MalformedQueryFailsClosedWithoutThrowing", "retained/current",
     "the direct engine reports malformed robot state without throwing"},
}};

TEST(ManyForgeCollisionEngineTest,
     ConvenienceInstancesOwnDistinctInternalAssets) {
  const auto engine_a = make_engine();
  const auto engine_b = make_engine();
  EXPECT_NE(engine_a->internal_model_identity(),
            engine_b->internal_model_identity());
  EXPECT_NE(engine_a->internal_scene_cache_identity(),
            engine_b->internal_scene_cache_identity());
  EXPECT_NE(engine_a->capability().mutable_context_id,
            engine_b->capability().mutable_context_id);
}

TEST(ManyForgeCollisionEngineTest,
     ExplicitContextsShareImmutableAssetsAndChooseCacheDomains) {
  const auto model = load_shared_model();
  const auto shared_cache = ManyForgeCollisionEngine::make_shared_scene_cache(model);
  const auto isolated_cache = ManyForgeCollisionEngine::make_shared_scene_cache(model);
  ManyForgeCollisionEngine validation(model, shared_cache);
  ManyForgeCollisionEngine lookahead(model, shared_cache);
  ManyForgeCollisionEngine immediate(model, isolated_cache);

  EXPECT_EQ(validation.internal_model_identity(), lookahead.internal_model_identity());
  EXPECT_EQ(lookahead.internal_model_identity(), immediate.internal_model_identity());
  EXPECT_EQ(validation.internal_scene_cache_identity(),
            lookahead.internal_scene_cache_identity());
  EXPECT_NE(validation.internal_scene_cache_identity(),
            immediate.internal_scene_cache_identity());
  EXPECT_NE(validation.capability().mutable_context_id,
            lookahead.capability().mutable_context_id);
}

TEST(ManyForgeCollisionEngineTest,
     SharedCacheTreatsRevisionNumbersAsPublisherLocal) {
  TemporaryModel temporary;
  const auto model = load_empty_collision_model(temporary);
  const auto cache = ManyForgeCollisionEngine::make_shared_scene_cache(model);
  ManyForgeCollisionEngine first(model, cache);
  ManyForgeCollisionEngine replacement(model, cache);
  auto original = minimal_snapshot();
  auto next_incarnation = original;
  next_incarnation.publisher_id = "replacement-collision-publisher";
  next_incarnation.revision.content_hash = "sha256:replacement-world";

  (void)check(first, original, one_joint_query());
  const auto result = check(replacement, next_incarnation, one_joint_query());

  EXPECT_EQ(replacement.internal_prepared_scene_build_count(), 2U);
  EXPECT_EQ(result.raw_diagnostics.count("snapshot_sync_error"), 0U);
}

TEST(ManyForgeCollisionEngineTest,
     SharedCacheRejectsRevisionReuseWithinOnePublisher) {
  TemporaryModel temporary;
  const auto model = load_empty_collision_model(temporary);
  const auto cache = ManyForgeCollisionEngine::make_shared_scene_cache(model);
  ManyForgeCollisionEngine first(model, cache);
  ManyForgeCollisionEngine replacement(model, cache);
  auto original = minimal_snapshot();
  auto invalid_reuse = original;
  invalid_reuse.revision.content_hash = "sha256:changed-without-revision";

  (void)check(first, original, one_joint_query());
  const auto result = check(replacement, invalid_reuse, one_joint_query());

  EXPECT_EQ(result.raw_diagnostics.at("snapshot_sync_error"),
            "collision snapshot revision reused with different content hash");
}

TEST(ManyForgeCollisionEngineTest, EngineExposesStableIdAndVersion) {
  const auto engine = make_engine();
  EXPECT_FALSE(engine->engine_id().empty());
  EXPECT_FALSE(engine->engine_version().empty());
  EXPECT_FALSE(engine->capability().mutable_context_id.empty());
}

TEST(ManyForgeCollisionDistanceEvidenceTest,
     MixedFiniteAndNonFiniteComponentsRemainIncomplete) {
  manyforge::collision::detail::SelectedCollidingDistanceEvidence evidence;
  evidence.observe(-0.001);
  evidence.observe(-0.003);
  evidence.observe(std::numeric_limits<double>::quiet_NaN());
  evidence.observe(-0.002);
  EXPECT_FALSE(evidence.complete);
  ASSERT_TRUE(evidence.deepest_finite_m.has_value());
  EXPECT_DOUBLE_EQ(*evidence.deepest_finite_m, -0.003);

  manyforge::collision::detail::SelectedCollidingDistanceEvidence infinite_only;
  infinite_only.observe(std::numeric_limits<double>::infinity());
  EXPECT_FALSE(infinite_only.complete);
  EXPECT_FALSE(infinite_only.deepest_finite_m.has_value());
}

TEST(ManyForgeCollisionEngineTest, SnapshotPortRejectsStaleRevision) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionQuery query;
  query.query_id = "stale-revision";
  query.collision_world_revision = snapshot.revision;
  ++query.collision_world_revision.revision_id;
  const auto result = engine->check(
      snapshot, query, CollisionPolicySnapshot{}, CollisionCancellationToken{});
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("query_revision_id"), "2");
  EXPECT_EQ(result.raw_diagnostics.at("snapshot_revision_id"), "1");
}

TEST(ManyForgeCollisionEngineTest,
     SnapshotPortRejectsUnknownSnapshotFormatWithDiagnostic) {
  const auto engine = make_engine();
  auto snapshot = minimal_snapshot();
  snapshot.snapshot_format_version = 99;
  CollisionQuery query;
  query.query_id = "unknown-format";
  const auto result = check(*engine, snapshot, query);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("snapshot_format_version"), "99");
}

TEST(ManyForgeCollisionEngineTest, SnapshotPortHonorsCancellation) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionCancellationToken cancel;
  cancel.request_cancel("owner-neutral-test");
  const auto result = check(*engine, snapshot, CollisionQuery{}, &cancel);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kCancelled);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("cancel_reason"), "owner-neutral-test");
}

TEST(ManyForgeCollisionEngineTest, ZeroTimeoutFailsClosedWithoutQueryPass) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionQuery query;
  query.timeout_budget_ns = 0;
  const auto result = check(*engine, snapshot, query);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kTimeout);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(result.raw_diagnostics.at("timeout_budget_ns"), "0");
}

TEST(ManyForgeCollisionEngineTest, MalformedQueryFailsClosedWithoutThrowing) {
  const auto engine = make_engine();
  const auto snapshot = minimal_snapshot();
  CollisionQuery query;
  query.robot_state.positions_rad = {0.0};
  const auto result = check(*engine, snapshot, query);
  EXPECT_EQ(result.engine_status, CollisionEngineStatus::kDiagnosticIncomplete);
  EXPECT_EQ(result.decision, CollisionDecisionResult::kInconclusiveFail);
  EXPECT_FALSE(result.raw_diagnostics.at("robot_state_error").empty());
}

TEST(CollisionQualificationPrimitivesTest,
     RetainedDistributionsAndBudgetsRemainExact) {
  const std::array<std::uint64_t, 4> samples{10, 20, 30, 40};
  const auto distribution =
      manyforge::qualification::summarize_distribution(samples);
  EXPECT_DOUBLE_EQ(distribution.p50, 25.0);
  EXPECT_DOUBLE_EQ(distribution.p95, 38.5);
  EXPECT_DOUBLE_EQ(distribution.p99, 39.7);
  EXPECT_DOUBLE_EQ(distribution.maximum, 40.0);
  EXPECT_DOUBLE_EQ(manyforge::qualification::median_absolute_deviation(samples),
                   10.0);

  using Schedule = manyforge::qualification::RuntimeScheduleContract;
  EXPECT_EQ(Schedule::kMonitorPeriodNs, 50'000'000);
  EXPECT_EQ(Schedule::kImmediateQueryBudgetNs, 50'000'000);
  EXPECT_EQ(Schedule::kLookaheadCycleBudgetNs, 200'000'000);
  EXPECT_EQ(Schedule::kLookaheadHorizonNs, 2'000'000'000);
  EXPECT_EQ(Schedule::kLookaheadQueryBudgetNs, 75'000'000);
  EXPECT_EQ(Schedule::kLookaheadSamples, 32U);
  EXPECT_EQ(Schedule::kTrajectoryWaypointCount, 201U);
}

TEST(CollisionQualificationPrimitivesTest,
     HistoricalFactoryCasesHaveExactRestorationAssignments) {
  EXPECT_EQ(kRetainedAcceptance.size(), 9U);
  EXPECT_EQ(kFactoryCoupledAcceptance.size(), 26U);
  EXPECT_EQ(kRetainedAcceptance.size() + kFactoryCoupledAcceptance.size(), 35U);
  for (const auto& disposition : kRetainedAcceptance) {
    EXPECT_FALSE(disposition.historical_case.empty());
    EXPECT_FALSE(disposition.restoration_gate.empty());
    EXPECT_FALSE(disposition.required_behavior.empty());
  }
  for (const auto& disposition : kFactoryCoupledAcceptance) {
    EXPECT_FALSE(disposition.historical_case.empty());
    EXPECT_FALSE(disposition.restoration_gate.empty());
    EXPECT_FALSE(disposition.required_behavior.empty());
  }
}

void write_mesh(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
}

CollisionWorldSnapshot relative_mesh_snapshot(const std::string& uri,
                                              const std::uint64_t revision) {
  auto snapshot = minimal_snapshot();
  snapshot.revision.revision_id = revision;
  snapshot.revision.content_hash =
      "sha256:relative-scene-mesh-" + std::to_string(revision);
  snapshot.component_model_frames.push_back(
      manyforge::core::CollisionComponentModelFrame{
          manyforge::core::ControllableComponentId{"arm"},
          manyforge::core::ModelInstanceId{"arm_model"},
          manyforge::core::FrameRoleId{"base"},
          "base",
          manyforge::core::ModelRevision{"model-revision-1"},
          {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}});
  manyforge::core::CollisionWorldBody body;
  body.body_id = manyforge::core::CollisionBodyId("scene/relative-mesh/shape/0");
  body.owner_kind = manyforge::core::CollisionOwnerKind::kSceneResource;
  body.frame_id = "world";
  body.pose = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  manyforge::core::CollisionGeometryDescriptor geometry;
  geometry.body_id = body.body_id;
  geometry.owner_kind = body.owner_kind;
  geometry.geometry_kind = manyforge::core::CollisionGeometryKind::kTriangleMesh;
  geometry.local_pose = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
  geometry.source_uri = uri;
  geometry.mesh_scale = manyforge::core::Vector3DTO{1.0, 1.0, 1.0};
  body.geometries.push_back(geometry);
  snapshot.bodies.push_back(body);
  return snapshot;
}

std::string sync_error(const CollisionResult& result) {
  const auto found = result.raw_diagnostics.find("snapshot_sync_error");
  return found == result.raw_diagnostics.end() ? std::string{} : found->second;
}

TEST(ManyForgeCollisionEngineTest,
     RelativeSceneMeshResolvesOnlyBelowTheApprovedSceneAssetRoot) {
  TemporaryModel temporary;
  const std::string triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  const auto outside = temporary.root / "outside";
  const auto approved = temporary.root / "approved";
  write_mesh(outside / "examples/x/meshes/outside.obj", triangle);
  write_mesh(approved / "examples/x/meshes/a.obj", triangle);
  // A repository-relative example mesh: the removed fallbacks would have
  // found it through the current directory or the development /workspace.
  const std::string repository_mesh =
      "examples/isaac_manipulator_scene_light/meshes/"
      "World__soup_can_collider_cooked.obj";

  const auto previous_directory = std::filesystem::current_path();
  struct RestoreDirectory final {
    std::filesystem::path path;
    ~RestoreDirectory() {
      std::error_code ignored;
      std::filesystem::current_path(path, ignored);
    }
  } restore{previous_directory};
  std::filesystem::current_path(outside);

  const auto urdf = temporary.root / "empty.urdf";
  const auto srdf = temporary.root / "empty.srdf";
  static_cast<void>(load_empty_collision_model(temporary));

  {
    // No approved root: relative scene meshes fail closed even when the
    // file exists below the current directory.
    const auto model = ManyForgeCollisionEngine::load_shared_model(
        "unrooted", urdf, srdf, {temporary.root}, "arm");
    ManyForgeCollisionEngine engine(model, nullptr);
    const auto result = check(
        engine, relative_mesh_snapshot("examples/x/meshes/outside.obj", 1U),
        one_joint_query());
    EXPECT_NE(sync_error(result).find("no approved scene-asset root"),
              std::string::npos)
        << sync_error(result);
  }

  const int root_fd =
      ::open(approved.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(root_fd, 0);
  const auto model = ManyForgeCollisionEngine::load_shared_model(
      "rooted", urdf, srdf, {temporary.root, repo_root()}, "arm", root_fd);
  static_cast<void>(::close(root_fd));  // The model keeps its own duplicate.
  {
    ManyForgeCollisionEngine engine(model, nullptr);
    for (const auto& [uri, revision] :
         std::vector<std::pair<std::string, std::uint64_t>>{
             {"examples/x/meshes/outside.obj", 2U},
             {repository_mesh, 3U},
             {"../outside/examples/x/meshes/outside.obj", 4U}}) {
      const auto result =
          check(engine, relative_mesh_snapshot(uri, revision), one_joint_query());
      EXPECT_FALSE(sync_error(result).empty()) << uri;
    }
    const auto loaded = check(
        engine, relative_mesh_snapshot("examples/x/meshes/a.obj", 5U),
        one_joint_query());
    EXPECT_TRUE(sync_error(loaded).empty()) << sync_error(loaded);
  }
  const auto pinned = ManyForgeCollisionEngine::scene_mesh_content_digest(
      model, "examples/x/meshes/a.obj");
  ASSERT_TRUE(pinned.ok()) << pinned.error().message;
  EXPECT_EQ(pinned.value().rfind("sha256:", 0), 0U);

  // Renaming the approved root does not change what the model reads.
  std::filesystem::rename(approved, temporary.root / "approved-displaced");
  ASSERT_TRUE(ManyForgeCollisionEngine::scene_mesh_content_digest(
                  model, "examples/x/meshes/a.obj")
                  .ok());

  // Changed bytes after the first load: a fresh cache must refuse them.
  write_mesh(temporary.root / "approved-displaced/examples/x/meshes/a.obj",
             "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n");
  ManyForgeCollisionEngine fresh(
      model, ManyForgeCollisionEngine::make_shared_scene_cache(model));
  const auto changed = check(
      fresh, relative_mesh_snapshot("examples/x/meshes/a.obj", 6U),
      one_joint_query());
  EXPECT_NE(sync_error(changed).find("scene_mesh_content_changed"),
            std::string::npos)
      << sync_error(changed);
  const auto still_pinned = ManyForgeCollisionEngine::scene_mesh_content_digest(
      model, "examples/x/meshes/a.obj");
  ASSERT_TRUE(still_pinned.ok());
  EXPECT_EQ(still_pinned.value(), pinned.value());
}

}  // namespace
