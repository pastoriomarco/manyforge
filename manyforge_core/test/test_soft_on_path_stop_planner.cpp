#include <gtest/gtest.h>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>

namespace manyforge::core {
namespace {

TrajectoryDTO linear_trajectory_1d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1"};
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, {0.0}, {1.0}, {0.0}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{1.0, {1.0}, {1.0}, {0.0}});
  return trajectory;
}

TrajectoryDTO linear_trajectory_2d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1", "joint_2"};
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{1.0, {1.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}});
  return trajectory;
}

TrajectoryDTO zero_tangent_curved_1d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1"};
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, {0.0}, {0.0}, {}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{1.0, {1.0}, {0.0}, {}});
  return trajectory;
}

TrajectoryDTO curved_corner_2d() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_1", "joint_2"};
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {1.0, 0.0}, {}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.3, {0.3, 0.0}, {1.0, 0.0}, {}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.6, {0.45, 0.2}, {0.0, 1.1}, {}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{1.0, {0.45, 0.7}, {0.0, 0.0}, {}});
  return trajectory;
}

MotionInterruptStopProfileDTO soft_profile(double decel_s = 0.5) {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "soft_profile";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = decel_s;
  profile.max_stop_time_s = 1.0;
  profile.per_joint_max_deceleration_rad_s2 = {
      {"joint_1", 100.0},
      {"joint_2", 100.0},
  };
  profile.allowed_fallback_modes = {
      MotionInterruptStopMode::kSpringBackHold};
  return profile;
}

TEST(SoftOnPathStopPlannerTest, StopProfileDefaultsAllowShortPathNaturalCompletion) {
  MotionInterruptStopProfileDTO profile;
  EXPECT_TRUE(profile.allow_natural_completion_on_short_path);
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

TEST(SoftOnPathStopPlannerTest,
     BuildsDerivativeCompleteWallTimeRampFromMeasuredScaledVelocity) {
  auto profile = soft_profile(0.5);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);

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

  // 50 ms handoff at 0.5 rad/s, then a smooth decel whose average
  // path-time speed is 0.25 rad/s for 450 ms.
  EXPECT_NEAR(final_wp.positions_rad.at(0), 0.3375, 1e-9);
}

TEST(SoftOnPathStopPlannerTest,
     CommandFrameSeamUsesOnPathVelocityAndReportsMeasuredDrift) {
  // Under the command-frame lead-in the seam continues the on-path COMMAND, not
  // the measured off-path velocity. A measured joint_2 velocity the path does not
  // have is a tracking error: it is reported for the drift guard / audit, but is
  // NOT injected into the seam (the old synthetic handoff did inject it, which is
  // what created the handoff excursion this seam removes).
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration_rad_s2 = {
      {"joint_1", 1000.0},
      {"joint_2", 1000.0},
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

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  ASSERT_GE(plan.replacement_trajectory.waypoints.size(), 2u);
  EXPECT_EQ(plan.diagnostics.at("seam"), "command_frame_lead_in");
  // Seam velocity is on-path: the off-path measured joint_2 velocity (1.0) is
  // NOT carried into the command.
  EXPECT_NEAR(
      plan.replacement_trajectory.waypoints.front().velocities_rad_s.at(1),
      0.0,
      1e-9);
  // The measured deviation is still measured and reported (drift guard input).
  EXPECT_NEAR(plan.initial_velocity_deviation_rad_s, 1.0, 1e-9);
  EXPECT_EQ(plan.diagnostics.at("initial_velocity_deviation_rad_s"), "1");
  // The lead-in rides the accepted path, so it adds no handoff excursion.
  EXPECT_NEAR(plan.path_deviation_max_rad, plan.initial_path_deviation_rad, 1e-9);
}

TEST(SoftOnPathStopPlannerTest,
     CanRejectMeasuredVelocityThatIsNotPathCoherent) {
  auto profile = soft_profile(0.5);
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.2, 0.0};
  state.state.velocities_rad_s = {0.5, 1.0};
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;
  config.max_initial_velocity_deviation_rad_s = 0.1;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_2d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "initial_velocity_deviation_exceeds_limit");
}

