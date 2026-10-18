#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include "../src/soft_on_path_stop_planner_internal.hpp"

namespace manyforge::core {
namespace {

TrajectoryDTO linear_trajectory_1d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1"};
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{0.0, {0.0}, {1.0}, {0.0}});
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{1.0, {1.0}, {1.0}, {0.0}});
  trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0};
  return trajectory;
}

TrajectoryDTO linear_trajectory_2d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1", "joint_2"};
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}});
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{1.0, {1.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}});
  trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0};
  return trajectory;
}

TrajectoryDTO zero_tangent_curved_1d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1"};
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{0.0, {0.0}, {0.0}, {}});
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{1.0, {1.0}, {0.0}, {}});
  trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0};
  return trajectory;
}

TrajectoryDTO curved_corner_2d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1", "joint_2"};
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {1.0, 0.0}, {}});
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{0.3, {0.3, 0.0}, {1.0, 0.0}, {}});
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{0.6, {0.45, 0.2}, {0.0, 1.1}, {}});
  trajectory.waypoints.push_back(TrajectoryWaypointDTO{1.0, {0.45, 0.7}, {0.0, 0.0}, {}});
  trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0};
  return trajectory;
}

MotionInterruptStopProfileDTO soft_profile(double decel_s = 0.5) {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "soft_profile";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = decel_s;
  profile.per_joint_max_deceleration = {
      {"joint_1", AngularAccelerationDTO{100.0}},
      {"joint_2", AngularAccelerationDTO{100.0}},
  };
  profile.allow_natural_completion_on_short_path = true;
  profile.recoverable_substitution_mode = MotionInterruptStopMode::kSpringBackHold;
  return profile;
}

MotionInterruptStopProfileDTO spring_back_profile(double decel_s = 1.0) {
  auto profile = soft_profile(decel_s);
  profile.profile_id = "spring_back_profile";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.allow_natural_completion_on_short_path = false;
  profile.recoverable_substitution_mode.reset();
  return profile;
}

TEST(SoftOnPathStopPlannerTest, NormalizedStopProfileDefaultsConservatively) {
  MotionInterruptStopProfileDTO profile;
  EXPECT_FALSE(profile.allow_natural_completion_on_short_path);
}

SoftOnPathInitialStateDTO measured_state_1d(double q, double qdot) {
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1"};
  state.state.positions_rad = {q};
  state.state.velocities_rad_s = {qdot};
  state.trajectory_time_from_start_s = q;
  state.runtime_velocity_scale_at_trip = qdot;
  return state;
}

TEST(SoftOnPathStopPlannerTest, JerkOffKeepsTheSmoothRampAndAccelerationGate) {
  auto profile = soft_profile(0.5);
  const auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 0.001}};
  ASSERT_FALSE(plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config).ok());
  config.jerk_limits_enabled = false;
  const auto off = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(off.ok()) << off.error().message;
  EXPECT_EQ(off.value().jerk_limit_source, "disabled");
  EXPECT_DOUBLE_EQ(off.value().replacement_trajectory.waypoints.back().velocities_rad_s[0], 0.0);
  EXPECT_DOUBLE_EQ(off.value().replacement_trajectory.waypoints.back().accelerations_rad_s2[0],
                   0.0);
  profile.allow_natural_completion_on_short_path = false;
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{0.01}}};
  const auto impossible = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(impossible.ok());
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(impossible.error()));
}

TEST(SpringBackOnPathStopPlannerTest, JerkOffAppliesToBothPhasesWithoutInventingALimit) {
  SoftOnPathPlannerConfig config;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 0.001}};
  config.jerk_limits_enabled = false;
  const auto off = plan_spring_back_on_path_stop(linear_trajectory_1d(), spring_back_profile(1.0),
                                                 measured_state_1d(0.2, 0.5), config);
  ASSERT_TRUE(off.ok()) << off.error().message;
  EXPECT_EQ(off.value().jerk_limit_source, "disabled");
  EXPECT_NEAR(off.value().replacement_trajectory.waypoints.back().positions_rad[0], 0.2, 1e-8);
}

TEST(SoftOnPathStopPlannerTest, CoarseRampChecksAccelerationBetweenKnotsWithJerkOff) {
  auto profile = soft_profile(1.0);
  profile.allow_natural_completion_on_short_path = false;
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{1.0}}};
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.0;
  config.sample_period_s = 1.0;
  config.jerk_limits_enabled = false;
  // Both emitted endpoints have zero acceleration; their Hermite segment
  // nevertheless reaches 1.5 rad/s². Endpoint checks must not certify it.
  const auto result =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, measured_state_1d(0.0, 1.0), config);
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(result.error()));
}

TEST(SoftOnPathStopPlannerTest, CoarseRampChecksJerkBetweenKnots) {
  auto profile = soft_profile(1.0);
  profile.allow_natural_completion_on_short_path = false;
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{4.0}}};
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.0;
  config.sample_period_s = 1.0;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 5.0}};
  const auto result =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, measured_state_1d(0.0, 1.0), config);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto& trajectory = result.value().replacement_trajectory;
  EXPECT_DOUBLE_EQ(trajectory.waypoints.back().time_from_start_s, 1.0);
  // The old two-point ramp has zero averaged jerk but 6 rad/s³ at its ends.
  // Check the actual shared interpolation, including both sides of switches.
  for (size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    const double begin = trajectory.waypoints[i - 1].time_from_start_s;
    const double span = trajectory.waypoints[i].time_from_start_s - begin;
    const double dt = span * 1e-5;
    for (double r : {0.0, 0.5, 1.0 - 1e-5}) {
      const auto a = sample_trajectory(trajectory, begin + r * span);
      const auto b = sample_trajectory(trajectory, begin + r * span + dt);
      ASSERT_TRUE(a.ok());
      ASSERT_TRUE(b.ok());
      EXPECT_LE(
          std::abs(b.value().accelerations_rad_s2[0] - a.value().accelerations_rad_s2[0]) / dt,
          5.0 + 1e-5);
    }
  }
}

TEST(SpringBackOnPathStopPlannerTest, CoarseReturnChecksInteriorLimitsInBothJerkModes) {
  for (bool jerk_enabled : {false, true}) {
    SCOPED_TRACE(jerk_enabled);
    auto profile = spring_back_profile(1.0);
    profile.per_joint_max_deceleration = {
        {"joint_1", AngularAccelerationDTO{jerk_enabled ? 100.0 : 4.0}}};
    SoftOnPathPlannerConfig config;
    config.stop_traj_delta_s = 0.0;
    config.sample_period_s = 1.0;
    config.jerk_limits_enabled = jerk_enabled;
    config.per_joint_max_jerk_rad_s3 = {{"joint_1", 100.0}};
    // Forward braking is feasible. The rest-to-rest reverse segment has
    // zero endpoint acceleration but peaks above A=4 and J=100 in its .5 s.
    const auto result = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile,
                                                      measured_state_1d(0.2, 1.0), config);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("spring_back_phase"), "reverse_return");
    EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(result.error()));
  }
}

TrajectoryDTO prepared_cruise_trajectory() {
  return {{ResourceId("joint.1")},
          {"joint_1"},
          {{0.0, {0.0}, {0.0}, {0.0}},
           {1.0, {0.5}, {1.0}, {0.0}},
           {3.0, {2.5}, {1.0}, {0.0}},
           {4.0, {3.0}, {0.0}, {0.0}}},
          TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0}};
}

// Item 6: main's quintic return peaks at 1.875 * d / T. Here d = 0.06 and
// T = 0.1, so the quintic would peak at path rate 1.125.
SoftOnPathInitialStateDTO cruise_trip_state() {
  auto state = measured_state_1d(1.2, 1.0);
  state.trajectory_time_from_start_s = 1.7;
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  return state;
}

SoftOnPathPlannerConfig cruise_trip_config() {
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.02;
  config.allow_model_velocity_fallback = true;
  config.jerk_limits_enabled = false;
  return config;
}

TEST(SpringBackOnPathStopPlannerTest, ReturnWithinStopPathRateKeepsTheQuintic) {
  auto trajectory = prepared_cruise_trajectory();
  trajectory.runtime_scale_bounds->max_stop_path_rate = 1.125 + 1e-6;
  const auto result = plan_spring_back_on_path_stop(trajectory, spring_back_profile(0.2),
                                                    cruise_trip_state(), cruise_trip_config());
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto& diagnostics = result.value().diagnostics;
  EXPECT_EQ(interrupt_structured_scalar_text(diagnostics.at("spring_back_return_profile")),
            "quintic_rest_to_rest");
  EXPECT_NEAR(std::stod(interrupt_structured_scalar_text(
                  diagnostics.at("spring_back_return_peak_path_rate"))),
              1.125, 1e-9);
  const auto& points = result.value().replacement_trajectory.waypoints;
  EXPECT_NEAR(points.back().time_from_start_s, 0.2, 1e-12);
  EXPECT_NEAR(points.back().positions_rad.front(), points.front().positions_rad.front(), 1e-12);
}

TEST(SpringBackOnPathStopPlannerTest, ReturnAboveStopPathRateIsCappedWithSameTargetAndTime) {
  // Over the cap the return ramps up to the certified Stop path rate, cruises
  // and ramps down; it reaches the same trip point at the same time (no
  // refusal and no cap at 1.0 when the certificate allows more).
  for (const double cap : {1.0, 1.1}) {
    SCOPED_TRACE(cap);
    auto trajectory = prepared_cruise_trajectory();
    trajectory.runtime_scale_bounds->max_stop_path_rate = cap;
    const auto result = plan_spring_back_on_path_stop(trajectory, spring_back_profile(0.2),
                                                      cruise_trip_state(), cruise_trip_config());
    ASSERT_TRUE(result.ok()) << result.error().message;
    const auto& diagnostics = result.value().diagnostics;
    EXPECT_EQ(interrupt_structured_scalar_text(diagnostics.at("spring_back_return_profile")),
              "ramp_cruise_ramp_rate_capped");
    const auto& points = result.value().replacement_trajectory.waypoints;
    EXPECT_NEAR(points.back().time_from_start_s, 0.2, 1e-12);
    EXPECT_NEAR(points.back().positions_rad.front(), points.front().positions_rad.front(), 1e-12);
    EXPECT_DOUBLE_EQ(points.back().velocities_rad_s.front(), 0.0);
    EXPECT_NEAR(points.back().accelerations_rad_s2.front(), 0.0, 1e-9);
    double peak = 0.0;
    for (const auto& point : points) {
      if (point.time_from_start_s <= 0.1) continue;
      // Unit path tangent on the cruise section: joint speed is path rate.
      peak = std::max(peak, std::abs(point.velocities_rad_s.front()));
    }
    EXPECT_LE(peak, cap + 1e-9);
    EXPECT_NEAR(peak, cap, 1e-9);
    // The emitted interpolation between knots respects the cap too.
    for (double t = 0.1; t <= 0.2; t += 1e-4) {
      const auto sampled = sample_trajectory(result.value().replacement_trajectory, t);
      ASSERT_TRUE(sampled.ok());
      EXPECT_LE(std::abs(sampled.value().velocities_rad_s.front()), cap + 1e-6);
    }
  }
}

