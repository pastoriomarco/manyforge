#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <future>
#include <functional>
#include <manyforge_collision/scene_asset_root.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <set>
#include <roboplan_example_models/resources.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using namespace manyforge::core;
using namespace std::chrono_literals;

constexpr std::string_view kDigestZero =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kComponentBodyId =
    "component/component-a/model/model-a/body/forearm/collision/0";
constexpr std::string_view kSceneBodyId = "scene/far-box/shape/0";
constexpr std::string_view kToolBodyId =
    "component/component-a/model/model-a/body/tool/collision/0";
constexpr std::string_view kCarriedFrameBodyId =
    "scene/carried-frame-box/shape/0";
constexpr std::string_view kFrameWitnessBodyId =
    "scene/frame-witness-box/shape/0";

struct ProcessFixture final {
  RoboPlanWorkerModelDTO model;
  RoboPlanCandidateBindingDTO binding;
  Eigen::VectorXd start;
  Eigen::VectorXd goal;
  std::shared_ptr<roboplan::Scene> reference_scene;
};

class ScopedPackageRootDescriptor final {
public:
  explicit ScopedPackageRootDescriptor(const std::filesystem::path &root) {
    const int existing_flags = ::fcntl(6, F_GETFD);
    if (existing_flags >= 0) {
      saved_flags_ = existing_flags;
      saved_descriptor_ = ::dup(6);
      if (saved_descriptor_ < 0) {
        throw std::runtime_error("could not save test FD 6");
      }
    }
    const int source =
        ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (source < 0) {
      throw std::runtime_error("could not open test package root");
    }
    if (source != 6) {
      if (::dup2(source, 6) < 0) {
        static_cast<void>(::close(source));
        throw std::runtime_error("could not reserve test FD 6");
      }
      static_cast<void>(::close(source));
    }
    const int flags = ::fcntl(6, F_GETFD);
    if (flags < 0 || ::fcntl(6, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
      throw std::runtime_error("could not make test FD 6 inheritable");
    }
  }

  ~ScopedPackageRootDescriptor() {
    if (saved_descriptor_ >= 0) {
      static_cast<void>(::dup2(saved_descriptor_, 6));
      static_cast<void>(::fcntl(6, F_SETFD, saved_flags_));
      static_cast<void>(::close(saved_descriptor_));
    } else {
      static_cast<void>(::close(6));
    }
  }

  ScopedPackageRootDescriptor(const ScopedPackageRootDescriptor &) = delete;
  ScopedPackageRootDescriptor &
  operator=(const ScopedPackageRootDescriptor &) = delete;

private:
  int saved_descriptor_{-1};
  int saved_flags_{-1};
};

class ScopedDuplicatePackageRootDescriptor final {
public:
  explicit ScopedDuplicatePackageRootDescriptor(const int source_descriptor) {
    descriptor_ = ::fcntl(source_descriptor, F_DUPFD, 9);
    if (descriptor_ < 0) {
      throw std::runtime_error("could not duplicate test package root");
    }
  }

