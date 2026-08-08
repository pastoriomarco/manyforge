#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <manyforge_core/pick_place_candidate.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr char kDigest3[] =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr char kDigest4[] =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";
constexpr char kDigest5[] =
    "sha256:5555555555555555555555555555555555555555555555555555555555555555";

[[nodiscard]] QuaternionDTO yaw_quaternion(const double radians) {
  return {0.0, 0.0, std::sin(radians / 2.0), std::cos(radians / 2.0)};
}

[[nodiscard]] SceneObjectSnapshotDTO box_object(const Vector3DTO dimensions = {0.2, 0.3, 0.4}) {
  return {SceneResourceId("workpiece"),
          {BoxShapeDTO{dimensions}},
          CellFixedSceneObjectPlacementDTO{CellId("cell:test"), Pose3DTO{}},
          std::nullopt,
          {}};
}

[[nodiscard]] PickCandidateSynthesisConfigDTO config() {
  PickCandidateSynthesisConfigDTO value;
  value.translation_step_m = 0.01;
  value.rotation_step_deg = 15.0;
  value.maximum_candidate_count = 128U;
  return value;
}

[[nodiscard]] CollisionWorldLeaseInventory predecessor_projection() {
  auto binding = testing::make_test_projection_binding();
  auto logical_scene = *testing::make_test_logical_scene(binding);
  logical_scene.objects.push_back(box_object());
  auto world = std::make_shared<CollisionWorldSnapshot>();
  world->publisher_id = "pick-candidate-test";
  world->revision = {1U, kDigest3};
  return {binding,
          std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene)),
          10,
          {std::move(world),
           testing::make_test_logical_scene(binding)->configured_collision_policy,
           {}},
          1U,
          1U,
          {}};
}

[[nodiscard]] CollisionWorldLeaseInventory carried_predecessor_projection() {
  auto predecessor = predecessor_projection();
  auto logical_scene = *predecessor.logical_scene;
  const CarriedObjectAttachmentId attachment_id("attachment:workpiece");
  const CarriedObjectAttachmentRevision attachment_revision("attachment-revision:1");
  logical_scene.objects.front().placement =
      CarriedSceneObjectPlacementDTO{attachment_id, attachment_revision};
  logical_scene.carried_object_attachments = {CarriedObjectAttachmentDTO{
      attachment_id,
      attachment_revision,
      SceneResourceId("workpiece"),
      ControllableComponentId("component:test"),
      CommandTargetId("target:test"),
      ComponentSceneFrameRefDTO{ControllableComponentId("component:test"),
                                ModelInstanceId("model:test"), FrameRoleId("frame-role:tcp")},
      Pose3DTO{{0.0, 0.0, 0.2}, {}},
      {CollisionBodyRoleId("collision-body-role:tool")}}};
  predecessor.logical_scene =
      std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene));
  return predecessor;
}

class FakeMotionPreview final : public IMotionPreview {
 public:
  Result<MotionPreviewResultDTO> preview_motion(ExecuteRequestDTO request,
                                                CollisionWorldLeaseInventory projection_inventory,
                                                RobotStateDTO start_state,
                                                const std::uint32_t planning_attempt_index,
                                                std::stop_token) override {
    requests.push_back(request);
    start_states.push_back(start_state);
    projection_bindings.push_back(projection_inventory.projection_binding);
    planning_attempt_indices.push_back(planning_attempt_index);
    if (fail_first_with_start_collision && requests.size() == 1U) {
      return Result<MotionPreviewResultDTO>::Err(ErrorDTO{
          "collision", "fixture start state is in collision", {{"failure_stage", "start_state"}}});
    }

    std::vector<double> final_positions(start_state.positions_rad.size(),
                                        static_cast<double>(requests.size()));
    RobotStateDTO final_state{start_state.joint_names, final_positions, {}};
    TrajectoryDTO trajectory{request.binding.resources,
                             start_state.joint_names,
                             {{0.0, start_state.positions_rad, {}, {}},
                              {static_cast<double>(requests.size()), final_positions, {}, {}}}};
    return Result<MotionPreviewResultDTO>::Ok(MotionPreviewResultDTO{
        std::move(trajectory), std::move(start_state), final_state,
        testing::make_test_controlled_state(request.binding, final_positions),
        Pose3DTO{{0.0, 0.0, static_cast<double>(requests.size())}, {}},
        projection_inventory.projection_binding, projection_inventory.bundle.world->revision, 0U});
  }

