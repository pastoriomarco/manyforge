#include <gtest/gtest.h>

#include <manyforge_core/pick_place_sequence.hpp>
#include <manyforge_core/types/command_schema.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr char kDigest1[] =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr char kDigest2[] =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr char kDigest3[] =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr char kDigest4[] =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";

[[nodiscard]] ProjectionRevisionDTO predecessor() {
  return {RetainedBundleId(kDigest1), GraphRevision(kDigest2),
          SceneRevision{SceneId(kDigest3), 7U, ContentDigest(kDigest1)}};
}

[[nodiscard]] ProjectionRevisionDTO successor() {
  return {RetainedBundleId(kDigest1), GraphRevision(kDigest2),
          SceneRevision{SceneId(kDigest3), 8U, ContentDigest(kDigest4)}};
}

[[nodiscard]] MotionParticipantIntentDTO motion(
    std::string id, const bool grasp) {
  return MotionParticipantIntentDTO{
      MoveId(std::move(id)),
      FrameRoleId("base"),
      FrameRoleId("tcp"),
      TolerancesDTO{0.01, 0.001, 0.01},
      SpeedProfileDTO{0.5, 0.5, std::nullopt},
      TimeoutsDTO{1.0, 2.0},
      CacheMode::kDefault,
      PlanningProfileDTO::kFirstFeasible,
      MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      PoseGoalDTO{Pose3DTO{}},
      grasp ? std::optional<ContactPolicyId>{ContactPolicyId("pick.contact")}
            : std::nullopt,
      {},
      {},
      grasp ? std::vector<RuntimeObjectReferenceBindingDTO>{
                  {RuntimeObjectId("selected_object"),
                   SceneResourceId("part")}}
            : std::vector<RuntimeObjectReferenceBindingDTO>{}};
}

[[nodiscard]] OperationIntentDTO motion_operation(std::string request_id,
                                                  std::string move_id,
                                                  const bool grasp = false) {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)),
                              RequestOwnerEpoch("operation.epoch"),
                              "pick_place_test", std::nullopt},
      ProgramId("program"),
      ProgramRevision(kDigest1),
      ProgramBindingId(kDigest2),
      ProgramRunId("run"),
      GraphRevision(kDigest2),
      RetainedBundleId(kDigest1),
      predecessor().scene_revision,
      {OperationParticipantIntentDTO{
          ComponentRoleId("robot"), TargetRoleId("arm"),
          CapabilityId("execution.trajectory.execute"), std::nullopt,
          motion(std::move(move_id), grasp)}},
      OperationCoordinationPolicy::kIndependent,
      {}};
}

[[nodiscard]] OperationIntentDTO tool_operation(std::string request_id) {
  CapabilityCommandIntentDTO command{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
      GripperPositionCommandValueDTO{LinearPositionDTO{0.0}, std::nullopt},
      1.0,
      {}};
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)),
                              RequestOwnerEpoch("operation.epoch"),
                              "pick_place_test", std::nullopt},
      ProgramId("program"),
      ProgramRevision(kDigest1),
      ProgramBindingId(kDigest2),
      ProgramRunId("run"),
      GraphRevision(kDigest2),
      RetainedBundleId(kDigest1),
      predecessor().scene_revision,
      {OperationParticipantIntentDTO{
          ComponentRoleId("robot"), TargetRoleId("gripper"),
          CapabilityId("tool.gripper.command"), std::nullopt,
          std::move(command)}},
      OperationCoordinationPolicy::kIndependent,
      {}};
}

[[nodiscard]] CartesianPathDTO cartesian_path(const double from_z,
                                              const double to_z) {
  CartesianPathDTO path;
  path.waypoints = {Pose3DTO{{0.0, 0.0, from_z}, {}},
                    Pose3DTO{{0.0, 0.0, to_z}, {}}};
  return path;
}

