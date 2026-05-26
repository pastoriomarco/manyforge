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
     PreservesMeasuredOffPathVelocityAndReportsHandoffDrift) {
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
  EXPECT_NEAR(
      plan.replacement_trajectory.waypoints.front().velocities_rad_s.at(1),
      1.0,
      1e-9);
  EXPECT_NEAR(plan.initial_velocity_deviation_rad_s, 1.0, 1e-9);
  EXPECT_EQ(plan.diagnostics.at("initial_velocity_deviation_rad_s"), "1");
  EXPECT_GT(plan.path_deviation_max_rad, plan.initial_path_deviation_rad);
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
     RejectsInfeasibleAccelerationWithSpringBackRecommendation) {
  auto profile = soft_profile(0.5);
  profile.per_joint_max_deceleration_rad_s2 = {{"joint_1", 0.01}};
  auto state = measured_state_1d(0.2, 0.5);
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.05;
  config.sample_period_s = 0.05;

  auto planned = plan_soft_on_path_stop(
      linear_trajectory_1d(), profile, state, config);

  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "soft_on_path_acceleration_limit_exceeded");
  EXPECT_EQ(planned.error().details.at("recommended_fallback"),
            "spring_back_hold");
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

}  // namespace
}  // namespace manyforge::core
