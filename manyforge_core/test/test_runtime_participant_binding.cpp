#include <gtest/gtest.h>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <optional>
#include <string>

namespace {

using namespace manyforge::core;

[[nodiscard]] RuntimeParticipantBindingDTO valid_binding() {
  return RuntimeParticipantBindingDTO{
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId("arm"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("ur5"),
      ComponentTypeRevision("ur5-revision"),
      CommandTargetId("arm"),
      CapabilityId("planning.motion.joint_goal"),
      CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      {RuntimeModelGroupBindingDTO{ModelGroupRoleId("arm_group"),
                                   ModelInstanceId("robot.model"),
                                   "mf2_robot_arm",
                                   {"mf2_robot_joint_1", "mf2_robot_joint_2"}}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_robot_tcp"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("joint_1"), ResourceId("robot.joint.1"), "joint_1",
                                 "mf2_robot_joint_1", CoordinateKind::kAngular},
       RuntimeResourceBindingDTO{ResourceRoleId("joint_2"), ResourceId("robot.joint.2"), "joint_2",
                                 "mf2_robot_joint_2", CoordinateKind::kAngular}},
      {},
      RuntimeControlPathDTO{ControlDomainId("domain.arm"),
                            ControlDomainRevision("domain-revision"),
                            ControlBindingId("binding.arm"),
                            ControlBindingRevision("binding-revision"),
                            ControlInterfaceContractId("interface.trajectory"),
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
                            RuntimeEpoch("provider-epoch.1"),
                            StateAuthorityId("state.native"),
                            StateAuthorityClass::kPredicted,
                            StopEvidencePolicyId("stop.native"),
                            StopEvidencePolicyRevision("stop-revision"),
                            ContainmentGranularity::kResource,
                            std::nullopt},
      std::nullopt,
      {},
      {}};
}

TEST(RuntimeParticipantBindingTest, SealsExactControlPathWithoutInventingRuntimeUnits) {
  auto result = verify_runtime_participant_binding(valid_binding());
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().value().resources.size(), 2U);
  EXPECT_TRUE(result.value().value().runtime_unit_epochs.empty());
  ASSERT_TRUE(result.value().value().control_path.has_value());
  EXPECT_EQ(result.value().value().control_path->active_control_configuration_generation.value(),
            1U);
}

TEST(RuntimeParticipantBindingTest,
     DerivesObservedStateContextWithoutCallerSelectedIdentityOrUnits) {
  auto sealed = verify_runtime_participant_binding(valid_binding());
  ASSERT_TRUE(sealed.ok());

  const auto context = make_observed_state_query_context(sealed.value(), 50'000'000);

  ASSERT_TRUE(context.ok()) << context.error().message;
  EXPECT_EQ(context.value().controllable_component_id,
            sealed.value().value().controllable_component_id);
  EXPECT_EQ(context.value().control_binding_id,
            sealed.value().value().control_path->control_binding_id);
  EXPECT_EQ(context.value().resources,
            (std::vector<ResourceId>{ResourceId("robot.joint.1"), ResourceId("robot.joint.2")}));
  EXPECT_EQ(context.value().joint_names,
            (std::vector<std::string>{"mf2_robot_joint_1", "mf2_robot_joint_2"}));
  EXPECT_EQ(context.value().coordinate_kinds,
            (std::vector<CoordinateKind>{CoordinateKind::kAngular, CoordinateKind::kAngular}));
  EXPECT_EQ(context.value().state_authority_class, StateAuthorityClass::kPredicted);
  EXPECT_EQ(context.value().freshness_budget_ns, 50'000'000);
}

TEST(RuntimeParticipantBindingTest,
     SealsNoSourceComponentLocalResourceWithoutInventingAModelGroup) {
  auto value = valid_binding();
  value.target_role_id = TargetRoleId("gripper");
  value.command_target_id = CommandTargetId("gripper");
  value.capability_id = CapabilityId("tool.gripper.command");
  value.model_groups.clear();
  value.resources = {RuntimeResourceBindingDTO{
      ResourceRoleId("fingers"), ResourceId("robot.gripper.logical_position"),
      std::nullopt, "robot.gripper.logical_position",
      CoordinateKind::kLinear}};

  auto result = verify_runtime_participant_binding(std::move(value));

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().value().model_groups.empty());
  ASSERT_EQ(result.value().value().resources.size(), 1U);
  EXPECT_FALSE(
      result.value().value().resources.front().source_joint_name.has_value());
  const auto context = make_observed_state_query_context(result.value(), 0);
  ASSERT_TRUE(context.ok()) << context.error().message;
  EXPECT_EQ(context.value().joint_names,
            (std::vector<std::string>{"robot.gripper.logical_position"}));
  EXPECT_EQ(context.value().coordinate_kinds,
            (std::vector<CoordinateKind>{CoordinateKind::kLinear}));
}

