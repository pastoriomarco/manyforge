#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <roboplan/core/collision_context.hpp>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>

namespace {

struct ModelSource {
  std::string urdf;
  std::string srdf;
  std::vector<std::filesystem::path> package_paths;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not open model input: " + path.string());
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw std::runtime_error("could not read model input: " + path.string());
  }
  return contents.str();
}

ModelSource load_model_source() {
  const auto models = roboplan::example_models::get_package_models_dir();
  return ModelSource{
      .urdf = read_file(models / "ur_robot_model/ur5_gripper.urdf"),
      .srdf = read_file(models / "ur_robot_model/ur5_gripper.srdf"),
      .package_paths = {
          roboplan::example_models::get_package_share_dir()},
  };
}

// Qualification-only model of the E1 native-lifetime rule. The adapter owns
// one fresh native projection for one worker generation, builds every borrowed
// scratch object beneath that projection, exposes no RoboPlan object, and
// fences a stale generation before native dispatch. This is not product code.
class OwnedProjection final {
 public:
  OwnedProjection(const std::uint64_t generation, const ModelSource& source)
      : generation_(generation),
        scene_(std::make_unique<roboplan::Scene>(
            "e1_generation_" + std::to_string(generation), source.urdf,
            source.srdf, source.package_paths)),
        collision_context_(
            std::make_unique<roboplan::CollisionContext>(*scene_)) {}

  OwnedProjection(const OwnedProjection&) = delete;
  OwnedProjection& operator=(const OwnedProjection&) = delete;
  OwnedProjection(OwnedProjection&&) = delete;
  OwnedProjection& operator=(OwnedProjection&&) = delete;

  [[nodiscard]] std::size_t geometry_count() const {
    return scene_->getCollisionModel().geometryObjects.size();
  }

  [[nodiscard]] Eigen::VectorXd current_positions() const {
    return scene_->getCurrentJointPositions();
  }

  [[nodiscard]] bool has_collisions(
      const std::uint64_t requested_generation, const Eigen::VectorXd& query,
      std::atomic<std::uint64_t>& native_dispatches) const {
    if (requested_generation != generation_) {
      throw std::invalid_argument("planner projection generation mismatch");
    }
    native_dispatches.fetch_add(1U, std::memory_order_relaxed);
    return collision_context_->hasCollisions(query);
  }

 private:
  const std::uint64_t generation_;
  // Destruction is reverse declaration order: borrowed CollisionContext dies
  // before the Scene whose model and geometry it references.
  const std::unique_ptr<roboplan::Scene> scene_;
  const std::unique_ptr<roboplan::CollisionContext> collision_context_;
};

TEST(E1RoboPlanNativeLifetime,
     ParsedProjectionOutlivesTransientSerializedInputs) {
  auto source = load_model_source();
  auto projection = std::make_unique<OwnedProjection>(7U, source);
  source.urdf.clear();
  source.urdf.shrink_to_fit();
  source.srdf.clear();
  source.srdf.shrink_to_fit();
  source.package_paths.clear();

  std::atomic<std::uint64_t> dispatches{0U};
  const auto query = projection->current_positions();
  EXPECT_NO_THROW((void)projection->has_collisions(7U, query, dispatches));
  EXPECT_EQ(dispatches.load(std::memory_order_relaxed), 1U);
}

TEST(E1RoboPlanNativeLifetime,
     FreshReconstructionCreatesIndependentProjectionGenerations) {
  const auto source = load_model_source();
  OwnedProjection first(11U, source);
  OwnedProjection second(12U, source);

  EXPECT_EQ(first.geometry_count(), second.geometry_count());
  EXPECT_TRUE(first.current_positions().isApprox(second.current_positions()));

  std::atomic<std::uint64_t> first_dispatches{0U};
  std::atomic<std::uint64_t> second_dispatches{0U};
  EXPECT_EQ(first.has_collisions(11U, first.current_positions(),
                                 first_dispatches),
            second.has_collisions(12U, second.current_positions(),
                                  second_dispatches));
  EXPECT_EQ(first_dispatches.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(second_dispatches.load(std::memory_order_relaxed), 1U);
}

TEST(E1RoboPlanNativeLifetime,
     WrongGenerationIsRejectedBeforeRoboPlanDispatch) {
  const auto source = load_model_source();
  OwnedProjection projection(23U, source);
  std::atomic<std::uint64_t> dispatches{0U};
  const auto query = projection.current_positions();

  EXPECT_THROW((void)projection.has_collisions(22U, query, dispatches),
               std::invalid_argument);
  EXPECT_EQ(dispatches.load(std::memory_order_relaxed), 0U);
  EXPECT_NO_THROW((void)projection.has_collisions(23U, query, dispatches));
  EXPECT_EQ(dispatches.load(std::memory_order_relaxed), 1U);
}

TEST(E1RoboPlanNativeLifetime,
     SeparateGenerationsSupportConcurrentIsolatedQueries) {
  const auto source = load_model_source();
  OwnedProjection first(31U, source);
  OwnedProjection second(32U, source);
  const auto first_query = first.current_positions();
  const auto second_query = second.current_positions();
  std::atomic<std::uint64_t> first_dispatches{0U};
  std::atomic<std::uint64_t> second_dispatches{0U};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};

  const bool expected_first =
      first.has_collisions(31U, first_query, first_dispatches);
  const bool expected_second =
      second.has_collisions(32U, second_query, second_dispatches);
  first_dispatches.store(0U, std::memory_order_relaxed);
  second_dispatches.store(0U, std::memory_order_relaxed);

  std::jthread first_thread([&](std::stop_token) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 128; ++iteration) {
      if (first.has_collisions(31U, first_query, first_dispatches) !=
          expected_first) {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
  });
  std::jthread second_thread([&](std::stop_token) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 128; ++iteration) {
      if (second.has_collisions(32U, second_query, second_dispatches) !=
          expected_second) {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
  });

  start.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();

  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_EQ(first_dispatches.load(std::memory_order_relaxed), 128U);
  EXPECT_EQ(second_dispatches.load(std::memory_order_relaxed), 128U);
}

TEST(E1RoboPlanNativeLifetime,
     DestroyedGenerationCanBeReconstructedFromTheSameLogicalInput) {
  const auto source = load_model_source();
  std::size_t first_geometry_count = 0U;
  bool first_collision_result = false;
  {
    OwnedProjection first(41U, source);
    first_geometry_count = first.geometry_count();
    std::atomic<std::uint64_t> dispatches{0U};
    first_collision_result =
        first.has_collisions(41U, first.current_positions(), dispatches);
  }

  OwnedProjection replacement(42U, source);
  std::atomic<std::uint64_t> replacement_dispatches{0U};
  EXPECT_EQ(replacement.geometry_count(), first_geometry_count);
  EXPECT_EQ(replacement.has_collisions(42U, replacement.current_positions(),
                                       replacement_dispatches),
            first_collision_result);
  EXPECT_EQ(replacement_dispatches.load(std::memory_order_relaxed), 1U);
}

}  // namespace
