#include "io_rule_outcome_fixture.hpp"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <sstream>

namespace manyforge::core {
namespace {

inline constexpr char kDigestA[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
inline constexpr char kDigestB[] =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

[[nodiscard]] ResolvedStopBasePlanDTO BasePlan() {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << stream.rdbuf();
  auto value = bytes.str();
  if (!value.empty() && value.back() == '\n') value.pop_back();
  auto decoded = DecodeResolvedStopBasePlan(value);
  EXPECT_TRUE(decoded.ok());
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

[[nodiscard]] ParticipantStopStageResultDTO SuccessfulStage(
    std::optional<MotionInterruptStopProfileDTO> profile = std::nullopt) {
  auto base = BasePlan();
  auto binding = Binding(base);
  auto plan = MakePreparedParticipantStopPlan(
      base,
      PreparedParticipantStopPlanContextDTO{
          OperationId("operation.1"), OperationRevision(4U),
          OperationParticipantId("participant.1"), binding.value().program_id,
          binding.value().program_revision, binding.value().program_binding_id,
          ProgramRunId("program-run.1"), profile,
          profile ? ParticipantStopProfilePurposeDTO::kOperator
                  : ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt, MotionInterruptNodeCategory::kGeneric},
      binding.value().resources,
      std::vector<HardStopResourceLimitDTO>{{ResourceId("joint.1"), AngularVelocityDTO{0.02}},
                                            {ResourceId("joint.2"), AngularVelocityDTO{0.02}}});
  EXPECT_TRUE(plan.ok());
  const auto& selected = plan.value().stage_commands.front().resolved_stage;
  const auto provider_runtime_epoch = selected.command_target.provider_runtime_epoch;
  const auto evidence_requirements = selected.evidence_requirements;
  ResolvedParticipantStopStageDTO stage{StopId("stop.1"),
                                        StopGeneration(7U),
                                        plan.value().operation_id,
                                        plan.value().operation_revision,
                                        plan.value().participant_id,
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
                                        provider_runtime_epoch,
                                        {ResourceId("joint.1"), ResourceId("joint.2")},
                                        selected.containment_granularity,
                                        base.stop_evidence_policy_id,
                                        base.stop_evidence_policy_revision,
                                        std::move(plan.value()),
                                        0U,
                                        ContentDigest(kDigestA)};
  ParticipantStopStageResultDTO result{std::move(stage),
                                       StopStageAttemptDispositionDTO::kDispatched,
                                       ParticipantStopStageOutcomeDTO::kSucceeded,
                                       MotionControlledStopMode::kSoftOnPath,
                                       std::nullopt,
                                       3U,
                                       provider_runtime_epoch,
                                       9U,
                                       10'000'000,
                                       {},
                                       std::nullopt};
  for (const auto& requirement : evidence_requirements) {
    result.evidence_items.push_back(StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
        StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source,
        requirement.resolved_source.runtime_target.provider_runtime_epoch, 9U, 1'000'000,
        JsonValueDTO{true}, std::nullopt});
  }
  return result;
}

[[nodiscard]] StopJournalTerminalProjectionDTO TerminalProjection(
    std::optional<MotionInterruptStopProfileDTO> profile = std::nullopt) {
  auto stage_result = SuccessfulStage(std::move(profile));
  const auto stage = stage_result.request;
  const StopScopeEntryKeyDTO key{
      stage.stop_id, stage.stop_generation,
      StopParticipantScopeKeyDTO{stage.operation_id, stage.participant_id}};
  StopScopeEntryDTO scope{
      key,
      StopParticipantScopeFactsDTO{
          OperationRevision(stage.operation_revision.value() + 1U), stage.controllable_component_id,
          stage.command_target_id, stage.capability_id, stage.control_domain_id,
          stage.control_domain_revision, stage.control_binding_id, stage.control_binding_revision,
          stage.controller_goal_id, stage.controller_goal_generation,
          stage.active_control_configuration_id, stage.active_control_configuration_generation,
          stage.controller_topology_id, stage.topology_revision, stage.control_route_id,
          stage.provider_runtime_epoch, stage.resources, stage.containment_granularity,
          StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled},
      StopScopeEntryDispositionDTO::kStoppable,
      StopScopeDispatchStateDTO::kDispatched,
      stage.stop_evidence_policy_id,
      stage.stop_evidence_policy_revision,
      stage.participant_stop_plan.participant_stop_invocation_plan_digest,
      10};
  StopTerminalReductionInputDTO input{
      StopCertificateId("certificate.1"),
      StopCertificateRevision(1U),
      stage.stop_id,
      stage.stop_generation,
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       ContentDigest(kDigestB), std::nullopt, std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop,
      {},
      {StopScopeCaptureHistoryDTO{
          1U,
          StopResolutionDispositionDTO::kExact,
          StopScopeResolutionReasonDTO::kExplicitFullStop,
          ContentDigest(kDigestA),
          {},
          {StopAffectedOperationRevisionDTO{
              stage.operation_id, OperationRevision(stage.operation_revision.value() + 1U)}},
          stage.resources}},
      {},
      {scope},
      {StopAttemptedStageResultDTO{key, 1U, 20, StopStageResultAuthorityDTO::kAuthoritative,
                                   std::move(stage_result)}},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      30,
      std::nullopt};
  auto certificate = ReduceStopTerminal(input);
  EXPECT_TRUE(certificate.ok()) << certificate.error().message;

  StopAcceptedDTO accepted{
      RequestId("request.1"),
      RequestReplayStatusDTO::kAcceptedNew,
      input.stop_id,
      input.stop_generation,
      input.requested_selector.requested_selector_digest,
      input.resolution_disposition,
      input.resolution_reason,
      {},
      input.capture_history.front().captured_scope_digest,
      {},
      input.capture_history.front().newly_fenced_operation_revisions,
      10};
  const auto accepted_error = stop_accepted_field_error(accepted);
  EXPECT_FALSE(accepted_error.has_value())
      << (accepted_error.has_value() ? std::string(*accepted_error) : std::string{});
  const StopCertificateRefDTO reference{
      certificate.value().stop_certificate_id, certificate.value().stop_certificate_revision,
      certificate.value().stop_id, certificate.value().stop_generation};
  StopStatusDTO status{accepted,
                       StopStatusStateDTO::kSucceeded,
                       certificate.value().scope_entries,
                       {},
                       certificate.value().attempted_stage_results,
                       certificate.value().evidence_items,
                       certificate.value().unsettled,
                       certificate.value().failed,
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                                      std::nullopt, std::nullopt},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                      std::nullopt, std::nullopt},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                                      std::nullopt, std::nullopt},
                       reference,
                       std::nullopt,
                       std::nullopt};
  const auto terminal_error =
      stop_terminal_projection_field_error(status, certificate.value(), std::nullopt);
  EXPECT_FALSE(terminal_error.has_value())
      << (terminal_error.has_value() ? std::string(*terminal_error) : std::string{});
  return StopJournalTerminalProjectionDTO{std::move(status), std::move(certificate.value()),
                                          std::nullopt};
}

