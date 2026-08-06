#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include <Eigen/Core>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>

namespace {

std::shared_ptr<roboplan::Scene> make_scene(const std::string& name) {
  const auto models = roboplan::example_models::get_package_models_dir();
  const auto package_share =
      roboplan::example_models::get_package_share_dir();
  return std::make_shared<roboplan::Scene>(
      name, models / "ur_robot_model/ur5_gripper.urdf",
      models / "ur_robot_model/ur5_gripper.srdf",
      std::vector<std::filesystem::path>{package_share});
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

}  // namespace
