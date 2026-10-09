#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <sstream>

namespace manyforge::core {
namespace {

inline constexpr char kDigestA[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
inline constexpr char kDigestB[] =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
inline constexpr char kDigestC[] =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

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
  auto decoded = DecodeResolvedStopBasePlan(GoldenBytes());
  EXPECT_TRUE(decoded.ok());
  // Decode the historical wire fixture, then prepare with the current typed Stop contract.
  auto base = std::move(decoded.value());
  auto& nominal = base.escalation_stages.front();
  const ProviderContractRefDTO contract{
      ProviderContractRoleId("stop_contract"),
      SchemaId(std::string(kMotionControlledStopContractSchemaId)),
      SchemaRevision(std::string(kMotionControlledStopContractSchemaRevision))};
  nominal.containment_command.provider_contract_ref = contract;
  if (nominal.command_qualification.controlled_substitution_contract)
    nominal.command_qualification.controlled_substitution_contract->provider_contract_ref =
        contract;
  JsonValueDTO::Array limits;
  for (const auto* id : {"joint.1", "joint.2"}) {
    limits.emplace_back(JsonObjectDTO{
        {"resource_id", JsonValueDTO{std::string(id)}},
        {"max_deceleration",
         JsonValueDTO{JsonObjectDTO{{"coordinate_kind", JsonValueDTO{"angular_rad_s2"}},
                                    {"value_rad_s2", JsonValueDTO{2.0}}}}},
        {"settle_velocity_threshold",
         JsonValueDTO{JsonObjectDTO{{"coordinate_kind", JsonValueDTO{"angular_rad_s"}},
                                    {"value_rad_s", JsonValueDTO{0.02}}}}},
    });
  }
  nominal.default_contract_parameters = JsonObjectDTO{
      {"stop_mode", JsonValueDTO{"soft_on_path"}},
      {"deceleration_time_s", JsonValueDTO{0.5}},
      {"jerk_limits_enabled", JsonValueDTO{false}},
      {"allow_natural_completion_on_short_path", JsonValueDTO{true}},
      {"per_resource_limits", JsonValueDTO{std::move(limits)}},
  };
  auto current = MakeResolvedStopBasePlan(std::move(base));
  EXPECT_TRUE(current.ok()) << current.error().message;
  return std::move(current.value());
}

[[nodiscard]] RuntimeParticipantBinding Binding(const ResolvedStopBasePlanDTO& base) {
  const auto& target = base.escalation_stages.front().command_target;
  RuntimeParticipantBindingDTO value{
      ProgramId("program.1"),
      ProgramRevision(kDigestA),
      ProgramBindingId(kDigestB),
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
  auto plan = MakePreparedParticipantStopPlan(
      base,
      PreparedParticipantStopPlanContextDTO{
          OperationId("operation.1"), OperationRevision(4U),
          OperationParticipantId("participant.1"), binding.value().program_id,
          binding.value().program_revision, binding.value().program_binding_id,
          ProgramRunId("program-run.1"), std::nullopt, ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt, MotionInterruptNodeCategory::kGeneric},
      binding.value().resources,
      std::vector<HardStopResourceLimitDTO>{{ResourceId("joint.1"), AngularVelocityDTO{0.02}},
                                            {ResourceId("joint.2"), AngularVelocityDTO{0.02}}});
  EXPECT_TRUE(plan.ok()) << plan.error().message;
  return std::move(plan.value());
}

[[nodiscard]] ResolvedParticipantStopStageDTO Stage(std::size_t stage_index = 0U) {
  auto base = BasePlan();
  auto binding = Binding(base);
  auto plan = Plan(base, binding);
  const auto& selected = plan.stage_commands.at(stage_index).resolved_stage;
  return ResolvedParticipantStopStageDTO{StopId("stop.1"),
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
                                         ContentDigest(kDigestC)};
}

[[nodiscard]] ParticipantStopStageResultDTO SuccessfulStage(std::size_t stage_index = 0U) {
  auto stage = Stage(stage_index);
  ParticipantStopStageResultDTO result{std::move(stage),
                                       StopStageAttemptDispositionDTO::kDispatched,
                                       ParticipantStopStageOutcomeDTO::kSucceeded,
                                       std::nullopt,
                                       std::nullopt,
                                       3U,
                                       RuntimeEpoch("provider.epoch"),
                                       9U,
                                       10'000'000,
                                       {},
                                       std::nullopt};
  const auto& selected =
      result.request.participant_stop_plan.stage_commands.at(stage_index).resolved_stage;
  if (stage_index == 0U) {
    result.actual_provider_contract_mode = MotionControlledStopMode::kSoftOnPath;
  }
  result.source_epoch = selected.command_target.provider_runtime_epoch;
  for (const auto& requirement : selected.evidence_requirements) {
    result.evidence_items.push_back(StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
        StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source,
        requirement.resolved_source.runtime_target.provider_runtime_epoch, 9U, 1'000'000,
        JsonValueDTO{true}, std::nullopt});
  }
  return result;
}

[[nodiscard]] StopScopeEntryKeyDTO ParticipantKey(const ResolvedParticipantStopStageDTO& stage) {
  return StopScopeEntryKeyDTO{stage.stop_id, stage.stop_generation,
                              StopParticipantScopeKeyDTO{stage.operation_id, stage.participant_id}};
}

[[nodiscard]] StopScopeEntryDTO ScopeEntry(
    StopScopeDispatchStateDTO dispatch_state = StopScopeDispatchStateDTO::kDispatched) {
  const auto stage = Stage();
  std::vector<StopAcceptedPhaseBudgetDTO> phases;
  for (const auto& phase : stage.participant_stop_plan.deadline_budget.phases) {
    phases.push_back({phase.phase_id, phase.deadline_offset_ns, phase.deadline_offset_ns});
  }
  return StopScopeEntryDTO{
      ParticipantKey(stage),
      StopParticipantScopeFactsDTO{
          OperationRevision(stage.operation_revision.value() + 1U), stage.controllable_component_id,
          stage.command_target_id, stage.capability_id, stage.control_domain_id,
          stage.control_domain_revision, stage.control_binding_id, stage.control_binding_revision,
          stage.controller_goal_id, stage.controller_goal_generation,
          stage.active_control_configuration_id, stage.active_control_configuration_generation,
          stage.controller_topology_id, stage.topology_revision, stage.control_route_id,
          stage.provider_runtime_epoch, stage.resources, ContainmentGranularity::kControllerSet,
          StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled},
      StopScopeEntryDispositionDTO::kStoppable,
      dispatch_state,
      stage.stop_evidence_policy_id,
      stage.stop_evidence_policy_revision,
      stage.participant_stop_plan.participant_stop_invocation_plan_digest,
      10,
      stage.participant_stop_plan.deadline_budget.outer_duration_ns,
      std::move(phases)};
}

[[nodiscard]] StopRequestedSelectorSnapshotDTO OperationSelector() {
  return StopRequestedSelectorSnapshotDTO{
      StopRequestedSelectorKindDTO::kOrdinaryOperations, ContentDigest(kDigestA),
      StopSelectorDTO{std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
          OperationId("operation.1"), OperationRevision(3U), StopGeneration(0U)}}},
      std::nullopt};
}