[[nodiscard]] StopJournalFaultClearProjectionDTO FaultClearProjection() {
  auto terminal = TerminalProjection();
  const StopCertificateRefDTO certificate_ref{
      terminal.certificate.stop_certificate_id, terminal.certificate.stop_certificate_revision,
      terminal.certificate.stop_id, terminal.certificate.stop_generation};
  StopFaultLatchDTO cleared_latch{
      CellId("cell.1"),
      terminal.certificate.stop_id,
      terminal.certificate.stop_generation,
      certificate_ref,
      StopFaultLatchId("stop-fault-latch.1"),
      StopFaultLatchGeneration(1U),
      StopFaultTriggerDTO::kScopePromotion,
      false,
      {OperationId("operation.1")},
      {ResourceId("joint.1"), ResourceId("joint.2")},
      RetainedBundleId("retained-bundle.1"),
      GraphRevision(kDigestA),
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopFaultLatchStateDTO::kCleared,
      3U,
      StopFaultRecoveryStatusDTO::kCleared,
      "stop_fault_recovery_cleared",
      {StopFaultRecoveryEvidenceRefDTO{StopFaultRecoveryEvidenceKindDTO::kSettlement,
                                       ContentDigest(kDigestB)}}};
  terminal.stop_status.terminal_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  terminal.stop_status.fault_latch = cleared_latch;
  const StopFaultRecoveryIntentDTO intent{
      PublicRequestContextDTO{RequestId("recovery.1"), RequestOwnerEpoch("owner.1"), "test",
                              std::nullopt},
      cleared_latch.latch_id,
      cleared_latch.latch_generation,
      2U,
      terminal.certificate.stop_certificate_id,
      terminal.certificate.stop_certificate_revision,
      cleared_latch.cell_id,
      cleared_latch.retained_bundle_id,
      cleared_latch.graph_revision,
      {StopFaultRecoveryConfigurationExpectationDTO{ControlDomainId("domain.1"),
                                                    ActiveControlConfigurationId("configuration.1"),
                                                    ActiveControlConfigurationGeneration(1U)}},
      std::string("operator verified cell")};
  const StopFaultRecoveryResultDTO result{intent.request_context.request_id,
                                          RequestReplayStatusDTO::kAcceptedNew, true, cleared_latch,
                                          std::nullopt};
  return StopJournalFaultClearProjectionDTO{intent, result, terminal.stop_status,
                                            terminal.certificate};
}

