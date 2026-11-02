#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/stop_terminal_state.hpp>
#include "../src/stop_terminal_reducer_internal.hpp"
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
      10};
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

TEST(StopTerminalReducerTest, NonproofReductionKeepsOneToOneFreshScope) {
  const auto input = Input();
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  ASSERT_EQ(input.capture_history.back().newly_fenced_operation_revisions.size(), 1U);
  EXPECT_EQ(input.capture_history.back().newly_fenced_operation_revisions.front().operation_id,
            OperationId("operation.1"));
  ASSERT_EQ(reduced.value().scope_entries.size(), 1U);
  EXPECT_EQ(reduced.value().scope_entries.front().key,
            input.newly_captured_scope_entries.front().key);
  EXPECT_TRUE(reduced.value().predecessor_certificates.empty());
}

TEST(StopTerminalReducerTest,
     RetainsPhysicalContainmentButRefusesSuccessWhenParticipantIsUnsettled) {
  auto input = Input();
  auto& facts =
      std::get<StopParticipantScopeFactsDTO>(input.newly_captured_scope_entries.front().facts);
  facts.terminal_settlement =
      StopParticipantScopeFactsDTO::TerminalSettlementState::kContainedUnsettled;
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

  const auto reduced = ReduceStopTerminal(input);

  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(reduced.value().pre_dispatch_cancelled.size(), 1U);
  ASSERT_EQ(reduced.value().unsettled.size(), 1U);
  EXPECT_EQ(reduced.value().pre_dispatch_cancelled.front(), reduced.value().unsettled.front());
  EXPECT_TRUE(reduced.value().attempted_stage_results.empty());
}

TEST(StopTerminalReducerTest, StrongerStageProducesEscalatedTruth) {
  auto input = Input();
  auto hard = SuccessfulStage(1U);
  input.newly_completed_stage_results.push_back(
      StopAttemptedStageResultDTO{ParticipantKey(hard.request), 2U, 25,
                                  StopStageResultAuthorityDTO::kAuthoritative, std::move(hard)});
  std::sort(input.newly_completed_stage_results.begin(), input.newly_completed_stage_results.end(),
            [](const auto& left, const auto& right) {
              return left.result.request.stage_index < right.result.request.stage_index;
            });
  // A successful fallback hard stop is a degraded Stop, not a cell fault.
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
                                                               ContentDigest(kDigestB)};
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
      40,
      std::nullopt};
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  ASSERT_EQ(reduced.value().predecessor_certificates.size(), 1U);
  EXPECT_EQ(reduced.value().scope_entries, predecessor.value().scope_entries);
  EXPECT_EQ(reduced.value().attempted_stage_results, predecessor.value().attempted_stage_results);
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kSucceededNominal);
}