  ~ScopedDuplicatePackageRootDescriptor() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  ScopedDuplicatePackageRootDescriptor(
      const ScopedDuplicatePackageRootDescriptor &) = delete;
  ScopedDuplicatePackageRootDescriptor &
  operator=(const ScopedDuplicatePackageRootDescriptor &) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
  int descriptor_{-1};
};

[[nodiscard]] std::vector<double> values(const Eigen::VectorXd &vector) {
  return {vector.data(), vector.data() + vector.size()};
}

[[nodiscard]] std::vector<RoboPlanJointLimitInput>
joint_limit_rows(const roboplan::Scene &scene) {
  std::vector<RoboPlanJointLimitInput> rows;
  for (const auto &joint_name : scene.getJointNames()) {
    const auto info = scene.getJointInfo(joint_name);
    if (!info.has_value() || info->mimic_info.has_value()) {
      continue;
    }
    RoboPlanJointLimitInput row;
    row.joint_name = joint_name;
    if (info->type == roboplan::JointType::PRISMATIC ||
        info->type == roboplan::JointType::REVOLUTE) {
      row.minimum_position = values(info->limits.min_position);
      row.maximum_position = values(info->limits.max_position);
    }
    row.maximum_velocity = values(info->limits.max_velocity);
    const auto acceleration = values(info->limits.max_acceleration);
    if (std::all_of(acceleration.begin(), acceleration.end(), [](double value) {
          return value != std::numeric_limits<double>::max();
        })) {
      row.maximum_acceleration = acceleration;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

[[nodiscard]] Pose3DTO pose(const Eigen::Matrix4d &transform) {
  const Eigen::Quaterniond orientation(transform.block<3, 3>(0, 0));
  return {{transform(0, 3), transform(1, 3), transform(2, 3)},
          {orientation.x(), orientation.y(), orientation.z(), orientation.w()}};
}

[[nodiscard]] ProcessFixture make_fixture() {
  const auto models = roboplan::example_models::get_package_models_dir();
  const auto package_share = roboplan::example_models::get_package_share_dir();
  const auto urdf_path = models / "ur_robot_model/ur5_gripper.urdf";
  const auto srdf_path = models / "ur_robot_model/ur5_gripper.srdf";
  auto source_scene = std::make_shared<roboplan::Scene>(
      "process-candidate-source",
      roboplan::loadUrdfSceneDescription(urdf_path, {package_share}));
  source_scene->importJointLimitsFromConfig(roboplan::loadJointLimitsConfig(
      models / "ur_robot_model/ur5_config.yaml"));
  if (const auto imported =
          source_scene->importSrdf(roboplan::loadTextFile(srdf_path));
      !imported.has_value()) {
    throw std::runtime_error(imported.error());
  }
  RoboPlanWorkerModelDTO model{"process-candidate-scene", urdf_path,
                               srdf_path, {package_share},
                               joint_limit_rows(*source_scene), {}, {}};
  auto scene = make_roboplan_scene(
      RoboPlanSceneInput{model.native_scene_name,
                         roboplan::loadTextFile(model.urdf_path),
                         roboplan::loadTextFile(model.srdf_path),
                         model.package_paths, model.joint_limits});
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
  for (const auto &joint : group->joint_names) {
    resources.emplace_back("resource/" + joint);
  }
  const ControllableComponentId component("component-a");
  const ModelInstanceId model_instance("model-a");
  const ModelRevision model_revision("model-revision-a");
  return ProcessFixture{std::move(model),
                        RoboPlanCandidateBindingDTO{
                            component,
                            CommandTargetId("arm-target"),
                            model_instance,
                            model_revision,
                            "arm",
                            resources,
                            group->joint_names,
                            {ResolvedComponentModelFrameDTO{
                                 component, model_instance, FrameRoleId("base"),
                                 "base", model_revision},
                             ResolvedComponentModelFrameDTO{
                                 component, model_instance, FrameRoleId("tcp"),
                                 "tool0", model_revision}}},
                        start.value(), goal.value(), std::move(scene)};
}

[[nodiscard]] ControlledStateDTO state(const ProcessFixture &fixture) {
  std::vector<CoordinatePositionDTO> positions;
  for (double value : values(fixture.start)) {
    positions.emplace_back(AngularPositionDTO{value});
  }
  return ControlledStateDTO{
      .controllable_component_id = fixture.binding.controllable_component_id,
      .command_target_id = fixture.binding.command_target_id,
      .control_domain_id = ControlDomainId("domain-a"),
      .control_domain_revision = ControlDomainRevision("domain-revision-a"),
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

[[nodiscard]] RoboPlanCandidateRequestDTO
request(const ProcessFixture &fixture) {
  return RoboPlanCandidateRequestDTO{
      .controllable_component_id = fixture.binding.controllable_component_id,
      .command_target_id = fixture.binding.command_target_id,
      .model_instance_id = fixture.binding.model_instance_id,
      .model_revision = fixture.binding.model_revision,
      .resources = fixture.binding.resources,
      .base_frame = fixture.binding.permitted_frames[0],
      .tcp_frame = fixture.binding.permitted_frames[1],
      .start_state = state(fixture),
      .motion = ResolvedJointGoalDTO{fixture.binding.resources,
                                     fixture.binding.compiled_joint_names,
                                     values(fixture.goal)},
      .tolerances = TolerancesDTO{1.0e-4, 5.0e-3, 1.0e-2},
      .speed_profile = SpeedProfileDTO{0.5, 0.5, std::nullopt},
      .planner = {},
      .dynamics_limits = {},
      .planning_timeout_s = 3.0,
      .planning_attempt_index = 0U,
      .action_span = std::nullopt};
}

[[nodiscard]] ProcessContainedCandidateClientConfig
config(const std::filesystem::path &worker) {
  return ProcessContainedCandidateClientConfig{
      worker, 10s, 250ms, 500ms, 4U * 1024U * 1024U,
      256U * 1024U * 1024U, false, std::nullopt, std::nullopt, {}};
}

[[nodiscard]] RoboPlanCandidateRequestDTO
moving_request(const ProcessFixture &fixture, double remaining_budget_s) {
  auto input = request(fixture);
  const auto width = fixture.binding.resources.size();
  input.moving_replacement = MovingReplacementInputDTO{
      ExecutionGoalKey{ControllerGoalId("source-goal"),
                       ControllerGoalGeneration(2U)},
      ExecutionBindingEpoch(1U),
      1.0,
      TrajectoryDTO{
          fixture.binding.resources,
          fixture.binding.compiled_joint_names,
          {{0.0, values(fixture.start), std::vector<double>(width, 0.1),
            std::vector<double>(width, 0.0)},
           {0.2, values(fixture.start), std::vector<double>(width, 0.1),
            std::vector<double>(width, 0.0)}}},
      1.2,
      state(fixture),
      fixture.binding.permitted_frames[0],
      fixture.binding.permitted_frames[1],
      input.speed_profile,
      input.dynamics_limits,
      remaining_budget_s};
  return input;
}

[[nodiscard]] Pose3DTO identity_pose(double x_m = 0.0) {
  return {{x_m, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
}

[[nodiscard]] CollisionGeometryDescriptor
box_geometry(const CollisionBodyId &body_id, CollisionOwnerKind owner_kind) {
  CollisionGeometryDescriptor geometry;
  geometry.body_id = body_id;
  geometry.owner_kind = owner_kind;
  geometry.geometry_kind = CollisionGeometryKind::kBox;
  geometry.local_pose = identity_pose();
  geometry.box_dimensions_m = Vector3DTO{0.1, 0.1, 0.1};
  return geometry;
}

[[nodiscard]] CollisionWorldBody frame_relative_tool_body() {
  CollisionWorldBody body;
  body.body_id = CollisionBodyId(std::string(kToolBodyId));
  body.owner_kind = CollisionOwnerKind::kComponentModelBody;
  body.frame_id = "tool0";
  body.pose = identity_pose();
  body.geometries.push_back(box_geometry(body.body_id, body.owner_kind));
  return body;
}

[[nodiscard]] RoboPlanCandidateProjectionDTO
frame_relative_collision_projection(const ProcessFixture &fixture) {
  constexpr double kWitnessOffsetM = 0.2;
  auto carried = CollisionWorldBody{};
  carried.body_id = CollisionBodyId(std::string(kCarriedFrameBodyId));
  carried.owner_kind = CollisionOwnerKind::kSceneResource;
  carried.parent_body_id = CollisionBodyId(std::string(kToolBodyId));
  carried.frame_id = "tool0";
  carried.pose = {{0.0, 0.0, kWitnessOffsetM}, {0.0, 0.0, 0.0, 1.0}};
  carried.geometries.push_back(
      box_geometry(carried.body_id, carried.owner_kind));

  Eigen::Matrix4d tool_from_carried = Eigen::Matrix4d::Identity();
  tool_from_carried(2, 3) = kWitnessOffsetM;
  const auto base_from_carried =
      fixture.reference_scene->forwardKinematics(fixture.start, "tool0", "base") *
      tool_from_carried;
  auto witness = CollisionWorldBody{};
  witness.body_id = CollisionBodyId(std::string(kFrameWitnessBodyId));
  witness.owner_kind = CollisionOwnerKind::kSceneResource;
  witness.frame_id = "base";
  witness.pose = pose(base_from_carried);
  witness.geometries.push_back(
      box_geometry(witness.body_id, witness.owner_kind));

  auto policy = std::make_shared<CollisionPolicySnapshot>();
  policy->policy_id = "process-client-frame-projection";
  policy->revision = CollisionPolicyRevision{1U, std::string(kDigestZero)};
  auto world = CollisionWorldSnapshotBuilder()
                   .set_publisher_id("process-client-frame-test")
                   .set_revision_id(1U)
                   .set_policy_overlay_content_hash(policy->revision.content_hash)
                   .add_body(frame_relative_tool_body())
                   .add_body(std::move(carried))
                   .add_body(std::move(witness))
                   .build_shared();
  if (!world.ok()) {
    throw std::runtime_error(world.error().message);
  }
  return RoboPlanCandidateProjectionDTO{
      ProjectionLeaseBindingDTO{
          ProjectionRevisionDTO{
              RetainedBundleId("retained-bundle-a"),
              GraphRevision("graph-revision-a"),
              SceneRevision{SceneId(std::string(kDigestZero)), 1U,
                            ContentDigest(std::string(kDigestZero))}},
          ProjectionLeaseId("process-client-frame-lease")},
      std::move(world.value()), std::move(policy)};
}

[[nodiscard]] CollisionWorldBody component_body() {
  CollisionWorldBody body;
  body.body_id = CollisionBodyId(std::string(kComponentBodyId));
  body.owner_kind = CollisionOwnerKind::kComponentModelBody;
  body.frame_id = "forearm_link";
  body.pose = identity_pose();
  body.geometries.push_back(box_geometry(body.body_id, body.owner_kind));
  return body;
}

[[nodiscard]] CollisionWorldBody far_scene_body(
    const bool carried_by_component = false,
    const std::optional<std::string> &mesh_resource_uri = std::nullopt,
    const double x_m = 100.0) {
  CollisionWorldBody body;
  body.body_id = CollisionBodyId(std::string(kSceneBodyId));
  body.owner_kind = CollisionOwnerKind::kSceneResource;
  body.frame_id = carried_by_component ? "forearm_link" : "base";
  if (carried_by_component) {
    body.parent_body_id = CollisionBodyId(std::string(kComponentBodyId));
  }
  body.pose = identity_pose(x_m);
  if (mesh_resource_uri.has_value()) {
    CollisionGeometryDescriptor geometry;
    geometry.body_id = body.body_id;
    geometry.owner_kind = body.owner_kind;
    geometry.geometry_kind = CollisionGeometryKind::kTriangleMesh;
    geometry.local_pose = identity_pose();
    geometry.source_uri = *mesh_resource_uri;
    geometry.mesh_scale = Vector3DTO{1.0, 1.0, 1.0};
    body.geometries.push_back(std::move(geometry));
  } else {
    body.geometries.push_back(box_geometry(body.body_id, body.owner_kind));
  }
  return body;
}

[[nodiscard]] CollisionPairId projected_allowed_pair() {
  return {CollisionBodyId(std::string(kComponentBodyId)),
          CollisionBodyId(std::string(kSceneBodyId))};
}

[[nodiscard]] RoboPlanCandidateProjectionDTO
projection(std::uint64_t world_revision = 1U,
           std::uint64_t scene_revision = 1U,
           const bool carried_by_component = false,
           const std::optional<std::string> &mesh_resource_uri =
               std::nullopt,
           const double scene_body_x_m = 100.0) {
  auto policy = std::make_shared<CollisionPolicySnapshot>();
  policy->policy_id = "process-client-projection";
  policy->revision = CollisionPolicyRevision{1U, std::string(kDigestZero)};
  policy->allowed_touch_link_pairs.push_back(projected_allowed_pair());
  auto world =
      CollisionWorldSnapshotBuilder()
          .set_publisher_id("process-client-test")
          .set_revision_id(world_revision)
          .set_policy_overlay_content_hash(policy->revision.content_hash)
          .add_body(component_body())
          .add_body(
              far_scene_body(carried_by_component, mesh_resource_uri,
                             scene_body_x_m))
          .build_shared();
  if (!world.ok()) {
    throw std::runtime_error(world.error().message);
  }
  return RoboPlanCandidateProjectionDTO{
      ProjectionLeaseBindingDTO{
          ProjectionRevisionDTO{
              RetainedBundleId("retained-bundle-a"),
              GraphRevision("graph-revision-a"),
              SceneRevision{SceneId(std::string(kDigestZero)), scene_revision,
                            ContentDigest(std::string(scene_revision == 1U
                                                          ? kDigestZero
                                                          : kDigestOne))}},
          ProjectionLeaseId("process-client-lease-" +
                            std::to_string(scene_revision))},
      std::move(world.value()), std::move(policy)};
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
  EXPECT_EQ(planned.value().joint_names, fixture.binding.compiled_joint_names);
  EXPECT_EQ(planned.value().waypoints.front().positions_rad,
            values(fixture.start));
  EXPECT_EQ(planned.value().waypoints.back().positions_rad,
            values(fixture.goal));

  const auto goal_pose = pose(fixture.reference_scene->forwardKinematics(
      fixture.goal, "tool0", "base"));
  const auto ik =
      client.solve_ik(goal_pose, fixture.binding.permitted_frames[0],
                      fixture.binding.permitted_frames[1], state(fixture), 3.0);
  ASSERT_TRUE(ik.ok()) << ik.error().message;
  EXPECT_EQ(ik.value().size(), fixture.binding.resources.size());
}

// Planner settings are cell context: a retained planner takes new ones at
// Apply, and the next request plans in a worker started with them.
TEST(ProcessContainedCandidateClientTest,
     RetainedSettingsReplacementStartsTheNextWorkerWithThem) {
  auto fixture = make_fixture();
  const auto generation_counter =
      std::filesystem::temp_directory_path() /
      ("manyforge-worker-settings-generations-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove(generation_counter, ignored);
  fixture.model.native_scene_name =
      "count-generations:" + generation_counter.string();
  auto process_config = config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH);
  process_config.require_retained_projection = true;
  const RoboPlanCandidateComputerConfig first_settings{0x1234U, 65'536U, 0.01};
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding, first_settings, std::move(process_config));
  ASSERT_TRUE(client.select_retained_projection(projection()).ok());
  const auto generations = [&] {
    std::ifstream input(generation_counter);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  ASSERT_TRUE(client.compute(request(fixture)).ok());
  EXPECT_EQ(generations(), "g");

  // The same settings keep the running worker.
  ASSERT_TRUE(client.replace_retained_settings(first_settings).ok());
  ASSERT_TRUE(client.compute(request(fixture)).ok());
  EXPECT_EQ(generations(), "g");

  // A new seed: the next request plans in a worker started with it.
  auto seeded = first_settings;
  seeded.rng_seed = 0x1235U;
  ASSERT_TRUE(client.replace_retained_settings(seeded).ok());
  ASSERT_TRUE(client.compute(request(fixture)).ok());
  EXPECT_EQ(generations(), "gh");

  // A new collision-check step restarts it too, with the same seed.
  auto stepped = seeded;
  stepped.collision_check_step_rad = 0.02;
  ASSERT_TRUE(client.replace_retained_settings(stepped).ok());
  ASSERT_TRUE(client.compute(request(fixture)).ok());
  EXPECT_EQ(generations(), "ghh");
  std::filesystem::remove(generation_counter, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     RetainedProjectionPinsAndReplacesOnlyItsExactWorkerGeneration) {
  auto fixture = make_fixture();
  const auto generation_counter =
      std::filesystem::temp_directory_path() /
      ("manyforge-worker-projection-generations-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove(generation_counter, ignored);
  fixture.model.native_scene_name =
      "count-generations:" + generation_counter.string();
  auto process_config = config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH);
  process_config.require_retained_projection = true;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));

  const auto unselected = client.compute(request(fixture));
  ASSERT_FALSE(unselected.ok());
  EXPECT_EQ(unselected.error().details.at("reason"),
            "native_planner_projection_not_selected");
  EXPECT_FALSE(std::filesystem::exists(generation_counter));

  const auto first_projection = projection();
  const auto first_selection =
      client.select_retained_projection(first_projection);
  ASSERT_TRUE(first_selection.ok()) << first_selection.error().message;
  const auto first = client.compute(request(fixture));
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(std::filesystem::exists(generation_counter));
  EXPECT_EQ(std::filesystem::file_size(generation_counter), 1U);

  const auto repeated_selection =
      client.select_retained_projection(first_projection);
  ASSERT_TRUE(repeated_selection.ok()) << repeated_selection.error().message;
  const auto repeated = client.compute(request(fixture));
  ASSERT_TRUE(repeated.ok()) << repeated.error().message;
  EXPECT_EQ(std::filesystem::file_size(generation_counter), 1U);

  // New scene/world revisions and a contact selector that relaxes no pair
  // project byte-identical worker content: the worker is kept.
  for (auto same_content : {projection(1U, 2U), projection(2U, 3U)}) {
    for (const auto &contact : {std::optional<std::string>{},
                                std::optional<std::string>("absent-policy")}) {
      same_content.contact_policy_id = contact;
      const auto kept = client.select_retained_projection(same_content);
      ASSERT_TRUE(kept.ok()) << kept.error().message;
      const auto planned = client.compute(request(fixture));
      ASSERT_TRUE(planned.ok()) << planned.error().message;
      EXPECT_EQ(planned.value().waypoints.back().positions_rad,
                first.value().waypoints.back().positions_rad);
      EXPECT_EQ(std::filesystem::file_size(generation_counter), 1U);
    }
  }

  // Different worker content: the scene body moved.
  const auto next_projection = projection(1U, 2U, false, std::nullopt, 101.0);
  const auto next_selection =
      client.select_retained_projection(next_projection);
  ASSERT_TRUE(next_selection.ok()) << next_selection.error().message;
  const auto next = client.compute(request(fixture));
  ASSERT_TRUE(next.ok()) << next.error().message;
  EXPECT_EQ(std::filesystem::file_size(generation_counter), 2U);

  auto invalid_projection = projection(2U, 3U);
  auto invalid_policy =
      std::make_shared<CollisionPolicySnapshot>(*invalid_projection.policy);
  invalid_policy->revision =
      CollisionPolicyRevision{2U, std::string(kDigestOne)};
  invalid_policy->allowed_touch_link_pairs = {
      CollisionPairId{CollisionBodyId(std::string(kComponentBodyId)),
                      CollisionBodyId("scene/absent/shape/0")}};
  invalid_projection.policy = std::move(invalid_policy);
  const auto rejected = client.select_retained_projection(invalid_projection);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "native_planner_projection_selection_failed");
  EXPECT_EQ(std::filesystem::file_size(generation_counter), 2U);
  std::filesystem::remove(generation_counter, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     RealWorkerAcceptsProjectedWorldAndResolvedCollisionPolicy) {
  auto fixture = make_fixture();
  auto process_config = config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH);
  process_config.require_retained_projection = true;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));

  const auto selected = client.select_retained_projection(projection());
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  const auto planned = client.compute(request(fixture));
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_EQ(planned.value().waypoints.front().positions_rad,
            values(fixture.start));
  EXPECT_EQ(planned.value().waypoints.back().positions_rad,
            values(fixture.goal));
}

class ScopedDirectoryDescriptor final {
public:
  explicit ScopedDirectoryDescriptor(const std::filesystem::path &root) {
    // Like the host's inherited FD 9, keep the root above the descriptors
    // that worker spawn rewrites (protocol, package root, executable).
    const int opened = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (opened < 0) {
      throw std::runtime_error("could not open test scene-asset root");
    }
    descriptor_ = ::fcntl(opened, F_DUPFD_CLOEXEC, 10);
    static_cast<void>(::close(opened));
    if (descriptor_ < 0) {
      throw std::runtime_error("could not place test scene-asset root");
    }
  }

  ~ScopedDirectoryDescriptor() { static_cast<void>(::close(descriptor_)); }

  ScopedDirectoryDescriptor(const ScopedDirectoryDescriptor &) = delete;
  ScopedDirectoryDescriptor &
  operator=(const ScopedDirectoryDescriptor &) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
  int descriptor_{-1};
};

constexpr std::string_view kSceneMeshUri = "examples/x/meshes/a.obj";
constexpr std::string_view kTriangleObj =
    "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view kChangedTriangleObj =
    "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n";

void write_file(const std::filesystem::path &path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  if (!stream.good()) {
    throw std::runtime_error("could not write test file");
  }
}

/// Emulates the collision model: the first read of each URI below the scene
/// descriptor pins its digest; later queries return the pinned digest.
[[nodiscard]] std::function<Result<std::string>(const std::string &)>
pinned_scene_mesh_digests(const int scene_root_fd) {
  auto pinned = std::make_shared<std::map<std::string, std::string>>();
  return [scene_root_fd, pinned](const std::string &uri) {
    if (const auto found = pinned->find(uri); found != pinned->end()) {
      return Result<std::string>::Ok(found->second);
    }
    std::string error;
    const auto opened =
        collision::OpenedSceneMesh::open(scene_root_fd, uri, error);
    if (!opened.has_value()) {
      return Result<std::string>::Err(ErrorDTO{
          "invalid_request", error, {{"reason", "scene_mesh_unavailable"}}});
    }
    pinned->emplace(uri, opened->content_digest());
    return Result<std::string>::Ok(opened->content_digest());
  };
}

[[nodiscard]] RoboPlanCandidateProjectionDTO
scene_mesh_projection(const std::uint64_t revision, const std::string &uri,
                      const std::vector<Vector3DTO> &scales = {
                          Vector3DTO{1.0, 1.0, 1.0}}) {
  CollisionWorldBody body;
  body.body_id = CollisionBodyId(std::string(kSceneBodyId));
  body.owner_kind = CollisionOwnerKind::kSceneResource;
  body.frame_id = "base";
  body.pose = identity_pose(100.0);
  for (const auto &scale : scales) {
    CollisionGeometryDescriptor geometry;
    geometry.body_id = body.body_id;
    geometry.owner_kind = body.owner_kind;
    geometry.geometry_kind = CollisionGeometryKind::kTriangleMesh;
    geometry.local_pose = identity_pose();
    geometry.source_uri = uri;
    geometry.mesh_scale = scale;
    body.geometries.push_back(std::move(geometry));
  }
  auto policy = std::make_shared<CollisionPolicySnapshot>();
  policy->policy_id = "process-client-scene-mesh-projection";
  policy->revision = CollisionPolicyRevision{1U, std::string(kDigestZero)};
  policy->allowed_touch_link_pairs.push_back(projected_allowed_pair());
  auto world =
      CollisionWorldSnapshotBuilder()
          .set_publisher_id("process-client-scene-mesh-test")
          .set_revision_id(revision)
          .set_policy_overlay_content_hash(policy->revision.content_hash)
          .add_body(component_body())
          .add_body(std::move(body))
          .build_shared();
  if (!world.ok()) {
    throw std::runtime_error(world.error().message);
  }
  return RoboPlanCandidateProjectionDTO{
      ProjectionLeaseBindingDTO{
          ProjectionRevisionDTO{
              RetainedBundleId("retained-bundle-a"),
              GraphRevision("graph-revision-a"),
              SceneRevision{SceneId(std::string(kDigestZero)), revision,
                            ContentDigest(std::string(revision % 2U == 1U
                                                          ? kDigestZero
                                                          : kDigestOne))}},
          ProjectionLeaseId("process-client-scene-mesh-lease-" +
                            std::to_string(revision))},
      std::move(world.value()), std::move(policy)};
}

/// A package root without examples/ and a separate scene root holding the
/// relative scene mesh, below one per-test temporary parent.
struct SeparatedSceneRoots final {
  explicit SeparatedSceneRoots(const std::string &name)
      : parent(std::filesystem::temp_directory_path() /
               ("manyforge-" + name + "-" + std::to_string(::getpid()))),
        package_root(parent / "package"), scene_root(parent / "scene") {
    std::error_code ignored;
    std::filesystem::remove_all(parent, ignored);
    std::filesystem::create_directories(package_root);
    write_file(scene_root / kSceneMeshUri, kTriangleObj);
  }
  ~SeparatedSceneRoots() {
    std::error_code ignored;
    std::filesystem::remove_all(parent, ignored);
  }
  SeparatedSceneRoots(const SeparatedSceneRoots &) = delete;
  SeparatedSceneRoots &operator=(const SeparatedSceneRoots &) = delete;

  std::filesystem::path parent;
  std::filesystem::path package_root;
  std::filesystem::path scene_root;
};

[[nodiscard]] ProcessContainedCandidateClientConfig
scene_mesh_config(const std::filesystem::path &worker,
                  const std::optional<int> scene_root_fd,
                  const std::optional<int> package_root_fd = 6) {
  auto process_config = config(worker);
  process_config.require_retained_projection = true;
  process_config.retained_package_root_fd = package_root_fd;
  process_config.scene_asset_root_fd = scene_root_fd;
  if (scene_root_fd.has_value()) {
    process_config.scene_mesh_content_digest =
        pinned_scene_mesh_digests(*scene_root_fd);
  }
  return process_config;
}

TEST(ProcessContainedCandidateClientTest,
     RetainedWorkerResolvesWorkspaceRelativeSceneMeshUnderSceneAssetDescriptor) {
  auto fixture = make_fixture();
  SeparatedSceneRoots roots("worker-scene-root");
  ASSERT_FALSE(std::filesystem::exists(roots.package_root / "examples"));
  ScopedPackageRootDescriptor package_root(roots.package_root);
  ScopedDirectoryDescriptor scene_root(roots.scene_root);
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      scene_mesh_config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH,
                        scene_root.get()));
  const std::vector<Vector3DTO> scales{Vector3DTO{1.0, 1.0, 1.0},
                                       Vector3DTO{1.0, 1.5, 1.15}};

  const auto selected = client.select_retained_projection(
      scene_mesh_projection(1U, std::string(kSceneMeshUri), scales));
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  const auto planned = client.compute(request(fixture));
  ASSERT_TRUE(planned.ok()) << planned.error().message;

  // Rename the approved root and put a different tree at its old path. The
  // descriptor still names the original directory and its exact bytes.
  std::filesystem::rename(roots.scene_root, roots.parent / "scene-displaced");
  write_file(roots.scene_root / kSceneMeshUri, kChangedTriangleObj);
  const auto reselected = client.select_retained_projection(
      scene_mesh_projection(2U, std::string(kSceneMeshUri), scales));
  ASSERT_TRUE(reselected.ok()) << reselected.error().message;
  const auto replanned = client.compute(request(fixture));
  ASSERT_TRUE(replanned.ok()) << replanned.error().message;
}

TEST(ProcessContainedCandidateClientTest,
     RetainedWorkerRefusesSceneMeshesOutsideTheSceneAssetDescriptor) {
  auto fixture = make_fixture();
  SeparatedSceneRoots roots("worker-scene-refusals");
  ScopedPackageRootDescriptor package_root(roots.package_root);
  ScopedDirectoryDescriptor scene_root(roots.scene_root);
  const auto refused = [](const Result<void> &result,
                          std::string_view message) {
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "native_planner_projection_selection_failed");
    EXPECT_NE(result.error().message.find(message), std::string::npos)
        << result.error().message;
  };

  {
    // No scene descriptor: the existing fail-closed error, even though the
    // package root descriptor is configured.
    ProcessContainedRoboPlanCandidateClient unconfigured(
        fixture.model, fixture.binding,
        RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
        scene_mesh_config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH,
                          std::nullopt));
    refused(unconfigured.select_retained_projection(scene_mesh_projection(
                1U, std::string(kSceneMeshUri))),
            "has no exact staged asset path");
  }

  const auto meshes = roots.scene_root / "examples/x/meshes";
  std::filesystem::create_directory_symlink(meshes.parent_path(),
                                            roots.scene_root / "examples/link");
  std::filesystem::create_symlink(meshes / "a.obj", meshes / "leaf.obj");
  write_file(roots.package_root / "examples/x/meshes/package-only.obj",
             kTriangleObj);
  {
    const std::filesystem::path oversized = meshes / "oversized.obj";
    write_file(oversized, kTriangleObj);
    std::filesystem::resize_file(oversized,
                                 collision::kMaximumSceneMeshBytes + 1U);
  }

  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      scene_mesh_config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH,
                        scene_root.get()));
  refused(client.select_retained_projection(
              scene_mesh_projection(1U, "../scene/" + std::string(kSceneMeshUri))),
          "asset path is invalid");
  refused(client.select_retained_projection(
              scene_mesh_projection(2U, "examples/link/meshes/a.obj")),
          "scene mesh is unavailable");
  refused(client.select_retained_projection(
              scene_mesh_projection(3U, "examples/x/meshes/leaf.obj")),
          "scene mesh is unavailable");
  refused(client.select_retained_projection(
              scene_mesh_projection(4U, "examples/x/meshes/oversized.obj")),
          "byte bound");
  refused(client.select_retained_projection(
              scene_mesh_projection(5U, "examples/x/meshes/package-only.obj")),
          "scene mesh is unavailable");

  // The worker re-walks below its own descriptor: a leaf replaced by a
  // symlink after the digest was pinned is still refused.
  const auto selected = client.select_retained_projection(
      scene_mesh_projection(6U, std::string(kSceneMeshUri)));
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  ASSERT_TRUE(client.compute(request(fixture)).ok());
  std::filesystem::rename(meshes / "a.obj", meshes / "a-original.obj");
  std::filesystem::create_symlink(meshes / "a-original.obj", meshes / "a.obj");
  ASSERT_TRUE(client
                  .select_retained_projection(
                      scene_mesh_projection(7U, std::string(kSceneMeshUri)))
                  .ok());
  const auto symlinked = client.compute(request(fixture));
  ASSERT_FALSE(symlinked.ok());
  EXPECT_EQ(symlinked.error().details.at("reason"),
            "native_planner_worker_protocol_failure");
}

