#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <filesystem>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>
#include <manyforge_roboplan/roboplan_candidate_computer.hpp>
#include <memory>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using namespace manyforge::core;

struct CandidateFixture final {
  std::shared_ptr<roboplan::Scene> scene;
  RoboPlanCandidateBindingDTO binding;
  Eigen::VectorXd start;
  Eigen::VectorXd goal;
};

[[nodiscard]] std::vector<double> values(const Eigen::VectorXd& vector) {
  return {vector.data(), vector.data() + vector.size()};
}

[[nodiscard]] Pose3DTO pose(const Eigen::Matrix4d& transform) {
  const Eigen::Quaterniond orientation(transform.block<3, 3>(0, 0));
  return Pose3DTO{
      Vector3DTO{transform(0, 3), transform(1, 3), transform(2, 3)},
      QuaternionDTO{orientation.x(), orientation.y(), orientation.z(),
                    orientation.w()}};
}

[[nodiscard]] CandidateFixture make_fixture(
    std::string component = "component-a",
    std::string model = "model-a") {
  const auto models = roboplan::example_models::get_package_models_dir();
  const auto package_share =
      roboplan::example_models::get_package_share_dir();
  auto scene = std::make_shared<roboplan::Scene>(
      "candidate-scene", models / "ur_robot_model/ur5_gripper.urdf",
      models / "ur_robot_model/ur5_gripper.srdf",
      std::vector<std::filesystem::path>{package_share},
      models / "ur_robot_model/ur5_config.yaml");
  const auto group = scene->getJointGroupInfo("arm");
  if (!group.has_value()) {
    throw std::runtime_error(group.error());
  }
  scene->setRngSeed(0x5a17U);
  const auto start = scene->randomCollisionFreePositions();
  const auto goal = scene->randomCollisionFreePositions();
  if (!start.has_value() || !goal.has_value()) {
    throw std::runtime_error("could not construct collision-free fixture");
  }
  std::vector<ResourceId> resources;
  resources.reserve(group->joint_names.size());
  for (const auto& joint : group->joint_names) {
    resources.emplace_back("resource/" + joint);
  }
  const ControllableComponentId component_id(std::move(component));
  const ModelInstanceId model_id(std::move(model));
  const ModelRevision model_revision("model-revision-a");
  const ResolvedFrameDTO base = ResolvedComponentModelFrameDTO{
      component_id, model_id, FrameRoleId("base"), "base",
      model_revision};
  const ResolvedFrameDTO tcp = ResolvedComponentModelFrameDTO{
      component_id, model_id, FrameRoleId("tcp"), "tool0",
      model_revision};
  return CandidateFixture{
      scene,
      RoboPlanCandidateBindingDTO{
          component_id, CommandTargetId("arm-target"), model_id,
          model_revision, "arm", resources, group->joint_names, {base, tcp}},
      start.value(), goal.value()};
}

[[nodiscard]] ControlledStateDTO controlled_state(
    const RoboPlanCandidateBindingDTO& binding,
    const Eigen::VectorXd& positions) {
  std::vector<CoordinatePositionDTO> coordinates;
  coordinates.reserve(static_cast<std::size_t>(positions.size()));
  for (Eigen::Index index = 0; index < positions.size(); ++index) {
    coordinates.emplace_back(AngularPositionDTO{positions[index]});
  }
  return ControlledStateDTO{
      .controllable_component_id = binding.controllable_component_id,
      .command_target_id = binding.command_target_id,
      .control_domain_id = ControlDomainId("domain-a"),
      .control_domain_revision = ControlDomainRevision("domain-revision-a"),
      .control_binding_id = ControlBindingId("binding-a"),
      .control_binding_revision =
          ControlBindingRevision("binding-revision-a"),
      .controller_topology_id = ControllerTopologyId("topology-a"),
      .topology_revision = TopologyRevision("topology-revision-a"),
      .control_route_id = ControlRouteId("route-a"),
      .active_control_configuration_id =
          ActiveControlConfigurationId("configuration-a"),
      .active_control_configuration_generation =
          ActiveControlConfigurationGeneration(1U),
      .resources = binding.resources,
      .joint_names = binding.compiled_joint_names,
      .positions = std::move(coordinates),
      .velocities = std::nullopt};
}

