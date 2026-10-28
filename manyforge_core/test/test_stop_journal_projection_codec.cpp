#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <sstream>
#include <string>
#include <string_view>

namespace manyforge::core {
namespace {

inline constexpr char kDigestA[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
inline constexpr char kDigestB[] =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
inline constexpr char kDigestC[] =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

/// `legacy_fallback_parameters`: the provider plan states its fallback with the
/// historical `recoverable_substitution_mode` key, as a provider schema may.
[[nodiscard]] ResolvedStopBasePlanDTO BasePlan(const bool legacy_fallback_parameters = false) {
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
  if (legacy_fallback_parameters) {
    nominal.default_contract_parameters->emplace("recoverable_substitution_mode",
                                                 JsonValueDTO{"spring_back_hold"});
    nominal.command_qualification.controlled_substitution_contract->fallback_stop_modes = {
        MotionControlledStopMode::kSpringBackHold};
  }
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

// One participant's stage result. Every operation/participant pair gets its
// own prepared plan (and so its own plan digest); repeated calls for one pair
// rebuild an equal plan, as the Kernel's attempts of one participant share it.
[[nodiscard]] ParticipantStopStageResultDTO SuccessfulStage(
    std::optional<MotionInterruptStopProfileDTO> profile = std::nullopt,
    const std::size_t stage_index = 0U, const std::string& operation = "operation.1",
    const std::string& participant = "participant.1",
    const bool legacy_fallback_parameters = false) {
  auto base = BasePlan(legacy_fallback_parameters);
  if (profile && !profile->fallback_stop_modes.empty()) {
    auto& qualification =
        base.escalation_stages.front().command_qualification.controlled_substitution_contract;
    qualification->fallback_stop_modes.clear();
    for (const auto mode : profile->fallback_stop_modes)
      qualification->fallback_stop_modes.push_back(mode == MotionInterruptStopMode::kSpringBackHold
                                                       ? MotionControlledStopMode::kSpringBackHold
                                                       : MotionControlledStopMode::kSpringBackFast);
    auto qualified = MakeResolvedStopBasePlan(std::move(base));
    EXPECT_TRUE(qualified.ok());
    base = std::move(qualified.value());
  }
  auto binding = Binding(base);
  auto plan = MakePreparedParticipantStopPlan(
      base,
      PreparedParticipantStopPlanContextDTO{
          OperationId(operation), OperationRevision(4U), OperationParticipantId(participant),
          binding.value().program_id, binding.value().program_revision,
          binding.value().program_binding_id, ProgramRunId("program-run.1"), profile,
          profile ? ParticipantStopProfilePurposeDTO::kOperator
                  : ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt, MotionInterruptNodeCategory::kGeneric},
      binding.value().resources,
      std::vector<HardStopResourceLimitDTO>{{ResourceId("joint.1"), AngularVelocityDTO{0.02}},
                                            {ResourceId("joint.2"), AngularVelocityDTO{0.02}}});
  EXPECT_TRUE(plan.ok());
  const auto& selected = plan.value().stage_commands.at(stage_index).resolved_stage;
  const auto provider_runtime_epoch = selected.command_target.provider_runtime_epoch;
  const auto containment_granularity = selected.containment_granularity;
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
                                        containment_granularity,
                                        base.stop_evidence_policy_id,
                                        base.stop_evidence_policy_revision,
                                        std::move(plan.value()),
                                        stage_index,
                                        ContentDigest(kDigestA)};
  ParticipantStopStageResultDTO result{
      std::move(stage),
      StopStageAttemptDispositionDTO::kDispatched,
      ParticipantStopStageOutcomeDTO::kSucceeded,
      stage_index == 0U
          ? std::optional<MotionControlledStopMode>(MotionControlledStopMode::kSoftOnPath)
          : std::nullopt,
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

[[nodiscard]] ParticipantStopStageResultDTO FailedStage(ParticipantStopStageResultDTO result) {
  result.outcome = ParticipantStopStageOutcomeDTO::kFailed;
  result.error = ErrorDTO{"stop_stage_failed", "failed", {}};
  for (auto& evidence : result.evidence_items) {
    evidence.state = StopEvidenceItemStateDTO::kFailed;
    evidence.error = ErrorDTO{"stop_evidence_failed", "failed", {}};
  }
  return result;
}

[[nodiscard]] StopScopeEntryKeyDTO KeyOf(const ResolvedParticipantStopStageDTO& stage) {
  return StopScopeEntryKeyDTO{stage.stop_id, stage.stop_generation,
                              StopParticipantScopeKeyDTO{stage.operation_id, stage.participant_id}};
}

[[nodiscard]] StopScopeEntryDTO EntryOf(const ResolvedParticipantStopStageDTO& stage,
                                        const ContainmentGranularity granularity) {
  return StopScopeEntryDTO{
      KeyOf(stage),
      StopParticipantScopeFactsDTO{
          OperationRevision(stage.operation_revision.value() + 1U), stage.controllable_component_id,
          stage.command_target_id, stage.capability_id, stage.control_domain_id,
          stage.control_domain_revision, stage.control_binding_id, stage.control_binding_revision,
          stage.controller_goal_id, stage.controller_goal_generation,
          stage.active_control_configuration_id, stage.active_control_configuration_generation,
          stage.controller_topology_id, stage.topology_revision, stage.control_route_id,
          stage.provider_runtime_epoch, stage.resources, granularity,
          StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled},
      StopScopeEntryDispositionDTO::kStoppable,
      StopScopeDispatchStateDTO::kDispatched,
      stage.stop_evidence_policy_id,
      stage.stop_evidence_policy_revision,
      stage.participant_stop_plan.participant_stop_invocation_plan_digest,
      10};
}

[[nodiscard]] StopTerminalReductionInputDTO ExplicitFullInput(
    std::vector<StopScopeEntryDTO> entries, std::vector<StopAttemptedStageResultDTO> attempts,
    std::vector<StopAffectedOperationRevisionDTO> fenced, std::vector<ResourceId> resources) {
  const auto stop_id = entries.front().key.origin_stop_id;
  const auto stop_generation = entries.front().key.origin_stop_generation;
  return StopTerminalReductionInputDTO{
      StopCertificateId("certificate.1"),
      StopCertificateRevision(1U),
      stop_id,
      stop_generation,
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       ContentDigest(kDigestB), std::nullopt, std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop,
      {},
      {StopScopeCaptureHistoryDTO{1U, StopResolutionDispositionDTO::kExact,
                                  StopScopeResolutionReasonDTO::kExplicitFullStop,
                                  ContentDigest(kDigestA), {}, std::move(fenced),
                                  std::move(resources)}},
      {},
      std::move(entries),
      std::move(attempts),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      30,
      std::nullopt};
}

// Reduces the input and pairs the certificate with its exact terminal status,
// built field by field (independently of TerminalStopStatusFromCertificate).
[[nodiscard]] StopJournalTerminalProjectionDTO ProjectionFor(
    const StopTerminalReductionInputDTO& input,
    std::vector<StopPredecessorAcceptanceRefDTO> joined_predecessors = {}) {
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
      input.capture_history.back().captured_scope_digest,
      std::move(joined_predecessors),
      input.capture_history.back().newly_fenced_operation_revisions,
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

[[nodiscard]] StopJournalTerminalProjectionDTO TerminalProjection(
    std::optional<MotionInterruptStopProfileDTO> profile = std::nullopt,
    const bool legacy_fallback_parameters = false) {
  auto stage_result = SuccessfulStage(std::move(profile), 0U, "operation.1", "participant.1",
                                      legacy_fallback_parameters);
  const auto stage = stage_result.request;
  const auto key = KeyOf(stage);
  return ProjectionFor(ExplicitFullInput(
      {EntryOf(stage, stage.containment_granularity)},
      {StopAttemptedStageResultDTO{key, 1U, 20, StopStageResultAuthorityDTO::kAuthoritative,
                                   std::move(stage_result)}},
      {StopAffectedOperationRevisionDTO{stage.operation_id,
                                        OperationRevision(stage.operation_revision.value() + 1U)}},
      stage.resources));
}

// Two participants, each escalated: a failed nominal stage, then a successful
// hard stage. Each participant's two attempts carry one shared plan.
[[nodiscard]] StopJournalTerminalProjectionDTO TwoParticipantEscalatedProjection() {
  std::vector<StopScopeEntryDTO> entries;
  std::vector<StopAttemptedStageResultDTO> attempts;
  std::vector<StopAffectedOperationRevisionDTO> fenced;
  std::uint64_t sequence = 0U;
  for (const char* suffix : {"1", "2"}) {
    const auto operation = std::string("operation.") + suffix;
    const auto participant = std::string("participant.") + suffix;
    auto nominal = FailedStage(SuccessfulStage(std::nullopt, 0U, operation, participant));
    auto hard = SuccessfulStage(std::nullopt, 1U, operation, participant);
    const auto key = KeyOf(nominal.request);
    entries.push_back(EntryOf(nominal.request, ContainmentGranularity::kControllerSet));
    fenced.push_back(StopAffectedOperationRevisionDTO{
        nominal.request.operation_id,
        OperationRevision(nominal.request.operation_revision.value() + 1U)});
    for (auto* result : {&nominal, &hard}) {
      ++sequence;
      attempts.push_back(StopAttemptedStageResultDTO{
          key, sequence, static_cast<std::int64_t>(20U + sequence),
          StopStageResultAuthorityDTO::kAuthoritative, std::move(*result)});
    }
  }
  auto projection =
      ProjectionFor(ExplicitFullInput(std::move(entries), std::move(attempts), std::move(fenced),
                                      {ResourceId("joint.1"), ResourceId("joint.2")}));
  EXPECT_EQ(projection.certificate.result, StopCertificateResultDTO::kSucceededEscalated);
  return projection;
}

// An explicit full Stop that merges a terminated predecessor: its certificate
// repeats the predecessor's origin-tagged scope rows and attempts.
[[nodiscard]] StopJournalTerminalProjectionDTO PredecessorMergeProjection() {
  const auto predecessor = TerminalProjection().certificate;
  const StopPredecessorAcceptanceRefDTO joined{
      predecessor.stop_id, predecessor.stop_generation,
      predecessor.capture_history.back().captured_scope_digest};
  const StopTerminalReductionInputDTO input{
      StopCertificateId("certificate.full"),
      StopCertificateRevision(1U),
      StopId("stop.full"),
      StopGeneration(8U),
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       ContentDigest(kDigestC), std::nullopt, std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop,
      {},
      {StopScopeCaptureHistoryDTO{1U, StopResolutionDispositionDTO::kExact,
                                  StopScopeResolutionReasonDTO::kExplicitFullStop,
                                  ContentDigest(kDigestB), {joined}, {}, {}}},
      {predecessor},
      {},
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      40,
      std::nullopt};
  auto projection = ProjectionFor(input, {joined});
  EXPECT_EQ(projection.certificate.predecessor_certificates.size(), 1U);
  EXPECT_EQ(projection.certificate.attempted_stage_results, predecessor.attempted_stage_results);
  return projection;
}

[[nodiscard]] StopFaultLatchDTO ClearedLatch(const StopCertificateDTO& certificate) {
  return StopFaultLatchDTO{
      CellId("cell.1"),
      certificate.stop_id,
      certificate.stop_generation,
      StopCertificateRefDTO{certificate.stop_certificate_id, certificate.stop_certificate_revision,
                            certificate.stop_id, certificate.stop_generation},
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
                                       ContentDigest(kDigestB)}},
      {StopFaultTriggerDTO::kScopePromotion}};
}

[[nodiscard]] StopJournalFaultClearProjectionDTO FaultClearProjection() {
  const auto terminal = TerminalProjection();
  const auto cleared_latch = ClearedLatch(terminal.certificate);
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
  return StopJournalFaultClearProjectionDTO{intent, result};
}

using V = content_identity::CanonicalValue;

[[nodiscard]] V::Array Root(const std::string& bytes) {
  return content_identity::parse_canonical_json(bytes).as_array();
}

[[nodiscard]] std::string Bytes(V::Array root) {
  return content_identity::canonical_json(V(std::move(root)));
}

[[nodiscard]] std::size_t Occurrences(const std::string_view haystack,
                                      const std::string_view needle) {
  std::size_t count = 0U;
  for (auto at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + 1U)) {
    ++count;
  }
  return count;
}

[[nodiscard]] std::string Reason(const ErrorDTO& error) {
  const auto found = error.details.find("reason");
  return found == error.details.end() ? std::string{} : found->second;
}

void ExpectExactRoundTrip(const StopJournalTerminalProjectionDTO& projection) {
  const auto encoded = EncodeStopJournalTerminalProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message << " " << Reason(encoded.error());
  const auto decoded = DecodeStopJournalTerminalProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << Reason(decoded.error());
  EXPECT_EQ(decoded.value(), projection);
  const auto reencoded = EncodeStopJournalTerminalProjection(decoded.value());
  ASSERT_TRUE(reencoded.ok());
  EXPECT_EQ(reencoded.value(), encoded.value());
}

TEST(StopJournalProjectionCodecTest, AcceptanceV12RoundTripsTheAcceptedRecordOnly) {
  const StopJournalAcceptanceProjectionDTO projection{
      TwoParticipantEscalatedProjection().stop_status.accepted};
  const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto root = Root(encoded.value());
  ASSERT_EQ(root.size(), 2U);
  EXPECT_EQ(root[0], V(std::string(kStopJournalAcceptanceProjectionApiVersion)));
  const auto decoded = DecodeStopJournalAcceptanceProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << Reason(decoded.error());
  EXPECT_EQ(decoded.value(), projection);
  EXPECT_EQ(EncodeStopJournalAcceptanceProjection(decoded.value()).value(), encoded.value());
  EXPECT_FALSE(DecodeStopJournalAcceptanceProjection(encoded.value() + "\n").ok());

  auto invalid = projection;
  invalid.accepted.stop_generation = StopGeneration(0U);
  EXPECT_FALSE(EncodeStopJournalAcceptanceProjection(invalid).ok());
}

TEST(StopJournalProjectionCodecTest, AcceptanceRefusesEveryEarlierVersionWithoutNormalizing) {
  const StopJournalAcceptanceProjectionDTO projection{TerminalProjection().stop_status.accepted};
  const auto encoded = EncodeStopJournalAcceptanceProjection(projection);
  ASSERT_TRUE(encoded.ok());
  for (int version = 4; version <= 11; ++version) {
    for (const bool with_affected_state : {false, true}) {
      SCOPED_TRACE(version);
      auto root = Root(encoded.value());
      root[0] = V("manyforge.internal.stop-journal-acceptance-projection.v" +
                  std::to_string(version));
      if (with_affected_state) root.emplace_back(V::Array{V(V::Array{}), V(V::Array{})});
      const auto decoded = DecodeStopJournalAcceptanceProjection(Bytes(std::move(root)));
      ASSERT_FALSE(decoded.ok());
      EXPECT_EQ(decoded.error().code, "invalid_stop_journal_projection");
      EXPECT_EQ(Reason(decoded.error()),
                "Stop journal acceptance projection API version is unsupported");
    }
  }
}

TEST(StopJournalProjectionCodecTest, RoundTripsEveryEmbeddedTerminalStageFieldCanonically) {
  const auto projection = TerminalProjection();
  ASSERT_EQ(projection.certificate.attempted_stage_results.size(), 1U);
  ASSERT_FALSE(projection.certificate.attempted_stage_results.front()
                   .result.request.participant_stop_plan.stage_commands.empty());
  // status_rest keeps the still-pending terminal projection state verbatim.
  ASSERT_EQ(projection.stop_status.terminal_projection.state,
            StopDurableProjectionStateDTO::kPending);
  ExpectExactRoundTrip(projection);
  const auto root = Root(EncodeStopJournalTerminalProjection(projection).value());
  ASSERT_EQ(root.size(), 5U);
  EXPECT_EQ(root[1].as_array().size(), 6U);
  EXPECT_EQ(root[2].as_array().size(), 23U);
  EXPECT_EQ(root[4].as_array().size(), 1U);
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
    ExpectExactRoundTrip(TerminalProjection(profile));
  }
}

TEST(StopJournalProjectionCodecTest, RoundTripsTheFastSpringBackModeAndItsActualProviderMode) {
  // Q130: the appended mode is read back, as SQLite owner-epoch replay does.
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "journal.fast";
  profile.stop_mode = MotionInterruptStopMode::kSpringBackFast;
  profile.deceleration_time_s = 0.3;
  profile.jerk_limits_enabled = false;
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}},
                                        {"joint_2", AngularAccelerationDTO{2.0}}};
  auto projection = TerminalProjection(profile);
  ASSERT_EQ(projection.certificate.attempted_stage_results.size(), 1U);
  for (auto* attempted : {&projection.certificate.attempted_stage_results,
                          &projection.stop_status.attempted_stage_results}) {
    attempted->front().result.actual_provider_contract_mode =
        MotionControlledStopMode::kSpringBackFast;
  }
  ExpectExactRoundTrip(projection);
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
  ExpectExactRoundTrip(projection);
}