  bool fail_first_with_start_collision{};
  std::vector<ExecuteRequestDTO> requests;
  std::vector<RobotStateDTO> start_states;
  std::vector<ProjectionLeaseBindingDTO> projection_bindings;
  std::vector<std::uint32_t> planning_attempt_indices;
};

class FakeSceneCandidatePreview final : public ISceneCandidatePreview {
 public:
  explicit FakeSceneCandidatePreview(CollisionWorldLeaseInventory predecessor)
      : predecessor_(std::move(predecessor)) {}

  Result<SceneCandidatePreviewDTO> PreviewResolvedSceneCandidate(
      const ResolvedSceneCandidatePreviewRequestDTO& request) const override {
    ++call_count;
    last_request = request;
    const auto* attach = std::get_if<AttachCarriedObjectRequestDTO>(&request.request.operation);
    const auto* detach = std::get_if<DetachCarriedObjectRequestDTO>(&request.request.operation);
    if ((attach == nullptr) == (detach == nullptr) || !request.frame_resolution.has_value()) {
      return Result<SceneCandidatePreviewDTO>::Err(
          ErrorDTO{"invalid_request", "fixture candidate is incomplete", {}});
    }

    auto logical_scene = *predecessor_.logical_scene;
    auto proposed = predecessor_.projection_binding.projection_revision;
    ++proposed.scene_revision.revision_id;
    proposed.scene_revision.content_hash = ContentDigest(kDigest4);
    logical_scene.revision = proposed.scene_revision;
    ++logical_scene.transaction_sequence;
    ++logical_scene.topology_epoch;
    auto& selected = logical_scene.objects.front();
    std::optional<ResolvedSceneOperationDTO> resolved_operation;
    if (attach != nullptr) {
      const CarriedObjectAttachmentRevision attachment_revision("attachment-revision:1");
      selected.placement =
          CarriedSceneObjectPlacementDTO{attach->carried_object_attachment_id, attachment_revision};
      const auto& resolved =
          std::get<ResolvedComponentModelFrameDTO>(request.frame_resolution->resolved_frame);
      CarriedObjectAttachmentDTO attachment{
          attach->carried_object_attachment_id,
          attachment_revision,
          attach->scene_resource_id,
          attach->controllable_component_id,
          attach->command_target_id,
          ComponentSceneFrameRefDTO{attach->controllable_component_id, resolved.model_instance_id,
                                    attach->frame_role_id},
          Pose3DTO{},
          attach->touch_body_roles};
      logical_scene.carried_object_attachments = {attachment};
      resolved_operation.emplace(ResolvedAttachCarriedObjectDTO{selected, std::move(attachment)});
    } else {
      const auto attachment = logical_scene.carried_object_attachments.front();
      selected.placement = CellFixedSceneObjectPlacementDTO{logical_scene.cell_id, Pose3DTO{}};
      logical_scene.carried_object_attachments.clear();
      resolved_operation.emplace(
          ResolvedDetachCarriedObjectDTO{selected, detach->carried_object_attachment_id,
                                         attachment.carried_object_attachment_revision});
    }
    auto snapshot = std::make_shared<const LogicalSceneSnapshotDTO>(std::move(logical_scene));
    SceneCandidatePreviewDTO candidate{ContentDigest(kDigest3),
                                       predecessor_.projection_binding.projection_revision,
                                       proposed,
                                       std::move(snapshot),
                                       std::move(*resolved_operation),
                                       predecessor_.bundle.policy.value(),
                                       SceneUpdateStatusDTO::kGeometryChanged,
                                       ContentDigest(kDigest5)};
    if (!candidate.valid()) {
      return Result<SceneCandidatePreviewDTO>::Err(
          ErrorDTO{"invariant_violation", "fixture candidate is invalid", {}});
    }
    return Result<SceneCandidatePreviewDTO>::Ok(std::move(candidate));
  }

  mutable std::size_t call_count{};
  mutable std::optional<ResolvedSceneCandidatePreviewRequestDTO> last_request;

 private:
  CollisionWorldLeaseInventory predecessor_;
};