TEST(ProcessContainedCandidateClientTest,
     RetainedWorkerRefusesSceneMeshContentChangedAfterFirstLoad) {
  auto fixture = make_fixture();
  SeparatedSceneRoots roots("worker-scene-content");
  ScopedPackageRootDescriptor package_root(roots.package_root);
  ScopedDirectoryDescriptor scene_root(roots.scene_root);
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      scene_mesh_config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH,
                        scene_root.get()));

  ASSERT_TRUE(client
                  .select_retained_projection(
                      scene_mesh_projection(1U, std::string(kSceneMeshUri)))
                  .ok());
  const auto first = client.compute(request(fixture));
  ASSERT_TRUE(first.ok()) << first.error().message;

  // Same path, different bytes: the worker generation for the next
  // projection hashes what it loads and refuses the pinned-digest mismatch.
  write_file(roots.scene_root / kSceneMeshUri, kChangedTriangleObj);
  ASSERT_TRUE(client
                  .select_retained_projection(
                      scene_mesh_projection(2U, std::string(kSceneMeshUri)))
                  .ok());
  const auto changed = client.compute(request(fixture));
  ASSERT_FALSE(changed.ok());
  EXPECT_EQ(changed.error().details.at("reason"), "scene_mesh_content_changed");

  // A digest source that itself observed changed bytes refuses selection
  // with the same reason.
  auto process_config = scene_mesh_config(
      MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH, scene_root.get());
  process_config.scene_mesh_content_digest = [](const std::string &) {
    return Result<std::string>::Err(ErrorDTO{
        "invalid_request", "scene_mesh_content_changed: bytes differ",
        {{"reason", "scene_mesh_content_changed"}}});
  };
  ProcessContainedRoboPlanCandidateClient refusing(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));
  const auto refused = refusing.select_retained_projection(
      scene_mesh_projection(1U, std::string(kSceneMeshUri)));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "scene_mesh_content_changed");
}

