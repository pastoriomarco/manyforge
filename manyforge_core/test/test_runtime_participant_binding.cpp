#include <gtest/gtest.h>

#include <manyforge_core/program_frame_identity.hpp>
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
      {ProgramFrameBindingDTO{{ControllableComponentId("robot.1"), FrameRoleId("tcp")}, ModelInstanceId("robot.model"),
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
}

[[nodiscard]] RuntimeParticipantBindingDTO gripper_binding() {
  auto value = valid_binding();
  value.command_target_id = CommandTargetId("gripper");
  value.capability_id = CapabilityId("tool.gripper.command");
  value.model_groups.clear();
  value.resources = {{ResourceRoleId("fingers"), ResourceId("robot.gripper.logical_position"),
                      std::nullopt, "robot.gripper.logical_position", CoordinateKind::kLinear}};
  value.control_path->control_domain_id = ControlDomainId("domain.gripper");
  value.control_path->control_binding_id = ControlBindingId("binding.gripper");
  value.control_path->control_route_id = ControlRouteId("route.gripper");
  value.control_path->active_control_configuration_id =
      ActiveControlConfigurationId("active.gripper");
  return value;
}

TEST(RuntimeParticipantBindingTest, SealsExactControlPathRuntimeUnitEpochJoin) {
  auto result = verify_runtime_participant_binding(valid_binding());
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().value().resources.size(), 2U);
  ASSERT_EQ(result.value().value().runtime_unit_epochs.size(), 1U);
  ASSERT_TRUE(result.value().value().control_path.has_value());
  EXPECT_EQ(result.value().value().control_path->provider_runtime_unit_id,
            result.value().value().runtime_unit_epochs.front().runtime_unit_id);
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
  value.command_target_id = CommandTargetId("gripper");
  value.capability_id = CapabilityId("tool.gripper.command");
  value.model_groups.clear();
  value.resources = {RuntimeResourceBindingDTO{
      ResourceRoleId("fingers"), ResourceId("robot.gripper.logical_position"), std::nullopt,
      "robot.gripper.logical_position", CoordinateKind::kLinear}};

  auto result = verify_runtime_participant_binding(std::move(value));

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().value().model_groups.empty());
  ASSERT_EQ(result.value().value().resources.size(), 1U);
  EXPECT_FALSE(result.value().value().resources.front().source_joint_name.has_value());
  const auto context = make_observed_state_query_context(result.value(), 0);
  ASSERT_TRUE(context.ok()) << context.error().message;
  EXPECT_EQ(context.value().joint_names,
            (std::vector<std::string>{"robot.gripper.logical_position"}));
  EXPECT_EQ(context.value().coordinate_kinds,
            (std::vector<CoordinateKind>{CoordinateKind::kLinear}));
}

TEST(RuntimeParticipantBindingTest, AcceptsFramelessGripperAndStillValidatesPresentFrames) {
  auto value = gripper_binding();
  value.frame_bindings.clear();
  auto frameless = verify_runtime_participant_binding(value);
  ASSERT_TRUE(frameless.ok()) << frameless.error().message;
  EXPECT_TRUE(frameless.value().value().frame_bindings.empty());
  EXPECT_EQ(frameless.value().value().command_target_id, CommandTargetId("gripper"));
  EXPECT_EQ(frameless.value().value().resources.front().resource_role_id, ResourceRoleId("fingers"));

  value.frame_bindings = valid_binding().frame_bindings;
  value.frame_bindings.push_back(value.frame_bindings.front());
  auto duplicate = verify_runtime_participant_binding(value);
  ASSERT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.error().details.at("reason"), "runtime_frame_binding_invalid");

  value.frame_bindings.pop_back();
  value.frame_bindings.front().compiled_frame_name.clear();
  auto incomplete = verify_runtime_participant_binding(value);
  ASSERT_FALSE(incomplete.ok());
  EXPECT_EQ(incomplete.error().details.at("reason"), "runtime_frame_binding_invalid");
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

TEST(RuntimeParticipantBindingTest, RejectsMissingProviderRuntimeUnitJoin) {
  auto mismatched_unit = valid_binding();
  mismatched_unit.runtime_unit_epochs.front().runtime_unit_id = RuntimeUnitId("runtime.other");
  auto unit_result = verify_runtime_participant_binding(std::move(mismatched_unit));
  ASSERT_FALSE(unit_result.ok());
  EXPECT_EQ(unit_result.error().details.at("reason"), "runtime_provider_epoch_join_invalid");
}

