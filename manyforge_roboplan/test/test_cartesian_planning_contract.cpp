#include <gtest/gtest.h>

#include <manyforge_roboplan/cartesian_planning_contract.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <roboplan/core/scene.hpp>
#include <roboplan/core/types.hpp>

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

TEST(CartesianPathFidelityTest,
     AcceptsEquivalentPathWithDifferentSamplingDensity) {
  const auto pose = [](double x, double y = 0.0) {
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    result(0, 3) = x;
    result(1, 3) = y;
    return result;
  };
  const std::vector<Eigen::Matrix4d> requested{
      pose(0.0), pose(1.0), pose(2.0)};
  const std::vector<Eigen::Matrix4d> actual{
      pose(0.0), pose(0.25), pose(0.5), pose(1.0), pose(1.5), pose(2.0)};

  const auto report =
      verify_cartesian_path_fidelity(requested, actual, 1e-6, 1e-6);

  EXPECT_TRUE(report.complete);
  EXPECT_DOUBLE_EQ(report.fraction_completed, 1.0);
  EXPECT_FALSE(report.first_failing_path_sample_index.has_value());
  EXPECT_GT(report.interval_checks, 0U);
  EXPECT_LE(report.max_translation_error_m, 1e-12);
  EXPECT_LE(report.max_rotation_error_rad, 1e-12);
}

TEST(CartesianPathFidelityTest,
     ExtremeNormalTolerancesRemainBoundedAndScaleInvariant) {
  const auto pose = [](double x) {
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    result(0, 3) = x;
    return result;
  };
  const std::vector<Eigen::Matrix4d> requested{
      pose(0.0), pose(0.5), pose(1.0)};
  const std::vector<Eigen::Matrix4d> actual{
      pose(0.0), pose(0.25), pose(0.5), pose(0.75), pose(1.0)};

  const auto report = verify_cartesian_path_fidelity(
      requested, actual, std::numeric_limits<double>::min(),
      std::numeric_limits<double>::min());

  EXPECT_TRUE(report.complete);
  EXPECT_DOUBLE_EQ(report.fraction_completed, 1.0);
  EXPECT_FALSE(report.first_failing_path_sample_index.has_value());
  EXPECT_GT(report.interval_checks, 0U);
}

TEST(CartesianPathFidelityTest,
     RejectsMiddleDetourEvenWhenBothEndpointsMatch) {
  const auto pose = [](double x, double y = 0.0) {
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    result(0, 3) = x;
    result(1, 3) = y;
    return result;
  };
  const std::vector<Eigen::Matrix4d> requested{
      pose(0.0), pose(1.0), pose(2.0)};
  const std::vector<Eigen::Matrix4d> actual{
      pose(0.0), pose(1.0, 0.2), pose(2.0)};

  const auto report =
      verify_cartesian_path_fidelity(requested, actual, 0.01, 0.01);

  EXPECT_FALSE(report.complete);
  EXPECT_LT(report.fraction_completed, 1.0);
  ASSERT_TRUE(report.first_failing_path_sample_index.has_value());
  EXPECT_LE(*report.first_failing_path_sample_index, 1U);
  EXPECT_GT(report.max_translation_error_m, 0.1);
}

TEST(CartesianPathFidelityTest,
     RejectsRotationalExcursionHiddenBetweenEndpointAndMidpointSamples) {
  const auto pose = [](double x, double y, double z, double w) {
    Eigen::Quaterniond orientation(w, x, y, z);
    orientation.normalize();
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    result.block<3, 3>(0, 0) = orientation.toRotationMatrix();
    return result;
  };
  // These two pairs of shortest-arc slerp endpoints stay below 2.8 rad at
  // t={0, 0.5, 1}, but their relative rotation reaches pi near t=0.75. A
  // three-point checker accepts this path incorrectly.
  const std::vector<Eigen::Matrix4d> requested{
      pose(-0.7976291493229339, 0.4587640303975524,
           -0.13736419282209256, -0.3666802191202862),
      pose(-0.6438591859348575, -0.6592910366987037,
           0.3729907515185029, -0.1079748901265729)};
  const std::vector<Eigen::Matrix4d> actual{
      pose(0.5887466021432151, -0.7265574691650781,
           -0.0012234876298491953, -0.3542459393462047),
      pose(0.33860359723742856, -0.924357227032254,
           -0.14999402758991756, 0.09172302033007032)};

  const auto report =
      verify_cartesian_path_fidelity(requested, actual, 1e-6, 2.8);

  EXPECT_FALSE(report.complete);
  EXPECT_LT(report.fraction_completed, 1.0);
  EXPECT_TRUE(report.first_failing_path_sample_index.has_value());
  EXPECT_GT(report.max_rotation_error_rad, 2.8);
}