TEST(SoftOnPathStopPlannerTest,
     UsesModelDerivedVelocityOnlyWhenExplicitlyAuthorized) {
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

  auto rejected = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "model_velocity_not_authorized");

  config.allow_model_velocity_fallback = true;
  auto planned = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_TRUE(planned.value().used_model_velocity);
  EXPECT_EQ(planned.value().velocity_source, "model_derived");
  EXPECT_NEAR(
      planned.value().replacement_trajectory.waypoints.back().positions_rad.at(0),
      0.26875,
      1e-9);
}

TEST(SoftOnPathStopPlannerTest,
     CommandFrameSeamRidesProjectedPathNotMeasuredDrift) {
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

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  EXPECT_TRUE(plan.used_model_velocity);
  EXPECT_EQ(plan.diagnostics.at("seam"), "command_frame_lead_in");
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
  state.state.positions_rad = {0.2};         // measured projects to t = 0.2
  state.velocity_source = SoftOnPathStateSource::kModelDerived;
  state.trajectory_time_from_start_s = 0.3;  // command cursor is ahead (0.1 lag)
  state.runtime_velocity_scale_at_trip = 1.0;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;
  config.allow_model_velocity_fallback = true;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  // Lead-in opens at the command cursor (q(0.3) = 0.3), not the measured 0.2.
  EXPECT_NEAR(plan.replacement_trajectory.waypoints.front().positions_rad.at(0),
              0.3, 1e-9);
  EXPECT_NEAR(plan.initial_path_deviation_rad, 0.0, 1e-9);  // measured is on-path
}

TEST(SoftOnPathStopPlannerTest,
     RejectsWhenHandoffConsumesDecelerationWindow) {
  auto profile = soft_profile(0.05);
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "deceleration_time_not_larger_than_handoff");
  EXPECT_EQ(planned.error().details.at("recommended_fallback"),
            "spring_back_hold");
}

TEST(SoftOnPathStopPlannerTest,
     UsesJerkLimitedRetimerWhenQuinticJerkWouldExceedLimit) {
  // On a straight path, a symmetric S-curve can keep jerk lower than the
  // default quintic stop ramp for the same exact stop time. This proves the
  // planner's successful retimer wiring, not just the standalone helper.
  auto profile = soft_profile(1.0);
  profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 3.0}};
  auto state = measured_state_1d(0.0, 1.0);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.0;
  config.sample_period_s = 0.01;
  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 4.5}};

  auto planned =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_EQ(planned.value().diagnostics.at("soft_on_path_retimer"),
            "jerk_limited");
  EXPECT_EQ(planned.value().diagnostics.at("retimer_trigger_reason"),
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
  profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 0.5}};
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.01;

  auto planned =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "soft_on_path_retimer_not_dispatchable");
  EXPECT_EQ(planned.error().details.at("trigger_reason"),
            "soft_on_path_acceleration_limit_exceeded");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"),
            "hard_stop");
  EXPECT_TRUE(planned.error().details.contains("ali_validation_failure_reason"));
  EXPECT_TRUE(planned.error().details.contains("ali_motion_stop_time_s"));
}

