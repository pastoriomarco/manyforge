#include <gtest/gtest.h>

#include <roboplan/core/scene_context.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <manyforge_roboplan/roboplan_scene_factory.hpp>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>

namespace {

using manyforge::roboplan_backend::RoboPlanJointLimitInput;
using manyforge::roboplan_backend::RoboPlanSceneInput;
using manyforge::roboplan_backend::make_roboplan_scene;

constexpr std::string_view kBoundedRobot = R"(
<robot name="bounded_robot" version="1.0">
  <link name="base"/>
  <link name="tip"/>
  <joint name="joint1" type="revolute">
    <parent link="base"/>
    <child link="tip"/>
    <axis xyz="0 0 1"/>
    <limit lower="-1" upper="1" effort="5" velocity="2"/>
  </joint>
</robot>)";

constexpr std::string_view kBoundedSemanticRobot = R"(
<robot name="bounded_robot">
  <group name="arm"><joint name="joint1"/></group>
</robot>)";

RoboPlanSceneInput bounded_input() {
  return RoboPlanSceneInput{
      "bounded", std::string(kBoundedRobot),
      std::string(kBoundedSemanticRobot), {},
      {RoboPlanJointLimitInput{"joint1", std::vector<double>{-0.75},
                               std::vector<double>{0.75},
                               std::vector<double>{1.5},
                               std::vector<double>{10.0}}}};
}

std::shared_ptr<roboplan::Scene> make_scene(const std::string& name) {
  const auto models = roboplan::example_models::get_package_models_dir();
  const auto package_share =
      roboplan::example_models::get_package_share_dir();
  auto scene = std::make_shared<roboplan::Scene>(
      name, roboplan::loadUrdfSceneDescription(
                models / "ur_robot_model/ur5_gripper.urdf", {package_share}));
  scene->importJointLimitsFromConfig(roboplan::loadJointLimitsConfig(
      models / "ur_robot_model/ur5_config.yaml"));
  if (const auto imported = scene->importSrdf(roboplan::loadTextFile(
          models / "ur_robot_model/ur5_gripper.srdf"));
      !imported.has_value()) {
    throw std::runtime_error(imported.error());
  }
  return scene;
}

TEST(RoboPlanSceneGenerationTest,
     SharedFactoryAppliesVerifiedLimitsBeforeSemanticGroups) {
  auto scene = make_roboplan_scene(bounded_input());
  ASSERT_NE(scene, nullptr);
  const auto joint = scene->getJointInfo("joint1");
  ASSERT_TRUE(joint.has_value());
  EXPECT_DOUBLE_EQ(joint->limits.min_position[0], -0.75);
  EXPECT_DOUBLE_EQ(joint->limits.max_position[0], 0.75);
  EXPECT_DOUBLE_EQ(joint->limits.max_velocity[0], 1.5);
  EXPECT_DOUBLE_EQ(joint->limits.max_acceleration[0], 10.0);
  const auto group = scene->getJointGroupInfo("arm");
  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(group->joint_names, std::vector<std::string>{"joint1"});
}

TEST(RoboPlanSceneGenerationTest,
     SharedFactoryRejectsIncompleteDuplicateAndForeignLimitRows) {
  auto missing = bounded_input();
  missing.joint_limits.clear();
  EXPECT_THROW(static_cast<void>(make_roboplan_scene(std::move(missing))),
               std::runtime_error);

  auto duplicate = bounded_input();
  duplicate.joint_limits.push_back(duplicate.joint_limits.front());
  EXPECT_THROW(static_cast<void>(make_roboplan_scene(std::move(duplicate))),
               std::runtime_error);

  auto foreign = bounded_input();
  foreign.joint_limits.front().joint_name = "not-in-model";
  EXPECT_THROW(static_cast<void>(make_roboplan_scene(std::move(foreign))),
               std::runtime_error);

  auto malformed = bounded_input();
  malformed.joint_limits.front().maximum_acceleration =
      std::vector<double>{-1.0};
  EXPECT_THROW(static_cast<void>(make_roboplan_scene(std::move(malformed))),
               std::runtime_error);
}

