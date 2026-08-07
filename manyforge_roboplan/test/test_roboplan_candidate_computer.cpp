#include <gtest/gtest.h>

#include <manyforge_roboplan/roboplan_candidate_computer.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>

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
      .speed_profile = SpeedProfileDTO{0.5, 0.5, std::nullopt},
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

TEST(RoboPlanCandidateComputerTest, PoseAndCartesianInputsUseTheSameExactProjection) {
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
  const Eigen::Matrix4d start_pose = fixture.scene->forwardKinematics(
      cartesian_start, "tool0", "base");
  Eigen::Matrix4d end_pose = start_pose;
  end_pose(0, 3) += 0.01;
  CartesianPathDTO path;
  path.waypoints = {pose(start_pose), pose(end_pose)};
  path.max_translation_step_m = 0.005;
  path.max_rotation_step_rad = 0.05;
  path.max_linear_speed_mps = 0.05;
  path.max_angular_speed_rad_s = 0.25;
  path.max_linear_acceleration_mps2 = 0.25;
  path.max_angular_acceleration_rad_s2 = 1.0;
  const auto cartesian_result = computer.compute(request(
      fixture, ResolvedMotionSpecDTO{std::move(path)}, &cartesian_start));
  ASSERT_TRUE(cartesian_result.ok()) << cartesian_result.error().message;
  EXPECT_EQ(cartesian_result.value().resources,
            fixture.binding.resources);
  EXPECT_EQ(cartesian_result.value().joint_names,
            fixture.binding.compiled_joint_names);
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
     UnresolvedTargetsUnsupportedCapsAndCancellationFailClosed) {
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
  jerk.max_jerk_rad_s3 = 1.0;
  capped.dynamics_limits.push_back(ResolvedResourceDynamicsLimitDTO{
      fixture.binding.resources.front(),
      fixture.binding.compiled_joint_names.front(), jerk});
  const auto cap_result = computer.compute(capped);
  ASSERT_FALSE(cap_result.ok());
  EXPECT_EQ(cap_result.error().details.at("reason"),
            "unsupported_joint_dynamics_limit");
  EXPECT_EQ(cap_result.error().details.at("dimension"), "jerk");

  const RoboPlanCandidateControl cancelled{[] { return true; }, {}};
  const auto cancelled_result = computer.compute(
      request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)}),
      cancelled);
  ASSERT_FALSE(cancelled_result.ok());
  EXPECT_EQ(cancelled_result.error().details.at("reason"),
            "planning_cancelled_by_interrupt");
}

}  // namespace
}  // namespace manyforge::roboplan_backend