TEST(SpringBackOnPathStopPlannerTest, CappedReturnStillEscalatesJointLimitFailureToHardStop) {
  auto trajectory = prepared_cruise_trajectory();
  auto profile = spring_back_profile(0.2);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{30.0}}};
  const auto result =
      plan_spring_back_on_path_stop(trajectory, profile, cruise_trip_state(), cruise_trip_config());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "spring_back_return_acceleration_limit_exceeded");
  EXPECT_EQ(result.error().details.at("spring_back_phase"), "reverse_return");
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(result.error()));
}

// The spring-back return must stay on the original path: every return knot is
// the accepted path's own point (position and tangent) at a path time inside
// [trip cursor, forward stop], visited in decreasing path time, ending exactly
// at the trip cursor. Joint 0 must be increasing in path time on that interval
// (used to recover the path time of each knot).
void ExpectReturnRetracesTheOriginalPath(const TrajectoryDTO& accepted,
                                         const SoftOnPathStopPlanDTO& plan) {
  const double trip = plan.command_path_time_s;
  const double stop = plan.forward_stop_path_time_s;
  ASSERT_LT(trip, stop);
  const double return_start = std::stod(
      interrupt_structured_scalar_text(plan.diagnostics.at("spring_back_return_start_s")));
  const auto path_at = [&](double u) {
    const auto sampled = sample_trajectory(accepted, u);
    EXPECT_TRUE(sampled.ok());
    return sampled.value();
  };
  const auto& points = plan.replacement_trajectory.waypoints;
  const auto forward_end = std::find_if(points.begin(), points.end(), [&](const auto& point) {
    return point.time_from_start_s >= return_start - 1e-12;
  });
  ASSERT_NE(forward_end, points.end());
  for (size_t j = 0; j < accepted.joint_names.size(); ++j)
    EXPECT_NEAR(forward_end->positions_rad[j], path_at(stop).positions_rad[j], 1e-9);
  double previous_u = stop;
  size_t return_knots = 0;
  for (auto it = std::next(forward_end); it != points.end(); ++it) {
    ++return_knots;
    double lo = trip;
    double hi = stop;
    for (int i = 0; i < 200; ++i) {
      const double mid = 0.5 * (lo + hi);
      (path_at(mid).positions_rad[0] < it->positions_rad[0] ? lo : hi) = mid;
    }
    const double u = 0.5 * (lo + hi);
    EXPECT_LE(u, previous_u + 1e-9);
    previous_u = u;
    const auto on_path = path_at(u);
    for (size_t j = 0; j < accepted.joint_names.size(); ++j) {
      EXPECT_NEAR(it->positions_rad[j], on_path.positions_rad[j], 1e-9) << "joint " << j;
      // Reversed along this occurrence: velocity opposes the path tangent.
      EXPECT_LE(it->velocities_rad_s[j] * on_path.velocities_rad_s[j], 1e-12) << "joint " << j;
    }
  }
  EXPECT_GT(return_knots, 1U);
  const auto at_trip = path_at(trip);
  for (size_t j = 0; j < accepted.joint_names.size(); ++j) {
    EXPECT_DOUBLE_EQ(points.back().positions_rad[j], at_trip.positions_rad[j]);
    EXPECT_DOUBLE_EQ(points.back().velocities_rad_s[j], 0.0);
  }
}

TEST(SpringBackOnPathStopPlannerTest, ReturnStaysOnTheOriginalPathAtTheTripOccurrence) {
  // The path passes the trip pose three times (u = 0.5, 1.5, 2.5). The trip
  // is the third occurrence; the return must retrace that one.
  const TrajectoryDTO retracing{{ResourceId("joint.1"), ResourceId("joint.2")},
                                {"joint_1", "joint_2"},
                                {{0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                                 {1.0, {1.0, 0.5}, {0.0, 0.0}, {0.0, 0.0}},
                                 {2.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                                 {3.0, {1.0, 0.5}, {0.0, 0.0}, {0.0, 0.0}}},
                                TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0}};
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.5, 0.25};
  state.trajectory_time_from_start_s = 2.5;
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.runtime_velocity_scale_at_trip = 0.6;
  SoftOnPathPlannerConfig config;
  config.allow_model_velocity_fallback = true;
  config.jerk_limits_enabled = false;
  const auto quintic =
      plan_spring_back_on_path_stop(retracing, spring_back_profile(1.0), state, config);
  ASSERT_TRUE(quintic.ok()) << quintic.error().message;
  EXPECT_EQ(interrupt_structured_scalar_text(
                quintic.value().diagnostics.at("spring_back_return_profile")),
            "quintic_rest_to_rest");
  EXPECT_DOUBLE_EQ(quintic.value().command_path_time_s, 2.5);
  ExpectReturnRetracesTheOriginalPath(retracing, quintic.value());

  // The rate-capped return samples the same path the same way.
  const auto capped =
      plan_spring_back_on_path_stop(prepared_cruise_trajectory(), spring_back_profile(0.2),
                                    cruise_trip_state(), cruise_trip_config());
  ASSERT_TRUE(capped.ok()) << capped.error().message;
  EXPECT_EQ(
      interrupt_structured_scalar_text(capped.value().diagnostics.at("spring_back_return_profile")),
      "ramp_cruise_ramp_rate_capped");
  ExpectReturnRetracesTheOriginalPath(prepared_cruise_trajectory(), capped.value());
}

TEST(SpringBackOnPathStopPlannerTest, ReturnUsesTheExactOccurrenceOnARetracingPath) {
  const TrajectoryDTO trajectory{{ResourceId("joint.1")},
                                 {"joint_1"},
                                 {{0.0, {0.0}, {0.0}, {0.0}},
                                  {1.0, {1.0}, {0.0}, {0.0}},
                                  {2.0, {0.0}, {0.0}, {0.0}},
                                  {3.0, {1.0}, {0.0}, {0.0}}},
                                 TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0}};
  auto state = measured_state_1d(0.5, 0.2);
  state.state.velocities_rad_s.clear();
  state.trajectory_time_from_start_s = 2.5;
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  SoftOnPathPlannerConfig config;
  config.allow_model_velocity_fallback = true;
  config.jerk_limits_enabled = false;
  const auto result =
      plan_spring_back_on_path_stop(trajectory, spring_back_profile(1.0), state, config);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_DOUBLE_EQ(result.value().command_path_time_s, 2.5);
  EXPECT_GT(result.value().forward_stop_path_time_s, 2.5);
  EXPECT_LT(result.value().forward_stop_path_time_s, 3.0);
  EXPECT_NEAR(result.value().replacement_trajectory.waypoints.back().positions_rad.front(), 0.5,
              1e-12);
}

TEST(JerkLimitedStopProfileTest, RisingSeamAccelerationCannotOverspeedTheEnvelope) {
  // The public retimer still accepts a seam path acceleration; a rising one
  // that would push the path rate above 1 is never dispatchable.
  const auto fallback =
      compute_jerk_limited_stop_profile(prepared_cruise_trajectory(), 1.7, 0.9, 1.6, 1.0,
                                        {{"joint_1", 100.0}}, {{"joint_1", 2.0}}, 0.01);
  ASSERT_TRUE(fallback.ok()) << fallback.error().message;
  EXPECT_FALSE(fallback.value().dispatchable);
}

TEST(SoftOnPathStopPlannerTest, BuildsDerivativeCompleteWallTimeRampFromMeasuredScaledVelocity) {
  auto profile = soft_profile(0.5);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  EXPECT_FALSE(plan.used_model_velocity);
  EXPECT_EQ(plan.velocity_source, "measured_feedback");
  EXPECT_NEAR(plan.effective_ramp_s, 0.45, 1e-9);
  ASSERT_GE(plan.replacement_trajectory.waypoints.size(), 3u);
  const auto& anchor = plan.replacement_trajectory.waypoints.front();
  EXPECT_NEAR(anchor.velocities_rad_s.at(0), 0.5, 1e-9);
  const auto& first_future = plan.replacement_trajectory.waypoints[1];
  EXPECT_NEAR(first_future.time_from_start_s, 0.05, 1e-9);
  const auto& final_wp = plan.replacement_trajectory.waypoints.back();
  EXPECT_NEAR(final_wp.time_from_start_s, 0.5, 1e-9);
  EXPECT_NEAR(final_wp.velocities_rad_s.at(0), 0.0, 1e-9);
  EXPECT_NEAR(final_wp.accelerations_rad_s2.at(0), 0.0, 1e-9);

  // As main (item 24): 50 ms handoff at 0.5 rad/s inside the 500 ms
  // deceleration time, then a smooth decel whose average path-time speed is
  // 0.25 rad/s for the remaining 450 ms.
  EXPECT_NEAR(final_wp.positions_rad.at(0), 0.3375, 1e-9);
}

