#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>

namespace manyforge::core {
namespace {

TrajectoryDTO Trajectory() {
  return {{ResourceId("joint.1"), ResourceId("joint.2")},
          {"joint_1", "joint_2"},
          {{0.0, {0.0, 0.0}, {}, {}}, {1.0, {0.5, -0.5}, {0.0, 0.0}, {0.0, 0.0}}}};
}

TEST(PreparedPlanningCodecTest, RoundTripsTrajectoryWithAbsentDerivativeArrays) {
  const auto encoded = EncodePreparedTrajectory(Trajectory());
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_EQ(encoded.value().find("\"velocities_rad_s\"") == std::string::npos, false);
  const auto decoded = DecodePreparedTrajectory(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  const auto reencoded = EncodePreparedTrajectory(decoded.value());
  ASSERT_TRUE(reencoded.ok()) << reencoded.error().message;
  EXPECT_EQ(reencoded.value(), encoded.value());
  EXPECT_TRUE(decoded.value().waypoints.front().velocities_rad_s.empty());
}

TEST(PreparedPlanningCodecTest, RejectsUnknownMembersAndInvalidPlanResultPresence) {
  const auto encoded = EncodePreparedTrajectory(Trajectory());
  ASSERT_TRUE(encoded.ok());
  auto unknown = encoded.value();
  unknown.insert(unknown.size() - 1U, ",\"unknown\":true");
  EXPECT_FALSE(DecodePreparedTrajectory(unknown).ok());

  PlanResultDTO failure{std::nullopt, ErrorDTO{"planning_failed", "failed", {}}, {}};
  const auto failed_bytes = EncodePreparedPlanResult(failure);
  ASSERT_TRUE(failed_bytes.ok());
  const auto failed_roundtrip = DecodePreparedPlanResult(failed_bytes.value());
  ASSERT_TRUE(failed_roundtrip.ok());
  EXPECT_FALSE(failed_roundtrip.value().projection_revision.valid());

  PlanResultDTO invalid{Trajectory(), std::nullopt, {}};
  EXPECT_FALSE(EncodePreparedPlanResult(invalid).ok());
}

TEST(PreparedPlanningCodecTest, RuntimeScaleBoundsAreIdentityBoundAndRequireCompleteRestKnots) {
  auto trajectory = Trajectory();
  trajectory.waypoints.front().velocities_rad_s = {0.0, 0.0};
  trajectory.waypoints.front().accelerations_rad_s2 = {0.0, 0.0};
  const auto plain = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(plain.ok());
  trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{2.0, 8.0};
  const auto encoded = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_NE(encoded.value(), plain.value());
  const auto decoded = DecodePreparedTrajectory(encoded.value());
  ASSERT_TRUE(decoded.ok());
  ASSERT_TRUE(decoded.value().runtime_scale_bounds);
  EXPECT_EQ(decoded.value().runtime_scale_bounds->max_rate_per_s, 2.0);
  EXPECT_EQ(decoded.value().runtime_scale_bounds->max_rate_change_per_s2, 8.0);
  EXPECT_EQ(EncodePreparedTrajectory(decoded.value()).value(), encoded.value());
  EXPECT_DOUBLE_EQ(decoded.value().runtime_scale_bounds->max_stop_path_rate, 1.0);
  EXPECT_FALSE(decoded.value().runtime_scale_bounds->nominal_time_multiplier);
  trajectory.runtime_scale_bounds->nominal_time_multiplier = 2.5;
  const auto timed = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(timed.ok());
  EXPECT_NE(timed.value(), encoded.value());
  const auto timed_roundtrip = DecodePreparedTrajectory(timed.value());
  ASSERT_TRUE(timed_roundtrip.ok());
  ASSERT_TRUE(timed_roundtrip.value().runtime_scale_bounds->nominal_time_multiplier);
  EXPECT_DOUBLE_EQ(*timed_roundtrip.value().runtime_scale_bounds->nominal_time_multiplier, 2.5);
  EXPECT_EQ(EncodePreparedTrajectory(timed_roundtrip.value()).value(), timed.value());
  for (const double invalid : {0.9, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    trajectory.runtime_scale_bounds->nominal_time_multiplier = invalid;
    EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
  }
  trajectory.runtime_scale_bounds->nominal_time_multiplier.reset();
  trajectory.runtime_scale_bounds->max_stop_path_rate = 1.5;
  trajectory.runtime_scale_bounds->nominal_time_multiplier = 1.0;
  const auto enhanced = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(enhanced.ok());
  EXPECT_NE(enhanced.value(), encoded.value());
  ASSERT_TRUE(DecodePreparedTrajectory(enhanced.value()).ok());
  EXPECT_DOUBLE_EQ(DecodePreparedTrajectory(enhanced.value()).value()
                      .runtime_scale_bounds->max_stop_path_rate, 1.5);
  trajectory.runtime_scale_bounds->max_stop_path_rate = 0.9;
  EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
  trajectory.runtime_scale_bounds->max_stop_path_rate = 1.0;
  trajectory.runtime_scale_bounds->max_rate_per_s = 0.0;
  EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
  trajectory.runtime_scale_bounds->max_rate_per_s = 2.0;
  trajectory.waypoints.front().velocities_rad_s[0] = 0.1;
  EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
  trajectory.waypoints.front().velocities_rad_s.clear();
  EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
}

TEST(PreparedPlanningCodecTest, CartesianMilestonesAreClosedAndIdentityBound) {
  auto trajectory = Trajectory();
  const double end = trajectory.waypoints.back().time_from_start_s;
  const auto plain = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(plain.ok());
  trajectory.cartesian_waypoint_times_s = {0.0, end / 2.0, end / 2.0, end};
  const auto encoded = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_NE(encoded.value(), plain.value());
  const auto decoded = DecodePreparedTrajectory(encoded.value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().cartesian_waypoint_times_s, trajectory.cartesian_waypoint_times_s);
  EXPECT_EQ(EncodePreparedTrajectory(decoded.value()).value(), encoded.value());
  for (const auto& invalid :
       std::vector<std::vector<double>>{{0.0},
                                        {0.1, end},
                                        {0.0, end + 1.0},
                                        {0.0, end, end / 2.0, end},
                                        {0.0, std::numeric_limits<double>::quiet_NaN(), end}}) {
    trajectory.cartesian_waypoint_times_s = invalid;
    EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
  }
}

TEST(PreparedPlanningCodecTest, RoundTripsCompletePlanRequestWithoutPrivateHandles) {
  auto request = testing::make_test_plan_request(
      "move.1", ResolvedJointGoalDTO{{ResourceId("resource:joint1"), ResourceId("resource:joint2")},
                                     {"joint1", "joint2"},
                                     {0.5, -0.5}});
  const auto encoded = EncodePreparedPlanRequest(request);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_EQ(encoded.value().find("retained_collision_world"), std::string::npos);
  EXPECT_EQ(encoded.value().find("retained_collision_policy"), std::string::npos);
  const auto decoded = DecodePreparedPlanRequest(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_FALSE(decoded.value().retained_collision_world);
  EXPECT_FALSE(decoded.value().retained_collision_policy);
  const auto reencoded = EncodePreparedPlanRequest(decoded.value());
  ASSERT_TRUE(reencoded.ok()) << reencoded.error().message;
  EXPECT_EQ(reencoded.value(), encoded.value());

  auto overflow = encoded.value();
  const auto attempt = overflow.find("\"planning_attempt_index\":0");
  ASSERT_NE(attempt, std::string::npos);
  overflow.replace(attempt, std::string("\"planning_attempt_index\":0").size(),
                   "\"planning_attempt_index\":4294967296");
  EXPECT_FALSE(DecodePreparedPlanRequest(overflow).ok());
}

}  // namespace
}  // namespace manyforge::core
