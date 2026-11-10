#include <gtest/gtest.h>

#include <manyforge_core/runtime_participant_registry.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace manyforge::core;

[[nodiscard]] RuntimeParticipantBinding binding_for(
    std::string command_target, std::string capability,
    CoordinateKind kind, std::optional<ProgramCommandSchemaRefDTO> command_schema = std::nullopt) {
  RuntimeParticipantBindingDTO value{
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("aggregate.robot"),
      ComponentTypeRevision("aggregate-revision"),
      CommandTargetId(std::move(command_target)),
      CapabilityId(std::move(capability)),
      CapabilityDefinitionRevision("capability-revision"),
      std::move(command_schema),
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      {RuntimeModelGroupBindingDTO{
          ModelGroupRoleId("command_group"),
          ModelInstanceId("robot.model"),
          "mf2_robot_group",
          {kind == CoordinateKind::kAngular ? "mf2_robot_joint" : "mf2_gripper_joint"}}},
      {ProgramFrameBindingDTO{{ControllableComponentId("robot.1"), FrameRoleId("base")}, ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_robot_base"},
       ProgramFrameBindingDTO{{ControllableComponentId("robot.1"), FrameRoleId("tcp")}, ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_robot_tcp"}},
      {RuntimeResourceBindingDTO{
          ResourceRoleId(kind == CoordinateKind::kAngular ? "joint" : "finger"),
          ResourceId(kind == CoordinateKind::kAngular ? "robot.joint" : "robot.finger"),
          kind == CoordinateKind::kAngular ? "joint" : "finger_joint",
          kind == CoordinateKind::kAngular ? "mf2_robot_joint" : "mf2_gripper_joint", kind}},
      {},
      RuntimeControlPathDTO{
          ControlDomainId(kind == CoordinateKind::kAngular ? "domain.arm" : "domain.gripper"),
          ControlDomainRevision("domain-revision"),
          ControlBindingId(kind == CoordinateKind::kAngular ? "binding.arm" : "binding.gripper"),
          ControlBindingRevision("binding-revision"),
          ControlInterfaceContractId("interface"),
          ControlInterfaceRevision("interface-revision"),
          ControllerTopologyId("topology.native"),
          TopologyRevision("topology-revision"),
          ControlRouteId("route.native"),
          ActivationSetId("activation.native"),
          ActiveControlConfigurationId("active.native"),
          ActiveControlConfigurationGeneration(1U),
          ProviderInstanceId("provider.native"),
          ProviderManifestId("provider-manifest.native"),
          ProviderManifestRevision("provider-manifest-revision"),
          RuntimeUnitId("runtime.native"),
          RuntimeEpoch("runtime-epoch.1"),
          StateAuthorityId("state.native"),
          StateAuthorityClass::kPredicted,
          StopEvidencePolicyId("stop.native"),
          StopEvidencePolicyRevision("stop-revision"),
          ContainmentGranularity::kResource,
          std::nullopt},
      std::nullopt,
      {},
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime.native"),
                                  RuntimeEpoch("runtime-epoch.1")}}};
  value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
      {value.controllable_component_id, value.command_target_id}, value.frame_bindings.back(), {}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] MotionParticipantIntentDTO motion_payload() {
  return MotionParticipantIntentDTO{
      MoveId("move.1"),
      {ControllableComponentId("robot.1"), FrameRoleId("base")},
      CurrentTcpDTO{},
      TolerancesDTO{0.01, 0.001, 0.01},
      SpeedProfileDTO{},
      TimeoutsDTO{},
      CacheMode::kDefault,
      PlannerOverrideDTO{},
      MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      JointGoalDTO{{"joint"}, {0.2}},
      std::nullopt,
      {},
      {}};
}

[[nodiscard]] OperationIntentDTO operation_for(std::string target_role, std::string capability,
                                               OperationParticipantPayloadDTO payload) {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId("request.1"), RequestOwnerEpoch("owner.epoch.1"), "test",
                              std::nullopt},
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      GraphRevision("graph"),
      RetainedBundleId("bundle"),
      std::nullopt,
      {OperationParticipantIntentDTO{{ControllableComponentId("robot.1"), CommandTargetId(std::move(target_role))},
                                     CapabilityId(std::move(capability)), std::nullopt,
                                     std::move(payload)}},
      OperationCoordinationPolicy::kIndependent};
}

TEST(RuntimeParticipantRegistryTest, ResolvesExactAddressWithoutRobotOrGroupFallback) {
  RuntimeParticipantRegistry registry(
      {binding_for("arm", "planning.motion.joint_goal", CoordinateKind::kAngular)});
  const auto operation = operation_for("arm", "planning.motion.joint_goal", motion_payload());

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().value().controllable_component_id.value(), "robot.1");
  EXPECT_EQ(resolved.value().value().model_groups.front().compiled_group_name, "mf2_robot_group");
  EXPECT_EQ(registry.resource_inventory().size(), 1U);
}