TEST(StopJournalProjectionCodecTest, RejectsUnknownVersionAndNoncanonicalBytes) {
  const auto projection = TerminalProjection();
  const auto encoded = EncodeStopJournalTerminalProjection(projection);
  ASSERT_TRUE(encoded.ok());
  EXPECT_FALSE(DecodeStopJournalTerminalProjection(encoded.value() + "\n").ok());
  EXPECT_FALSE(DecodeStopJournalTerminalProjection(" " + encoded.value()).ok());
  std::string wrong_version = encoded.value();
  const auto position = wrong_version.find(kStopJournalTerminalProjectionApiVersion);
  ASSERT_NE(position, std::string::npos);
  wrong_version.replace(position, kStopJournalTerminalProjectionApiVersion.size(),
                        "manyforge.internal.stop-journal-terminal-projection.v0");
  EXPECT_FALSE(DecodeStopJournalTerminalProjection(wrong_version).ok());
}

TEST(StopJournalProjectionCodecTest, TwoParticipantEscalatedStopStoresEachPlanOnce) {
  const auto projection = TwoParticipantEscalatedProjection();
  const auto& attempts = projection.certificate.attempted_stage_results;
  ASSERT_EQ(attempts.size(), 4U);
  ASSERT_EQ(projection.certificate.scope_entries.size(), 2U);
  EXPECT_EQ(attempts[0].result.request.participant_stop_plan,
            attempts[1].result.request.participant_stop_plan);
  EXPECT_EQ(attempts[2].result.request.participant_stop_plan,
            attempts[3].result.request.participant_stop_plan);
  EXPECT_NE(attempts[0].result.request.participant_stop_plan.participant_stop_invocation_plan_digest,
            attempts[2].result.request.participant_stop_plan.participant_stop_invocation_plan_digest);
  ASSERT_FALSE(projection.certificate.evidence_items.empty());
  ExpectExactRoundTrip(projection);

  const auto encoded = EncodeStopJournalTerminalProjection(projection).value();
  const auto root = Root(encoded);
  ASSERT_EQ(root.size(), 5U);
  const auto& plans = root[4].as_array();
  ASSERT_EQ(plans.size(), 2U);
  EXPECT_LT(plans[0].as_array()[0].as_string(), plans[1].as_array()[0].as_string());
  for (const auto& plan : plans) {
    EXPECT_EQ(Occurrences(encoded, content_identity::canonical_json(plan)), 1U);
  }
  // Each lean attempt names its plan by digest (resolved stage slot 26).
  const auto& certificate = root[2].as_array();
  ASSERT_EQ(certificate.size(), 23U);
  for (const auto& attempt : certificate[11].as_array()) {
    const auto& request = attempt.as_array()[4].as_array()[0].as_array();
    ASSERT_EQ(request.size(), 29U);
    EXPECT_TRUE(request[26].is_string());
  }
}