TEST(StopTerminalReducerTest, ProofProjectionKeepsExactOriginRowsFromPartialPromotedAncestor) {
  const auto retarget = [](StopTerminalReductionInputDTO input, StopId stop_id,
                           StopGeneration generation, StopCertificateId certificate_id,
                           OperationId operation_id) {
    input.stop_id = stop_id;
    input.stop_generation = generation;
    input.stop_certificate_id = std::move(certificate_id);
    auto& stage = input.newly_completed_stage_results.front().result.request;
    stage.stop_id = stop_id;
    stage.stop_generation = generation;
    stage.operation_id = operation_id;
    input.newly_captured_scope_entries.front().key = ParticipantKey(stage);
    input.newly_completed_stage_results.front().scope_key = ParticipantKey(stage);
    input.capture_history.front().newly_fenced_operation_revisions.front().operation_id = operation_id;
    auto& targets = std::get<std::vector<StopOperationTargetDTO>>(
        *input.requested_selector.selector);
    targets.front().operation_id = operation_id;
    return input;
  };
  auto a_input = retarget(Input(), StopId("stop.origin.a"), StopGeneration(1U),
                          StopCertificateId("certificate.origin.a"), OperationId("operation.1"));
  const auto a = ReduceStopTerminal(a_input);
  ASSERT_TRUE(a.ok()) << a.error().message;

  auto b_input = retarget(Input(), StopId("stop.origin.b"), StopGeneration(2U),
                          StopCertificateId("certificate.origin.b"), OperationId("operation.2"));
  auto& b_facts =
      std::get<StopParticipantScopeFactsDTO>(b_input.newly_captured_scope_entries.front().facts);
  b_facts.terminal_settlement =
      StopParticipantScopeFactsDTO::TerminalSettlementState::kContainedUnsettled;
  b_input.terminal_error =
      ErrorDTO{"participant_stop_settlement_failed", "B was not settled", {{"operation_id", "operation.2"}}};
  const auto b = ReduceStopTerminal(b_input);
  ASSERT_TRUE(b.ok()) << b.error().message;
  ASSERT_EQ(b.value().result, StopCertificateResultDTO::kPartial);

  auto p2_input = Input();
  p2_input.stop_id = StopId("stop.promoted.p2");
  p2_input.stop_generation = StopGeneration(3U);
  p2_input.stop_certificate_id = StopCertificateId("certificate.promoted.p2");
  p2_input.requested_selector = StopRequestedSelectorSnapshotDTO{
      StopRequestedSelectorKindDTO::kExplicitFull, ContentDigest(kDigestC), std::nullopt,
      std::nullopt};
  p2_input.resolution_reason = StopScopeResolutionReasonDTO::kExplicitFullStop;
  p2_input.newly_captured_scope_entries.clear();
  p2_input.newly_completed_stage_results.clear();
  p2_input.capture_history = {Capture(
      {{a.value().stop_id, a.value().stop_generation, ContentDigest(kDigestA)},
       {b.value().stop_id, b.value().stop_generation, ContentDigest(kDigestB)}},
      {}, {}, StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop)};
  p2_input.predecessor_certificates = {a.value(), b.value()};
  const auto p2 = ReduceStopTerminal(p2_input);
  ASSERT_TRUE(p2.ok()) << p2.error().message;
  ASSERT_EQ(p2.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(p2.value().scope_entries.size(), 2U);
  ASSERT_EQ(p2.value().predecessor_certificates.size(), 2U);
  EXPECT_TRUE(std::any_of(p2.value().predecessor_certificates.begin(),
                          p2.value().predecessor_certificates.end(), [&](const auto& ref) {
                            return ref.stop_id == a.value().stop_id;
                          }));

  auto next = Input();
  next.stop_id = StopId("stop.next.a");
  next.stop_generation = StopGeneration(4U);
  next.stop_certificate_id = StopCertificateId("certificate.next.a");
  next.newly_captured_scope_entries.clear();
  next.newly_completed_stage_results.clear();
  const auto ref_for = [](const StopCertificateDTO& cert) {
    return StopPredecessorAcceptanceRefDTO{cert.stop_id, cert.stop_generation,
                                           ContentDigest(kDigestC)};
  };
  next.capture_history = {Capture(
      {ref_for(a.value()), ref_for(p2.value())}, {}, {},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExactCurrentScope)};
  next.predecessor_certificates = {p2.value()};
  const auto a_row = a.value().scope_entries.front().key;
  const internal::StopProofPredecessorScope proof{
      StopCertificateRefDTO{p2.value().stop_certificate_id,
                            p2.value().stop_certificate_revision,
                            p2.value().stop_id, p2.value().stop_generation},
      {a_row}};
  const auto reused = internal::ReduceStopTerminalWithProofScopes(next, {&proof, 1U});
  ASSERT_TRUE(reused.ok()) << reused.error().message;
  EXPECT_EQ(reused.value().result, StopCertificateResultDTO::kSucceededNominal);
  ASSERT_EQ(reused.value().scope_entries.size(), 1U);
  EXPECT_EQ(reused.value().scope_entries.front().key, a_row);
  ASSERT_EQ(reused.value().attempted_stage_results.size(), 1U);
  EXPECT_EQ(reused.value().attempted_stage_results.front().scope_key, a_row);
  ASSERT_EQ(reused.value().predecessor_certificates.size(), 2U);
  EXPECT_TRUE(std::any_of(reused.value().predecessor_certificates.begin(),
                          reused.value().predecessor_certificates.end(), [&](const auto& ref) {
                            return ref.stop_id == a.value().stop_id;
                          }));
  EXPECT_TRUE(std::any_of(reused.value().predecessor_certificates.begin(),
                          reused.value().predecessor_certificates.end(), [&](const auto& ref) {
                            return ref.stop_id == p2.value().stop_id;
                          }));
  EXPECT_TRUE(std::none_of(reused.value().predecessor_certificates.begin(),
                           reused.value().predecessor_certificates.end(), [&](const auto& ref) {
                             return ref.stop_id == b.value().stop_id;
                           }));
  ASSERT_EQ(reused.value().capture_history.back().predecessor_stops.size(), 2U);
  EXPECT_TRUE(std::any_of(reused.value().capture_history.back().predecessor_stops.begin(),
                          reused.value().capture_history.back().predecessor_stops.end(),
                          [&](const auto& ref) { return ref.predecessor_stop_id == a.value().stop_id; }));
  EXPECT_TRUE(std::any_of(reused.value().capture_history.back().predecessor_stops.begin(),
                          reused.value().capture_history.back().predecessor_stops.end(),
                          [&](const auto& ref) { return ref.predecessor_stop_id == p2.value().stop_id; }));

  auto over_cap = next;
  const auto p2_acceptance = ref_for(p2.value());
  over_cap.capture_history = {Capture(
      std::vector<StopPredecessorAcceptanceRefDTO>(65U, p2_acceptance), {}, {},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExactCurrentScope)};
  over_cap.predecessor_certificates.assign(65U, p2.value());
  const auto rejected_over_cap =
      internal::ReduceStopTerminalWithProofScopes(over_cap, {&proof, 1U});
  ASSERT_FALSE(rejected_over_cap.ok());
  EXPECT_EQ(rejected_over_cap.error().code, "stop_terminal_reduction_input_invalid");

  auto c_input = retarget(Input(), StopId("stop.origin.c"), StopGeneration(5U),
                          StopCertificateId("certificate.origin.c"), OperationId("operation.3"));
  const auto c = ReduceStopTerminal(c_input);
  ASSERT_TRUE(c.ok()) << c.error().message;

  auto aggregate = Input();
  aggregate.stop_id = StopId("stop.aggregate.p3");
  aggregate.stop_generation = StopGeneration(6U);
  aggregate.stop_certificate_id = StopCertificateId("certificate.aggregate.p3");
  aggregate.requested_selector = StopRequestedSelectorSnapshotDTO{
      StopRequestedSelectorKindDTO::kExplicitFull, ContentDigest(kDigestC), std::nullopt,
      std::nullopt};
  aggregate.resolution_reason = StopScopeResolutionReasonDTO::kExplicitFullStop;
  aggregate.newly_captured_scope_entries.clear();
  aggregate.newly_completed_stage_results.clear();
  aggregate.capture_history = {Capture(
      {ref_for(a.value()), ref_for(c.value()), ref_for(p2.value())}, {}, {},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop)};
  aggregate.predecessor_certificates = {c.value(), p2.value()};
  const internal::StopProofPredecessorScope aggregate_proof{
      proof.certificate, {a_row}};
  const auto mixed =
      internal::ReduceStopTerminalWithProofScopes(aggregate, {&aggregate_proof, 1U});
  ASSERT_TRUE(mixed.ok()) << mixed.error().message;
  EXPECT_EQ(mixed.value().result, StopCertificateResultDTO::kSucceededNominal);
  ASSERT_EQ(mixed.value().scope_entries.size(), 2U);
  EXPECT_TRUE(std::any_of(mixed.value().scope_entries.begin(), mixed.value().scope_entries.end(),
                          [&](const auto& row) { return row.key == a_row; }));
  EXPECT_TRUE(std::any_of(mixed.value().scope_entries.begin(), mixed.value().scope_entries.end(),
                          [&](const auto& row) {
                            return row.key == c.value().scope_entries.front().key;
                          }));
  EXPECT_TRUE(std::none_of(mixed.value().scope_entries.begin(), mixed.value().scope_entries.end(),
                           [&](const auto& row) {
                             return row.key == b.value().scope_entries.front().key;
                           }));

  auto full_aggregate = aggregate;
  full_aggregate.capture_history = {Capture(
      {ref_for(a.value()), ref_for(b.value()), ref_for(c.value()), ref_for(p2.value())}, {}, {},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop)};
  const auto full_union = ReduceStopTerminal(full_aggregate);
  ASSERT_TRUE(full_union.ok()) << full_union.error().message;
  EXPECT_EQ(full_union.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(full_union.value().scope_entries.size(), 3U);
  EXPECT_TRUE(std::any_of(full_union.value().scope_entries.begin(), full_union.value().scope_entries.end(),
                          [&](const auto& row) {
                            return row.key == b.value().scope_entries.front().key;
                          }));

  const auto stale_b_row = b.value().scope_entries.front().key;
  const internal::StopProofPredecessorScope stale_b_proof{
      proof.certificate, {stale_b_row}};
  const auto rejected = internal::ReduceStopTerminalWithProofScopes(next, {&stale_b_proof, 1U});
  EXPECT_FALSE(rejected.ok());

  auto unsettled_capture = next;
  unsettled_capture.capture_history = {Capture(
      {ref_for(b.value())}, {}, {}, StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExactCurrentScope)};
  unsettled_capture.predecessor_certificates = {b.value()};
  const internal::StopProofPredecessorScope unsettled_proof{
      StopCertificateRefDTO{b.value().stop_certificate_id, b.value().stop_certificate_revision,
                            b.value().stop_id, b.value().stop_generation},
      {b.value().scope_entries.front().key}};
  const auto unsettled =
      internal::ReduceStopTerminalWithProofScopes(unsettled_capture, {&unsettled_proof, 1U});
  EXPECT_FALSE(unsettled.ok());
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
                        10}};
  input.newly_completed_stage_results.clear();
  input.terminal_error = ErrorDTO{"external_resource_uncontained", "failed", {}};
  const auto reduced = ReduceStopTerminal(input);
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().result, StopCertificateResultDTO::kFailedUncontained);
  EXPECT_TRUE(
      std::holds_alternative<ResourceId>(reduced.value().scope_entries.front().key.subject));
  EXPECT_TRUE(reduced.value().attempted_stage_results.empty());
}