TEST(SoftOnPathStopPlannerTest, OneSecondOperatorCurveHasExactMidpointAndEndpointDerivatives) {
  auto profile = soft_profile(1.0);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  EXPECT_NEAR(plan.effective_ramp_s, 0.95, 1e-9);

  const auto midpoint_it = std::find_if(plan.replacement_trajectory.waypoints.begin(),
                                        plan.replacement_trajectory.waypoints.end(),
                                        [](const TrajectoryWaypointDTO& waypoint) {
                                          return std::abs(waypoint.time_from_start_s - 0.5) < 1e-9;
                                        });
  ASSERT_NE(midpoint_it, plan.replacement_trajectory.waypoints.end());

  // The first 50 ms continues the accepted command inside the 1 s
  // deceleration time (item 24). At 500 ms wall time the 950 ms ramp has
  // advanced 450 ms.
  const double ramp_s = 0.95;
  const double r = 0.45 / ramp_s;
  const double r2 = r * r;
  const double r3 = r2 * r;
  const double r4 = r2 * r2;
  const double r5 = r4 * r;
  const double r6 = r5 * r;
  const double position_factor = r - 2.5 * r4 + 3.0 * r5 - r6;
  const double velocity_factor = 1.0 - 10.0 * r3 + 15.0 * r4 - 6.0 * r5;
  const double acceleration_factor = -30.0 * r2 + 60.0 * r3 - 30.0 * r4;
  const double handoff_position = 0.2 + 0.5 * 0.05;

  EXPECT_NEAR(midpoint_it->positions_rad.at(0), handoff_position + 0.5 * ramp_s * position_factor,
              1e-9);
  EXPECT_NEAR(midpoint_it->velocities_rad_s.at(0), 0.5 * velocity_factor, 1e-9);
  EXPECT_NEAR(midpoint_it->accelerations_rad_s2.at(0), 0.5 / ramp_s * acceleration_factor, 1e-9);

  const auto& endpoint = plan.replacement_trajectory.waypoints.back();
  EXPECT_NEAR(endpoint.time_from_start_s, 1.0, 1e-9);
  EXPECT_NEAR(endpoint.positions_rad.at(0), handoff_position + 0.5 * ramp_s * 0.5, 1e-9);
  EXPECT_NEAR(endpoint.velocities_rad_s.at(0), 0.0, 1e-9);
  EXPECT_NEAR(endpoint.accelerations_rad_s2.at(0), 0.0, 1e-9);
}

TEST(SpringBackOnPathStopPlannerTest, BuildsTwoPhaseOnPathReturnToTripCursor) {
  auto profile = spring_back_profile(1.0);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 1e6}, {"joint_2", 1e6}};

  auto planned = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  EXPECT_EQ(interrupt_structured_scalar_text(plan.diagnostics.at("spring_back_mode")),
            "on_path_two_phase");
  EXPECT_EQ(interrupt_structured_scalar_text(plan.diagnostics.at("spring_back_return_profile")),
            "quintic_rest_to_rest");
  ASSERT_GE(plan.replacement_trajectory.waypoints.size(), 4u);
  const auto& first = plan.replacement_trajectory.waypoints.front();
  const auto& last = plan.replacement_trajectory.waypoints.back();
  EXPECT_NEAR(first.time_from_start_s, 0.0, 1e-9);
  EXPECT_NEAR(last.time_from_start_s, 1.0, 1e-9);
  EXPECT_NEAR(last.positions_rad.at(0), first.positions_rad.at(0), 1e-9);
  EXPECT_NEAR(last.velocities_rad_s.at(0), 0.0, 1e-9);
  EXPECT_NEAR(last.accelerations_rad_s2.at(0), 0.0, 1e-9);

  double max_position = -1.0;
  bool saw_forward_motion = false;
  bool saw_return_motion = false;
  double previous_time = -1.0;
  double previous_position = first.positions_rad.at(0);
  for (const auto& wp : plan.replacement_trajectory.waypoints) {
    EXPECT_GT(wp.time_from_start_s, previous_time);
    previous_time = wp.time_from_start_s;
    max_position = std::max(max_position, wp.positions_rad.at(0));
    if (wp.positions_rad.at(0) > previous_position + 1e-9) {
      saw_forward_motion = true;
    }
    if (wp.positions_rad.at(0) < previous_position - 1e-9) {
      saw_return_motion = true;
    }
    previous_position = wp.positions_rad.at(0);
  }
  EXPECT_TRUE(saw_forward_motion);
  EXPECT_TRUE(saw_return_motion);
  EXPECT_GT(max_position, first.positions_rad.at(0) + 0.02);

  const auto seam_it = std::find_if(
      plan.replacement_trajectory.waypoints.begin(), plan.replacement_trajectory.waypoints.end(),
      [](const TrajectoryWaypointDTO& wp) { return std::abs(wp.time_from_start_s - 0.5) < 1e-9; });
  ASSERT_NE(seam_it, plan.replacement_trajectory.waypoints.end());
  ASSERT_FALSE(seam_it->velocities_rad_s.empty());
  ASSERT_FALSE(seam_it->accelerations_rad_s2.empty());
  EXPECT_NEAR(seam_it->velocities_rad_s.at(0), 0.0, 1e-9);
  EXPECT_NEAR(seam_it->accelerations_rad_s2.at(0), 0.0, 1e-9);
  const auto next_it = std::next(seam_it);
  ASSERT_NE(next_it, plan.replacement_trajectory.waypoints.end());
  EXPECT_LT(next_it->velocities_rad_s.at(0), 0.0);
}

TEST(SpringBackJoinTest, LateForwardBrakingShiftsTheReturnWhileTheStopFitsItsDeadline) {
  // Item 6 at-rest join: early or on-time braking joins at the half boundary.
  EXPECT_DOUBLE_EQ(*detail::spring_back_return_start_s(0.5, 0.5, 0.5, 1.0), 0.5);
  EXPECT_DOUBLE_EQ(*detail::spring_back_return_start_s(0.47, 0.5, 0.5, 1.0), 0.5);
  // Late braking: the return starts after it, never overlapping it ...
  EXPECT_DOUBLE_EQ(*detail::spring_back_return_start_s(0.52, 0.5, 0.5, 1.05), 0.52);
  EXPECT_DOUBLE_EQ(*detail::spring_back_return_start_s(0.52, 0.5, 0.5, 1.02), 0.52);
  // ... as long as the whole Stop still fits its deadline; otherwise hard stop.
  EXPECT_FALSE(detail::spring_back_return_start_s(0.52, 0.5, 0.5, 1.019).has_value());
  EXPECT_FALSE(detail::spring_back_return_start_s(0.52, 0.5, 0.5, 1.0).has_value());
  EXPECT_FALSE(detail::spring_back_return_start_s(0.5, 0.5, 0.5, 0.99).has_value());
  EXPECT_FALSE(
      detail::spring_back_return_start_s(std::numeric_limits<double>::quiet_NaN(), 0.5, 0.5, 1.0)
          .has_value());
}

TEST(SpringBackOnPathStopPlannerTest, WholeStopMustFitItsDeadline) {
  auto profile = spring_back_profile(1.0);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;
  config.stop_deadline_s = 1.0;  // exactly the requested deceleration time
  auto fits = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(fits.ok()) << fits.error().message;
  EXPECT_NEAR(fits.value().replacement_trajectory.waypoints.back().time_from_start_s, 1.0, 1e-12);

  config.stop_deadline_s = 0.99;
  auto late = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(late.ok());
  EXPECT_EQ(late.error().details.at("reason"), "spring_back_stop_deadline_exceeded");
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(late.error()));

  config.stop_deadline_s = std::numeric_limits<double>::infinity();
  auto invalid = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().details.at("reason"), "invalid_stop_deadline_s");
}

TEST(SpringBackOnPathStopPlannerTest, RejectsWhenForwardHalfCannotContainLeadIn) {
  auto profile = spring_back_profile(0.1);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;

  auto planned = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "spring_back_phase_time_not_larger_than_handoff");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
}

TEST(SpringBackOnPathStopPlannerTest, ForwardPhaseFailureEscalatesToHardStop) {
  auto profile = spring_back_profile(0.5);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{0.01}}};
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 1e6}};

  auto planned = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("spring_back_phase"), "forward_deceleration");
  EXPECT_EQ(planned.error().details.at("requested_stop_mode"), "spring_back_hold");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
  EXPECT_FALSE(planned.error().details.contains("recommended_fallback"));
}

TEST(SpringBackOnPathStopPlannerTest, RejectsReturnAccelerationLimitExceeded) {
  auto profile = spring_back_profile(1.0);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{3.0}}};
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 1e6}};

  auto planned = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "spring_back_return_acceleration_limit_exceeded");
  EXPECT_EQ(planned.error().details.at("spring_back_phase"), "reverse_return");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
}

TEST(SpringBackOnPathStopPlannerTest, RejectsReturnJerkLimitExceeded) {
  auto profile = spring_back_profile(1.0);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 40.0}};

  auto planned = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "spring_back_return_jerk_limit_exceeded");
  EXPECT_EQ(planned.error().details.at("spring_back_phase"), "reverse_return");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
}

TEST(SoftOnPathStopPlannerTest, CommandFrameSeamUsesOnPathVelocityAndReportsMeasuredDrift) {
  // Under the command-frame lead-in the seam continues the on-path COMMAND, not
  // the measured off-path velocity. A measured joint_2 velocity the path does not
  // have is a tracking error: it is reported for the drift guard / audit, but is
  // NOT injected into the seam (the old synthetic handoff did inject it, which is
  // what created the handoff excursion this seam removes).
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration = {
      {"joint_1", AngularAccelerationDTO{1000.0}},
      {"joint_2", AngularAccelerationDTO{1000.0}},
  };
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.2, 0.0};
  state.state.velocities_rad_s = {0.5, 1.0};
  state.runtime_velocity_scale_at_trip = 0.5;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;
  config.per_joint_max_jerk_rad_s3 = {
      {"joint_1", 1e9},
      {"joint_2", 1e9},
  };

  auto planned = plan_soft_on_path_stop(linear_trajectory_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  ASSERT_GE(plan.replacement_trajectory.waypoints.size(), 2u);
  EXPECT_EQ(interrupt_structured_scalar_text(plan.diagnostics.at("seam")), "command_frame_lead_in");
  // Seam velocity is on-path: the off-path measured joint_2 velocity (1.0) is
  // NOT carried into the command.
  EXPECT_NEAR(plan.replacement_trajectory.waypoints.front().velocities_rad_s.at(1), 0.0, 1e-9);
  // The measured deviation is still measured and reported (drift guard input).
  EXPECT_NEAR(plan.initial_velocity_deviation_rad_s, 1.0, 1e-9);
  EXPECT_EQ(
      interrupt_structured_scalar_text(plan.diagnostics.at("initial_velocity_deviation_rad_s")),
      "1");
  // The lead-in rides the accepted path, so it adds no handoff excursion.
  EXPECT_NEAR(plan.path_deviation_max_rad, plan.initial_path_deviation_rad, 1e-9);
}

TEST(SoftOnPathStopPlannerTest, CanRejectMeasuredVelocityThatIsNotPathCoherent) {
  auto profile = soft_profile(0.5);
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.2, 0.0};
  state.state.velocities_rad_s = {0.5, 1.0};
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;
  config.max_initial_velocity_deviation_rad_s = 0.1;

  auto planned = plan_soft_on_path_stop(linear_trajectory_2d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "initial_velocity_deviation_exceeds_limit");
  // Item 6: a deviation trigger goes straight to the hard stop.
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(planned.error()));
  EXPECT_FALSE(planned.error().details.contains("recommended_fallback"));
}