TEST(StopJournalProjectionCodecTest, RoundTripsAcceptanceAndAffectedStateCanonically) {
  for (const auto capture_order :
       {std::uint64_t{0}, std::uint64_t{1}, (std::uint64_t{1} << 53U) + 1U,
        static_cast<std::uint64_t>(INT64_MAX)}) {
    SCOPED_TRACE(capture_order);
    auto terminal = TerminalProjection();
    OperationStateDTO operation{OperationId("operation.1"),
                                OperationRevision(5U),
                                ProgramBindingId(kDigestB),
                                ProgramRunId("program-run.1"),
                                OperationCoordinationPolicy::kIndependent,
                                OperationStateKind::kStopped,
                                {},
                                30,
                                terminal.certificate.stop_id,
                                terminal.certificate.stop_generation,
                                std::nullopt};
    operation.journal_capture_order = capture_order;
    StopJournalAcceptanceProjectionDTO projection{
        terminal.stop_status.accepted,
        EmergencyStopAffectedStateSnapshotDTO{{std::move(operation)},
                                              {ResourceId("joint.1"), ResourceId("joint.2")}}};
    const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message;
    EXPECT_NE(encoded.value().find(
                  capture_order == 0U ? "manyforge.internal.stop-journal-acceptance-projection.v4"
                                      : "manyforge.internal.stop-journal-acceptance-projection.v5"),
              std::string::npos);
    const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value(), projection);
    const auto reencoded = EncodeStopJournalAcceptanceProjection(decoded.value());
    ASSERT_TRUE(reencoded.ok());
    EXPECT_EQ(reencoded.value(), encoded.value());
  }
}

TEST(StopJournalProjectionCodecTest, CapturedNodeOriginUsesAnExplicitVersionAndRejectsAliasing) {
  auto terminal = TerminalProjection();
  OperationStateDTO operation{OperationId("operation.1"),
                              OperationRevision(1U),
                              ProgramBindingId(kDigestB),
                              ProgramRunId("run.1"),
                              OperationCoordinationPolicy::kIndependent,
                              OperationStateKind::kStopped,
                              {},
                              30,
                              terminal.certificate.stop_id,
                              terminal.certificate.stop_generation,
                              std::nullopt};
  operation.journal_capture_order = 1U;
  OperationParticipantStateDTO participant{OperationParticipantId("participant.1"),
                                           ControllerGoalId("goal.1"),
                                           ControllerGoalGeneration(1U),
                                           ControllableComponentId("arm"),
                                           CommandTargetId("joints"),
                                           CapabilityId("execution.trajectory.execute"),
                                           ControlDomainId("domain.1"),
                                           ControlDomainRevision(kDigestA),
                                           ControlBindingId("binding.1"),
                                           ControlBindingRevision(kDigestA),
                                           std::nullopt,
                                           std::nullopt,
                                           OperationParticipantPayloadKind::kMotion,
                                           OperationStateKind::kStopped,
                                           0.0,
                                           {},
                                           std::nullopt};
  participant.execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("pick"), "attempt.2"};
  operation.participants.push_back(participant);
  StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted, {{operation}, {}}};
  const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_NE(encoded.value().find("acceptance-projection.v6"), std::string::npos);
  const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  auto legacy_alias = encoded.value();
  legacy_alias.replace(legacy_alias.find("acceptance-projection.v6"),
                       std::string("acceptance-projection.v6").size(), "acceptance-projection.v5");
  EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(legacy_alias).ok());
  // Q51/Q53: the earlier (budgeted) layouts are refused, never converted.
  for (const auto* earlier : {"acceptance-projection.v1", "acceptance-projection.v2",
                              "acceptance-projection.v3"}) {
    auto old = encoded.value();
    old.replace(old.find("acceptance-projection.v6"),
                std::string("acceptance-projection.v6").size(), earlier);
    EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(old).ok()) << earlier;
  }
  auto unordered = projection;
  unordered.affected_state.operations.front().journal_capture_order = 0U;
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(unordered).ok());
  projection.affected_state.operations.front()
      .participants.front()
      .execution_scope->node_attempt_id.clear();
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(projection).ok());
}