TEST(CartesianTrajectoryContractTest,
     RejectsNonFiniteEndpointDerivativesBeforeNormalization) {
  const auto make_trajectory = [] {
    roboplan::JointTrajectory trajectory;
    trajectory.joint_names = {"joint_1"};
    trajectory.times = {0.0, 1.0};
    trajectory.positions = {Eigen::VectorXd::Zero(1),
                            Eigen::VectorXd::Ones(1)};
    trajectory.velocities = {Eigen::VectorXd::Ones(1),
                             Eigen::VectorXd::Ones(1)};
    trajectory.accelerations = {Eigen::VectorXd::Ones(1),
                                Eigen::VectorXd::Ones(1)};
    return trajectory;
  };
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};

  auto invalid_start = make_trajectory();
  invalid_start.velocities.front()[0] =
      std::numeric_limits<double>::quiet_NaN();
  const auto start_error =
      detail::validate_and_normalize_cartesian_trajectory_output(
          invalid_start, invalid_start.joint_names, control);
  ASSERT_TRUE(start_error.has_value());
  EXPECT_EQ(start_error->details.at("reason"),
            "cartesian_trajectory_sample_invalid");
  EXPECT_TRUE(std::isnan(invalid_start.velocities.front()[0]));

  auto invalid_end = make_trajectory();
  invalid_end.accelerations.back()[0] =
      std::numeric_limits<double>::infinity();
  const auto end_error =
      detail::validate_and_normalize_cartesian_trajectory_output(
          invalid_end, invalid_end.joint_names, control);
  ASSERT_TRUE(end_error.has_value());
  EXPECT_EQ(end_error->details.at("reason"),
            "cartesian_trajectory_sample_invalid");
  EXPECT_TRUE(std::isinf(invalid_end.accelerations.back()[0]));
}

TEST(CartesianTrajectoryContractTest,
     NormalizesOnlyStructurallyValidFiniteRestBoundaries) {
  roboplan::JointTrajectory trajectory;
  trajectory.joint_names = {"joint_1"};
  trajectory.times = {0.0, 1.0};
  trajectory.positions = {Eigen::VectorXd::Zero(1),
                          Eigen::VectorXd::Ones(1)};
  trajectory.velocities = {Eigen::VectorXd::Ones(1),
                           Eigen::VectorXd::Ones(1)};
  trajectory.accelerations = {Eigen::VectorXd::Ones(1),
                              Eigen::VectorXd::Ones(1)};
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};

  const auto error = detail::validate_and_normalize_cartesian_trajectory_output(
      trajectory, trajectory.joint_names, control);

  EXPECT_FALSE(error.has_value());
  EXPECT_TRUE(trajectory.velocities.front().isZero());
  EXPECT_TRUE(trajectory.velocities.back().isZero());
  EXPECT_TRUE(trajectory.accelerations.front().isZero());
  EXPECT_TRUE(trajectory.accelerations.back().isZero());
}

TEST(CartesianTrajectoryContractTest,
     DynamicsEnvelopeMeasuresInteriorQuinticExtremaInsteadOfTrustingProducer) {
  roboplan::JointTrajectory trajectory;
  trajectory.joint_names = {"joint_1"};
  trajectory.times = {0.0, 1.0};
  trajectory.positions = {Eigen::VectorXd::Zero(1),
                          Eigen::VectorXd::Ones(1)};
  trajectory.velocities = {Eigen::VectorXd::Zero(1),
                           Eigen::VectorXd::Zero(1)};
  trajectory.accelerations = {Eigen::VectorXd::Zero(1),
                              Eigen::VectorXd::Zero(1)};

  detail::CartesianTrajectoryContractInput request;
  request.speed_profile.velocity_scale = 1.0;
  request.speed_profile.acceleration_scale = 1.0;
  JointDynamicsLimitDTO limits;
  limits.max_velocity_rad_s = 1.0;
  limits.max_acceleration_rad_s2 = 1.0;
  request.per_joint_dynamics_limits["joint_1"] = limits;
  const std::vector<double> model_velocity_limits{100.0};
  const std::vector<double> model_acceleration_limits{100.0};
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};

  const auto envelope = detail::measure_cartesian_joint_dynamics_envelope(
      request, trajectory, model_velocity_limits, model_acceleration_limits,
      control);

  ASSERT_TRUE(envelope.ok()) << envelope.error().message;
  ASSERT_TRUE(envelope.value().complete);
  EXPECT_NEAR(envelope.value().velocity_ratio, 1.875, 1e-10);
  EXPECT_NEAR(envelope.value().acceleration_ratio,
              10.0 / std::sqrt(3.0), 1e-9);
}

