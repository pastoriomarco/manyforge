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

PlanRequestDTO MovingRequest() {
  auto request = testing::make_test_plan_request(
      "moving.1",
      ResolvedJointGoalDTO{{ResourceId("resource:joint1"), ResourceId("resource:joint2")},
                           {"joint1", "joint2"},
                           {0.5, -0.5}});
  request.start_state.velocities =
      std::vector<CoordinateVelocityDTO>{AngularVelocityDTO{0.5}, AngularVelocityDTO{-0.25}};
  TrajectoryDTO prefix{request.resources,
                       request.model_group_binding.compiled_joint_names,
                       {{0.0, {0.0, 0.0}, {0.5, -0.25}, {0.1, 0.0}},
                        {0.25, {0.125, -0.0625}, {0.5, -0.25}, {0.0, 0.0}}}};
  auto suffix = request.start_state;
  suffix.positions = {AngularPositionDTO{0.125}, AngularPositionDTO{-0.0625}};
  suffix.velocities =
      std::vector<CoordinateVelocityDTO>{AngularVelocityDTO{0.0}, AngularVelocityDTO{0.0}};
  request.moving_replacement =
      MovingReplacementInputDTO{{ControllerGoalId("old.goal"), ControllerGoalGeneration(3U)},
                                ExecutionBindingEpoch(4U),
                                2.0,
                                std::move(prefix),
                                2.25,
                                std::move(suffix),
                                request.base_frame,
                                request.tcp_frame,
                                SpeedProfileDTO{1.0, 1.0, 1.0},
                                {},
                                1.5};
  return request;
}