TEST(RuntimeParticipantBindingTest, RequiresAtLeastOneRuntimePath) {
  auto value = valid_binding();
  value.control_path.reset();

  auto result = verify_runtime_participant_binding(std::move(value));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_path_missing");
}

TEST(RuntimeParticipantBindingTest,
     PhysicalContainmentIgnoresProgramIdentityButFencesRuntimeFacts) {
  auto physical = verify_runtime_participant_binding(valid_binding());
  ASSERT_TRUE(physical.ok()) << physical.error().message;

  auto alias_value = physical.value().value();
  alias_value.program_id = ProgramId("program.place");
  alias_value.program_revision = ProgramRevision("program-place-revision");
  alias_value.program_binding_id = ProgramBindingId("program-place-binding");
  alias_value.resources.pop_back();
  auto alias = verify_runtime_participant_binding(std::move(alias_value));
  ASSERT_TRUE(alias.ok()) << alias.error().message;
  EXPECT_TRUE(runtime_participant_binding_physically_contains(
      physical.value(), alias.value()));

  auto crossed_resource_value = alias.value().value();
  crossed_resource_value.resources.front().compiled_resource_name =
      "mf2_other_joint_1";
  auto crossed_resource =
      verify_runtime_participant_binding(std::move(crossed_resource_value));
  ASSERT_TRUE(crossed_resource.ok()) << crossed_resource.error().message;
  EXPECT_FALSE(runtime_participant_binding_physically_contains(
      physical.value(), crossed_resource.value()));

  auto crossed_epoch_value = alias.value().value();
  crossed_epoch_value.control_path->provider_runtime_epoch =
      RuntimeEpoch("runtime-epoch.2");
  crossed_epoch_value.runtime_unit_epochs.front().runtime_epoch =
      RuntimeEpoch("runtime-epoch.2");
  auto crossed_epoch =
      verify_runtime_participant_binding(std::move(crossed_epoch_value));
  ASSERT_TRUE(crossed_epoch.ok()) << crossed_epoch.error().message;
  EXPECT_FALSE(runtime_participant_binding_physically_contains(
      physical.value(), crossed_epoch.value()));
}

// mock_ur5_pick_and_place: the UR5 `flange` and `tcp` roles compile to one link.
// A Program naming `base` and `tcp` is still exactly contained (the run was
// refused with runtime_program_binding_crosses_physical_owner).
TEST(RuntimeParticipantBindingTest, PhysicalContainmentKeepsRolesThatShareOneFrame) {
  auto value = valid_binding();
  const auto frame = [&](const char *role, const char *compiled) {
    return ProgramFrameBindingDTO{{ControllableComponentId("robot.1"), FrameRoleId(role)}, ModelInstanceId("robot.model"),
                                      ModelRevision("model-revision"), compiled};
  };
  value.frame_bindings = {frame("base", "mf2_base"), frame("flange", "mf2_tool0"),
                          frame("tcp", "mf2_tool0")};
  auto physical = verify_runtime_participant_binding(value);
  ASSERT_TRUE(physical.ok()) << physical.error().message;
  value.frame_bindings = {frame("base", "mf2_base"), frame("tcp", "mf2_tool0")};
  auto selected = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_TRUE(runtime_participant_binding_physically_contains(physical.value(), selected.value()));

  auto foreign = selected.value().value();
  foreign.frame_bindings.back().reference.frame_id = FrameRoleId("camera");
  auto crossed = verify_runtime_participant_binding(std::move(foreign));
  ASSERT_TRUE(crossed.ok()) << crossed.error().message;
  EXPECT_FALSE(runtime_participant_binding_physically_contains(physical.value(), crossed.value()));
}

// The one Program frame identity (`same_program_frame`).
// Frame ID, model, revision and compiled name count; contact sets are separate.
TEST(SameProgramFrameTest, ComparesRoleModelRevisionAndCompiledNameOnly) {
  const ProgramFrameBindingDTO tcp{{ControllableComponentId("robot.1"), FrameRoleId("tcp")}, ModelInstanceId("robot.model"),
                                       ModelRevision("model-revision"), "mf2_tool0"};
  EXPECT_TRUE(same_program_frame(tcp, tcp));

  // A UR5's `flange` and `tcp` share one compiled frame and stay distinct.
  auto flange = tcp;
  flange.reference.frame_id = FrameRoleId("flange");
  EXPECT_FALSE(same_program_frame(tcp, flange));
  auto other_component = tcp;
  other_component.reference.component_id = ControllableComponentId("robot.2");
  EXPECT_TRUE(same_program_frame(tcp, other_component));
  auto other_model = tcp;
  other_model.model_instance_id = ModelInstanceId("gripper.model");
  EXPECT_FALSE(same_program_frame(tcp, other_model));
  auto other_revision = tcp;
  other_revision.model_revision = ModelRevision("other-revision");
  EXPECT_FALSE(same_program_frame(tcp, other_revision));
  auto other_name = tcp;
  other_name.compiled_frame_name = "mf2_link6";
  EXPECT_FALSE(same_program_frame(tcp, other_name));
}