[[nodiscard]] StopScopeCaptureHistoryDTO Capture(
    std::vector<StopPredecessorAcceptanceRefDTO> predecessors = {},
    std::vector<StopAffectedOperationRevisionDTO> operations = {StopAffectedOperationRevisionDTO{
        OperationId("operation.1"), OperationRevision(4U)}},
    std::vector<ResourceId> resources = {ResourceId("joint.1"), ResourceId("joint.2")},
    StopResolutionDispositionDTO disposition = StopResolutionDispositionDTO::kExact,
    StopScopeResolutionReasonDTO reason = StopScopeResolutionReasonDTO::kExactCurrentScope) {
  return StopScopeCaptureHistoryDTO{1U,
                                    disposition,
                                    reason,
                                    ContentDigest(kDigestB),
                                    std::move(predecessors),
                                    std::move(operations),
                                    std::move(resources)};
}

[[nodiscard]] StopTerminalReductionInputDTO Input() {
  const auto stage = SuccessfulStage();
  const auto key = ParticipantKey(stage.request);
  return StopTerminalReductionInputDTO{
      StopCertificateId("certificate.1"),
      StopCertificateRevision(1U),
      stage.request.stop_id,
      stage.request.stop_generation,
      OperationSelector(),
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExactCurrentScope,
      {},
      {Capture()},
      {},
      {ScopeEntry()},
      {StopAttemptedStageResultDTO{key, 1U, 20, StopStageResultAuthorityDTO::kAuthoritative,
                                   stage}},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      30,
      std::nullopt};
}