TEST(ProcessContainedCandidateClientTest,
     RetainedWorkerMapsAlternateSourceDescriptorsToReservedWorkerFds) {
  auto fixture = make_fixture();
  SeparatedSceneRoots roots("worker-fd-alt");
  ScopedPackageRootDescriptor package_root(roots.package_root);
  ScopedDuplicatePackageRootDescriptor alternate_package_root(6);
  ScopedDirectoryDescriptor opened_scene_root(roots.scene_root);
  ScopedDuplicatePackageRootDescriptor alternate_scene_root(
      opened_scene_root.get());

  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      scene_mesh_config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH,
                        alternate_scene_root.get(),
                        alternate_package_root.get()));
  ASSERT_TRUE(client
                  .select_retained_projection(
                      scene_mesh_projection(1U, std::string(kSceneMeshUri)))
                  .ok());
  std::filesystem::rename(roots.scene_root, roots.parent / "scene-displaced");
  ASSERT_TRUE(std::filesystem::create_directory(roots.scene_root));
  const auto planned = client.compute(request(fixture));
  ASSERT_TRUE(planned.ok()) << planned.error().message;
}

TEST(ProcessContainedCandidateClientTest,
     RealWorkerAppliesCarriedObjectTouchPolicyWithoutNativeSelfPair) {
  auto fixture = make_fixture();
  auto process_config = config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH);
  process_config.require_retained_projection = true;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));

  const auto selected = client.select_retained_projection(
      projection(1U, 1U, true));
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  const auto planned = client.compute(request(fixture));
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  EXPECT_EQ(planned.value().waypoints.front().positions_rad,
            values(fixture.start));
  EXPECT_EQ(planned.value().waypoints.back().positions_rad,
            values(fixture.goal));
}

