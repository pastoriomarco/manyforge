#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <manyforge_core/testing/planner_backend_conformance.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <roboplan/core/scene.hpp>
#include <roboplan/core/types.hpp>
#include <roboplan_cartesian_planning/cartesian_path_planner.hpp>
#include <roboplan_example_models/resources.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>
#include <roboplan_toppra/toppra.hpp>

namespace manyforge::core::testing {
namespace {

constexpr auto kDigestD =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr auto kGroup = "arm";
constexpr auto kBase = "base";
constexpr auto kTip = "tool0";

const std::vector<std::string> kJointNames{
    "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
    "wrist_1_joint",      "wrist_2_joint",       "wrist_3_joint"};

void append_u64_little_endian(std::string& target, std::uint64_t value) {
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    target.push_back(static_cast<char>((value >> (byte * 8U)) & 0xffU));
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("fixture resource is unreadable: " + path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void append_hash_frame(std::string& target, std::string_view label,
                       std::string_view payload) {
  append_u64_little_endian(target, label.size());
  target.append(label);
  append_u64_little_endian(target, payload.size());
  target.append(payload);
}

struct FixtureProjection {
  std::filesystem::path urdf;
  std::filesystem::path srdf;
  std::filesystem::path package_share;
  std::string resource_digest;
};

FixtureProjection make_fixture_projection() {
  const auto models = roboplan::example_models::get_package_models_dir();
  FixtureProjection projection{
      models / "ur_robot_model/ur5_gripper.urdf",
      models / "ur_robot_model/ur5_gripper.srdf",
      roboplan::example_models::get_package_share_dir(), {}};
  constexpr char identity_prefix[] =
      "manyforge.e1.projection-resource-closure.v1\0";
  std::string framed(identity_prefix, sizeof(identity_prefix) - 1U);
  const std::string urdf = read_file(projection.urdf);
  const std::string srdf = read_file(projection.srdf);
  append_hash_frame(framed, "source/urdf", urdf);
  append_hash_frame(framed, "source/srdf", srdf);

  const std::regex package_uri(
      R"(package://([A-Za-z0-9_.-]+)/([^"'<>\s]+))");
  std::set<std::pair<std::string, std::string>> resources;
  for (const auto* payload : {&urdf, &srdf}) {
    for (auto match = std::sregex_iterator(payload->begin(), payload->end(),
                                           package_uri);
         match != std::sregex_iterator(); ++match) {
      resources.emplace((*match)[1].str(), (*match)[2].str());
    }
  }
  for (const auto& [package_name, relative_text] : resources) {
    const std::filesystem::path relative(relative_text);
    if (relative.is_absolute() || relative.empty() ||
        std::any_of(relative.begin(), relative.end(), [](const auto& part) {
          return part == "." || part == ".." || part.empty();
        })) {
      throw std::runtime_error("fixture package URI is not canonical");
    }
    const auto resource = projection.package_share / package_name / relative;
    if (!std::filesystem::is_regular_file(resource) ||
        std::filesystem::is_symlink(resource)) {
      throw std::runtime_error("fixture package resource is unresolved");
    }
    const std::string logical_path = package_name + "/" + relative.generic_string();
    append_hash_frame(framed, "resource-resolution/" + logical_path, "0");
    append_hash_frame(framed, "resource/" + logical_path, read_file(resource));
  }
  projection.resource_digest =
      manyforge::content_identity::sha256_digest(framed);
  return projection;
}

std::string derived_fixture_digest(std::string_view domain,
                                   std::string_view resource_digest) {
  return manyforge::content_identity::sha256_digest(
      std::string(domain) + ":" + std::string(resource_digest));
}

ProjectionLeaseBindingDTO projection_binding(
    std::string_view resource_digest) {
  return ProjectionLeaseBindingDTO{
      ProjectionRevisionDTO{
          RetainedBundleId{
              derived_fixture_digest("e1-bundle", resource_digest)},
          GraphRevision{derived_fixture_digest("e1-graph", resource_digest)},
          SceneRevision{
              SceneId{derived_fixture_digest("e1-scene", resource_digest)}, 7U,
              ContentDigest{std::string(resource_digest)}}},
      ProjectionLeaseId{"e1-lease-seven"}};
}

Eigen::VectorXd eigen_vector(const std::vector<double>& values) {
  return Eigen::Map<const Eigen::VectorXd>(
      values.data(), static_cast<Eigen::Index>(values.size()));
}

std::vector<double> std_vector(const Eigen::VectorXd& values) {
  return {values.data(), values.data() + values.size()};
}

Eigen::Matrix4d native_pose(const Pose3DTO& pose) {
  const Eigen::Quaterniond orientation(
      pose.orientation.w, pose.orientation.x, pose.orientation.y,
      pose.orientation.z);
  Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
  result.block<3, 3>(0, 0) = orientation.normalized().toRotationMatrix();
  result(0, 3) = pose.position_m.x_m;
  result(1, 3) = pose.position_m.y_m;
  result(2, 3) = pose.position_m.z_m;
  return result;
}

Pose3DTO portable_pose(const Eigen::Matrix4d& transform) {
  const Eigen::Quaterniond orientation(transform.block<3, 3>(0, 0));
  Pose3DTO result;
  result.position_m = {
      transform(0, 3), transform(1, 3), transform(2, 3)};
  result.orientation = {orientation.x(), orientation.y(), orientation.z(),
                        orientation.w()};
  return result;
}

ErrorDTO error(std::string code, std::string reason, std::string message) {
  return ErrorDTO{std::move(code), std::move(message),
                  {{"reason", std::move(reason)}}};
}

class E1RoboPlanCandidateAdapter final : public IPlannerBackend {
 public:
  E1RoboPlanCandidateAdapter()
      : projection_(make_fixture_projection()),
        binding_(projection_binding(projection_.resource_digest)),
        scene_(make_scene(projection_)) {}

  [[nodiscard]] const ProjectionLeaseBindingDTO& binding() const noexcept {
    return binding_;
  }

  [[nodiscard]] Pose3DTO forward_kinematics(
      const RobotStateDTO& state) const {
    const auto group = eigen_vector(state.positions_rad);
    const auto full = scene_->toFullJointPositions(kGroup, group);
    return portable_pose(scene_->forwardKinematics(full, kTip, kBase));
  }

  PlanResultDTO plan(const PlanRequestDTO& request) override {
    if (!request.projection_binding.valid() ||
        request.projection_binding != binding_) {
      return {std::nullopt,
              error(kErrorInvalidRequest, "planner_projection_binding_invalid",
                    "candidate adapter requires its exact retained projection"),
              {}};
    }
    const auto start = angular_position_projection(request.start_state);
    if (request.model_group_binding.compiled_group_name != kGroup ||
        compiled_frame_name(request.base_frame) != kBase ||
        compiled_frame_name(request.tcp_frame) != kTip ||
        request.model_group_binding.compiled_joint_names != kJointNames ||
        request.start_state.joint_names != kJointNames ||
        request.start_state.resources != request.resources || !start.ok() ||
        start.value().size() != kJointNames.size()) {
      return {std::nullopt,
              error(kErrorInvalidRequest, "planner_request_model_mismatch",
                    "portable request does not match the retained model"),
              {}};
    }
    if (request.planning_profile != PlanningProfileDTO::kFirstFeasible) {
      const auto capability =
          request.planning_profile == PlanningProfileDTO::kBalanced
              ? "planning.profile.balanced"
              : "planning.profile.quality_bounded";
      return {std::nullopt,
              ErrorDTO{kErrorUnsupportedCapability,
                       "E1 qualifies only RoboPlan first-feasible semantics",
                       {{"reason", "planner_capability_unsupported"},
                        {"capability", capability}}},
              {}};
    }

    try {
      std::optional<roboplan::JointTrajectory> trajectory;
      if (const auto* goal =
              std::get_if<ResolvedJointGoalDTO>(&request.motion)) {
        if (goal->joint_names != kJointNames ||
            goal->resources != request.resources ||
            goal->positions_rad.size() != kJointNames.size()) {
          return {std::nullopt,
                  error(kErrorInvalidRequest, "joint_goal_shape_invalid",
                        "joint goal does not match the canonical group"),
                  {}};
        }
        trajectory = plan_joint(start.value(), goal->positions_rad, request);
      } else if (const auto* goal =
                     std::get_if<PoseGoalDTO>(&request.motion)) {
        const auto joint_goal = solve_pose_goal(
            goal->goal, start.value(),
            request.planning_timeout_s);
        if (!joint_goal.has_value()) {
          return {std::nullopt,
                  error(kErrorPlanningFailed, "pose_ik_failed",
                        "RoboPlan could not resolve the portable pose goal"),
                  {}};
        }
        trajectory = plan_joint(start.value(), *joint_goal, request);
      } else if (const auto* path =
                     std::get_if<CartesianPathDTO>(&request.motion)) {
        trajectory = plan_cartesian(*path, request);
      } else {
        return {std::nullopt,
                ErrorDTO{kErrorUnsupportedCapability,
                         "named targets are resolved before the planner port",
                         {{"reason", "planner_capability_unsupported"},
                          {"capability", "planning.motion.named_target"}}},
                {}};
      }
      if (!trajectory.has_value()) {
        return {std::nullopt,
                error(kErrorPlanningFailed, "native_candidate_unavailable",
                      "RoboPlan did not produce a candidate trajectory"),
                {}};
      }
      return {portable_trajectory(*trajectory, request.resources), std::nullopt,
              binding_.projection_revision};
    } catch (const std::exception& exception) {
      return {std::nullopt,
              ErrorDTO{kErrorPlanningFailed,
                       "RoboPlan candidate computation failed",
                       {{"reason", "native_candidate_exception"},
                        {"native_detail", exception.what()}}},
              {}};
    }
  }

  IkResultDTO solve_ik(const IkRequestDTO& request) override {
    const auto seed = angular_position_projection(request.seed_state);
    if (compiled_frame_name(request.base_frame) != kBase ||
        compiled_frame_name(request.tcp_frame) != kTip ||
        request.resources != request.seed_state.resources ||
        request.seed_state.joint_names != kJointNames ||
        !seed.ok() || seed.value().size() != kJointNames.size()) {
      return {false, {}, {}, {},
              error(kErrorInvalidRequest, "ik_request_model_mismatch",
                    "portable IK request does not match the retained model")};
    }
    const auto solution = solve_pose_goal(
        request.goal_pose, seed.value(), request.timeout_s);
    if (!solution.has_value()) {
      return {false, {}, {}, {},
              error(kErrorPlanningFailed, "ik_no_solution",
                    "RoboPlan did not produce an IK candidate")};
    }
    return {true, request.resources, kJointNames, *solution, std::nullopt};
  }

 private:
  static std::shared_ptr<roboplan::Scene> make_scene(
      const FixtureProjection& projection) {
    return std::make_shared<roboplan::Scene>(
        "e1_conformance_projection", projection.urdf, projection.srdf,
        std::vector<std::filesystem::path>{projection.package_share});
  }

  std::optional<std::vector<double>> solve_pose_goal(
      const Pose3DTO& goal, const std::vector<double>& seed,
      double timeout_s) {
    roboplan::SimpleIkOptions options;
    options.group_name = kGroup;
    options.max_time =
        std::clamp(timeout_s > 0.0 ? timeout_s : 1.0, 0.001, 2.0);
    options.max_restarts = 0;
    options.check_collisions = false;
    roboplan::SimpleIk planner(scene_, options);
    scene_->setRngSeed(286);
    roboplan::CartesianConfiguration native_goal{
        kBase, kTip, native_pose(goal)};
    roboplan::JointConfiguration native_seed{
        kJointNames, eigen_vector(seed), {}, {}};
    roboplan::JointConfiguration native_solution;
    if (!planner.solveIk(native_goal, native_seed, native_solution)) {
      return std::nullopt;
    }
    return std_vector(native_solution.positions);
  }

  std::optional<roboplan::JointTrajectory> plan_joint(
      const std::vector<double>& start, const std::vector<double>& goal,
      const PlanRequestDTO& request) {
    roboplan::RRTOptions options;
    options.group_name = kGroup;
    options.max_nodes = 1000;
    options.max_connection_distance = 1.0;
    options.collision_check_step_size = 0.05;
    options.max_planning_time =
        std::clamp(request.planning_timeout_s, 0.001, 2.0);
    options.fast_return = true;
    roboplan::RRT planner(scene_, options);
    planner.setRngSeed(1234U + request.planning_attempt_index);
    const roboplan::JointConfiguration native_start{
        kJointNames, eigen_vector(start), {}, {}};
    const roboplan::JointConfiguration native_goal{
        kJointNames, eigen_vector(goal), {}, {}};
    const auto path = planner.plan(native_start, native_goal);
    if (!path.has_value()) {
      return std::nullopt;
    }
    roboplan::TOPPRAOptions timing;
    timing.dt = 0.01;
    timing.mode = roboplan::SplineFittingMode::Hermite;
    timing.velocity_scale = request.speed_profile.velocity_scale;
    timing.acceleration_scale = request.speed_profile.acceleration_scale;
    timing.max_output_samples = 65'536;
    roboplan::PathParameterizerTOPPRA parameterizer(scene_, kGroup);
    const auto trajectory = parameterizer.generate(*path, timing);
    if (!trajectory.has_value()) {
      return std::nullopt;
    }
    return *trajectory;
  }

  std::optional<roboplan::JointTrajectory> plan_cartesian(
      const CartesianPathDTO& path, const PlanRequestDTO& request) {
    if (path.waypoints.size() < 2U) {
      return std::nullopt;
    }
    std::vector<Eigen::Matrix4d> transforms;
    transforms.reserve(path.waypoints.size());
    std::transform(path.waypoints.begin(), path.waypoints.end(),
                   std::back_inserter(transforms), native_pose);
    const roboplan::CartesianPath native_path{
        {kBase}, {kTip}, {std::move(transforms)}};
    roboplan::CartesianPlannerOptions options;
    options.group_name = kGroup;
    options.dt = 0.01;
    options.speed_mode = roboplan::CartesianSpeedMode::Bounded;
    options.max_linear_speed = path.max_linear_speed_mps;
    options.max_angular_speed = path.max_angular_speed_rad_s;
    options.max_linear_acceleration = path.max_linear_acceleration_mps2;
    options.max_angular_acceleration = path.max_angular_acceleration_rad_s2;
    options.max_position_error = request.tolerances.translation_tol_m;
    options.max_orientation_error = request.tolerances.rotation_tol_rad;
    options.velocity_scale = request.speed_profile.velocity_scale;
    options.acceleration_scale = request.speed_profile.acceleration_scale;
    roboplan::CartesianPathPlanner planner(scene_, options);
    const auto start = angular_position_projection(request.start_state);
    if (!start.ok()) {
      return std::nullopt;
    }
    const roboplan::JointConfiguration native_start{
        kJointNames, eigen_vector(start.value()), {}, {}};
    const auto trajectory = planner.plan(native_path, native_start);
    if (!trajectory.has_value()) {
      return std::nullopt;
    }
    return *trajectory;
  }

  static TrajectoryDTO portable_trajectory(
      const roboplan::JointTrajectory& native,
      const std::vector<ResourceId>& resources) {
    if (native.positions.size() != native.times.size()) {
      throw std::runtime_error("native trajectory shape mismatch");
    }
    TrajectoryDTO output;
    output.resources = resources;
    output.joint_names = native.joint_names;
    output.waypoints.reserve(native.positions.size());
    for (std::size_t index = 0; index < native.positions.size(); ++index) {
      output.waypoints.push_back(TrajectoryWaypointDTO{
          native.times[index],
          std_vector(native.positions[index]),
          index < native.velocities.size()
              ? std_vector(native.velocities[index])
              : std::vector<double>{},
          index < native.accelerations.size()
              ? std_vector(native.accelerations[index])
              : std::vector<double>{}});
    }
    return output;
  }

  const FixtureProjection projection_;
  const ProjectionLeaseBindingDTO binding_;
  const std::shared_ptr<roboplan::Scene> scene_;
};

PlanRequestDTO request_base(std::string move_id,
                            std::vector<double> start) {
  auto request = make_test_plan_request(
      std::move(move_id),
      ResolvedJointGoalDTO{{}, kJointNames,
                           std::vector<double>(kJointNames.size(), 0.0)},
      kJointNames, std::move(start));
  request.model_group_binding.compiled_group_name = kGroup;
  std::get<ResolvedComponentModelFrameDTO>(request.base_frame)
      .compiled_frame_name = kBase;
  std::get<ResolvedComponentModelFrameDTO>(request.tcp_frame)
      .compiled_frame_name = kTip;
  request.tolerances = {1e-6, 0.01, 0.05};
  request.speed_profile = {0.5, 0.5, std::nullopt};
  request.planning_timeout_s = 1.0;
  return request;
}

PlannerBackendConformanceInput make_candidate_input(
    E1RoboPlanCandidateAdapter& adapter) {
  const std::vector<double> joint_start{
      2.261477656358459, 1.9570617913216783,  -1.156210802275467,
      2.0229644353394325, -0.8713978455862086, -1.3594747944952112};
  const std::vector<double> joint_goal{
      2.959756700843902,  -1.6588986483183334, -1.2938684506809712,
      -0.4132159894019849, -2.76715775824764, 0.7740437209968403};
  auto joint = request_base("candidate_joint", joint_start);
  joint.projection_binding = adapter.binding();
  joint.motion = ResolvedJointGoalDTO{joint.resources, kJointNames, joint_goal};

  const std::vector<double> pose_target{
      0.0, -1.0, 1.0, -1.5, -1.5, 0.0};
  auto pose = request_base(
      "candidate_pose", {0.0, -1.2, 1.1, -1.4, -1.4, 0.1});
  pose.projection_binding = adapter.binding();
  pose.motion = PoseGoalDTO{
      adapter.forward_kinematics(
          RobotStateDTO{kJointNames, pose_target, {}})};

  const std::vector<double> cartesian_start{
      0.0, -1.5708, 1.5708, -1.5708, -1.5708, 0.0};
  auto cartesian = request_base("candidate_cartesian", cartesian_start);
  cartesian.projection_binding = adapter.binding();
  const Pose3DTO start_pose = adapter.forward_kinematics(
      RobotStateDTO{kJointNames, cartesian_start, {}});
  CartesianPathDTO path;
  for (int index = 0; index < 3; ++index) {
    auto waypoint = start_pose;
    waypoint.position_m.x_m += static_cast<double>(index) * 0.025;
    path.waypoints.push_back(waypoint);
  }
  path.max_linear_speed_mps = 0.05;
  cartesian.motion = path;

  PlannerBackendConformanceInput input;
  input.adapter_id = "planning.roboplan_0_6_e1_qualification";
  input.planner = &adapter;
  input.capabilities = {true, true, true, true, false, false, true};
  input.plan_cases =
      make_portable_planner_matrix(joint, pose, cartesian);
  input.forward_kinematics = [&adapter](const FkRequestDTO& request) {
    if (request.robot_state.joint_names != kJointNames ||
        request.robot_state.positions_rad.size() != kJointNames.size()) {
      return Result<Pose3DTO>::Err(
          error(kErrorInvalidRequest, "fk_request_model_mismatch",
                "portable FK request does not match retained model"));
    }
    return Result<Pose3DTO>::Ok(
        adapter.forward_kinematics(request.robot_state));
  };
  auto invalid = joint;
  invalid.move_id = MoveId("invalid_binding");
  invalid.projection_binding.projection_revision.scene_revision.content_hash =
      ContentDigest{kDigestD};
  input.expected_failures.push_back(
      {"invalid_projection_binding", invalid, kErrorInvalidRequest,
       "planner_projection_binding_invalid"});
  input.ik_case = PlannerIkConformanceCase{
      "single_shot_ik",
      IkRequestDTO{
          pose.controllable_component_id, pose.command_target_id,
          pose.model_bindings, pose.resources,
          adapter.forward_kinematics(
              RobotStateDTO{kJointNames, pose_target, {}}),
          pose.base_frame, pose.tcp_frame, pose.start_state,
          pose.projection_binding, 1.0}};
  input.max_waypoint_count = 65'536;
  input.max_plan_wall_time = std::chrono::seconds(5);
  return input;
}

TEST(E1RoboPlanCandidateConformance,
     UnchangedPortableMatrixReachesTheExactCandidateAdapter) {
  E1RoboPlanCandidateAdapter adapter;
  const auto report =
      run_planner_backend_conformance(make_candidate_input(adapter));
  EXPECT_TRUE(report.passed()) << report.summary();
  EXPECT_EQ(report.completed_cases.size(), 11U);
}

}  // namespace
}  // namespace manyforge::core::testing
