#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_surveillance.hpp>
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
  if (!value.empty() && value.back() == '\n') value.pop_back();
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
      binding.value().resources, {});
  EXPECT_TRUE(plan.ok());
  return std::move(plan.value());
}

[[nodiscard]] ResolvedParticipantStopStageDTO Stage() {
  auto base = BasePlan();
  auto binding = Binding(base);
  auto plan = Plan(base, binding);
  const auto& selected = plan.stage_commands.front().resolved_stage;
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
                                         0U,
                                         ContentDigest(kDigestC)};
}

[[nodiscard]] ParticipantStopStageResultDTO FailedStage() {
  auto stage = Stage();
  ParticipantStopStageResultDTO result{std::move(stage),
                                       StopStageAttemptDispositionDTO::kDispatched,
                                       ParticipantStopStageOutcomeDTO::kFailed,
                                       MotionControlledStopMode::kSoftOnPath,
                                       std::nullopt,
                                       std::nullopt,
                                       3U,
                                       RuntimeEpoch("provider.epoch"),
                                       9U,
                                       10'000'000,
                                       {},
                                       ErrorDTO{"stop_stage_failed", "failed", {}}};
  const auto& selected = result.request.participant_stop_plan.stage_commands.front().resolved_stage;
  result.source_epoch = selected.command_target.provider_runtime_epoch;
  for (const auto& requirement : selected.evidence_requirements) {
    result.evidence_items.push_back(StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
        StopEvidenceItemStateDTO::kFailed, requirement.resolved_source,
        requirement.resolved_source.runtime_target.provider_runtime_epoch, 9U, 1'000'000,
        std::nullopt, ErrorDTO{"stop_evidence_failed", "failed", {}}});
  }
  return result;
}

[[nodiscard]] StopScopeEntryKeyDTO ParticipantKey(const ResolvedParticipantStopStageDTO& stage) {
  return StopScopeEntryKeyDTO{stage.stop_id, stage.stop_generation,
                              StopParticipantScopeKeyDTO{stage.operation_id, stage.participant_id}};
}

[[nodiscard]] StopScopeEntryDTO ScopeEntry() {
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
          stage.provider_runtime_epoch, stage.resources, ContainmentGranularity::kControllerSet},
      StopScopeEntryDispositionDTO::kStoppable,
      StopScopeDispatchStateDTO::kDispatched,
      stage.stop_evidence_policy_id,
      stage.stop_evidence_policy_revision,
      stage.participant_stop_plan.participant_stop_invocation_plan_digest,
      10,
      stage.participant_stop_plan.deadline_budget.outer_duration_ns,
      std::move(phases)};
}

[[nodiscard]] StopCertificateDTO Certificate() {
  auto failed = FailedStage();
  const auto key = ParticipantKey(failed.request);
  auto reduced = ReduceStopTerminal(StopTerminalReductionInputDTO{
      StopCertificateId("certificate.1"),
      StopCertificateRevision(1U),
      failed.request.stop_id,
      failed.request.stop_generation,
      StopRequestedSelectorSnapshotDTO{
          StopRequestedSelectorKindDTO::kOrdinaryOperations, ContentDigest(kDigestA),
          StopSelectorDTO{std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
              OperationId("operation.1"), OperationRevision(3U), StopGeneration(0U)}}},
          std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExactCurrentScope,
      {},
      {StopScopeCaptureHistoryDTO{
          1U,
          StopResolutionDispositionDTO::kExact,
          StopScopeResolutionReasonDTO::kExactCurrentScope,
          ContentDigest(kDigestB),
          {},
          {StopAffectedOperationRevisionDTO{OperationId("operation.1"), OperationRevision(4U)}},
          {ResourceId("joint.1"), ResourceId("joint.2")}}},
      {},
      {ScopeEntry()},
      {StopAttemptedStageResultDTO{key, 1U, 20, StopStageResultAuthorityDTO::kAuthoritative,
                                   std::move(failed)}},
      std::nullopt,
      StopFaultLatchId("latch.1"),
      StopFaultLatchGeneration(1U),
      30,
      ErrorDTO{"stop_failed_uncontained", "failed", {}}});
  EXPECT_TRUE(reduced.ok()) << reduced.error().message;
  return std::move(reduced.value());
}

