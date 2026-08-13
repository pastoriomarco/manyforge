#include <gtest/gtest.h>

#include <manyforge_core/runtime_participant_registry.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace manyforge::core;

[[nodiscard]] RuntimeParticipantBinding binding_for(
    std::string target_role, std::string command_target, std::string capability,
    CoordinateKind kind, std::optional<ProgramCommandSchemaRefDTO> command_schema = std::nullopt) {
  RuntimeParticipantBindingDTO value{
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId(std::move(target_role)),
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
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_robot_base"},
       ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
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
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] MotionParticipantIntentDTO motion_payload() {
  return MotionParticipantIntentDTO{MoveId("move.1"),
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
      {OperationParticipantIntentDTO{ComponentRoleId("robot"), TargetRoleId(std::move(target_role)),
                                     CapabilityId(std::move(capability)), std::nullopt,
                                     std::move(payload)}},
      OperationCoordinationPolicy::kIndependent};
}

TEST(RuntimeParticipantRegistryTest, ResolvesExactAddressWithoutRobotOrGroupFallback) {
  RuntimeParticipantRegistry registry(
      {binding_for("arm", "arm", "planning.motion.joint_goal", CoordinateKind::kAngular)});
  const auto operation = operation_for("arm", "planning.motion.joint_goal", motion_payload());

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().value().controllable_component_id.value(), "robot.1");
  EXPECT_EQ(resolved.value().value().model_groups.front().compiled_group_name, "mf2_robot_group");
  EXPECT_EQ(registry.resource_inventory().size(), 1U);
}

TEST(RuntimeParticipantRegistryTest, RejectsWrongProgramBindingContext) {
  RuntimeParticipantRegistry registry(
      {binding_for("arm", "arm", "planning.motion.joint_goal", CoordinateKind::kAngular)});
  auto operation = operation_for("arm", "planning.motion.joint_goal", motion_payload());
  operation.program_binding_id = ProgramBindingId("other-binding");

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_participant_unavailable");
}

TEST(RuntimeParticipantRegistryTest, RejectsTrajectoryMotionAgainstPrismaticCommandTarget) {
  RuntimeParticipantRegistry registry(
      {binding_for("gripper", "gripper", "planning.motion.joint_goal", CoordinateKind::kLinear)});
  const auto operation = operation_for("gripper", "planning.motion.joint_goal", motion_payload());

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_motion_resource_not_angular");
}

TEST(RuntimeParticipantRegistryTest, RejectsCommandCoordinateUnitMismatch) {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  RuntimeParticipantRegistry registry(
      {binding_for("gripper", "gripper", "tool.gripper.command", CoordinateKind::kLinear, schema)});
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
  auto first = binding_for("arm", "arm", "planning.motion.joint_goal", CoordinateKind::kAngular);
  auto duplicate = first;
  EXPECT_THROW(RuntimeParticipantRegistry({std::move(first), std::move(duplicate)}),
               std::invalid_argument);
}

}  // namespace
