#include <gtest/gtest.h>

#include <manyforge_roboplan/process_contained_candidate_client.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <roboplan/core/scene.hpp>
#include <roboplan_example_models/resources.hpp>
#include <unistd.h>

namespace manyforge::roboplan_backend {
namespace {

using namespace manyforge::core;
using namespace std::chrono_literals;

struct ProcessFixture final {
  RoboPlanWorkerModelDTO model;
  RoboPlanCandidateBindingDTO binding;
  Eigen::VectorXd start;
  Eigen::VectorXd goal;
  std::shared_ptr<roboplan::Scene> reference_scene;
};

[[nodiscard]] std::vector<double> values(const Eigen::VectorXd& vector) {
  return {vector.data(), vector.data() + vector.size()};
}

[[nodiscard]] Pose3DTO pose(const Eigen::Matrix4d& transform) {
  const Eigen::Quaterniond orientation(transform.block<3, 3>(0, 0));
  return {{transform(0, 3), transform(1, 3), transform(2, 3)},
          {orientation.x(), orientation.y(), orientation.z(),
           orientation.w()}};
}

[[nodiscard]] ProcessFixture make_fixture() {
  const auto models = roboplan::example_models::get_package_models_dir();
  const auto package_share =
      roboplan::example_models::get_package_share_dir();
  RoboPlanWorkerModelDTO model{
      "process-candidate-scene",
      models / "ur_robot_model/ur5_gripper.urdf",
      models / "ur_robot_model/ur5_gripper.srdf",
      {package_share},
      models / "ur_robot_model/ur5_config.yaml",
      {},
      {}};
  auto scene = std::make_shared<roboplan::Scene>(
      model.native_scene_name, model.urdf_path, model.srdf_path,
      model.package_paths, *model.yaml_config_path);
  const auto group = scene->getJointGroupInfo("arm");
  if (!group.has_value()) {
    throw std::runtime_error(group.error());
  }
  scene->setRngSeed(0x5a17U);
  const auto start = scene->randomCollisionFreePositions();
  const auto goal = scene->randomCollisionFreePositions();
  if (!start.has_value() || !goal.has_value()) {
    throw std::runtime_error("could not construct process fixture");
  }
  std::vector<ResourceId> resources;
  for (const auto& joint : group->joint_names) {
    resources.emplace_back("resource/" + joint);
  }
  const ControllableComponentId component("component-a");
  const ModelInstanceId model_instance("model-a");
  const ModelRevision model_revision("model-revision-a");
  return ProcessFixture{
      std::move(model),
      RoboPlanCandidateBindingDTO{
          component,
          CommandTargetId("arm-target"),
          model_instance,
          model_revision,
          "arm",
          resources,
          group->joint_names,
          {ResolvedComponentModelFrameDTO{
               component, model_instance, FrameRoleId("base"), "base",
               model_revision},
           ResolvedComponentModelFrameDTO{
               component, model_instance, FrameRoleId("tcp"), "tool0",
               model_revision}}},
      start.value(), goal.value(), std::move(scene)};
}

[[nodiscard]] ControlledStateDTO state(
    const ProcessFixture& fixture) {
  std::vector<CoordinatePositionDTO> positions;
  for (double value : values(fixture.start)) {
    positions.emplace_back(AngularPositionDTO{value});
  }
  return ControlledStateDTO{
      .controllable_component_id =
          fixture.binding.controllable_component_id,
      .command_target_id = fixture.binding.command_target_id,
      .control_domain_id = ControlDomainId("domain-a"),
      .control_domain_revision =
          ControlDomainRevision("domain-revision-a"),
      .control_binding_id = ControlBindingId("control-binding-a"),
      .control_binding_revision =
          ControlBindingRevision("control-binding-revision-a"),
      .controller_topology_id = ControllerTopologyId("topology-a"),
      .topology_revision = TopologyRevision("topology-revision-a"),
      .control_route_id = ControlRouteId("route-a"),
      .active_control_configuration_id =
          ActiveControlConfigurationId("configuration-a"),
      .active_control_configuration_generation =
          ActiveControlConfigurationGeneration(1U),
      .resources = fixture.binding.resources,
      .joint_names = fixture.binding.compiled_joint_names,
      .positions = std::move(positions),
      .velocities = std::nullopt};
}

[[nodiscard]] RoboPlanCandidateRequestDTO request(
    const ProcessFixture& fixture) {
  return RoboPlanCandidateRequestDTO{
      .controllable_component_id =
          fixture.binding.controllable_component_id,
      .command_target_id = fixture.binding.command_target_id,
      .model_instance_id = fixture.binding.model_instance_id,
      .model_revision = fixture.binding.model_revision,
      .resources = fixture.binding.resources,
      .base_frame = fixture.binding.permitted_frames[0],
      .tcp_frame = fixture.binding.permitted_frames[1],
      .start_state = state(fixture),
      .motion = ResolvedJointGoalDTO{
          fixture.binding.resources, fixture.binding.compiled_joint_names,
          values(fixture.goal)},
      .tolerances = TolerancesDTO{1.0e-4, 5.0e-3, 1.0e-2},
      .speed_profile = SpeedProfileDTO{0.5, 0.5, std::nullopt},
      .planning_profile = PlanningProfileDTO::kFirstFeasible,
      .dynamics_limits = {},
      .planning_timeout_s = 3.0,
      .planning_attempt_index = 0U,
      .action_span = std::nullopt};
}

[[nodiscard]] ProcessContainedCandidateClientConfig config(
    const std::filesystem::path& worker) {
  return ProcessContainedCandidateClientConfig{
      worker, 10s, 250ms, 500ms, 4U * 1024U * 1024U,
      256U * 1024U * 1024U};
}

TEST(ProcessContainedCandidateClientTest,
     RealWorkerPlansAndSolvesIkAcrossPrivateProtocol) {
  auto fixture = make_fixture();
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH));

