#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>
#include <filesystem>
#include <functional>
#include <map>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/detail/cartesian_path_densification.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>
#include <manyforge_roboplan/roboplan_candidate_computer.hpp>
#include <manyforge_roboplan/roboplan_planner_options.hpp>
#include <memory>
#include <optional>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>
#include <roboplan_toppra/toppra.hpp>
#include <sstream>
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
      "candidate-scene", roboplan::loadUrdfSceneDescription(
                             models / "ur_robot_model/ur5_gripper.urdf",
                             {package_share}));
  scene->importJointLimitsFromConfig(roboplan::loadJointLimitsConfig(
      models / "ur_robot_model/ur5_config.yaml"));
  if (const auto imported = scene->importSrdf(roboplan::loadTextFile(
          models / "ur_robot_model/ur5_gripper.srdf"));
      !imported.has_value()) {
    throw std::runtime_error(imported.error());
  }
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
      .planner = {},
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

// A small fixed prefix around a collision-free captured start, with moving
// P/V/A. It represents the Kernel's already retained source; no old
// certificates are transmitted or reused when the new candidate is measured.
[[nodiscard]] RoboPlanCandidateRequestDTO
moving_request(CandidateFixture &fixture) {
  const Eigen::VectorXd direction = (fixture.goal - fixture.start).normalized();
  fixture.goal = fixture.start + 0.02 * direction;
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  const Eigen::VectorXd velocity = 0.001 * direction;
  const Eigen::VectorXd acceleration = 0.0001 * direction;
  constexpr double divergence = 0.125;
  auto prefix_point = [&](double time) {
    return TrajectoryWaypointDTO{time,
                                 values(fixture.start + time * velocity +
                                        0.5 * time * time * acceleration),
                                 values(velocity + time * acceleration),
                                 values(acceleration)};
  };
  TrajectoryDTO prefix{fixture.binding.resources,
                       fixture.binding.compiled_joint_names,
                       {prefix_point(0.0), prefix_point(divergence / 2.0),
                        prefix_point(divergence)}};
  input.start_state.velocities = std::vector<CoordinateVelocityDTO>{};
  for (double value : values(velocity))
    input.start_state.velocities->push_back(AngularVelocityDTO{value});
  auto seed = controlled_state(
      fixture.binding, fixture.start + divergence * velocity +
                           0.5 * divergence * divergence * acceleration);
  input.moving_replacement = MovingReplacementInputDTO{
      ExecutionGoalKey{ControllerGoalId("source-moving"),
                       ControllerGoalGeneration(11U)},
      ExecutionBindingEpoch(7U),
      1.0,
      std::move(prefix),
      1.0 + divergence,
      std::move(seed),
      input.base_frame,
      input.tcp_frame,
      input.speed_profile,
      input.dynamics_limits,
      30.0};
  return input;
}

[[nodiscard]] roboplan::JointTrajectory native_trajectory(
    const TrajectoryDTO &trajectory);

TEST(RoboPlanCandidateComputerTest, QualifiedTargetEvidenceRequiresFiniteInLimitGoal) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding, {0x1234U, 65536U, 0.01});
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  int checks = 0;
  RoboPlanCandidateControl control;
  control.deadline_expired = [&checks] { return ++checks >= 3; };
  std::optional<ControlledStateDTO> target;
  const auto result = computer.compute(input, control, &target);
  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(target);
  ASSERT_EQ(target->resources, fixture.binding.resources);
  EXPECT_EQ(target->joint_names, fixture.binding.compiled_joint_names);
  const auto projected = angular_position_projection(*target);
  ASSERT_TRUE(projected.ok());
  EXPECT_EQ(projected.value(), values(fixture.goal));
  EXPECT_FALSE(target->velocities);

  auto invalid = joint_goal(fixture);
  invalid.positions_rad[0] = 1e9;
  input.motion = invalid;
  checks = 0;
  target.reset();
  static_cast<void>(computer.compute(input, control, &target));
  EXPECT_FALSE(target);
  auto invalid_target = controlled_state(fixture.binding, fixture.goal);
  invalid_target.positions[0] = AngularPositionDTO{1e9};
  input.collision_mode = PlannerCollisionMode::kDiagnosticDisabled;
  input.diagnostic_qualified_target = invalid_target;
  const auto rejected = computer.compute(input);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "native_diagnostic_target_invalid");
}

TEST(
    RoboPlanCandidateComputerTest,
    MovingCandidatePreservesPrefixAndEntersAMovingSuffixWithoutGlobalRetiming) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  const ResolvedFrameDTO source_tcp = ResolvedComponentModelFrameDTO{
      fixture.binding.controllable_component_id,
      fixture.binding.model_instance_id, FrameRoleId("source-tcp"),
      "wrist_3_link", fixture.binding.model_revision};
  ASSERT_TRUE(fixture.scene->getFrameId("wrist_3_link"));
  fixture.binding.permitted_frames.push_back(source_tcp);
  input.moving_replacement->source_tcp_frame = source_tcp;
  for (std::size_t index = 0; index < fixture.binding.resources.size(); ++index)
    input.dynamics_limits.push_back(
        {fixture.binding.resources[index],
         fixture.binding.compiled_joint_names[index],
         JointDynamicsLimitDTO{std::nullopt, std::nullopt, 10.0}});
  input.moving_replacement->source_per_resource_dynamics_limits =
      input.dynamics_limits;
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});

  const auto result = computer.compute(input);

  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &curve = result.value();
  const auto &prefix = input.moving_replacement->source_prefix_trajectory;
  ASSERT_GT(curve.waypoints.size(), prefix.waypoints.size());
  for (std::size_t index = 0; index < prefix.waypoints.size(); ++index) {
    EXPECT_EQ(curve.waypoints[index].time_from_start_s,
              prefix.waypoints[index].time_from_start_s);
    EXPECT_EQ(curve.waypoints[index].positions_rad,
              prefix.waypoints[index].positions_rad);
    EXPECT_EQ(curve.waypoints[index].velocities_rad_s,
              prefix.waypoints[index].velocities_rad_s);
    EXPECT_EQ(curve.waypoints[index].accelerations_rad_s2,
              prefix.waypoints[index].accelerations_rad_s2);
  }
  const auto &entry = curve.waypoints[prefix.waypoints.size()];
  EXPECT_TRUE(std::any_of(entry.velocities_rad_s.begin(),
                          entry.velocities_rad_s.end(),
                          [](double value) { return value != 0.0; }));
  EXPECT_GT(entry.time_from_start_s, prefix.waypoints.back().time_from_start_s);
  EXPECT_EQ(curve.waypoints.back().positions_rad, values(fixture.goal));
  for (const double velocity : curve.waypoints.back().velocities_rad_s)
    EXPECT_EQ(velocity, 0.0);
  for (const double acceleration : curve.waypoints.back().accelerations_rad_s2)
    EXPECT_EQ(acceleration, 0.0);
  ASSERT_TRUE(curve.runtime_scale_bounds);
  EXPECT_GT(curve.runtime_scale_bounds->max_rate_per_s, 0.0);
  EXPECT_GT(curve.runtime_scale_bounds->max_rate_change_per_s2, 0.0);
  EXPECT_GE(curve.runtime_scale_bounds->max_stop_path_rate, 1.0);
  // Measure only the new connector and its seam with the retained prefix.
  // The prefix is checked against its own source TCP above and is never retimed.
  auto suffix = curve;
  suffix.waypoints.erase(suffix.waypoints.begin(),
                         suffix.waypoints.begin() + prefix.waypoints.size() - 1);
  const detail::CartesianTrajectoryContractInput tcp_contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  const auto chord = detail::measure_tcp_chord_speed_ratio(
      tcp_contract, fixture.scene, native_trajectory(suffix), {});
  ASSERT_TRUE(chord.ok()) << chord.error().message;
  EXPECT_LE(chord.value(), 1.0 + 1e-9);
  // Independently measure the delivered connector alone. Its fixed P/V/A
  // must retain ordinary planning's nominal acceleration and enabled jerk
  // reserve, rather than merely remaining infinitesimally below physical caps.
  roboplan::JointTrajectory connector;
  connector.joint_names = curve.joint_names;
  for (const auto index : {prefix.waypoints.size() - 1U, prefix.waypoints.size()}) {
    const auto& point = curve.waypoints[index];
    connector.times.push_back(point.time_from_start_s);
    connector.positions.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        point.positions_rad.data(), point.positions_rad.size()));
    connector.velocities.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        point.velocities_rad_s.data(), point.velocities_rad_s.size()));
    connector.accelerations.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        point.accelerations_rad_s2.data(), point.accelerations_rad_s2.size()));
  }
  const auto model_v = fixture.scene->getVelocityLimitVectors("arm");
  const auto model_a = fixture.scene->getAccelerationLimitVectors("arm");
  ASSERT_TRUE(model_v && model_a);
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  const auto measured = detail::measure_cartesian_joint_dynamics_envelope(
      contract, connector, values(model_v->second), values(model_a->second),
      {});
  ASSERT_TRUE(measured.ok());
  ASSERT_TRUE(measured.value().complete);
  for (std::size_t index = 0; index < measured.value().joint_derivatives.size();
       ++index) {
    const auto &derivative = measured.value().joint_derivatives[index];
    EXPECT_LE(derivative.acceleration,
              0.8 * model_a->second[static_cast<Eigen::Index>(index)] *
                      input.speed_profile.acceleration_scale +
                  1e-9);
    EXPECT_LE(derivative.jerk, 8.0 + 1e-6);
  }
  EXPECT_FALSE(curve.runtime_scale_bounds->nominal_time_multiplier);
  EXPECT_FALSE(curve.cartesian_fidelity_metrics);
  EXPECT_TRUE(curve.cartesian_waypoint_times_s.empty());
  EXPECT_EQ(input.start_state.velocities->size(),
            fixture.binding.resources.size());
  EXPECT_FALSE(
      input.moving_replacement->source_prefix_trajectory.runtime_scale_bounds);
}