[[nodiscard]] RoboPlanCandidateRequestDTO request(
    const CandidateFixture& fixture, ResolvedMotionSpecDTO motion,
    const Eigen::VectorXd* start = nullptr) {
  return RoboPlanCandidateRequestDTO{
      .controllable_component_id =
          fixture.binding.controllable_component_id,
      .command_target_id = fixture.binding.command_target_id,
      .model_instance_id = fixture.binding.model_instance_id,
      .model_revision = fixture.binding.model_revision,
      .resources = fixture.binding.resources,
      .base_frame = fixture.binding.permitted_frames[0],
      .tcp_frame = fixture.binding.permitted_frames[1],
      .start_state = controlled_state(
          fixture.binding, start == nullptr ? fixture.start : *start),
      .motion = std::move(motion),
      .tolerances = TolerancesDTO{1.0e-4, 5.0e-3, 1.0e-2},
      .speed_profile = SpeedProfileDTO{0.5, 0.5, 0.1},
      .planning_profile = PlanningProfileDTO::kFirstFeasible,
      .dynamics_limits = {},
      .planning_timeout_s = 3.0,
      .planning_attempt_index = 0U,
      .action_span = std::nullopt};
}

[[nodiscard]] ResolvedJointGoalDTO joint_goal(
    const CandidateFixture& fixture) {
  return ResolvedJointGoalDTO{fixture.binding.resources,
                              fixture.binding.compiled_joint_names,
                              values(fixture.goal)};
}

TEST(RoboPlanCandidateComputerTest,
     JointGoalReturnsTheExactAddressedPortableTrajectory) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01});

  const auto result = computer.compute(
      request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)}));

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().runtime_scale_bounds);
  ASSERT_TRUE(result.value().runtime_scale_bounds->nominal_time_multiplier);
  EXPECT_GE(*result.value().runtime_scale_bounds->nominal_time_multiplier, 1.0);
  EXPECT_EQ(result.value().resources, fixture.binding.resources);
  EXPECT_EQ(result.value().joint_names,
            fixture.binding.compiled_joint_names);
  ASSERT_GE(result.value().waypoints.size(), 2U);
  EXPECT_EQ(result.value().waypoints.front().positions_rad,
            values(fixture.start));
  EXPECT_EQ(result.value().waypoints.back().positions_rad,
            values(fixture.goal));
}

TEST(RoboPlanCandidateComputerTest,
     FinalInterpolantJerkCapReservesLiveScalingHeadroom) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  for (std::size_t index = 0; index < fixture.binding.resources.size();
       ++index) {
    JointDynamicsLimitDTO limit;
    limit.max_jerk_rad_s3 = 10.0;
    input.dynamics_limits.push_back(
        {fixture.binding.resources[index],
         fixture.binding.compiled_joint_names[index], limit});
  }
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().runtime_scale_bounds);
  roboplan::JointTrajectory native;
  native.joint_names = result.value().joint_names;
  for (const auto &waypoint : result.value().waypoints) {
    native.times.push_back(waypoint.time_from_start_s);
    native.positions.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        waypoint.positions_rad.data(), waypoint.positions_rad.size()));
    native.velocities.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        waypoint.velocities_rad_s.data(), waypoint.velocities_rad_s.size()));
    native.accelerations.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        waypoint.accelerations_rad_s2.data(),
        waypoint.accelerations_rad_s2.size()));
  }
  const auto model_v = fixture.scene->getVelocityLimitVectors("arm");
  const auto model_a = fixture.scene->getAccelerationLimitVectors("arm");
  ASSERT_TRUE(model_v && model_a);
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  const auto measured = detail::measure_cartesian_joint_dynamics_envelope(
      contract, native, values(model_v->second), values(model_a->second), {});
  ASSERT_TRUE(measured.ok());
  ASSERT_TRUE(measured.value().complete);
  const auto &bounds = *result.value().runtime_scale_bounds;
  EXPECT_GE(bounds.max_stop_path_rate, 1.0);
  for (std::size_t i = 0; i < measured.value().joint_derivatives.size(); ++i) {
    EXPECT_LE(measured.value().joint_derivatives[i].velocity *
                  bounds.max_stop_path_rate,
              model_v->second[static_cast<Eigen::Index>(i)] *
                      input.speed_profile.velocity_scale +
                  1e-9);
  }
  core::detail::RuntimeScaleGovernor scale(
      1.0, {bounds.max_rate_per_s, bounds.max_rate_change_per_s2});
  ASSERT_TRUE(scale.retarget(0.2));
  RecordProperty(
      "trajectory_duration_s",
      std::to_string(result.value().waypoints.back().time_from_start_s));
  RecordProperty("scale_100_to_20_percent_s",
                 std::to_string(scale.remaining_transition_s()));
  for (const auto &derivative : measured.value().joint_derivatives) {
    EXPECT_LE(derivative.jerk, 8.0 + 1e-6);
    EXPECT_LE(derivative.jerk +
                  3.0 * derivative.acceleration * bounds.max_rate_per_s +
                  derivative.velocity * bounds.max_rate_change_per_s2,
              10.0 + 1e-6);
  }
}