class FakeOperationOwner final : public IAddressedOperationOwner {
 public:
  Result<OperationAcceptedDTO> execute(
      const OperationIntentDTO& intent) override {
    if (active_.has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          ErrorDTO{"execution_rejected", "operation owner is busy",
                   {{"reason", "busy"}}});
    }
    submitted.push_back(intent);
    const auto id = OperationId("operation." + std::to_string(++next_id_));
    active_ = OperationStateDTO{
        id,
        OperationRevision(1U),
        intent.program_binding_id,
        intent.program_run_id,
        OperationCoordinationPolicy::kIndependent,
        OperationStateKind::kExecuting,
        {},
        1,
        std::nullopt,
        std::nullopt,
        std::nullopt};
    return Result<OperationAcceptedDTO>::Ok(OperationAcceptedDTO{
        intent.request_context.request_id, RequestReplayStatusDTO::kAcceptedNew,
        id, OperationRevision(1U), {}, 1});
  }

  Result<OperationControlAcceptedDTO> acknowledge_completed(
      const OperationControlRequestDTO& request) override {
    if (!active_.has_value() ||
        active_->operation_id != request.operation_id ||
        active_->operation_revision != request.expected_operation_revision ||
        active_->state != OperationStateKind::kCompleted) {
      return Result<OperationControlAcceptedDTO>::Err(
          ErrorDTO{"invalid_state_transition", "completion mismatch",
                   {{"reason", "completion_mismatch"}}});
    }
    acknowledged.push_back(request.operation_id);
    const auto accepted = OperationControlAcceptedDTO{
        request.request_context.request_id,
        RequestReplayStatusDTO::kAcceptedNew,
        request.operation_id,
        request.expected_operation_revision,
        OperationControlKind::kAcknowledgeCompleted,
        OperationRevision(request.expected_operation_revision.value() + 1U),
        2};
    active_.reset();
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }

  Result<OperationStateDTO> get_operation_state(
      const OperationId& operation_id) const override {
    if (!active_.has_value() || active_->operation_id != operation_id) {
      return Result<OperationStateDTO>::Err(
          ErrorDTO{"missing_reference", "operation missing",
                   {{"reason", "operation_missing"}}});
    }
    return Result<OperationStateDTO>::Ok(*active_);
  }

  Result<void> reconcile_subordinate_state() override {
    return Result<void>::Ok();
  }

  const RequestOwnerEpoch& request_owner_epoch() const noexcept override {
    return epoch_;
  }

  void complete() {
    ASSERT_TRUE(active_.has_value());
    active_->state = OperationStateKind::kCompleted;
    active_->operation_revision = OperationRevision(2U);
  }

  void fail() {
    ASSERT_TRUE(active_.has_value());
    active_->state = OperationStateKind::kFailed;
    active_->operation_revision = OperationRevision(2U);
    // The coordinator must handle an owner-level terminal failure even when
    // the bounded observation has no participant detail.
    active_->participants.clear();
  }

  std::vector<OperationIntentDTO> submitted;
  std::vector<OperationId> acknowledged;

 private:
  RequestOwnerEpoch epoch_{"operation.epoch"};
  std::uint64_t next_id_{};
  std::optional<OperationStateDTO> active_;
};

class FakeSceneOwner final : public ISceneAuthority {
 public:
  SceneCommitResultDTO Commit(
      const SceneTransactionRequestDTO& request) override {
    ++commit_count;
    last_request = request;
    if (fail_commit) {
      return {request.request_context.request_id,
              RequestReplayStatusDTO::kAcceptedNew,
              false,
              ErrorDTO{"scene_commit_failed", "fixture scene failure",
                       {{"reason", "fixture_scene_failure"}}},
              std::nullopt,
              std::nullopt,
              SceneProjectionCommitStatusDTO::kFailedProjectionUncertain,
              std::nullopt};
    }
    current_ = successor();
    if (std::holds_alternative<AttachCarriedObjectRequestDTO>(
            request.operation)) {
      state_.attached = true;
      state_.carried_object_attachment_id =
          std::get<AttachCarriedObjectRequestDTO>(request.operation)
              .carried_object_attachment_id;
    } else {
      state_.attached = false;
      state_.carried_object_attachment_id.reset();
    }
    return {request.request_context.request_id,
            RequestReplayStatusDTO::kAcceptedNew,
            true,
            std::nullopt,
            1U,
            current_,
            SceneProjectionCommitStatusDTO::kCommitted,
            SceneUpdateStatusDTO::kGeometryChanged};
  }

  RequestOwnerEpoch CurrentRequestOwnerEpoch() const override {
    return RequestOwnerEpoch("scene.epoch");
  }
  ProjectionRevisionDTO CurrentProjectionRevision() const override {
    return current_;
  }
  std::shared_ptr<const LogicalSceneSnapshotDTO> LogicalSnapshot()
      const override {
    return nullptr;
  }
  std::vector<SceneObjectSnapshotDTO> GetSceneObjects() const override {
    return {};
  }
  SceneObjectStateDTO GetObjectState(
      const SceneResourceId& scene_resource_id) const override {
    auto result = state_;
    result.scene_resource_id = scene_resource_id;
    return result;
  }
  Result<Pose3DTO> GetObjectPose(const SceneResourceId&) const override {
    return Result<Pose3DTO>::Ok(Pose3DTO{});
  }

  void set_attached(const bool value) {
    state_.attached = value;
    state_.carried_object_attachment_id =
        value ? std::optional(CarriedObjectAttachmentId("attachment"))
              : std::nullopt;
  }
  void advance_externally() { current_ = successor(); }

