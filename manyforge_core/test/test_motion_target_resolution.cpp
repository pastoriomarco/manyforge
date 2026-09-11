#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/detail/motion_resume_intent.hpp>
#include <manyforge_core/motion_target_resolution.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <map>
#include <string>

namespace manyforge::core {
namespace {

Pose3DTO pose_at(double x_m, double y_m, double z_m) {
  Pose3DTO pose;
  pose.position_m = {x_m, y_m, z_m};
  pose.orientation.w = 1.0;
  return pose;
}

TEST(MotionResumeIntentTest, RetainsResolvedJointAndPoseDestinationsWithoutCursor) {
  const auto current = pose_at(9.0, 8.0, 7.0);
  for (const ResolvedMotionSpecDTO& source : std::vector<ResolvedMotionSpecDTO>{
           ResolvedJointGoalDTO{{ResourceId("joint")}, {"joint"}, {0.5}},
           PoseGoalDTO{pose_at(0.1, 0.2, 0.3)}}) {
    auto retained = detail::capture_motion_resume_intent(source, {}, std::nullopt);
    ASSERT_TRUE(retained.ok());
    const auto resumed = detail::resume_motion_from_current_tcp(retained.value(), current);
    EXPECT_EQ(resumed, source);
  }
}

TEST(MotionResumeIntentTest, ReplacesOnlyConsumedCartesianPrefixAndKeepsConstraints) {
  CartesianPathDTO source;
  source.waypoints = {pose_at(0, 0, 0), pose_at(1, 0, 0), pose_at(2, 0, 0), pose_at(1, 0, 0)};
  source.max_linear_speed_mps = 0.037;
  source.max_angular_acceleration_rad_s2 = 0.123;
  TrajectoryDTO trajectory;
  trajectory.waypoints = {{0.0, {0.0}, {}, {}}, {3.0, {1.0}, {}, {}}};
  trajectory.cartesian_waypoint_times_s = {0.0, 1.0, 2.0, 3.0};
  auto retained = detail::capture_motion_resume_intent(source, trajectory, 1.5);
  ASSERT_TRUE(retained.ok());
  EXPECT_EQ(retained.value().next_cartesian_waypoint, 2U);
  const auto current = pose_at(0.3, 0.4, 0.5);
  const auto resumed = detail::resume_motion_from_current_tcp(retained.value(), current);
  auto expected = source;
  expected.waypoints = {current, source.waypoints[2], source.waypoints[3]};
  EXPECT_EQ(std::get<CartesianPathDTO>(resumed), expected);
  EXPECT_EQ(std::get<CartesianPathDTO>(retained.value().motion), source);
}

TEST(MotionResumeIntentTest, ExactMilestonesConsumeDuplicatesButRetainInterruptedFinalTarget) {
  CartesianPathDTO source;
  source.waypoints = {pose_at(0, 0, 0), pose_at(1, 0, 0), pose_at(1, 0, 0), pose_at(0, 0, 0)};
  TrajectoryDTO trajectory;
  trajectory.waypoints = {{0.0, {0.0}, {}, {}}, {2.0, {0.0}, {}, {}}};
  trajectory.cartesian_waypoint_times_s = {0.0, 1.0, 1.0, 2.0};
  const auto before =
      detail::capture_motion_resume_intent(source, trajectory, std::nextafter(1.0, 0.0));
  ASSERT_TRUE(before.ok());
  EXPECT_EQ(before.value().next_cartesian_waypoint, 1U);
  const auto reached = detail::capture_motion_resume_intent(source, trajectory, 1.0);
  ASSERT_TRUE(reached.ok());
  EXPECT_EQ(reached.value().next_cartesian_waypoint, 3U);
  const auto complete = detail::capture_motion_resume_intent(source, trajectory, 2.0);
  ASSERT_TRUE(complete.ok());
  EXPECT_EQ(complete.value().next_cartesian_waypoint, 3U);
  for (const auto& current : {source.waypoints.back(), pose_at(8, 9, 10)}) {
    const auto resumed = detail::resume_motion_from_current_tcp(complete.value(), current);
    const auto& path = std::get<CartesianPathDTO>(resumed);
    ASSERT_EQ(path.waypoints.size(), 2U);
    EXPECT_EQ(path.waypoints.front(), current);
    EXPECT_EQ(path.waypoints.back(), source.waypoints.back());
  }
}

TEST(MotionResumeIntentTest, MissingOrCrossedProgressNeverGuessesRemainingDestinations) {
  CartesianPathDTO source;
  source.waypoints = {pose_at(0, 0, 0), pose_at(1, 0, 0), pose_at(2, 0, 0)};
  TrajectoryDTO trajectory;
  trajectory.waypoints = {{0.0, {0.0}, {}, {}}, {2.0, {1.0}, {}, {}}};
  trajectory.cartesian_waypoint_times_s = {0.0, 1.0, 2.0};
  for (const auto cursor : std::vector<std::optional<double>>{
           std::nullopt, -1.0, 2.1, std::numeric_limits<double>::quiet_NaN()})
    EXPECT_FALSE(detail::capture_motion_resume_intent(source, trajectory, cursor).ok());
  for (const auto& times : std::vector<std::vector<double>>{
           {}, {0.0, 2.0}, {0.0, 2.0, 1.0}, {0.1, 1.0, 2.0}, {0.0, 1.0, 3.0}}) {
    trajectory.cartesian_waypoint_times_s = times;
    EXPECT_FALSE(detail::capture_motion_resume_intent(source, trajectory, 0.5).ok());
  }
}

class ReferenceResolver final : public IReferencePoseResolver {
 public:
  mutable int call_count = 0;
  std::map<std::string, Pose3DTO> poses;