class FakeSceneCandidateProjection final : public ISceneCandidateProjection {
 public:
  explicit FakeSceneCandidateProjection(CollisionWorldLeaseInventory predecessor)
      : predecessor_(std::move(predecessor)) {}

  Result<SceneFrameResolutionReceiptDTO> ResolveCandidateSceneFrame(
      const CandidateSceneFrameResolutionQueryDTO& query) const override {
    ++frame_call_count;
    last_frame_query = query;
    const auto& selector = std::get<TargetCarrierSceneFrameRefDTO>(query.selector);
    return Result<SceneFrameResolutionReceiptDTO>::Ok(SceneFrameResolutionReceiptDTO{
        ResolvedComponentModelFrameDTO{selector.controllable_component_id,
                                       ModelInstanceId("model:test"), selector.frame_role_id, "tcp",
                                       ModelRevision("model-revision:test")},
        Pose3DTO{},
        {},
        query.projection_binding,
        ContentDigest(kDigest4),
        ContentDigest(kDigest5)});
  }

  Result<CollisionWorldLeaseInventory> ProjectSceneCandidate(
      const SceneCandidatePreviewDTO& candidate,
      const ProjectionLeaseBindingDTO& predecessor_binding,
      ProjectionLeaseId candidate_lease_id) const override {
    ++projection_call_count;
    last_predecessor_binding = predecessor_binding;
    last_candidate_lease_id = candidate_lease_id;
    if (!candidate.valid()) {
      return Result<CollisionWorldLeaseInventory>::Err(
          ErrorDTO{"invalid_request", "fixture candidate is invalid", {}});
    }
    auto projected = predecessor_;
    projected.projection_binding = {candidate.proposed_projection_revision,
                                    std::move(candidate_lease_id)};
    projected.logical_scene = candidate.logical_scene;
    projected.bundle.policy = candidate.effective_collision_policy;
    return Result<CollisionWorldLeaseInventory>::Ok(std::move(projected));
  }

  mutable std::size_t frame_call_count{};
  mutable std::size_t projection_call_count{};
  mutable std::optional<CandidateSceneFrameResolutionQueryDTO> last_frame_query;
  mutable std::optional<ProjectionLeaseBindingDTO> last_predecessor_binding;
  mutable std::optional<ProjectionLeaseId> last_candidate_lease_id;

 private:
  CollisionWorldLeaseInventory predecessor_;
};

[[nodiscard]] PickCandidatePlanningRequestDTO pick_request(
    PickCandidateGrid grid, CollisionWorldLeaseInventory predecessor) {
  auto approach = testing::make_test_execute_request({"joint1", "joint2"}, "run:test");
  approach.move_id = "approach";
  approach.motion = PoseGoalDTO{Pose3DTO{}};
  auto descend = approach;
  descend.move_id = "descend";
  descend.motion = CartesianPathDTO{};
  descend.contact_policy_id = "pick.contact";
  descend.resolved_contact_grasp = ResolvedContactGraspBindingDTO{
      ContactPolicyId("pick.contact"),
      ContactPolicyRevision("contact-policy-revision:1"),
      RuntimeObjectId("selected_object"),
      SceneResourceId("workpiece"),
      {"component/component:test/model/model:test/body/collision-body-role:tool/collision/0"},
      0.001,
      0.8};
  auto retreat = approach;
  retreat.move_id = "retreat";
  retreat.motion = CartesianPathDTO{};
  const TargetCarrierSceneFrameRefDTO carrier{approach.binding.controllable_component_id,
                                              approach.binding.command_target_id,
                                              FrameRoleId("frame-role:tcp")};
  return PickCandidatePlanningRequestDTO{
      std::move(grid),
      std::move(approach),
      std::move(descend),
      std::move(retreat),
      RobotStateDTO{{"joint1", "joint2"}, {0.0, 0.0}, {}},
      predecessor,
      ResolvedSceneCandidatePreviewRequestDTO{
          SceneTransactionRequestDTO{
              PublicRequestContextDTO{RequestId("pick.candidate.attach"),
                                      RequestOwnerEpoch("pick.owner.epoch"), "pick_candidate_test",
                                      std::nullopt},
              {},
              predecessor.projection_binding.projection_revision,
              AttachCarriedObjectRequestDTO{CarriedObjectAttachmentId("attachment:workpiece"),
                                            SceneResourceId("workpiece"),
                                            carrier.controllable_component_id,
                                            carrier.command_target_id,
                                            carrier.frame_role_id,
                                            {CollisionBodyRoleId("collision-body-role:tool")},
                                            {}}},
          std::nullopt},
      carrier,
      "pick.candidate"};
}