TEST(SoftOnPathStopPlannerTest, UsesModelDerivedVelocityOnlyWhenExplicitlyAuthorized) {
  auto profile = soft_profile(0.5);
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1"};
  state.state.positions_rad = {0.2};
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.trajectory_time_from_start_s = 0.2;
  state.runtime_velocity_scale_at_trip = 0.25;

  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto rejected = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "model_velocity_not_authorized");

  config.allow_model_velocity_fallback = true;
  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_TRUE(planned.value().used_model_velocity);
  EXPECT_EQ(planned.value().velocity_source, "model_derived");
  EXPECT_NEAR(planned.value().replacement_trajectory.waypoints.back().positions_rad.at(0), 0.26875,
              1e-9);
}

TEST(SoftOnPathStopPlannerTest, CommandFrameSeamRidesProjectedPathNotMeasuredDrift) {
  // Regression for the handoff-jerk escalation: the command-frame lead-in seam
  // rides the on-path COMMAND (the accepted trajectory resampled from the trip
  // cursor), so even with an off-path measured position the seam's first
  // waypoint is the projected on-path point — never the raw measured drift that
  // the old synthetic handoff amplified into a limit-breaking jerk. The measured
  // drift is still measured and reported for the drift guard.
  auto profile = soft_profile(0.5);
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.2, 0.1};  // joint_2 = 0.1 is off the path
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.trajectory_time_from_start_s = 0.2;
  state.runtime_velocity_scale_at_trip = 0.5;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;
  config.allow_model_velocity_fallback = true;

  auto planned = plan_soft_on_path_stop(linear_trajectory_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  EXPECT_TRUE(plan.used_model_velocity);
  EXPECT_EQ(interrupt_structured_scalar_text(plan.diagnostics.at("seam")), "command_frame_lead_in");
  // The seam's first waypoint is the projected ON-PATH point: joint_2 snapped to
  // the path (0.0), not the measured 0.1 drift.
  const auto& anchor = plan.replacement_trajectory.waypoints.front();
  EXPECT_NEAR(anchor.positions_rad.at(1), 0.0, 1e-9);
}

TEST(SoftOnPathStopPlannerTest, SeamAnchorsAtCommandCursorNotLaggingMeasured) {
  // Command-frame authority: when the measured position lags the command cursor
  // (the controller has commanded further along the path than the robot has
  // physically reached), the lead-in must open at the COMMAND cursor, not the
  // lagging measured-position projection — otherwise the seam re-issues points
  // the controller's command has already passed (a backward step at the seam).
  // q(t) = t on linear_trajectory_1d, so the cursor maps directly to position.
  auto profile = soft_profile(0.5);
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1"};
  state.state.positions_rad = {0.2};  // measured projects to t = 0.2
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.trajectory_time_from_start_s = 0.3;  // command cursor is ahead (0.1 lag)
  state.runtime_velocity_scale_at_trip = 1.0;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;
  config.allow_model_velocity_fallback = true;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  // Lead-in opens at the command cursor (q(0.3) = 0.3), not the measured 0.2.
  EXPECT_NEAR(plan.replacement_trajectory.waypoints.front().positions_rad.at(0), 0.3, 1e-9);
  EXPECT_NEAR(plan.initial_path_deviation_rad, 0.0, 1e-9);  // measured is on-path
}

TEST(SoftOnPathStopPlannerTest, LeadInConsumesTheDecelerationTime) {
  // As main (item 24): the lead-in sits inside the deceleration time.
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), soft_profile(0.5), state, config);
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_DOUBLE_EQ(planned.value().effective_ramp_s, 0.45);
  EXPECT_NEAR(planned.value().replacement_trajectory.waypoints.back().time_from_start_s, 0.5,
              1e-12);
}

TEST(SoftOnPathStopPlannerTest, RejectsWhenHandoffConsumesDecelerationWindow) {
  auto profile = soft_profile(0.05);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "deceleration_time_not_larger_than_handoff");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
  EXPECT_FALSE(planned.error().details.contains("recommended_fallback"));
}

TEST(SoftOnPathStopPlannerTest, UsesJerkLimitedRetimerWhenQuinticJerkWouldExceedLimit) {
  // On a straight path, a symmetric S-curve can keep jerk lower than the
  // default quintic stop ramp for the same exact stop time. This proves the
  // planner's successful retimer wiring, not just the standalone helper.
  auto profile = soft_profile(1.0);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{3.0}}};
  auto state = measured_state_1d(0.0, 1.0);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.0;
  config.sample_period_s = 0.01;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 4.5}};

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_EQ(
      interrupt_structured_scalar_text(planned.value().diagnostics.at("soft_on_path_retimer")),
      "jerk_limited");
  EXPECT_EQ(
      interrupt_structured_scalar_text(planned.value().diagnostics.at("retimer_trigger_reason")),
      "soft_on_path_jerk_limit_exceeded");
  const auto& final_wp = planned.value().replacement_trajectory.waypoints.back();
  EXPECT_NEAR(final_wp.time_from_start_s, 1.0, 2e-2);
  EXPECT_NEAR(final_wp.velocities_rad_s.at(0), 0.0, 1e-9);
  EXPECT_NEAR(final_wp.accelerations_rad_s2.at(0), 0.0, 1e-9);
}

TEST(SoftOnPathStopPlannerTest, EscalatesAccelBindWhenRetimerCannotProveDispatch) {
  // s_trip=0.5, D=0.5 -> T_min=1.0 s; a 1.15 s stop passes the exact-time gate,
  // but the quintic's peak path-rate deceleration (~0.85 rad/s^2) exceeds D.
  // The jerk-limited retimer is tried, but if it cannot prove a dispatchable
  // derivative-complete replacement, the planner escalates to hard stop rather
  // than emitting a limit-violating ramp or silently springing back.
  auto profile = soft_profile(1.15);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{0.5}}};
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "soft_on_path_retimer_not_dispatchable");
  EXPECT_EQ(planned.error().details.at("trigger_reason"),
            "soft_on_path_acceleration_limit_exceeded");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
  EXPECT_FALSE(planned.error().details.contains("recommended_fallback"));
  EXPECT_NEAR(std::stod(planned.error().details.at("handoff_path_time_s")), 0.225, 1e-12);
  EXPECT_TRUE(planned.error().details.contains("ali_validation_failure_reason"));
  EXPECT_TRUE(planned.error().details.contains("ali_motion_stop_time_s"));
}

TEST(SoftOnPathStopPlannerTest, UsesAliBacktrackingToStopBeforeFutureCornerAndHold) {
  // The exact-time quintic and jerk-limited retimer would ride into the curved
  // corner. ALI/backtracking may legally stop the moving portion earlier and
  // hold to the requested stop horizon, as long as the emitted trajectory still
  // passes the same acceleration/jerk validation.
  auto profile = soft_profile(1.0);
  profile.per_joint_max_deceleration = {
      {"joint_1", AngularAccelerationDTO{5.0}},
      {"joint_2", AngularAccelerationDTO{5.0}},
  };
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.0, 0.0};
  state.state.velocities_rad_s = {1.0, 0.0};
  state.trajectory_time_from_start_s = 0.0;
  state.runtime_velocity_scale_at_trip = 1.0;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.0;
  config.sample_period_s = 0.01;
  config.per_joint_max_jerk_rad_s3 = {
      {"joint_1", 12.0},
      {"joint_2", 12.0},
  };

  auto planned = plan_soft_on_path_stop(curved_corner_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  EXPECT_EQ(interrupt_structured_scalar_text(plan.diagnostics.at("soft_on_path_retimer")),
            "ali_backtracking");
  EXPECT_EQ(interrupt_structured_scalar_text(plan.diagnostics.at("prior_retimer")), "jerk_limited");
  EXPECT_EQ(
      interrupt_structured_scalar_text(plan.diagnostics.at("ali_stopped_before_requested_time")),
      "true");
  ASSERT_GE(plan.replacement_trajectory.waypoints.size(), 3u);
  const auto& final_wp = plan.replacement_trajectory.waypoints.back();
  EXPECT_NEAR(final_wp.time_from_start_s, 1.0, 1e-9);
  EXPECT_LT(final_wp.positions_rad.at(0), 0.3);
  EXPECT_NEAR(final_wp.positions_rad.at(1), 0.0, 1e-9);
  EXPECT_NEAR(final_wp.velocities_rad_s.at(0), 0.0, 1e-9);
  EXPECT_NEAR(final_wp.velocities_rad_s.at(1), 0.0, 1e-9);
  EXPECT_NEAR(final_wp.accelerations_rad_s2.at(0), 0.0, 1e-9);
  EXPECT_NEAR(final_wp.accelerations_rad_s2.at(1), 0.0, 1e-9);
}

TEST(SoftOnPathStopPlannerTest, MeasuresInitialPathDriftAndCanEnforceDeploymentLimit) {
  auto profile = soft_profile(0.5);
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.2, 0.1};
  state.state.velocities_rad_s = {0.5, 0.0};
  state.runtime_velocity_scale_at_trip = 0.5;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_NEAR(planned.value().initial_path_deviation_rad, 0.1, 1e-9);
  EXPECT_NEAR(planned.value().path_deviation_max_rad, 0.1, 1e-9);
  EXPECT_EQ(interrupt_structured_scalar_text(planned.value().diagnostics.at("position_source")),
            "measured_feedback");

  config.max_initial_path_deviation_rad = 0.05;
  auto rejected = plan_soft_on_path_stop(linear_trajectory_2d(), profile, state, config);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "initial_path_deviation_exceeds_limit");
  // Item 6: a deviation trigger goes straight to the hard stop, never to the
  // spring-back fallback, even though this profile allows that fallback.
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(rejected.error()));
  EXPECT_FALSE(rejected.error().details.contains("recommended_fallback"));
}

TEST(SpringBackOnPathStopPlannerTest, DeviationTriggersEscalateToHardStop) {
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.2, 0.1};
  state.state.velocities_rad_s = {0.5, 1.0};
  SoftOnPathPlannerConfig path_config;
  path_config.max_initial_path_deviation_rad = 0.05;
  SoftOnPathPlannerConfig velocity_config;
  velocity_config.max_initial_velocity_deviation_rad_s = 0.1;
  for (const auto& [config, reason] :
       {std::pair{path_config, "initial_path_deviation_exceeds_limit"},
        std::pair{velocity_config, "initial_velocity_deviation_exceeds_limit"}}) {
    SCOPED_TRACE(reason);
    auto profile = spring_back_profile(1.0);
    const auto result =
        plan_spring_back_on_path_stop(linear_trajectory_2d(), profile, state, config);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"), reason);
    EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(result.error()));
    EXPECT_FALSE(result.error().details.contains("recommended_fallback"));
  }
}