[[nodiscard]] StopFaultLatchDTO Latch(const StopCertificateDTO& certificate) {
  return StopFaultLatchDTO{
      CellId("cell.1"),
      certificate.stop_id,
      certificate.stop_generation,
      StopCertificateRefDTO{certificate.stop_certificate_id, certificate.stop_certificate_revision,
                            certificate.stop_id, certificate.stop_generation},
      StopFaultLatchId("latch.1"),
      StopFaultLatchGeneration(1U),
      StopFaultTriggerDTO::kFailedUncontained,
      false,
      {OperationId("operation.1")},
      {ResourceId("joint.1"), ResourceId("joint.2")},
      RetainedBundleId(kDigestA),
      GraphRevision(kDigestB),
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                     std::nullopt},
      StopFaultLatchStateDTO::kLatched,
      1U,
      StopFaultRecoveryStatusDTO::kWaitingForObservation,
      "stop_fault_latched",
      {}};
}

[[nodiscard]] StopSurveillanceSourcePlanDTO SourcePlan(const StopCertificateDTO& certificate) {
  const auto& attempted = certificate.attempted_stage_results.front();
  const auto& requirement = attempted.result.request.participant_stop_plan.stage_commands.front()
                                .resolved_stage.evidence_requirements.front();
  return StopSurveillanceSourcePlanDTO{attempted.scope_key, requirement.requirement,
                                       requirement.resolved_source};
}

[[nodiscard]] StopSurveillanceObservationSampleDTO Sample(
    const StopSurveillanceObservationQueryDTO& query, const StopEvidenceItemStateDTO state,
    const std::uint64_t sequence) {
  return StopSurveillanceObservationSampleDTO{
      query.stop_certificate,
      query.scope_key,
      query.evidence_requirement.evidence_item_id,
      query.resolved_source,
      StopSurveillanceSourceAvailabilityDTO::kAvailable,
      StopEvidenceItemDTO{
          query.evidence_requirement.evidence_item_id, query.evidence_requirement.evidence_kind,
          state, query.resolved_source, query.resolved_source.runtime_target.provider_runtime_epoch,
          sequence, 1'000'000,
          state == StopEvidenceItemStateDTO::kSatisfied
              ? std::optional<JsonValueDTO>(JsonValueDTO{true})
              : std::nullopt,
          state == StopEvidenceItemStateDTO::kFailed
              ? std::optional<ErrorDTO>(ErrorDTO{"stop_evidence_failed", "failed", {}})
              : std::nullopt},
      std::nullopt};
}

[[nodiscard]] StopSurveillanceReducerState Seed() {
  const auto certificate = Certificate();
  auto seeded = MakeStopSurveillanceReducerState(
      RequestOwnerEpoch("owner.1"), 1U, certificate, Latch(certificate),
      StopSurveillancePolicyDTO{10'000'000, 100'000'000, 8U}, {SourcePlan(certificate)});
  EXPECT_TRUE(seeded.ok()) << seeded.error().message;
  return std::move(seeded.value());
}

TEST(StopSurveillanceTest, FailedObservationIsMonotonicAndExactReplayIsNoOp) {
  auto state = Seed();
  auto query = MakeStopSurveillanceObservationQuery(state, 0U, 100'000'000);
  ASSERT_TRUE(query.ok());
  auto reduced = ReduceStopSurveillanceObservation(
      state, query.value(), Sample(query.value(), StopEvidenceItemStateDTO::kFailed, 10U));
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().projection.continued_motion_count, 1U);
  EXPECT_EQ(reduced.value().projection.state, StopPostTerminalSurveillanceStateDTO::kMonitoring);

  auto replay = ReduceStopSurveillanceObservation(
      reduced.value(), query.value(),
      Sample(query.value(), StopEvidenceItemStateDTO::kFailed, 10U));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value(), reduced.value());

  auto conflict = Sample(query.value(), StopEvidenceItemStateDTO::kSatisfied, 10U);
  auto rejected = ReduceStopSurveillanceObservation(reduced.value(), query.value(), conflict);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "stop_surveillance_observation_conflict");
}