TEST(StopJournalProjectionCodecTest, RoundTripsEveryEmbeddedTerminalStageFieldCanonically) {
  const auto projection = TerminalProjection();
  ASSERT_EQ(projection.certificate.attempted_stage_results.size(), 1U);
  ASSERT_FALSE(projection.certificate.attempted_stage_results.front()
                   .result.request.participant_stop_plan.stage_commands.empty());
  const auto encoded = EncodeStopJournalTerminalProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodeStopJournalTerminalProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  const auto reencoded = EncodeStopJournalTerminalProjection(decoded.value());
  ASSERT_TRUE(reencoded.ok());
  EXPECT_EQ(reencoded.value(), encoded.value());
}

TEST(StopJournalProjectionCodecTest, RetainsExplicitOffAndOnJerkProfilesExactly) {
  for (const bool enabled : {false, true}) {
    MotionInterruptStopProfileDTO profile;
    profile.profile_id = "journal.jerk";
    profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
    profile.deceleration_time_s = 0.5;
    profile.jerk_limits_enabled = enabled;
    profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}},
                                          {"joint_2", AngularAccelerationDTO{2.0}}};
    profile.per_joint_max_jerk = {{"joint_1", AngularJerkDTO{20.0}},
                                  {"joint_2", AngularJerkDTO{21.0}}};
    const auto projection = TerminalProjection(profile);
    const auto encoded = EncodeStopJournalTerminalProjection(projection);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message;
    const auto decoded = DecodeStopJournalTerminalProjection(encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value(), projection);
    EXPECT_EQ(EncodeStopJournalTerminalProjection(decoded.value()).value(), encoded.value());
  }
}

TEST(StopJournalProjectionCodecTest, RoundTripsInternalProgramRunSelectorCanonically) {
  auto projection = TerminalProjection();
  const StopRequestedSelectorSnapshotDTO selector{
      StopRequestedSelectorKindDTO::kInternalProgramRun, ContentDigest(kDigestB),
      StopSelectorDTO{std::vector<StopProgramRunTargetDTO>{
          StopProgramRunTargetDTO{ProgramRunId("program-run.1")}}},
      std::nullopt};
  projection.stop_status.accepted.resolution_reason =
      StopScopeResolutionReasonDTO::kExactCurrentScope;
  projection.certificate.requested_selector = selector;
  projection.certificate.resolution_reason = StopScopeResolutionReasonDTO::kExactCurrentScope;
  projection.certificate.capture_history.front().resolution_reason =
      StopScopeResolutionReasonDTO::kExactCurrentScope;

  const auto encoded = EncodeStopJournalTerminalProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodeStopJournalTerminalProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  const auto reencoded = EncodeStopJournalTerminalProjection(decoded.value());
  ASSERT_TRUE(reencoded.ok());
  EXPECT_EQ(reencoded.value(), encoded.value());
}

TEST(StopJournalProjectionCodecTest, RejectsUnknownVersionAndNoncanonicalBytes) {
  const auto projection = TerminalProjection();
  const auto encoded = EncodeStopJournalTerminalProjection(projection);
  ASSERT_TRUE(encoded.ok());
  EXPECT_FALSE(DecodeStopJournalTerminalProjection(encoded.value() + "\n").ok());
  std::string wrong_version = encoded.value();
  const auto position = wrong_version.find(kStopJournalTerminalProjectionApiVersion);
  ASSERT_NE(position, std::string::npos);
  wrong_version.replace(position, kStopJournalTerminalProjectionApiVersion.size(),
                        "manyforge.internal.stop-journal-terminal-projection.v0");
  EXPECT_FALSE(DecodeStopJournalTerminalProjection(wrong_version).ok());
}

