#include <gtest/gtest.h>

#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <optional>
#include <string>

#include "../src/component_lifecycle_journal_codec_internal.hpp"

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
  value.target_role_id = TargetRoleId("gripper");
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

[[nodiscard]] SelectedComponentBringUpOfferDTO selected_offer(
    std::vector<RuntimeParticipantBindingDTO> predecessor_bindings,
    std::vector<RuntimeParticipantBindingDTO> candidates,
    std::optional<std::string> request_reason = std::nullopt) {
  const auto sha = [](char digit) { return "sha256:" + std::string(64U, digit); };
  const ProjectionRevisionDTO revision{RetainedBundleId("bundle"),
                                       GraphRevision("graph"),
                                       {SceneId(sha('a')), 1U, ContentDigest(sha('b'))}};
  const ActiveBundlePublicationReceiptDTO publication{
      {BootstrapPublicationTransactionDTO{BootstrapId("boot"), BootstrapGeneration(1U)}, revision,
       revision.retained_bundle_id, revision.graph_revision, std::nullopt,
       ContentDigest("private")},
      ActiveBundlePublicationGeneration(2U),
      ContentDigest("pointer")};
  ComponentLifecycleCurrentSnapshotDTO predecessor{ContentDigest("pending"),
                                                   ControllableComponentId("robot.1"),
                                                   ComponentLifecycleGeneration(3U),
                                                   ComponentLifecycleStateDTO::kFaulted,
                                                   publication,
                                                   {},
                                                   {}};
  std::vector<DomainActiveControlConfigurationDTO> configurations;
  for (const auto& binding : predecessor_bindings) {
    const auto& path = *binding.control_path;
    std::vector<ResourceId> resources;
    for (const auto& resource : binding.resources) resources.push_back(resource.resource_id);
    const ActiveControlConfigurationDTO active{
        path.active_control_configuration_id,
        path.control_domain_id,
        path.control_domain_revision,
        {{path.control_binding_id, path.control_binding_revision, path.control_route_id}},
        path.activation_set_id,
        path.controller_topology_id,
        path.topology_revision,
        path.provider_runtime_unit_id,
        path.provider_runtime_epoch,
        1U,
        {},
        path.active_control_configuration_generation,
        ActiveControlConfigurationStateDTO::kReconciled};
    predecessor.domains.push_back(
        {path.control_domain_id,
         path.control_domain_revision,
         path.controller_topology_id,
         path.topology_revision,
         active,
         {{path.control_binding_id, path.control_binding_revision, path.control_route_id}},
         std::move(resources)});
    predecessor.provider_occurrences.push_back(
        {path.control_domain_id, path.control_domain_revision, path.provider_instance_id,
         path.provider_manifest_id, path.provider_manifest_revision, path.provider_runtime_unit_id,
         path.provider_runtime_epoch, ContentDigest(sha('c'))});
    configurations.push_back({path.control_domain_id, path.active_control_configuration_id,
                              path.active_control_configuration_generation});
  }
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(predecessor);
  EXPECT_TRUE(digest.ok());
  if (digest.ok()) predecessor.snapshot_digest = digest.value();
  std::vector<ExpectedControlDomainRevisionDTO> expected_domains;
  for (const auto& domain : predecessor.domains) {
    const ExpectedActiveControlConfigurationDTO expected_active{
        ExpectedActiveControlConfigurationPresenceDTO::kPresent,
        domain.controller().active_control_configuration->active_control_configuration_id,
        domain.controller().active_control_configuration->active_control_configuration_generation};
    ExpectedControlDomainRevisionDTO expected_domain{domain.control_domain_id,
                                                     domain.control_domain_revision,
                                                     domain.controller().controller_topology_id,
                                                     domain.controller().topology_revision,
                                                     expected_active,
                                                     domain.controller().current_control_bindings};
    expected_domains.push_back(std::move(expected_domain));
  }
  const ComponentLifecycleIntentDTO request{
      {RequestId("bring-up"), RequestOwnerEpoch("cclc.owner"), "test", std::nullopt},
      predecessor.controllable_component_id,
      ComponentLifecycleIntentKindDTO::kEnable,
      revision.graph_revision,
      predecessor.component_lifecycle_generation,
      std::move(expected_domains),
      std::move(request_reason)};
  const auto original_request_digest = SelectedComponentBringUpSemanticRequestDigest(
      request, revision, ContentDigest(sha('d')), 100U, RuntimeUnitId("runtime.native"),
      RuntimeEpoch("runtime-epoch.1"), RuntimeEpoch("manager.1"));
  SelectedComponentBringUpOfferDTO offer{
      {RequestId("bring-up"), RequestOwnerEpoch("cclc.owner"), "test", std::nullopt},
      LifecycleTransactionId("selected-lifecycle.4"),
      LifecycleTransactionGeneration(4U),
      std::move(predecessor),
      "incident.1",
      {StopId("stop.1"), StopGeneration(2U), {}, std::nullopt},
      revision,
      ContentDigest(sha('d')),
      100U,
      std::move(candidates),
      {{{}, RuntimeEpoch("manager.1")},
       {RuntimeUnitId("runtime.native"), RuntimeEpoch("runtime-epoch.1"),
        RequestOwnerEpoch("kernel.1"), ContentDigest(sha('e')),
        ActiveBundlePublicationGeneration(2U)},
       revision,
       ContentDigest("pointer"),
       1U,
       ConfigurationFenceGeneration(1U),
       std::move(configurations),
       0U,
       0U,
       ContentDigest(sha('f'))},
      original_request_digest};
  const auto scope = SelectedComponentBringUpFenceScope(offer);
  EXPECT_TRUE(scope.ok());
  if (scope.ok()) offer.fence_admission.echoed_query.affected_scope = scope.value();
  return offer;
}