TEST(RuntimeParticipantBindingTest, RejectsDuplicateResourceIdentity) {
  auto value = valid_binding();
  value.resources.back().resource_id = value.resources.front().resource_id;

  auto result = verify_runtime_participant_binding(std::move(value));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_resource_binding_invalid");
}

TEST(RuntimeParticipantBindingTest, RejectsAmbiguousTargetScopedSourceJointName) {
  auto value = valid_binding();
  value.resources.back().source_joint_name = value.resources.front().source_joint_name;

  auto result = verify_runtime_participant_binding(std::move(value));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_resource_binding_invalid");
}

TEST(RuntimeParticipantBindingTest, RejectsMissingActiveOrProviderEpoch) {
  auto missing_active = valid_binding();
  missing_active.control_path->active_control_configuration_generation =
      ActiveControlConfigurationGeneration(0U);
  auto active_result = verify_runtime_participant_binding(std::move(missing_active));
  ASSERT_FALSE(active_result.ok());
  EXPECT_EQ(active_result.error().details.at("reason"), "runtime_control_path_invalid");

  auto missing_provider = valid_binding();
  missing_provider.control_path->provider_runtime_epoch = RuntimeEpoch("");
  auto provider_result = verify_runtime_participant_binding(std::move(missing_provider));
  ASSERT_FALSE(provider_result.ok());
  EXPECT_EQ(provider_result.error().details.at("reason"), "runtime_control_path_invalid");
}

TEST(RuntimeParticipantBindingTest, RequiresAtLeastOneRuntimePath) {
  auto value = valid_binding();
  value.control_path.reset();

  auto result = verify_runtime_participant_binding(std::move(value));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_path_missing");
}

TEST(RuntimeParticipantBindingTest,
     ResolvesOverlappingSemanticBodyRolesToOnePhysicalProjection) {
  auto value = valid_binding();
  const auto semantic = make_component_collision_body_id(
      value.controllable_component_id, ModelInstanceId("robot.model"),
      CollisionBodyRoleId("tool_collision"), 0U);
  const auto physical = make_component_collision_body_id(
      value.controllable_component_id, ModelInstanceId("robot.model"),
      CollisionBodyRoleId("gripper_collision"), 0U);
  value.collision_body_bindings = {ProgramCollisionBodyRoleBindingDTO{
      CollisionBodyRoleId("tool_collision"),
      ModelInstanceId("robot.model"), ModelRevision("model-revision"),
      {"mf2_robot_tool"}}};
  value.contact_grasp_bindings = {ProgramContactGraspBindingDTO{
      ContactPolicyId("grasp.tool"), ContactPolicyRevision("grasp-revision"),
      RuntimeObjectId("selected_object"),
      {ProgramContactGraspBodyBindingDTO{
          value.component_role_id, value.target_role_id,
          value.controllable_component_id, value.command_target_id,
          ModelRoleId("body"), CollisionBodyRoleId("tool_collision"),
          ModelInstanceId("robot.model"), ModelRevision("model-revision"),
          {semantic}}},
      0.002, 0.8}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;

  const std::vector<RuntimeCollisionBodyAliasDTO> aliases{
      {physical, physical},
      {semantic, physical},
  };
  auto resolved = bind_runtime_participant_collision_body_aliases(
      std::move(sealed.value()), aliases);

  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  const auto& body = resolved.value()
                         .value()
                         .contact_grasp_bindings.front()
                         .allowed_body_bindings.front();
  EXPECT_EQ(body.collision_body_role_id.value(), "tool_collision");
  EXPECT_EQ(body.collision_body_ids,
            (std::vector<std::string>{physical}));
}

TEST(RuntimeParticipantBindingTest,
     RejectsCollisionBodyAliasWithoutPhysicalFixedPoint) {
  auto sealed = verify_runtime_participant_binding(valid_binding());
  ASSERT_TRUE(sealed.ok());
  const std::vector<RuntimeCollisionBodyAliasDTO> aliases{
      {CollisionBodyId("semantic"), CollisionBodyId("physical")}};

  auto resolved = bind_runtime_participant_collision_body_aliases(
      std::move(sealed.value()), aliases);

  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"),
            "runtime_collision_body_alias_table_invalid");
}

}  // namespace