  bool fail_commit{};
  std::size_t commit_count{};
  std::optional<SceneTransactionRequestDTO> last_request;

 private:
  ProjectionRevisionDTO current_{predecessor()};
  SceneObjectStateDTO state_{SceneResourceId("part"), true, false,
                             std::nullopt, std::nullopt};
};

[[nodiscard]] PickPlaceSequenceRequestDTO pick_request() {
  const auto before = predecessor();
  const auto after = successor();
  PickPlaceExecutionPlanDTO plan{
      PickPlaceExecutionKindDTO::kPick,
      SceneResourceId("part"),
      before,
      after,
      PoseGoalDTO{Pose3DTO{{0.0, 0.0, 0.3}, {}}},
      cartesian_path(0.3, 0.2),
      cartesian_path(0.2, 0.3),
      SceneTransactionRequestDTO{
          PublicRequestContextDTO{RequestId("sequence.scene"),
                                  RequestOwnerEpoch("scene.epoch"),
                                  "pick_place_test", std::nullopt},
          {},
          before,
          AttachCarriedObjectRequestDTO{
              CarriedObjectAttachmentId("attachment"),
              SceneResourceId("part"), ControllableComponentId("robot.1"),
              CommandTargetId("arm"), FrameRoleId("tcp"),
              {CollisionBodyRoleId("finger")}, {}}}};
  return {"sequence.pick",
          std::move(plan),
          motion_operation("sequence.approach", "approach"),
          motion_operation("sequence.descend", "descend", true),
          tool_operation("sequence.close"),
          motion_operation("sequence.retreat", "retreat")};
}

[[nodiscard]] PickPlaceSequenceRequestDTO place_request() {
  auto request = pick_request();
  request.sequence_id = "sequence.place";
  request.plan.kind = PickPlaceExecutionKindDTO::kPlace;
  request.plan.scene_transition.request_context.request_id =
      RequestId("sequence.place.scene");
  request.plan.scene_transition.operation = DetachCarriedObjectRequestDTO{
      CarriedObjectAttachmentId("attachment"),
      CarriedObjectAttachmentRevision(kDigest1), Pose3DTO{},
      CellSceneFrameRefDTO{CellId("cell")}};
  request.approach_operation.request_context.request_id =
      RequestId("sequence.place.approach");
  request.descend_operation.request_context.request_id =
      RequestId("sequence.place.descend");
  request.tool_operation.request_context.request_id =
      RequestId("sequence.open");
  request.retreat_operation.request_context.request_id =
      RequestId("sequence.place.retreat");
  auto& descend = std::get<MotionParticipantIntentDTO>(
      request.descend_operation.participants.front().payload);
  descend.contact_policy_id.reset();
  descend.runtime_object_bindings.clear();
  return request;
}

void complete_and_reconcile(FakeOperationOwner& operations,
                            PickPlaceSequenceCoordinator& coordinator) {
  operations.complete();
  auto result = coordinator.Reconcile();
  ASSERT_TRUE(result.ok()) << result.error().message;
}

TEST(PickPlaceSequence, PickOrdersMotionToolAttachAndCarriedRetreat) {
  FakeOperationOwner operations;
  FakeSceneOwner scene;
  PickPlaceSequenceCoordinator coordinator(operations, scene);

  auto begun = coordinator.Begin(pick_request());
  ASSERT_TRUE(begun.ok()) << begun.error().message;
  ASSERT_EQ(operations.submitted.size(), 1U);
  auto& approach = std::get<MotionParticipantIntentDTO>(
      operations.submitted[0].participants.front().payload);
  EXPECT_FALSE(approach.contact_policy_id.has_value());

  complete_and_reconcile(operations, coordinator);
  ASSERT_EQ(operations.submitted.size(), 2U);
  auto& descend = std::get<MotionParticipantIntentDTO>(
      operations.submitted[1].participants.front().payload);
  EXPECT_EQ(descend.contact_policy_id, ContactPolicyId("pick.contact"));
  EXPECT_EQ(scene.commit_count, 0U);

  complete_and_reconcile(operations, coordinator);
  ASSERT_EQ(operations.submitted.size(), 3U);
  EXPECT_TRUE(std::holds_alternative<CapabilityCommandIntentDTO>(
      operations.submitted[2].participants.front().payload));
  EXPECT_EQ(scene.commit_count, 0U);

  complete_and_reconcile(operations, coordinator);
  ASSERT_EQ(scene.commit_count, 1U);
  ASSERT_EQ(operations.submitted.size(), 4U);
  EXPECT_EQ(operations.submitted[3].expected_scene_revision,
            successor().scene_revision);
  auto& retreat = std::get<MotionParticipantIntentDTO>(
      operations.submitted[3].participants.front().payload);
  EXPECT_FALSE(retreat.contact_policy_id.has_value());

  complete_and_reconcile(operations, coordinator);
  const auto status = coordinator.status();
  EXPECT_EQ(status.phase, PickPlaceSequencePhaseDTO::kCompleted);
  EXPECT_EQ(status.completed_operations.size(), 4U);
  EXPECT_EQ(operations.acknowledged.size(), 4U);
  EXPECT_EQ(status.committed_projection_revision, successor());
}