TEST(StopJournalProjectionCodecTest, TerminalWithFaultLatchRebuildsTheStatusLatch) {
  auto projection = TerminalProjection();
  const auto latch = ClearedLatch(projection.certificate);
  projection.stop_status.terminal_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  projection.stop_status.fault_latch = latch;
  projection.fault_latch = latch;
  ExpectExactRoundTrip(projection);
}

TEST(StopJournalProjectionCodecTest, PredecessorMergeCertificateRoundTripsExactly) {
  const auto projection = PredecessorMergeProjection();
  ASSERT_FALSE(projection.certificate.attempted_stage_results.empty());
  EXPECT_NE(projection.certificate.attempted_stage_results.front().scope_key.origin_stop_id,
            projection.certificate.stop_id);
  ExpectExactRoundTrip(projection);
}

TEST(StopJournalProjectionCodecTest, UnequalPlansUnderOneDigestFailWithTheTypedConflictCode) {
  auto projection = TwoParticipantEscalatedProjection();
  for (auto* attempts : {&projection.certificate.attempted_stage_results,
                         &projection.stop_status.attempted_stage_results}) {
    (*attempts)[1].result.request.participant_stop_plan.program_run_id =
        ProgramRunId("program-run.conflict");
  }
  const auto encoded = EncodeStopJournalTerminalProjection(projection);
  ASSERT_FALSE(encoded.ok());
  EXPECT_EQ(encoded.error().code, kStopJournalProjectionPlanConflictCode);
}