TEST(RoboPlanCandidateComputerTest,
     StateInsideTheCollisionClearanceIsRefusedAsClearance) {
  // A 10 m margin on every scene pair puts the collision-free start inside
  // the clearance: the refusal names the pair and classifies it as
  // clearance, with its positive distance (not "in collision").
  auto fixture = make_fixture();
  const auto pairs = fixture.scene->getCollisionModel().collisionPairs.size();
  ASSERT_GT(pairs, 0U);
  ASSERT_TRUE(fixture.scene
                  ->setCollisionPairMargins(std::vector<double>(pairs, 10.0))
                  .has_value());
  const auto collision_results = [&] {
    const auto& model = fixture.scene->getCollisionModel();
    const auto& data = fixture.scene->getCollisionData();
    std::ostringstream out;
    for (std::size_t index = 0; index < model.collisionPairs.size() &&
                                index < data.collisionResults.size(); ++index) {
      if (!data.collisionResults[index].isCollision()) continue;
      const auto& pair = model.collisionPairs[index];
      out << " [pair=" << index << ", bodies="
          << model.geometryObjects[pair.first].name << "/"
          << model.geometryObjects[pair.second].name;
      if (index < data.distanceResults.size())
        out << ", distance=" << data.distanceResults[index].min_distance;
      if (index < data.collisionRequests.size())
        out << ", margin=" << data.collisionRequests[index].security_margin;
      out << "]";
    }
    return out.str();
  };
  const auto prior_collision_results = collision_results();
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(request(fixture, joint_goal(fixture)));
  ASSERT_FALSE(result.ok());
  for (const auto* key : {"reason", "classification", "collision_signed_distance_m",
                          "collision_clearance_m"}) {
    ASSERT_NE(result.error().details.find(key), result.error().details.end())
        << "missing " << key << "; " << result.error().message << "; details: "
        << ::testing::PrintToString(result.error().details)
        << "; collision results before compute:" << prior_collision_results
        << "; collision results after compute:" << collision_results();
  }
  EXPECT_EQ(result.error().details.at("reason"), "native_start_state_collision");
  EXPECT_EQ(result.error().details.at("classification"), "clearance");
  EXPECT_GT(std::stod(result.error().details.at("collision_signed_distance_m")), 0.0);
  EXPECT_DOUBLE_EQ(std::stod(result.error().details.at("collision_clearance_m")), 10.0);
  EXPECT_NE(result.error().message.find("closer than the collision clearance"),
            std::string::npos);
}

TEST(RoboPlanCandidateComputerTest, MeasuresSceneDistanceAndInactivePairBehavior) {
  auto fixture = make_fixture();
  auto& scene = *fixture.scene;
  const auto full_start = scene.toFullJointPositions("arm", fixture.start);
  const Eigen::Vector4d color(0.2, 0.4, 0.8, 1.0);
  auto added = scene.addSphereGeometry("measure_link_sphere", "forearm_link",
                                       roboplan::Sphere(0.1),
                                       Eigen::Matrix4d::Identity(), color);
  ASSERT_TRUE(added.has_value()) << added.error();
  Eigen::Matrix4d world_pose = Eigen::Matrix4d::Identity();
  world_pose(0, 3) = 100.0;
  added = scene.addSphereGeometry("measure_world_sphere", "universe",
                                  roboplan::Sphere(0.1), world_pose, color);
  ASSERT_TRUE(added.has_value()) << added.error();
  const auto link_ids = scene.getCollisionGeometryIds("measure_link_sphere");
  const auto world_ids = scene.getCollisionGeometryIds("measure_world_sphere");
  ASSERT_TRUE(link_ids.has_value());
  ASSERT_TRUE(world_ids.has_value());
  ASSERT_EQ(link_ids->size(), 1U);
  ASSERT_EQ(world_ids->size(), 1U);
  const auto& model = scene.getCollisionModel();
  const auto pair_it = std::find_if(model.collisionPairs.begin(), model.collisionPairs.end(),
      [&](const auto& pair) {
        return (pair.first == link_ids->front() && pair.second == world_ids->front()) ||
               (pair.second == link_ids->front() && pair.first == world_ids->front());
      });
  ASSERT_NE(pair_it, model.collisionPairs.end());
  const auto pair_index = static_cast<std::size_t>(pair_it - model.collisionPairs.begin());
  scene.computeCollisionDistances(full_start);
  const auto center = scene.getCollisionData().oMg[link_ids->front()].translation().eval();
  auto place = [&](double offset) {
    world_pose.block<3, 1>(0, 3) = center + Eigen::Vector3d(offset, 0, 0);
    return scene.updateGeometryPlacement("measure_world_sphere", "universe", world_pose);
  };
  ASSERT_TRUE(place(0.25).has_value());
  std::vector<double> margins(model.collisionPairs.size(), 0.0);
  margins[pair_index] = 0.1;
  ASSERT_TRUE(scene.setCollisionPairMargins(margins).has_value());
  scene.computeCollisionDistances(full_start);
  const double separated = scene.getCollisionData().distanceResults[pair_index].min_distance;
  EXPECT_NEAR(separated, 0.05, 1e-6);
  EXPECT_GT(separated, 0.0);
  EXPECT_LT(separated, margins[pair_index]);

  ASSERT_TRUE(place(0.2).has_value());
  scene.computeCollisionDistances(full_start);
  const double touching = scene.getCollisionData().distanceResults[pair_index].min_distance;
  EXPECT_NEAR(touching, 0.0, 1e-6);
  ASSERT_TRUE(place(0.15).has_value());
  scene.computeCollisionDistances(full_start);
  const double penetrating = scene.getCollisionData().distanceResults[pair_index].min_distance;
  EXPECT_LE(penetrating, 0.0);
  RecordProperty("separated_distance_m", std::to_string(separated));
  RecordProperty("touching_distance_m", std::to_string(touching));
  RecordProperty("penetrating_distance_m", std::to_string(penetrating));

  // Use the Scene's own scratch: the production diagnostic reads this same
  // distanceResults array. Toggle only this pair, then restore it afterwards.
  auto& data = const_cast<pinocchio::GeometryData&>(scene.getCollisionData());
  ASSERT_TRUE(place(0.25).has_value());
  scene.computeCollisionDistances(full_start);
  const double prior = data.distanceResults[pair_index].min_distance;
  ASSERT_NEAR(prior, 0.05, 1e-6);
  ASSERT_TRUE(data.activeCollisionPairs[pair_index]);
  data.activeCollisionPairs[pair_index] = false;
  ASSERT_TRUE(place(0.15).has_value());
  scene.computeCollisionDistances(full_start);
  EXPECT_FALSE(data.activeCollisionPairs[pair_index]);
  EXPECT_DOUBLE_EQ(data.distanceResults[pair_index].min_distance, prior);
  // These are the clearance-loop distance and margin conditions used by the
  // production diagnostic; no active-pair check precedes that loop.
  const auto stale = data.distanceResults[pair_index].min_distance;
  const auto margin = data.collisionRequests[pair_index].security_margin;
  EXPECT_GT(stale, 0.0);
  EXPECT_LT(stale, margin);
  data.activeCollisionPairs[pair_index] = true;
  scene.computeCollisionDistances(full_start);
  ASSERT_LT(data.distanceResults[pair_index].min_distance, 0.0);
  // Keep a measured positive pair as a competing clearance and suppress all
  // other pairs, so the diagnostic must choose between these two.
  std::size_t competitor = model.collisionPairs.size();
  for (std::size_t i = 0; i < model.collisionPairs.size(); ++i) {
    const auto d = data.distanceResults[i].min_distance;
    if (i != pair_index && data.activeCollisionPairs[i] && d > 0.0 && d < 10.0) {
      competitor = i;
      break;
    }
  }
  ASSERT_LT(competitor, model.collisionPairs.size());
  margins.assign(model.collisionPairs.size(), 0.0);
  margins[competitor] = 10.0;
  ASSERT_TRUE(scene.setCollisionPairMargins(margins).has_value());
  for (std::size_t i = 0; i < model.collisionPairs.size(); ++i) {
    data.activeCollisionPairs[i] = (i == pair_index || i == competitor);
    if (!data.activeCollisionPairs[i]) data.distanceResults[i].min_distance = 1000.0;
  }
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto first = computer.compute(request(fixture, joint_goal(fixture)));
  ASSERT_FALSE(first.ok());
  const auto& details = first.error().details;
  EXPECT_EQ(details.at("reason"), "native_start_state_collision");
  EXPECT_EQ(details.at("failure_stage"), "start_state");
  EXPECT_EQ(details.count("classification"), 0U);
  const auto& pair = model.collisionPairs[pair_index];
  EXPECT_EQ(details.at("collision_body_a"), model.geometryObjects[pair.first].name);
  EXPECT_EQ(details.at("collision_body_b"), model.geometryObjects[pair.second].name);
  EXPECT_LT(std::stod(details.at("collision_signed_distance_m")), 0.0);
  ASSERT_TRUE(data.activeCollisionPairs[competitor]);
  ASSERT_GT(data.distanceResults[competitor].min_distance, 0.0);
  ASSERT_LT(data.distanceResults[competitor].min_distance, margins[competitor]);
  const auto stale_contact = data.distanceResults[pair_index].min_distance;
  ASSERT_LT(stale_contact, 0.0);
  data.activeCollisionPairs[pair_index] = false;
  const auto second = computer.compute(request(fixture, joint_goal(fixture)));
  ASSERT_FALSE(second.ok());
  ASSERT_FALSE(data.activeCollisionPairs[pair_index]);
  ASSERT_DOUBLE_EQ(data.distanceResults[pair_index].min_distance, stale_contact);
  for (std::size_t i = 0; i < pair_index; ++i)
    ASSERT_GT(data.distanceResults[i].min_distance, 0.0);
  EXPECT_EQ(second.error().details.at("reason"), "native_start_state_collision");
  EXPECT_EQ(second.error().details.at("failure_stage"), "start_state");
  EXPECT_EQ(second.error().details.at("classification"), "clearance");
  EXPECT_GT(std::stod(second.error().details.at("collision_signed_distance_m")), 0.0);
}