TEST(RuntimeParticipantRegistryTest, RejectsWrongProgramBindingContext) {
  RuntimeParticipantRegistry registry(
      {binding_for("arm", "planning.motion.joint_goal", CoordinateKind::kAngular)});
  auto operation = operation_for("arm", "planning.motion.joint_goal", motion_payload());
  operation.program_binding_id = ProgramBindingId("other-binding");

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_participant_unavailable");
}

TEST(RuntimeParticipantRegistryTest, RejectsTrajectoryMotionAgainstPrismaticCommandTarget) {
  RuntimeParticipantRegistry registry(
      {binding_for("gripper", "planning.motion.joint_goal", CoordinateKind::kLinear)});
  const auto operation =
      operation_for("gripper", "planning.motion.joint_goal", motion_payload());

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_motion_resource_not_angular");
}

TEST(RuntimeParticipantRegistryTest, RejectsCommandCoordinateUnitMismatch) {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  RuntimeParticipantRegistry registry(
      {binding_for("gripper", "tool.gripper.command", CoordinateKind::kLinear, schema)});
  CapabilityCommandIntentDTO command{
      schema.command_schema_id,
      schema.command_schema_revision,
      GripperPositionCommandValueDTO{AngularPositionDTO{0.2}, std::nullopt},
      2.0,
      {}};
  const auto operation = operation_for("gripper", "tool.gripper.command", std::move(command));

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_command_resource_unit_mismatch");
}

TEST(RuntimeParticipantRegistryTest, RejectsAmbiguousRegistrationAtStartup) {
  auto first = binding_for("arm", "planning.motion.joint_goal", CoordinateKind::kAngular);
  auto duplicate = first;
  EXPECT_THROW(RuntimeParticipantRegistry({std::move(first), std::move(duplicate)}),
               std::invalid_argument);
}

TEST(RuntimeParticipantRegistryTest,
     QualifiedTcpMustMatchTheSelectedCaptureWithoutChangingArmOwnership) {
  auto value =
      binding_for("arm", "planning.motion.joint_goal", CoordinateKind::kAngular).value();
  value.motion_endpoint =
      RuntimeMotionEndpointBindingDTO{{ControllableComponentId("robot.1"), CommandTargetId("arm")},
                                      {{ControllableComponentId("tool.1"), FrameRoleId("fingertip")}, ModelInstanceId("robot.model"),
                                       ModelRevision("model-revision"), "mf2_fingertip"},
                                      {}};
  auto selected = verify_runtime_participant_binding(value);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  RuntimeParticipantRegistry registry({selected.value()});
  const TcpEntryReferenceDTO fingertip{{ControllableComponentId("robot.1"), CommandTargetId("arm")},
                                      {ControllableComponentId("tool.1"), FrameRoleId("fingertip")}};
  auto motion = motion_payload();
  motion.tcp_endpoint = fingertip;
  auto operation = operation_for("arm", "planning.motion.joint_goal", motion);
  const auto resolved = registry.resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().value().command_target_id, CommandTargetId("arm"));
  EXPECT_EQ(resolved.value().value().resources, value.resources);
  auto& requested = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
  // A different target or frame cannot consume this captured TCP. The owner
  // is not in the sealed binding (B7); the Kernel checks it.
  auto other = fingertip;
  other.target.command_target_id = CommandTargetId("other_gripper");
  requested.tcp_endpoint = other;
  EXPECT_FALSE(registry.resolve(operation, 0U).ok());
  other = fingertip;
  other.frame.frame_id = FrameRoleId("other_tip");
  requested.tcp_endpoint = other;
  EXPECT_FALSE(registry.resolve(operation, 0U).ok());
  // Current and form 2 were resolved by the owner's resolver into this capture.
  requested.tcp_endpoint = CurrentTcpDTO{};
  EXPECT_TRUE(registry.resolve(operation, 0U).ok());
  requested.tcp_endpoint = DefaultTcpOfTargetDTO{{ControllableComponentId("robot.1"), CommandTargetId("arm")}};
  EXPECT_TRUE(registry.resolve(operation, 0U).ok());
}

TEST(RuntimeParticipantRegistryTest, SetTcpResolvesToTheRobotMotionTargetLikeAMove) {
  RuntimeParticipantRegistry registry(
      {binding_for("arm", "planning.motion.joint_goal", CoordinateKind::kAngular)});
  const TcpEntryReferenceDTO own{{ControllableComponentId("robot.1"), CommandTargetId("arm")},
                                {ControllableComponentId("robot.1"), FrameRoleId("tcp")}};
  auto operation = operation_for("arm", "planning.motion.joint_goal", motion_payload());
  operation.participants.front().payload = SetTcpParticipantIntentDTO{own};
  const auto resolved = registry.resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(tcp_entry_of(*resolved.value().value().motion_endpoint,
                         resolved.value().value().controllable_component_id),
            own);
  auto other = own;
  other.frame.frame_id = FrameRoleId("base");
  operation.participants.front().payload = SetTcpParticipantIntentDTO{other};
  const auto refused = registry.resolve(operation, 0U);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "runtime_motion_mapping_incomplete");
}

}  // namespace
