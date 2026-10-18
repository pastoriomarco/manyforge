#include <gtest/gtest.h>

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <array>
#include <cmath>
#include <limits>
#include <manyforge_roboplan/cartesian_planning_contract.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>
#include <memory>
#include <roboplan/core/scene.hpp>
#include <roboplan/core/types.hpp>
#include <string>
#include <vector>

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;
namespace trajectory_detail = manyforge::roboplan_backend::detail;

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
     WaypointCorrespondencePreservesRetracingAndDuplicateVisits) {
  const auto pose = [](double x) {
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    result(0, 3) = x;
    return result;
  };
  const std::vector<Eigen::Matrix4d> requested{pose(0), pose(1), pose(1),
                                               pose(0), pose(1)};
  const std::vector<Eigen::Matrix4d> actual{
      pose(0), pose(0.5), pose(1), pose(0.5), pose(0), pose(0.5), pose(1)};
  const std::vector<std::size_t> indices{0, 1, 2, 3, 4};
  const auto result = verify_cartesian_path_fidelity_controlled(
      requested, actual, 1e-6, 1e-6, {}, indices);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value().complete);
  EXPECT_EQ(result.value().waypoint_actual_sample_indices,
            (std::vector<std::size_t>{0, 2, 2, 4, 6}));
  const auto invalid = verify_cartesian_path_fidelity_controlled(
      requested, actual, 1e-6, 1e-6, {}, std::array<std::size_t, 2>{2, 1});
  EXPECT_FALSE(invalid.ok());
  auto detour = actual;
  detour[3](1, 3) = 0.1;
  const auto failed = verify_cartesian_path_fidelity_controlled(
      requested, detour, 1e-6, 1e-6, {}, indices);
  ASSERT_TRUE(failed.ok());
  EXPECT_FALSE(failed.value().complete);
  EXPECT_TRUE(failed.value().waypoint_actual_sample_indices.empty());
}

TEST(CartesianPathFidelityTest,
     RotationOnlyAndStationaryWaypointOrderIsRetained) {
  const auto pose = [](double angle) {
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    result.block<3, 3>(0, 0) =
        Eigen::AngleAxisd(angle, Eigen::Vector3d::UnitZ()).toRotationMatrix();
    return result;
  };
  const std::vector<Eigen::Matrix4d> requested{pose(0), pose(0.3), pose(0),
                                               pose(0.3)};
  const auto result = verify_cartesian_path_fidelity_controlled(
      requested, requested, 1e-6, 1e-3, {},
      std::array<std::size_t, 4>{0, 1, 2, 3});
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().complete);
  EXPECT_EQ(result.value().waypoint_actual_sample_indices,
            (std::vector<std::size_t>{0, 1, 2, 3}));
  const std::vector<Eigen::Matrix4d> still(3, pose(0));
  const auto stationary = verify_cartesian_path_fidelity_controlled(
      still, still, 1e-6, 1e-6, {}, std::array<std::size_t, 3>{0, 1, 2});
  ASSERT_TRUE(stationary.ok());
  ASSERT_TRUE(stationary.value().complete);
  EXPECT_EQ(stationary.value().waypoint_actual_sample_indices,
            (std::vector<std::size_t>{0, 0, 2}));
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
      trajectory_detail::validate_and_normalize_cartesian_trajectory_output(
          invalid_start, invalid_start.joint_names, control);
  ASSERT_TRUE(start_error.has_value());
  EXPECT_EQ(start_error->details.at("reason"),
            "cartesian_trajectory_sample_invalid");
  EXPECT_TRUE(std::isnan(invalid_start.velocities.front()[0]));

  auto invalid_end = make_trajectory();
  invalid_end.accelerations.back()[0] =
      std::numeric_limits<double>::infinity();
  const auto end_error =
      trajectory_detail::validate_and_normalize_cartesian_trajectory_output(
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

  const auto error =
      trajectory_detail::validate_and_normalize_cartesian_trajectory_output(
          trajectory, trajectory.joint_names, control);

  EXPECT_FALSE(error.has_value());
  EXPECT_TRUE(trajectory.velocities.front().isZero());
  EXPECT_TRUE(trajectory.velocities.back().isZero());
  EXPECT_TRUE(trajectory.accelerations.front().isZero());
  EXPECT_TRUE(trajectory.accelerations.back().isZero());
}