TEST(SelectedBringUpScope,
     AddedGripperHasCandidateOnlyDomainAndNoInventedPredecessorConfiguration) {
  const auto offer = selected_offer({valid_binding()}, {valid_binding(), gripper_binding()});
  const auto scope = SelectedComponentBringUpFenceScope(offer);
  ASSERT_TRUE(scope.ok()) << scope.error().message;
  ASSERT_EQ(scope.value().domains.size(), 2U);
  EXPECT_EQ(scope.value().domains.back().control_domain_id, ControlDomainId("domain.gripper"));
  EXPECT_FALSE(scope.value().domains.back().predecessor_control_domain_revision);
  EXPECT_TRUE(scope.value().domains.back().candidate_control_domain_revision);
  EXPECT_EQ(scope.value().resources.size(), 3U);
  const auto admission = SelectedComponentBringUpAdmissionScope(offer);
  ASSERT_TRUE(admission.ok());
  ASSERT_EQ(admission.value().configurations.size(), 1U);
  EXPECT_EQ(admission.value().configurations.front().control_domain_id,
            ControlDomainId("domain.arm"));
}

TEST(SelectedBringUpScope, RemovedGripperRemainsFencedUntilItsExactPredecessorIsRetired) {
  const auto offer = selected_offer({valid_binding(), gripper_binding()}, {valid_binding()});
  const auto scope = SelectedComponentBringUpFenceScope(offer);
  ASSERT_TRUE(scope.ok()) << scope.error().message;
  ASSERT_EQ(scope.value().domains.size(), 2U);
  EXPECT_TRUE(scope.value().domains.back().predecessor_control_domain_revision);
  EXPECT_FALSE(scope.value().domains.back().candidate_control_domain_revision);
  EXPECT_EQ(scope.value().resources.size(), 3U);
  const auto admission = SelectedComponentBringUpAdmissionScope(offer);
  ASSERT_TRUE(admission.ok());
  EXPECT_EQ(admission.value().configurations.size(), 2U);
}

TEST(SelectedBringUpScope, RefusesForeignComponentAndConflictingCandidateRevision) {
  auto offer = selected_offer({valid_binding()}, {valid_binding(), gripper_binding()});
  offer.candidate_participants.back().controllable_component_id = ControllableComponentId("peer");
  EXPECT_FALSE(SelectedComponentBringUpFenceScope(offer).ok());
  offer.candidate_participants.back() = valid_binding();
  offer.candidate_participants.back().control_path->control_domain_revision =
      ControlDomainRevision("foreign-revision");
  EXPECT_FALSE(SelectedComponentBringUpFenceScope(offer).ok());
}