TEST(RoboPlanSceneGenerationTest,
     FreshReconstructionOwnsIndependentGeometryStateAndCollisionScratch) {
  auto source = make_scene("source");
  auto retained = make_scene("retained");
  ASSERT_TRUE(source);
  ASSERT_TRUE(retained);
  const auto retained_geometry_count =
      retained->getCollisionModel().geometryObjects.size();
  const Eigen::VectorXd retained_positions =
      retained->getCurrentJointPositions();
  const bool retained_collision_before =
      retained->hasCollisions(retained_positions);

  Eigen::Matrix4d far_pose = Eigen::Matrix4d::Identity();
  far_pose(0, 3) = 100.0;
  const auto added = source->addSphereGeometry(
      "source_only_sphere", "universe", roboplan::Sphere(0.05), far_pose,
      Eigen::Vector4d(0.2, 0.4, 0.8, 1.0));
  ASSERT_TRUE(added.has_value()) << added.error();
  source->setJointPositions(source->randomPositions());

  EXPECT_EQ(retained->getCollisionModel().geometryObjects.size(),
            retained_geometry_count);
  EXPECT_TRUE(
      retained->getCurrentJointPositions().isApprox(retained_positions));
  EXPECT_EQ(retained->hasCollisions(retained_positions),
            retained_collision_before);
  EXPECT_GT(source->getCollisionModel().geometryObjects.size(),
            retained_geometry_count);
}

TEST(RoboPlanSceneGenerationTest,
     RequestPrivateSceneKeepsNativeScratchInsideItsExactGeneration) {
  auto generation = make_scene("owned_generation");
  ASSERT_TRUE(generation);
  const Eigen::VectorXd query = generation->getCurrentJointPositions();
  const bool expected_collision = generation->hasCollisions(query);

  // Scene owns its model, geometry, and private collision scratch. Repeated
  // queries remain valid without exporting a borrowed CollisionContext.
  EXPECT_EQ(generation->hasCollisions(query), expected_collision);
  generation.reset();

  auto replacement = make_scene("replacement_generation");
  ASSERT_TRUE(replacement);
  EXPECT_EQ(replacement->hasCollisions(
                replacement->getCurrentJointPositions()),
            expected_collision);
}

TEST(RoboPlanSceneGenerationTest,
     ReplacementChangesNativeIdentityAndRetainsPriorGenerationIsolation) {
  auto live_scene = make_scene("live_generation");
  ASSERT_TRUE(live_scene);
  const auto retained_generation = live_scene;
  auto replacement = make_scene("replacement_generation");
  ASSERT_TRUE(replacement);

  const auto* const old_identity = live_scene.get();
  const auto retained_geometry_count =
      retained_generation->getCollisionModel().geometryObjects.size();
  Eigen::Matrix4d far_pose = Eigen::Matrix4d::Identity();
  far_pose(1, 3) = 100.0;
  const auto added = replacement->addSphereGeometry(
      "replacement_only_sphere", "universe", roboplan::Sphere(0.05),
      far_pose, Eigen::Vector4d(0.8, 0.4, 0.2, 1.0));
  ASSERT_TRUE(added.has_value()) << added.error();

  live_scene = replacement;

  EXPECT_NE(live_scene.get(), old_identity);
  EXPECT_GT(live_scene->getCollisionModel().geometryObjects.size(),
            retained_geometry_count);
  EXPECT_EQ(retained_generation->getCollisionModel().geometryObjects.size(),
            retained_geometry_count);
  EXPECT_NO_THROW((void)live_scene->hasCollisions(
      live_scene->getCurrentJointPositions()));
}

TEST(RoboPlanSceneGenerationTest,
     FreshGenerationRetainsIndependentCollisionPairPolicyAfterMutation) {
  auto source = make_scene("source_policy");
  auto retained = make_scene("retained_policy");
  ASSERT_TRUE(source);
  ASSERT_TRUE(retained);
  Eigen::Matrix4d far_pose = Eigen::Matrix4d::Identity();
  far_pose(0, 3) = 100.0;
  for (auto* scene : {source.get(), retained.get()}) {
    const auto added = scene->addSphereGeometry(
        "policy_sphere", "universe", roboplan::Sphere(0.05), far_pose,
        Eigen::Vector4d(0.2, 0.8, 0.4, 1.0));
    ASSERT_TRUE(added.has_value()) << added.error();
  }
  const auto retained_pair_count =
      retained->getCollisionModel().collisionPairs.size();
  ASSERT_EQ(source->getCollisionModel().collisionPairs.size(),
            retained_pair_count);

  const auto disabled =
      source->setCollisions("forearm_link", "policy_sphere", false);
  ASSERT_TRUE(disabled.has_value()) << disabled.error();

  EXPECT_LT(source->getCollisionModel().collisionPairs.size(),
            retained_pair_count);
  EXPECT_EQ(retained->getCollisionModel().collisionPairs.size(),
            retained_pair_count);
  EXPECT_NO_THROW((void)retained->hasCollisions(
      retained->getCurrentJointPositions()));
}