// Lean Stop record helpers: DeriveStopCertificateEvidence and the terminal
// status rest/rebuild pair must reproduce exactly what the reducer and the
// Kernel produce, so a lean terminal record decodes to the same values.

[[nodiscard]] StopTerminalReductionInputDTO EscalatedMultiEvidenceInput() {
  auto input = Input();
  auto hard = SuccessfulStage(1U);
  input.newly_completed_stage_results.push_back(
      StopAttemptedStageResultDTO{ParticipantKey(hard.request), 2U, 25,
                                  StopStageResultAuthorityDTO::kAuthoritative, std::move(hard)});
  // A second evidence item on the nominal attempt, appended after the policy
  // item but sorting before it, so the helper's canonical sort is exercised.
  auto& nominal = input.newly_completed_stage_results.front().result;
  auto extra = nominal.evidence_items.front();
  extra.evidence_item_id = StopEvidenceItemId("evidence.nominal.extra");
  nominal.evidence_items.push_back(std::move(extra));
  return input;
}

[[nodiscard]] StopTerminalReductionInputDTO PartialInput() {
  auto input = Input();
  auto& facts =
      std::get<StopParticipantScopeFactsDTO>(input.newly_captured_scope_entries.front().facts);
  facts.terminal_settlement =
      StopParticipantScopeFactsDTO::TerminalSettlementState::kContainedUnsettled;
  input.terminal_error =
      ErrorDTO{"participant_stop_settlement_failed", "participant terminal settlement failed", {}};
  return input;
}