TEST(RoboPlanCandidateComputerTest,
     JointMoveStopHeadroomIncludesItsTcpSpeedCap) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  input.speed_profile.max_tcp_linear_speed_mps = 0.05;
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().runtime_scale_bounds);
  const double rate = result.value().runtime_scale_bounds->max_stop_path_rate;
  ASSERT_GE(rate, 1.0);
  // Re-measure the final curve traversed at the maximum certified Stop rate.
  // This exercises the complete producer, not just a hand-written DTO.
  roboplan::JointTrajectory native;
  native.joint_names = result.value().joint_names;
  for (const auto &waypoint : result.value().waypoints) {
    native.times.push_back(waypoint.time_from_start_s / rate);
    native.positions.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        waypoint.positions_rad.data(), waypoint.positions_rad.size()));
    native.velocities.emplace_back(
        rate *
        Eigen::Map<const Eigen::VectorXd>(waypoint.velocities_rad_s.data(),
                                          waypoint.velocities_rad_s.size()));
    native.accelerations.emplace_back(
        rate * rate *
        Eigen::Map<const Eigen::VectorXd>(
            waypoint.accelerations_rad_s2.data(),
            waypoint.accelerations_rad_s2.size()));
  }
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  const auto envelope = detail::measure_cartesian_frame_dynamics_envelope(
      contract, nullptr, fixture.scene, native, {});
  ASSERT_TRUE(envelope.ok()) << envelope.error().message;
  ASSERT_TRUE(envelope.value().complete);
  // Independently subdividing the dilated curve has a different conservative
  // residual, bounded by the primitive's declared adaptive resolution.
  EXPECT_LE(envelope.value().linear_speed_ratio, 1.02 + 1e-10);
}

TEST(RoboPlanCandidateComputerTest,
     EarlyAndSteadyStopUseActualRoboPlanOutputWithJerkOff) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto candidate = computer.compute(
      request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)}));
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  const auto &trajectory = candidate.value();
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "test-stop";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 1.0;
  for (const auto &joint : trajectory.joint_names)
    profile.per_joint_max_deceleration.emplace(joint,
                                               AngularAccelerationDTO{4.0});
  SoftOnPathPlannerConfig config;
  config.allow_model_velocity_fallback = true;
  config.jerk_limits_enabled = false;
  config.stop_traj_delta_s = 0.02;
  for (double fraction : {0.02, 0.4}) {
    const double cursor =
        trajectory.waypoints.back().time_from_start_s * fraction;
    const auto current = sample_trajectory(trajectory, cursor);
    ASSERT_TRUE(current.ok());
    SoftOnPathInitialStateDTO initial;
    initial.state = {trajectory.joint_names, current.value().positions_rad,
                     current.value().velocities_rad_s};
    initial.trajectory_time_from_start_s = cursor;
    initial.velocity_source = SoftOnPathStateSource::kModelDerived;
    initial.runtime_scale_target_at_trip = 1.0;
    const auto stopped =
        plan_soft_on_path_stop(trajectory, profile, initial, config);
    ASSERT_TRUE(stopped.ok())
        << "fraction=" << fraction << ": " << stopped.error().message;
    EXPECT_EQ(stopped.value().jerk_limit_source, "disabled");
    for (double velocity : stopped.value()
                               .replacement_trajectory.waypoints.back()
                               .velocities_rad_s)
      EXPECT_NEAR(velocity, 0.0, 1e-10);
  }
}

TEST(RoboPlanCandidateComputerTest,
     JointSubsetRetainsAuthoredPairsAndUsesStartForUnselectedCoordinates) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01});
  const std::size_t last = fixture.binding.resources.size() - 1U;
  const ResolvedJointGoalDTO subset{
      {fixture.binding.resources[last], fixture.binding.resources.front()},
      {fixture.binding.compiled_joint_names[last], fixture.binding.compiled_joint_names.front()},
      {fixture.goal[static_cast<Eigen::Index>(last)], fixture.goal[0]}};

  const auto result = computer.compute(request(fixture, ResolvedMotionSpecDTO{subset}));

  ASSERT_TRUE(result.ok()) << result.error().message;
  auto expected = fixture.start;
  expected[0] = fixture.goal[0];
  expected[static_cast<Eigen::Index>(last)] = fixture.goal[static_cast<Eigen::Index>(last)];
  EXPECT_EQ(result.value().waypoints.back().positions_rad, values(expected));
  EXPECT_EQ(result.value().resources, fixture.binding.resources);
  EXPECT_EQ(result.value().joint_names, fixture.binding.compiled_joint_names);
}