  Result<Pose3DTO> resolve_reference_pose(
      const ReferenceDTO &reference,
      const ProgramFrameResolutionContextDTO &) const override {
    ++call_count;
    const auto identity = std::string(reference_identity_value(reference));
    const auto found = poses.find(identity);
    if (found == poses.end()) {
      return Result<Pose3DTO>::Err(ErrorDTO{kErrorMissingReference,
                                            "reference unavailable",
                                            {{"reference_id", identity}}});
    }
    return Result<Pose3DTO>::Ok(found->second);
  }
};

ExecuteRequestDTO base_request() {
  auto request = testing::make_test_execute_request(
      {"joint_a", "joint_b"}, "run:motion-resolution-test");
  request.move_id = "move";
  std::get<ResolvedComponentModelFrameDTO>(request.binding.tcp_frame)
      .compiled_frame_name = "tool";
  return request;
}

ReferenceDTO frame_reference(std::string id) {
  return ReferenceDTO(ReferenceDTO::Type::kFrameRole, std::move(id));
}

TEST(MotionTargetResolution, RelativeJointDeltasResolveByJointName) {
  auto request = base_request();
  request.target_mode = MotionTargetModeDTO::kRelative;
  request.motion =
      ResolvedJointGoalDTO{{ResourceId("resource_b"), ResourceId("resource_a")},
                           {"joint_b", "joint_a"},
                           {0.25, -0.5}};
  auto context =
      testing::make_test_program_frame_context(request.binding, {1.0, 2.0});
  ReferenceResolver resolver;

  auto result = resolve_motion_target(request, &context, resolver);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<ResolvedJointGoalDTO>(result.value()));
  const auto &goal = std::get<ResolvedJointGoalDTO>(result.value());
  EXPECT_EQ(goal.joint_names, (std::vector<std::string>{"joint_b", "joint_a"}));
  EXPECT_EQ(goal.positions_rad, (std::vector<double>{2.25, 0.5}));
  EXPECT_EQ(resolver.call_count, 0);
}

TEST(MotionTargetResolution, RelativeJointRejectsMissingObservedStart) {
  auto request = base_request();
  request.target_mode = MotionTargetModeDTO::kRelative;
  request.motion =
      ResolvedJointGoalDTO{{ResourceId("resource_a")}, {"joint_a"}, {0.25}};
  ReferenceResolver resolver;

  auto result = resolve_motion_target(request, nullptr, resolver);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorStartStateMismatch);
  EXPECT_EQ(result.error().details.at("reason"),
            "relative_joint_start_state_unavailable");
}