TEST(RoboPlanCandidateComputerTest, CollisionCheckStepMustBePositive) {
  auto fixture = make_fixture();
  EXPECT_THROW(RoboPlanCandidateComputer(fixture.scene, fixture.binding,
                                         {0x1234U, 65'536U, 0.01, 0.0}),
               std::invalid_argument);
  EXPECT_NO_THROW(RoboPlanCandidateComputer(fixture.scene, fixture.binding,
                                            {0x1234U, 65'536U, 0.01, 0.02}));
}

TEST(RoboPlanCandidateComputerTest,
     MovingPoseGoalKeepsItsAuthoredPoseAfterTheInteriorJoin) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  const auto target =
      fixture.scene->forwardKinematics(fixture.goal, "tool0", "base");
  input.motion = PoseGoalDTO{pose(target)};
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &final = result.value().waypoints.back().positions_rad;
  const Eigen::VectorXd positions =
      Eigen::Map<const Eigen::VectorXd>(final.data(), final.size());
  const auto reached = fixture.scene->forwardKinematics(
      fixture.scene->toFullJointPositions("arm", positions), "tool0", "base");
  EXPECT_LE((reached.block<3, 1>(0, 3) - target.block<3, 1>(0, 3)).norm(),
            input.tolerances.translation_tol_m);
  EXPECT_LE(Eigen::AngleAxisd(reached.block<3, 3>(0, 0).transpose() *
                              target.block<3, 3>(0, 0))
                .angle(),
            input.tolerances.rotation_tol_rad);
  EXPECT_FALSE(result.value().cartesian_fidelity_metrics);
}

TEST(RoboPlanCandidateComputerTest,
     MovingPreparationAllowsAnActuallyRestingSource) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  input.start_state.velocities.reset();
  input.moving_replacement->suffix_start_state = input.start_state;
  for (auto &point :
       input.moving_replacement->source_prefix_trajectory.waypoints) {
    point.positions_rad = values(fixture.start);
    point.velocities_rad_s.assign(fixture.binding.resources.size(), 0.0);
    point.accelerations_rad_s2.assign(fixture.binding.resources.size(), 0.0);
  }
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().waypoints.back().positions_rad,
            values(fixture.goal));
}

TEST(RoboPlanCandidateComputerTest,
     ZeroVelocityWithNonzeroAccelerationCannotSelectARestSuffixEntry) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  input.start_state.velocities = std::vector<CoordinateVelocityDTO>(
      fixture.binding.resources.size(), AngularVelocityDTO{0.0});
  input.moving_replacement->suffix_start_state = input.start_state;
  input.motion = ResolvedJointGoalDTO{fixture.binding.resources,
                                      fixture.binding.compiled_joint_names,
                                      values(fixture.start)};
  for (auto &point :
       input.moving_replacement->source_prefix_trajectory.waypoints) {
    point.positions_rad = values(fixture.start);
    point.velocities_rad_s.assign(fixture.binding.resources.size(), 0.0);
  }
  ASSERT_TRUE(std::any_of(
      input.moving_replacement->source_prefix_trajectory.waypoints.back()
          .accelerations_rad_s2.begin(),
      input.moving_replacement->source_prefix_trajectory.waypoints.back()
          .accelerations_rad_s2.end(),
      [](double value) { return value != 0.0; }));
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(input);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "native_moving_connector_unqualified");
}

TEST(RoboPlanCandidateComputerTest,
     MovingSourcePositionOvershootCannotBeHiddenBySuffixCertificates) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  const auto positions = fixture.scene->getPositionLimitVectors("arm", false);
  ASSERT_TRUE(positions);
  input.moving_replacement->source_prefix_trajectory.waypoints[1]
      .positions_rad[0] = positions->second[0] + 1.0;
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(input);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "native_moving_position_bounds_unqualified");
}

TEST(RoboPlanCandidateComputerTest,
     MovingPartialJointGoalRetainsOriginalCapturedUnselectedTargets) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  const auto index = fixture.binding.resources.size() - 1U;
  input.motion =
      ResolvedJointGoalDTO{{fixture.binding.resources[index]},
                           {fixture.binding.compiled_joint_names[index]},
                           {fixture.goal[static_cast<Eigen::Index>(index)]}};
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});

  const auto result = computer.compute(input);

  ASSERT_TRUE(result.ok()) << result.error().message;
  auto expected = values(fixture.start);
  expected[index] = fixture.goal[static_cast<Eigen::Index>(index)];
  EXPECT_EQ(result.value().waypoints.back().positions_rad, expected);
  const auto seed =
      angular_position_projection(input.moving_replacement->suffix_start_state);
  ASSERT_TRUE(seed.ok());
  EXPECT_NE(seed.value(), values(fixture.start));
}

TEST(RoboPlanCandidateComputerTest,
     MovingIncomingLimitViolationCannotBeRepairedByDurationSearch) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  for (std::size_t index = 0; index < fixture.binding.resources.size(); ++index)
    input.dynamics_limits.push_back(
        {fixture.binding.resources[index],
         fixture.binding.compiled_joint_names[index],
         JointDynamicsLimitDTO{0.00001, std::nullopt, std::nullopt}});
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(input);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "native_moving_incoming_limits_violated");
}

TEST(RoboPlanCandidateComputerTest,
     MovingSourceIdentitySeedAndDerivedEvidenceMutationsAreRefused) {
  auto fixture = make_fixture();
  const auto input = moving_request(fixture);
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  auto changed = input;
  changed.moving_replacement->source_binding_epoch = ExecutionBindingEpoch(0U);
  EXPECT_FALSE(computer.compute(changed).ok());
  changed = input;
  changed.moving_replacement->source_prefix_trajectory.runtime_scale_bounds =
      TrajectoryRuntimeScaleBoundsDTO{-4.0, 16.0, 1.0};
  EXPECT_FALSE(computer.compute(changed).ok());
  changed = input;
  changed.moving_replacement->suffix_start_state.positions[0] =
      AngularPositionDTO{0.0};
  EXPECT_FALSE(computer.compute(changed).ok());
  changed = input;
  changed.moving_replacement->source_prefix_trajectory.waypoints[1]
      .accelerations_rad_s2.clear();
  EXPECT_FALSE(computer.compute(changed).ok());
}

TEST(RoboPlanCandidateComputerTest,
     MovingRetainedDynamicsEvidenceComposesWithChangedBoundaryBounds) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  input.start_state.velocities.reset();
  input.moving_replacement->suffix_start_state = input.start_state;
  auto &prefix = input.moving_replacement->source_prefix_trajectory;
  for (auto &point : prefix.waypoints) {
    point.positions_rad = values(fixture.start);
    point.velocities_rad_s.assign(fixture.binding.resources.size(), 0.0);
    point.accelerations_rad_s2.assign(fixture.binding.resources.size(), 0.0);
  }
  // Known stationary incumbent: retain its already-selected conservative box.
  prefix.runtime_scale_bounds =
      TrajectoryRuntimeScaleBoundsDTO{0.05, 0.2, 1.01};
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().runtime_scale_bounds);
  const auto &bounds = *result.value().runtime_scale_bounds;
  EXPECT_GT(bounds.max_rate_per_s, 0.0);
  EXPECT_LE(bounds.max_rate_per_s, prefix.runtime_scale_bounds->max_rate_per_s);
  EXPECT_GT(bounds.max_rate_change_per_s2, 0.0);
  EXPECT_LE(bounds.max_rate_change_per_s2,
            prefix.runtime_scale_bounds->max_rate_change_per_s2);
  EXPECT_GE(bounds.max_stop_path_rate, 1.0);
  EXPECT_LE(bounds.max_stop_path_rate,
            prefix.runtime_scale_bounds->max_stop_path_rate);
  EXPECT_EQ(result.value().waypoints.back().positions_rad,
            values(fixture.goal));
  EXPECT_EQ(prefix.waypoints.front().positions_rad,
            result.value().waypoints.front().positions_rad);
}