TEST(ProcessContainedCandidateClientTest,
     RealWorkerPreservesAProjectedPoseRelativeToANonJointFrame) {
  auto fixture = make_fixture();
  auto process_config = config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH);
  process_config.require_retained_projection = true;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));

  const auto selected =
      client.select_retained_projection(frame_relative_collision_projection(fixture));
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  const auto planned = client.compute(request(fixture));
  ASSERT_FALSE(planned.ok());
  EXPECT_EQ(planned.error().details.at("reason"),
            "native_start_state_collision");
  EXPECT_NE(planned.error().details.at("collision_body_a")
                .find("manyforge_scene_"),
            std::string::npos);
  EXPECT_NE(planned.error().details.at("collision_body_b")
                .find("manyforge_scene_"),
            std::string::npos);
  EXPECT_NE(planned.error().message.find("manyforge_scene_"),
            std::string::npos);
}

// A retained planner keeps its worker client across an Apply whose candidate
// carries another whole-cell planning model. The replaced model, not the
// worker's first one, must define what the next plan collides with.
TEST(ProcessContainedCandidateClientTest,
     ReplacedRetainedModelDefinesTheNextWorkerCollisionWorld) {
  auto fixture = make_fixture();
  const auto goal_tool =
      fixture.reference_scene->forwardKinematics(fixture.goal, "tool0");
  const auto model_root =
      std::filesystem::temp_directory_path() /
      ("manyforge-replaced-model-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(model_root, ignored);
  std::filesystem::create_directories(model_root);
  auto urdf = roboplan::loadTextFile(fixture.model.urdf_path);
  const auto robot_end = urdf.rfind("</robot>");
  ASSERT_NE(robot_end, std::string::npos);
  std::ostringstream obstacle;
  obstacle << "  <link name=\"replacement_obstacle\">\n"
              "    <collision><geometry><sphere radius=\"0.08\"/></geometry>"
              "</collision>\n  </link>\n"
              "  <joint name=\"replacement_obstacle_joint\" type=\"fixed\">\n"
              "    <parent link=\"world\"/>\n"
              "    <child link=\"replacement_obstacle\"/>\n"
              "    <origin rpy=\"0 0 0\" xyz=\""
           << goal_tool(0, 3) << ' ' << goal_tool(1, 3) << ' '
           << goal_tool(2, 3) << "\"/>\n  </joint>\n";
  urdf.insert(robot_end, obstacle.str());
  auto replacement = fixture.model;
  replacement.urdf_path = model_root / "replacement.urdf";
  {
    std::ofstream output(replacement.urdf_path);
    output << urdf;
  }
  const auto replacement_scene = make_roboplan_scene(RoboPlanSceneInput{
      "replacement-reference", urdf,
      roboplan::loadTextFile(replacement.srdf_path), replacement.package_paths,
      replacement.joint_limits});
  ASSERT_FALSE(replacement_scene->hasCollisions(fixture.start));
  ASSERT_TRUE(replacement_scene->hasCollisions(fixture.goal));

  auto process_config = config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH);
  process_config.require_retained_projection = true;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));
  ASSERT_TRUE(client.select_retained_projection(projection()).ok());
  const auto first = client.compute(request(fixture));
  ASSERT_TRUE(first.ok()) << first.error().message;

  const auto replaced = client.replace_retained_model(replacement);
  ASSERT_TRUE(replaced.ok()) << replaced.error().message;
  const auto unselected = client.compute(request(fixture));
  ASSERT_FALSE(unselected.ok());
  EXPECT_EQ(unselected.error().details.at("reason"),
            "native_planner_projection_not_selected");
  ASSERT_TRUE(client.select_retained_projection(projection()).ok());
  const auto blocked = client.compute(request(fixture));
  EXPECT_FALSE(blocked.ok());

  ASSERT_TRUE(client.replace_retained_model(fixture.model).ok());
  ASSERT_TRUE(client.select_retained_projection(projection()).ok());
  const auto restored = client.compute(request(fixture));
  ASSERT_TRUE(restored.ok()) << restored.error().message;
  EXPECT_EQ(restored.value().waypoints.back().positions_rad,
            values(fixture.goal));

  auto projected = replacement;
  projected.geometries.push_back(RoboPlanWorkerGeometryDTO{
      "projected", "base", BoxShapeDTO{{0.1, 0.1, 0.1}}, identity_pose(),
      std::nullopt, std::nullopt});
  const auto rejected = client.replace_retained_model(projected);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "native_planner_model_replacement_invalid");
  std::filesystem::remove_all(model_root, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     FixedModelClientCannotReplaceItsModel) {
  auto fixture = make_fixture();
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));
  const auto rejected = client.replace_retained_model(fixture.model);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "native_planner_model_replacement_unavailable");
}