TEST(SoftOnPathStopPlannerTest,
     UsesAliBacktrackingToStopBeforeFutureCornerAndHold) {
  // The exact-time quintic and jerk-limited retimer would ride into the curved
  // corner. ALI/backtracking may legally stop the moving portion earlier and
  // hold to the requested stop horizon, as long as the emitted trajectory still
  // passes the same acceleration/jerk validation.
  auto profile = soft_profile(1.0);
  profile.per_joint_max_deceleration_rad_s2 = {
      {"joint_1", 5.0},
      {"joint_2", 5.0},
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

  auto planned =
      plan_soft_on_path_stop(curved_corner_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto& plan = planned.value();
  EXPECT_EQ(plan.diagnostics.at("soft_on_path_retimer"), "ali_backtracking");
  EXPECT_EQ(plan.diagnostics.at("prior_retimer"), "jerk_limited");
  EXPECT_EQ(plan.diagnostics.at("ali_stopped_before_requested_time"), "true");
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

TEST(SoftOnPathStopPlannerTest,
     MeasuresInitialPathDriftAndCanEnforceDeploymentLimit) {
  auto profile = soft_profile(0.5);
  SoftOnPathInitialStateDTO state;
  state.state.joint_names = {"joint_1", "joint_2"};
  state.state.positions_rad = {0.2, 0.1};
  state.state.velocities_rad_s = {0.5, 0.0};
  state.runtime_velocity_scale_at_trip = 0.5;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_2d(), profile, state, config);

  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_NEAR(planned.value().initial_path_deviation_rad, 0.1, 1e-9);
  EXPECT_NEAR(planned.value().path_deviation_max_rad, 0.1, 1e-9);
  EXPECT_EQ(planned.value().diagnostics.at("position_source"),
            "measured_feedback");

  config.max_initial_path_deviation_rad = 0.05;
  auto rejected = plan_soft_on_path_stop(
      linear_trajectory_2d(), profile, state, config);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "initial_path_deviation_exceeds_limit");
}

TEST(SoftOnPathStopPlannerTest,
     MeasuresFeedbackPathDeviationWithJointNameOrdering) {
  RobotStateDTO observed;
  observed.joint_names = {"joint_2", "joint_1"};
  observed.positions_rad = {0.25, 0.4};

  auto measured = measure_soft_on_path_path_deviation(
      linear_trajectory_2d(), observed);

  ASSERT_TRUE(measured.ok()) << measured.error().message;
  EXPECT_NEAR(measured.value(), 0.25, 1e-9);
}

TEST(SoftOnPathStopPlannerTest,
     RecordsDeploymentAndDefaultJerkLimitSources) {
  auto profile = soft_profile(0.5);
  auto state = measured_state_1d(0.2, 0.1);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto default_jerk = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(default_jerk.ok()) << default_jerk.error().message;
  EXPECT_EQ(default_jerk.value().jerk_limit_source, "default");

  config.per_joint_max_jerk_rad_s3 = {{"joint_1", 100.0}};
  auto deployment_jerk = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);
  ASSERT_TRUE(deployment_jerk.ok()) << deployment_jerk.error().message;
  EXPECT_EQ(deployment_jerk.value().jerk_limit_source, "deployment");
}

TEST(SoftOnPathStopPlannerTest,
     RejectsRampThatWouldPassAcceptedTrajectoryEnd) {
  auto profile = soft_profile(0.5);
  profile.allowed_fallback_modes.clear();
  profile.allow_natural_completion_on_short_path = false;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "soft_on_path_remaining_time_short");
  EXPECT_EQ(planned.error().details.at("recommended_fallback"),
            "spring_back_hold");
}

TEST(SoftOnPathStopPlannerTest,
     RecommendsNaturalCompletionWhenShortPathPolicyAllowsIt) {
  auto profile = soft_profile(0.5);
  profile.allowed_fallback_modes.clear();
  profile.allow_natural_completion_on_short_path = true;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "soft_on_path_remaining_time_short");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"),
            "natural_completion");
  EXPECT_FALSE(planned.error().details.contains("recommended_fallback"));
}