TEST(
    RoboPlanCandidateComputerTest,
    MovingCartesianDeclaresTheUnqualifiedBlendConstraintWithoutFabricatingEvidence) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  input.motion = CartesianPathDTO{};
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto result = computer.compute(input);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "native_moving_cartesian_blend_proof_unavailable");
}

TEST(RoboPlanCandidateComputerTest,
     MovingPreparationHonorsCancellationAndItsSingleWholeDeadline) {
  auto fixture = make_fixture();
  const auto input = moving_request(fixture);
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  const auto cancelled = computer.compute(input, {[] { return true; }, {}});
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().details.at("reason"),
            "planning_cancelled_by_interrupt");
  const auto expired = computer.compute(input, {{}, [] { return true; }});
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "planning_timeout");
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
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  // Re-measure the delivered samples, at nominal speed and traversed at the
  // maximum Stop rate. This exercises the complete producer, not just a
  // hand-written DTO.
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
  const auto at_stop_rate =
      detail::measure_tcp_sample_speed_ratio(contract, nullptr, fixture.scene,
                                             native);
  ASSERT_TRUE(at_stop_rate.has_value());
  EXPECT_LE(*at_stop_rate, 1.0 + 1e-9);
  for (auto &velocity : native.velocities)
    velocity /= rate;
  const auto nominal =
      detail::measure_tcp_sample_speed_ratio(contract, nullptr, fixture.scene,
                                             native);
  ASSERT_TRUE(nominal.has_value());
  EXPECT_LE(*nominal, 1.0 + 1e-9);
}

// The TCP bound inside TOPP-RA holds the 0.98 target at the samples and
// slows only where the cap binds, so without jerk it is never slower than
// timing within the joint limits and stretching uniformly to the same target.
TEST(RoboPlanCandidateComputerTest,
     TcpSpeedBoundInsideToppraHoldsTargetAndBeatsUniformStretch) {
  auto fixture = make_fixture();
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", SpeedProfileDTO{1.0, 1.0, 0.25}, {}};
  roboplan::PathParameterizerTOPPRA toppra(fixture.scene, "arm");
  const roboplan::JointPath path{fixture.binding.compiled_joint_names,
                                 {fixture.start, fixture.goal}};
  roboplan::TOPPRAOptions options;
  const auto unbounded = toppra.generate(path, options);
  ASSERT_TRUE(unbounded.has_value()) << unbounded.error();
  const auto unbounded_ratio = detail::measure_tcp_sample_speed_ratio(
      contract, nullptr, fixture.scene, *unbounded);
  ASSERT_TRUE(unbounded_ratio.has_value());
  ASSERT_GT(*unbounded_ratio, 1.0);  // the cap binds

  auto bound = detail::make_tcp_speed_path_velocity_bound(contract, nullptr,
                                                          fixture.scene);
  ASSERT_TRUE(bound.has_value());
  options.max_path_velocity = std::move(*bound);
  const auto bounded = toppra.generate(path, options);
  ASSERT_TRUE(bounded.has_value()) << bounded.error();
  const auto bounded_ratio = detail::measure_tcp_sample_speed_ratio(
      contract, nullptr, fixture.scene, *bounded);
  ASSERT_TRUE(bounded_ratio.has_value());
  EXPECT_LE(*bounded_ratio, 1.0);
  EXPECT_GE(*bounded_ratio, 0.9 * detail::kTcpSpeedTargetFraction);
  const double uniform_stretch =
      unbounded->times.back() * *unbounded_ratio /
      detail::kTcpSpeedTargetFraction;
  // Up to grid error.
  EXPECT_LE(bounded->times.back(), uniform_stretch * (1.0 + 1e-2));
}

// Motion timing fast: TOPP-RA times joint moves within the nominal
// acceleration share (0.8 of the limit), so the live-scaling headroom costs
// no extra uniform stretch: the nominal multiplier stays near one (it was >=
// sqrt(1/0.8) whenever the acceleration limit bound) and the delivered
// samples keep the headroom.
TEST(RoboPlanCandidateComputerTest,
     FastJointMoveKeepsAccelerationHeadroomWithoutAUniformStretch) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputerConfig config{0x1234U, 65'536U, 0.01};
  config.motion_timing = MotionTimingDTO::kFast;
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding, config);
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  input.speed_profile.max_tcp_linear_speed_mps = 100.0;  // never binds
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().runtime_scale_bounds);
  const auto &bounds = *result.value().runtime_scale_bounds;
  ASSERT_TRUE(bounds.nominal_time_multiplier);
  EXPECT_LT(*bounds.nominal_time_multiplier, 1.1);
  const Eigen::VectorXd model_acceleration =
      fixture.scene->getAccelerationLimitVectors("arm")->second.cwiseAbs();
  double ratio = 0.0;
  for (const auto &waypoint : result.value().waypoints)
    for (std::size_t joint = 0; joint < waypoint.accelerations_rad_s2.size();
         ++joint)
      ratio = std::max(
          ratio, std::abs(waypoint.accelerations_rad_s2[joint]) /
                     (input.speed_profile.acceleration_scale *
                      model_acceleration[static_cast<Eigen::Index>(joint)]));
  EXPECT_LE(ratio, 0.8 * 1.02);
  EXPECT_GE(ratio, 0.7);  // the acceleration limit binds
}

[[nodiscard]] roboplan::JointTrajectory native_trajectory(
    const TrajectoryDTO &trajectory) {
  roboplan::JointTrajectory native;
  native.joint_names = trajectory.joint_names;
  for (const auto &waypoint : trajectory.waypoints) {
    native.times.push_back(waypoint.time_from_start_s);
    native.positions.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        waypoint.positions_rad.data(), waypoint.positions_rad.size()));
    native.velocities.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        waypoint.velocities_rad_s.data(), waypoint.velocities_rad_s.size()));
    native.accelerations.emplace_back(Eigen::Map<const Eigen::VectorXd>(
        waypoint.accelerations_rad_s2.data(),
        waypoint.accelerations_rad_s2.size()));
  }
  return native;
}

TEST(RoboPlanCandidateComputerTest, TcpChordFindsMotionBetweenZeroVelocitySamples) {
  auto fixture = make_fixture();
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  const detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  const auto tcp_position = [&](const Eigen::VectorXd &q) {
    return fixture.scene->forwardKinematics(
        fixture.scene->toFullJointPositions("arm", q), "tool0", "base")
        .block<3, 1>(0, 3).eval();
  };
  const auto start = tcp_position(fixture.start);
  Eigen::VectorXd end = fixture.start;
  double displacement = 0.0;
  for (Eigen::Index joint = 0; joint < end.size(); ++joint) {
    auto candidate = fixture.start;
    candidate[joint] += 0.01;
    const double distance = (tcp_position(candidate) - start).norm();
    if (distance > displacement) {
      end = candidate;
      displacement = distance;
    }
  }
  ASSERT_GT(displacement, 1e-6);
  roboplan::JointTrajectory trajectory;
  trajectory.joint_names = fixture.binding.compiled_joint_names;
  trajectory.times = {0.0, 0.001};
  trajectory.positions = {fixture.start, end};
  trajectory.velocities = {Eigen::VectorXd::Zero(end.size()), Eigen::VectorXd::Zero(end.size())};
  trajectory.accelerations = {Eigen::VectorXd::Zero(end.size()), Eigen::VectorXd::Zero(end.size())};
  const auto sampled = detail::measure_tcp_sample_speed_ratio(contract, nullptr, fixture.scene, trajectory);
  const auto chord = detail::measure_tcp_chord_speed_ratio(contract, fixture.scene, trajectory, {});
  ASSERT_TRUE(sampled);
  ASSERT_TRUE(chord.ok()) << chord.error().message;
  EXPECT_NEAR(*sampled, 0.0, 1e-12);
  const double cap = *input.speed_profile.max_tcp_linear_speed_mps * input.speed_profile.velocity_scale;
  EXPECT_NEAR(chord.value(), displacement / (0.001 * cap), 1e-8);
  ASSERT_GT(chord.value(), 1.0);
  auto tcp_as_base = contract;
  tcp_as_base.compiled_base_frame = "tool0";
  const auto relative_to_itself = detail::measure_tcp_chord_speed_ratio(
      tcp_as_base, fixture.scene, trajectory, {});
  ASSERT_TRUE(relative_to_itself.ok()) << relative_to_itself.error().message;
  EXPECT_NEAR(relative_to_itself.value(), 0.0, 1e-9);

  // The validator compares absolute m/s to its admitted cap with 1e-8
  // tolerance; the preparation helper must report that absolute speed even
  // when the request's velocity scale is fractional.
  const double validator_cap = *input.speed_profile.max_tcp_linear_speed_mps;
  for (const double delta : {-2e-8, 0.5e-8, 2e-8}) {
    auto at_threshold = trajectory;
    at_threshold.times = {0.0, displacement / (validator_cap + delta)};
    const auto speed = detail::measure_tcp_chord_speed_mps(
        contract, fixture.scene, at_threshold, {}, 1.0);
    ASSERT_TRUE(speed.ok()) << speed.error().message;
    EXPECT_NEAR(speed.value(), validator_cap + delta, 1e-12);
    EXPECT_EQ(speed.value() > validator_cap + 1e-8, delta > 1e-8);
  }
  auto late = trajectory;
  late.times = {100000000.0, 100000000.25};
  constexpr double multiplier = 1.3;
  const auto rounded = detail::measure_tcp_chord_speed_mps(
      contract, fixture.scene, late, {}, multiplier);
  ASSERT_TRUE(rounded.ok()) << rounded.error().message;
  const volatile double later_time = late.times[1] * multiplier;
  const volatile double earlier_time = late.times[0] * multiplier;
  EXPECT_DOUBLE_EQ(rounded.value(), displacement / (later_time - earlier_time));

  // A single stretch by the stricter peak yields the approved 0.98 target.
  trajectory.times.back() *= std::max(*sampled, chord.value()) / detail::kTcpSpeedTargetFraction;
  const auto stretched = detail::measure_tcp_chord_speed_ratio(contract, fixture.scene, trajectory, {});
  ASSERT_TRUE(stretched.ok()) << stretched.error().message;
  EXPECT_NEAR(stretched.value(), detail::kTcpSpeedTargetFraction, 1e-9);
}

