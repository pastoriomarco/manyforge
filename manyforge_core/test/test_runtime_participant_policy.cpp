#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <optional>
#include <string>

namespace {

using namespace manyforge::core;

[[nodiscard]] std::string digest(char value) {
  return "sha256:" + std::string(64U, value);
}

[[nodiscard]] RuntimeParticipantBinding binding() {
  auto result = verify_runtime_participant_binding(RuntimeParticipantBindingDTO{
      ProgramId("program"),
      ProgramRevision("program.revision"),
      ProgramBindingId("program.binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId("arm"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("robot.type"),
      ComponentTypeRevision("robot.type.revision"),
      CommandTargetId("arm"),
      CapabilityId("execution.trajectory.execute"),
      CapabilityDefinitionRevision("capability.revision"),
      std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision")}},
      {RuntimeModelGroupBindingDTO{
          ModelGroupRoleId("arm"), ModelInstanceId("robot.model"), "robot_arm", {"joint_1"}}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "tcp"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("joint_1"), ResourceId("robot.joint.1"), "joint_1",
                                 "joint_1", CoordinateKind::kAngular}},
      {},
      RuntimeControlPathDTO{ControlDomainId("domain"),
                            ControlDomainRevision("domain.revision"),
                            ControlBindingId("binding"),
                            ControlBindingRevision("binding.revision"),
                            ControlInterfaceContractId("interface"),
                            ControlInterfaceRevision("interface.revision"),
                            ControllerTopologyId("topology"),
                            TopologyRevision("topology.revision"),
                            ControlRouteId("route"),
                            ActivationSetId("activation"),
                            ActiveControlConfigurationId("active"),
                            ActiveControlConfigurationGeneration(1U),
                            ProviderInstanceId("provider"),
                            ProviderManifestId("manifest"),
                            ProviderManifestRevision("manifest.revision"),
                            RuntimeUnitId("runtime"),
                            RuntimeEpoch("runtime.epoch"),
                            StateAuthorityId("state"),
                            StateAuthorityClass::kMeasured,
                            StopEvidencePolicyId("stop"),
                            StopEvidencePolicyRevision("stop.revision"),
                            ContainmentGranularity::kResource,
                            std::nullopt},
      std::nullopt,
      {},
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime"), RuntimeEpoch("runtime.epoch")}}});
  EXPECT_TRUE(result.ok()) << result.error().message;
  return std::move(result.value());
}

[[nodiscard]] RuntimePolicyDTO policy() {
  return RuntimePolicyDTO{
      RuntimePolicyId("runtime.safe"),
      RuntimePolicyRevision(digest('a')),
      std::nullopt,
      {InterruptPolicyId("interrupt.contact"), InterruptPolicyId("interrupt.limit")},
      {RuntimeOpenLoopGraspAcknowledgementDTO{ControllableComponentId("robot.1"),
                                              CommandTargetId("arm"),
                                              OpenLoopGraspEnvironmentDTO::kSimulation, true}}};
}

[[nodiscard]] RuntimePolicyCatalogRecordRefDTO policy_reference(const RuntimePolicyDTO& value) {
  return {value.runtime_policy_id,
          value.runtime_policy_revision,
          SchemaId(std::string(kRuntimePolicyApiVersion)),
          SchemaRevision(digest('b')),
          ContentDigest(digest('c')),
          ContentAddressedStoreKey("catalog/runtime-policy/runtime.safe.json")};
}

TEST(RuntimeParticipantPolicy, SealsExactTargetWithoutTcpSpeedAuthority) {
  const auto participant = binding();
  auto source = policy();
  const auto source_reference = policy_reference(source);
  auto selected =
      verify_runtime_participant_policy_selection(source_reference, std::move(source), participant);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_EQ(selected.value().value().source_artifact.content_addressed_store_key,
            ContentAddressedStoreKey("catalog/runtime-policy/runtime.safe.json"));
  EXPECT_EQ(selected.value().value().policy, policy());
}

TEST(RuntimeParticipantPolicy, DeploymentMaximumSelectsOrRejectsOperationRequest) {
  auto unspecified = apply_deployment_motion_speed_limit(0.25, {});
  ASSERT_TRUE(unspecified.ok());
  EXPECT_EQ(unspecified.value().max_tcp_linear_speed_mps, 0.25);

  SpeedProfileDTO tighter;
  tighter.max_tcp_linear_speed_mps = 0.1;
  auto applied = apply_deployment_motion_speed_limit(0.25, tighter);
  ASSERT_TRUE(applied.ok());
  EXPECT_EQ(applied.value().max_tcp_linear_speed_mps, 0.1);
  tighter.max_tcp_linear_speed_mps = 0.3;
  const auto excessive = apply_deployment_motion_speed_limit(0.25, tighter);
  ASSERT_FALSE(excessive.ok());
  EXPECT_EQ(excessive.error().details.at("reason"), "deployment_motion_speed_exceeded");
  EXPECT_FALSE(apply_deployment_motion_speed_limit(0.0, {}).ok());
  EXPECT_FALSE(apply_deployment_motion_speed_limit(1000.1, {}).ok());
}

TEST(RuntimeParticipantPolicy, EnforcesCanonicalShapeAndInterruptMembership) {
  const auto participant = binding();
  auto noncanonical = policy();
  std::reverse(noncanonical.interrupt_policy_ids.begin(), noncanonical.interrupt_policy_ids.end());
  const auto noncanonical_reference = policy_reference(noncanonical);
  EXPECT_FALSE(verify_runtime_participant_policy_selection(noncanonical_reference,
                                                           std::move(noncanonical), participant)
                   .ok());

  auto source = policy();
  const auto source_reference = policy_reference(source);
  auto selected =
      verify_runtime_participant_policy_selection(source_reference, std::move(source), participant);
  ASSERT_TRUE(selected.ok());
  MotionInterruptScopeDTO allowed;
  allowed.scope_id = "scope.1";
  allowed.policy_id = "interrupt.contact";
  EXPECT_TRUE(
      validate_runtime_participant_interrupt_policy_selection(selected.value(), {&allowed, 1U})
          .ok());
  allowed.policy_id = "interrupt.foreign";
  auto rejected =
      validate_runtime_participant_interrupt_policy_selection(selected.value(), {&allowed, 1U});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "runtime_policy_interrupt_policy_not_selected");
}

TEST(RuntimeParticipantPolicy, OrdinaryJerkPolicyFillsOmissionsAndRejectsWeakerRequests) {
  const auto participant = binding();
  const auto source = policy();
  const auto base =
      verify_runtime_participant_policy_selection(policy_reference(source), source, participant);
  ASSERT_TRUE(base.ok());
  OrdinaryMotionJerkPolicy profile{true, false, {{"joint_1", AngularJerkDTO{10.0}}}};
  const auto selected = apply_deployment_motion_jerk_policy(base.value(), participant, profile);
  ASSERT_TRUE(selected.ok());
  profile.limits.at("joint_1") = AngularJerkDTO{100.0};
  const auto filled = apply_runtime_participant_motion_jerk_policy(selected.value(), {});
  ASSERT_TRUE(filled.ok());
  EXPECT_EQ(filled.value().at("joint_1").max_jerk_rad_s3, 10.0);
  PerJointDynamicsLimitsDTO requested{{"joint_1", {1.0, 2.0, 5.0}}};
  const auto tightened = apply_runtime_participant_motion_jerk_policy(selected.value(), requested);
  ASSERT_TRUE(tightened.ok());
  EXPECT_EQ(tightened.value(), requested);
  requested.at("joint_1").max_jerk_rad_s3 = 11.0;
  const auto rejected = apply_runtime_participant_motion_jerk_policy(selected.value(), requested);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "ordinary_motion_jerk_limit_exceeded");
  EXPECT_FALSE(base.value().value().ordinary_motion_jerk);
}

TEST(RuntimeParticipantPolicy, OrdinaryJerkOffDoesNotActivateRetainedValuesOrWaiveRequiredLimits) {
  const auto participant = binding();
  const auto source = policy();
  const auto base =
      verify_runtime_participant_policy_selection(policy_reference(source), source, participant);
  ASSERT_TRUE(base.ok());
  OrdinaryMotionJerkPolicy profile{false, false, {{"joint_1", AngularJerkDTO{10.0}}}};
  const auto selected = apply_deployment_motion_jerk_policy(base.value(), participant, profile);
  ASSERT_TRUE(selected.ok());
  EXPECT_TRUE(apply_runtime_participant_motion_jerk_policy(selected.value(), {}).value().empty());
  const auto rejected =
      apply_runtime_participant_motion_jerk_policy(selected.value(), {{"joint_1", {{}, {}, 5.0}}});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "ordinary_motion_jerk_disabled");
  profile.machine_required = true;
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(base.value(), participant, profile).ok());
  profile.enabled = true;
  const auto required = apply_deployment_motion_jerk_policy(base.value(), participant, profile);
  ASSERT_TRUE(required.ok());
  profile.machine_required = false;
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(required.value(), participant, profile).ok());
  profile.machine_required = true;
  profile.limits.at("joint_1") = AngularJerkDTO{11.0};
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(required.value(), participant, profile).ok());
  profile.limits.at("joint_1") = AngularJerkDTO{5.0};
  EXPECT_TRUE(apply_deployment_motion_jerk_policy(required.value(), participant, profile).ok());
}

TEST(RuntimeParticipantPolicy, OrdinaryJerkPolicyRejectsIncompleteCrossedAndWrongUnitLimits) {
  const auto participant = binding();
  const auto source = policy();
  const auto base =
      verify_runtime_participant_policy_selection(policy_reference(source), source, participant);
  ASSERT_TRUE(base.ok());
  EXPECT_FALSE(
      apply_deployment_motion_jerk_policy(base.value(), participant, {true, false, {}}).ok());
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(base.value(), participant,
                                                   {true, false, {{"other", AngularJerkDTO{10.0}}}})
                   .ok());
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(
                   base.value(), participant, {true, false, {{"joint_1", LinearJerkDTO{10.0}}}})
                   .ok());
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(
                   base.value(), participant, {true, false, {{"joint_1", AngularJerkDTO{0.0}}}})
                   .ok());
}

}  // namespace
