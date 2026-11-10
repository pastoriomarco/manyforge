#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>

#include <manyforge_deployment/model_source.hpp>
#include <manyforge_roboplan/retained_tool_context.hpp>

namespace manyforge::roboplan_backend {
namespace {

// A wrist joint, the flange, a tool mounted 10 mm out turned 90 degrees
// about z, its TCP 100 mm further out, and a moving finger.
constexpr const char *kCellUrdf = R"(<?xml version="1.0"?>
<robot name="cell">
  <link name="link5"/><link name="flange"/><link name="tool_mount"/>
  <link name="tool_tcp"/><link name="finger"/><link name="finger_tip"/>
  <joint name="j6" type="revolute"><parent link="link5"/><child link="flange"/>
    <axis xyz="0 0 1"/><limit lower="-3" upper="3" effort="1" velocity="1"/></joint>
  <joint name="mount" type="fixed"><parent link="flange"/><child link="tool_mount"/>
    <origin xyz="0 0 0.01" rpy="0 0 1.5707963267948966"/></joint>
  <joint name="tcp" type="fixed"><parent link="tool_mount"/><child link="tool_tcp"/>
    <origin xyz="0 0 0.1"/></joint>
  <joint name="finger" type="prismatic"><parent link="tool_mount"/><child link="finger"/>
    <axis xyz="1 0 0"/><limit lower="0" upper="0.01" effort="1" velocity="1"/></joint>
  <joint name="tip" type="fixed"><parent link="finger"/><child link="finger_tip"/></joint>
</robot>
)";

const core::ControllableComponentId kRobot("lite6_robot");
const core::ControllableComponentId kTool("lite_gripper");
const core::CommandTargetId kGripper("gripper");

[[nodiscard]] deployment::UrdfDocumentIr CellUrdf() {
  return deployment::parse_model_documents_for_projection(kCellUrdf, std::nullopt).urdf;
}

[[nodiscard]] RetainedToolContextFrames Frames(double tool_mass_kg = 0.5) {
  RetainedToolContextFrames frames;
  frames.flange_link = "flange";
  frames.end_effector_tcp_frame = "tool_tcp";
  frames.carriers = {kRobot, kTool};
  frames.carrier_frames = {
      {{kTool, kGripper, core::FrameRoleId("tcp")}, "tool_tcp"},
      {{kTool, kGripper, core::FrameRoleId("finger")}, "finger_tip"}};
  // The tool's centre of mass 20 mm along its own x, 40 mm out.
  frames.tool_payloads = {{"tool_mount", {tool_mass_kg, {0.02, 0.0, 0.04}}}};
  return frames;
}

/// One object of `mass_kg` attached 20 mm beyond the tool's TCP.
[[nodiscard]] std::shared_ptr<const core::LogicalSceneSnapshotDTO>
Carrying(double mass_kg, core::ControllableComponentId carrier = kTool,
         std::string frame_role = "tcp") {
  core::SceneObjectSnapshotDTO object{
      core::SceneResourceId("box"),
      {},
      core::CarriedSceneObjectPlacementDTO{core::CarriedObjectAttachmentId("attach.box"),
                                           core::CarriedObjectAttachmentRevision("r1")},
      std::nullopt,
      {},
      mass_kg};
  core::CarriedObjectAttachmentDTO attachment{
      core::CarriedObjectAttachmentId("attach.box"),
      core::CarriedObjectAttachmentRevision("r1"),
      core::SceneResourceId("box"),
      carrier,
      kGripper,
      core::ComponentSceneFrameRefDTO{carrier, core::ModelInstanceId("tool_model"),
                                      core::FrameRoleId(frame_role)},
      {{0.0, 0.0, 0.02}, {0.0, 0.0, 0.0, 1.0}},
      {},
      std::nullopt};
  return std::make_shared<const core::LogicalSceneSnapshotDTO>(core::LogicalSceneSnapshotDTO{
      core::SceneRevision{}, core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{},
      {std::move(object)}, {std::move(attachment)}});
}

void ExpectNear(const std::array<double, 3> &actual, const std::array<double, 3> &expected) {
  for (std::size_t axis = 0; axis < 3U; ++axis)
    EXPECT_NEAR(actual[axis], expected[axis], 1e-12) << "axis " << axis;
}

// The default TCP through the mount, and the tool's own payload at
// the mount, both in the flange frame.
TEST(RetainedToolContext, DefaultTcpAndToolPayloadInTheFlangeFrame) {
  const RetainedToolContextSource source(CellUrdf(), Frames(), nullptr);
  const auto context = source.Compose(std::nullopt);
  ASSERT_TRUE(context.ok()) << context.error().message;
  const auto &tcp = context.value().tcp_pose_in_flange;
  EXPECT_NEAR(tcp.position_m.x_m, 0.0, 1e-12);
  EXPECT_NEAR(tcp.position_m.y_m, 0.0, 1e-12);
  EXPECT_NEAR(tcp.position_m.z_m, 0.11, 1e-12);
  EXPECT_NEAR(tcp.orientation.z, std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(tcp.orientation.w, std::sqrt(0.5), 1e-12);
  EXPECT_DOUBLE_EQ(context.value().payload_mass_kg, 0.5);
  // The tool's x is the flange's y after the 90-degree mount.
  ExpectNear(context.value().payload_center_of_mass_m, {0.0, 0.02, 0.05});
}

// A TCP switch changes the TCP only; a moving frame is refused.
TEST(RetainedToolContext, TcpSwitchChangesTheTcpAndAMovingFrameIsRefused) {
  const RetainedToolContextSource source(CellUrdf(), Frames(), nullptr);
  const auto tool_tcp = source.Compose(std::nullopt);
  const auto flange = source.Compose(std::string("flange"));
  ASSERT_TRUE(tool_tcp.ok());
  ASSERT_TRUE(flange.ok());
  EXPECT_EQ(flange.value().tcp_pose_in_flange, core::Pose3DTO{});
  EXPECT_EQ(flange.value().payload_mass_kg, tool_tcp.value().payload_mass_kg);
  EXPECT_EQ(flange.value().payload_center_of_mass_m,
            tool_tcp.value().payload_center_of_mass_m);
  for (const std::string frame : {"finger_tip", "link5", "no_such_link"}) {
    const auto refused = source.Compose(frame);
    ASSERT_FALSE(refused.ok()) << frame;
    EXPECT_EQ(refused.error().details.at("reason"), "tool_context_frame_not_rigid");
    EXPECT_EQ(refused.error().details.at("frame"), frame);
  }
}

TEST(RetainedToolContext, PayloadCompositionDoesNotDependOnMovingTcp) {
  auto frames = Frames();
  frames.end_effector_tcp_frame = "finger_tip";
  const RetainedToolContextSource source(CellUrdf(), frames, nullptr);
  ASSERT_FALSE(source.Compose(std::nullopt).ok());
  const auto scene = Carrying(1.0);
  const auto payload = source.ComposePayload(*scene);
  ASSERT_TRUE(payload.ok());
  EXPECT_DOUBLE_EQ(payload.value().payload_mass_kg, 1.5);
}

// With no End-effector mark the robot has no TCP at all: its
// context is at the flange (a zero offset, the payload kept); an explicit
// TCP still composes.
TEST(RetainedToolContext, NoEndEffectorMarkIsTheFlangeAndAnExplicitTcpStillComposes) {
  auto frames = Frames();
  frames.end_effector_tcp_frame.reset();
  const RetainedToolContextSource source(CellUrdf(), frames, nullptr);
  const auto none = source.Compose(std::nullopt);
  ASSERT_TRUE(none.ok()) << none.error().message;
  EXPECT_EQ(none.value().tcp_pose_in_flange, core::Pose3DTO{});
  EXPECT_DOUBLE_EQ(none.value().payload_mass_kg, 0.5);
  const auto tool_tcp = source.Compose(std::string("tool_tcp"));
  ASSERT_TRUE(tool_tcp.ok()) << tool_tcp.error().message;
  EXPECT_NEAR(tool_tcp.value().tcp_pose_in_flange.position_m.z_m, 0.11, 1e-12);
  const auto flange = source.Compose(std::string("flange"));
  ASSERT_TRUE(flange.ok()) << flange.error().message;
  EXPECT_EQ(flange.value().tcp_pose_in_flange, core::Pose3DTO{});
}

// An attached object adds its mass at its attach pose; a detach
// (the next scene without it) restores the tool's payload exactly.
TEST(RetainedToolContext, AttachAddsTheObjectAtItsPoseAndDetachRemovesIt) {
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> scene;
  const RetainedToolContextSource source(CellUrdf(), Frames(), [&] { return scene; });
  const auto empty = source.Compose(std::nullopt);
  scene = Carrying(1.0);
  const auto carrying = source.Compose(std::nullopt);
  ASSERT_TRUE(empty.ok());
  ASSERT_TRUE(carrying.ok()) << carrying.error().message;
  EXPECT_EQ(carrying.value().tcp_pose_in_flange, empty.value().tcp_pose_in_flange);
  EXPECT_DOUBLE_EQ(carrying.value().payload_mass_kg, 1.5);
  // 0.5 kg at (0, 0.02, 0.05) and 1 kg at (0, 0, 0.13).
  ExpectNear(carrying.value().payload_center_of_mass_m,
             {0.0, 0.01 / 1.5, (0.025 + 0.13) / 1.5});
  scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(core::LogicalSceneSnapshotDTO{
      core::SceneRevision{}, core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{},
      {}, {}});
  EXPECT_EQ(source.Compose(std::nullopt).value(), empty.value());
  // A massless object, or one held by another robot, adds nothing.
  scene = Carrying(0.0);
  EXPECT_EQ(source.Compose(std::nullopt).value(), empty.value());
  scene = Carrying(2.0, core::ControllableComponentId("other_gripper"));
  EXPECT_EQ(source.Compose(std::nullopt).value(), empty.value());
  // An object on a moving frame, or on a frame the tool lacks, is refused.
  scene = Carrying(1.0, kTool, "finger");
  const auto moving = source.Compose(std::nullopt);
  ASSERT_FALSE(moving.ok());
  EXPECT_EQ(moving.error().details.at("reason"), "tool_context_frame_not_rigid");
  scene = Carrying(1.0, kTool, "cup");
  const auto unknown = source.Compose(std::nullopt);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"), "tool_context_frame_unknown");
}

TEST(RetainedToolContext, ExplicitCommittedSnapshotDoesNotReadProjection) {
  const auto provisional = Carrying(2.0);
  const auto committed = Carrying(1.0);
  int reads = 0;
  const RetainedToolContextSource source(CellUrdf(), Frames(), [&] {
    ++reads;
    return provisional;
  });
  const auto result = source.Compose(std::nullopt, *committed);
  ASSERT_TRUE(result.ok());
  EXPECT_DOUBLE_EQ(result.value().payload_mass_kg, 1.5);
  EXPECT_EQ(reads, 0);
}

TEST(RetainedToolContext, NoToolPayloadIsZeroAndAMovingMountIsRefusedAtBind) {
  const RetainedToolContextSource massless(CellUrdf(), Frames(0.0), nullptr);
  const auto context = massless.Compose(std::nullopt);
  ASSERT_TRUE(context.ok());
  EXPECT_EQ(context.value().payload_mass_kg, 0.0);
  EXPECT_EQ(context.value().payload_center_of_mass_m, (std::array<double, 3>{}));
  auto frames = Frames();
  frames.tool_payloads = {{"finger", {0.5, {0.0, 0.0, 0.0}}}};
  EXPECT_THROW(RetainedToolContextSource(CellUrdf(), frames, nullptr),
               std::invalid_argument);
}

TEST(RetainedToolContext, UrdfOriginIsFixedAxisRollPitchYaw) {
  deployment::UrdfOriginIr origin;
  origin.xyz_m = {0.1, 0.2, 0.3};
  origin.rpy_rad = {0.0, 0.0, std::numbers::pi / 2.0};
  const auto pose = urdf_origin_pose(origin);
  EXPECT_EQ(pose.position_m, (core::Vector3DTO{0.1, 0.2, 0.3}));
  EXPECT_NEAR(pose.orientation.z, std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(pose.orientation.w, std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(pose.orientation.x, 0.0, 1e-12);
  EXPECT_NEAR(pose.orientation.y, 0.0, 1e-12);
}

} // namespace
} // namespace manyforge::roboplan_backend