TEST(SoftOnPathStopPlannerTest,
     FeasibilityEnvelopeRejectsLateTripPointsBeforeDispatch) {
  auto profile = soft_profile(0.5);
  profile.allowed_fallback_modes.clear();
  profile.allow_natural_completion_on_short_path = false;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto envelope = validate_soft_on_path_feasibility_envelope(
      linear_trajectory_1d(), profile, config);

  ASSERT_FALSE(envelope.ok());
  EXPECT_EQ(envelope.error().code, kErrorMotionInterruptArmFailed);
  EXPECT_EQ(envelope.error().details.at("reason"),
            "soft_on_path_feasibility_envelope_infeasible");
  EXPECT_EQ(envelope.error().details.at("planner_reason"),
            "soft_on_path_remaining_time_short");
  EXPECT_EQ(envelope.error().details.at("recommended_fallback"),
            "spring_back_hold");
}

TEST(SoftOnPathStopPlannerTest,
     FeasibilityEnvelopeAllowsShortPathNaturalCompletionPolicy) {
  auto profile = soft_profile(0.5);
  profile.allowed_fallback_modes.clear();
  profile.allow_natural_completion_on_short_path = true;
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto envelope = validate_soft_on_path_feasibility_envelope(
      linear_trajectory_1d(), profile, config);

  ASSERT_TRUE(envelope.ok()) << envelope.error().message;
  EXPECT_GT(envelope.value().checked_samples, 0u);
  EXPECT_GT(envelope.value().natural_completion_samples, 0u);
  EXPECT_EQ(envelope.value().diagnostics.at("natural_completion_samples"),
            std::to_string(envelope.value().natural_completion_samples));
}

TEST(SoftOnPathStopPlannerTest,
     FeasibilityEnvelopeDoesNotRecommendFallbackForMalformedProfile) {
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration_rad_s2.erase("joint_2");
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto envelope = validate_soft_on_path_feasibility_envelope(
      linear_trajectory_2d(), profile, config);

  ASSERT_FALSE(envelope.ok());
  EXPECT_EQ(envelope.error().details.at("reason"),
            "soft_on_path_feasibility_envelope_infeasible");
  EXPECT_EQ(envelope.error().details.at("planner_reason"),
            "missing_joint_deceleration_limit");
  EXPECT_FALSE(envelope.error().details.contains("recommended_fallback"));
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
  profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 1000.0}};
  profile.per_joint_max_deceleration_rad_s2["joint_2"] = 1000.0;
  SoftOnPathInitialStateDTO state = measured_state_1d(0.2, 1.0);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.0;   // no handoff offset: r=0 sample is the seam
  config.sample_period_s = 0.001;   // fine sampling for finite-difference checks
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
    jerk.push_back((wps[i].accelerations_rad_s2.at(0) -
                    wps[i - 1].accelerations_rad_s2.at(0)) / dt);
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
    t.waypoints.push_back(TrajectoryWaypointDTO{
        tt, {0.5 * tt}, {i == 10 ? 0.0 : 0.5}, {0.0}});
  }
  return t;
}

TEST(SoftStopDispatchLatencyTest, ShiftsDropsConsumedAndPrependsCommandPin) {
  // accepted q(t)=t (vel 1); command cursor 0.2, rate 1.0, latency 0.05 ->
  // advanced cursor 0.25. The pin is the COMMAND state there (full q,qdot,qddot
  // sampled from the accepted trajectory), not a measured/partial state.
  auto r = shift_soft_stop_for_dispatch_latency(
      planned_stop_1d(), /*compute_latency_s=*/0.05, /*lead_in_budget_s=*/0.5,
      linear_trajectory_1d(), /*command_cursor_s=*/0.2, /*path_time_rate=*/1.0);
  ASSERT_TRUE(r.ok()) << r.error().message;
  const auto& wps = r.value().waypoints;
  EXPECT_NEAR(wps.front().time_from_start_s, -0.010, 1e-9);
  EXPECT_NEAR(wps.front().positions_rad[0], 0.25, 1e-9);     // q(0.25)
  EXPECT_NEAR(wps.front().velocities_rad_s[0], 1.0, 1e-9);   // rate * dq/dt
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
  auto r = shift_soft_stop_for_dispatch_latency(
      planned_stop_1d(), /*compute_latency_s=*/0.02, /*lead_in_budget_s=*/0.5,
      linear_trajectory_1d(), /*command_cursor_s=*/0.0, /*path_time_rate=*/1.0);
  ASSERT_TRUE(r.ok()) << r.error().message;
  const auto& wps = r.value().waypoints;
  EXPECT_EQ(wps.size(), 11u);  // pin + 10 (only the t=0 planned point dropped)
  EXPECT_NEAR(wps[1].time_from_start_s, 0.08, 1e-9);  // 0.10 − 0.02
}