TEST(StopTerminalReducerTest, DerivesNominalCertificateAndExactFlattenedEvidence) {
  const auto reduced = ReduceStopTerminal(Input());
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kSucceededNominal);
  EXPECT_TRUE(reduced.value().unsettled.empty());
  ASSERT_EQ(reduced.value().dispatched_stopped.size(), 1U);
  ASSERT_EQ(reduced.value().attempted_stage_results.size(), 1U);
  ASSERT_FALSE(reduced.value().evidence_items.empty());
  EXPECT_EQ(reduced.value().evidence_items.front().result_sequence, 1U);
  EXPECT_FALSE(stop_certificate_field_error(reduced.value()).has_value());
}

TEST(StopTerminalReducerTest,
     RetainsPhysicalContainmentButRefusesSuccessWhenParticipantIsUnsettled) {
  auto input = Input();
  auto& facts =
      std::get<StopParticipantScopeFactsDTO>(input.newly_captured_scope_entries.front().facts);
  facts.terminal_settlement =
      StopParticipantScopeFactsDTO::TerminalSettlementState::kContainedUnsettled;
  input.fault_latch_id = StopFaultLatchId("stop-fault.1");
  input.fault_latch_generation = StopFaultLatchGeneration(1U);
  input.terminal_error =
      ErrorDTO{"participant_stop_settlement_failed", "participant terminal settlement failed", {}};

  const auto reduced = ReduceStopTerminal(input);

  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(reduced.value().dispatched_stopped.size(), 1U);
  EXPECT_EQ(reduced.value().dispatched_stopped.front(),
            input.newly_captured_scope_entries.front().key);
  ASSERT_EQ(reduced.value().unsettled.size(), 1U);
  EXPECT_EQ(reduced.value().unsettled.front(), input.newly_captured_scope_entries.front().key);
  EXPECT_TRUE(reduced.value().failed.empty());
  EXPECT_EQ(reduced.value().terminal_error, input.terminal_error);
  EXPECT_FALSE(stop_certificate_field_error(reduced.value()).has_value());
}

TEST(StopTerminalReducerTest, PendingParticipantSettlementFailsClosedAfterPhysicalContainment) {
  auto input = Input();
  auto& facts =
      std::get<StopParticipantScopeFactsDTO>(input.newly_captured_scope_entries.front().facts);
  facts.terminal_settlement = StopParticipantScopeFactsDTO::TerminalSettlementState::kPending;
  input.fault_latch_id = StopFaultLatchId("stop-fault.1");
  input.fault_latch_generation = StopFaultLatchGeneration(1U);

  const auto reduced = ReduceStopTerminal(input);

  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(reduced.value().dispatched_stopped.size(), 1U);
  ASSERT_EQ(reduced.value().unsettled.size(), 1U);
  EXPECT_EQ(reduced.value().dispatched_stopped.front(), reduced.value().unsettled.front());
  EXPECT_FALSE(stop_certificate_field_error(reduced.value()).has_value());
}

TEST(StopTerminalReducerTest, RejectsScopeThatDoesNotAdvanceBeyondPreparedOperationRevision) {
  auto input = Input();
  auto& facts =
      std::get<StopParticipantScopeFactsDTO>(input.newly_captured_scope_entries.front().facts);
  facts.operation_revision_after_acceptance =
      input.newly_completed_stage_results.front().result.request.operation_revision;
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_FALSE(reduced.ok());
  EXPECT_EQ(reduced.error().code, "stop_terminal_attempt_join_invalid");
}

