#include <gtest/gtest.h>

#include <manyforge_core/types/operation_intent.hpp>
#include <optional>
#include <string>

namespace {

using namespace manyforge::core;

[[nodiscard]] OperationIntentDTO valid_motion_operation() {
  MotionParticipantIntentDTO motion{MoveId("move.pick"),
                                    FrameRoleId("base"),
                                    FrameRoleId("tcp"),
                                    TolerancesDTO{0.01, 0.001, 0.01},
                                    SpeedProfileDTO{},
                                    TimeoutsDTO{},
                                    CacheMode::kDefault,
                                    PlanningProfileDTO::kBalanced,
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
      {OperationParticipantIntentDTO{ComponentRoleId("robot"), TargetRoleId("arm"),
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

TEST(OperationIntentTest, AcceptsUnitTaggedPrismaticGripperCommand) {
  auto operation = valid_motion_operation();
  operation.request_context.request_id = RequestId("request.command.1");
  operation.participants.front() = OperationParticipantIntentDTO{
      ComponentRoleId("robot"), TargetRoleId("gripper"), CapabilityId("tool.gripper.command"),
      std::nullopt,
      CapabilityCommandIntentDTO{
          CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
          CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
          GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
          2.0,
          {CommandDurationOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}};

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
  motion.target_reference = ReferenceDTO(ReferenceDTO::Type::kFrameRole, "fixture");

  const auto result = validate_operation_intent(operation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "absolute_motion_has_reference");
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

}  // namespace