TEST(RuntimeParticipantBindingTest, ResolvesOverlappingSemanticBodyRolesToOnePhysicalProjection) {
  auto value = valid_binding();
  const auto semantic = make_component_collision_body_id(value.controllable_component_id,
                                                         ModelInstanceId("robot.model"),
                                                         CollisionBodyRoleId("tool_collision"), 0U);
  const auto physical = make_component_collision_body_id(
      value.controllable_component_id, ModelInstanceId("robot.model"),
      CollisionBodyRoleId("gripper_collision"), 0U);
  value.collision_body_bindings = {
      ProgramCollisionBodyBindingDTO{{ControllableComponentId("robot.1"), CollisionBodyRoleId("tool_collision")},
                                         ModelInstanceId("robot.model"),
                                         ModelRevision("model-revision"),
                                         {"mf2_robot_tool"}}};
  value.contact_grasp_bindings = {ProgramContactGraspBindingDTO{
      ContactPolicyId("grasp.tool"),
      ContactPolicyRevision("grasp-revision"),
      RuntimeObjectId("selected_object"),
      {ProgramContactGraspBodyBindingDTO{{value.controllable_component_id, CollisionBodyRoleId("tool_collision")},
                                         ModelRoleId("body"),
                                         ModelInstanceId("robot.model"),
                                         ModelRevision("model-revision"),
                                         {semantic}}},
      ContactGraspEnvelopeDTO{0.002, 0.8}}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;

  const std::vector<RuntimeCollisionBodyAliasDTO> aliases{
      {physical, physical},
      {semantic, physical},
  };
  auto resolved =
      bind_runtime_participant_collision_body_aliases(std::move(sealed.value()), aliases);

  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  const auto& body =
      resolved.value().value().contact_grasp_bindings.front().allowed_body_bindings.front();
  EXPECT_EQ(body.body.collision_body_id.value(), "tool_collision");
  EXPECT_EQ(body.collision_body_ids, (std::vector<std::string>{physical}));
}

TEST(RuntimeParticipantBindingTest, RejectsCollisionBodyAliasWithoutPhysicalFixedPoint) {
  auto sealed = verify_runtime_participant_binding(valid_binding());
  ASSERT_TRUE(sealed.ok());
  const std::vector<RuntimeCollisionBodyAliasDTO> aliases{
      {CollisionBodyId("semantic"), CollisionBodyId("physical")}};

  auto resolved =
      bind_runtime_participant_collision_body_aliases(std::move(sealed.value()), aliases);

  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_collision_body_alias_table_invalid");
}

TEST(RuntimeParticipantBindingTest, FrameAssociationsMustNameUniqueTargetLocalFrames) {
  auto value = valid_binding();
  value.frame_associations = {{value.frame_bindings.front().reference, {}}};
  ASSERT_TRUE(verify_runtime_participant_binding(value).ok());
  auto duplicate = value;
  duplicate.frame_associations.push_back(duplicate.frame_associations.front());
  EXPECT_FALSE(verify_runtime_participant_binding(std::move(duplicate)).ok());
  value.frame_associations.front().frame.frame_id = FrameRoleId("not_bound");
  EXPECT_FALSE(verify_runtime_participant_binding(std::move(value)).ok());
}

TEST(RuntimeParticipantBindingTest, MotionEndpointRetainsOneExactModelAndBodyAssociation) {
  auto value = valid_binding();
  const auto endpoint = [&](std::string role, std::string body_role) {
    return RuntimeMotionEndpointBindingDTO{
        {ControllableComponentId("robot.1"), CommandTargetId("arm")},
        {{ControllableComponentId("robot.1"), FrameRoleId(role)},
         ModelInstanceId("robot.model"),
         ModelRevision("model-revision"),
         "mf2_robot_" + role},
        {{{ControllableComponentId("robot.1"), CollisionBodyRoleId(body_role)},
          std::nullopt,
          ModelInstanceId("robot.model"),
          ModelRevision("model-revision"),
          {make_component_collision_body_id(ControllableComponentId("robot.1"),
                                            ModelInstanceId("robot.model"),
                                            CollisionBodyRoleId(body_role), 0U)}}},
        {CollisionBodyRoleId(body_role)}};
  };
  value.motion_endpoint = endpoint("left_tip", "left_finger");
  const auto left = verify_runtime_participant_binding(value);
  ASSERT_TRUE(left.ok()) << left.error().message;
  value.motion_endpoint = endpoint("right_tip", "right_finger");
  const auto right = verify_runtime_participant_binding(value);
  ASSERT_TRUE(right.ok()) << right.error().message;
  EXPECT_NE(left.value().value().motion_endpoint, right.value().value().motion_endpoint);
  EXPECT_EQ(left.value().value().resources, right.value().value().resources);
  EXPECT_EQ(left.value().value().command_target_id, CommandTargetId("arm"));
  // The same physical frame can have different contact sets for two targets.
  auto first_target = endpoint("shared_tip", "left_finger");
  auto second_target = endpoint("shared_tip", "right_finger");
  second_target.target.command_target_id = CommandTargetId("other_arm");
  EXPECT_EQ(first_target.frame, second_target.frame);
  EXPECT_NE(first_target.contact_body_role_ids, second_target.contact_body_role_ids);
  value.motion_endpoint = first_target;
  ASSERT_TRUE(verify_runtime_participant_binding(value).ok());
  value.motion_endpoint = second_target;
  ASSERT_TRUE(verify_runtime_participant_binding(value).ok());
  value.motion_endpoint = endpoint("right_tip", "right_finger");
  for (int invalid = 0; invalid < 6; ++invalid) {
    SCOPED_TRACE(invalid);
    auto wrong = value;
    auto& selected = *wrong.motion_endpoint;
    switch (invalid) {
      case 0:
        selected.frame.model_instance_id = ModelInstanceId("other.model");
        break;
      case 1:
        selected.frame.model_revision = ModelRevision("other-revision");
        break;
      case 2:
        selected.contact_body_bindings.front().body.component_id =
            ControllableComponentId("other.robot");
        break;
      case 3:
        selected.target.component_id = ControllableComponentId("");
        break;
      case 4:
        selected.contact_body_bindings.front().body.collision_body_id =
            CollisionBodyRoleId("other");
        break;
      case 5:
        selected.contact_body_bindings.front().collision_body_ids.front() = "source-link-name";
        break;
    }
    EXPECT_FALSE(verify_runtime_participant_binding(std::move(wrong)).ok());
  }
  // Endpoints without contact bodies remain valid for ordinary motion.
  value.motion_endpoint->contact_body_role_ids.clear();
  value.motion_endpoint->contact_body_bindings.clear();
  EXPECT_TRUE(verify_runtime_participant_binding(value).ok());
}

// TCP-D: a robot motion endpoint may sit on a tool mounted on the robot (its
// own model instance); the contact bodies keep the tool's component identity.
// The resolver proves the mount against the cell graph.
TEST(RuntimeParticipantBindingTest, MotionEndpointMayUseAMountedToolModel) {
  auto value = valid_binding();
  const auto body_id = [](std::string component) {
    return make_component_collision_body_id(ControllableComponentId(component),
                                            ModelInstanceId("lite_gripper.model"),
                                            CollisionBodyRoleId("gripper_fingers"), 0U);
  };
  value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
      {ControllableComponentId("robot.1"), CommandTargetId("arm")},
      {{ControllableComponentId("lite_gripper"), FrameRoleId("tcp")},
       ModelInstanceId("lite_gripper.model"),
       ModelRevision("tool-model-revision"),
       "mf2_lite_gripper_link_tcp"},
      {{{ControllableComponentId("lite_gripper"), CollisionBodyRoleId("gripper_fingers")},
        std::nullopt,
        ModelInstanceId("lite_gripper.model"),
        ModelRevision("tool-model-revision"),
        {body_id("lite_gripper")}}},
      {CollisionBodyRoleId("gripper_fingers")}};
  const auto accepted = verify_runtime_participant_binding(value);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  // Body ids keyed by the motion component instead of the body owner fail.
  value.motion_endpoint->contact_body_bindings.front().collision_body_ids.front() =
      body_id("robot.1");
  EXPECT_FALSE(verify_runtime_participant_binding(value).ok());
  // The default robot `tcp` on the tool carries no contact bodies.
  value.motion_endpoint->contact_body_role_ids.clear();
  value.motion_endpoint->contact_body_bindings.clear();
  EXPECT_TRUE(verify_runtime_participant_binding(value).ok());
}

}  // namespace
