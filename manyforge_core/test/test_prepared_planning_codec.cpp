#include <gtest/gtest.h>

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
