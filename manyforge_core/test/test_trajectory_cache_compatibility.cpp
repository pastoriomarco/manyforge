#include <gtest/gtest.h>

#include <manyforge_core/cache/trajectory_cache_compatibility.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>

#include <stdexcept>

namespace manyforge::core {
namespace {

class FkOnlyPlanningContext final : public IPlanningContext {
 public:
  Pose3DTO next_fk;
  bool throw_fk = false;
  FkRequestDTO last_fk_request;

  RobotStateDTO get_current_state() override { return {}; }

  ValidationResultDTO validate_trajectory(
      const TrajectoryDTO& /*trajectory*/,
      const ValidationOptionsDTO& /*options*/) override {
    return ValidationResultDTO{true, std::nullopt};
  }

  CollisionCheckResultDTO collision_check(
      const CollisionCheckRequestDTO& /*request*/) override {
    return {};
  }

  Pose3DTO forward_kinematics(const FkRequestDTO& request) override {
    last_fk_request = request;
    if (throw_fk) {
      throw std::runtime_error("fk failed");
    }
    return next_fk;
  }

  Pose3DTO get_object_pose(const std::string& /*object_id*/,
                           const std::string& /*base_frame*/) override {
    return {};
  }

  ObjectStateDTO get_object_state(const std::string& /*object_id*/) override {
    return {};
  }

  Result<Pose3DTO> resolve_reference_pose(
      const ReferenceDTO& /*reference*/,
      const std::string& /*base_frame*/) override {
    return Result<Pose3DTO>::Ok(Pose3DTO{});
  }

  JointLimitsDTO get_joint_limits(const std::string& /*robot_id*/,
                                  const std::string& /*group_id*/) override {
    return {};
  }

  SceneEditResultDTO add_object(
      const AddObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{true, std::nullopt};
  }

  SceneEditResultDTO remove_object(
      const RemoveObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{true, std::nullopt};
  }

  SceneEditResultDTO update_object_pose(
      const UpdateObjectPoseRequestDTO& /*request*/) override {
    return SceneEditResultDTO{true, std::nullopt};
  }

  SceneEditResultDTO attach_object(
      const AttachObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{true, std::nullopt};
  }

  SceneEditResultDTO detach_object(
      const DetachObjectRequestDTO& /*request*/) override {
    return SceneEditResultDTO{true, std::nullopt};
  }
};

TrajectoryDTO make_two_joint_trajectory() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"joint_a", "joint_b"};
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, {0.5, -0.25}, {}, {}});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{1.0, {1.0, -0.5}, {0.0, 0.0}, {}});
  return trajectory;
}

ExecuteRequestDTO make_joint_request(std::vector<std::string> names,
                                     std::vector<double> positions) {
  ExecuteRequestDTO request;
  request.tolerances.joint_position_tol_rad = 1e-4;
  JointGoalDTO goal;
  goal.joint_names = std::move(names);
  goal.positions_rad = std::move(positions);
  request.motion = goal;
  return request;
}

ExecuteRequestDTO make_pose_request(Pose3DTO goal) {
  ExecuteRequestDTO request;
  request.base_frame = "base";
  request.tcp_frame = "tcp";
  request.tolerances.translation_tol_m = 1e-3;
  request.tolerances.rotation_tol_rad = 1e-3;
  PoseGoalDTO pose_goal;
  pose_goal.goal = goal;
  request.motion = pose_goal;
  return request;
}

TEST(TrajectoryCacheCompatibilityTest,
     JointGoalMatchesByNameIndependentOfOrder) {
  FkOnlyPlanningContext context;
  auto trajectory = make_two_joint_trajectory();
  auto request =
      make_joint_request({"joint_b", "joint_a"}, {-0.5, 1.0});

  const auto result =
      cached_trajectory_matches_request_goal(context, trajectory, request);

  EXPECT_TRUE(result.compatible);
  EXPECT_TRUE(result.reason.empty());
}

TEST(TrajectoryCacheCompatibilityTest,
     JointGoalRejectsDuplicateTrajectoryNamesAsUnverifiable) {
  FkOnlyPlanningContext context;
  auto trajectory = make_two_joint_trajectory();
  trajectory.joint_names = {"joint_a", "joint_a"};
  auto request =
      make_joint_request({"joint_a", "joint_b"}, {1.0, -0.5});

  const auto result =
      cached_trajectory_matches_request_goal(context, trajectory, request);

  EXPECT_FALSE(result.compatible);
  EXPECT_EQ(result.reason, "goal_unverifiable");
}

TEST(TrajectoryCacheCompatibilityTest,
     JointGoalRejectsDifferentEndpointAsChanged) {
  FkOnlyPlanningContext context;
  auto trajectory = make_two_joint_trajectory();
  auto request =
      make_joint_request({"joint_a", "joint_b"}, {1.25, -0.5});

  const auto result =
      cached_trajectory_matches_request_goal(context, trajectory, request);

  EXPECT_FALSE(result.compatible);
  EXPECT_EQ(result.reason, "goal_changed");
}

TEST(TrajectoryCacheCompatibilityTest,
     CurrentStartMatchesByNameIndependentOfOrder) {
  auto trajectory = make_two_joint_trajectory();
  RobotStateDTO state;
  state.joint_names = {"joint_b", "joint_a"};
  state.positions_rad = {-0.25, 0.5};

  const auto result =
      cached_trajectory_matches_current_start(trajectory, state, 1e-4);

  EXPECT_TRUE(result.compatible);
}

TEST(TrajectoryCacheCompatibilityTest, CurrentStartRejectsStalePosition) {
  auto trajectory = make_two_joint_trajectory();
  RobotStateDTO state;
  state.joint_names = {"joint_a", "joint_b"};
  state.positions_rad = {0.55, -0.25};

  const auto result =
      cached_trajectory_matches_current_start(trajectory, state, 1e-4);

  EXPECT_FALSE(result.compatible);
  EXPECT_EQ(result.reason, "start_state_changed");
}

TEST(TrajectoryCacheCompatibilityTest, PoseGoalUsesFinalWaypointFk) {
  FkOnlyPlanningContext context;
  context.next_fk.position_m = {0.1, 0.2, 0.3};
  context.next_fk.orientation = {0.0, 0.0, 0.0, 1.0};
  auto trajectory = make_two_joint_trajectory();
  auto request = make_pose_request(context.next_fk);

  const auto result =
      cached_trajectory_matches_request_goal(context, trajectory, request);

  EXPECT_TRUE(result.compatible);
  EXPECT_EQ(context.last_fk_request.target_frame, "tcp");
  EXPECT_EQ(context.last_fk_request.base_frame, "base");
  EXPECT_EQ(context.last_fk_request.robot_state.joint_names,
            trajectory.joint_names);
  EXPECT_EQ(context.last_fk_request.robot_state.positions_rad,
            trajectory.waypoints.back().positions_rad);
}

TEST(TrajectoryCacheCompatibilityTest, PoseGoalRejectsFkFailure) {
  FkOnlyPlanningContext context;
  context.throw_fk = true;
  auto trajectory = make_two_joint_trajectory();
  auto request = make_pose_request(Pose3DTO{});

  const auto result =
      cached_trajectory_matches_request_goal(context, trajectory, request);

  EXPECT_FALSE(result.compatible);
  EXPECT_EQ(result.reason, "fk_failed");
}

}  // namespace
}  // namespace manyforge::core