TEST(StopSurveillanceTest, CompleteFreshSatisfiedSetHonorsContinuousDwell) {
  auto state = Seed();
  auto query = MakeStopSurveillanceObservationQuery(state, 0U, 100'000'000);
  ASSERT_TRUE(query.ok());
  auto reduced = ReduceStopSurveillanceObservation(
      std::move(state), query.value(),
      Sample(query.value(), StopEvidenceItemStateDTO::kSatisfied, 1U));
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().projection.state, StopPostTerminalSurveillanceStateDTO::kMonitoring);

  auto dwell_query = MakeStopSurveillanceObservationQuery(reduced.value(), 0U, 200'000'000);
  ASSERT_TRUE(dwell_query.ok());
  reduced = ReduceStopSurveillanceObservation(
      std::move(reduced.value()), dwell_query.value(),
      Sample(dwell_query.value(), StopEvidenceItemStateDTO::kSatisfied, 2U));
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  EXPECT_EQ(reduced.value().projection.state,
            StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved);
  EXPECT_TRUE(reduced.value().projection.late_quiescence_evidence_digest.has_value());
  EXPECT_TRUE(reduced.value().projection.late_quiescence_observed_at_ns.has_value());
  auto checkpoint = MakeStopSurveillanceCheckpoint(reduced.value());
  ASSERT_TRUE(checkpoint.ok()) << checkpoint.error().message;
  EXPECT_EQ(checkpoint.value(), reduced.value().projection);
  EXPECT_FALSE(MakeStopSurveillanceObservationQuery(reduced.value(), 0U, 210'000'000).ok());
}

TEST(StopSurveillanceTest, ReplayDoesNotAdvanceDwellAndUnavailableBreaksContinuity) {
  auto state = Seed();
  auto first_query = MakeStopSurveillanceObservationQuery(state, 0U, 100'000'000);
  ASSERT_TRUE(first_query.ok());
  auto first = ReduceStopSurveillanceObservation(
      std::move(state), first_query.value(),
      Sample(first_query.value(), StopEvidenceItemStateDTO::kSatisfied, 1U));
  ASSERT_TRUE(first.ok()) << first.error().message;

  auto replay_query = MakeStopSurveillanceObservationQuery(first.value(), 0U, 200'000'000);
  ASSERT_TRUE(replay_query.ok());
  auto replay = ReduceStopSurveillanceObservation(
      first.value(), replay_query.value(),
      Sample(replay_query.value(), StopEvidenceItemStateDTO::kSatisfied, 1U));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value(), first.value());

  const StopSurveillanceObservationSampleDTO unavailable{
      replay_query.value().stop_certificate,
      replay_query.value().scope_key,
      replay_query.value().evidence_requirement.evidence_item_id,
      replay_query.value().resolved_source,
      StopSurveillanceSourceAvailabilityDTO::kUnavailable,
      std::nullopt,
      ErrorDTO{"stop_surveillance_source_unavailable", "unavailable", {}}};
  auto gap = ReduceStopSurveillanceObservation(std::move(replay.value()), replay_query.value(),
                                               unavailable);
  ASSERT_TRUE(gap.ok()) << gap.error().message;
  EXPECT_TRUE(gap.value().latest_evidence.front().has_value());
  EXPECT_FALSE(gap.value().satisfied_since_evaluation_time_ns.front().has_value());

  auto resumed_query = MakeStopSurveillanceObservationQuery(gap.value(), 0U, 210'000'000);
  ASSERT_TRUE(resumed_query.ok());
  auto resumed = ReduceStopSurveillanceObservation(
      std::move(gap.value()), resumed_query.value(),
      Sample(resumed_query.value(), StopEvidenceItemStateDTO::kSatisfied, 2U));
  ASSERT_TRUE(resumed.ok()) << resumed.error().message;
  EXPECT_EQ(resumed.value().projection.state, StopPostTerminalSurveillanceStateDTO::kMonitoring);
}