  const auto planned = client.compute(request(fixture));
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_EQ(planned.value().resources, fixture.binding.resources);
  EXPECT_EQ(planned.value().joint_names,
            fixture.binding.compiled_joint_names);
  EXPECT_EQ(planned.value().waypoints.front().positions_rad,
            values(fixture.start));
  EXPECT_EQ(planned.value().waypoints.back().positions_rad,
            values(fixture.goal));

  const auto goal_pose = pose(fixture.reference_scene->forwardKinematics(
      fixture.goal, "tool0", "base"));
  const auto ik = client.solve_ik(
      goal_pose, fixture.binding.permitted_frames[0],
      fixture.binding.permitted_frames[1], state(fixture), 3.0);
  ASSERT_TRUE(ik.ok()) << ik.error().message;
  EXPECT_EQ(ik.value().size(), fixture.binding.resources.size());
}

TEST(ProcessContainedCandidateClientTest,
     CancellationKillsGenerationAndNextRequestRespawns) {
  auto fixture = make_fixture();
  const auto marker = std::filesystem::temp_directory_path() /
                      ("manyforge-worker-generation-" +
                       std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
  fixture.model.native_scene_name = marker.string();
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));

  auto first = std::async(std::launch::async, [&] {
    return client.compute(request(fixture));
  });
  const auto marker_deadline = std::chrono::steady_clock::now() + 2s;
  while (!std::filesystem::exists(marker) &&
         std::chrono::steady_clock::now() < marker_deadline) {
    std::this_thread::sleep_for(5ms);
  }
  if (!std::filesystem::exists(marker)) {
    client.request_cancel();
    static_cast<void>(first.wait_for(2s));
  }
  ASSERT_TRUE(std::filesystem::exists(marker));
  const auto concurrent = client.compute(request(fixture));
  ASSERT_FALSE(concurrent.ok());
  EXPECT_EQ(concurrent.error().details.at("reason"),
            "native_planner_concurrent_request_rejected");
  client.request_cancel();
  ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
  const auto cancelled = first.get();
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().details.at("reason"),
            "planning_cancelled_by_interrupt");

  const auto recovered = client.compute(request(fixture));
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().resources, fixture.binding.resources);
  EXPECT_EQ(recovered.value().waypoints.back().positions_rad,
            values(fixture.goal));
  std::filesystem::remove(marker, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     TimeoutKillsGenerationAndNextRequestRespawns) {
  auto fixture = make_fixture();
  const auto marker = std::filesystem::temp_directory_path() /
                      ("manyforge-worker-timeout-" +
                       std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
  fixture.model.native_scene_name = marker.string();
  auto process_config = config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH);
  process_config.request_transport_grace = 10ms;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));

  auto timed_request = request(fixture);
  timed_request.planning_timeout_s = 0.025;
  const auto timed_out = client.compute(timed_request);
  ASSERT_FALSE(timed_out.ok());
  EXPECT_EQ(timed_out.error().details.at("reason"), "planning_timeout");

  const auto recovered = client.compute(request(fixture));
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().waypoints.back().positions_rad,
            values(fixture.goal));
  std::filesystem::remove(marker, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     UnexpectedWorkerExitIsContainedAndNextRequestRespawns) {
  auto fixture = make_fixture();
  const auto marker = std::filesystem::temp_directory_path() /
                      ("manyforge-worker-crash-" +
                       std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
  fixture.model.native_scene_name = "crash-once:" + marker.string();
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));

  const auto crashed = client.compute(request(fixture));
  ASSERT_FALSE(crashed.ok());
  EXPECT_EQ(crashed.error().details.at("reason"),
            "native_planner_worker_unavailable");

  const auto recovered = client.compute(request(fixture));
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().waypoints.back().positions_rad,
            values(fixture.goal));
  std::filesystem::remove(marker, ignored);
}

}  // namespace
}  // namespace manyforge::roboplan_backend