TEST(PickPlaceSequence, PlanningOrMotionFailureLeavesSceneUnchanged) {
  FakeOperationOwner operations;
  FakeSceneOwner scene;
  PickPlaceSequenceCoordinator coordinator(operations, scene);
  ASSERT_TRUE(coordinator.Begin(pick_request()).ok());
  complete_and_reconcile(operations, coordinator);

  operations.fail();
  auto reconciled = coordinator.Reconcile();
  ASSERT_TRUE(reconciled.ok());
  EXPECT_EQ(reconciled.value().phase, PickPlaceSequencePhaseDTO::kFailed);
  EXPECT_EQ(scene.commit_count, 0U);
  EXPECT_EQ(operations.submitted.size(), 2U);
}

TEST(PickPlaceSequence, FailedAttachAfterCloseRequiresInterventionWithoutReopen) {
  FakeOperationOwner operations;
  FakeSceneOwner scene;
  scene.fail_commit = true;
  PickPlaceSequenceCoordinator coordinator(operations, scene);
  ASSERT_TRUE(coordinator.Begin(pick_request()).ok());
  complete_and_reconcile(operations, coordinator);
  complete_and_reconcile(operations, coordinator);
  complete_and_reconcile(operations, coordinator);

  const auto status = coordinator.status();
  EXPECT_EQ(status.phase,
            PickPlaceSequencePhaseDTO::kInterventionRequired);
  EXPECT_EQ(scene.commit_count, 1U);
  EXPECT_EQ(operations.submitted.size(), 3U);
  EXPECT_EQ(operations.acknowledged.size(), 3U);
}

TEST(PickPlaceSequence, RetreatFailurePreservesCommittedAttachmentTruth) {
  FakeOperationOwner operations;
  FakeSceneOwner scene;
  PickPlaceSequenceCoordinator coordinator(operations, scene);
  ASSERT_TRUE(coordinator.Begin(pick_request()).ok());
  complete_and_reconcile(operations, coordinator);
  complete_and_reconcile(operations, coordinator);
  complete_and_reconcile(operations, coordinator);
  ASSERT_EQ(scene.commit_count, 1U);

  operations.fail();
  auto reconciled = coordinator.Reconcile();
  ASSERT_TRUE(reconciled.ok());
  EXPECT_EQ(reconciled.value().phase, PickPlaceSequencePhaseDTO::kFailed);
  EXPECT_TRUE(scene.GetObjectState(SceneResourceId("part")).attached);
  EXPECT_EQ(operations.submitted.size(), 4U);
}

TEST(PickPlaceSequence, PlaceOpensThenDetachesBeforeDetachedRetreat) {
  FakeOperationOwner operations;
  FakeSceneOwner scene;
  scene.set_attached(true);
  PickPlaceSequenceCoordinator coordinator(operations, scene);
  ASSERT_TRUE(coordinator.Begin(place_request()).ok());
  complete_and_reconcile(operations, coordinator);
  complete_and_reconcile(operations, coordinator);
  complete_and_reconcile(operations, coordinator);

  EXPECT_EQ(scene.commit_count, 1U);
  EXPECT_FALSE(scene.GetObjectState(SceneResourceId("part")).attached);
  ASSERT_EQ(operations.submitted.size(), 4U);
  auto& retreat = std::get<MotionParticipantIntentDTO>(
      operations.submitted.back().participants.front().payload);
  EXPECT_FALSE(retreat.contact_policy_id.has_value());
}

TEST(PickPlaceSequence, RejectsAPlanAfterItsSceneRevisionChanges) {
  FakeOperationOwner operations;
  FakeSceneOwner scene;
  scene.advance_externally();
  PickPlaceSequenceCoordinator coordinator(operations, scene);

  auto result = coordinator.Begin(pick_request());

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "pick_place_sequence_plan_stale_or_invalid");
  EXPECT_TRUE(operations.submitted.empty());
  EXPECT_EQ(scene.commit_count, 0U);
}

}  // namespace
}  // namespace manyforge::core
