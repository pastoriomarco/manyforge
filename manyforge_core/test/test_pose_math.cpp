#include <gtest/gtest.h>

#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/types/scene_frame_resolution.hpp>

#include <cmath>

namespace manyforge::core {
namespace {

constexpr double kPi = 3.14159265358979323846;

QuaternionDTO yaw_quaternion(double radians) {
  return QuaternionDTO{0.0, 0.0, std::sin(radians / 2.0),
                       std::cos(radians / 2.0)};
}

TEST(PoseMathTest, PoseWithPositionAndOrientationCopiesFields) {
  const Vector3DTO position{1.0, 2.0, 3.0};
  const QuaternionDTO orientation{0.1, 0.2, 0.3, 0.9};

  const auto pose = pose_with_position_and_orientation(position, orientation);

  EXPECT_DOUBLE_EQ(pose.position_m.x_m, 1.0);
  EXPECT_DOUBLE_EQ(pose.position_m.y_m, 2.0);
  EXPECT_DOUBLE_EQ(pose.position_m.z_m, 3.0);
  EXPECT_DOUBLE_EQ(pose.orientation.x, 0.1);
  EXPECT_DOUBLE_EQ(pose.orientation.y, 0.2);
  EXPECT_DOUBLE_EQ(pose.orientation.z, 0.3);
  EXPECT_DOUBLE_EQ(pose.orientation.w, 0.9);
}

TEST(PoseMathTest, MultiplyPosesComposesTranslationInParentFrame) {
  Pose3DTO parent;
  parent.position_m = {1.0, 2.0, 0.0};
  parent.orientation = yaw_quaternion(kPi / 2.0);

  Pose3DTO child;
  child.position_m = {1.0, 0.0, 0.0};
  child.orientation = yaw_quaternion(kPi / 2.0);

  const auto out = multiply_poses(parent, child);

  EXPECT_NEAR(out.position_m.x_m, 1.0, 1e-12);
  EXPECT_NEAR(out.position_m.y_m, 3.0, 1e-12);
  EXPECT_NEAR(out.position_m.z_m, 0.0, 1e-12);
  EXPECT_NEAR(std::abs(out.orientation.z), 1.0, 1e-12);
  EXPECT_NEAR(out.orientation.w, 0.0, 1e-12);
}

TEST(PoseMathTest, InversePoseRoundTripsTranslationAndRotation) {
  const Pose3DTO pose{{1.25, -2.0, 0.75}, yaw_quaternion(kPi / 3.0)};

  const auto identity = multiply_poses(pose, inverse_pose(pose));

  EXPECT_NEAR(identity.position_m.x_m, 0.0, 1e-12);
  EXPECT_NEAR(identity.position_m.y_m, 0.0, 1e-12);
  EXPECT_NEAR(identity.position_m.z_m, 0.0, 1e-12);
  EXPECT_NEAR(identity.orientation.x, 0.0, 1e-12);
  EXPECT_NEAR(identity.orientation.y, 0.0, 1e-12);
  EXPECT_NEAR(identity.orientation.z, 0.0, 1e-12);
  EXPECT_NEAR(std::abs(identity.orientation.w), 1.0, 1e-12);
}

TEST(SceneFrameResolutionTest, CompactReceiptRejectsIncompleteFrameIdentity) {
  constexpr std::string_view digest =
      "sha256:1111111111111111111111111111111111111111111111111111111111111111";
  SceneFrameResolutionReceiptDTO receipt{
      ResolvedComponentModelFrameDTO{
          ControllableComponentId("component.1"),
          ModelInstanceId("model.1"), FrameRoleId("tcp"), "compiled_tcp",
          ModelRevision("model-revision-1")},
      {},
      {},
      ProjectionLeaseBindingDTO{
          ProjectionRevisionDTO{
              RetainedBundleId(std::string(digest)),
              GraphRevision(std::string(digest)),
              SceneRevision{SceneId(std::string(digest)), 1U,
                            ContentDigest(std::string(digest))}},
          ProjectionLeaseId("lease.1")},
      std::nullopt,
      ContentDigest(std::string(digest))};
  ASSERT_TRUE(receipt.valid());

  std::get<ResolvedComponentModelFrameDTO>(receipt.resolved_frame)
      .compiled_frame_name.clear();
  EXPECT_FALSE(receipt.valid());
}

}  // namespace
}  // namespace manyforge::core