TEST(StopJournalProjectionCodecTest, DecodeRefusesAMissingOrAnUnreferencedPlan) {
  const auto encoded =
      EncodeStopJournalTerminalProjection(TwoParticipantEscalatedProjection()).value();
  {
    auto root = Root(encoded);
    auto plans = root[4].as_array();
    plans.pop_back();
    root[4] = V(std::move(plans));
    const auto decoded = DecodeStopJournalTerminalProjection(Bytes(std::move(root)));
    ASSERT_FALSE(decoded.ok());
    EXPECT_NE(Reason(decoded.error()).find("absent participant plan"), std::string::npos)
        << Reason(decoded.error());
  }
  {
    auto root = Root(encoded);
    auto plans = root[4].as_array();
    auto extra = plans.back().as_array();
    extra[0] = V("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    plans.emplace_back(std::move(extra));
    root[4] = V(std::move(plans));
    const auto decoded = DecodeStopJournalTerminalProjection(Bytes(std::move(root)));
    ASSERT_FALSE(decoded.ok());
    EXPECT_NE(Reason(decoded.error()).find("unreferenced participant plan"), std::string::npos)
        << Reason(decoded.error());
  }
}

TEST(StopJournalProjectionCodecTest, DecodeRefusesUnsortedOrDuplicatePlans) {
  const auto encoded =
      EncodeStopJournalTerminalProjection(TwoParticipantEscalatedProjection()).value();
  const auto original = Root(encoded)[4].as_array();
  ASSERT_EQ(original.size(), 2U);
  for (const auto& plans : {V::Array{original[1], original[0]},
                            V::Array{original[0], original[0], original[1]}}) {
    auto root = Root(encoded);
    root[4] = V(plans);
    const auto decoded = DecodeStopJournalTerminalProjection(Bytes(std::move(root)));
    ASSERT_FALSE(decoded.ok());
    EXPECT_NE(Reason(decoded.error()).find("not sorted and unique"), std::string::npos)
        << Reason(decoded.error());
  }
}

TEST(StopJournalProjectionCodecTest, RoundTripsExactFaultClearAuthorityCanonically) {
  const auto projection = FaultClearProjection();
  const auto encoded = EncodeStopJournalFaultClearProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto root = Root(encoded.value());
  ASSERT_EQ(root.size(), 3U);
  EXPECT_EQ(root[0], V(std::string(kStopJournalFaultClearProjectionApiVersion)));
  const auto decoded = DecodeStopJournalFaultClearProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  EXPECT_EQ(EncodeStopJournalFaultClearProjection(decoded.value()).value(), encoded.value());
  EXPECT_FALSE(DecodeStopJournalFaultClearProjection(encoded.value() + "\n").ok());

  auto conflict = projection;
  conflict.recovery_result.request_id = RequestId("recovery.conflict");
  EXPECT_FALSE(EncodeStopJournalFaultClearProjection(conflict).ok());
  auto latched = projection;
  latched.recovery_result.latch.state = StopFaultLatchStateDTO::kLatched;
  EXPECT_FALSE(EncodeStopJournalFaultClearProjection(latched).ok());
}

TEST(StopJournalProjectionCodecTest, RoundTripsThePostReserveFenceFailedTriggerAndObservedTriggers) {
  auto projection = FaultClearProjection();
  projection.recovery_result.latch.trigger = StopFaultTriggerDTO::kPostReserveFenceFailed;
  projection.recovery_result.latch.observed_triggers = {
      StopFaultTriggerDTO::kScopePromotion, StopFaultTriggerDTO::kPostReserveFenceFailed};
  const auto encoded = EncodeStopJournalFaultClearProjection(projection);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodeStopJournalFaultClearProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), projection);
  EXPECT_EQ(decoded.value().recovery_result.latch.trigger,
            StopFaultTriggerDTO::kPostReserveFenceFailed);
  EXPECT_EQ(decoded.value().recovery_result.latch.observed_triggers,
            projection.recovery_result.latch.observed_triggers);
}