TEST(CartesianTrajectoryContractTest,
     ConstValidationPreservesMovingBoundariesAndRequiresZeroOrigin) {
  roboplan::JointTrajectory trajectory;
  trajectory.joint_names = {"joint_1"};
  trajectory.times = {0.0, 1.0};
  trajectory.positions = {Eigen::VectorXd::Zero(1),
                          Eigen::VectorXd::Ones(1)};
  trajectory.velocities = {Eigen::VectorXd::Constant(1, 0.4),
                           Eigen::VectorXd::Constant(1, 0.2)};
  trajectory.accelerations = {Eigen::VectorXd::Constant(1, -0.3),
                              Eigen::VectorXd::Constant(1, 0.1)};
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};

  EXPECT_FALSE(trajectory_detail::validate_cartesian_trajectory_output(
                   trajectory, trajectory.joint_names, control)
                   .has_value());
  EXPECT_DOUBLE_EQ(trajectory.positions.front()[0], 0.0);
  EXPECT_DOUBLE_EQ(trajectory.positions.back()[0], 1.0);
  EXPECT_DOUBLE_EQ(trajectory.velocities.front()[0], 0.4);
  EXPECT_DOUBLE_EQ(trajectory.velocities.back()[0], 0.2);
  EXPECT_DOUBLE_EQ(trajectory.accelerations.front()[0], -0.3);
  EXPECT_DOUBLE_EQ(trajectory.accelerations.back()[0], 0.1);
  EXPECT_EQ(trajectory.times, (std::vector<double>{0.0, 1.0}));

  trajectory.times.front() = -0.1;
  const auto error = trajectory_detail::validate_cartesian_trajectory_output(
      trajectory, trajectory.joint_names, control);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->details.at("reason"),
            "cartesian_trajectory_sample_invalid");
  EXPECT_DOUBLE_EQ(trajectory.times.front(), -0.1);
  EXPECT_DOUBLE_EQ(trajectory.velocities.front()[0], 0.4);
  EXPECT_DOUBLE_EQ(trajectory.accelerations.back()[0], 0.1);
}

TEST(CartesianTrajectoryContractTest,
     BernsteinPositionBoundsDetectInteriorViolationWithLegalEndpoints) {
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};
  const auto result = trajectory_detail::check_quintic_position_bounds(
      {0.0, 10.0, 0.0}, {0.0, -10.0, 0.0}, 1.0, -1.0, 1.0,
      0.0, 32, control);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(),
            trajectory_detail::QuinticPositionBoundResult::kProvenViolation);
}

TEST(CartesianTrajectoryContractTest,
     BernsteinPositionBoundsSubdivideInsteadOfRejectingControlHull) {
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};
  // Control polygon spans [-0.6, 1.6], while the actual quintic stays in
  // [0, 1]. An out-of-bounds control point is not a proven violation.
  const auto check = [&](std::size_t budget) {
    return trajectory_detail::check_quintic_position_bounds(
        {0.0, 4.0, 0.0}, {1.0, 4.0, 0.0}, 1.0, -0.01, 1.01,
        0.0, budget, control);
  };
  const auto exhausted = check(1);
  ASSERT_TRUE(exhausted.ok());
  EXPECT_EQ(exhausted.value(),
            trajectory_detail::QuinticPositionBoundResult::kInconclusive);
  const auto qualified = check(255);
  ASSERT_TRUE(qualified.ok());
  EXPECT_EQ(qualified.value(),
            trajectory_detail::QuinticPositionBoundResult::kProvenWithin);
  const auto repeat = check(1);
  ASSERT_TRUE(repeat.ok());
  EXPECT_EQ(repeat.value(), exhausted.value());
}

TEST(CartesianTrajectoryContractTest,
     BernsteinPositionBoundsUseAsymmetricPhysicalPVAAndCallerTolerance) {
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};
  // q(u)=.2+.3u-.1u^2+.2u^3-.1u^4+.05u^5, u=t/2.
  const auto asymmetric = trajectory_detail::check_quintic_position_bounds(
      {0.2, 0.15, -0.05}, {0.55, 0.275, 0.2}, 2.0, 0.19, 0.56,
      0.0, 1, control);
  ASSERT_TRUE(asymmetric.ok());
  EXPECT_EQ(asymmetric.value(),
            trajectory_detail::QuinticPositionBoundResult::kProvenWithin);

  const auto constant = trajectory_detail::check_quintic_position_bounds(
      {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 1.0, 0.0, 0.0,
      0.0, 1, control);
  ASSERT_TRUE(constant.ok());
  EXPECT_EQ(constant.value(),
            trajectory_detail::QuinticPositionBoundResult::kProvenWithin);
  const auto tolerated = trajectory_detail::check_quintic_position_bounds(
      {1.005, 0.0, 0.0}, {1.005, 0.0, 0.0}, 1.0, 0.0, 1.0,
      0.01, 1, control);
  ASSERT_TRUE(tolerated.ok());
  EXPECT_EQ(tolerated.value(),
            trajectory_detail::QuinticPositionBoundResult::kProvenWithin);
  const auto outside = trajectory_detail::check_quintic_position_bounds(
      {1.015, 0.0, 0.0}, {1.015, 0.0, 0.0}, 1.0, 0.0, 1.0,
      0.01, 1, control);
  ASSERT_TRUE(outside.ok());
  EXPECT_EQ(outside.value(),
            trajectory_detail::QuinticPositionBoundResult::kProvenViolation);
}