TEST(RoboPlanCandidateComputerTest, ScalesAbsoluteJointCapsIndependentlyAfterResolvingCeilings) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01});
  for (const auto& [velocity_scale, acceleration_scale] :
       {std::pair{0.25, 0.8}, std::pair{0.8, 0.25}, std::pair{0.0005, 0.8}}) {
    SCOPED_TRACE(velocity_scale);
    if (velocity_scale < 0.001) {
      // A short path keeps this sub-floor regression small. Retiming must
      // reduce the model-scaled velocity to the tighter cap, never raise it.
      fixture.goal = fixture.start;
      fixture.goal[0] += 0.001;
    }
    auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
    input.speed_profile = {velocity_scale, acceleration_scale, std::nullopt};
    for (std::size_t index = 0; index < fixture.binding.resources.size(); ++index) {
      input.dynamics_limits.push_back(
          {fixture.binding.resources[index],
           fixture.binding.compiled_joint_names[index],
           {0.7, 1.2, std::nullopt}});
    }
    const auto result = computer.compute(input);
    ASSERT_TRUE(result.ok()) << result.error().message;
    ASSERT_TRUE(result.value().runtime_scale_bounds);
    const double stop_rate =
        result.value().runtime_scale_bounds->max_stop_path_rate;
    for (const auto &waypoint : result.value().waypoints) {
      ASSERT_EQ(waypoint.velocities_rad_s.size(),
                fixture.binding.resources.size());
      ASSERT_EQ(waypoint.accelerations_rad_s2.size(),
                fixture.binding.resources.size());
      for (const auto velocity : waypoint.velocities_rad_s)
        EXPECT_LE(std::abs(velocity), 0.7 * velocity_scale);
      for (const auto velocity : waypoint.velocities_rad_s)
        EXPECT_LE(stop_rate * std::abs(velocity), 0.7 * velocity_scale + 1e-10);
      for (const auto acceleration : waypoint.accelerations_rad_s2)
        EXPECT_LE(std::abs(acceleration), 1.2 * acceleration_scale);
    }
  }
}

TEST(RoboPlanCandidateComputerTest,
     AlreadyAtTargetUsesOrdinaryStationaryTrajectory) {
  auto fixture = make_fixture();
  fixture.goal = fixture.start;
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  const auto current =
      pose(fixture.scene->forwardKinematics(fixture.start, "tool0", "base"));
  CartesianPathDTO path;
  path.waypoints = {current, current};
  for (const auto &motion : std::vector<ResolvedMotionSpecDTO>{
           joint_goal(fixture), PoseGoalDTO{current}, path}) {
    SCOPED_TRACE(motion.index());
    const auto result = computer.compute(request(fixture, motion));
    EXPECT_TRUE(result.ok()) << result.error().message;
    if (!result.ok())
      continue;
    ASSERT_GE(result.value().waypoints.size(), 2U);
    const double duration = result.value().waypoints.back().time_from_start_s;
    EXPECT_GT(duration, 0.0);
    if (std::holds_alternative<CartesianPathDTO>(motion)) {
      EXPECT_EQ(result.value().cartesian_waypoint_times_s,
                (std::vector<double>{0.0, duration}));
    }
    for (const auto &waypoint : result.value().waypoints) {
      ASSERT_EQ(waypoint.positions_rad.size(),
                static_cast<std::size_t>(fixture.start.size()));
      for (std::size_t joint = 0U; joint < waypoint.positions_rad.size();
           ++joint)
        EXPECT_NEAR(waypoint.positions_rad[joint],
                    fixture.start[static_cast<Eigen::Index>(joint)], 1e-8);
    }
  }
}