[[nodiscard]] PlacePlanningRequestDTO place_request(CollisionWorldLeaseInventory predecessor) {
  auto approach = testing::make_test_execute_request({"joint1", "joint2"}, "run:test");
  approach.move_id = "place.approach";
  approach.motion = PoseGoalDTO{Pose3DTO{{0.0, 0.0, 0.3}, {}}};
  auto descend = approach;
  descend.move_id = "place.descend";
  descend.motion = CartesianPathDTO{{Pose3DTO{{0.0, 0.0, 0.3}, {}}, Pose3DTO{{0.0, 0.0, 0.2}, {}}}};
  auto retreat = approach;
  retreat.move_id = "place.retreat";
  retreat.motion = CartesianPathDTO{{Pose3DTO{{0.0, 0.0, 0.2}, {}}, Pose3DTO{{0.0, 0.0, 0.3}, {}}}};
  const TargetCarrierSceneFrameRefDTO carrier{approach.binding.controllable_component_id,
                                              approach.binding.command_target_id,
                                              FrameRoleId("frame-role:tcp")};
  return PlacePlanningRequestDTO{
      SceneResourceId("workpiece"),
      std::move(approach),
      std::move(descend),
      std::move(retreat),
      RobotStateDTO{{"joint1", "joint2"}, {0.0, 0.0}, {}},
      predecessor,
      ResolvedSceneCandidatePreviewRequestDTO{
          SceneTransactionRequestDTO{PublicRequestContextDTO{RequestId("place.candidate.detach"),
                                                             RequestOwnerEpoch("place.owner.epoch"),
                                                             "place_candidate_test", std::nullopt},
                                     {},
                                     predecessor.projection_binding.projection_revision,
                                     DetachCarriedObjectRequestDTO{
                                         CarriedObjectAttachmentId("attachment:workpiece"),
                                         CarriedObjectAttachmentRevision("attachment-revision:1"),
                                         Pose3DTO{}, CellSceneFrameRefDTO{CellId("cell:test")}}},
          std::nullopt},
      carrier,
      "place.candidate"};
}

TEST(PickCandidateGrid, PreservesDeterministicPickThenApproachOrdering) {
  auto value = config();
  value.pick_offset_envelope.position_min_m = {-0.01, 0.0, 0.0};
  value.pick_offset_envelope.position_max_m = {0.01, 0.0, 0.0};
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.06};
  const Pose3DTO object_pose{{1.0, 2.0, 0.5}, {0.0, 0.0, 0.0, 1.0}};

  auto grid = PickCandidateGrid::Create(box_object(), object_pose, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_EQ(grid.value().size(), 6U);
  auto first = grid.value().candidate(0U);
  auto second = grid.value().candidate(1U);
  auto third = grid.value().candidate(2U);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(third.ok());
  EXPECT_EQ(first.value().candidate_index, 1U);
  EXPECT_NEAR(first.value().pick_pose.position_m.x_m, 0.99, 1e-12);
  EXPECT_NEAR(first.value().pick_pose.position_m.z_m, 0.7, 1e-12);
  EXPECT_NEAR(first.value().approach_pose.position_m.z_m, 0.75, 1e-12);
  EXPECT_NEAR(second.value().pick_pose.position_m.x_m, 0.99, 1e-12);
  EXPECT_NEAR(second.value().approach_pose.position_m.z_m, 0.76, 1e-12);
  EXPECT_NEAR(third.value().pick_pose.position_m.x_m, 1.0, 1e-12);
  ASSERT_EQ(first.value().descend_path.waypoints.size(), 2U);
  ASSERT_EQ(first.value().retreat_path.waypoints.size(), 2U);
  EXPECT_EQ(first.value().descend_path.waypoints.front(), first.value().approach_pose);
  EXPECT_EQ(first.value().descend_path.waypoints.back(), first.value().pick_pose);
  EXPECT_EQ(first.value().retreat_path.waypoints.front(), first.value().pick_pose);
  EXPECT_EQ(first.value().retreat_path.waypoints.back(), first.value().retreat_pose);
}