TEST(SoftStopDispatchLatencyTest, GuardEscalatesWhenLatencyConsumesLeadIn) {
  // Δ = 0.25 ≥ lead-in budget 0.2 → reject, recommend hard stop.
  auto r = shift_soft_stop_for_dispatch_latency(
      planned_stop_1d(), 0.25, 0.2, linear_trajectory_1d(), 0.1, 1.0);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().details.at("reason"), "dispatch_latency_exceeds_lead_in");
  EXPECT_EQ(r.error().details.at("recommended_disposition"), "hard_stop");
}

TEST(SoftStopDispatchLatencyTest, RejectsInvalidInputs) {
  EXPECT_FALSE(shift_soft_stop_for_dispatch_latency(
                   planned_stop_1d(), -0.01, 0.5, linear_trajectory_1d(), 0.1, 1.0)
                   .ok());  // negative latency
  EXPECT_FALSE(shift_soft_stop_for_dispatch_latency(
                   planned_stop_1d(), 0.05, 0.0, linear_trajectory_1d(), 0.1, 1.0)
                   .ok());  // zero lead-in budget
  EXPECT_FALSE(shift_soft_stop_for_dispatch_latency(
                   planned_stop_1d(), 0.05, 0.5, linear_trajectory_1d(), 0.1, -1.0)
                   .ok());  // negative rate
}

// compute_soft_on_path_min_stop_time — the exact-time feasibility gate. On a
// straight unit-rate path (q'=1, q''=0) the path-rate phase plane reduces to a
// constant max-deceleration ramp, so T_min and the stop distance match the
// closed-form u̇0/D and u̇0^2/(2D).
TEST(SoftOnPathMinStopTimeTest, StraightPathMatchesAnalytic) {
  auto result = compute_soft_on_path_min_stop_time(
      linear_trajectory_1d(), /*trip_path_time_s=*/0.0,
      /*trip_path_rate=*/1.0, {{"joint_1", 4.0}});
  ASSERT_TRUE(result.ok());
  const auto& m = result.value();
  EXPECT_TRUE(m.reaches_rest_on_path);
  EXPECT_NEAR(m.min_stop_time_s, 0.25, 1e-4);        // u̇0 / D
  EXPECT_NEAR(m.stop_distance_path_s, 0.125, 1e-4);  // u̇0^2 / (2 D)
  EXPECT_EQ(m.binding_joint, "joint_1");
}

TEST(SoftOnPathMinStopTimeTest, LowerDecelLimitGivesLongerStop) {
  auto fast = compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0,
                                                 1.0, {{"joint_1", 4.0}});
  auto slow = compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0,
                                                 1.0, {{"joint_1", 2.0}});
  ASSERT_TRUE(fast.ok());
  ASSERT_TRUE(slow.ok());
  EXPECT_NEAR(slow.value().min_stop_time_s, 0.5, 1e-4);  // u̇0 / D
  EXPECT_GT(slow.value().min_stop_time_s, fast.value().min_stop_time_s);
}

TEST(SoftOnPathMinStopTimeTest, AlreadyAtRestIsZeroTime) {
  auto result = compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.2,
                                                   0.0, {{"joint_1", 4.0}});
  ASSERT_TRUE(result.ok());
  EXPECT_NEAR(result.value().min_stop_time_s, 0.0, 1e-9);
  EXPECT_TRUE(result.value().reaches_rest_on_path);
}