[[nodiscard]] StopTerminalReductionInputDTO FailedInput() {
  auto input = Input();
  auto& failed = input.newly_completed_stage_results.front().result;
  failed.outcome = ParticipantStopStageOutcomeDTO::kFailed;
  failed.error = ErrorDTO{"stop_stage_failed", "failed", {}};
  for (auto& evidence : failed.evidence_items) {
    evidence.state = StopEvidenceItemStateDTO::kFailed;
    evidence.error = ErrorDTO{"stop_evidence_failed", "failed", {}};
  }
  input.terminal_error = ErrorDTO{"stop_failed_uncontained", "failed", {}};
  return input;
}

[[nodiscard]] StopCertificateDTO PredecessorMergeCertificate() {
  const auto predecessor = ReduceStopTerminal(EscalatedMultiEvidenceInput());
  EXPECT_TRUE(predecessor.ok()) << predecessor.error().message;
  const StopPredecessorAcceptanceRefDTO acceptance{predecessor.value().stop_id,
                                                   predecessor.value().stop_generation,
                                                   ContentDigest(kDigestB)};
  auto merged = ReduceStopTerminal(StopTerminalReductionInputDTO{
      StopCertificateId("certificate.full"),
      StopCertificateRevision(1U),
      StopId("stop.full"),
      StopGeneration(8U),
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       ContentDigest(kDigestC), std::nullopt, std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop,
      {},
      {Capture({acceptance}, {}, {}, StopResolutionDispositionDTO::kExact,
               StopScopeResolutionReasonDTO::kExplicitFullStop)},
      {predecessor.value()},
      {},
      {},
      std::nullopt,
      40,
      std::nullopt});
  EXPECT_TRUE(merged.ok()) << merged.error().message;
  EXPECT_EQ(merged.value().predecessor_certificates.size(), 1U);
  return std::move(merged.value());
}