TEST(StopTerminalReducerTest, PreDispatchCancellationSettlesWithoutEvaluatingPolicy) {
  auto input = Input();
  input.newly_captured_scope_entries.front().dispatch_state =
      StopScopeDispatchStateDTO::kNeverDispatchedCancelled;
  input.newly_completed_stage_results.clear();
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kSucceededNominal);
  EXPECT_EQ(reduced.value().pre_dispatch_cancelled.size(), 1U);
  EXPECT_TRUE(reduced.value().attempted_stage_results.empty());
  EXPECT_TRUE(reduced.value().evidence_items.empty());
}

TEST(StopTerminalReducerTest, PreDispatchCancellationNeedsNoSettlementWithoutAnInnerOccurrence) {
  auto input = Input();
  input.newly_captured_scope_entries.front().dispatch_state =
      StopScopeDispatchStateDTO::kNeverDispatchedCancelled;
  auto& facts =
      std::get<StopParticipantScopeFactsDTO>(input.newly_captured_scope_entries.front().facts);
  facts.terminal_settlement = StopParticipantScopeFactsDTO::TerminalSettlementState::kNotRequired;
  input.newly_completed_stage_results.clear();

  const auto reduced = ReduceStopTerminal(input);

  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kSucceededNominal);
  EXPECT_EQ(reduced.value().pre_dispatch_cancelled.size(), 1U);
  EXPECT_TRUE(reduced.value().unsettled.empty());
}

TEST(StopTerminalReducerTest, PreDispatchCancellationCannotOutrunParticipantSettlement) {
  auto input = Input();
  input.newly_captured_scope_entries.front().dispatch_state =
      StopScopeDispatchStateDTO::kNeverDispatchedCancelled;
  auto& facts =
      std::get<StopParticipantScopeFactsDTO>(input.newly_captured_scope_entries.front().facts);
  facts.terminal_settlement = StopParticipantScopeFactsDTO::TerminalSettlementState::kPending;
  input.newly_completed_stage_results.clear();
  input.fault_latch_id = StopFaultLatchId("stop-fault.1");
  input.fault_latch_generation = StopFaultLatchGeneration(1U);

  const auto reduced = ReduceStopTerminal(input);

  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(reduced.value().pre_dispatch_cancelled.size(), 1U);
  ASSERT_EQ(reduced.value().unsettled.size(), 1U);
  EXPECT_EQ(reduced.value().pre_dispatch_cancelled.front(), reduced.value().unsettled.front());
  EXPECT_TRUE(reduced.value().attempted_stage_results.empty());
}

TEST(StopTerminalReducerTest, StrongerStageProducesEscalatedTruthWithoutRequiringALatch) {
  auto input = Input();
  auto hard = SuccessfulStage(1U);
  input.newly_completed_stage_results.push_back(
      StopAttemptedStageResultDTO{ParticipantKey(hard.request), 2U, 25,
                                  StopStageResultAuthorityDTO::kAuthoritative, std::move(hard)});
  std::sort(input.newly_completed_stage_results.begin(), input.newly_completed_stage_results.end(),
            [](const auto& left, const auto& right) {
              return left.result.request.stage_index < right.result.request.stage_index;
            });
  // A successful fallback hard stop is a degraded Stop, not a cell-wide
  // containment fault (item 25): no latch is required.
  const auto without_latch = ReduceStopTerminal(input);
  ASSERT_TRUE(without_latch.ok()) << without_latch.error().message;
  EXPECT_EQ(without_latch.value().result, StopCertificateResultDTO::kSucceededEscalated);
  EXPECT_FALSE(without_latch.value().fault_latch_id.has_value());

  // A latch held for another reason (for example a collision on the Stop
  // path) is still recorded on the escalated certificate.
  input.fault_latch_id = StopFaultLatchId("stop-fault.1");
  input.fault_latch_generation = StopFaultLatchGeneration(1U);
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kSucceededEscalated);
}