TEST(SoftOnPathMinStopTimeTest, PathTooShortDoesNotReachRest) {
  // u̇0^2/(2D) = 25/8 = 3.125 path-units, but the path spans only [0, 1].
  auto result = compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0,
                                                   5.0, {{"joint_1", 4.0}});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().reaches_rest_on_path);
}

TEST(SoftOnPathMinStopTimeTest, RejectsInvalidInputs) {
  EXPECT_FALSE(compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0,
                                                  -1.0, {{"joint_1", 4.0}})
                   .ok());  // negative rate
  EXPECT_FALSE(
      compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0, 1.0, {})
          .ok());  // missing per-joint limit
  EXPECT_FALSE(compute_soft_on_path_min_stop_time(linear_trajectory_1d(), 0.0,
                                                  1.0, {{"joint_1", 0.0}})
                   .ok());  // non-positive limit
}

// Exact-time gate inside plan_soft_on_path_stop: a deceleration_time_s shorter
// than the minimum budget-respecting on-path stop time (s_trip=1.0, D=4.0 ->
// T_min=0.25 s) is infeasible and must escalate to a hard stop rather than
// emit a limit-violating ramp.
TEST(SoftOnPathStopPlannerTest, InfeasibleDecelerationTimeEscalatesToHardStop) {
  auto profile = soft_profile(/*decel_s=*/0.1);
  profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 4.0}};
  auto state = measured_state_1d(0.2, 1.0);  // s_trip = 1.0
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.02;
  config.sample_period_s = 0.01;

  auto planned =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "soft_on_path_deceleration_time_infeasible");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
}

// Gate: even the fastest budget-respecting stop overruns the remaining path
// (cursor 0.9, rate 0.8, D=0.5 -> min stop distance 0.64 >> 0.1 left). With no
// natural-completion policy this escalates to a hard stop, not the gentler
// short-path spring-back fallback.
TEST(SoftOnPathStopPlannerTest, CannotStopOnPathWithinBudgetEscalatesToHardStop) {
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 0.5}};
  profile.allow_natural_completion_on_short_path = false;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "soft_on_path_cannot_stop_on_path_within_budget");
  EXPECT_EQ(planned.error().details.at("recommended_disposition"), "hard_stop");
}

TEST(SoftOnPathStopPlannerTest, CannotStopOnPathButNaturalCompletionAllowed) {
  // Same unstoppable-on-path case, but the policy permits natural completion,
  // so the planner defers to letting the original motion finish (not hard stop).
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 0.5}};
  profile.allow_natural_completion_on_short_path = true;
  auto state = measured_state_1d(0.9, 0.8);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned =
      plan_soft_on_path_stop(linear_trajectory_1d(), profile, state, config);
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("recommended_disposition"),
            "natural_completion");
}