TEST(SoftOnPathStopPlannerTest, MeasuresFeedbackPathDeviationWithJointNameOrdering) {
  RobotStateDTO observed;
  observed.joint_names = {"joint_2", "joint_1"};
  observed.positions_rad = {0.25, 0.4};

  auto measured = measure_soft_on_path_path_deviation(linear_trajectory_2d(), observed);

  ASSERT_TRUE(measured.ok()) << measured.error().message;
  EXPECT_NEAR(measured.value(), 0.25, 1e-9);
}

TEST(StopPhaseJerkLimitsTest, AuthoredJerkIsVerbatimAndMissingJerkUsesMainsDefault) {
  // Decision Q36, as main (soft 1073-1086, spring-back 1491-1499).
  const auto trajectory = linear_trajectory_2d();
  const std::map<std::string, double> accel{{"joint_1", 4.0}, {"joint_2", 6.0}};
  SoftOnPathPlannerConfig config;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 12.5}};
  for (const double reference_s : {0.45, 0.5, 0.08}) {
    SCOPED_TRACE(reference_s);
    const auto mixed = detail::stop_phase_jerk_limits(trajectory, accel, config, reference_s);
    EXPECT_EQ(mixed.source, "mixed");
    EXPECT_DOUBLE_EQ(mixed.limits.at("joint_1"), 12.5);
    // main's default_jerk_limit: accel / min(max(0.15 T, 0.02), T / 3).
    const double ramp = std::min(std::max(0.15 * reference_s, 0.02), reference_s / 3.0);
    EXPECT_DOUBLE_EQ(mixed.limits.at("joint_2"), 6.0 / ramp);
  }
  config.per_joint_max_jerk_rad_s3.clear();
  EXPECT_EQ(detail::stop_phase_jerk_limits(trajectory, accel, config, 0.45).source, "default");
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 1.0}, {"joint_2", 2.0}};
  EXPECT_EQ(detail::stop_phase_jerk_limits(trajectory, accel, config, 0.45).source, "deployment");
  config.jerk_limits_enabled = false;
  const auto off = detail::stop_phase_jerk_limits(trajectory, accel, config, 0.45);
  EXPECT_EQ(off.source, "disabled");
  EXPECT_TRUE(off.limits.empty());
}

TEST(SoftOnPathStopPlannerTest, MissingJerkIsDerivedPerPhaseFromTheLanesLeadIn) {
  auto state = measured_state_1d(0.2, 0.1);
  SoftOnPathPlannerConfig config;
  config.sample_period_s = 0.05;
  for (const double lead_in_s : {0.02, 0.05, 0.1}) {
    SCOPED_TRACE(lead_in_s);
    config.stop_traj_delta_s = lead_in_s;
    // Soft ramp: deceleration time minus this lane's lead-in.
    const auto soft =
        plan_soft_on_path_stop(linear_trajectory_1d(), soft_profile(0.5), state, config);
    ASSERT_TRUE(soft.ok()) << soft.error().message;
    EXPECT_EQ(soft.value().jerk_limit_source, "default");
    EXPECT_DOUBLE_EQ(std::stod(interrupt_structured_scalar_text(
                         soft.value().diagnostics.at("jerk_reference_s"))),
                     0.5 - lead_in_s);
    // Spring-back: forward half minus the lead-in, return half on its own.
    const auto spring = plan_spring_back_on_path_stop(linear_trajectory_1d(),
                                                      spring_back_profile(1.0), state, config);
    ASSERT_TRUE(spring.ok()) << spring.error().message;
    EXPECT_DOUBLE_EQ(std::stod(interrupt_structured_scalar_text(
                         spring.value().diagnostics.at("jerk_reference_s"))),
                     0.5 - lead_in_s);
    EXPECT_DOUBLE_EQ(std::stod(interrupt_structured_scalar_text(
                         spring.value().diagnostics.at("spring_back_return_jerk_reference_s"))),
                     0.5);
  }
  config.stop_traj_delta_s = 0.05;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 100.0}};
  const auto authored =
      plan_soft_on_path_stop(linear_trajectory_1d(), soft_profile(0.5), state, config);
  ASSERT_TRUE(authored.ok()) << authored.error().message;
  EXPECT_EQ(authored.value().jerk_limit_source, "deployment");
  config.jerk_limits_enabled = false;
  const auto off = plan_soft_on_path_stop(linear_trajectory_1d(), soft_profile(0.5), state, config);
  ASSERT_TRUE(off.ok()) << off.error().message;
  EXPECT_EQ(off.value().jerk_limit_source, "disabled");
}

TEST(SoftOnPathStopPlannerTest, RejectsRampThatWouldPassAcceptedTrajectoryEnd) {
  auto profile = soft_profile(0.5);
  profile.recoverable_substitution_mode.reset();
  profile.allow_natural_completion_on_short_path = false;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "soft_on_path_remaining_time_short");
  EXPECT_EQ(planned.error().details.at("recommended_fallback"), "spring_back_hold");
}

TEST(SoftOnPathStopPlannerTest, RecommendsNaturalCompletionWhenShortPathPolicyAllowsIt) {
  // main's predicate (item 2): the ramp overruns the accepted path end.
  auto profile = soft_profile(0.5);
  profile.recoverable_substitution_mode.reset();
  profile.allow_natural_completion_on_short_path = true;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "soft_on_path_remaining_time_short");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "natural_completion");
  EXPECT_FALSE(planned.error().details.contains("recommended_fallback"));

  // A ramp that ends on the path is not natural completion, flag or not.
  auto stoppable =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, measured_state_1d(0.5, 0.8), config);
  ASSERT_TRUE(stoppable.ok()) << stoppable.error().message;
}

TEST(SpringBackOnPathStopPlannerTest, NaturalCompletionIsNotReCheckedInsideSpringBack) {
  // Item 2: natural completion is decided once, before spring-back. A
  // spring-back whose forward half would overrun the path end is a hard stop,
  // whatever the profile's natural-completion flag says.
  for (const bool allow : {false, true}) {
    SCOPED_TRACE(allow);
    auto profile = spring_back_profile(1.0);
    profile.allow_natural_completion_on_short_path = allow;
    SoftOnPathPlannerConfig config;
    config.stop_traj_delta_s = 0.05;
    config.sample_period_s = 0.05;
    const auto result = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile,
                                                      measured_state_1d(0.9, 0.8), config);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("spring_back_phase"), "forward_deceleration");
    EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(result.error()));
    EXPECT_FALSE(soft_on_path_error_recommends_natural_completion(result.error()));
    EXPECT_FALSE(result.error().details.contains("recommended_fallback"));
  }
}

TEST(SoftOnPathStopPlannerTest, MissingRuntimeScaleBoundsIsRefused) {
  auto trajectory = linear_trajectory_1d();
  trajectory.runtime_scale_bounds.reset();
  SoftOnPathPlannerConfig config;
  config.jerk_limits_enabled = false;
  const auto soft =
      plan_soft_on_path_stop(trajectory, soft_profile(0.5), measured_state_1d(0.2, 0.5), config);
  ASSERT_FALSE(soft.ok());
  EXPECT_EQ(soft.error().details.at("reason"), "runtime_scale_bounds_required");
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(soft.error()));
  const auto spring = plan_spring_back_on_path_stop(trajectory, spring_back_profile(1.0),
                                                    measured_state_1d(0.2, 0.5), config);
  ASSERT_FALSE(spring.ok());
  EXPECT_EQ(spring.error().details.at("reason"), "runtime_scale_bounds_required");
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(spring.error()));

  trajectory = linear_trajectory_1d();
  trajectory.runtime_scale_bounds->max_stop_path_rate = 0.9;
  const auto invalid =
      plan_soft_on_path_stop(trajectory, soft_profile(0.5), measured_state_1d(0.2, 0.5), config);
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().details.at("reason"), "runtime_scale_bounds_invalid");
}

TEST(SpringBackOnPathStopPlannerTest, DecelerationTimeIsRequiredNotDefaultedByThePlanner) {
  auto profile = spring_back_profile(1.0);
  profile.deceleration_time_s.reset();
  SoftOnPathPlannerConfig config;
  config.jerk_limits_enabled = false;
  const auto result = plan_spring_back_on_path_stop(linear_trajectory_1d(), profile,
                                                    measured_state_1d(0.2, 0.5), config);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "spring_back_deceleration_time_required");
  EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(result.error()));
}

// --- Jerk-continuity of the stop ramp ---
//
// The directive: the stop trajectory must be acceleration- AND jerk-continuous
// at the seam (r=0, matching the steady pre-stop traversal where path-time
// accel and jerk are zero) and at the terminus (r=1, at rest). With the
// minimum-jerk quintic time-scaling factor, both the path-time acceleration
// and jerk vanish at r=0 and r=1, so the finite-difference jerk of the
// generated joint trajectory tends to zero at both ends as the sampling
// refines, and stays bounded throughout.
TEST(SoftOnPathStopPlannerTest, RampIsAccelerationAndJerkContinuousAtSeamAndTerminus) {
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{1000.0}},
                                        {"joint_2", AngularAccelerationDTO{1000.0}}};
  SoftOnPathInitialStateDTO state = measured_state_1d(0.2, 1.0);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.0;  // no handoff offset: r=0 sample is the seam
  config.sample_period_s = 0.001;  // fine sampling for finite-difference checks
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 1e9}};

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& wps = planned.value().replacement_trajectory.waypoints;
  ASSERT_GE(wps.size(), 4u);

  // Endpoints: zero velocity AND zero acceleration at the terminus; the seam
  // carries the incoming velocity and (for a straight path at steady rate)
  // near-zero acceleration.
  EXPECT_NEAR(wps.back().velocities_rad_s.at(0), 0.0, 1e-9);
  EXPECT_NEAR(wps.back().accelerations_rad_s2.at(0), 0.0, 1e-9);
  EXPECT_NEAR(wps.front().accelerations_rad_s2.at(0), 0.0, 1e-6);

  // Finite-difference jerk across the ramp; assert it is bounded and that the
  // first and last jerk samples (seam + terminus) are near zero — the property
  // the cubic factor violated (it had |jerk| ~ const != 0 at the ends).
  std::vector<double> jerk;
  for (size_t i = 1; i < wps.size(); ++i) {
    const double dt = wps[i].time_from_start_s - wps[i - 1].time_from_start_s;
    if (dt <= 1e-9) continue;
    jerk.push_back((wps[i].accelerations_rad_s2.at(0) - wps[i - 1].accelerations_rad_s2.at(0)) /
                   dt);
  }
  ASSERT_GE(jerk.size(), 3u);
  double max_abs_jerk = 0.0;
  for (double j : jerk) max_abs_jerk = std::max(max_abs_jerk, std::abs(j));
  EXPECT_LT(max_abs_jerk, 1e6);  // bounded
  // Seam and terminus jerk → 0 (within a tolerance scaled by the fine dt).
  EXPECT_LT(std::abs(jerk.front()), 0.05 * max_abs_jerk + 1e-6);
  EXPECT_LT(std::abs(jerk.back()), 0.05 * max_abs_jerk + 1e-6);
}