[[nodiscard]] StopCertificateDTO Reduced(const StopTerminalReductionInputDTO& input) {
  auto reduced = ReduceStopTerminal(input);
  EXPECT_TRUE(reduced.ok()) << reduced.error().message;
  return std::move(reduced.value());
}

void ExpectEvidenceEqualsReducer(const StopCertificateDTO& certificate) {
  ASSERT_FALSE(stop_certificate_field_error(certificate).has_value());
  const auto derived = DeriveStopCertificateEvidence(certificate.attempted_stage_results);
  ASSERT_TRUE(derived.ok()) << derived.error().message;
  EXPECT_EQ(derived.value(), certificate.evidence_items);
}

TEST(StopTerminalReducerTest, EvidenceHelperEqualsReducerEvidence) {
  const auto nominal = Reduced(Input());
  ASSERT_EQ(nominal.result, StopCertificateResultDTO::kSucceededNominal);
  ASSERT_EQ(nominal.evidence_items.size(), 1U);
  ExpectEvidenceEqualsReducer(nominal);

  const auto escalated = Reduced(EscalatedMultiEvidenceInput());
  ASSERT_EQ(escalated.result, StopCertificateResultDTO::kSucceededEscalated);
  ASSERT_EQ(escalated.attempted_stage_results.size(), 2U);
  ASSERT_EQ(escalated.evidence_items.size(), 3U);
  EXPECT_EQ(escalated.evidence_items.front().evidence_item.evidence_item_id,
            StopEvidenceItemId("evidence.nominal.extra"));
  EXPECT_EQ(escalated.evidence_items.back().stage_index, 1U);
  ExpectEvidenceEqualsReducer(escalated);

  ExpectEvidenceEqualsReducer(Reduced(PartialInput()));
  ExpectEvidenceEqualsReducer(Reduced(FailedInput()));

  const auto merged = PredecessorMergeCertificate();
  ASSERT_EQ(merged.evidence_items.size(), 3U);
  ExpectEvidenceEqualsReducer(merged);

  const auto empty = DeriveStopCertificateEvidence({});
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty.value().empty());
}

TEST(StopTerminalReducerTest, EvidenceHelperRefusesStageIndexOutsidePlan) {
  auto attempts = Input().newly_completed_stage_results;
  auto& request = attempts.front().result.request;
  request.stage_index = request.participant_stop_plan.stage_commands.size();
  const auto derived = DeriveStopCertificateEvidence(attempts);
  ASSERT_FALSE(derived.ok());
  EXPECT_EQ(derived.error().code, "stop_terminal_attempt_join_invalid");
}

TEST(StopTerminalReducerTest, EvidenceHelperAndReducerRefuseDuplicateEvidence) {
  auto input = Input();
  auto& evidence = input.newly_completed_stage_results.front().result.evidence_items;
  evidence.push_back(evidence.front());

  const auto derived = DeriveStopCertificateEvidence(input.newly_completed_stage_results);
  ASSERT_FALSE(derived.ok());
  EXPECT_EQ(derived.error().code, "stop_terminal_evidence_conflict");

  const auto reduced = ReduceStopTerminal(input);
  ASSERT_FALSE(reduced.ok());
  EXPECT_EQ(reduced.error().code, "stop_terminal_evidence_conflict");
}