TEST(ProcessContainedCandidateClientTest,
     CancellationKillsGenerationAndNextRequestRespawns) {
  auto fixture = make_fixture();
  const auto marker =
      std::filesystem::temp_directory_path() /
      ("manyforge-worker-generation-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
  fixture.model.native_scene_name = marker.string();
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));

  auto first = std::async(std::launch::async,
                          [&] { return client.compute(request(fixture)); });
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
  const auto marker =
      std::filesystem::temp_directory_path() /
      ("manyforge-worker-timeout-" + std::to_string(::getpid()));
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
     PreviousProtocolVersionIsRejectedDuringInitialization) {
  auto fixture = make_fixture();
  fixture.model.native_scene_name = "old-protocol";
  try {
    ProcessContainedRoboPlanCandidateClient client(
        fixture.model, fixture.binding,
        RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
        config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));
    FAIL() << "The eager worker initialization accepted the old protocol";
  } catch (const std::runtime_error &failure) {
    EXPECT_STREQ(failure.what(),
                 "worker response has an invalid magic or version");
  }
}

TEST(ProcessContainedCandidateClientTest,
     MovingTransactionUsesWholeBudgetInsteadOfSuffixPlanningCap) {
  auto fixture = make_fixture();
  const auto marker =
      std::filesystem::temp_directory_path() /
      ("manyforge-worker-moving-budget-" + std::to_string(::getpid()));
  // An existing marker selects the fake worker's immediate-response branch.
  {
    std::ofstream existing(marker);
    ASSERT_TRUE(existing.good());
  }
  fixture.model.native_scene_name = marker.string();
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));
  auto input = moving_request(fixture, 3.0);
  input.planning_timeout_s = 0.000001;
  const auto result = client.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().waypoints.back().positions_rad,
            values(fixture.goal));
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     MovingPlanningCapCarriesOneAbsoluteDeadlineThroughTheProcessBoundary) {
  auto fixture = make_fixture();
  fixture.model.native_scene_name = "inspect-moving-planning-budget";
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));
  auto input = moving_request(fixture, 12.0);
  input.moving_replacement->remaining_planning_budget_s = 10.0;
  // The fake peer checks both decremented budgets, their unchanged difference,
  // and the absolute deadline against its own same-host steady clock.
  const auto result = client.compute(input);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().waypoints.back().positions_rad, values(fixture.goal));
}