TEST(StopJournalProjectionCodecTest, RoundTripsExactFaultClearAuthorityCanonically) {
  const auto projection = FaultClearProjection();
  const auto encoded = EncodeStopJournalFaultClearProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodeStopJournalFaultClearProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  EXPECT_FALSE(DecodeStopJournalFaultClearProjection(encoded.value() + "\n").ok());

  auto conflict = projection;
  conflict.recovery_result.request_id = RequestId("recovery.conflict");
  EXPECT_FALSE(EncodeStopJournalFaultClearProjection(conflict).ok());
}

}  // namespace

TEST(StopJournalProjectionCodecTest, CapturedScopeUsesVersionSixWithoutRetryPolicy) {
  auto terminal = TerminalProjection();
  OperationParticipantStateDTO participant{
      OperationParticipantId("participant.1"),
      ControllerGoalId("goal.1"),
      ControllerGoalGeneration(1U),
      ControllableComponentId("arm"),
      CommandTargetId("joints"),
      CapabilityId("execution.trajectory.execute"),
      ControlDomainId("domain.1"),
      ControlDomainRevision(kDigestA),
      ControlBindingId("binding.1"),
      ControlBindingRevision(kDigestA),
      std::nullopt,
      std::nullopt,
      OperationParticipantPayloadKind::kMotion,
      OperationStateKind::kStopped,
      0.0,
      {},
      std::nullopt,
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("pick"), "attempt.2"}};
  OperationStateDTO operation{OperationId("operation.1"),
                              OperationRevision(1U),
                              ProgramBindingId(kDigestB),
                              ProgramRunId("run.1"),
                              OperationCoordinationPolicy::kIndependent,
                              OperationStateKind::kStopped,
                              {participant},
                              30,
                              terminal.certificate.stop_id,
                              terminal.certificate.stop_generation,
                              std::nullopt};
  operation.journal_capture_order = 1U;
  StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted, {{operation}, {}}};
  const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_NE(encoded.value().find("acceptance-projection.v6"), std::string::npos);
  const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  auto alias = encoded.value();
  alias.replace(alias.find("acceptance-projection.v6"),
                std::string("acceptance-projection.v6").size(), "acceptance-projection.v7");
  EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(alias).ok());
}

TEST(StopJournalProjectionCodecTest, ExactParticipantFactUsesVersionEightWithOrWithoutScope) {
  for (const bool scoped : {false, true}) {
    auto terminal = TerminalProjection();
    const ErrorDTO cause{"planning_failed", "exact failure", {{"phase", "planning"}}};
    OperationParticipantStateDTO participant{OperationParticipantId("participant.1"),
                                             ControllerGoalId("goal.1"),
                                             ControllerGoalGeneration(1U),
                                             ControllableComponentId("arm"),
                                             CommandTargetId("joints"),
                                             CapabilityId("execution.trajectory.execute"),
                                             ControlDomainId("domain.1"),
                                             ControlDomainRevision(kDigestA),
                                             ControlBindingId("binding.1"),
                                             ControlBindingRevision(kDigestA),
                                             std::nullopt,
                                             std::nullopt,
                                             OperationParticipantPayloadKind::kMotion,
                                             OperationStateKind::kFailed,
                                             0.0,
                                             {},
                                             cause};
    if (scoped)
      participant.execution_scope =
          ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("pick"), "attempt.1"};
    participant.terminal_fact = ParticipantExecutionFact{
        ParticipantExecutionFactKind::kLocalFailure, ParticipantEffectKnowledge::kNoEffect, cause};
    OperationStateDTO operation{OperationId("operation.1"),
                                OperationRevision(1U),
                                ProgramBindingId(kDigestB),
                                ProgramRunId("run.1"),
                                OperationCoordinationPolicy::kIndependent,
                                OperationStateKind::kFailed,
                                {participant},
                                30,
                                std::nullopt,
                                std::nullopt,
                                std::nullopt};
    operation.journal_capture_order = 1U;
    StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted, {{operation}, {}}};
    const auto bytes = EncodeStopJournalAcceptanceProjection(projection);
    ASSERT_TRUE(bytes.ok()) << bytes.error().message;
    EXPECT_NE(bytes.value().find("acceptance-projection.v8"), std::string::npos);
    const auto decoded = DecodeStopJournalAcceptanceProjection(bytes.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value(), projection);
    auto alias = bytes.value();
    alias.replace(alias.find("acceptance-projection.v8"),
                  std::string("acceptance-projection.v8").size(), "acceptance-projection.v7");
    EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(alias).ok());
    auto nonterminal = projection;
    nonterminal.affected_state.operations.front().participants.front().state =
        OperationStateKind::kExecuting;
    EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(nonterminal).ok());
    projection.affected_state.operations.front().participants.front().terminal_fact.reset();
    const auto old_shape = EncodeStopJournalAcceptanceProjection(projection);
    ASSERT_TRUE(old_shape.ok());
    auto empty_fact_version = old_shape.value();
    const std::string old_version =
        scoped ? "acceptance-projection.v6" : "acceptance-projection.v5";
    const auto version_at = empty_fact_version.find(old_version);
    ASSERT_NE(version_at, std::string::npos);
    empty_fact_version.replace(version_at, old_version.size(), "acceptance-projection.v8");
    EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(empty_fact_version).ok());
    projection.affected_state.operations.front().participants.front().terminal_fact =
        participant.terminal_fact;
    projection.affected_state.operations.front().participants.front().terminal_error =
        ErrorDTO{"resource_slot_state_invalid", "selected resource transition failed", {}};
    const auto retained = EncodeStopJournalAcceptanceProjection(projection);
    ASSERT_TRUE(retained.ok()) << retained.error().message;
    const auto retained_roundtrip = DecodeStopJournalAcceptanceProjection(retained.value());
    ASSERT_TRUE(retained_roundtrip.ok());
    EXPECT_EQ(retained_roundtrip.value(), projection);
  }
}