TEST(StopJournalProjectionCodecTest, BoundedFallbackListsRoundTripAndSingleModeWireIsRefused) {
  for (const auto& modes : std::vector<std::vector<MotionInterruptStopMode>>{
           {MotionInterruptStopMode::kSpringBackHold},
           {MotionInterruptStopMode::kSpringBackHold, MotionInterruptStopMode::kSpringBackFast}}) {
    MotionInterruptStopProfileDTO profile;
    profile.profile_id = "journal.fallback";
    profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
    profile.deceleration_time_s = 0.5;
    profile.jerk_limits_enabled = false;
    profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}},
                                          {"joint_2", AngularAccelerationDTO{2.0}}};
    profile.fallback_stop_modes = modes;
    const auto projection = TerminalProjection(profile);
    ExpectExactRoundTrip(projection);
    if (modes.size() != 1U) continue;
    // The historical single-mode profile wire is no longer normalized.
    std::size_t replaced = 0U;
    const auto old_profile = [&](const auto& self, const V& value) -> V {
      if (!value.is_array()) return value;
      auto fields = value.as_array();
      if (fields.size() == 12U && fields.front().is_string() &&
          fields.front().as_string() == "journal.fallback") {
        fields[5] = V(static_cast<double>(MotionInterruptStopMode::kSpringBackHold));
        ++replaced;
      } else {
        for (auto& item : fields) item = self(self, item);
      }
      return V(std::move(fields));
    };
    const auto encoded = EncodeStopJournalTerminalProjection(projection).value();
    const auto old = content_identity::canonical_json(
        old_profile(old_profile, content_identity::parse_canonical_json(encoded)));
    ASSERT_EQ(replaced, 1U);
    EXPECT_FALSE(DecodeStopJournalTerminalProjection(old).ok());
  }
}