[[nodiscard]] StopCertificateRefDTO RefOf(const StopCertificateDTO& certificate) {
  return StopCertificateRefDTO{certificate.stop_certificate_id,
                               certificate.stop_certificate_revision, certificate.stop_id,
                               certificate.stop_generation};
}

[[nodiscard]] StopDurableProjectionStatusDTO Projection(StopDurableProjectionStateDTO state) {
  return StopDurableProjectionStatusDTO{state, std::nullopt, std::nullopt};
}

[[nodiscard]] StopStatusDTO TerminalStatus(const StopCertificateDTO& certificate,
                                           StopStatusStateDTO state,
                                           StopDurableProjectionStateDTO terminal_projection) {
  return StopStatusDTO{
      StopAcceptedDTO{RequestId("request.1"), RequestReplayStatusDTO::kAcceptedNew,
                      certificate.stop_id, certificate.stop_generation,
                      certificate.requested_selector.requested_selector_digest,
                      certificate.resolution_disposition, certificate.resolution_reason,
                      certificate.stale_diagnostics, ContentDigest(kDigestB), {}, {}, 10},
      state,
      certificate.scope_entries,
      {},
      certificate.attempted_stage_results,
      certificate.evidence_items,
      certificate.unsettled,
      certificate.failed,
      Projection(StopDurableProjectionStateDTO::kRecorded),
      Projection(terminal_projection),
      RefOf(certificate)};
}

void ExpectTerminalStatusRoundTrip(const StopStatusDTO& status,
                                   const StopCertificateDTO& certificate) {
  const auto field = stop_terminal_projection_field_error(status, certificate);
  ASSERT_FALSE(field.has_value()) << *field;
  const auto rest = TerminalStopStatusRest(status);
  EXPECT_EQ(rest.accepted, status.accepted);
  EXPECT_EQ(rest.current_stages, status.current_stages);
  EXPECT_EQ(rest.acceptance_projection, status.acceptance_projection);
  EXPECT_EQ(rest.terminal_projection, status.terminal_projection);
  EXPECT_EQ(TerminalStopStatusFromCertificate(rest, certificate), status);
}

TEST(StopTerminalReducerTest, TerminalStatusRebuildsFromRestAndCertificate) {
  const auto nominal = Reduced(Input());
  ExpectTerminalStatusRoundTrip(TerminalStatus(nominal, StopStatusStateDTO::kSucceeded,
                                               StopDurableProjectionStateDTO::kPending),
                                nominal);
  ExpectTerminalStatusRoundTrip(TerminalStatus(nominal, StopStatusStateDTO::kSucceeded,
                                               StopDurableProjectionStateDTO::kRecorded),
                                nominal);

  const auto escalated = Reduced(EscalatedMultiEvidenceInput());
  ExpectTerminalStatusRoundTrip(TerminalStatus(escalated, StopStatusStateDTO::kSucceeded,
                                               StopDurableProjectionStateDTO::kRecorded),
                                escalated);

  const auto partial = Reduced(PartialInput());
  ASSERT_EQ(partial.result, StopCertificateResultDTO::kPartial);
  ExpectTerminalStatusRoundTrip(TerminalStatus(partial, StopStatusStateDTO::kPartial,
                                               StopDurableProjectionStateDTO::kPending),
                                partial);

  const auto failed = Reduced(FailedInput());
  ASSERT_EQ(failed.result, StopCertificateResultDTO::kFailedUncontained);
  ExpectTerminalStatusRoundTrip(TerminalStatus(failed, StopStatusStateDTO::kFailed,
                                               StopDurableProjectionStateDTO::kRecorded),
                                failed);

  const auto merged = PredecessorMergeCertificate();
  ExpectTerminalStatusRoundTrip(TerminalStatus(merged, StopStatusStateDTO::kSucceeded,
                                               StopDurableProjectionStateDTO::kPending),
                                merged);
}

}  // namespace
}  // namespace manyforge::core
