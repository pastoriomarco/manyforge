#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/participant_stop_stage.hpp>
#include <sstream>

namespace manyforge::core {
namespace {

[[nodiscard]] std::string GoldenBytes() {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << stream.rdbuf();
  auto value = bytes.str();
  if (!value.empty() && value.back() == '\n') {
    value.pop_back();
  }
  return value;
}

[[nodiscard]] ResolvedStopBasePlanDTO BasePlan() {
  auto base = DecodeResolvedStopBasePlan(GoldenBytes());
  EXPECT_TRUE(base.ok());
  return std::move(base.value());
}

[[nodiscard]] RuntimeParticipantBinding Binding(const ResolvedStopBasePlanDTO& base) {
  const auto& target = base.escalation_stages.front().command_target;
  RuntimeParticipantBindingDTO value{
      ProgramId("program.1"),
      ProgramRevision("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
      ProgramBindingId("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
      base.retained_bundle_id,
      base.graph_revision,
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
      {},
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "robot_base"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("joint.1"), ResourceId("joint.1"),
                                 std::string("joint_1"), "joint_1", CoordinateKind::kAngular},
       RuntimeResourceBindingDTO{ResourceRoleId("joint.2"), ResourceId("joint.2"),
                                 std::string("joint_2"), "joint_2", CoordinateKind::kAngular}},
      {},
      RuntimeControlPathDTO{base.control_domain_id,
                            base.control_domain_revision,
                            base.control_binding_id,
                            base.control_binding_revision,
                            ControlInterfaceContractId("control.interface"),
                            ControlInterfaceRevision("control.interface.revision"),
                            base.controller_topology_id,
                            base.topology_revision,
                            base.control_route_id,
                            base.activation_set_id,
                            base.active_control_configuration_id,
                            base.active_control_configuration_generation,
                            target.provider_instance_id,
                            target.provider_manifest_id,
                            target.provider_manifest_revision,
                            target.runtime_unit_id,
                            target.provider_runtime_epoch,
                            StateAuthorityId("state.measured"),
                            StateAuthorityClass::kMeasured,
                            base.stop_evidence_policy_id,
                            base.stop_evidence_policy_revision,
                            ContainmentGranularity::kResource,
                            std::nullopt},
      std::nullopt,
      {},
      {RuntimeUnitEpochBindingDTO{target.runtime_unit_id, target.runtime_unit_epoch}},
      {}};
  auto binding = verify_runtime_participant_binding(std::move(value));
  EXPECT_TRUE(binding.ok());
  return std::move(binding.value());
}

[[nodiscard]] PreparedParticipantStopPlanDTO Plan(const ResolvedStopBasePlanDTO& base,
                                                  const RuntimeParticipantBinding& binding) {
  const auto plan = MakePreparedParticipantStopPlan(
      base,
      PreparedParticipantStopPlanContextDTO{
          OperationId("operation.1"), OperationRevision(4U),
          OperationParticipantId("participant.1"), binding.value().program_id,
          binding.value().program_revision, binding.value().program_binding_id,
          ProgramRunId("program-run.1"), std::nullopt, ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt, MotionInterruptNodeCategory::kGeneric},
      binding.value().resources, {});
  EXPECT_TRUE(plan.ok());
  return plan.value();
}

[[nodiscard]] RemainingStopBudgetDTO Budget(const ResolvedParticipantStopStageDTO& stage,
                                            std::int64_t remaining = 4'000'000'000LL) {
  RemainingStopBudgetDTO result{stage.stop_id, stage.stop_generation, remaining, {}};
  const auto& fixed = stage.participant_stop_plan.deadline_budget;
  const auto elapsed = fixed.outer_duration_ns - remaining;
  for (const auto& phase : fixed.phases) {
    const auto offset =
        remaining <= fixed.kernel_completion_reserve_ns
            ? 0
            : std::max<std::int64_t>(0, std::min(phase.deadline_offset_ns - elapsed,
                                                 remaining - fixed.kernel_completion_reserve_ns));
    result.phases.push_back({phase.phase_id, offset});
  }
  return result;
}

struct ExactFixture final {
  ResolvedStopBasePlanDTO base;
  RuntimeParticipantBinding binding;
  ResolvedParticipantStopStageDTO stage;
};

[[nodiscard]] ExactFixture Fixture(const std::size_t stage_index = 0U) {
  auto base = BasePlan();
  auto binding = Binding(base);
  auto plan = Plan(base, binding);
  const auto& selected = plan.stage_commands.at(stage_index).resolved_stage;
  ResolvedParticipantStopStageDTO stage{StopId("stop.1"),
                                        StopGeneration(7U),
                                        plan.operation_id,
                                        plan.operation_revision,
                                        plan.participant_id,
                                        ControllerGoalId("goal.1"),
                                        ControllerGoalGeneration(3U),
                                        binding.value().controllable_component_id,
                                        binding.value().command_target_id,
                                        binding.value().capability_id,
                                        base.control_domain_id,
                                        base.control_domain_revision,
                                        base.control_binding_id,
                                        base.control_binding_revision,
                                        base.active_control_configuration_id,
                                        base.active_control_configuration_generation,
                                        base.controller_topology_id,
                                        base.topology_revision,
                                        base.control_route_id,
                                        std::nullopt,
                                        std::nullopt,
                                        selected.command_target.provider_runtime_epoch,
                                        {ResourceId("joint.1"), ResourceId("joint.2")},
                                        selected.containment_granularity,
                                        base.stop_evidence_policy_id,
                                        base.stop_evidence_policy_revision,
                                        std::move(plan),
                                        stage_index,
                                        ContentDigest("pending")};
  auto made =
      MakeResolvedParticipantStopStage(stage, Budget(stage), base, binding, {},
                                       MotionInterruptNodeCategory::kGeneric, stage.resources);
  EXPECT_TRUE(made.ok());
  return ExactFixture{std::move(base), std::move(binding), std::move(made.value())};
}

[[nodiscard]] Result<void> ValidateStage(const ExactFixture& fixture,
                                         const ResolvedParticipantStopStageDTO& stage,
                                         const RemainingStopBudgetDTO& budget) {
  return ValidateResolvedParticipantStopStage(stage, budget, fixture.base, fixture.binding, {},
                                              MotionInterruptNodeCategory::kGeneric,
                                              fixture.stage.resources);
}

[[nodiscard]] ParticipantStopStageResultDTO Success(ResolvedParticipantStopStageDTO stage) {
  ParticipantStopStageResultDTO result{std::move(stage),
                                       StopStageAttemptDispositionDTO::kDispatched,
                                       ParticipantStopStageOutcomeDTO::kSucceeded,
                                       MotionControlledStopMode::kSoftOnPath,
                                       std::nullopt,
                                       std::nullopt,
                                       3U,
                                       RuntimeEpoch("provider.epoch"),
                                       9U,
                                       10'000'000,
                                       {},
                                       std::nullopt};
  const auto& requirements = result.request.participant_stop_plan.stage_commands.front()
                                 .resolved_stage.evidence_requirements;
  result.source_epoch = requirements.front().resolved_source.runtime_target.provider_runtime_epoch;
  for (const auto& requirement : requirements) {
    result.evidence_items.push_back(StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
        StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source,
        requirement.resolved_source.runtime_target.provider_runtime_epoch, 9U, 1'000'000,
        JsonValueDTO{true}, std::nullopt});
  }
  return result;
}

[[nodiscard]] Result<void> ValidateResult(const ExactFixture& fixture,
                                          const ParticipantStopStageResultDTO& result) {
  return ValidateParticipantStopStageResult(result, fixture.base, fixture.binding, {},
                                            MotionInterruptNodeCategory::kGeneric,
                                            fixture.stage.resources);
}

TEST(ParticipantStopStageTest, AcceptsExactVerifiedPreparedStageAndShortenedBudget) {
  const auto fixture = Fixture();
  const auto budget = Budget(fixture.stage);
  EXPECT_TRUE(ValidateStage(fixture, fixture.stage, budget).ok());
  EXPECT_TRUE(ValidateResult(fixture, Success(fixture.stage)).ok());
}

TEST(ParticipantStopStageTest, AcceptsRegisteredControlPathProviderWithoutCapabilityBinding) {
  const auto initial = Fixture();
  const auto registered = std::find_if(
      initial.stage.participant_stop_plan.stage_commands.begin(),
      initial.stage.participant_stop_plan.stage_commands.end(), [](const auto& command) {
        return command.resolved_stage.containment_command.command_kind ==
               StopContainmentCommandKind::kRegistered;
      });
  ASSERT_NE(registered, initial.stage.participant_stop_plan.stage_commands.end());
  const auto stage_index = static_cast<std::size_t>(
      std::distance(initial.stage.participant_stop_plan.stage_commands.begin(), registered));
  const auto fixture = Fixture(stage_index);
  EXPECT_FALSE(fixture.stage.capability_provider_binding_id.has_value());
  EXPECT_TRUE(ValidateStage(fixture, fixture.stage, Budget(fixture.stage)).ok());
}

TEST(ParticipantStopStageTest, AcceptsDirectTargetWhenSupportingProviderIsCoLocated) {
  const auto fixture = Fixture();
  auto value = fixture.binding.value();
  const auto& path = *value.control_path;
  value.supporting_capability_provider_paths = {RuntimeCapabilityProviderPathDTO{
      CapabilityId("planning.motion.pose_goal"),
      CapabilityDefinitionRevision("planning.motion.pose_goal.revision"),
      CapabilityProviderBindingId("planning.motion.pose_goal.provider"),
      CapabilityProviderBindingRevision("planning.motion.pose_goal.provider.revision"),
      path.provider_instance_id,
      path.provider_manifest_id,
      path.provider_manifest_revision,
      path.provider_runtime_unit_id,
      path.provider_runtime_epoch}};
  auto colocated = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(colocated.ok()) << colocated.error().message;

  EXPECT_FALSE(fixture.stage.capability_provider_binding_id.has_value());
  EXPECT_TRUE(ValidateResolvedParticipantStopStage(
                  fixture.stage, Budget(fixture.stage), fixture.base, colocated.value(), {},
                  MotionInterruptNodeCategory::kGeneric, fixture.stage.resources)
                  .ok());
}

TEST(ParticipantStopStageTest, RejectsForeignOrExtendedBudgetAndExpandedScope) {
  const auto fixture = Fixture();
  auto budget = Budget(fixture.stage);
  budget.phases.front().remaining_deadline_offset_ns += 1;
  EXPECT_FALSE(ValidateStage(fixture, fixture.stage, budget).ok());

  auto stage = fixture.stage;
  stage.resources.pop_back();
  auto digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  ASSERT_TRUE(digest.ok());
  stage.resolved_stop_scope_policy_digest = digest.value();
  EXPECT_FALSE(ValidateStage(fixture, stage, Budget(stage)).ok());
}

TEST(ParticipantStopStageTest, RejectsMutatedReferentEvenWithRecomputedDigest) {
  const auto fixture = Fixture();
  auto stage = fixture.stage;
  stage.control_route_id = ControlRouteId("route.foreign");
  auto digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  ASSERT_TRUE(digest.ok());
  stage.resolved_stop_scope_policy_digest = digest.value();
  EXPECT_FALSE(ValidateStage(fixture, stage, Budget(stage)).ok());

  stage = fixture.stage;
  stage.participant_stop_plan.stage_commands.front().resolved_stage.containment_scope =
      ResolvedStopResourceContainmentScopeDTO{ActivationSetId("activation.foreign"),
                                              stage.resources};
  digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  ASSERT_TRUE(digest.ok());
  stage.resolved_stop_scope_policy_digest = digest.value();
  EXPECT_FALSE(ValidateStage(fixture, stage, Budget(stage)).ok());
}

TEST(ParticipantStopStageTest, RejectsForeignProviderTargetEvenWithRecomputedDigest) {
  const auto fixture = Fixture();
  auto stage = fixture.stage;
  stage.participant_stop_plan.stage_commands.front()
      .resolved_stage.command_target.provider_manifest_revision = ProviderManifestRevision(
      "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  auto digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  ASSERT_TRUE(digest.ok());
  stage.resolved_stop_scope_policy_digest = digest.value();
  EXPECT_FALSE(ValidateStage(fixture, stage, Budget(stage)).ok());

  stage = fixture.stage;
  stage.capability_provider_binding_id = CapabilityProviderBindingId("provider.binding.foreign");
  stage.capability_provider_binding_revision =
      CapabilityProviderBindingRevision("provider.binding.revision.foreign");
  digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  ASSERT_TRUE(digest.ok());
  stage.resolved_stop_scope_policy_digest = digest.value();
  EXPECT_FALSE(ValidateStage(fixture, stage, Budget(stage)).ok());
}

TEST(ParticipantStopStageTest, SupersededNoEffectRequiresCancellationProvenance) {
  const auto fixture = Fixture();
  auto result = Success(fixture.stage);
  result.attempt_disposition = StopStageAttemptDispositionDTO::kNotDispatched;
  result.outcome = ParticipantStopStageOutcomeDTO::kSupersededNoEffect;
  result.actual_provider_contract_mode.reset();
  result.recoverable_substitution_reason.reset();
  result.controller_acknowledgement_sequence.reset();
  result.source_epoch.reset();
  result.source_sequence.reset();
  result.error = ErrorDTO{"stop_stage_superseded_before_dispatch", "cancelled", {}};
  for (auto& evidence : result.evidence_items) {
    evidence.state = StopEvidenceItemStateDTO::kInapplicable;
    evidence.source_epoch.reset();
    evidence.source_sequence.reset();
    evidence.source_age_at_evaluation_ns.reset();
    evidence.evidence.reset();
    evidence.error = result.error;
  }
  EXPECT_TRUE(ValidateResult(fixture, result).ok());

  result.evidence_items.front().error.reset();
  EXPECT_FALSE(ValidateResult(fixture, result).ok());

  result = Success(fixture.stage);
  result.evidence_items.front().source_epoch = RuntimeEpoch("foreign.epoch");
  EXPECT_FALSE(ValidateResult(fixture, result).ok());
}

TEST(ParticipantStopStageTest, RejectsClaimedSuccessWithoutFreshTypedEvidence) {
  const auto fixture = Fixture();
  auto result = Success(fixture.stage);
  result.evidence_items.front().evidence.reset();
  EXPECT_FALSE(ValidateResult(fixture, result).ok());

  result = Success(fixture.stage);
  result.evidence_items.front().source_age_at_evaluation_ns =
      result.request.participant_stop_plan.stage_commands.front()
          .resolved_stage.evidence_requirements.front()
          .requirement.source_requirement.freshness_budget_ns +
      1;
  EXPECT_FALSE(ValidateResult(fixture, result).ok());
}

TEST(ParticipantStopStageTest, RejectsFailureWithoutStableErrorOrTerminalEvidence) {
  const auto fixture = Fixture();
  auto result = Success(fixture.stage);
  result.outcome = ParticipantStopStageOutcomeDTO::kFailed;
  result.evidence_items.front().state = StopEvidenceItemStateDTO::kFailed;
  result.evidence_items.front().error = ErrorDTO{"evidence_failed", "failed", {}};
  EXPECT_FALSE(ValidateResult(fixture, result).ok());

  result.error = ErrorDTO{"stop_stage_failed", "failed", {}};
  EXPECT_TRUE(ValidateResult(fixture, result).ok());

  result.evidence_items.front().state = StopEvidenceItemStateDTO::kRequiredPending;
  EXPECT_FALSE(ValidateResult(fixture, result).ok());
}

TEST(ParticipantStopStageTest, AllowsModeWithoutSubstitutionProvenanceButRejectsFalseProvenance) {
  const auto fixture = Fixture();
  auto result = Success(fixture.stage);
  EXPECT_TRUE(ValidateResult(fixture, result).ok());

  ASSERT_TRUE(fixture.base.escalation_stages.front()
                  .command_qualification.controlled_substitution_qualification.has_value());
  result.actual_provider_contract_provenance =
      fixture.base.escalation_stages.front()
          .command_qualification.controlled_substitution_qualification;
  EXPECT_FALSE(ValidateResult(fixture, result).ok());

  result.actual_provider_contract_mode = MotionControlledStopMode::kSpringBackHold;
  result.recoverable_substitution_reason = "qualified_spring_back";
  EXPECT_FALSE(ValidateResult(fixture, result).ok());

  result = Success(fixture.stage);
  result.recoverable_substitution_reason = "reason_without_mode_change";
  EXPECT_FALSE(ValidateResult(fixture, result).ok());
}

TEST(ParticipantStopStageTest, RequiresActualModeAfterPhysicalDispatch) {
  const auto fixture = Fixture();
  auto result = Success(fixture.stage);
  result.actual_provider_contract_mode.reset();
  EXPECT_FALSE(ValidateResult(fixture, result).ok());

  result.outcome = ParticipantStopStageOutcomeDTO::kFailed;
  result.error = ErrorDTO{"stop_stage_failed", "failed", {}};
  result.evidence_items.front().state = StopEvidenceItemStateDTO::kFailed;
  result.evidence_items.front().error = ErrorDTO{"evidence_failed", "failed", {}};
  EXPECT_FALSE(ValidateResult(fixture, result).ok());
}

TEST(ParticipantStopStageTest, RejectsUnboundedErrorAndNoDispatchLiveEvidence) {
  const auto fixture = Fixture();
  auto result = Success(fixture.stage);
  result.outcome = ParticipantStopStageOutcomeDTO::kFailed;
  result.error = ErrorDTO{"stop_stage_failed", "failed", {{"detail", std::string(257U, 'x')}}};
  EXPECT_FALSE(ValidateResult(fixture, result).ok());

  result = Success(fixture.stage);
  result.attempt_disposition = StopStageAttemptDispositionDTO::kNotDispatched;
  result.outcome = ParticipantStopStageOutcomeDTO::kFailed;
  result.actual_provider_contract_mode.reset();
  result.controller_acknowledgement_sequence.reset();
  result.source_epoch.reset();
  result.source_sequence.reset();
  result.error = ErrorDTO{"stop_stage_refused", "refused", {}};
  EXPECT_FALSE(ValidateResult(fixture, result).ok());
}

}  // namespace
}  // namespace manyforge::core