[[nodiscard]] MotionInterruptStopProfileDTO SingleFallbackProfile() {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "journal.fallback";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.5;
  profile.jerk_limits_enabled = false;
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}},
                                        {"joint_2", AngularAccelerationDTO{2.0}}};
  profile.fallback_stop_modes = {MotionInterruptStopMode::kSpringBackHold};
  return profile;
}

TEST(StopJournalProjectionCodecTest, SingleModeSubstitutionContractWireIsRefused) {
  // The decoder has no scalar (pre-list) substitution-contract branch: a
  // historical single executed mode in contract slot 2 is refused.
  const auto projection = TerminalProjection(SingleFallbackProfile());
  ExpectExactRoundTrip(projection);
  auto root = Root(EncodeStopJournalTerminalProjection(projection).value());
  std::size_t replaced = 0U;
  auto plans = root[4].as_array();
  for (auto& plan_wire : plans) {
    auto plan = plan_wire.as_array();
    auto stages = plan[12].as_array();
    for (auto& stage_wire : stages) {
      auto stage = stage_wire.as_array();
      auto resolved = stage[0].as_array();
      auto qualification = resolved[6].as_array();
      if (qualification[1].is_null()) continue;
      auto contract = qualification[1].as_array();
      ASSERT_EQ(contract[2], V(V::Array{V(static_cast<double>(
                                 MotionControlledStopMode::kSpringBackHold))}));
      contract[2] = V(static_cast<double>(MotionControlledStopMode::kSpringBackHold));
      qualification[1] = V(std::move(contract));
      resolved[6] = V(std::move(qualification));
      stage[0] = V(std::move(resolved));
      stage_wire = V(std::move(stage));
      ++replaced;
    }
    plan[12] = V(std::move(stages));
    plan_wire = V(std::move(plan));
  }
  root[4] = V(std::move(plans));
  ASSERT_GT(replaced, 0U);
  const auto decoded = DecodeStopJournalTerminalProjection(Bytes(std::move(root)));
  ASSERT_FALSE(decoded.ok());
  EXPECT_NE(Reason(decoded.error()).find("fallback modes is not an array"), std::string::npos)
      << Reason(decoded.error());
}

TEST(StopJournalProjectionCodecTest, LegacyFallbackProviderPlanIsNormalizedAtMakeAndRoundTrips) {
  // A provider plan using the historical key is normalized by the existing
  // MakeResolvedStopBasePlan boundary, so its Stop projection records and
  // reads back with the same fallback (soft on path, then spring-back hold).
  const auto base = BasePlan(true);
  const auto& defaults = *base.escalation_stages.front().default_contract_parameters;
  EXPECT_FALSE(defaults.contains("recoverable_substitution_mode"));
  const JsonValueDTO hold_only{JsonValueDTO::Array{JsonValueDTO{"spring_back_hold"}}};
  ASSERT_TRUE(defaults.contains("fallback_stop_modes"));
  EXPECT_EQ(defaults.at("fallback_stop_modes"), hold_only);

  const auto projection = TerminalProjection(std::nullopt, true);
  ExpectExactRoundTrip(projection);
  const auto decoded =
      DecodeStopJournalTerminalProjection(EncodeStopJournalTerminalProjection(projection).value());
  ASSERT_TRUE(decoded.ok()) << Reason(decoded.error());
  const auto& plan =
      decoded.value().certificate.attempted_stage_results.front().result.request.participant_stop_plan;
  const auto& nominal = plan.stage_commands.front();
  ASSERT_TRUE(nominal.resolved_stage.default_contract_parameters.has_value());
  EXPECT_EQ(nominal.resolved_stage.default_contract_parameters->at("fallback_stop_modes"), hold_only);
  EXPECT_EQ(nominal.resolved_stage.command_qualification.controlled_substitution_contract
                ->fallback_stop_modes,
            std::vector<MotionControlledStopMode>{MotionControlledStopMode::kSpringBackHold});
  for (const auto& stage : plan.stage_commands) {
    for (const auto* parameters : {&stage.resolved_stage.default_contract_parameters,
                                   &stage.validated_contract_parameters}) {
      if (parameters->has_value()) {
        EXPECT_FALSE((*parameters)->contains("recoverable_substitution_mode"));
      }
    }
  }
}