// ---- Dispatch-latency shift (compute-latency preemption) ----

// A planned stop: t=0 anchor + ramp points at 0.1 .. 1.0 s (11 waypoints).
TrajectoryDTO planned_stop_1d() {
  TrajectoryDTO t;
  t.joint_names = {"joint_1"};
  for (int i = 0; i <= 10; ++i) {
    const double tt = 0.1 * static_cast<double>(i);
    t.waypoints.push_back(TrajectoryWaypointDTO{tt, {0.5 * tt}, {i == 10 ? 0.0 : 0.5}, {0.0}});
  }
  return t;
}

TEST(SoftStopDispatchLatencyTest, ShiftsDropsConsumedAndPrependsCommandPin) {
  // accepted q(t)=t (vel 1); command cursor 0.2, rate 1.0, latency 0.05 ->
  // advanced cursor 0.25. The pin is the COMMAND state there (full q,qdot,qddot
  // sampled from the accepted trajectory), not a measured/partial state.
  auto r = shift_soft_stop_for_dispatch_latency(planned_stop_1d(), /*compute_latency_s=*/0.05,
                                                /*lead_in_budget_s=*/0.5, linear_trajectory_1d(),
                                                /*command_cursor_s=*/0.2, /*path_time_rate=*/1.0);
  ASSERT_TRUE(r.ok()) << r.error().message;
  const auto& wps = r.value().waypoints;
  EXPECT_NEAR(wps.front().time_from_start_s, -0.010, 1e-9);
  EXPECT_NEAR(wps.front().positions_rad[0], 0.25, 1e-9);        // q(0.25)
  EXPECT_NEAR(wps.front().velocities_rad_s[0], 1.0, 1e-9);      // rate * dq/dt
  EXPECT_NEAR(wps.front().accelerations_rad_s2[0], 0.0, 1e-9);  // straight: qddot=0
  // The t=0 planned point (< Δ=0.05) is dropped; first future = 0.10 − 0.05.
  ASSERT_GE(wps.size(), 2u);
  EXPECT_NEAR(wps[1].time_from_start_s, 0.05, 1e-9);
  EXPECT_EQ(wps.size(), 11u);  // pin + 10 (the t=0 planned point dropped)
  for (size_t i = 1; i < wps.size(); ++i) {
    EXPECT_GT(wps[i].time_from_start_s, wps[i - 1].time_from_start_s);
  }
}

TEST(SoftStopDispatchLatencyTest, TinyLatencyKeepsAllFuturePoints) {
  auto r = shift_soft_stop_for_dispatch_latency(planned_stop_1d(), /*compute_latency_s=*/0.02,
                                                /*lead_in_budget_s=*/0.5, linear_trajectory_1d(),
                                                /*command_cursor_s=*/0.0, /*path_time_rate=*/1.0);
  ASSERT_TRUE(r.ok()) << r.error().message;
  const auto& wps = r.value().waypoints;
  EXPECT_EQ(wps.size(), 11u);  // pin + 10 (only the t=0 planned point dropped)
  EXPECT_NEAR(wps[1].time_from_start_s, 0.08, 1e-9);  // 0.10 − 0.02
}

TEST(SoftStopDispatchLatencyTest, GuardEscalatesWhenLatencyConsumesLeadIn) {
  // Δ = 0.25 ≥ lead-in budget 0.2 → reject, recommend hard stop.
  auto r = shift_soft_stop_for_dispatch_latency(planned_stop_1d(), 0.25, 0.2,
                                                linear_trajectory_1d(), 0.1, 1.0);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().details.at("reason"), "dispatch_latency_exceeds_lead_in");
  EXPECT_EQ(r.error().details.at("recommended_disposition"), "hard_stop");
}

TEST(SoftStopDispatchLatencyTest, RejectsInvalidInputs) {
  EXPECT_FALSE(shift_soft_stop_for_dispatch_latency(planned_stop_1d(), -0.01, 0.5,
                                                    linear_trajectory_1d(), 0.1, 1.0)
                   .ok());  // negative latency
  EXPECT_FALSE(shift_soft_stop_for_dispatch_latency(planned_stop_1d(), 0.05, 0.0,
                                                    linear_trajectory_1d(), 0.1, 1.0)
                   .ok());  // zero lead-in budget
  EXPECT_FALSE(shift_soft_stop_for_dispatch_latency(planned_stop_1d(), 0.05, 0.5,
                                                    linear_trajectory_1d(), 0.1, -1.0)
                   .ok());  // negative rate
}

// compute_soft_on_path_min_stop_time — the exact-time feasibility gate. On a
// straight unit-rate path (q'=1, q''=0) the path-rate phase plane reduces to a
// constant max-deceleration ramp, so T_min and the stop distance match the
// closed-form u̇0/D and u̇0^2/(2D).
TEST(SoftOnPathMinStopTimeTest, StraightPathMatchesAnalytic) {
  auto result = compute_soft_on_path_min_stop_time(linear_trajectory_1d(), /*trip_path_time_s=*/0.0,
                                                   /*trip_path_rate=*/1.0, {{"joint_1", 4.0}});
  ASSERT_TRUE(result.ok());
  const auto& m = result.value();
  EXPECT_TRUE(m.reaches_rest_on_path);
  EXPECT_NEAR(m.min_stop_time_s, 0.25, 1e-4);        // u̇0 / D
  EXPECT_NEAR(m.stop_distance_path_s, 0.125, 1e-4);  // u̇0^2 / (2 D)
  EXPECT_EQ(m.binding_joint, "joint_1");
}

TEST(SoftOnPathMinStopTimeTest, LowerDecelLimitGivesLongerStop) {
  auto fast =
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0, 1.0, {{"joint_1", 4.0}});
  auto slow =
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0, 1.0, {{"joint_1", 2.0}});
  ASSERT_TRUE(fast.ok());
  ASSERT_TRUE(slow.ok());
  EXPECT_NEAR(slow.value().min_stop_time_s, 0.5, 1e-4);  // u̇0 / D
  EXPECT_GT(slow.value().min_stop_time_s, fast.value().min_stop_time_s);
}

TEST(SoftOnPathMinStopTimeTest, AlreadyAtRestIsZeroTime) {
  auto result =
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.2, 0.0, {{"joint_1", 4.0}});
  ASSERT_TRUE(result.ok());
  EXPECT_NEAR(result.value().min_stop_time_s, 0.0, 1e-9);
  EXPECT_TRUE(result.value().reaches_rest_on_path);
}

TEST(SoftOnPathMinStopTimeTest, PathTooShortDoesNotReachRest) {
  // u̇0^2/(2D) = 25/8 = 3.125 path-units, but the path spans only [0, 1].
  auto result =
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0, 5.0, {{"joint_1", 4.0}});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().reaches_rest_on_path);
}

TEST(SoftOnPathMinStopTimeTest, RejectsInvalidInputs) {
  EXPECT_FALSE(
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0, -1.0, {{"joint_1", 4.0}})
          .ok());  // negative rate
  auto nan_time = compute_soft_on_path_min_stop_time(
      linear_trajectory_1d(), std::numeric_limits<double>::quiet_NaN(), 1.0, {{"joint_1", 4.0}});
  ASSERT_FALSE(nan_time.ok());
  EXPECT_EQ(nan_time.error().details.at("reason"), "invalid_trip_path_time");
  EXPECT_FALSE(
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), -0.1, 1.0, {{"joint_1", 4.0}})
          .ok());  // start before trajectory
  EXPECT_FALSE(
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 1.1, 1.0, {{"joint_1", 4.0}})
          .ok());  // start after trajectory
  EXPECT_FALSE(compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0, 1.0, {})
                   .ok());  // missing per-joint limit
  EXPECT_FALSE(
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0, 1.0, {{"joint_1", 0.0}})
          .ok());  // non-positive limit
}

// Exact-time gate inside plan_soft_on_path_stop: a deceleration_time_s shorter
// than the minimum budget-respecting on-path stop time (s_trip=1.0, D=4.0 ->
// T_min=0.25 s) is infeasible and must escalate to a hard stop rather than
// emit a limit-violating ramp.
TEST(SoftOnPathStopPlannerTest, InfeasibleDecelerationTimeEscalatesToHardStop) {
  auto profile = soft_profile(/*decel_s=*/0.1);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{4.0}}};
  auto state = measured_state_1d(0.2, 1.0);  // s_trip = 1.0
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.02;
  config.sample_period_s = 0.01;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "soft_on_path_deceleration_time_infeasible");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
}

// Gate: even the fastest budget-respecting stop overruns the remaining path
// (cursor 0.9, rate 0.8, D=0.5 -> min stop distance 0.64 >> 0.1 left). With no
// natural-completion policy this escalates to a hard stop, not the gentler
// short-path spring-back fallback.
TEST(SoftOnPathStopPlannerTest, CannotStopOnPathWithinBudgetEscalatesToHardStop) {
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{0.5}}};
  profile.allow_natural_completion_on_short_path = false;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "soft_on_path_cannot_stop_on_path_within_budget");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
}

TEST(SoftOnPathStopPlannerTest, CannotStopOnPathButNaturalCompletionAllowed) {
  // Same unstoppable-on-path case, but the policy permits natural completion,
  // so the planner defers to letting the original motion finish (as main).
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{0.5}}};
  profile.allow_natural_completion_on_short_path = true;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "natural_completion");
}