TEST(RoboPlanCandidateComputerTest,
     ReturningToStartDoesNotDiscardIntermediateCartesianDestination) {
  auto fixture = make_fixture();
  fixture.start = fixture.goal;
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  const Eigen::Matrix4d start =
      fixture.scene->forwardKinematics(fixture.start, "tool0", "base");
  Eigen::Matrix4d middle = start;
  middle(0, 3) += 0.01;
  CartesianPathDTO path;
  path.waypoints = {pose(start), pose(middle), pose(start)};
  path.max_translation_step_m = 0.005;
  path.max_rotation_step_rad = 0.05;
  const auto candidate_request = request(fixture, path);
  const auto result = computer.compute(candidate_request);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().cartesian_fidelity_metrics.has_value());
  const auto &fidelity = *result.value().cartesian_fidelity_metrics;
  EXPECT_DOUBLE_EQ(fidelity.requested_translation_tolerance_m,
                   candidate_request.tolerances.translation_tol_m);
  EXPECT_DOUBLE_EQ(fidelity.requested_rotation_tolerance_rad,
                   candidate_request.tolerances.rotation_tol_rad);
  EXPECT_LE(fidelity.certified_max_translation_error_m,
            candidate_request.tolerances.translation_tol_m);
  EXPECT_LE(fidelity.certified_max_rotation_error_rad,
            candidate_request.tolerances.rotation_tol_rad);
  EXPECT_LE(fidelity.accepted_max_blend_deviation_rad,
            fidelity.requested_max_blend_deviation_rad);
  EXPECT_GE(fidelity.planning_attempt_count, 1U);
  EXPECT_LE(fidelity.planning_attempt_count, 5U);
  ASSERT_EQ(result.value().cartesian_waypoint_times_s.size(), 3U);
  EXPECT_GT(result.value().cartesian_waypoint_times_s[1], 0.0);
  EXPECT_LT(result.value().cartesian_waypoint_times_s[1],
            result.value().cartesian_waypoint_times_s.back());
  const auto sample = sample_trajectory(
      result.value(), result.value().cartesian_waypoint_times_s[1]);
  ASSERT_TRUE(sample.ok());
  const auto positions = Eigen::Map<const Eigen::VectorXd>(
      sample.value().positions_rad.data(), sample.value().positions_rad.size());
  const auto actual = fixture.scene->forwardKinematics(
      fixture.scene->toFullJointPositions("arm", positions), "tool0", "base");
  EXPECT_LE((actual.block<3, 1>(0, 3) - middle.block<3, 1>(0, 3)).norm(),
            candidate_request.tolerances.translation_tol_m);
}

TEST(RoboPlanCandidateComputerTest,
     PoseAndCartesianInputsUseTheSameExactProjection) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  const auto goal_pose = pose(fixture.scene->forwardKinematics(
      fixture.goal, "tool0", "base"));

  const auto pose_result = computer.compute(
      request(fixture, ResolvedMotionSpecDTO{PoseGoalDTO{goal_pose}}));
  ASSERT_TRUE(pose_result.ok()) << pose_result.error().message;
  EXPECT_EQ(pose_result.value().resources, fixture.binding.resources);

  const Eigen::VectorXd cartesian_start =
      fixture.scene->getCurrentJointPositions();
  const Eigen::Matrix4d start_pose =
      fixture.scene->forwardKinematics(cartesian_start, "tool0", "base");
  Eigen::Matrix4d end_pose = start_pose;
  end_pose(0, 3) += 0.01;
  Eigen::Matrix4d middle_pose = start_pose;
  middle_pose(0, 3) += 0.005;
  CartesianPathDTO path;
  path.waypoints = {pose(start_pose), pose(middle_pose), pose(end_pose)};
  path.max_translation_step_m = 0.005;
  path.max_rotation_step_rad = 0.05;
  path.max_angular_speed_rad_s = 0.25;
  path.max_linear_acceleration_mps2 = 0.25;
  path.max_angular_acceleration_rad_s2 = 1.0;
  path.max_blend_deviation_rad = 0.0;
  const auto cartesian_result = computer.compute(request(
      fixture, ResolvedMotionSpecDTO{std::move(path)}, &cartesian_start));
  ASSERT_TRUE(cartesian_result.ok()) << cartesian_result.error().message;
  ASSERT_TRUE(cartesian_result.value().cartesian_fidelity_metrics.has_value());
  EXPECT_DOUBLE_EQ(
      cartesian_result.value().cartesian_fidelity_metrics
          ->requested_max_blend_deviation_rad,
      0.0);
  EXPECT_DOUBLE_EQ(
      cartesian_result.value().cartesian_fidelity_metrics
          ->accepted_max_blend_deviation_rad,
      0.0);
  EXPECT_EQ(cartesian_result.value().cartesian_fidelity_metrics
                ->planning_attempt_count,
            1U);
  EXPECT_EQ(cartesian_result.value().resources, fixture.binding.resources);
  EXPECT_EQ(cartesian_result.value().joint_names,
            fixture.binding.compiled_joint_names);
  const auto &milestones = cartesian_result.value().cartesian_waypoint_times_s;
  ASSERT_EQ(milestones.size(), 3U);
  EXPECT_DOUBLE_EQ(milestones.front(), 0.0);
  EXPECT_DOUBLE_EQ(milestones.back(),
                   cartesian_result.value().waypoints.back().time_from_start_s);
  ASSERT_GT(milestones[1], 0.0);
  ASSERT_LT(milestones[1], milestones.back());
  const auto sample =
      sample_trajectory(cartesian_result.value(), milestones[1]);
  ASSERT_TRUE(sample.ok());
  const auto positions = Eigen::Map<const Eigen::VectorXd>(
      sample.value().positions_rad.data(), sample.value().positions_rad.size());
  const auto actual = fixture.scene->forwardKinematics(
      fixture.scene->toFullJointPositions("arm", positions), "tool0", "base");
  EXPECT_LE((actual.block<3, 1>(0, 3) - middle_pose.block<3, 1>(0, 3)).norm(),
            5e-3);
  EXPECT_TRUE(pose_result.value().cartesian_waypoint_times_s.empty());
  EXPECT_FALSE(pose_result.value().cartesian_fidelity_metrics.has_value());
}