TEST(StopJournalProjectionCodecTest, LegacyFallbackParameterKeyIsRefusedByEncodeAndDecode) {
  // Encode and decode are symmetric: a parameter object still carrying the
  // historical key (it never passed the normalizing boundary) is refused with
  // the typed codec error at encode, so a journal writer cannot record bytes
  // that it cannot read back; such bytes are refused at decode as well.
  const auto valid = TerminalProjection(SingleFallbackProfile());
  ExpectExactRoundTrip(valid);
  auto projection = valid;
  std::size_t rewritten = 0U;
  for (auto* attempts : {&projection.certificate.attempted_stage_results,
                         &projection.stop_status.attempted_stage_results}) {
    for (auto& attempt : *attempts) {
      for (auto& stage : attempt.result.request.participant_stop_plan.stage_commands) {
        if (!stage.validated_contract_parameters.has_value()) continue;
        auto& parameters = *stage.validated_contract_parameters;
        if (parameters.erase("fallback_stop_modes") == 0U) continue;
        parameters.emplace("recoverable_substitution_mode",
                           JsonValueDTO{std::string("spring_back_hold")});
        ++rewritten;
      }
    }
  }
  ASSERT_GT(rewritten, 0U);
  const auto encoded = EncodeStopJournalTerminalProjection(projection);
  ASSERT_FALSE(encoded.ok());
  EXPECT_EQ(encoded.error().code, "invalid_stop_journal_projection");
  EXPECT_NE(Reason(encoded.error()).find("legacy Stop fallback parameters are refused"),
            std::string::npos)
      << Reason(encoded.error());

  // The same legacy object written directly into canonical bytes.
  auto root = Root(EncodeStopJournalTerminalProjection(valid).value());
  std::size_t replaced = 0U;
  auto plans = root[4].as_array();
  for (auto& plan_wire : plans) {
    auto plan = plan_wire.as_array();
    auto stages = plan[12].as_array();
    for (auto& stage_wire : stages) {
      auto stage = stage_wire.as_array();
      // Wire of a JSON object: [5, [[key, value]...]] in key order.
      if (!stage[1].is_array()) continue;
      auto object = stage[1].as_array();
      V::Array rows;
      bool had_fallback = false;
      for (const auto& row : object[1].as_array()) {
        if (row.as_array()[0].as_string() == "fallback_stop_modes") {
          had_fallback = true;
          continue;
        }
        rows.push_back(row);
      }
      if (!had_fallback) continue;
      rows.push_back(V(V::Array{V("recoverable_substitution_mode"),
                                V(V::Array{V(3.0), V("spring_back_hold")})}));
      std::sort(rows.begin(), rows.end(), [](const V& left, const V& right) {
        return left.as_array()[0].as_string() < right.as_array()[0].as_string();
      });
      object[1] = V(std::move(rows));
      auto parameters = std::move(object);
      stage[1] = V(std::move(parameters));
      stage_wire = V(std::move(stage));
      ++replaced;
    }
    plan[12] = V(std::move(stages));
    plan_wire = V(std::move(plan));
  }
  root[4] = V(std::move(plans));
  ASSERT_GT(replaced, 0U);
  const auto decoded = DecodeStopJournalTerminalProjection(Bytes(std::move(root)));
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(Reason(decoded.error()), "legacy Stop fallback parameters are refused");
}

TEST(StopJournalProjectionCodecTest, ClearedLatchWithAnUnclearedRecoveryStatusIsRefused) {
  // The lean clear keeps M3's latch validation (stop_fault_latch_field_error):
  // state Cleared with a recovery status that is not Cleared is refused at
  // encode, and its canonical bytes are refused at decode.
  const auto valid = FaultClearProjection();
  auto malformed = valid;
  malformed.recovery_result.latch.recovery_status = StopFaultRecoveryStatusDTO::kReady;
  const auto encoded = EncodeStopJournalFaultClearProjection(malformed);
  ASSERT_FALSE(encoded.ok());
  EXPECT_EQ(encoded.error().code, "invalid_stop_journal_projection");
  EXPECT_EQ(Reason(encoded.error()), "fault-clear projection is invalid");

  auto root = Root(EncodeStopJournalFaultClearProjection(valid).value());
  auto result = root[2].as_array();
  auto latch = result[3].as_array();
  ASSERT_EQ(latch[16], V(static_cast<double>(StopFaultRecoveryStatusDTO::kCleared)));
  latch[16] = V(static_cast<double>(StopFaultRecoveryStatusDTO::kReady));
  result[3] = V(std::move(latch));
  root[2] = V(std::move(result));
  const auto decoded = DecodeStopJournalFaultClearProjection(Bytes(std::move(root)));
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error().code, "invalid_stop_journal_projection");
}