// The backend routes a real hard deceleration on this helper, so pin it to the
// planner's actual escalation dispositions: a soft-stop infeasibility must be
// classified hard_stop (never silently downgraded to spring_back), while a
// natural-completion disposition must NOT read as hard_stop.
TEST(SoftOnPathStopPlannerTest, HardStopDispositionHelperMatchesPlannerEscalations) {
  {
    auto profile = soft_profile(/*decel_s=*/0.1);
    profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{4.0}}};
    profile.allow_natural_completion_on_short_path = false;
    SoftOnPathPlannerConfig config;
    config.stop_traj_delta_s = 0.02;
    config.sample_period_s = 0.01;
    auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile,
                                          measured_state_1d(0.2, 1.0), config);
    ASSERT_FALSE(planned.ok());
    EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(planned.error()));
    EXPECT_FALSE(soft_on_path_error_recommends_natural_completion(planned.error()));
  }
  {
    auto profile = soft_profile(0.5);
    profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{0.5}}};
    profile.allow_natural_completion_on_short_path = true;
    SoftOnPathPlannerConfig config;
    config.stop_traj_delta_s = 0.05;
    config.sample_period_s = 0.05;
    auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile,
                                          measured_state_1d(0.9, 0.8), config);
    ASSERT_FALSE(planned.ok());
    EXPECT_FALSE(soft_on_path_error_recommends_hard_stop(planned.error()));
    EXPECT_TRUE(soft_on_path_error_recommends_natural_completion(planned.error()));
  }
}

// compute_jerk_limited_stop_profile — the jerk-bounded exact-time retimer. On a
// straight path it lands rest at the target time with a smooth S-curve endpoint
// (ü -> 0 as u̇ -> 0), and within_budget is true: every sample's executed accel
// and finite-difference jerk are within the limits. (The acceleration-only
// prototype failed exactly here — its abrupt endpoint deceleration spiked the
// finite-difference jerk.)
TEST(JerkLimitedStopProfileTest, StraightPathExactTimeSmoothEndpointWithinBudget) {
  auto result = compute_jerk_limited_stop_profile(
      linear_trajectory_1d(), /*start_path_time_s=*/0.0,
      /*start_path_rate=*/1.0, /*start_path_rate_accel=*/0.0,
      /*target_stop_time_s=*/1.0, {{"joint_1", 4.0}}, {{"joint_1", 50.0}});
  ASSERT_TRUE(result.ok());
  const auto& p = result.value();
  EXPECT_TRUE(p.reaches_rest_on_path);
  // within_budget is the real smoothness guarantee: every sample's executed
  // acceleration and finite-difference jerk are within the limits (no endpoint
  // jerk spike, which is what the acceleration-only prototype could not avoid).
  EXPECT_TRUE(p.hits_target_time);
  EXPECT_TRUE(p.terminal_state_zero);
  EXPECT_TRUE(p.within_budget) << p.validation_failure_reason;
  EXPECT_TRUE(p.dispatchable) << p.validation_failure_reason;
  EXPECT_NEAR(p.achieved_stop_time_s, 1.0, 2e-2);
  ASSERT_GE(p.samples.size(), 3u);
  EXPECT_NEAR(p.samples.front().time_from_ramp_start_s, 0.0, 1e-9);
  EXPECT_NEAR(p.samples.front().path_rate, 1.0, 1e-9);
  EXPECT_NEAR(p.samples.front().path_rate_accel, 0.0, 1e-9);
  EXPECT_NEAR(p.samples.back().path_rate, 0.0, 1e-6);
  EXPECT_NEAR(p.samples.back().path_rate_accel, 0.0, 1e-6);
}

TEST(JerkLimitedStopProfileTest, TightJerkCannotMeetAggressiveTimeWithinBudget) {
  // A very tight jerk limit (J=2) cannot ramp the deceleration fast enough to
  // stop a 1.0 path rate in 0.2 s; the retimer reports it is not dispatchable
  // at the requested time (the caller then escalates), never a false success.
  auto result = compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0, 1.0, 0.0,
                                                  /*target_stop_time_s=*/0.2, {{"joint_1", 100.0}},
                                                  {{"joint_1", 2.0}});
  ASSERT_TRUE(result.ok());
  const auto& p = result.value();
  EXPECT_FALSE(p.dispatchable);
  EXPECT_TRUE(!p.within_budget || !p.hits_target_time || p.achieved_stop_time_s > 0.2 + 2e-2);
}

TEST(JerkLimitedStopProfileTest, PreservesNonzeroSeamAccelerationSample) {
  auto result = compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0, 1.0, -0.5,
                                                  /*target_stop_time_s=*/0.7, {{"joint_1", 10.0}},
                                                  {{"joint_1", 100.0}});
  ASSERT_TRUE(result.ok());
  ASSERT_GE(result.value().samples.size(), 2u);
  EXPECT_NEAR(result.value().samples.front().path_rate, 1.0, 1e-9);
  EXPECT_NEAR(result.value().samples.front().path_rate_accel, -0.5, 1e-9);
}

TEST(JerkLimitedStopProfileTest, ZeroTangentCurvatureOverBudgetIsNotDispatchable) {
  auto result = compute_jerk_limited_stop_profile(zero_tangent_curved_1d(), 0.0, 1.0, 0.0,
                                                  /*target_stop_time_s=*/0.5, {{"joint_1", 1.0}},
                                                  {{"joint_1", 100.0}});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().dispatchable);
  EXPECT_FALSE(result.value().within_budget);
}

TEST(JerkLimitedStopProfileTest, BackwardEnvelopeRejectsOverfastFutureCorner) {
  auto result = compute_jerk_limited_stop_profile(curved_corner_2d(), 0.0, 0.7, 0.0,
                                                  /*target_stop_time_s=*/1.5,
                                                  {{"joint_1", 0.5}, {"joint_2", 0.5}},
                                                  {{"joint_1", 50.0}, {"joint_2", 50.0}},
                                                  /*sample_period_s=*/0.01);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().used_backward_envelope);
  EXPECT_FALSE(result.value().dispatchable);
  EXPECT_FALSE(result.value().validation_failure_reason.empty());
}

TEST(JerkLimitedStopProfileTest, BackwardEnvelopeTimingSearchMatchesEmission) {
  // The search must apply the same envelope as emission, even though it
  // does not collect samples or request the "used envelope" diagnostic.
  // Timing convergence alone does not certify the corner's joint jerk.
  for (const double target_time : {1.0, 1.5}) {
    SCOPED_TRACE(target_time);
    const auto result = compute_jerk_limited_stop_profile(
        curved_corner_2d(), 0.0, 1.0, 0.0, target_time, {{"joint_1", 5.0}, {"joint_2", 5.0}},
        {{"joint_1", 50.0}, {"joint_2", 50.0}}, 0.01);
    ASSERT_TRUE(result.ok()) << result.error().message;
    // The full chain-rule budget can remove the need for the old jerk-only
    // backward cap. Check search/emission agreement and the advertised timing
    // tolerance, not which heuristic happened to bind.
    ASSERT_FALSE(result.value().samples.empty());
    EXPECT_DOUBLE_EQ(result.value().achieved_stop_time_s,
                     result.value().samples.back().time_from_ramp_start_s);
    EXPECT_TRUE(result.value().hits_target_time);
    EXPECT_NEAR(result.value().achieved_stop_time_s, target_time, 0.02);
    EXPECT_FALSE(result.value().dispatchable);
    EXPECT_EQ(result.value().validation_failure_reason, "interpolated_jerk_limit_exceeded");
  }
}

TEST(JerkLimitedStopProfileTest, RejectsInvalidInputs) {
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0, 0.0, 0.0, 0.5,
                                                 {{"joint_1", 4.0}}, {{"joint_1", 50.0}})
                   .ok());  // zero start rate
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0, 1.0, 0.0, 0.0,
                                                 {{"joint_1", 4.0}}, {{"joint_1", 50.0}})
                   .ok());  // zero target time
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0, 1.0, 0.0, 0.5,
                                                 {{"joint_1", 4.0}}, {})
                   .ok());  // missing jerk limit
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), -0.1, 1.0, 0.0, 0.5,
                                                 {{"joint_1", 4.0}}, {{"joint_1", 50.0}})
                   .ok());  // start path time outside trajectory
}

TEST(SoftOnPathStopPlannerTest, NonFiniteCommandCursorIsRejectedInsteadOfEmittingZeroTrajectory) {
  auto trajectory = linear_trajectory_1d();
  auto profile = soft_profile(0.5);
  profile.allow_natural_completion_on_short_path = true;

  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1"};
  state.state.positions_rad = {0.2};
  state.trajectory_time_from_start_s = std::numeric_limits<double>::quiet_NaN();
  state.position_source = SoftOnPathStateSource::kModelDerived;
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.runtime_velocity_scale_at_trip = 1.0;

  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;
  config.allow_model_velocity_fallback = true;

  auto planned = plan_soft_on_path_stop(trajectory, profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "non_finite_command_cursor");
}

TEST(SoftOnPathStopPlannerTest, CommandCursorOutsideTrajectoryRangeIsRejected) {
  auto trajectory = linear_trajectory_1d();
  auto profile = soft_profile(0.5);
  profile.allow_natural_completion_on_short_path = true;

  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1"};
  state.state.positions_rad = {0.2};
  state.trajectory_time_from_start_s = -0.1;
  state.position_source = SoftOnPathStateSource::kModelDerived;
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.runtime_velocity_scale_at_trip = 1.0;

  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;
  config.allow_model_velocity_fallback = true;

  auto planned = plan_soft_on_path_stop(trajectory, profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "command_cursor_out_of_range");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");

  state.trajectory_time_from_start_s = 1.1;
  planned = plan_soft_on_path_stop(trajectory, profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "command_cursor_out_of_range");
}

