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
      {RuntimeMotionSpeedLimitDTO{RuntimePolicyCellScopeDTO{}, 0.5},
       RuntimeMotionSpeedLimitDTO{RuntimePolicyCommandTargetScopeDTO{
                                      ControllableComponentId("robot.1"), CommandTargetId("arm")},
                                  0.25}},
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

TEST(RuntimeParticipantPolicy, SealsExactTargetAndAppliesMinimumCeiling) {
  const auto participant = binding();
  auto source = policy();
  const auto source_reference = policy_reference(source);
  auto selected =
      verify_runtime_participant_policy_selection(source_reference, std::move(source), participant);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_DOUBLE_EQ(selected.value().value().effective_maximum_tcp_linear_speed_mps, 0.25);
  EXPECT_EQ(selected.value().value().source_artifact.content_addressed_store_key,
            ContentAddressedStoreKey("catalog/runtime-policy/runtime.safe.json"));
  ASSERT_TRUE(std::holds_alternative<RuntimePolicyCommandTargetScopeDTO>(
      selected.value().value().selected_motion_speed_limit.scope));
  EXPECT_DOUBLE_EQ(
      selected.value().value().selected_motion_speed_limit.maximum_tcp_linear_speed_mps, 0.25);

  auto unspecified = apply_runtime_participant_motion_speed_policy(selected.value(), {});
  ASSERT_TRUE(unspecified.ok());
  EXPECT_EQ(unspecified.value().max_tcp_linear_speed_mps, 0.25);

  SpeedProfileDTO tighter;
  tighter.max_tcp_linear_speed_mps = 0.1;
  auto applied = apply_runtime_participant_motion_speed_policy(selected.value(), tighter);
  ASSERT_TRUE(applied.ok());
  EXPECT_EQ(applied.value().max_tcp_linear_speed_mps, 0.1);
}

TEST(RuntimeParticipantPolicy, RejectsUnmatchedAndWeakeningTargetScopes) {
  const auto participant = binding();
  auto unmatched = policy();
  unmatched.motion_speed_limits.erase(unmatched.motion_speed_limits.begin());
  auto& target =
      std::get<RuntimePolicyCommandTargetScopeDTO>(unmatched.motion_speed_limits.front().scope);
  target.command_target_id = CommandTargetId("other");
  const auto unmatched_reference = policy_reference(unmatched);
  EXPECT_FALSE(verify_runtime_participant_policy_selection(unmatched_reference,
                                                           std::move(unmatched), participant)
                   .ok());

  auto weakening = policy();
  weakening.motion_speed_limits.back().maximum_tcp_linear_speed_mps = 0.75;
  const auto weakening_reference = policy_reference(weakening);
  auto result = verify_runtime_participant_policy_selection(weakening_reference,
                                                            std::move(weakening), participant);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_policy_target_limit_weakens_cell");
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

}  // namespace