// A monitoring checkpoint and its current (non-cleared) latch for `terminal`.
[[nodiscard]] StopJournalSurveillanceProjectionDTO SurveillanceProjection() {
  const auto terminal = TerminalProjection();
  const auto& certificate = terminal.certificate;
  auto latch = ClearedLatch(certificate);
  latch.clear_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kNotApplicable, std::nullopt, std::nullopt};
  latch.state = StopFaultLatchStateDTO::kLatched;
  latch.recovery_assessment_revision = 1U;
  latch.recovery_status = StopFaultRecoveryStatusDTO::kWaitingForObservation;
  latch.reason_code = "stop_fault_latched";
  latch.evidence_refs.clear();
  const auto& attempt = certificate.attempted_stage_results.front();
  const auto& requirement = attempt.result.request.participant_stop_plan.stage_commands.front()
                                .resolved_stage.evidence_requirements.front();
  const StopPostTerminalSurveillanceDTO checkpoint{
      certificate.stop_id,
      certificate.stop_generation,
      *latch.stop_certificate,
      latch.latch_id,
      latch.latch_generation,
      RequestOwnerEpoch("owner.1"),
      1U,
      {ResourceId("joint.1"), ResourceId("joint.2")},
      {StopSurveillanceSourceStatusDTO{attempt.scope_key, requirement.requirement.evidence_item_id,
                                       requirement.resolved_source,
                                       StopSurveillanceSourceAvailabilityDTO::kAvailable,
                                       RuntimeEpoch("provider.epoch"), 5U, 0, std::nullopt}},
      StopSurveillancePolicyDTO{10'000'000, 100'000'000, 8U},
      0U,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      StopPostTerminalSurveillanceStateDTO::kMonitoring,
      1U,
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt}};
  return StopJournalSurveillanceProjectionDTO{checkpoint, latch};
}

TEST(StopJournalProjectionCodecTest, SurveillanceLatchWithoutCertificateIsRefused) {
  // SX-310: a latch without its Stop certificate is refused with the typed
  // codec error (never dereferenced), at encode and at decode.
  const auto valid = SurveillanceProjection();
  const auto encoded = EncodeStopJournalSurveillanceProjection(valid);
  ASSERT_TRUE(encoded.ok()) << Reason(encoded.error());
  const auto decoded = DecodeStopJournalSurveillanceProjection(encoded.value());
  ASSERT_TRUE(decoded.ok()) << Reason(decoded.error());
  EXPECT_EQ(decoded.value(), valid);

  auto absent = valid;
  absent.fault_latch.stop_certificate.reset();
  ASSERT_FALSE(stop_fault_latch_field_error(absent.fault_latch).has_value());
  const auto refused = EncodeStopJournalSurveillanceProjection(absent);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "invalid_stop_journal_projection");
  EXPECT_EQ(Reason(refused.error()), "surveillance projection is invalid");

  auto root = Root(encoded.value());
  auto latch = root[2].as_array();
  ASSERT_TRUE(latch[3].is_array());
  latch[3] = V(nullptr);
  root[2] = V(std::move(latch));
  const auto refused_bytes = DecodeStopJournalSurveillanceProjection(Bytes(std::move(root)));
  ASSERT_FALSE(refused_bytes.ok());
  EXPECT_EQ(refused_bytes.error().code, "invalid_stop_journal_projection");
}

TEST(StopJournalProjectionCodecTest, TerminalV3AndFaultClearV2BytesAreRefused) {
  const auto terminal = EncodeStopJournalTerminalProjection(TerminalProjection()).value();
  const auto terminal_root = Root(terminal);
  const std::string terminal_v3 = "manyforge.internal.stop-journal-terminal-projection.v3";
  for (auto root : {terminal_root,
                    V::Array{V(terminal_v3), terminal_root[1], terminal_root[2], V(nullptr)}}) {
    root[0] = V(terminal_v3);
    const auto decoded = DecodeStopJournalTerminalProjection(Bytes(std::move(root)));
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(Reason(decoded.error()),
              "Stop journal terminal projection API version is unsupported");
  }

  const auto clear = EncodeStopJournalFaultClearProjection(FaultClearProjection()).value();
  const auto clear_root = Root(clear);
  const std::string clear_v2 = "manyforge.internal.stop-journal-fault-clear-projection.v2";
  for (auto root : {clear_root, V::Array{V(clear_v2), clear_root[1], clear_root[2],
                                         terminal_root[1], terminal_root[2]}}) {
    root[0] = V(clear_v2);
    const auto decoded = DecodeStopJournalFaultClearProjection(Bytes(std::move(root)));
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(Reason(decoded.error()),
              "Stop journal fault-clear projection API version is unsupported");
  }
}

}  // namespace
}  // namespace manyforge::core