// Peak joint jerk between consecutive samples (finite difference).
[[nodiscard]] double peak_sample_jerk(const TrajectoryDTO &trajectory) {
  double peak = 0.0;
  for (std::size_t k = 1U; k < trajectory.waypoints.size(); ++k) {
    const auto &a = trajectory.waypoints[k - 1U];
    const auto &b = trajectory.waypoints[k];
    const double dt = b.time_from_start_s - a.time_from_start_s;
    for (std::size_t j = 0U; j < a.accelerations_rad_s2.size(); ++j)
      peak = std::max(peak, std::abs(b.accelerations_rad_s2[j] -
                                     a.accelerations_rad_s2[j]) / dt);
  }
  return peak;
}

// The Deployment default is smooth (robot_defaults.tsv).
TEST(RoboPlanCandidateComputerTest, MotionTimingDefaultsToSmooth) {
  EXPECT_EQ(kDefaultMotionTimingName, "smooth");
  EXPECT_EQ(kDefaultMotionTiming, MotionTimingDTO::kSmooth);
  EXPECT_EQ(RoboPlanCandidateComputerConfig{}.motion_timing,
            MotionTimingDTO::kSmooth);
  EXPECT_EQ(parse_motion_timing("fast"), MotionTimingDTO::kFast);
  EXPECT_FALSE(parse_motion_timing("rp3"));
}