TEST(StopTerminalReducerTest, SupersededLateSuccessCannotStealTerminalAuthority) {
  auto input = Input();
  auto failed = SuccessfulStage();
  failed.outcome = ParticipantStopStageOutcomeDTO::kFailed;
  failed.error = ErrorDTO{"stop_stage_failed", "failed", {}};
  for (auto& evidence : failed.evidence_items) {
    evidence.state = StopEvidenceItemStateDTO::kFailed;
    evidence.error = ErrorDTO{"stop_evidence_failed", "failed", {}};
  }
  auto late = SuccessfulStage();
  input.newly_completed_stage_results = {
      StopAttemptedStageResultDTO{ParticipantKey(failed.request), 1U, 20,
                                  StopStageResultAuthorityDTO::kAuthoritative, std::move(failed)},
      StopAttemptedStageResultDTO{ParticipantKey(late.request), 2U, 25,
                                  StopStageResultAuthorityDTO::kSupersededLate, std::move(late)}};
  input.fault_latch_id = StopFaultLatchId("stop-fault.1");
  input.fault_latch_generation = StopFaultLatchGeneration(1U);
  input.terminal_error = ErrorDTO{"stop_failed_uncontained", "failed", {}};
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kFailedUncontained);
  EXPECT_EQ(reduced.value().failed.size(), 1U);
}

TEST(StopTerminalReducerTest, ExplicitFullMergesPredecessorWithoutRepeatingStage) {
  const auto predecessor = ReduceStopTerminal(Input());
  ASSERT_TRUE(predecessor.ok());
  const StopPredecessorAcceptanceRefDTO predecessor_acceptance{predecessor.value().stop_id,
                                                               predecessor.value().stop_generation,
                                                               ContentDigest(kDigestB), 500};
  auto input = StopTerminalReductionInputDTO{
      StopCertificateId("certificate.full"),
      StopCertificateRevision(1U),
      StopId("stop.full"),
      StopGeneration(8U),
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       ContentDigest(kDigestC), std::nullopt, std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop,
      {},
      {Capture({predecessor_acceptance}, {}, {}, StopResolutionDispositionDTO::kExact,
               StopScopeResolutionReasonDTO::kExplicitFullStop)},
      {predecessor.value()},
      {},
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      40,
      std::nullopt};
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  ASSERT_EQ(reduced.value().predecessor_certificates.size(), 1U);
  EXPECT_EQ(reduced.value().scope_entries, predecessor.value().scope_entries);
  EXPECT_EQ(reduced.value().attempted_stage_results, predecessor.value().attempted_stage_results);
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kSucceededNominal);
}

TEST(StopTerminalReducerTest, ExternalUncontainedResourceFailsWithoutInventedParticipant) {
  auto input = Input();
  const ResourceId resource("external.resource");
  input.requested_selector = StopRequestedSelectorSnapshotDTO{
      StopRequestedSelectorKindDTO::kOrdinaryResources, ContentDigest(kDigestA),
      StopSelectorDTO{std::vector<StopResourceTargetDTO>{
          StopResourceTargetDTO{resource, GraphRevision("graph.1"), 1U, StopGeneration(0U)}}},
      std::nullopt};
  input.capture_history = {Capture({}, {}, {resource})};
  input.newly_captured_scope_entries = {
      StopScopeEntryDTO{StopScopeEntryKeyDTO{input.stop_id, input.stop_generation, resource},
                        resource,
                        StopScopeEntryDispositionDTO::kNotStoppable,
                        StopScopeDispatchStateDTO::kEffectUncertain,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        10,
                        500,
                        {}}};
  input.newly_completed_stage_results.clear();
  input.fault_latch_id = StopFaultLatchId("stop-fault.1");
  input.fault_latch_generation = StopFaultLatchGeneration(1U);
  input.terminal_error = ErrorDTO{"external_resource_uncontained", "failed", {}};
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kFailedUncontained);
  EXPECT_TRUE(
      std::holds_alternative<ResourceId>(reduced.value().scope_entries.front().key.subject));
  EXPECT_TRUE(reduced.value().attempted_stage_results.empty());
}

}  // namespace
}  // namespace manyforge::core