// The backend routes a real hard deceleration on this helper, so pin it to the
// planner's actual escalation dispositions: a soft-stop infeasibility must be
// classified hard_stop (never silently downgraded to spring_back), while a
// natural-completion disposition must NOT read as hard_stop.
TEST(SoftOnPathStopPlannerTest, HardStopDispositionHelperMatchesPlannerEscalations) {
  {
    auto profile = soft_profile(/*decel_s=*/0.1);
    profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 4.0}};
    profile.allow_natural_completion_on_short_path = false;
    SoftOnPathPlannerConfig config;
    config.stop_traj_delta_s = 0.02;
    config.sample_period_s = 0.01;
    auto planned = plan_soft_on_path_stop(
        linear_trajectory_1d(), profile, measured_state_1d(0.2, 1.0), config);
    ASSERT_FALSE(planned.ok());
    EXPECT_TRUE(soft_on_path_error_recommends_hard_stop(planned.error()));
    EXPECT_FALSE(
        soft_on_path_error_recommends_natural_completion(planned.error()));
  }
  {
    auto profile = soft_profile(0.5);
    profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 0.5}};
    profile.allow_natural_completion_on_short_path = true;
    SoftOnPathPlannerConfig config;
    config.stop_traj_delta_s = 0.05;
    config.sample_period_s = 0.05;
    auto planned = plan_soft_on_path_stop(
        linear_trajectory_1d(), profile, measured_state_1d(0.9, 0.8), config);
    ASSERT_FALSE(planned.ok());
    EXPECT_FALSE(soft_on_path_error_recommends_hard_stop(planned.error()));
    EXPECT_TRUE(
        soft_on_path_error_recommends_natural_completion(planned.error()));
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
  auto result = compute_jerk_limited_stop_profile(
      linear_trajectory_1d(), 0.0, 1.0, 0.0, /*target_stop_time_s=*/0.2,
      {{"joint_1", 100.0}}, {{"joint_1", 2.0}});
  ASSERT_TRUE(result.ok());
  const auto& p = result.value();
  EXPECT_FALSE(p.dispatchable);
  EXPECT_TRUE(!p.within_budget || !p.hits_target_time ||
              p.achieved_stop_time_s > 0.2 + 2e-2);
}

TEST(JerkLimitedStopProfileTest, PreservesNonzeroSeamAccelerationSample) {
  auto result = compute_jerk_limited_stop_profile(
      linear_trajectory_1d(), 0.0, 1.0, -0.5, /*target_stop_time_s=*/0.7,
      {{"joint_1", 10.0}}, {{"joint_1", 100.0}});
  ASSERT_TRUE(result.ok());
  ASSERT_GE(result.value().samples.size(), 2u);
  EXPECT_NEAR(result.value().samples.front().path_rate, 1.0, 1e-9);
  EXPECT_NEAR(result.value().samples.front().path_rate_accel, -0.5, 1e-9);
}

TEST(JerkLimitedStopProfileTest,
     ZeroTangentCurvatureOverBudgetIsNotDispatchable) {
  auto result = compute_jerk_limited_stop_profile(
      zero_tangent_curved_1d(), 0.0, 1.0, 0.0,
      /*target_stop_time_s=*/0.5, {{"joint_1", 1.0}}, {{"joint_1", 100.0}});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().dispatchable);
  EXPECT_FALSE(result.value().within_budget);
}

TEST(JerkLimitedStopProfileTest, BackwardEnvelopeRejectsOverfastFutureCorner) {
  auto result = compute_jerk_limited_stop_profile(
      curved_corner_2d(), 0.0, 0.7, 0.0,
      /*target_stop_time_s=*/1.5,
      {{"joint_1", 0.5}, {"joint_2", 0.5}},
      {{"joint_1", 50.0}, {"joint_2", 50.0}},
      /*sample_period_s=*/0.01);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().used_backward_envelope);
  EXPECT_FALSE(result.value().dispatchable);
  EXPECT_FALSE(result.value().validation_failure_reason.empty());
}

TEST(JerkLimitedStopProfileTest, RejectsInvalidInputs) {
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0,
                                                 0.0, 0.0, 0.5,
                                                 {{"joint_1", 4.0}},
                                                 {{"joint_1", 50.0}})
                   .ok());  // zero start rate
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0,
                                                 1.0, 0.0, 0.0,
                                                 {{"joint_1", 4.0}},
                                                 {{"joint_1", 50.0}})
                   .ok());  // zero target time
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), 0.0,
                                                 1.0, 0.0, 0.5,
                                                 {{"joint_1", 4.0}}, {})
                   .ok());  // missing jerk limit
  EXPECT_FALSE(compute_jerk_limited_stop_profile(linear_trajectory_1d(), -0.1,
                                                 1.0, 0.0, 0.5,
                                                 {{"joint_1", 4.0}},
                                                 {{"joint_1", 50.0}})
                   .ok());  // start path time outside trajectory
}

}  // namespace
}  // namespace manyforge::core
