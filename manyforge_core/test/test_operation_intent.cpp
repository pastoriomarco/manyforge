#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/types/operation_intent.hpp>
#include <optional>
#include <string>

namespace {

using namespace manyforge::core;

[[nodiscard]] OperationIntentDTO valid_motion_operation() {
  MotionParticipantIntentDTO motion{
      MoveId("move.pick"),
      {ControllableComponentId("robot"), FrameRoleId("base")},
      CurrentTcpDTO{},
      TolerancesDTO{0.01, 0.001, 0.01},
      SpeedProfileDTO{},
      TimeoutsDTO{},
      CacheMode::kDefault,
      PlannerOverrideDTO{},
      MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      JointGoalDTO{{"joint_1"}, {0.25}},
      std::nullopt,
      {},
      {}};
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId("request.motion.1"), RequestOwnerEpoch("owner.epoch.1"),
                              "test", std::nullopt},
      ProgramId("program.pick"),
      ProgramRevision("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
      ProgramBindingId("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
      ProgramRunId("run.1"),
      GraphRevision("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"),
      RetainedBundleId("sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"),
      std::nullopt,
      {OperationParticipantIntentDTO{{ControllableComponentId("robot"), CommandTargetId("arm")},
                                     CapabilityId("planning.motion.joint_goal"),
                                     ActionSpanDTO{ActionSource::kProgramNode, "attempt.1", "run.1",
                                                   "root/move", "attempt.1", ""},
                                     std::move(motion)}},
      OperationCoordinationPolicy::kIndependent};
}

TEST(OperationIntentTest, AcceptsPortableSingleMotionIntent) {
  const auto result = validate_operation_intent(valid_motion_operation());
  ASSERT_TRUE(result.ok()) << result.error().message;
}

TEST(OperationIntentTest, EveryParticipantActionSpanMustMatchTheOperationRun) {
  auto operation = valid_motion_operation();
  operation.coordination_policy = OperationCoordinationPolicy::kCoordinatedAll;
  operation.participants.push_back(operation.participants.front());
  operation.participants.back().action_span->program_run_id = "run.other";
  const auto rejected = validate_operation_intent(operation);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "operation_action_span_run_mismatch");
  operation.participants.back().action_span->program_run_id = operation.program_run_id.value();
  EXPECT_TRUE(validate_operation_intent(operation).ok());
}

TEST(OperationIntentTest, RejectsDuplicateSourceJointNames) {
  auto operation = valid_motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
  motion.motion = JointGoalDTO{{"joint_1", "joint_1"}, {0.25, 0.5}};

  const auto result = validate_operation_intent(operation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "motion_payload_invalid");
}

TEST(OperationIntentTest, MotionClearanceIsOptionalAndBounded) {
  // A move's own collision clearance, in [0, 0.05] m.
  for (const double admitted : {0.0, 0.0005, 0.05}) {
    auto operation = valid_motion_operation();
    std::get<MotionParticipantIntentDTO>(operation.participants.front().payload).clearance_m =
        admitted;
    EXPECT_TRUE(validate_operation_intent(operation).ok()) << admitted;
  }
  for (const double refused : {-0.001, 0.0501, std::numeric_limits<double>::quiet_NaN()}) {
    auto operation = valid_motion_operation();
    std::get<MotionParticipantIntentDTO>(operation.participants.front().payload).clearance_m =
        refused;
    const auto result = validate_operation_intent(operation);
    ASSERT_FALSE(result.ok()) << refused;
    EXPECT_EQ(result.error().details.at("reason"), "motion_clearance_invalid");
  }
}