TEST(ProcessContainedCandidateClientTest,
     MovingPlanningCapConsumedByLocalTransportSetupDoesNotRestartInWorker) {
  auto fixture = make_fixture();
  fixture.model.native_scene_name = "inspect-moving-planning-budget";
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));
  auto input = moving_request(fixture, 3.0);
  input.moving_replacement->remaining_planning_budget_s = 1e-300;
  const auto expired = client.compute(input);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "planning_timeout");
}

TEST(ProcessContainedCandidateClientTest,
     InvalidMovingBudgetsFailBeforeStartingWorker) {
  auto fixture = make_fixture();
  auto process_config = config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH);
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));
  for (const double budget :
       {0.0, -1.0, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::max()}) {
    const auto result = client.compute(moving_request(fixture, budget));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "native_planner_worker_protocol_failure");
  }
}

TEST(ProcessContainedCandidateClientTest,
     MovingWholeBudgetExpiresWithoutTransportGrace) {
  auto fixture = make_fixture();
  const auto marker =
      std::filesystem::temp_directory_path() /
      ("manyforge-worker-moving-timeout-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
  fixture.model.native_scene_name = marker.string();
  auto process_config = config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH);
  process_config.request_transport_grace = 5s;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      std::move(process_config));
  auto input = moving_request(fixture, 0.025);
  input.planning_timeout_s = 3.0;
  auto result =
      std::async(std::launch::async, [&] { return client.compute(input); });
  const auto ready = result.wait_for(2s);
  if (ready != std::future_status::ready) {
    client.request_cancel();
  }
  ASSERT_EQ(ready, std::future_status::ready);
  const auto expired = result.get();
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "planning_timeout");
  const auto &details = expired.error().details;
  EXPECT_FALSE(details.at("moving_worker_phase").empty());
  EXPECT_GT(std::stoll(details.at("moving_worker_request_elapsed_ns")), 0);
  EXPECT_TRUE(details.contains("moving_worker_lock_wait_ns"));
  EXPECT_TRUE(details.contains("moving_worker_pid"));
  std::filesystem::remove(marker, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     MovingPrefixKeepsExistingFourMiBRequestBound) {
  auto fixture = make_fixture();
  const auto marker =
      std::filesystem::temp_directory_path() /
      ("manyforge-worker-moving-oversize-" + std::to_string(::getpid()));
  {
    std::ofstream existing(marker);
    ASSERT_TRUE(existing.good());
  }
  fixture.model.native_scene_name = marker.string();
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01},
      config(MANYFORGE_ROBOPLAN_FAKE_WORKER_PATH));
  auto input = moving_request(fixture, 3.0);
  input.moving_replacement->source_prefix_trajectory.joint_names[0] =
      std::string(4U * 1024U * 1024U, 'x');
  const auto result = client.compute(input);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "native_planner_worker_protocol_failure");
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
}

TEST(ProcessContainedCandidateClientTest,
     UnexpectedWorkerExitIsContainedAndNextRequestRespawns) {
  auto fixture = make_fixture();
  const auto marker = std::filesystem::temp_directory_path() /
                      ("manyforge-worker-crash-" + std::to_string(::getpid()));
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

// One modeled rotating tool: two disjoint 10mm spheres at radii 0.1/0.2m.
// At q=0.5 the selected object is 19mm from the chosen tool sphere, so the
// endpoint penetrates by 1mm. The planner projection alone permits this pair;
// the final coordinator still owns terminal-window and penetration checks.
[[nodiscard]] ProcessFixture
make_contact_fixture(const std::filesystem::path &root) {
  auto fixture = make_fixture();
  const std::string urdf = R"(<robot name="contact_tool">
<link name="base"/><link name="rotor"/>
<joint name="spin" type="revolute"><parent link="base"/><child link="rotor"/>
<axis xyz="0 0 1"/><limit lower="-1" upper="1" effort="1" velocity="1"/></joint>
<link name="finger"><collision><geometry><sphere radius="0.01"/></geometry></collision></link>
<joint name="finger_mount" type="fixed"><parent link="rotor"/><child link="finger"/><origin xyz="0.1 0 0"/></joint>
<link name="palm"><collision><geometry><sphere radius="0.01"/></geometry></collision></link>
<joint name="palm_mount" type="fixed"><parent link="rotor"/><child link="palm"/><origin xyz="0.2 0 0"/></joint>
</robot>)";
  const std::string srdf =
      R"(<robot name="contact_tool"><group name="arm"><joint name="spin"/></group></robot>)";
  write_file(root / "tool.urdf", urdf);
  write_file(root / "tool.srdf", srdf);
  fixture.model = RoboPlanWorkerModelDTO{
      "contact-tool",
      root / "tool.urdf",
      root / "tool.srdf",
      {root},
      {RoboPlanJointLimitInput{"spin",
                               std::vector<double>{-1.0},
                               std::vector<double>{1.0},
                               {1.0},
                               std::vector<double>{2.0}}},
      {},
      {}};
  fixture.binding.resources = {ResourceId("resource/spin")};
  fixture.binding.compiled_joint_names = {"spin"};
  std::get<ResolvedComponentModelFrameDTO>(fixture.binding.permitted_frames[1])
      .compiled_frame_name = "finger";
  fixture.start = Eigen::VectorXd::Constant(1, 0.0);
  fixture.goal = Eigen::VectorXd::Constant(1, 0.5);
  fixture.reference_scene = make_roboplan_scene(RoboPlanSceneInput{
      "contact-reference", urdf, srdf, {root}, fixture.model.joint_limits});
  return fixture;
}