// Smooth: TOPP-RA within the full joint limits, then one uniform
// stretch until the TCP is at its 0.98 target at the samples. Undoing the
// stretch gives back a trajectory at the full acceleration limit. Fast
// on the same path is quicker with far larger jerk; the TCP holds in
// both.
TEST(RoboPlanCandidateComputerTest,
     SmoothStretchesUniformlyToTheTcpTargetAndFastHoldsItInsideToppra) {
  auto fixture = make_fixture();
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  input.speed_profile.max_tcp_linear_speed_mps = 0.25;
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  const Eigen::VectorXd model_acceleration =
      fixture.scene->getAccelerationLimitVectors("arm")->second.cwiseAbs();
  std::map<MotionTimingDTO, TrajectoryDTO> timed;
  for (const auto timing : {MotionTimingDTO::kSmooth, MotionTimingDTO::kFast}) {
    SCOPED_TRACE(static_cast<int>(timing));
    RoboPlanCandidateComputerConfig config{0x1234U, 65'536U, 0.01};
    config.motion_timing = timing;
    RoboPlanCandidateComputer computer(fixture.scene, fixture.binding, config);
    const auto result = computer.compute(input);
    ASSERT_TRUE(result.ok()) << result.error().message;
    ASSERT_TRUE(result.value().runtime_scale_bounds);
    const auto tcp = detail::measure_tcp_sample_speed_ratio(
        contract, nullptr, fixture.scene, native_trajectory(result.value()));
    ASSERT_TRUE(tcp.has_value());
    const auto chord = detail::measure_tcp_chord_speed_ratio(
        contract, fixture.scene, native_trajectory(result.value()), {});
    ASSERT_TRUE(chord.ok()) << chord.error().message;
    EXPECT_LE(*tcp, 1.0 + 1e-9);
    EXPECT_LE(chord.value(), 1.0 + 1e-9);
    const double multiplier =
        *result.value().runtime_scale_bounds->nominal_time_multiplier;
    if (timing == MotionTimingDTO::kSmooth) {
      EXPECT_NEAR(std::max(*tcp, chord.value()), detail::kTcpSpeedTargetFraction, 1e-6);
      EXPECT_GT(multiplier, 1.5);  // the TCP target binds the stretch
      double acceleration = 0.0;
      for (const auto &waypoint : result.value().waypoints)
        for (std::size_t j = 0; j < waypoint.accelerations_rad_s2.size(); ++j)
          acceleration = std::max(
              acceleration,
              std::abs(waypoint.accelerations_rad_s2[j]) * multiplier *
                  multiplier /
                  (input.speed_profile.acceleration_scale *
                   model_acceleration[static_cast<Eigen::Index>(j)]));
      EXPECT_GE(acceleration, 0.85);  // the acceleration limit binds
      EXPECT_LE(acceleration, 1.02);
    } else {
      EXPECT_LT(multiplier, 1.1);
    }
    timed.emplace(timing, result.value());
  }
  const auto &smooth = timed.at(MotionTimingDTO::kSmooth);
  const auto &fast = timed.at(MotionTimingDTO::kFast);
  EXPECT_GT(smooth.waypoints.back().time_from_start_s,
            fast.waypoints.back().time_from_start_s);
  EXPECT_LT(peak_sample_jerk(smooth), peak_sample_jerk(fast));
  RecordProperty("smooth_duration_s",
                 std::to_string(smooth.waypoints.back().time_from_start_s));
  RecordProperty("fast_duration_s",
                 std::to_string(fast.waypoints.back().time_from_start_s));
  RecordProperty("smooth_peak_jerk", std::to_string(peak_sample_jerk(smooth)));
  RecordProperty("fast_peak_jerk", std::to_string(peak_sample_jerk(fast)));
}

// Jerk limits stay one more condition of the final stretch in both
// motion timings, on top of the TCP limit.
TEST(RoboPlanCandidateComputerTest, JerkLimitsStackOnBothMotionTimings) {
  auto fixture = make_fixture();
  auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
  input.speed_profile.max_tcp_linear_speed_mps = 0.25;
  for (std::size_t index = 0; index < fixture.binding.resources.size();
       ++index) {
    JointDynamicsLimitDTO limit;
    limit.max_jerk_rad_s3 = 10.0;
    input.dynamics_limits.push_back(
        {fixture.binding.resources[index],
         fixture.binding.compiled_joint_names[index], limit});
  }
  const auto model_v = fixture.scene->getVelocityLimitVectors("arm");
  const auto model_a = fixture.scene->getAccelerationLimitVectors("arm");
  ASSERT_TRUE(model_v && model_a);
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  for (const auto timing : {MotionTimingDTO::kSmooth, MotionTimingDTO::kFast}) {
    SCOPED_TRACE(static_cast<int>(timing));
    RoboPlanCandidateComputerConfig config{0x1234U, 65'536U, 0.01};
    config.motion_timing = timing;
    RoboPlanCandidateComputer computer(fixture.scene, fixture.binding, config);
    const auto result = computer.compute(input);
    ASSERT_TRUE(result.ok()) << result.error().message;
    const auto native = native_trajectory(result.value());
    const auto measured = detail::measure_cartesian_joint_dynamics_envelope(
        contract, native, values(model_v->second), values(model_a->second),
        {});
    ASSERT_TRUE(measured.ok());
    ASSERT_TRUE(measured.value().complete);
    for (const auto &derivative : measured.value().joint_derivatives)
      EXPECT_LE(derivative.jerk, 8.0 + 1e-6);
    const auto tcp = detail::measure_tcp_sample_speed_ratio(
        contract, nullptr, fixture.scene, native);
    ASSERT_TRUE(tcp.has_value());
    EXPECT_LE(*tcp, 1.0 + 1e-9);
  }
}

TEST(RoboPlanCandidateComputerTest,
     TcpSpeedBoundIsUnavailableWithoutAResolvableTcpOrLimit) {
  auto fixture = make_fixture();
  detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "missing_frame", SpeedProfileDTO{1.0, 1.0, 0.25}, {}};
  EXPECT_FALSE(detail::make_tcp_speed_path_velocity_bound(contract, nullptr,
                                                          fixture.scene));
  contract.compiled_tcp_frame = "tool0";
  contract.speed_profile.max_tcp_linear_speed_mps.reset();
  EXPECT_FALSE(detail::make_tcp_speed_path_velocity_bound(contract, nullptr,
                                                          fixture.scene));
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
     SmallPoseGoalsLandOnTargetNotAtTheRequestToleranceEdge) {
  // GUI jog tolerances (5 mm / 0.05 rad) stay the acceptance check; the
  // reached pose must not stop at their edge (or at the seed for sub-mm goals).
  auto fixture = make_fixture();
  // This configuration has free +X travel (see the Cartesian return test).
  fixture.start = fixture.goal;
  for (const double delta_m : {0.01, 0.0005}) {
    RoboPlanCandidateComputer computer(
        fixture.scene, fixture.binding,
        RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
    Eigen::Matrix4d target =
        fixture.scene->forwardKinematics(fixture.start, "tool0", "base");
    target(0, 3) += delta_m;
    auto input =
        request(fixture, ResolvedMotionSpecDTO{PoseGoalDTO{pose(target)}});
    input.tolerances = TolerancesDTO{1.0e-4, 5.0e-3, 5.0e-2};
    const auto result = computer.compute(input);
    ASSERT_TRUE(result.ok()) << result.error().message << " delta " << delta_m;
    const auto &final = result.value().waypoints.back().positions_rad;
    const Eigen::VectorXd positions =
        Eigen::Map<const Eigen::VectorXd>(final.data(), final.size());
    const auto reached = fixture.scene->forwardKinematics(
        fixture.scene->toFullJointPositions("arm", positions), "tool0",
        "base");
    EXPECT_LE((reached.block<3, 1>(0, 3) - target.block<3, 1>(0, 3)).norm(),
              1.0e-4)
        << "delta " << delta_m;
    EXPECT_LE(Eigen::AngleAxisd(reached.block<3, 3>(0, 0).transpose() *
                                target.block<3, 3>(0, 0))
                  .angle(),
              1.0e-3)
        << "delta " << delta_m;
  }
}

TEST(RoboPlanCandidateComputerTest,
     PoseGoalRefinementNeverStartsAfterCancellationOrDeadline) {
  // A Stop or deadline that arrives while the first IK solve runs is seen
  // right after it: the request ends there and refinement never starts.
  auto fixture = make_fixture();
  fixture.start = fixture.goal;
  Eigen::Matrix4d target =
      fixture.scene->forwardKinematics(fixture.start, "tool0", "base");
  target(0, 3) += 0.01;
  auto input =
      request(fixture, ResolvedMotionSpecDTO{PoseGoalDTO{pose(target)}});
  input.tolerances = TolerancesDTO{1.0e-4, 5.0e-3, 5.0e-2};
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  for (const bool cancel : {true, false}) {
    SCOPED_TRACE(cancel ? "cancellation" : "deadline");
    // Each stage polls the control once; trip it from poll `trip` on.
    const auto stopped_at = [&](std::size_t trip) {
      std::size_t polls = 0U;
      const std::function<bool()> tripped = [&polls, trip] {
        return ++polls >= trip;
      };
      const auto result = computer.compute(
          input, cancel ? RoboPlanCandidateControl{tripped, {}}
                        : RoboPlanCandidateControl{{}, tripped});
      if (result.ok()) return std::string("completed");
      EXPECT_EQ(result.error().details.at("reason"),
                cancel ? "planning_cancelled_by_interrupt"
                       : "planning_timeout");
      return result.error().details.at("failure_stage");
    };
    std::size_t before_ik = 0U;
    for (std::size_t trip = 1U; before_ik == 0U && trip <= 8U; ++trip) {
      if (stopped_at(trip) == "before_ik") before_ik = trip;
    }
    ASSERT_NE(before_ik, 0U);
    // The next poll is the first one after the first solve.
    EXPECT_EQ(stopped_at(before_ik + 1U), "before_ik_refinement");
  }
}

TEST(RoboPlanCandidateComputerTest,
     PoseGoalWithNoIkBudgetLeftKeepsTheAcceptedFirstSolve) {
  // The first solve accepts the seed for a 0.5 mm goal within 5 mm. With the
  // whole budget spent by then, refinement is skipped and the accepted
  // solution (the seed) is the result.
  auto fixture = make_fixture();
  fixture.start = fixture.goal;
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  Eigen::Matrix4d target =
      fixture.scene->forwardKinematics(fixture.start, "tool0", "base");
  target(0, 3) += 0.0005;
  auto input =
      request(fixture, ResolvedMotionSpecDTO{PoseGoalDTO{pose(target)}});
  input.tolerances = TolerancesDTO{1.0e-4, 5.0e-3, 5.0e-2};
  input.planning_timeout_s = 1.0e-9;
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().waypoints.back().positions_rad,
            values(fixture.start));
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
  const auto cartesian_result = computer.compute(request(
      fixture, ResolvedMotionSpecDTO{std::move(path)}, &cartesian_start));
  ASSERT_TRUE(cartesian_result.ok()) << cartesian_result.error().message;
  ASSERT_TRUE(cartesian_result.value().cartesian_fidelity_metrics.has_value());
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

// Core supplies the stationary successor's already-clamped PlanRequest timeout;
// smooth compute additionally deducts time spent checking its moving prefix.
// These are the actual solver-option constructors used by compute, not a test
// planner. The Core request assertion and source clamp prove which cap arrives.
TEST(RoboPlanCandidateComputerTest,
     ProductionSolverOptionsPreserveTheBoundedTakeoverPlanningTimeout) {
  for (const double capped_timeout : {0.1, 0.2, 0.5}) {
    SCOPED_TRACE(capped_timeout);
    for (const auto optimize :
         {PlannerOptimizeDTO::kFirst, PlannerOptimizeDTO::kRrtStar,
          PlannerOptimizeDTO::kBestOfN}) {
      SCOPED_TRACE(static_cast<int>(optimize));
      PlannerSettingsDTO planner;
      planner.optimize = optimize;
      const auto options =
          production_rrt_options("arm", capped_timeout, 0U, planner);
      EXPECT_DOUBLE_EQ(options.max_planning_time, capped_timeout);
    }
  }
}

TEST(RoboPlanCandidateComputerTest, EveryPlannerBlockPlansTheJointGoal) {
  // Deployment default (RRT-Connect, first path) and each override mode.
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  std::vector<PlannerOverrideDTO> overrides{PlannerOverrideDTO{}};
  overrides.resize(4U);
  overrides[1].search = PlannerSearchDTO::kRrt;
  overrides[1].optimize = PlannerOptimizeDTO::kFirst;
  overrides[2].optimize = PlannerOptimizeDTO::kRrtStar;
  overrides[2].max_nodes = 500U;
  overrides[3].optimize = PlannerOptimizeDTO::kBestOfN;
  overrides[3].n = 3U;
  overrides[3].collision_check_step_rad = 0.02;
  for (const auto &planner : overrides) {
    SCOPED_TRACE(planner.optimize ? static_cast<int>(*planner.optimize) : -1);
    auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
    input.planner = planner;
    const auto result = computer.compute(input);
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().waypoints.back().positions_rad,
              values(fixture.goal));
  }
}

TEST(RoboPlanCandidateComputerTest, OutOfRangePlannerOverrideIsRefused) {
  auto fixture = make_fixture();
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  std::vector<PlannerOverrideDTO> invalid(4U);
  invalid[0].n = 1U;
  invalid[1].n = kMaxPlannerBestOfN + 1U;
  invalid[2].max_nodes = 10U;
  invalid[3].collision_check_step_rad = 0.0;
  for (const auto &planner : invalid) {
    auto input = request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
    input.planner = planner;
    const auto result = computer.compute(input);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "native_candidate_request_numeric_invalid");
  }
}

TEST(RoboPlanCandidateComputerTest,
     RealMovingAndStationarySuccessorCandidatesRefuseAnExpiredSourceClock) {
  for (const bool moving : {true, false}) {
    SCOPED_TRACE(moving ? "smooth moving replacement"
                        : "stationary spring successor shape");
    auto fixture = make_fixture();
    auto input =
        moving ? moving_request(fixture)
               : request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)});
    // Full B belongs to this successor. A stationary candidate receives its
    // min(P, residual B) from Core; it has no moving-prefix budget field.
    constexpr Duration preparation_budget = 500'000'000;
    if (input.moving_replacement)
      input.moving_replacement->remaining_preparation_budget_s = 0.5;
    RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                       {0x1234U, 65'536U, 0.01});
    for (const double capped_timeout : {0.1, 0.2, 0.5}) {
      SCOPED_TRACE(capped_timeout);
      input.planning_timeout_s = capped_timeout;
      FakeClock source_clock;
      // Exact boundary from the caller's real clock contract, no wall-clock test.
      source_clock.set(preparation_budget);
      std::size_t deadline_checks{};
      const auto deadline_expired = [&] {
        ++deadline_checks;
        return source_clock.now() >= preparation_budget;
      };
      const RoboPlanCandidateControl control{{}, deadline_expired};
      const auto expired = computer.compute(input, control);
      ASSERT_FALSE(expired.ok());
      EXPECT_EQ(expired.error().details.at("reason"), "planning_timeout");
      EXPECT_GT(deadline_checks, 0U);
    }
  }
}

