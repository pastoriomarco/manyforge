#include <manyforge_core/motion_trip_stop_coordinator.hpp>

#include <gtest/gtest.h>

namespace manyforge::core {
namespace {

TEST(MotionTripStopCoordinatorTest, BeginSetsActiveAndClearsStaleCollision) {
  MotionTripStopCoordinator coordinator;
  std::stop_source first_source;
  coordinator.begin(first_source);
  coordinator.signal_collision_and_cancel();
  EXPECT_TRUE(coordinator.finish().collision_detected);

  std::stop_source second_source;
  coordinator.begin(second_source);

  EXPECT_TRUE(coordinator.active());
  EXPECT_FALSE(coordinator.collision_detected());
  EXPECT_FALSE(second_source.get_token().stop_requested());
}

TEST(MotionTripStopCoordinatorTest, SignalCollisionRequestsActiveStop) {
  MotionTripStopCoordinator coordinator;
  std::stop_source source;
  auto token = source.get_token();

  coordinator.begin(source);

  EXPECT_TRUE(coordinator.signal_collision_and_cancel());
  EXPECT_TRUE(coordinator.collision_detected());
  EXPECT_TRUE(token.stop_requested());
}

TEST(MotionTripStopCoordinatorTest, FinishClearsActiveAndReturnsCollision) {
  MotionTripStopCoordinator coordinator;
  std::stop_source source;
  coordinator.begin(source);
  coordinator.signal_collision_and_cancel();

  const auto result = coordinator.finish();

  EXPECT_TRUE(result.collision_detected);
  EXPECT_FALSE(coordinator.active());
  EXPECT_TRUE(coordinator.collision_detected());
}

TEST(MotionTripStopCoordinatorTest, DeactivateClosesWindowWithoutDroppingSource) {
  MotionTripStopCoordinator coordinator;
  std::stop_source source;
  auto token = source.get_token();
  coordinator.begin(source);

  coordinator.deactivate();

  EXPECT_FALSE(coordinator.active());
  EXPECT_TRUE(coordinator.request_stop());
  EXPECT_TRUE(token.stop_requested());
}

TEST(MotionTripStopCoordinatorTest, ResetClearsStateAndDropsStopSource) {
  MotionTripStopCoordinator coordinator;
  std::stop_source source;
  auto token = source.get_token();
  coordinator.begin(source);

  coordinator.reset();

  EXPECT_FALSE(coordinator.active());
  EXPECT_FALSE(coordinator.collision_detected());
  EXPECT_FALSE(coordinator.request_stop());
  EXPECT_FALSE(token.stop_requested());
}

}  // namespace
}  // namespace manyforge::core