TEST(StopJournalProjectionCodecTest, LogicalPreemptionHasADistinctVersionAndPreservesPhysicalFacts) {
  const auto terminal = TerminalProjection();
  OperationParticipantStateDTO participant{OperationParticipantId("arm.participant"),
                                           ControllerGoalId("arm.goal"),
                                           ControllerGoalGeneration(1U),
                                           ControllableComponentId("arm"),
                                           CommandTargetId("joints"),
                                           CapabilityId("execution.trajectory.execute"),
                                           ControlDomainId("domain.1"),
                                           ControlDomainRevision(kDigestA),
                                           ControlBindingId("binding.1"),
                                           ControlBindingRevision(kDigestA),
                                           std::nullopt,
                                           std::nullopt,
                                           OperationParticipantPayloadKind::kMotion,
                                           OperationStateKind::kExecuting,
                                           0.0,
                                           {},
                                           std::nullopt};
  participant.execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("source.node"), "source.attempt"};
  OperationStateDTO operation{OperationId("source.operation"),
                              OperationRevision(1U),
                              ProgramBindingId(kDigestB),
                              ProgramRunId("run.1"),
                              OperationCoordinationPolicy::kIndependent,
                              OperationStateKind::kExecuting,
                              {participant},
                              30,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt};
  operation.journal_capture_order = 1U;
  operation.preemption = OperationPreemptionDTO{
      OperationPreemptionKind::kPreempted,
      {OperationId("winner.operation"), ProgramNodeInstanceId("winner.node"), "winner.attempt"}};
  StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted, {{operation}, {}}};
  for (const bool settled : {false, true}) {
    auto& row = projection.affected_state.operations.front().participants.front();
    if (settled) {
      row.state = OperationStateKind::kStopped;
      row.terminal_fact = ParticipantExecutionFact{
          ParticipantExecutionFactKind::kPreemptedByReplacement,
          ParticipantEffectKnowledge::kEffectSettled, std::nullopt};
    }
    const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message;
    EXPECT_NE(encoded.value().find("acceptance-projection.v9"), std::string::npos);
    const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value(), projection);
    auto alias = encoded.value();
    alias.replace(alias.find("acceptance-projection.v9"),
                  std::string("acceptance-projection.v9").size(), "acceptance-projection.v8");
    EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(alias).ok());
  }
  auto invalid = projection;
  invalid.affected_state.operations.front().preemption->by.operation_id = operation.operation_id;
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(invalid).ok());
  invalid = projection;
  invalid.affected_state.operations.front().participants.front().terminal_fact->effect =
      ParticipantEffectKnowledge::kEffectUncertain;
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(invalid).ok());
  invalid = projection;
  invalid.affected_state.operations.front().preemption.reset();
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(invalid).ok());
}