TEST(OperationIntentTest, AcceptsUnitTaggedPrismaticGripperCommand) {
  auto operation = valid_motion_operation();
  operation.request_context.request_id = RequestId("request.command.1");
  operation.participants.front() = OperationParticipantIntentDTO{
      {ControllableComponentId("robot"), CommandTargetId("gripper")}, CapabilityId("tool.gripper.command"),
      std::nullopt,
      CapabilityCommandIntentDTO{
          CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
          CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
          GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
          2.0,
          {CommandOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}};

  const auto result = validate_operation_intent(operation);
  ASSERT_TRUE(result.ok()) << result.error().message;
}

TEST(OperationIntentTest, RejectsMalformedIdentityBeforeRuntimeResolution) {
  auto operation = valid_motion_operation();
  operation.program_binding_id = ProgramBindingId("");

  const auto result = validate_operation_intent(operation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "operation_identity_empty");
}

TEST(OperationIntentTest, RejectsPrincipalWithoutTrustedAdapterAssurance) {
  auto operation = valid_motion_operation();
  operation.attribution = RequestAttributionDTO{
      RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified, "forged-principal"};

  const auto result = validate_operation_intent(operation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "operation_attribution_invalid");
}

TEST(OperationIntentTest, RejectsUnknownAdapterAttributionEnum) {
  auto operation = valid_motion_operation();
  operation.attribution.transport = static_cast<RequestTransportDTO>(99);

  const auto result = validate_operation_intent(operation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "operation_attribution_invalid");
}

TEST(OperationIntentTest, RejectsAbsoluteMotionWithRelativeReference) {
  auto operation = valid_motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
  motion.target_reference = ReferenceDTO(ProgramFrameReferenceDTO{ControllableComponentId("robot"), FrameRoleId("fixture")});

  const auto result = validate_operation_intent(operation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "absolute_motion_has_reference");
}

TEST(OperationIntentTest, AcceptsAbsolutePoseInNamedGoalFrameOnly) {
  auto operation = valid_motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
  Pose3DTO goal;
  goal.orientation.w = 1.0;
  motion.motion = PoseGoalDTO{goal};
  const ProgramFrameReferenceDTO peer{ControllableComponentId("peer"), FrameRoleId("tcp")};
  for (const auto& frame :
       {ReferenceDTO(ReferenceDTO::Type::kCellFrame, ""), ReferenceDTO(peer),
        ReferenceDTO(ReferenceDTO::Type::kSceneResource, "fixture")}) {
    motion.goal_frame_reference = frame;
    const auto accepted = validate_operation_intent(operation);
    EXPECT_TRUE(accepted.ok()) << accepted.error().message;
  }

  const auto rejected = [&](const std::string& reason) {
    const auto result = validate_operation_intent(operation);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"), reason);
  };
  motion.goal_frame_reference = ReferenceDTO(goal);
  rejected("goal_frame_inline_pose_unsupported");
  motion.goal_frame_reference = ReferenceDTO(ReferenceDTO::Type::kCellFrame, "");
  motion.target_mode = MotionTargetModeDTO::kRelative;
  motion.target_reference = ReferenceDTO(ProgramFrameReferenceDTO{ControllableComponentId("robot"), FrameRoleId("fixture")});
  rejected("goal_frame_requires_absolute_target");
  motion.target_mode = MotionTargetModeDTO::kAbsolute;
  motion.target_reference.reset();
  motion.motion = JointGoalDTO{{"joint_1"}, {0.25}};
  rejected("goal_frame_motion_type_unsupported");
  motion.motion = PoseGoalDTO{goal};
  auto incomplete = peer;
  incomplete.component_id = ControllableComponentId("");
  motion.goal_frame_reference = ReferenceDTO(incomplete);
  rejected("motion_reference_invalid");
}

TEST(OperationIntentTest, RejectsGripperEffortWithDifferentCoordinateKind) {
  auto operation = valid_motion_operation();
  operation.participants.front().payload = CapabilityCommandIntentDTO{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
      GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, AngularEffortDTO{1.0}},
      2.0,
      {}};

  const auto result = validate_operation_intent(operation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "gripper_command_effort_unit_mismatch");
}

TEST(OperationIntentTest, TcpEndpointTakesTheThreeSelectorFormsWithCompleteIdentities) {
  auto operation = valid_motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
  EXPECT_TRUE(validate_operation_intent(operation).ok());
  motion.tcp_endpoint = DefaultTcpOfTargetDTO{{ControllableComponentId("lite_gripper"), CommandTargetId("gripper")}};
  EXPECT_TRUE(validate_operation_intent(operation).ok());
  motion.tcp_endpoint = DefaultTcpOfTargetDTO{{ControllableComponentId(""), CommandTargetId("gripper")}};
  EXPECT_EQ(validate_operation_intent(operation).error().details.at("reason"),
            "motion_tcp_endpoint_invalid");
  const TcpEntryReferenceDTO entry{{ControllableComponentId("robot"), CommandTargetId("arm")},
                                   {ControllableComponentId("lite_gripper"), FrameRoleId("finger_tip")}};
  motion.tcp_endpoint = entry;
  EXPECT_TRUE(validate_operation_intent(operation).ok());
  for (int missing = 0; missing < 4; ++missing) {
    auto incomplete = entry;
    if (missing == 0) incomplete.target.component_id = ControllableComponentId("");
    if (missing == 1) incomplete.target.command_target_id = CommandTargetId("");
    if (missing == 2) incomplete.frame.component_id = ControllableComponentId("");
    if (missing == 3) incomplete.frame.frame_id = FrameRoleId("");
    motion.tcp_endpoint = incomplete;
    EXPECT_FALSE(validate_operation_intent(operation).ok()) << missing;
  }
}

TEST(OperationIntentTest, SetTcpNamesOneRobotAndNeverCurrent) {
  auto operation = valid_motion_operation();
  operation.participants.front().payload = SetTcpParticipantIntentDTO{TcpEntryReferenceDTO{
      {ControllableComponentId("robot"), CommandTargetId("arm")},
      {ControllableComponentId("lite_gripper"), FrameRoleId("tcp")}}};
  EXPECT_TRUE(validate_operation_intent(operation).ok());
  operation.participants.front().payload =
      SetTcpParticipantIntentDTO{DefaultTcpOfTargetDTO{{ControllableComponentId("lite_gripper"), CommandTargetId("gripper")}}};
  EXPECT_TRUE(validate_operation_intent(operation).ok());
  operation.participants.front().payload = SetTcpParticipantIntentDTO{CurrentTcpDTO{}};
  EXPECT_EQ(validate_operation_intent(operation).error().details.at("reason"),
            "set_tcp_current_invalid");
  operation.participants.front().payload =
      SetTcpParticipantIntentDTO{DefaultTcpOfTargetDTO{{ControllableComponentId("lite_gripper"), CommandTargetId("gripper")}}};
  operation.coordination_policy = OperationCoordinationPolicy::kCoordinatedAll;
  operation.participants.push_back(operation.participants.front());
  EXPECT_EQ(validate_operation_intent(operation).error().details.at("reason"),
            "set_tcp_shape_invalid");
}

}  // namespace