TEST(SelectedBringUpScope, OriginalRequestDigestIsSealedByTheOwnerOfferProof) {
  auto offer = selected_offer({valid_binding()}, {valid_binding()});
  const auto changed =
      selected_offer({valid_binding()}, {valid_binding()}, "operator changed selected settings");
  ASSERT_NE(offer.original_request_payload_digest, changed.original_request_payload_digest);
  const auto proof = detail::SelectedComponentBringUpSemanticOfferDigest(offer);
  offer.original_request_payload_digest = changed.original_request_payload_digest;
  EXPECT_EQ(offer, changed);
  EXPECT_NE(detail::SelectedComponentBringUpSemanticOfferDigest(offer), proof);
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
  value.target_role_id = TargetRoleId("gripper");
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
  alias_value.component_role_id = ComponentRoleId("place_robot");
  alias_value.target_role_id = TargetRoleId("place_arm");
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

TEST(RuntimeParticipantBindingTest, ResolvesOverlappingSemanticBodyRolesToOnePhysicalProjection) {
  auto value = valid_binding();
  const auto semantic = make_component_collision_body_id(value.controllable_component_id,
                                                         ModelInstanceId("robot.model"),
                                                         CollisionBodyRoleId("tool_collision"), 0U);
  const auto physical = make_component_collision_body_id(
      value.controllable_component_id, ModelInstanceId("robot.model"),
      CollisionBodyRoleId("gripper_collision"), 0U);
  value.collision_body_bindings = {
      ProgramCollisionBodyRoleBindingDTO{CollisionBodyRoleId("tool_collision"),
                                         ModelInstanceId("robot.model"),
                                         ModelRevision("model-revision"),
                                         {"mf2_robot_tool"}}};
  value.contact_grasp_bindings = {ProgramContactGraspBindingDTO{
      ContactPolicyId("grasp.tool"),
      ContactPolicyRevision("grasp-revision"),
      RuntimeObjectId("selected_object"),
      {ProgramContactGraspBodyBindingDTO{value.component_role_id,
                                         value.target_role_id,
                                         value.controllable_component_id,
                                         value.command_target_id,
                                         ModelRoleId("body"),
                                         CollisionBodyRoleId("tool_collision"),
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
  EXPECT_EQ(body.collision_body_role_id.value(), "tool_collision");
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

TEST(RuntimeParticipantBindingTest, MotionEndpointRetainsOneExactModelAndBodyAssociation) {
  auto value = valid_binding();
  const auto endpoint = [&](std::string role, std::string body_role) {
    return RuntimeMotionEndpointBindingDTO{
        TargetRoleId("tool"),
        CommandTargetId("gripper"),
        {FrameRoleId(role),
         ModelInstanceId("robot.model"),
         ModelRevision("model-revision"),
         "mf2_robot_" + role,
         {CollisionBodyRoleId(body_role)}},
        {{ComponentRoleId("robot"),
          TargetRoleId("tool"),
          ControllableComponentId("robot.1"),
          CommandTargetId("gripper"),
          std::nullopt,
          CollisionBodyRoleId(body_role),
          ModelInstanceId("robot.model"),
          ModelRevision("model-revision"),
          {make_component_collision_body_id(ControllableComponentId("robot.1"),
                                            ModelInstanceId("robot.model"),
                                            CollisionBodyRoleId(body_role), 0U)}}}};
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
        selected.contact_body_bindings.front().controllable_component_id =
            ControllableComponentId("other.robot");
        break;
      case 3:
        selected.contact_body_bindings.front().target_role_id = TargetRoleId("other.tool");
        break;
      case 4:
        selected.contact_body_bindings.front().collision_body_role_id =
            CollisionBodyRoleId("other");
        break;
      case 5:
        selected.contact_body_bindings.front().collision_body_ids.front() = "source-link-name";
        break;
    }
    EXPECT_FALSE(verify_runtime_participant_binding(std::move(wrong)).ok());
  }
  // Endpoints without contact bodies remain valid for ordinary motion.
  value.motion_endpoint->frame.contact_body_role_ids.clear();
  value.motion_endpoint->contact_body_bindings.clear();
  EXPECT_TRUE(verify_runtime_participant_binding(value).ok());
}

}  // namespace