TEST(RoboPlanCandidateComputerTest, MovingPlanningDeadlineIsSuffixOnlyAndDoesNotRestartAtCompute) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  input.moving_replacement->remaining_planning_budget_s = 1.0;
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  std::size_t suffix_checks = 0U;
  const RoboPlanCandidateControl control{
      {}, [] { return false; }, [&] { ++suffix_checks; return true; }};
  const auto expired = computer.compute(input, control);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "planning_timeout");
  EXPECT_GT(suffix_checks, 0U);

  // The same private suffix callback has no authority over ordinary requests.
  const auto before = suffix_checks;
  const auto ordinary = computer.compute(
      request(fixture, ResolvedMotionSpecDTO{joint_goal(fixture)}), control);
  ASSERT_TRUE(ordinary.ok()) << ordinary.error().message;
  EXPECT_EQ(suffix_checks, before);
}

TEST(RoboPlanCandidateComputerTest, MovingPlanningBudgetRejectsInvalidOrAlreadyConsumedCaps) {
  auto fixture = make_fixture();
  auto input = moving_request(fixture);
  RoboPlanCandidateComputer computer(fixture.scene, fixture.binding,
                                     {0x1234U, 65'536U, 0.01});
  for (const double cap : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    input.moving_replacement->remaining_planning_budget_s = cap;
    const auto result = computer.compute(input);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"), "native_moving_source_invalid");
  }
  // A positive cap smaller than one clock tick expires during local setup.
  // The whole preparation budget remains available; this is the suffix cap.
  input.moving_replacement->remaining_planning_budget_s = 1e-300;
  const auto expired = computer.compute(input);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "planning_timeout");
}


// -- Corner blend radius (densification geometry) ---------------------------

[[nodiscard]] Pose3DTO planar_pose(double x, double y, double yaw = 0.0) {
  const Eigen::Quaterniond orientation(
      Eigen::AngleAxisd(yaw, Eigen::Vector3d::UnitZ()));
  return Pose3DTO{{x, y, 0.0},
                  {orientation.x(), orientation.y(), orientation.z(),
                   orientation.w()}};
}

[[nodiscard]] CartesianPathDTO corner_path(std::vector<Pose3DTO> waypoints,
                                           std::vector<double> radii) {
  CartesianPathDTO path;
  path.waypoints = std::move(waypoints);
  path.blend_radii_m = std::move(radii);
  path.max_translation_step_m = 0.005;
  path.max_rotation_step_rad = 0.05;
  return path;
}

[[nodiscard]] bool contains_position(
    const std::vector<Eigen::Matrix4d> &poses, const Eigen::Vector3d &point) {
  return std::any_of(poses.begin(), poses.end(), [&](const auto &pose) {
    return (pose.template block<3, 1>(0, 3) - point).norm() < 1e-9;
  });
}

TEST(CartesianCornerBlendTest, RoundsCornerWithArcTangentToBothSegments) {
  const auto dense = detail::densify_cartesian_path(
      corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                   planar_pose(0.1, 0.1)},
                  {0.0, 0.02, 0.0}),
      RoboPlanCandidateControl{});
  ASSERT_TRUE(dense.ok()) << dense.error().message;
  const auto &poses = dense.value().poses;
  // Tangent points 0.02 m before and after the corner; the corner itself is cut.
  EXPECT_TRUE(contains_position(poses, {0.08, 0.0, 0.0}));
  EXPECT_TRUE(contains_position(poses, {0.1, 0.02, 0.0}));
  EXPECT_FALSE(contains_position(poses, {0.1, 0.0, 0.0}));
  // A 90 degree turn at tangent distance 0.02 m has radius 0.02 m about
  // (0.08, 0.02); every sample between the tangent points lies on it.
  const Eigen::Vector3d center(0.08, 0.02, 0.0);
  std::size_t on_arc = 0U;
  for (const auto &pose : poses) {
    const Eigen::Vector3d position = pose.block<3, 1>(0, 3);
    if (position.x() > 0.08 + 1e-9 && position.y() < 0.02 - 1e-9) {
      EXPECT_NEAR((position - center).norm(), 0.02, 1e-12);
      ++on_arc;
    }
  }
  EXPECT_GE(on_arc, 30U);
  // The waypoint stands for the arc middle.
  const auto &indices = dense.value().source_waypoint_dense_indices;
  ASSERT_EQ(indices.size(), 3U);
  EXPECT_EQ(indices.front(), 0U);
  EXPECT_EQ(indices.back(), poses.size() - 1U);
  const Eigen::Vector3d middle = poses[indices[1]].block<3, 1>(0, 3);
  const double half = 0.02 * std::sqrt(0.5);
  EXPECT_NEAR((middle - Eigen::Vector3d(0.08 + half, 0.02 - half, 0.0)).norm(),
              0.0, 1e-12);
  // No sample-to-sample direction change reaches RoboPlan's corner tolerance.
  for (std::size_t index = 2U; index < poses.size(); ++index) {
    const Eigen::Vector3d before = (poses[index - 1U].block<3, 1>(0, 3) -
                                    poses[index - 2U].block<3, 1>(0, 3))
                                       .normalized();
    const Eigen::Vector3d after = (poses[index].block<3, 1>(0, 3) -
                                   poses[index - 1U].block<3, 1>(0, 3))
                                      .normalized();
    EXPECT_LE(std::acos(std::clamp(before.dot(after), -1.0, 1.0)),
              detail::kCornerArcMaxTurnRad + 1e-9)
        << index;
  }
}

TEST(CartesianCornerBlendTest, ClampsRadiusToHalfOfEachAdjacentSegment) {
  const auto dense = detail::densify_cartesian_path(
      corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                   planar_pose(0.1, 0.04)},
                  {0.0, 1.0, 0.0}),
      RoboPlanCandidateControl{});
  ASSERT_TRUE(dense.ok()) << dense.error().message;
  // Half of the 0.04 m segment: the arc ends where the last segment ends its
  // first half and starts 0.02 m before the corner.
  EXPECT_TRUE(contains_position(dense.value().poses, {0.08, 0.0, 0.0}));
  EXPECT_TRUE(contains_position(dense.value().poses, {0.1, 0.02, 0.0}));
  EXPECT_TRUE(contains_position(dense.value().poses, {0.1, 0.04, 0.0}));
}

// A 10 mm corner is resolved at least every 1 mm (r/20 = 0.5 mm), on the
// arc and in the tolerances RoboPlan's Bounded resolution steps by (half of
// each).
TEST(CartesianCornerBlendTest, TenMillimetreCornerIsResolvedWithinAMillimetre) {
  const auto dense = detail::densify_cartesian_path(
      corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                   planar_pose(0.1, 0.1)},
                  {0.0, 0.01, 0.0}),
      RoboPlanCandidateControl{});
  ASSERT_TRUE(dense.ok()) << dense.error().message;
  EXPECT_NEAR(dense.value().smallest_corner_radius_m, 0.01, 1e-12);
  const auto &poses = dense.value().poses;
  for (std::size_t index = 1U; index < poses.size(); ++index) {
    const Eigen::Vector3d position = poses[index].block<3, 1>(0, 3);
    if (position.x() > 0.09 + 1e-9 && position.y() < 0.01 - 1e-9) {
      EXPECT_LE((position - poses[index - 1U].block<3, 1>(0, 3)).norm(),
                0.001)
          << index;
    }
  }
  const auto resolution =
      detail::cartesian_resolution_tolerances(dense.value(), 0.005, 0.01);
  EXPECT_NEAR(0.5 * resolution.translation_m, 0.0005, 1e-12);
  EXPECT_NEAR(resolution.rotation_rad / 0.01, resolution.translation_m / 0.005,
              1e-12);

  // Without a rounded corner, or with a step already fine enough, the node's
  // own tolerances apply.
  for (const auto &path :
       {corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                     planar_pose(0.1, 0.1)},
                    {0.0, 0.0, 0.0}),
        corner_path({planar_pose(0.0, 0.0), planar_pose(0.2, 0.0),
                     planar_pose(0.2, 0.2)},
                    {0.0, 0.08, 0.0})}) {
    const auto coarse =
        detail::densify_cartesian_path(path, RoboPlanCandidateControl{});
    ASSERT_TRUE(coarse.ok()) << coarse.error().message;
    const auto unchanged =
        detail::cartesian_resolution_tolerances(coarse.value(), 0.005, 0.01);
    EXPECT_DOUBLE_EQ(unchanged.translation_m, 0.005);
    EXPECT_DOUBLE_EQ(unchanged.rotation_rad, 0.01);
  }
}