TrajectoryDTO MovingTrajectory(const PlanRequestDTO& request) {
  auto result = request.moving_replacement->source_prefix_trajectory;
  result.waypoints.push_back({1.0, {0.5, -0.5}, {0.0, 0.0}, {0.0, 0.0}});
  result.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{2.0, 8.0};
  return result;
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
  for (const double invalid :
       {0.9, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
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
  EXPECT_DOUBLE_EQ(
      DecodePreparedTrajectory(enhanced.value()).value().runtime_scale_bounds->max_stop_path_rate,
      1.5);
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

TEST(PreparedPlanningCodecTest, CartesianFidelityEvidenceIsClosedValidatedAndIdentityBound) {
  auto trajectory = Trajectory();
  const double end = trajectory.waypoints.back().time_from_start_s;
  trajectory.cartesian_waypoint_times_s = {0.0, end};
  trajectory.cartesian_fidelity_metrics = CartesianFidelityMetricsDTO{
      0.005, 0.05, 0.001, 0.01, 0.0005, 0.002, 0.0015, 0.012, 0.01, 0.005, 2U};
  const auto encoded = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodePreparedTrajectory(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  ASSERT_TRUE(decoded.value().cartesian_fidelity_metrics.has_value());
  EXPECT_EQ(*decoded.value().cartesian_fidelity_metrics, *trajectory.cartesian_fidelity_metrics);
  EXPECT_EQ(EncodePreparedTrajectory(decoded.value()).value(), encoded.value());

  trajectory.cartesian_fidelity_metrics->certified_max_translation_error_m = 0.006;
  EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
  trajectory.cartesian_fidelity_metrics.reset();
  trajectory.cartesian_waypoint_times_s.clear();
  const auto legacy = EncodePreparedTrajectory(trajectory);
  ASSERT_TRUE(legacy.ok());
  EXPECT_EQ(legacy.value().find("cartesian_fidelity_metrics"), std::string::npos);
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

TEST(PreparedPlanningCodecTest, MovingRequestBindsSourceTimeFramesAndClosedContext) {
  auto request = MovingRequest();
  std::visit([](auto& frame) { frame.compiled_frame_name += "_old"; },
             request.moving_replacement->source_tcp_frame);
  const auto encoded = EncodePreparedPlanRequest(request);
  ASSERT_TRUE(encoded.ok()) << encoded.error().details.at("reason");
  auto decoded = DecodePreparedPlanRequest(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().details.at("reason");
  ASSERT_TRUE(decoded.value().moving_replacement);
  const auto& source = *decoded.value().moving_replacement;
  EXPECT_EQ(source.source_goal.controller_goal_id.value(), "old.goal");
  EXPECT_EQ(source.source_goal.controller_goal_generation.value(), 3U);
  EXPECT_EQ(source.source_binding_epoch.value(), 4U);
  EXPECT_DOUBLE_EQ(source.source_cursor_s, 2.0);
  EXPECT_DOUBLE_EQ(source.divergence_cursor_s, 2.25);
  EXPECT_DOUBLE_EQ(source.remaining_preparation_budget_s, 1.5);
  EXPECT_EQ(source.source_base_frame, request.base_frame);
  EXPECT_EQ(source.source_tcp_frame, request.moving_replacement->source_tcp_frame);
  EXPECT_NE(source.source_tcp_frame, request.tcp_frame);
  EXPECT_EQ(EncodePreparedPlanRequest(decoded.value()).value(), encoded.value());
  for (int mutation = 0; mutation < 7; ++mutation) {
    SCOPED_TRACE(mutation);
    auto invalid = request;
    auto& context = *invalid.moving_replacement;
    if (mutation == 0) context.source_binding_epoch = ExecutionBindingEpoch(0U);
    if (mutation == 1) context.divergence_cursor_s = 2.0;
    if (mutation == 2) context.remaining_preparation_budget_s = 0.0;
    if (mutation == 3) context.suffix_start_state.control_route_id = ControlRouteId("other.route");
    if (mutation == 4)
      context.source_prefix_trajectory.runtime_scale_bounds =
          TrajectoryRuntimeScaleBoundsDTO{2.0, 8.0};
    if (mutation == 5) invalid.start_state.velocities.reset();
    if (mutation == 6) {
      context.source_cursor_s = -2.0;
      context.divergence_cursor_s = -1.75;
    }
    EXPECT_FALSE(EncodePreparedPlanRequest(invalid).ok());
  }
  auto negative_cursor = encoded.value();
  const auto source_cursor = negative_cursor.find("\"source_cursor_s\":2");
  ASSERT_NE(source_cursor, std::string::npos);
  negative_cursor.replace(source_cursor, std::string("\"source_cursor_s\":2").size(),
                          "\"source_cursor_s\":-2");
  const auto divergence = negative_cursor.find("\"divergence_cursor_s\":2.25");
  ASSERT_NE(divergence, std::string::npos);
  negative_cursor.replace(divergence, std::string("\"divergence_cursor_s\":2.25").size(),
                          "\"divergence_cursor_s\":-1.75");
  EXPECT_FALSE(DecodePreparedPlanRequest(negative_cursor).ok());
  auto unknown = encoded.value();
  const auto goal = unknown.find("\"source_goal\":{");
  ASSERT_NE(goal, std::string::npos);
  unknown.insert(goal + std::string("\"source_goal\":{").size(), "\"unknown\":true,");
  EXPECT_FALSE(DecodePreparedPlanRequest(unknown).ok());

  request.moving_replacement.reset();
  const auto ordinary = EncodePreparedPlanRequest(request);
  ASSERT_TRUE(ordinary.ok());
  EXPECT_EQ(ordinary.value().find("moving_replacement"), std::string::npos);
  EXPECT_EQ(EncodePreparedPlanRequest(DecodePreparedPlanRequest(ordinary.value()).value()).value(),
            ordinary.value());
}

TEST(PreparedPlanningCodecTest,
     MovingRuntimeTrajectoryRequiresExactTypedPrefixAndNeverNegativeTime) {
  const auto request = MovingRequest();
  const auto& context = *request.moving_replacement;
  auto trajectory = MovingTrajectory(request);
  EXPECT_FALSE(EncodePreparedTrajectory(trajectory).ok());
  const auto encoded = EncodePreparedTrajectory(trajectory, &context);
  ASSERT_TRUE(encoded.ok()) << encoded.error().details.at("reason");
  EXPECT_FALSE(DecodePreparedTrajectory(encoded.value()).ok());
  auto negative_bytes = encoded.value();
  const auto origin = negative_bytes.find("\"time_from_start_s\":0");
  ASSERT_NE(origin, std::string::npos);
  negative_bytes.replace(origin, std::string("\"time_from_start_s\":0").size(),
                         "\"time_from_start_s\":-0.1");
  EXPECT_FALSE(DecodePreparedTrajectory(negative_bytes, &context).ok());
  const auto decoded = DecodePreparedTrajectory(encoded.value(), &context);
  ASSERT_TRUE(decoded.ok()) << decoded.error().details.at("reason");
  EXPECT_EQ(EncodePreparedTrajectory(decoded.value(), &context).value(), encoded.value());
  PlanResultDTO result{trajectory, std::nullopt, request.projection_binding.projection_revision};
  EXPECT_FALSE(EncodePreparedPlanResult(result).ok());
  const auto result_bytes = EncodePreparedPlanResult(result, &context);
  ASSERT_TRUE(result_bytes.ok());
  const auto roundtrip = DecodePreparedPlanResult(result_bytes.value(), &context);
  ASSERT_TRUE(roundtrip.ok());
  EXPECT_EQ(EncodePreparedTrajectory(*roundtrip.value().trajectory, &context).value(),
            encoded.value());
  for (int mutation = 0; mutation < 5; ++mutation) {
    SCOPED_TRACE(mutation);
    auto invalid = trajectory;
    if (mutation == 0) invalid.waypoints.front().time_from_start_s = -0.1;
    if (mutation == 1) invalid.waypoints.front().accelerations_rad_s2[0] += 0.01;
    if (mutation == 2) invalid.waypoints[1].positions_rad[0] += 0.01;
    if (mutation == 3) invalid.waypoints.back().velocities_rad_s[0] = 0.1;
    if (mutation == 4) invalid.runtime_scale_bounds.reset();
    EXPECT_FALSE(EncodePreparedTrajectory(invalid, &context).ok());
  }
}

}  // namespace
}  // namespace manyforge::core