TEST(CartesianTrajectoryContractTest,
     FrameDynamicsIncludesMultipleMimicDerivativesExactly) {
  const std::string urdf = R"(
<robot name="cartesian_mimic_chain">
  <link name="base"/>
  <joint name="source" type="prismatic">
    <parent link="base"/><child link="link_1"/>
    <axis xyz="1 0 0"/><limit lower="-1" upper="1" effort="1" velocity="2"/>
  </joint>
  <link name="link_1"/>
  <joint name="follower_1" type="prismatic">
    <parent link="link_1"/><child link="link_2"/>
    <axis xyz="1 0 0"/><limit lower="-2" upper="2" effort="1" velocity="4"/>
    <mimic joint="source" multiplier="2" offset="0"/>
  </joint>
  <link name="link_2"/>
  <joint name="follower_2" type="prismatic">
    <parent link="link_2"/><child link="tip"/>
    <axis xyz="1 0 0"/><limit lower="-6" upper="6" effort="1" velocity="12"/>
    <mimic joint="source" multiplier="6" offset="0"/>
  </joint>
  <link name="tip"/>
</robot>)";
  const std::string srdf = R"(
<robot name="cartesian_mimic_chain">
  <group name="arm">
    <joint name="source"/>
  </group>
</robot>)";
  auto scene = std::make_shared<roboplan::Scene>(
      "cartesian_mimic_chain", urdf, srdf);
  const auto group = scene->getJointGroupInfo("arm");
  ASSERT_TRUE(group.has_value()) << group.error();
  ASSERT_EQ(group->joint_names, std::vector<std::string>{"source"});

  const auto bounds = detail::resolve_cartesian_kinematic_motion_bounds(
      scene, "arm", group->joint_names);
  ASSERT_TRUE(bounds.ok()) << bounds.error().message;
  EXPECT_DOUBLE_EQ(
      static_cast<double>(bounds.value().prismatic_amplification[0]), 9.0);

  detail::CartesianTrajectoryContractInput request;
  request.compiled_group_name = "arm";
  request.compiled_base_frame = "base";
  request.compiled_tcp_frame = "tip";
  request.speed_profile.velocity_scale = 1.0;
  request.speed_profile.acceleration_scale = 1.0;
  CartesianPathDTO path;
  path.max_linear_speed_mps = 10.0;
  path.max_angular_speed_rad_s = 10.0;
  path.max_linear_acceleration_mps2 = 100.0;
  path.max_angular_acceleration_rad_s2 = 100.0;

  roboplan::JointTrajectory trajectory;
  trajectory.joint_names = group->joint_names;
  trajectory.times = {0.0, 1.0};
  trajectory.positions = {Eigen::VectorXd::Zero(1),
                          Eigen::VectorXd::Constant(1, 0.1)};
  trajectory.velocities = {Eigen::VectorXd::Zero(1),
                           Eigen::VectorXd::Zero(1)};
  trajectory.accelerations = trajectory.velocities;
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};

  const auto envelope = detail::measure_cartesian_frame_dynamics_envelope(
      request, path, scene, trajectory, control);

  ASSERT_TRUE(envelope.ok()) << envelope.error().message;
  ASSERT_TRUE(envelope.value().complete);
  // A rest-to-rest unit quintic peaks at 1.875 * delta / duration.
  // This serial chain moves (1 + 2 + 6) times the source coordinate.
  constexpr double kExactPeakLinearSpeedMps = 1.875 * 0.1 * 9.0;
  EXPECT_GE(envelope.value().linear_speed_ratio *
                path.max_linear_speed_mps,
            kExactPeakLinearSpeedMps * 0.999);
}

TEST(CartesianBackendErrorMappingTest,
     PartialPathFailuresCarryStableProgressAndWaypointDetails) {
  const auto stalled = map_cartesian_backend_failure(
      "Cartesian differential IK stalled near a singularity", 0.375, 2U);
  EXPECT_EQ(stalled.code, kErrorCartesianIncomplete);
  EXPECT_EQ(stalled.details.at("reason"), "cartesian_tracking_stalled");
  EXPECT_DOUBLE_EQ(std::stod(stalled.details.at("fraction_completed")),
                   0.375);
  EXPECT_EQ(stalled.details.at("first_failing_waypoint_index"), "2");
  EXPECT_EQ(stalled.details.at("failure_stage"), "cartesian_tracking");

  const auto tolerance = map_cartesian_backend_failure(
      "Reached maximum number of control steps", 0.75, 3U);
  EXPECT_EQ(tolerance.code, kErrorCartesianIncomplete);
  EXPECT_EQ(tolerance.details.at("reason"),
            "cartesian_path_tolerance_exceeded");

  const auto retime = map_cartesian_backend_failure(
      "TOPP-RA time parameterization failed", 1.0, std::nullopt);
  EXPECT_EQ(retime.code, kErrorPlanningFailed);
  EXPECT_EQ(retime.details.at("reason"),
            "cartesian_time_parameterization_failed");
  EXPECT_EQ(retime.details.at("failure_stage"),
            "cartesian_time_parameterization");
}