TEST(CartesianCornerBlendTest, ZeroRadiusAndReversalKeepTheExactCorner) {
  for (const auto &path :
       {corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                     planar_pose(0.1, 0.1)},
                    {0.0, 0.0, 0.0}),
        corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                     planar_pose(0.1, 0.1)},
                    {}),
        corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                     planar_pose(0.0, 0.0)},
                    {0.0, 0.02, 0.0})}) {
    const auto dense =
        detail::densify_cartesian_path(path, RoboPlanCandidateControl{});
    ASSERT_TRUE(dense.ok()) << dense.error().message;
    const auto &corner =
        dense.value().poses[dense.value().source_waypoint_dense_indices[1]];
    EXPECT_NEAR((corner.block<3, 1>(0, 3) - Eigen::Vector3d(0.1, 0.0, 0.0))
                    .norm(),
                0.0, 1e-12);
    // Straight segments only: 0.1 m / 0.005 m steps each.
    EXPECT_EQ(dense.value().poses.size(), 41U);
  }
}

TEST(CartesianCornerBlendTest, RoundedCornerKeepsOrientationAndItsRateContinuous) {
  const auto dense = detail::densify_cartesian_path(
      corner_path({planar_pose(0.0, 0.0, 0.0), planar_pose(0.1, 0.0, 0.2),
                   planar_pose(0.1, 0.1, 0.4)},
                  {0.0, 0.02, 0.0}),
      RoboPlanCandidateControl{});
  ASSERT_TRUE(dense.ok()) << dense.error().message;
  const auto &poses = dense.value().poses;
  const auto yaw = [](const Eigen::Matrix4d &pose) {
    return std::atan2(pose(1, 0), pose(0, 0));
  };
  EXPECT_NEAR(yaw(poses.front()), 0.0, 1e-12);
  EXPECT_NEAR(yaw(poses.back()), 0.4, 1e-12);
  // Rotation per metre is 2 rad/m on both segments; it may rise inside the
  // arc but changes by well under RoboPlan's 17.5 % between samples.
  double rate_before = 0.0;
  for (std::size_t index = 1U; index < poses.size(); ++index) {
    const double length = (poses[index].block<3, 1>(0, 3) -
                           poses[index - 1U].block<3, 1>(0, 3))
                              .norm();
    const double turn = yaw(poses[index]) - yaw(poses[index - 1U]);
    ASSERT_GT(length, 0.0);
    EXPECT_GT(turn, 0.0) << index;
    const double rate = turn / length;
    if (index > 1U) {
      EXPECT_LT(std::abs(rate - rate_before), 0.10 * rate_before) << index;
    }
    rate_before = rate;
  }
}

TEST(CartesianCornerBlendTest, RejectsBlendRadiiThatDoNotMatchTheWaypoints) {
  for (const auto &radii : {std::vector<double>{0.0, 0.01},
                            std::vector<double>{0.0, -0.01, 0.0},
                            std::vector<double>{
                                0.0, std::numeric_limits<double>::infinity(),
                                0.0}}) {
    const auto dense = detail::densify_cartesian_path(
        corner_path({planar_pose(0.0, 0.0), planar_pose(0.1, 0.0),
                     planar_pose(0.1, 0.1)},
                    radii),
        RoboPlanCandidateControl{});
    ASSERT_FALSE(dense.ok());
    EXPECT_EQ(dense.error().details.at("reason"), "cartesian_request_invalid");
  }
}

// -- Cartesian timing --------------------------------------------------------

TEST(RoboPlanCandidateComputerTest,
     TimeOptimalCartesianTimingFromMoveOrDeploymentDefault) {
  auto fixture = make_fixture();
  // Away from the fixture start's singular configuration.
  fixture.start = fixture.goal;
  const Eigen::Matrix4d start =
      fixture.scene->forwardKinematics(fixture.start, "tool0", "base");
  Eigen::Matrix4d middle = start;
  middle(0, 3) += 0.005;
  Eigen::Matrix4d end = start;
  end(0, 3) += 0.01;
  CartesianPathDTO path;
  path.waypoints = {pose(start), pose(middle), pose(end)};
  path.blend_radii_m = {0.0, 0.0, 0.0};
  path.max_translation_step_m = 0.005;
  path.max_rotation_step_rad = 0.05;

  RoboPlanCandidateComputer constant_speed(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  const auto bounded = constant_speed.compute(request(fixture, path));
  ASSERT_TRUE(bounded.ok()) << bounded.error().message;

  auto time_optimal_path = path;
  time_optimal_path.speed_mode = CartesianSpeedModeDTO::kTimeOptimal;
  const auto from_move =
      constant_speed.compute(request(fixture, time_optimal_path));
  ASSERT_TRUE(from_move.ok()) << from_move.error().message;
  ASSERT_TRUE(from_move.value().cartesian_fidelity_metrics.has_value());
  EXPECT_LE(from_move.value().cartesian_fidelity_metrics
                ->certified_max_translation_error_m,
            request(fixture, path).tolerances.translation_tol_m);
  ASSERT_EQ(from_move.value().cartesian_waypoint_times_s.size(), 3U);

  RoboPlanCandidateComputerConfig deployment_time_optimal{0x4321U, 65'536U,
                                                          0.01};
  deployment_time_optimal.cartesian_timing = CartesianSpeedModeDTO::kTimeOptimal;
  RoboPlanCandidateComputer time_optimal(fixture.scene, fixture.binding,
                                         deployment_time_optimal);
  const auto from_deployment = time_optimal.compute(request(fixture, path));
  ASSERT_TRUE(from_deployment.ok()) << from_deployment.error().message;
  EXPECT_EQ(from_deployment.value().waypoints.size(),
            from_move.value().waypoints.size());
  EXPECT_DOUBLE_EQ(from_deployment.value().waypoints.back().time_from_start_s,
                   from_move.value().waypoints.back().time_from_start_s);
}

// Bounded applies the scales to the joint limits only; the computer passes
// the scaled 0.98 TCP target and the nominal acceleration share, so the
// constant-speed trapezoid needs no uniform stretch for velocity_scale 0.5 or
// the live-scaling headroom (it was >= 2 here: full TCP speed at scale 0.5).
TEST(RoboPlanCandidateComputerTest,
     ConstantSpeedCartesianTimingHoldsScaledTargetWithoutUniformStretch) {
  auto fixture = make_fixture();
  fixture.start = fixture.goal;  // away from the start's singularity
  const Eigen::Matrix4d start =
      fixture.scene->forwardKinematics(fixture.start, "tool0", "base");
  Eigen::Matrix4d end = start;
  end(0, 3) += 0.02;
  CartesianPathDTO path;
  path.waypoints = {pose(start), pose(end)};
  path.blend_radii_m = {0.0, 0.0};
  path.max_translation_step_m = 0.005;
  path.max_rotation_step_rad = 0.05;
  RoboPlanCandidateComputer computer(
      fixture.scene, fixture.binding,
      RoboPlanCandidateComputerConfig{0x4321U, 65'536U, 0.01});
  const auto input = request(fixture, path);  // velocity_scale 0.5, 0.1 m/s
  const auto result = computer.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().runtime_scale_bounds);
  ASSERT_TRUE(result.value().runtime_scale_bounds->nominal_time_multiplier);
  // Only the quintic's acceleration overshoot between samples remains.
  EXPECT_LT(*result.value().runtime_scale_bounds->nominal_time_multiplier,
            1.5);
  const double limit = *input.speed_profile.max_tcp_linear_speed_mps *
                       input.speed_profile.velocity_scale;
  const detail::CartesianTrajectoryContractInput contract{
      "arm", "base", "tool0", input.speed_profile, {}};
  const auto chord = detail::measure_tcp_chord_speed_ratio(
      contract, fixture.scene, native_trajectory(result.value()), {});
  ASSERT_TRUE(chord.ok()) << chord.error().message;
  EXPECT_LE(chord.value(), 1.0 + 1e-9);
  double peak = 0.0;
  for (const auto &waypoint : result.value().waypoints) {
    const Eigen::VectorXd q = Eigen::Map<const Eigen::VectorXd>(
        waypoint.positions_rad.data(), waypoint.positions_rad.size());
    const Eigen::VectorXd v = Eigen::Map<const Eigen::VectorXd>(
        waypoint.velocities_rad_s.data(), waypoint.velocities_rad_s.size());
    const double h = 1e-7;
    const auto at = [&](const Eigen::VectorXd &positions) {
      return fixture.scene
          ->forwardKinematics(
              fixture.scene->toFullJointPositions("arm", positions), "tool0",
              "base")
          .block<3, 1>(0, 3)
          .eval();
    };
    peak = std::max(peak, (at(q + h * v) - at(q)).norm() / h);
  }
  EXPECT_LE(peak, limit * (1.0 + 1e-3));
}

} // namespace
} // namespace manyforge::roboplan_backend