TEST(PickCandidateGrid, DefaultApproachEnvelopePreservesHistoricalFiveToTenCentimetres) {
  auto value = config();

  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_EQ(grid.value().size(), 6U);
  auto first = grid.value().candidate(0U);
  auto last = grid.value().candidate(5U);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(last.ok());
  EXPECT_NEAR(first.value().approach_pose.position_m.z_m - first.value().pick_pose.position_m.z_m,
              0.05, 1e-12);
  EXPECT_NEAR(last.value().approach_pose.position_m.z_m - last.value().pick_pose.position_m.z_m,
              0.10, 1e-12);
}

TEST(PickCandidateGrid, VerticalPickPreservesObjectHeadingAndIgnoresRpyGrid) {
  auto value = config();
  value.force_vertical_pick = true;
  value.pick_offset_envelope.orientation_rpy_min_deg = {-90.0, -45.0, -30.0};
  value.pick_offset_envelope.orientation_rpy_max_deg = {90.0, 45.0, 30.0};
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  const Pose3DTO object_pose{{0.0, 0.0, 0.4}, yaw_quaternion(kPi / 3.0)};

  auto grid = PickCandidateGrid::Create(box_object(), object_pose, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_EQ(grid.value().size(), 1U);
  auto candidate = grid.value().candidate(0U);
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  const auto& orientation = candidate.value().pick_pose.orientation;
  const auto rotate = [&](const std::array<double, 3U>& vector) {
    const double xx = orientation.x * orientation.x;
    const double yy = orientation.y * orientation.y;
    const double zz = orientation.z * orientation.z;
    const double xy = orientation.x * orientation.y;
    const double xz = orientation.x * orientation.z;
    const double yz = orientation.y * orientation.z;
    const double wx = orientation.w * orientation.x;
    const double wy = orientation.w * orientation.y;
    const double wz = orientation.w * orientation.z;
    return std::array<double, 3U>{(1.0 - 2.0 * (yy + zz)) * vector[0] +
                                      2.0 * (xy - wz) * vector[1] + 2.0 * (xz + wy) * vector[2],
                                  2.0 * (xy + wz) * vector[0] +
                                      (1.0 - 2.0 * (xx + zz)) * vector[1] +
                                      2.0 * (yz - wx) * vector[2],
                                  2.0 * (xz - wy) * vector[0] + 2.0 * (yz + wx) * vector[1] +
                                      (1.0 - 2.0 * (xx + yy)) * vector[2]};
  };
  const auto tool_x = rotate({1.0, 0.0, 0.0});
  const auto tool_z = rotate({0.0, 0.0, 1.0});
  EXPECT_NEAR(tool_x[0], std::cos(kPi / 3.0), 1e-12);
  EXPECT_NEAR(tool_x[1], std::sin(kPi / 3.0), 1e-12);
  EXPECT_NEAR(tool_x[2], 0.0, 1e-12);
  EXPECT_NEAR(tool_z[0], 0.0, 1e-12);
  EXPECT_NEAR(tool_z[1], 0.0, 1e-12);
  EXPECT_NEAR(tool_z[2], -1.0, 1e-12);
  EXPECT_EQ(candidate.value().approach_pose.orientation, candidate.value().pick_pose.orientation);
}

TEST(PickCandidateGrid, AppliesMinimumTcpHeightToEveryLinkedPose) {
  auto value = config();
  value.min_tcp_z_m = 0.8;
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};

  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{{0.0, 0.0, 0.1}, {}}, value);

  ASSERT_TRUE(grid.ok()) << grid.error().message;
  auto candidate = grid.value().candidate(0U);
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  EXPECT_TRUE(candidate.value().min_tcp_z_applied);
  EXPECT_DOUBLE_EQ(candidate.value().pick_pose.position_m.z_m, 0.8);
  EXPECT_DOUBLE_EQ(candidate.value().approach_pose.position_m.z_m, 0.85);
  EXPECT_DOUBLE_EQ(candidate.value().descend_path.waypoints.front().position_m.z_m, 0.85);
  EXPECT_DOUBLE_EQ(candidate.value().descend_path.waypoints.back().position_m.z_m, 0.8);
}