TEST(StopSurveillanceTest, UnavailableAndStaleObservationsCannotProveQuiescence) {
  auto state = Seed();
  auto query = MakeStopSurveillanceObservationQuery(state, 0U, 100'000'000);
  ASSERT_TRUE(query.ok());
  auto failed = ReduceStopSurveillanceObservation(
      std::move(state), query.value(),
      Sample(query.value(), StopEvidenceItemStateDTO::kFailed, 4U));
  ASSERT_TRUE(failed.ok());

  auto stale = ReduceStopSurveillanceObservation(
      failed.value(), query.value(),
      Sample(query.value(), StopEvidenceItemStateDTO::kSatisfied, 3U));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "stop_surveillance_observation_stale");

  const StopSurveillanceObservationSampleDTO unavailable{
      query.value().stop_certificate,
      query.value().scope_key,
      query.value().evidence_requirement.evidence_item_id,
      query.value().resolved_source,
      StopSurveillanceSourceAvailabilityDTO::kUnavailable,
      std::nullopt,
      ErrorDTO{"stop_surveillance_source_unavailable", "unavailable", {}}};
  auto unavailable_result =
      ReduceStopSurveillanceObservation(std::move(failed.value()), query.value(), unavailable);
  ASSERT_TRUE(unavailable_result.ok()) << unavailable_result.error().message;
  EXPECT_EQ(unavailable_result.value().projection.state,
            StopPostTerminalSurveillanceStateDTO::kMonitoring);
  EXPECT_TRUE(unavailable_result.value().latest_evidence.front().has_value());
  EXPECT_FALSE(unavailable_result.value().satisfied_since_evaluation_time_ns.front().has_value());
}

TEST(StopSurveillanceTest, RestartAdoptionPreservesCheckpointAndRejectsOldSequence) {
  auto state = Seed();
  auto query = MakeStopSurveillanceObservationQuery(state, 0U, 100'000'000);
  ASSERT_TRUE(query.ok());
  auto reduced = ReduceStopSurveillanceObservation(
      std::move(state), query.value(),
      Sample(query.value(), StopEvidenceItemStateDTO::kFailed, 4U));
  ASSERT_TRUE(reduced.ok()) << reduced.error().message;
  auto recovered = reduced.value().projection;
  recovered.checkpoint_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  const auto certificate = Certificate();
  auto latch = Latch(certificate);
  auto adopted = AdoptStopSurveillanceReducerState(RequestOwnerEpoch("owner.2"), certificate, latch,
                                                   recovered, {SourcePlan(certificate)});
  ASSERT_TRUE(adopted.ok()) << adopted.error().message;
  EXPECT_EQ(adopted.value().projection.owner_epoch, RequestOwnerEpoch("owner.2"));
  EXPECT_EQ(adopted.value().projection.continued_motion_count, 1U);
  EXPECT_FALSE(adopted.value().latest_evidence.front().has_value());

  auto resumed_query = MakeStopSurveillanceObservationQuery(adopted.value(), 0U, 110'000'000);
  ASSERT_TRUE(resumed_query.ok());
  auto stale = ReduceStopSurveillanceObservation(
      adopted.value(), resumed_query.value(),
      Sample(resumed_query.value(), StopEvidenceItemStateDTO::kSatisfied, 4U));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "stop_surveillance_observation_stale");
}

}  // namespace
}  // namespace manyforge::core