TEST(StopJournalProjectionCodecTest, ProviderIdentityAndOutputEvidenceUseClosedV10Extension) {
  const auto terminal = TerminalProjection();
  OperationStateDTO operation{OperationId("operation.output"),
                              OperationRevision(1U),
                              ProgramBindingId(kDigestB),
                              ProgramRunId("run.output"),
                              OperationCoordinationPolicy::kIndependent,
                              OperationStateKind::kStopped,
                              {},
                              30,
                              terminal.certificate.stop_id,
                              terminal.certificate.stop_generation,
                              std::nullopt};
  operation.participants.emplace_back(
      OperationParticipantId("participant.output"), ControllerGoalId("goal.output"),
      ControllerGoalGeneration(1U), ControllableComponentId("io"), CommandTargetId("outputs"),
      CapabilityId("io.signal.write"),
      ParticipantBindingIdentityDTO{
          ControlDomainId("domain.io"), ControlDomainRevision(kDigestA),
          CapabilityProviderBindingIdentityDTO{CapabilityProviderBindingId("provider.io"),
                                               CapabilityProviderBindingRevision(kDigestB)}},
      OperationParticipantPayloadKind::kCapabilityCommand, OperationStateKind::kStopped, 0.0,
      std::vector<ResourceId>{ResourceId("io.output")}, std::nullopt);
  StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted, {{operation}, {}}};
  for (const auto evidence :
       {std::optional<IoOutputEvidence>{}, std::optional{IoOutputEvidence::kNotSubmitted},
        std::optional{IoOutputEvidence::kSubmitted},
        std::optional{IoOutputEvidence::kSourceAccepted},
        std::optional{IoOutputEvidence::kSourceRejected},
        std::optional{IoOutputEvidence::kAcceptanceUnknown}}) {
    projection.affected_state.operations.front().participants.front().output_evidence = evidence;
    const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message;
    EXPECT_NE(encoded.value().find("acceptance-projection.v10"), std::string::npos);
    const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value(), projection);
    const auto reencoded = EncodeStopJournalAcceptanceProjection(decoded.value());
    ASSERT_TRUE(reencoded.ok());
    EXPECT_EQ(reencoded.value(), encoded.value());
    auto downgraded = encoded.value();
    downgraded.replace(downgraded.find("acceptance-projection.v10"),
                       std::string("acceptance-projection.v10").size(), "acceptance-projection.v9");
    EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(downgraded).ok());
  }
  auto& row = projection.affected_state.operations.front().participants.front();
  row.output_evidence = static_cast<IoOutputEvidence>(999);
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(projection).ok());
  row.output_evidence.reset();
  row.binding_identity.binding = CapabilityProviderBindingIdentityDTO{
      CapabilityProviderBindingId(""), CapabilityProviderBindingRevision(kDigestB)};
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(projection).ok());
}

}  // namespace manyforge::core

namespace manyforge::core {
namespace {
TEST(StopJournalProjectionCodecTest, RuleEvidenceRoundTripsAndOldEnvelopeCannotAliasIt) {
  auto terminal = TerminalProjection();
  auto operation = rule_codec_test::State();
  StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted, {{operation}, {ResourceId("joint.1")}}};
  const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  ASSERT_NE(encoded.value().find("acceptance-projection.v10"), std::string::npos);
  const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  auto aliased = encoded.value();
  aliased.replace(aliased.find("acceptance-projection.v10"), std::string("acceptance-projection.v10").size(), "acceptance-projection.v9");
  EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(aliased).ok());
  projection.affected_state.operations.front().participants.front().command_generation = ControllerGoalGeneration(2U);
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(projection).ok());
}
}  // namespace
}  // namespace manyforge::core