TEST(PickCandidateGrid, UsesTheDeclaredResolvedOrCandidateTcpOffsetBasis) {
  auto resolved = config();
  resolved.base_tool_orientation_rpy_deg = {0.0, 0.0, 0.0};
  resolved.pick_offset_envelope.position_min_m = {0.1, 0.0, 0.0};
  resolved.pick_offset_envelope.position_max_m = {0.1, 0.0, 0.0};
  resolved.approach_offset_envelope.position_min_m = {};
  resolved.approach_offset_envelope.position_max_m = {};
  resolved.pick_offset_basis.resolved_orientation_in_base = yaw_quaternion(kPi / 2.0);
  auto candidate_tcp = resolved;
  candidate_tcp.pick_offset_basis.use_candidate_tcp_orientation = true;

  auto resolved_grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, resolved);
  auto candidate_grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, candidate_tcp);

  ASSERT_TRUE(resolved_grid.ok()) << resolved_grid.error().message;
  ASSERT_TRUE(candidate_grid.ok()) << candidate_grid.error().message;
  auto resolved_pose = resolved_grid.value().candidate(0U);
  auto candidate_pose = candidate_grid.value().candidate(0U);
  ASSERT_TRUE(resolved_pose.ok());
  ASSERT_TRUE(candidate_pose.ok());
  EXPECT_NEAR(resolved_pose.value().pick_pose.position_m.x_m, 0.0, 1e-12);
  EXPECT_NEAR(resolved_pose.value().pick_pose.position_m.y_m, 0.1, 1e-12);
  EXPECT_NEAR(candidate_pose.value().pick_pose.position_m.x_m, 0.1, 1e-12);
  EXPECT_NEAR(candidate_pose.value().pick_pose.position_m.y_m, 0.0, 1e-12);
}

TEST(PickCandidateGrid, RejectsInvalidOrOversizedGridsBeforePlanning) {
  auto reversed = config();
  reversed.pick_offset_envelope.position_min_m = {1.0, 0.0, 0.0};
  reversed.pick_offset_envelope.position_max_m = {0.0, 0.0, 0.0};
  auto invalid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, reversed);
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().details.at("reason"), "pick_candidate_grid_axis_reversed");

  auto oversized = config();
  oversized.maximum_candidate_count = 100U;
  oversized.pick_offset_envelope.position_max_m = {0.1, 0.1, 0.0};
  oversized.approach_offset_envelope.position_min_m = {};
  oversized.approach_offset_envelope.position_max_m = {};
  auto rejected = PickCandidateGrid::Create(box_object(), Pose3DTO{}, oversized);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "pick_candidate_count_exceeded");
}

TEST(AddressedPickCandidatePlanner,
     ChainsExactStatesAndUsesContactOnlyForDescendAndAttachedRetreat) {
  auto predecessor = predecessor_projection();
  FakeMotionPreview motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickCandidatePlanner planner(motion, scene, projection);
  auto value = config();
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, value);
  ASSERT_TRUE(grid.ok()) << grid.error().message;

  auto result = planner.Search(pick_request(std::move(grid.value()), std::move(predecessor)));

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().selected.has_value());
  EXPECT_EQ(result.value().attempted_candidate_count, 1U);
  ASSERT_EQ(motion.requests.size(), 3U);
  EXPECT_FALSE(motion.requests[0].resolved_contact_grasp.has_value());
  EXPECT_TRUE(motion.requests[0].contact_policy_id.empty());
  EXPECT_TRUE(motion.requests[1].resolved_contact_grasp.has_value());
  EXPECT_EQ(motion.requests[1].contact_policy_id, "pick.contact");
  EXPECT_FALSE(motion.requests[2].resolved_contact_grasp.has_value());
  EXPECT_TRUE(motion.requests[2].contact_policy_id.empty());
  EXPECT_EQ(motion.start_states[0].positions_rad, std::vector<double>({0.0, 0.0}));
  EXPECT_EQ(motion.start_states[1].positions_rad, std::vector<double>({1.0, 1.0}));
  EXPECT_EQ(motion.start_states[2].positions_rad, std::vector<double>({2.0, 2.0}));
  EXPECT_EQ(motion.projection_bindings[0], motion.projection_bindings[1]);
  EXPECT_NE(motion.projection_bindings[1], motion.projection_bindings[2]);
  EXPECT_EQ(scene.call_count, 1U);
  ASSERT_TRUE(scene.last_request.has_value());
  EXPECT_TRUE(scene.last_request->frame_resolution.has_value());
  EXPECT_EQ(projection.frame_call_count, 1U);
  EXPECT_EQ(projection.projection_call_count, 1U);
  ASSERT_TRUE(projection.last_frame_query.has_value());
  EXPECT_EQ(projection.last_frame_query->predicted_state.positions,
            testing::make_test_controlled_state(motion.requests[1].binding, {2.0, 2.0}).positions);
}

