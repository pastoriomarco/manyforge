#include <gtest/gtest.h>

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

class ReferenceResolver final : public IReferencePoseResolver {
 public:
  mutable int call_count = 0;
  std::map<std::string, Pose3DTO> poses;

  Result<Pose3DTO> resolve_reference_pose(
      const ReferenceDTO& reference,
      const std::string&,
      const ProjectionLeaseBindingDTO&) const override {
    ++call_count;
    const auto found = poses.find(reference.id);
    if (found == poses.end()) {
      return Result<Pose3DTO>::Err(ErrorDTO{
          kErrorMissingReference,
          "reference unavailable",
          {{"reference_id", reference.id}}});
    }
    return Result<Pose3DTO>::Ok(found->second);
  }
};

ExecuteRequestDTO base_request() {
  auto request = testing::make_test_execute_request(
      {"joint_a", "joint_b"});
  request.move_id = "move";
  std::get<ResolvedComponentModelFrameDTO>(request.binding.tcp_frame)
      .compiled_frame_name = "tool";
  return request;
}

ReferenceDTO frame_reference(std::string id) {
  ReferenceDTO reference;
  reference.type = ReferenceDTO::Type::kFrame;
  reference.id = std::move(id);
  return reference;
}

TEST(MotionTargetResolution, RelativeJointDeltasResolveByJointName) {
  auto request = base_request();
  request.target_mode = MotionTargetModeDTO::kRelative;
  request.motion = ResolvedJointGoalDTO{
      {ResourceId("resource_b"), ResourceId("resource_a")},
      {"joint_b", "joint_a"}, {0.25, -0.5}};
  RobotStateDTO start;
  start.joint_names = {"joint_a", "joint_b"};
  start.positions_rad = {1.0, 2.0};
  ReferenceResolver resolver;

  auto result = resolve_motion_target(
      request, ProjectionLeaseBindingDTO{}, &start, resolver);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(
      std::holds_alternative<ResolvedJointGoalDTO>(result.value()));
  const auto& goal = std::get<ResolvedJointGoalDTO>(result.value());
  EXPECT_EQ(goal.joint_names,
            (std::vector<std::string>{"joint_b", "joint_a"}));
  EXPECT_EQ(goal.positions_rad, (std::vector<double>{2.25, 0.5}));
  EXPECT_EQ(resolver.call_count, 0);
}

TEST(MotionTargetResolution, RelativeJointRejectsMissingObservedStart) {
  auto request = base_request();
  request.target_mode = MotionTargetModeDTO::kRelative;
  request.motion = ResolvedJointGoalDTO{{ResourceId("resource_a")},
                                        {"joint_a"}, {0.25}};
  ReferenceResolver resolver;

  auto result = resolve_motion_target(
      request, ProjectionLeaseBindingDTO{}, nullptr, resolver);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, kErrorStartStateMismatch);
  EXPECT_EQ(result.error().details.at("reason"),
            "relative_joint_start_state_unavailable");
}

TEST(MotionTargetResolution, JointTargetsRejectDuplicateNamesInEveryMode) {
  ReferenceResolver resolver;
  for (const auto mode : {MotionTargetModeDTO::kAbsolute,
                          MotionTargetModeDTO::kRelative}) {
    auto request = base_request();
    request.target_mode = mode;
    request.motion = ResolvedJointGoalDTO{
        {ResourceId("resource_a"), ResourceId("resource_a_duplicate")},
        {"joint_a", "joint_a"}, {0.25, -0.5}};
    RobotStateDTO start;
    start.joint_names = {"joint_a"};
    start.positions_rad = {1.0};

    auto result = resolve_motion_target(
        request, ProjectionLeaseBindingDTO{}, &start, resolver);

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

  auto result = resolve_motion_target(
      request, ProjectionLeaseBindingDTO{}, nullptr, resolver);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<PoseGoalDTO>(result.value()));
  const auto& goal = std::get<PoseGoalDTO>(result.value()).goal;
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

  auto result = resolve_motion_target(
      request, ProjectionLeaseBindingDTO{}, nullptr, resolver);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value()));
  const auto& resolved = std::get<CartesianPathDTO>(result.value());
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

  auto result = resolve_motion_target(
      request, ProjectionLeaseBindingDTO{}, nullptr, resolver);

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

  auto result = resolve_motion_target(
      request, ProjectionLeaseBindingDTO{}, nullptr, resolver);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "relative_named_target_unsupported");
}

}  // namespace
}  // namespace manyforge::core