[[nodiscard]] RoboPlanCandidateProjectionDTO
contact_projection(const std::optional<std::string> &active_policy,
                   const std::string &object_id = "probe-object",
                   const bool at_palm = false,
                   const bool include_second_object = false) {
  const auto robot_body = [](const std::string &link) {
    CollisionWorldBody body;
    body.body_id = CollisionBodyId("component/component-a/model/model-a/body/" +
                                   link + "/collision/0");
    body.owner_kind = CollisionOwnerKind::kComponentModelBody;
    body.frame_id = link;
    body.pose = identity_pose();
    auto geometry = box_geometry(body.body_id, body.owner_kind);
    geometry.geometry_kind = CollisionGeometryKind::kSphere;
    geometry.box_dimensions_m.reset();
    geometry.sphere_radius_m = 0.01;
    body.geometries.push_back(std::move(geometry));
    return body;
  };
  const auto object_body = [&](const std::string &id, bool inside = false) {
    CollisionWorldBody body;
    body.body_id = CollisionBodyId("scene/" + id + "/shape/0");
    body.owner_kind = CollisionOwnerKind::kSceneResource;
    body.frame_id = "base";
    const double radius = (at_palm ? 0.2 : 0.1) + (inside ? -0.019 : 0.019);
    body.pose = {{radius * std::cos(0.5), radius * std::sin(0.5), 0.0},
                 {0.0, 0.0, 0.0, 1.0}};
    auto geometry = box_geometry(body.body_id, body.owner_kind);
    geometry.geometry_kind = CollisionGeometryKind::kSphere;
    geometry.box_dimensions_m.reset();
    geometry.sphere_radius_m = 0.01;
    body.geometries.push_back(std::move(geometry));
    return body;
  };
  auto policy = std::make_shared<CollisionPolicySnapshot>();
  policy->policy_id = "operation-contact-projection";
  policy->revision = CollisionPolicyRevision{1U, std::string(kDigestZero)};
  policy->contact_grasps = {{"finger-contact",
                             "probe-object",
                             {robot_body("finger").body_id},
                             0.001,
                             0.8},
                            {"palm-contact",
                             "probe-object",
                             {robot_body("palm").body_id},
                             0.001,
                             0.8}};
  auto builder =
      CollisionWorldSnapshotBuilder()
          .set_publisher_id("operation-contact-test")
          .set_revision_id(1U)
          .set_policy_overlay_content_hash(policy->revision.content_hash);
  builder.add_body(robot_body("finger"));
  builder.add_body(robot_body("palm"));
  builder.add_body(object_body(object_id));
  if (include_second_object)
    builder.add_body(object_body("probe-object-extra", true));
  auto world = builder.build_shared();
  if (!world.ok())
    throw std::runtime_error(world.error().message);
  return {projection().projection_binding, std::move(world.value()),
          std::move(policy), active_policy};
}

TEST(ProcessContainedCandidateClientTest,
     RealWorkerContactSelectionAllowsOneMillimetreGoalAndDoesNotLeak) {
  SeparatedSceneRoots roots("contact-selected");
  auto fixture = make_contact_fixture(roots.package_root);
  auto process_config = config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH);
  process_config.require_retained_projection = true;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01}, process_config);
  auto selected = contact_projection("finger-contact");
  const auto base_from_finger = fixture.reference_scene->forwardKinematics(
      fixture.goal, "finger", "base");
  const Eigen::Vector3d object_center{0.119 * std::cos(0.5),
                                      0.119 * std::sin(0.5), 0.0};
  EXPECT_NEAR(0.02 -
                  (base_from_finger.block<3, 1>(0, 3) - object_center).norm(),
              0.001, 1e-12);
  ASSERT_TRUE(client.select_retained_projection(selected).ok());
  auto result = client.compute(request(fixture));
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().waypoints.back().positions_rad,
            values(fixture.goal));

  // Same world/policy identity: only the operation selector changes. A cached
  // contact allowance must not leak to another policy or an ordinary request.
  for (const auto &id : {std::optional<std::string>("palm-contact"),
                         std::optional<std::string>("absent-policy"),
                         std::optional<std::string>{}}) {
    selected.contact_policy_id = id;
    ASSERT_TRUE(client.select_retained_projection(selected).ok());
    const auto strict = client.compute(request(fixture));
    ASSERT_FALSE(strict.ok()) << "selector=" << id.value_or("<none>");
    EXPECT_EQ(strict.error().details.at("reason"),
              "native_goal_state_collision");
  }
  selected.contact_policy_id = "finger-contact";
  ASSERT_TRUE(client.select_retained_projection(selected).ok());
  result = client.compute(request(fixture));
  ASSERT_TRUE(result.ok()) << result.error().message;
}

/// This process's live child PIDs (the spawned workers).
[[nodiscard]] std::set<pid_t> child_pids() {
  std::set<pid_t> pids;
  for (const auto &task :
       std::filesystem::directory_iterator("/proc/self/task")) {
    std::ifstream children(task.path() / "children");
    pid_t pid = 0;
    while (children >> pid) {
      pids.insert(pid);
    }
  }
  return pids;
}

TEST(ProcessContainedCandidateClientTest,
     RealWorkerContactPolicyChangesKeepTheWorkerAndMatchAFreshWorker) {
  SeparatedSceneRoots roots("contact-reuse");
  auto fixture = make_contact_fixture(roots.package_root);
  auto process_config = config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH);
  process_config.require_retained_projection = true;
  const RoboPlanCandidateComputerConfig candidate_config{0x1234U, 65'536U,
                                                         0.01};
  ProcessContainedRoboPlanCandidateClient reused(
      fixture.model, fixture.binding, candidate_config, process_config);
  std::optional<std::set<pid_t>> workers;
  for (const auto &id : {std::optional<std::string>("finger-contact"),
                         std::optional<std::string>("palm-contact"),
                         std::optional<std::string>{},
                         std::optional<std::string>("finger-contact"),
                         std::optional<std::string>("absent-policy"),
                         std::optional<std::string>("palm-contact")}) {
    SCOPED_TRACE("selector=" + id.value_or("<none>"));
    const auto selected = contact_projection(id);
    ASSERT_TRUE(reused.select_retained_projection(selected).ok());
    const auto result = reused.compute(request(fixture));
    // Every policy change after the first plan keeps the same worker.
    if (!workers) {
      workers = child_pids();
    }
    EXPECT_EQ(child_pids(), *workers);

    ProcessContainedRoboPlanCandidateClient fresh(
        fixture.model, fixture.binding, candidate_config, process_config);
    ASSERT_TRUE(fresh.select_retained_projection(selected).ok());
    const auto expected = fresh.compute(request(fixture));
    ASSERT_EQ(result.ok(), expected.ok());
    EXPECT_EQ(result.ok(), id == std::optional<std::string>("finger-contact"));
    if (result.ok()) {
      EXPECT_EQ(result.value().waypoints.size(),
                expected.value().waypoints.size());
      EXPECT_EQ(result.value().waypoints.back().positions_rad,
                expected.value().waypoints.back().positions_rad);
    } else {
      // Same failure, naming the same colliding pair.
      EXPECT_EQ(result.error().code, expected.error().code);
      EXPECT_EQ(result.error().message, expected.error().message);
      EXPECT_EQ(result.error().details, expected.error().details);
      EXPECT_TRUE(result.error().details.contains("collision_body_a"));
    }
  }
}

TEST(ProcessContainedCandidateClientTest,
     RealWorkerContactSelectionKeepsOtherBodiesAndObjectsStrict) {
  SeparatedSceneRoots roots("contact-unrelated");
  auto fixture = make_contact_fixture(roots.package_root);
  auto process_config = config(MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH);
  process_config.require_retained_projection = true;
  ProcessContainedRoboPlanCandidateClient client(
      fixture.model, fixture.binding,
      RoboPlanCandidateComputerConfig{0x1234U, 65'536U, 0.01}, process_config);
  const auto assert_goal_collision =
      [&](const RoboPlanCandidateProjectionDTO &selected) {
        const auto selection = client.select_retained_projection(selected);
        ASSERT_TRUE(selection.ok()) << selection.error().message;
        const auto strict = client.compute(request(fixture));
        ASSERT_FALSE(strict.ok());
        EXPECT_EQ(strict.error().details.at("reason"),
                  "native_goal_state_collision");
      };
  // Same policy and object, but collision belongs to the unlisted palm.
  assert_goal_collision(
      contact_projection("finger-contact", "probe-object", true));
  // Similar object ID is not the selected runtime object.
  assert_goal_collision(
      contact_projection("finger-contact", "probe-object-extra"));
  // Allowing the selected object's pair does not allow another contacting one.
  assert_goal_collision(
      contact_projection("finger-contact", "probe-object", false, true));
}

} // namespace
} // namespace manyforge::roboplan_backend