TEST(AddressedPickCandidatePlanner, StopsTheGridOnRoboPlanStartStateCollisionEvidence) {
  auto predecessor = predecessor_projection();
  FakeMotionPreview motion;
  motion.fail_first_with_start_collision = true;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPickCandidatePlanner planner(motion, scene, projection);
  auto value = config();
  value.pick_offset_envelope.position_min_m = {-0.01, 0.0, 0.0};
  value.pick_offset_envelope.position_max_m = {0.01, 0.0, 0.0};
  value.approach_offset_envelope.position_min_m = {0.0, 0.0, 0.05};
  value.approach_offset_envelope.position_max_m = {0.0, 0.0, 0.05};
  auto grid = PickCandidateGrid::Create(box_object(), Pose3DTO{}, value);
  ASSERT_TRUE(grid.ok()) << grid.error().message;
  ASSERT_GT(grid.value().size(), 1U);

  auto result = planner.Search(pick_request(std::move(grid.value()), std::move(predecessor)));

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(result.value().selected.has_value());
  EXPECT_TRUE(result.value().stopped_on_invariant_failure);
  EXPECT_EQ(result.value().attempted_candidate_count, 1U);
  EXPECT_EQ(motion.requests.size(), 1U);
  EXPECT_EQ(scene.call_count, 0U);
  EXPECT_EQ(projection.frame_call_count, 0U);
  EXPECT_EQ(projection.projection_call_count, 0U);
}

TEST(AddressedPlacePlanner,
     KeepsTheObjectCarriedUntilDetachThenRetreatsAgainstTheDetachedSuccessor) {
  auto predecessor = carried_predecessor_projection();
  FakeMotionPreview motion;
  FakeSceneCandidatePreview scene(predecessor);
  FakeSceneCandidateProjection projection(predecessor);
  AddressedPlacePlanner planner(motion, scene, projection);

  auto result = planner.Plan(place_request(std::move(predecessor)));

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(result.value().selected.has_value());
  ASSERT_EQ(motion.requests.size(), 3U);
  EXPECT_EQ(motion.start_states[0].positions_rad, std::vector<double>({0.0, 0.0}));
  EXPECT_EQ(motion.start_states[1].positions_rad, std::vector<double>({1.0, 1.0}));
  EXPECT_EQ(motion.start_states[2].positions_rad, std::vector<double>({2.0, 2.0}));
  EXPECT_EQ(motion.projection_bindings[0], motion.projection_bindings[1]);
  EXPECT_NE(motion.projection_bindings[1], motion.projection_bindings[2]);
  EXPECT_EQ(scene.call_count, 1U);
  ASSERT_TRUE(scene.last_request.has_value());
  ASSERT_TRUE(scene.last_request->frame_resolution.has_value());
  const auto* detach =
      std::get_if<DetachCarriedObjectRequestDTO>(&scene.last_request->request.operation);
  ASSERT_NE(detach, nullptr);
  EXPECT_TRUE(std::holds_alternative<ComponentSceneFrameRefDTO>(detach->frame_ref));
  const Pose3DTO expected_release_pose{{0.0, 0.0, 0.2}, {}};
  EXPECT_EQ(detach->pose, expected_release_pose);
  EXPECT_EQ(projection.frame_call_count, 1U);
  EXPECT_EQ(projection.projection_call_count, 1U);
  ASSERT_TRUE(projection.last_frame_query.has_value());
  EXPECT_EQ(projection.last_frame_query->predicted_state.positions,
            testing::make_test_controlled_state(motion.requests[1].binding, {2.0, 2.0}).positions);
  EXPECT_TRUE(std::holds_alternative<PoseGoalDTO>(result.value().selected->approach_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().selected->descend_motion));
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(result.value().selected->retreat_motion));
}

}  // namespace
}  // namespace manyforge::core