TEST(SoftOnPathStopPlannerTest, NonFiniteObservedFeedbackIsRejectedBeforeProjection) {
  auto trajectory = linear_trajectory_1d();
  auto profile = soft_profile(0.5);
  auto state = measured_state_1d(std::numeric_limits<double>::quiet_NaN(), 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;

  auto planned = plan_soft_on_path_stop(trajectory, profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "observed_position_not_finite");

  SoftOnPathInitialStateDTO unnamed_state;
  unnamed_state.state.positions_rad = {0.2};
  unnamed_state.state.velocities_rad_s = {std::numeric_limits<double>::infinity()};
  planned = plan_soft_on_path_stop(trajectory, profile, unnamed_state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "observed_velocity_not_finite");
}

TEST(SoftOnPathStopPlannerTest, NonFiniteTrajectoryValuesAreRejectedByShapeValidation) {
  auto trajectory = linear_trajectory_1d();
  trajectory.waypoints[1].positions_rad[0] = std::numeric_limits<double>::quiet_NaN();
  auto profile = soft_profile(0.5);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;

  auto planned = plan_soft_on_path_stop(trajectory, profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "trajectory_value_not_finite");

  // Velocity and acceleration poisoning is rejected identically.
  auto vel_poisoned = linear_trajectory_1d();
  vel_poisoned.waypoints[0].velocities_rad_s[0] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(plan_soft_on_path_stop(vel_poisoned, profile, state, config).ok());

  auto accel_poisoned = linear_trajectory_1d();
  accel_poisoned.waypoints[0].accelerations_rad_s2[0] = -std::numeric_limits<double>::infinity();
  EXPECT_FALSE(plan_soft_on_path_stop(accel_poisoned, profile, state, config).ok());
}

TEST(SoftOnPathInitialStateTest, CommandCursorKeepsExactOccurrenceAndCanonicalJointOrder) {
  SoftOnPathInitialStateDTO state;
  state.state = {{"joint_2", "joint_1"}, {0.05, 0.2}, {0.0, 0.4}};
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.trajectory_time_from_start_s = 0.3;
  state.runtime_velocity_scale_at_trip = 0.4;
  SoftOnPathPlannerConfig config;
  config.allow_model_velocity_fallback = true;
  config.max_initial_path_deviation_rad = 0.1;

  const auto result = resolve_soft_on_path_stop_initial_state(linear_trajectory_2d(),
                                                              soft_profile(), state, config);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto& resolved = result.value();
  EXPECT_EQ(resolved.joint_names, (std::vector<std::string>{"joint_1", "joint_2"}));
  EXPECT_DOUBLE_EQ(resolved.command_path_time_s, 0.3);
  EXPECT_NEAR(resolved.projected_time_from_start_s, 0.2, 1e-12);
  EXPECT_NEAR(resolved.initial_path_deviation_rad, 0.05, 1e-12);
  EXPECT_DOUBLE_EQ(resolved.path_time_rate, 0.4);
  EXPECT_DOUBLE_EQ(resolved.runtime_velocity_scale_at_trip, 0.4);
  EXPECT_TRUE(resolved.used_model_velocity);
  const auto& point = resolved.spring_back_return_point;
  EXPECT_DOUBLE_EQ(point.time_from_start_s, 0.0);
  ASSERT_EQ(point.positions_rad.size(), 2U);
  EXPECT_NEAR(point.positions_rad[0], 0.3, 1e-12);
  EXPECT_DOUBLE_EQ(point.positions_rad[1], 0.0);
  EXPECT_EQ(point.velocities_rad_s, (std::vector<double>{0.0, 0.0}));
  EXPECT_EQ(point.accelerations_rad_s2, (std::vector<double>{0.0, 0.0}));
}

TEST(SoftOnPathInitialStateTest, MeasuredAuthorityRetainsProjectionAndVelocityDeviation) {
  SoftOnPathInitialStateDTO state;
  state.state = {{"joint_2", "joint_1"}, {0.05, 0.2}, {1.0, 0.5}};
  // Measured authority keeps its projection even when a different cursor and
  // factor were supplied. Those facts must not become a new authority rule.
  state.trajectory_time_from_start_s = 0.7;
  state.runtime_velocity_scale_at_trip = 0.9;
  const auto result =
      resolve_soft_on_path_stop_initial_state(linear_trajectory_2d(), soft_profile(), state);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(result.value().used_model_velocity);
  EXPECT_NEAR(result.value().command_path_time_s, 0.2, 1e-12);
  EXPECT_NEAR(result.value().path_time_rate, 0.5, 1e-12);
  EXPECT_NEAR(result.value().initial_velocity_deviation_rad_s, 1.0, 1e-12);
  EXPECT_DOUBLE_EQ(result.value().runtime_velocity_scale_at_trip, 0.9);
  EXPECT_NEAR(result.value().spring_back_return_point.positions_rad[0], 0.2, 1e-12);
  EXPECT_DOUBLE_EQ(result.value().spring_back_return_point.positions_rad[1], 0.0);
}

TEST(SoftOnPathInitialStateTest, ReturnPointMatchesBothSpringShapesAtTheExactOccurrence) {
  const TrajectoryDTO retracing{{ResourceId("joint.1"), ResourceId("joint.2")},
                                {"joint_1", "joint_2"},
                                {{0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                                 {1.0, {1.0, 0.5}, {0.0, 0.0}, {0.0, 0.0}},
                                 {2.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                                 {3.0, {1.0, 0.5}, {0.0, 0.0}, {0.0, 0.0}}},
                                TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0}};
  for (const bool capped : {false, true}) {
    SCOPED_TRACE(capped);
    const auto trajectory = capped ? prepared_cruise_trajectory() : retracing;
    auto state = capped ? cruise_trip_state() : measured_state_1d(0.5, 0.6);
    if (!capped) {
      state.state = {{"joint_2", "joint_1"}, {0.25, 0.5}, {}};
      state.trajectory_time_from_start_s = 2.5;
      state.velocity_source = SoftOnPathStateSource::kModelDerived;
    }
    auto config = cruise_trip_config();
    const auto profile = spring_back_profile(capped ? 0.2 : 1.0);
    auto forward_profile = profile;
    forward_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
    forward_profile.deceleration_time_s = 0.5 * *profile.deceleration_time_s;
    const auto resolved =
        resolve_soft_on_path_stop_initial_state(trajectory, forward_profile, state, config);
    ASSERT_TRUE(resolved.ok()) << resolved.error().message;
    const auto planned = plan_spring_back_on_path_stop(trajectory, profile, state, config);
    ASSERT_TRUE(planned.ok()) << planned.error().message;
    EXPECT_DOUBLE_EQ(resolved.value().command_path_time_s, capped ? 1.7 : 2.5);
    EXPECT_DOUBLE_EQ(resolved.value().command_path_time_s, planned.value().command_path_time_s);
    const auto& returned = planned.value().replacement_trajectory.waypoints.back();
    EXPECT_EQ(resolved.value().spring_back_return_point.positions_rad, returned.positions_rad);
    EXPECT_EQ(resolved.value().spring_back_return_point.velocities_rad_s,
              returned.velocities_rad_s);
    EXPECT_EQ(resolved.value().spring_back_return_point.accelerations_rad_s2,
              returned.accelerations_rad_s2);
    EXPECT_EQ(interrupt_structured_scalar_text(
                  planned.value().diagnostics.at("spring_back_return_profile")),
              capped ? "ramp_cruise_ramp_rate_capped" : "quintic_rest_to_rest");
  }
}

TEST(SoftOnPathInitialStateTest, RejectsInvalidTripWithTheExistingErrorAndCheckOrder) {
  const std::vector<std::string> reasons{
      "stop_mode_not_soft_on_path",       "soft_on_path_deceleration_time_required",
      "invalid_stop_traj_delta_s",        "invalid_sample_period_s",
      "runtime_scale_bounds_required",    "observed_state_size_mismatch",
      "missing_joint_deceleration_limit", "initial_path_deviation_exceeds_limit",
      "model_velocity_not_authorized",    "non_finite_command_cursor",
      "command_cursor_out_of_range",      "initial_velocity_deviation_exceeds_limit"};
  for (std::size_t i = 0U; i < reasons.size(); ++i) {
    SCOPED_TRACE(reasons[i]);
    auto trajectory = linear_trajectory_2d();
    auto profile = soft_profile();
    SoftOnPathInitialStateDTO state;
    state.state = {{"joint_1", "joint_2"}, {0.2, 0.0}, {0.5, 0.0}};
    SoftOnPathPlannerConfig config;
    switch (i) {
      case 0U:
        profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
        config.stop_traj_delta_s = -1.0;
        break;
      case 1U:
        profile.deceleration_time_s.reset();
        config.stop_traj_delta_s = -1.0;
        break;
      case 2U:
        config.stop_traj_delta_s = -1.0;
        trajectory.waypoints.clear();
        break;
      case 3U:
        config.sample_period_s = 0.0;
        trajectory.waypoints.clear();
        break;
      case 4U:
        trajectory.runtime_scale_bounds.reset();
        state.state.positions_rad.clear();
        break;
      case 5U:
        state.state.positions_rad.clear();
        profile.per_joint_max_deceleration.clear();
        break;
      case 6U:
        profile.per_joint_max_deceleration.erase("joint_2");
        state.state.positions_rad[1] = 0.5;
        config.max_initial_path_deviation_rad = 0.1;
        break;
      case 7U:
        state.state.positions_rad[1] = 0.5;
        config.max_initial_path_deviation_rad = 0.1;
        break;
      case 8U:
        state.velocity_source = SoftOnPathStateSource::kModelDerived;
        state.trajectory_time_from_start_s = std::numeric_limits<double>::quiet_NaN();
        break;
      case 9U:
      case 10U:
        state.velocity_source = SoftOnPathStateSource::kModelDerived;
        config.allow_model_velocity_fallback = true;
        state.trajectory_time_from_start_s =
            i == 9U ? std::numeric_limits<double>::quiet_NaN() : 2.0;
        break;
      case 11U:
        state.state.velocities_rad_s[1] = 1.0;
        config.max_initial_velocity_deviation_rad_s = 0.1;
        break;
    }
    const auto resolved =
        resolve_soft_on_path_stop_initial_state(trajectory, profile, state, config);
    ASSERT_FALSE(resolved.ok());
    EXPECT_EQ(resolved.error().details.at("reason"), reasons[i]);
    const auto planned = plan_soft_on_path_stop(trajectory, profile, state, config);
    ASSERT_FALSE(planned.ok());
    EXPECT_EQ(resolved.error(), planned.error());
  }
}

TEST(SoftOnPathInitialStateTest, ResolvingTheReturnPointDoesNotCertifyBrakingFeasibility) {
  auto profile = soft_profile(0.1);
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{4.0}}};
  const auto state = measured_state_1d(0.2, 1.0);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.02;
  const auto resolved =
      resolve_soft_on_path_stop_initial_state(linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_NEAR(resolved.value().spring_back_return_point.positions_rad[0], 0.2, 1e-12);
  const auto planned = plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"), "soft_on_path_deceleration_time_infeasible");
}

}  // namespace
}  // namespace manyforge::core
