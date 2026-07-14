#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include <Eigen/Core>
#include <roboplan/core/collision_context.hpp>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>

namespace {

static_assert(std::is_copy_constructible_v<roboplan::Scene>);
static_assert(!std::is_copy_assignable_v<roboplan::Scene>);
static_assert(!std::is_move_constructible_v<roboplan::Scene>);
static_assert(!std::is_move_assignable_v<roboplan::Scene>);

std::shared_ptr<roboplan::Scene> make_scene() {
  const auto models = roboplan::example_models::get_package_models_dir();
  const auto package_share =
      roboplan::example_models::get_package_share_dir();
  return std::make_shared<roboplan::Scene>(
      "ur5_gripper", models / "ur_robot_model/ur5_gripper.urdf",
      models / "ur_robot_model/ur5_gripper.srdf",
      std::vector<std::filesystem::path>{package_share});
}

TEST(RoboPlanSceneGenerationTest,
     CloneOwnsIndependentGeometryStateAndCollisionScratch) {
  auto source = make_scene();
  ASSERT_TRUE(source);
  auto clone = source->clone();
  ASSERT_TRUE(clone);
  const auto clone_geometry_count =
      clone->getCollisionModel().geometryObjects.size();
  const Eigen::VectorXd clone_positions = clone->getCurrentJointPositions();
  const bool clone_collision_before = clone->hasCollisions(clone_positions);

  Eigen::Matrix4d far_pose = Eigen::Matrix4d::Identity();
  far_pose(0, 3) = 100.0;
  const auto added = source->addSphereGeometry(
      "source_only_sphere", "universe", roboplan::Sphere(0.05), far_pose,
      Eigen::Vector4d(0.2, 0.4, 0.8, 1.0));
  ASSERT_TRUE(added.has_value()) << added.error();
  source->setJointPositions(source->randomPositions());

  EXPECT_EQ(clone->getCollisionModel().geometryObjects.size(),
            clone_geometry_count);
  EXPECT_TRUE(clone->getCurrentJointPositions().isApprox(clone_positions));
  EXPECT_EQ(clone->hasCollisions(clone_positions), clone_collision_before);
  EXPECT_GT(source->getCollisionModel().geometryObjects.size(),
            clone_geometry_count);
}

TEST(RoboPlanSceneGenerationTest,
     OwnedCollisionContextRetainsExactGenerationAfterSourceDestruction) {
  std::unique_ptr<roboplan::CollisionContext> context;
  std::weak_ptr<const roboplan::Scene> generation_lifetime;
  Eigen::VectorXd query;
  bool expected_collision = false;

  {
    auto source = make_scene();
    ASSERT_TRUE(source);
    auto generation = source->clone();
    ASSERT_TRUE(generation);
    query = generation->getCurrentJointPositions();
    expected_collision = generation->hasCollisions(query);
    generation_lifetime = generation;
    context = std::make_unique<roboplan::CollisionContext>(generation);

    generation.reset();
    source.reset();
  }

  ASSERT_FALSE(generation_lifetime.expired());
  EXPECT_EQ(context->hasCollisions(query), expected_collision);
  context.reset();
  EXPECT_TRUE(generation_lifetime.expired());
}

TEST(RoboPlanSceneGenerationTest,
     ExplicitReplacementKeepsLiveIdentityAndRetainedGenerationIsolation) {
  auto live_scene = make_scene();
  ASSERT_TRUE(live_scene);
  auto retained_generation = live_scene->clone();
  auto replacement = live_scene->clone();
  ASSERT_TRUE(retained_generation);
  ASSERT_TRUE(replacement);

  const auto* const live_identity = live_scene.get();
  const auto retained_geometry_count =
      retained_generation->getCollisionModel().geometryObjects.size();
  Eigen::Matrix4d far_pose = Eigen::Matrix4d::Identity();
  far_pose(1, 3) = 100.0;
  const auto added = replacement->addSphereGeometry(
      "replacement_only_sphere", "universe", roboplan::Sphere(0.05),
      far_pose, Eigen::Vector4d(0.8, 0.4, 0.2, 1.0));
  ASSERT_TRUE(added.has_value()) << added.error();

  live_scene->replaceWithIsolatedCopy(*replacement);

  EXPECT_EQ(live_scene.get(), live_identity);
  EXPECT_GT(live_scene->getCollisionModel().geometryObjects.size(),
            retained_geometry_count);
  EXPECT_EQ(retained_generation->getCollisionModel().geometryObjects.size(),
            retained_geometry_count);
  EXPECT_NO_THROW(
      (void)live_scene->hasCollisions(live_scene->getCurrentJointPositions()));

  const auto removed = replacement->removeGeometry("replacement_only_sphere");
  ASSERT_TRUE(removed.has_value()) << removed.error();
  EXPECT_GT(live_scene->getCollisionModel().geometryObjects.size(),
            replacement->getCollisionModel().geometryObjects.size());
}

TEST(RoboPlanSceneGenerationTest,
     CloneRetainsIndependentCollisionPairPolicyAfterSourceMutation) {
  auto source = make_scene();
  ASSERT_TRUE(source);
  Eigen::Matrix4d far_pose = Eigen::Matrix4d::Identity();
  far_pose(0, 3) = 100.0;
  const auto added = source->addSphereGeometry(
      "source_policy_sphere", "universe", roboplan::Sphere(0.05), far_pose,
      Eigen::Vector4d(0.2, 0.8, 0.4, 1.0));
  ASSERT_TRUE(added.has_value()) << added.error();

  auto clone = source->clone();
  ASSERT_TRUE(clone);
  const auto clone_pair_count =
      clone->getCollisionModel().collisionPairs.size();
  const auto source_pair_count =
      source->getCollisionModel().collisionPairs.size();
  ASSERT_EQ(clone_pair_count, source_pair_count);

  const auto disabled = source->setCollisions(
      "forearm_link", "source_policy_sphere", false);
  ASSERT_TRUE(disabled.has_value()) << disabled.error();

  EXPECT_LT(source->getCollisionModel().collisionPairs.size(),
            source_pair_count);
  EXPECT_EQ(clone->getCollisionModel().collisionPairs.size(),
            clone_pair_count);
  EXPECT_NO_THROW(
      (void)clone->hasCollisions(clone->getCurrentJointPositions()));
}

TEST(RoboPlanSceneGenerationTest,
     SourceMutationAndQueriesDoNotRaceOrAffectCloneQueries) {
  auto source = make_scene();
  ASSERT_TRUE(source);
  auto clone = source->clone();
  ASSERT_TRUE(clone);
  auto context = std::make_unique<roboplan::CollisionContext>(clone);
  const Eigen::VectorXd clone_query = clone->getCurrentJointPositions();
  const bool expected_collision = context->hasCollisions(clone_query);

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
  std::jthread clone_worker([&](std::stop_token) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 128; ++iteration) {
      if (context->hasCollisions(clone_query) != expected_collision) {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
  });

  start.store(true, std::memory_order_release);
  source_worker.join();
  clone_worker.join();
  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_TRUE(clone->getCurrentJointPositions().isApprox(clone_query));
}

}  // namespace