TEST(RoboPlanCandidateComputerTest,
     CrossInstanceAndGeneratedNameMismatchesFailBeforePlanning) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  auto mismatched =
      request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  mismatched.model_instance_id = ModelInstanceId("model-b");

  const auto model_result = computer.compute(mismatched);
  ASSERT_FALSE(model_result.ok());
  EXPECT_EQ(model_result.error().details.at("reason"),
            "native_candidate_request_binding_mismatch");

  mismatched = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  auto& goal = std::get<ResolvedJointGoalDTO>(mismatched.motion);
  goal.joint_names.front() = "caller_selected_joint";
  const auto name_result = computer.compute(mismatched);
  ASSERT_FALSE(name_result.ok());
  EXPECT_EQ(name_result.error().details.at("reason"),
            "native_joint_goal_binding_mismatch");
}

TEST(RoboPlanCandidateComputerTest,
     UnresolvedTargetsInvalidCapsAndCancellationFailClosed) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});

  const auto named = computer.compute(request(
      fixture, ResolvedMotionSpecDTO{NamedTargetDTO{"home"}}));
  ASSERT_FALSE(named.ok());
  EXPECT_EQ(named.error().details.at("reason"),
            "unresolved_named_target");

  auto capped = request(fixture,
                        ResolvedMotionSpecDTO{joint_goal(fixture)});
  JointDynamicsLimitDTO jerk;
  jerk.max_jerk_rad_s3 = -1.0;
  capped.dynamics_limits.push_back(ResolvedResourceDynamicsLimitDTO{
      fixture.binding.resources.front(),
      fixture.binding.compiled_joint_names.front(), jerk});
  const auto cap_result = computer.compute(capped);
  ASSERT_FALSE(cap_result.ok());
  EXPECT_EQ(cap_result.error().details.at("reason"),
            "invalid_joint_dynamics_limit");

  capped.dynamics_limits.front().limit.max_jerk_rad_s3 = 1.0;
  capped.dynamics_limits.front().compiled_joint_name = "absent_joint";
  const auto absent_joint = computer.compute(capped);
  ASSERT_FALSE(absent_joint.ok());
  EXPECT_EQ(absent_joint.error().details.at("reason"),
            "invalid_joint_dynamics_limit");
  capped.dynamics_limits.front().resource_id = ResourceId("absent_resource");
  const auto absent_resource = computer.compute(capped);
  ASSERT_FALSE(absent_resource.ok());
  EXPECT_EQ(absent_resource.error().details.at("reason"),
            "unsupported_joint_dynamics_limit");

  const RoboPlanCandidateControl cancelled{[] { return true; }, {}};
  const auto cancelled_result = computer.compute(
      request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)}), cancelled);
  ASSERT_FALSE(cancelled_result.ok());
  EXPECT_EQ(cancelled_result.error().details.at("reason"),
            "planning_cancelled_by_interrupt");
}

} // namespace
} // namespace manyforge::roboplan_backend