TEST(CartesianTrajectoryContractTest,
     BernsteinPositionBoundsFailClosedOnInvalidInputAndOverflow) {
  const CartesianPlanningControl control{[] { return false; },
                                         [] { return false; }};
  const trajectory_detail::QuinticPositionEndpoint rest{0.0, 0.0, 0.0};
  for (const auto duration : {0.0, -1.0,
                              std::numeric_limits<double>::infinity()}) {
    const auto invalid = trajectory_detail::check_quintic_position_bounds(
        rest, rest, duration, -1.0, 1.0, 0.0, 1, control);
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.error().details.at("reason"),
              "quintic_position_bounds_input_invalid");
  }
  EXPECT_FALSE(trajectory_detail::check_quintic_position_bounds(
                   rest, rest, 1.0, 1.0, -1.0, 0.0, 1, control)
                   .ok());
  EXPECT_FALSE(trajectory_detail::check_quintic_position_bounds(
                   rest, rest, 1.0, -1.0, 1.0, -0.1, 1, control)
                   .ok());
  EXPECT_FALSE(trajectory_detail::check_quintic_position_bounds(
                   {0.0, std::numeric_limits<double>::quiet_NaN(), 0.0},
                   rest, 1.0, -1.0, 1.0, 0.0, 1, control)
                   .ok());
  const auto overflow = trajectory_detail::check_quintic_position_bounds(
      rest, rest, std::numeric_limits<double>::max(), -1.0, 1.0,
      0.0, 1, control);
  ASSERT_FALSE(overflow.ok());
  EXPECT_EQ(overflow.error().details.at("reason"),
            "quintic_position_bounds_arithmetic_overflow");
  const auto expanded_overflow =
      trajectory_detail::check_quintic_position_bounds(
          rest, rest, 1.0, -1.0, std::numeric_limits<double>::max(),
          std::numeric_limits<double>::max(), 1, control);
  ASSERT_FALSE(expanded_overflow.ok());
  EXPECT_EQ(expanded_overflow.error().details.at("reason"),
            "quintic_position_bounds_arithmetic_overflow");
}

TEST(CartesianTrajectoryContractTest,
     BernsteinPositionBoundsHaveExplicitBudgetAndPlanningControl) {
  const trajectory_detail::QuinticPositionEndpoint rest{0.0, 0.0, 0.0};
  const auto empty_budget = trajectory_detail::check_quintic_position_bounds(
      rest, rest, 1.0, -1.0, 1.0, 0.0, 0, {});
  ASSERT_TRUE(empty_budget.ok());
  EXPECT_EQ(empty_budget.value(),
            trajectory_detail::QuinticPositionBoundResult::kInconclusive);
  const auto cancelled = trajectory_detail::check_quintic_position_bounds(
      rest, rest, 1.0, -1.0, 1.0, 0.0, 1,
      {[] { return true; }, [] { return false; }});
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().details.at("reason"),
            "planning_cancelled_by_interrupt");
  const auto expired = trajectory_detail::check_quintic_position_bounds(
      rest, rest, 1.0, -1.0, 1.0, 0.0, 1,
      {[] { return false; }, [] { return true; }});
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "planning_timeout");
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

  trajectory_detail::CartesianTrajectoryContractInput request;
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

  const auto envelope =
      trajectory_detail::measure_cartesian_joint_dynamics_envelope(
          request, trajectory, model_velocity_limits, model_acceleration_limits,
          control);

  ASSERT_TRUE(envelope.ok()) << envelope.error().message;
  ASSERT_TRUE(envelope.value().complete);
  EXPECT_NEAR(envelope.value().velocity_ratio, 1.875, 1e-10);
  EXPECT_NEAR(envelope.value().acceleration_ratio,
              10.0 / std::sqrt(3.0), 1e-9);
  ASSERT_EQ(envelope.value().joint_derivatives.size(), 1U);
  const auto &derivatives = envelope.value().joint_derivatives.front();
  EXPECT_NEAR(derivatives.velocity, 1.875, 1e-10);
  EXPECT_NEAR(derivatives.acceleration, 10.0 / std::sqrt(3.0), 1e-9);
  EXPECT_NEAR(derivatives.jerk, 60.0, 1e-9);

  // The common continuous gate also replaces the old sampled-cap helper's
  // missing-derivative checks; absent data must never certify a zero peak.
  for (int missing = 0; missing < 3; ++missing) {
    auto incomplete = trajectory;
    if (missing == 0)
      incomplete.velocities.clear();
    if (missing == 1)
      incomplete.accelerations.clear();
    if (missing == 2)
      incomplete.velocities.back().resize(0);
    const auto invalid =
        trajectory_detail::measure_cartesian_joint_dynamics_envelope(
            request, incomplete, model_velocity_limits,
            model_acceleration_limits, control);
    ASSERT_TRUE(invalid.ok());
    EXPECT_FALSE(invalid.value().complete);
  }
}