TEST(RoboPlanSceneGenerationTest,
     IndependentSourceMutationAndRetainedQueriesDoNotInterfere) {
  auto source = make_scene("concurrent_source");
  auto retained = make_scene("concurrent_retained");
  ASSERT_TRUE(source);
  ASSERT_TRUE(retained);
  const Eigen::VectorXd retained_query =
      retained->getCurrentJointPositions();
  const bool expected_collision =
      retained->hasCollisions(retained_query);

  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::jthread source_worker([&](std::stop_token) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 128; ++iteration) {
      auto positions = source->randomPositions();
      source->setJointPositions(positions);
      (void)source->hasCollisions(positions);
    }
  });
  std::jthread retained_worker([&](std::stop_token) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 128; ++iteration) {
      if (retained->hasCollisions(retained_query) != expected_collision) {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
  });

  start.store(true, std::memory_order_release);
  source_worker.join();
  retained_worker.join();
  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_TRUE(
      retained->getCurrentJointPositions().isApprox(retained_query));
}

TEST(RoboPlanSceneGenerationTest,
     SceneContextsKeepPrivateScratchAndBorrowOneStableScene) {
  static_assert(!std::is_copy_constructible_v<roboplan::SceneContext>);
  static_assert(!std::is_move_constructible_v<roboplan::SceneContext>);
  auto scene = make_scene("private_collision_contexts");
  ASSERT_TRUE(scene);
  const Eigen::VectorXd query = scene->getCurrentJointPositions();

  // RoboPlan 0.8 SceneContext is deliberately nonmovable: its broadphase
  // manager retains pointers into its own scratch.  Heap ownership keeps each
  // context at one address, while this shared Scene outlives both borrowed
  // model references.
  auto first = std::make_unique<roboplan::SceneContext>(*scene);
  auto second = std::make_unique<roboplan::SceneContext>(*scene);
  first->setRngSeed(11U);
  second->setRngSeed(29U);
  const bool expected = first->hasCollisions(query);

  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  const auto query_context = [&](roboplan::SceneContext &context) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 128; ++iteration) {
      if (context.hasCollisions(query) != expected) {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
  };
  std::jthread first_worker(query_context, std::ref(*first));
  std::jthread second_worker(query_context, std::ref(*second));
  start.store(true, std::memory_order_release);
  first_worker.join();
  second_worker.join();

  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_TRUE(scene->getCurrentJointPositions().isApprox(query));
}

TEST(RoboPlanSceneGenerationTest,
     TopologyChangeRejectsStaleManyForgeSceneContext) {
  auto scene = make_scene("collision_context_rebuild");
  ASSERT_TRUE(scene);

  auto context = std::make_unique<roboplan::SceneContext>(*scene);
  const auto query = scene->getCurrentJointPositions();
  ASSERT_TRUE(context->isGeometryCurrent());
  Eigen::Matrix4d far_pose = Eigen::Matrix4d::Identity();
  far_pose(2, 3) = 100.0;
  const auto added = scene->addSphereGeometry(
      "new_context_sphere", "universe", roboplan::Sphere(0.05), far_pose,
      Eigen::Vector4d(0.4, 0.2, 0.8, 1.0));
  ASSERT_TRUE(added.has_value()) << added.error();

  EXPECT_FALSE(context->isGeometryCurrent());
  EXPECT_THROW((void)context->hasCollisions(query), std::runtime_error);
  auto replacement = std::make_unique<roboplan::SceneContext>(*scene);
  EXPECT_TRUE(replacement->isGeometryCurrent());
  EXPECT_NO_THROW((void)replacement->hasCollisions(query));
}

TEST(RoboPlanSceneGenerationTest,
     PlacementOnlyChangeRetainsManyForgeSceneContext) {
  auto scene = make_scene("scene_context_placement");
  ASSERT_TRUE(scene);
  const auto added = scene->addSphereGeometry(
      "moving_context_sphere", "universe", roboplan::Sphere(0.05),
      Eigen::Matrix4d::Identity(), Eigen::Vector4d(0.4, 0.2, 0.8, 1.0));
  ASSERT_TRUE(added.has_value()) << added.error();

  auto context = std::make_unique<roboplan::SceneContext>(*scene);
  const auto query = scene->getCurrentJointPositions();
  Eigen::Matrix4d moved = Eigen::Matrix4d::Identity();
  moved(2, 3) = 100.0;
  const auto updated = scene->updateGeometryPlacement(
      "moving_context_sphere", "universe", moved);
  ASSERT_TRUE(updated.has_value()) << updated.error();

  EXPECT_TRUE(context->isGeometryCurrent());
  EXPECT_NO_THROW((void)context->hasCollisions(query));
  EXPECT_EQ(context->hasCollisions(query), scene->hasCollisions(query));
}

}  // namespace