namespace manyforge::core {
namespace {
TEST(StopJournalProjectionCodecTest, TakeoverDecisionUsesOptionalV11AndExactInt64Times) {
  const auto terminal = TerminalProjection();
  OperationStateDTO operation{OperationId("winner"),
                              OperationRevision(1U),
                              ProgramBindingId(kDigestB),
                              ProgramRunId("run.1"),
                              OperationCoordinationPolicy::kIndependent,
                              OperationStateKind::kExecuting,
                              {},
                              30,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt};
  StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted, {{operation}, {}}};
  const auto legacy = EncodeStopJournalAcceptanceProjection(projection);
  ASSERT_TRUE(legacy.ok());
  EXPECT_NE(legacy.value().find("acceptance-projection.v4"), std::string::npos);
  const auto old_decoded = DecodeStopJournalAcceptanceProjection(legacy.value());
  ASSERT_TRUE(old_decoded.ok());
  EXPECT_FALSE(old_decoded.value().affected_state.operations.front().motion_takeover);
  EXPECT_EQ(EncodeStopJournalAcceptanceProjection(old_decoded.value()).value(), legacy.value());
  for (const auto path :
       {MotionTakeoverPathDTO::kMovingReplacement, MotionTakeoverPathDTO::kSpringBackHold,
        MotionTakeoverPathDTO::kOrdinaryContainment}) {
    auto& decision = projection.affected_state.operations.front().motion_takeover;
    decision = MotionTakeoverDecisionDTO{
        OperationId("incumbent"),     path,        std::nullopt, INT64_MAX,
        (std::int64_t{1} << 53U) + 1, 500'000'000, std::nullopt};
    if (path != MotionTakeoverPathDTO::kMovingReplacement) {
      decision->cause = ErrorDTO{"takeover_capture_failed",
                                 "exact smooth failure",
                                 {{"predicate", "progress_observation_mismatch"}}};
      decision->stop_certificate =
          StopCertificateRefDTO{StopCertificateId("certificate.1"), StopCertificateRevision(2U),
                                StopId("fallback.stop"), StopGeneration(3U)};
    }
    const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message;
    EXPECT_NE(encoded.value().find("acceptance-projection.v11"), std::string::npos);
    const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value(), projection);
    EXPECT_EQ(EncodeStopJournalAcceptanceProjection(decoded.value()).value(), encoded.value());
    auto alias = encoded.value();
    alias.replace(alias.find("acceptance-projection.v11"),
                  std::string("acceptance-projection.v11").size(), "acceptance-projection.v10");
    EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(alias).ok());
  }
  const auto valid = projection;
  for (const auto invalidity : {0, 1, 2, 3}) {
    projection = valid;
    auto& decision = *projection.affected_state.operations.front().motion_takeover;
    if (invalidity == 0) decision.preparation_budget_ns = 0;
    if (invalidity == 1) decision.decided_at_ns = -1;
    if (invalidity == 2) decision.incumbent_operation_id = operation.operation_id;
    if (invalidity == 3) decision.stop_certificate->stop_generation = StopGeneration(0U);
    EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(projection).ok());
  }
}

TEST(StopJournalProjectionCodecTest, ForensicTakeoverCandidateDoesNotExpandPhysicalAffectedState) {
  const auto terminal = TerminalProjection();
  OperationStateDTO incumbent{OperationId("incumbent"),
                              OperationRevision(2U),
                              ProgramBindingId(kDigestB),
                              ProgramRunId("run.1"),
                              OperationCoordinationPolicy::kIndependent,
                              OperationStateKind::kStopping,
                              {},
                              30,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt};
  auto candidate = incumbent;
  candidate.operation_id = OperationId("candidate");
  candidate.state = OperationStateKind::kResolving;
  candidate.motion_takeover =
      MotionTakeoverDecisionDTO{incumbent.operation_id,
                                MotionTakeoverPathDTO::kSpringBackHold,
                                ErrorDTO{"smooth_failed", "exact reason", {{"detail", "retained"}}},
                                30,
                                20,
                                500'000'000,
                                std::nullopt};
  StopJournalAcceptanceProjectionDTO projection{terminal.stop_status.accepted,
                                                {{incumbent}, {ResourceId("physical")}, candidate}};
  const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  EXPECT_EQ(decoded.value().affected_state.operations, std::vector<OperationStateDTO>{incumbent});
  EXPECT_EQ(decoded.value().accepted, terminal.stop_status.accepted);
  projection.affected_state.motion_takeover_candidate_snapshot->motion_takeover
      ->incumbent_operation_id = OperationId("absent");
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(projection).ok());
  projection.affected_state.motion_takeover_candidate_snapshot = incumbent;
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(projection).ok());
}
}  // namespace
}  // namespace manyforge::core