TEST(CartesianTrajectoryContractTest,
     JerkEnvelopeFindsInteriorPeakAndScalesCubically) {
  // q(t)=t^4/24-t^5/60 on [0,1] has jerk t-t^2: both endpoint jerks
  // are zero while the interior peak is 1/4. Dilation must preserve that.
  for (double dilation : {1.0, 2.0, 4.0}) {
    roboplan::JointTrajectory trajectory;
    trajectory.joint_names = {"joint_1"};
    trajectory.times = {0.0, dilation};
    trajectory.positions = {Eigen::VectorXd::Zero(1),
                            Eigen::VectorXd::Constant(1, 1.0 / 40.0)};
    trajectory.velocities = {
        Eigen::VectorXd::Zero(1),
        Eigen::VectorXd::Constant(1, 1.0 / (12.0 * dilation))};
    trajectory.accelerations = {
        Eigen::VectorXd::Zero(1),
        Eigen::VectorXd::Constant(1, 1.0 / (6.0 * dilation * dilation))};
    trajectory_detail::CartesianTrajectoryContractInput request;
    request.speed_profile.velocity_scale = 1.0;
    request.speed_profile.acceleration_scale = 1.0;
    const std::array<double, 1U> limits{1.0};
    const auto result =
        trajectory_detail::measure_cartesian_joint_dynamics_envelope(
            request, trajectory, limits, limits, {});
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value().complete);
    ASSERT_EQ(result.value().joint_derivatives.size(), 1U);
    EXPECT_NEAR(result.value().joint_derivatives.front().jerk,
                0.25 / dilation / dilation / dilation, 1e-11);
  }
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
      "cartesian_mimic_chain",
      roboplan::loadUrdfSceneDescriptionFromXml(urdf));
  const auto imported = scene->importSrdf(srdf);
  ASSERT_TRUE(imported.has_value()) << imported.error();
  const auto group = scene->getJointGroupInfo("arm");
  ASSERT_TRUE(group.has_value()) << group.error();
  ASSERT_EQ(group->joint_names, std::vector<std::string>{"source"});

  const auto bounds =
      trajectory_detail::resolve_cartesian_kinematic_motion_bounds(
          scene, "arm", group->joint_names);
  ASSERT_TRUE(bounds.ok()) << bounds.error().message;
  EXPECT_DOUBLE_EQ(
      static_cast<double>(bounds.value().prismatic_amplification[0]), 9.0);

  trajectory_detail::CartesianTrajectoryContractInput request;
  request.compiled_group_name = "arm";
  request.compiled_base_frame = "base";
  request.compiled_tcp_frame = "tip";
  request.speed_profile.velocity_scale = 1.0;
  request.speed_profile.acceleration_scale = 1.0;
  CartesianPathDTO path;
  request.speed_profile.max_tcp_linear_speed_mps = 10.0;
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

  const auto envelope =
      trajectory_detail::measure_cartesian_frame_dynamics_envelope(
          request, &path, scene, trajectory, control);

  ASSERT_TRUE(envelope.ok()) << envelope.error().message;
  ASSERT_TRUE(envelope.value().complete);
  // A rest-to-rest unit quintic peaks at 1.875 * delta / duration.
  // This serial chain moves (1 + 2 + 6) times the source coordinate.
  constexpr double kExactPeakLinearSpeedMps = 1.875 * 0.1 * 9.0;
  EXPECT_GE(envelope.value().linear_speed_ratio * 10.0,
            kExactPeakLinearSpeedMps * 0.999);
  const auto linear_only = trajectory_detail::measure_cartesian_frame_dynamics_envelope(
      request, nullptr, scene, trajectory, control);
  ASSERT_TRUE(linear_only.ok()) << linear_only.error().message;
  ASSERT_TRUE(linear_only.value().complete);
  EXPECT_GE(linear_only.value().linear_speed_ratio * 10.0,
            kExactPeakLinearSpeedMps * 0.999);
  EXPECT_DOUBLE_EQ(linear_only.value().angular_speed_ratio, 0.0);
  EXPECT_DOUBLE_EQ(linear_only.value().linear_acceleration_ratio, 0.0);
  EXPECT_DOUBLE_EQ(linear_only.value().angular_acceleration_ratio, 0.0);
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