TEST(MotionTargetResolution, JointTargetsRejectDuplicateNamesInEveryMode) {
  ReferenceResolver resolver;
  for (const auto mode :
       {MotionTargetModeDTO::kAbsolute, MotionTargetModeDTO::kRelative}) {
    auto request = base_request();
    request.target_mode = mode;
    request.motion = ResolvedJointGoalDTO{
        {ResourceId("resource_a"), ResourceId("resource_a_duplicate")},
        {"joint_a", "joint_a"},
        {0.25, -0.5}};
    auto result = resolve_motion_target(request, nullptr, resolver);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "joint_target_duplicate_joint");
  }
}

TEST(MotionTargetResolution, RelativePoseResolvesReferenceOnce) {
  auto request = base_request();
  request.target_mode = MotionTargetModeDTO::kRelative;
  request.target_reference = frame_reference("fixture");
  request.motion = PoseGoalDTO{pose_at(0.1, -0.2, 0.3)};
  ReferenceResolver resolver;
  resolver.poses.emplace("fixture", pose_at(1.0, 2.0, 3.0));
  auto context =
      testing::make_test_program_frame_context(request.binding, {0.0, 0.0});

  auto result = resolve_motion_target(request, &context, resolver);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(result.value()));
  const auto &goal = std::get<PoseGoalDTO>(result.value()).goal;
  EXPECT_DOUBLE_EQ(goal.position_m.x_m, 1.1);
  EXPECT_DOUBLE_EQ(goal.position_m.y_m, 1.8);
  EXPECT_DOUBLE_EQ(goal.position_m.z_m, 3.3);
  EXPECT_EQ(resolver.call_count, 1);
}

TEST(MotionTargetResolution,
     RelativeCartesianTransformsEveryWaypointAgainstOneReference) {
  auto request = base_request();
  request.target_mode = MotionTargetModeDTO::kRelative;
  request.target_reference = frame_reference("fixture");
  CartesianPathDTO path;
  path.waypoints = {pose_at(0.1, 0.0, 0.0), pose_at(0.2, 0.0, 0.0),
                    pose_at(0.3, 0.0, 0.0)};
  request.motion = path;
  ReferenceResolver resolver;
  resolver.poses.emplace("fixture", pose_at(1.0, 2.0, 3.0));
  auto context =
      testing::make_test_program_frame_context(request.binding, {0.0, 0.0});

  auto result = resolve_motion_target(request, &context, resolver);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value()));
  const auto &resolved = std::get<CartesianPathDTO>(result.value());
  ASSERT_EQ(resolved.waypoints.size(), 3u);
  EXPECT_DOUBLE_EQ(resolved.waypoints[0].position_m.x_m, 1.1);
  EXPECT_DOUBLE_EQ(resolved.waypoints[1].position_m.x_m, 1.2);
  EXPECT_DOUBLE_EQ(resolved.waypoints[2].position_m.x_m, 1.3);
  EXPECT_EQ(resolver.call_count, 1);
}

TEST(MotionTargetResolution, AbsoluteMotionRejectsSpatialReferences) {
  auto request = base_request();
  request.motion = PoseGoalDTO{pose_at(1.0, 2.0, 3.0)};
  request.target_reference = frame_reference("fixture");
  ReferenceResolver resolver;

  auto result = resolve_motion_target(request, nullptr, resolver);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorInvalidRequest);
  EXPECT_EQ(result.error().details.at("reason"),
            "absolute_target_has_reference");
}

TEST(MotionTargetResolution, NamedTargetsAreAbsoluteOnly) {
  auto request = base_request();
  request.target_mode = MotionTargetModeDTO::kRelative;
  request.motion = NamedTargetDTO{"home"};
  ReferenceResolver resolver;

  auto result = resolve_motion_target(request, nullptr, resolver);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "relative_named_target_unsupported");
}

}  // namespace
}  // namespace manyforge::core
