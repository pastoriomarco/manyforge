#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace manyforge::core;

class ScopedTemporaryDirectory final {
 public:
  ScopedTemporaryDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-operation-journal-test-" +
             std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
             std::to_string(next.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create operation journal test root");
    }
  }

  ~ScopedTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] OperationIntentDTO CommandIntent(std::string request_id, std::string owner_epoch) {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)),
                              RequestOwnerEpoch(std::move(owner_epoch)), "journal-test",
                              std::nullopt},
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      GraphRevision("graph-revision"),
      RetainedBundleId("retained-bundle"),
      std::nullopt,
      {OperationParticipantIntentDTO{
          ComponentRoleId("robot"), TargetRoleId("gripper"), CapabilityId("tool.gripper.command"),
          std::nullopt,
          CapabilityCommandIntentDTO{
              CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
              CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
              GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
              2.0,
              {CommandDurationOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}}},
      OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] OperationStateDTO OperationState(std::string operation_id, OperationStateKind kind,
                                               std::int64_t updated_at_ns) {
  const OperationId id(std::move(operation_id));
  return OperationStateDTO{
      id,
      OperationRevision(1U),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      OperationCoordinationPolicy::kIndependent,
      kind,
      {OperationParticipantStateDTO{OperationParticipantId(id.value() + ":participant:1"),
                                    ControllerGoalId(id.value() + ":controller-goal:1"),
                                    ControllerGoalGeneration(1U),
                                    ControllableComponentId("robot.1"),
                                    CommandTargetId("gripper"),
                                    CapabilityId("tool.gripper.command"),
                                    ControlDomainId("domain.gripper"),
                                    ControlDomainRevision("domain-revision"),
                                    ControlBindingId("binding.gripper"),
                                    ControlBindingRevision("binding-revision"),
                                    std::nullopt,
                                    std::nullopt,
                                    OperationParticipantPayloadKind::kCapabilityCommand,
                                    kind,
                                    kind == OperationStateKind::kCompleted ? 1.0 : 0.25,
                                    {ResourceId("robot.gripper")},
                                    std::nullopt}},
      updated_at_ns,
      std::nullopt,
      std::nullopt,
      std::nullopt};
}

[[nodiscard]] OperationAcceptedDTO Acceptance(const OperationIntentDTO& intent,
                                              const OperationStateDTO& state) {
  return OperationAcceptedDTO{
      intent.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew,
      state.operation_id,
      state.operation_revision,
      {OperationParticipantAssignmentDTO{0U, state.participants.front().participant_id}},
      state.updated_at_ns};
}

[[nodiscard]] ContentDigest Digest(char digit) {
  return ContentDigest("sha256:" + std::string(64U, digit));
}

[[nodiscard]] EmergencyStopAcceptanceRecordDTO StopAcceptanceRecord(
    const RequestOwnerEpoch& owner_epoch, std::string request_id, std::string stop_id,
    std::uint64_t stop_generation, std::int64_t accepted_at_ns,
    StopResolutionDispositionDTO disposition = StopResolutionDispositionDTO::kExact,
    EmergencyStopAcceptancePathDTO path = EmergencyStopAcceptancePathDTO::kFreshReserved) {
  return EmergencyStopAcceptanceRecordDTO{
      owner_epoch,
      RequestId(std::move(request_id)),
      Digest('a'),
      std::nullopt,
      RequestAttributionDTO{},
      RequestReplayStatusDTO::kAcceptedNew,
      StopId(std::move(stop_id)),
      StopGeneration(stop_generation),
      accepted_at_ns,
      accepted_at_ns + 1'000'000'000,
      disposition,
      Digest('b'),
      EmergencyStopEntryStateDTO::kActive,
      path,
      EmergencyStopProjectionStateDTO::kPending,
  };
}

[[nodiscard]] EmergencyStopAffectedStateSnapshotDTO StopAffectedState(
    std::vector<OperationStateDTO> operations, std::vector<ResourceId> resources) {
  return EmergencyStopAffectedStateSnapshotDTO{std::move(operations), std::move(resources)};
}

[[nodiscard]] StopAcceptedDTO StopAcceptedProjection(
    const EmergencyStopAcceptanceRecordDTO& acceptance,
    const EmergencyStopProjectedIntentDTO& intent,
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  ContentDigest requested_selector_digest = Digest('c');
  auto reason = StopScopeResolutionReasonDTO::kExactCurrentScope;
  std::visit(
      [&](const auto& selected) {
        using Intent = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Intent, StopIntentDTO>) {
          requested_selector_digest = ComputeRequestedStopSelectorDigest(selected).value();
        } else if constexpr (std::is_same_v<Intent, FullStopIntentDTO>) {
          requested_selector_digest = ComputeRequestedStopSelectorDigest(selected).value();
          reason = StopScopeResolutionReasonDTO::kExplicitFullStop;
        } else {
          reason = StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent;
        }
      },
      intent);

  std::vector<StopAffectedOperationRevisionDTO> affected_operations;
  std::vector<StopAcceptedParticipantBudgetDTO> participant_budgets;
  affected_operations.reserve(affected_state.operations.size());
  for (const auto& operation : affected_state.operations) {
    affected_operations.push_back({operation.operation_id, operation.operation_revision});
    for (const auto& participant : operation.participants) {
      participant_budgets.push_back(StopAcceptedParticipantBudgetDTO{
          acceptance.stop_id,
          acceptance.stop_generation,
          operation.operation_id,
          participant.participant_id,
          Digest('d'),
          {StopAcceptedPhaseBudgetDTO{StopBudgetPhaseId("phase.stop"), 900'000'000, 900'000'000}}});
    }
  }
  std::sort(participant_budgets.begin(), participant_budgets.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.operation_id, left.participant_id) <
                     std::tie(right.operation_id, right.participant_id);
            });
  const auto remaining = acceptance.outer_deadline_ns - acceptance.accepted_at_ns;
  return StopAcceptedDTO{
      acceptance.request_id,
      acceptance.replay_status,
      acceptance.stop_id,
      acceptance.stop_generation,
      std::move(requested_selector_digest),
      acceptance.resolution_disposition,
      reason,
      {},
      acceptance.captured_scope_digest,
      {},
      std::move(affected_operations),
      1'000'000'000,
      100'000'000,
      remaining,
      acceptance.accepted_at_ns,
      std::move(participant_budgets),
  };
}

[[nodiscard]] EmergencyStopAcceptanceProjectionResultDTO ProjectStopAccepted(
    SqliteOperationJournal& journal, const RequestOwnerEpoch& owner_epoch,
    const EmergencyStopAcceptanceRecordDTO& acceptance,
    const EmergencyStopProjectedIntentDTO& intent,
    const EmergencyStopAffectedStateSnapshotDTO& affected_state) {
  auto normalized_acceptance = acceptance;
  std::visit(
      [&](const auto& selected) {
        using Intent = std::decay_t<decltype(selected)>;
        if constexpr (!std::is_same_v<Intent, std::monostate>) {
          normalized_acceptance.normalized_request_digest =
              ComputeNormalizedStopIntentDigest(selected).value();
        }
      },
      intent);
  return journal.RecordStopAccepted(
      owner_epoch, normalized_acceptance, intent,
      StopAcceptedProjection(normalized_acceptance, intent, affected_state), affected_state);
}

[[nodiscard]] StopJournalTerminalProjectionDTO EmptyTerminalProjection(
    const StopAcceptedDTO& accepted) {
  StopCertificateDTO certificate{
      StopCertificateId("certificate:" + accepted.stop_id.value() + ":" +
                        std::to_string(accepted.stop_generation.value())),
      StopCertificateRevision(1U),
      accepted.stop_id,
      accepted.stop_generation,
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       accepted.requested_selector_digest, std::nullopt,
                                       std::nullopt},
      accepted.resolution_disposition,
      accepted.resolution_reason,
      accepted.stale_diagnostics,
      {StopScopeCaptureHistoryDTO{1U,
                                  accepted.resolution_disposition,
                                  accepted.resolution_reason,
                                  accepted.captured_scope_digest,
                                  accepted.joined_predecessor_stops,
                                  accepted.affected_operation_revisions,
                                  {}}},
      {},
      {},
      {},
      {},
      StopCertificateResultDTO::kSucceededNominal,
      {},
      {},
      {},
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      accepted.accepted_at_ns + 1,
      std::nullopt};
  const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                        certificate.stop_certificate_revision, certificate.stop_id,
                                        certificate.stop_generation};
  StopStatusDTO status{accepted,
                       StopStatusStateDTO::kSucceeded,
                       {},
                       {},
                       {},
                       {},
                       {},
                       {},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                                      std::nullopt, std::nullopt},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                      std::nullopt, std::nullopt},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                                      std::nullopt, std::nullopt},
                       reference,
                       std::nullopt,
                       std::nullopt};
  EXPECT_FALSE(stop_terminal_projection_field_error(status, certificate, std::nullopt).has_value());
  return StopJournalTerminalProjectionDTO{std::move(status), std::move(certificate), std::nullopt};
}

[[nodiscard]] StopJournalTerminalProjectionDTO MonitoringTerminalProjection(
    const StopAcceptedDTO& accepted, const ResourceId& resource) {
  auto projection = EmptyTerminalProjection(accepted);
  const StopCertificateRefDTO reference{
      projection.certificate.stop_certificate_id, projection.certificate.stop_certificate_revision,
      projection.certificate.stop_id, projection.certificate.stop_generation};
  StopFaultLatchDTO latch{
      CellId("cell.1"),
      accepted.stop_id,
      accepted.stop_generation,
      reference,
      StopFaultLatchId("latch." + accepted.stop_id.value()),
      StopFaultLatchGeneration(1U),
      StopFaultTriggerDTO::kScopePromotion,
      false,
      {},
      {resource},
      RetainedBundleId("retained-bundle"),
      GraphRevision("graph-revision"),
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable, std::nullopt,
                                     std::nullopt},
      StopFaultLatchStateDTO::kLatched,
      1U,
      StopFaultRecoveryStatusDTO::kWaitingForObservation,
      "stop_fault_latched",
      {}};
  projection.stop_status.fault_latch = latch;
  projection.fault_latch = std::move(latch);
  EXPECT_FALSE(stop_terminal_projection_field_error(projection.stop_status, projection.certificate,
                                                    projection.fault_latch)
                   .has_value());
  return projection;
}

[[nodiscard]] StopPostTerminalSurveillanceDTO MonitoringCheckpoint(
    const StopJournalTerminalProjectionDTO& terminal, const StopFaultLatchDTO& latch,
    const ResourceId& resource, const std::uint64_t generation,
    const std::size_t retained_checkpoint_limit) {
  ResolvedStopRuntimeTargetDTO runtime_target{
      ProviderInstanceId("provider.1"),
      ProviderManifestId("provider-manifest.1"),
      ProviderManifestRevision(Digest('d').value()),
      RuntimeUnitId("runtime.1"),
      ProductId("product.1"),
      ProductContractId("product-contract.1"),
      ProductContractRevision("product-contract-revision.1"),
      Digest('e'),
      RuntimeEpoch("provider-epoch.1"),
      RuntimeEpoch("runtime-epoch.1"),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  const ResolvedStopEvidenceSourceDTO source{
      StopEvidenceSourceKind::kBindingStateAuthority, std::move(runtime_target),
      StateAuthorityId("state-authority.1"), StateAuthorityClass::kMeasured, std::nullopt};
  StopPostTerminalSurveillanceDTO checkpoint{
      terminal.certificate.stop_id,
      terminal.certificate.stop_generation,
      *latch.stop_certificate,
      latch.latch_id,
      latch.latch_generation,
      RequestOwnerEpoch("execution-host.epoch.stop-surveillance"),
      generation,
      {resource},
      {StopSurveillanceSourceStatusDTO{
          StopScopeEntryKeyDTO{terminal.certificate.stop_id, terminal.certificate.stop_generation,
                               resource},
          StopEvidenceItemId("evidence.rest"), source,
          StopSurveillanceSourceAvailabilityDTO::kUnavailable, std::nullopt, std::nullopt,
          std::nullopt,
          ErrorDTO{"stop_surveillance_source_unavailable", "source unavailable", {}}}},
      StopSurveillancePolicyDTO{10, 20, retained_checkpoint_limit},
      0U,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      StopPostTerminalSurveillanceStateDTO::kMonitoring,
      generation,
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                     std::nullopt}};
  EXPECT_FALSE(stop_post_terminal_surveillance_field_error(checkpoint).has_value());
  return checkpoint;
}

TEST(SqliteOperationJournalTest,
     EmptyRuntimeOwnerEpochCanStartAndCloseWithoutResourceRows) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "operation.sqlite3",
                                 CellId("cell.test"));
  const RequestOwnerEpoch epoch("execution-host.empty.1");
  const auto recovery = journal.BeginOwnerEpoch(epoch, 1, 8U, {});
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  EXPECT_TRUE(recovery.value().unreconciled_resources.empty());
  EXPECT_TRUE(journal.CloseOwnerEpoch(epoch, 2).ok());
}

TEST(SqliteOperationJournalTest, StopTerminalProjectionIsAtomicReplaySafeAndRecoveredAfterRestart) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-terminal");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  std::optional<StopJournalTerminalProjectionDTO> terminal;
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    const FullStopIntentDTO intent{PublicRequestContextDTO{RequestId("request.stop.terminal"),
                                                           epoch, "journal-test", std::nullopt},
                                   StopGeneration(0U), RequestAttributionDTO{}};
    auto acceptance = StopAcceptanceRecord(epoch, intent.request_context.request_id.value(),
                                           "stop.terminal", 1U, 10);
    acceptance.normalized_request_digest = ComputeNormalizedStopIntentDigest(intent).value();
    const EmergencyStopProjectedIntentDTO projected_intent(intent);
    const auto affected = StopAffectedState({}, inventory);
    const auto accepted = StopAcceptedProjection(acceptance, projected_intent, affected);
    const auto acceptance_result =
        journal.RecordStopAccepted(epoch, acceptance, projected_intent, accepted, affected);
    ASSERT_EQ(acceptance_result.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
        << (acceptance_result.error.has_value() ? acceptance_result.error->code : std::string{})
        << ": "
        << (acceptance_result.error.has_value() ? acceptance_result.error->message : std::string{});
    terminal = EmptyTerminalProjection(accepted);
    const auto first = journal.RecordStopTerminal(epoch, terminal->stop_status,
                                                  terminal->certificate, terminal->fault_latch);
    EXPECT_EQ(first.state, StopForensicProjectionStateDTO::kRecorded);
    ASSERT_TRUE(first.journal_sequence.has_value());

    const auto operational =
        journal.ReadOperationalEvents(epoch, 0U, kMaximumOperationJournalEventPage);
    ASSERT_TRUE(operational.ok()) << operational.error().message;
    ASSERT_EQ(operational.value().events.size(), 1U);
    EXPECT_EQ(operational.value().events.front().record_kind,
              "stop_acceptance_projected");
    EXPECT_FALSE(operational.value().has_more);

    const auto forensic =
        journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
    ASSERT_TRUE(forensic.ok()) << forensic.error().message;
    ASSERT_EQ(forensic.value().events.size(), 2U);
    EXPECT_EQ(forensic.value().events.back().record_kind,
              "stop_terminal_projected");
    const auto replay = journal.RecordStopTerminal(epoch, terminal->stop_status,
                                                   terminal->certificate, terminal->fault_latch);
    EXPECT_EQ(replay.state, StopForensicProjectionStateDTO::kRecorded);
    EXPECT_EQ(replay.journal_sequence, first.journal_sequence);

    auto conflict = *terminal;
    conflict.certificate.completed_at_ns += 1;
    const auto conflicting = journal.RecordStopTerminal(epoch, conflict.stop_status,
                                                        conflict.certificate, conflict.fault_latch);
    EXPECT_EQ(conflicting.state, StopForensicProjectionStateDTO::kFailed);
    EXPECT_FALSE(conflicting.journal_sequence.has_value());
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered = replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.stop-terminal.replacement"), 30, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().recovered_stops.size(), 1U);
  const auto& recovered_stop = recovered.value().recovered_stops.front();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(recovered_stop.accepted, terminal->stop_status.accepted);
  ASSERT_TRUE(recovered_stop.terminal_certificate.has_value());
  EXPECT_EQ(*recovered_stop.terminal_certificate, terminal->certificate);
  EXPECT_FALSE(recovered_stop.fault_latch.has_value());
  EXPECT_FALSE(recovered_stop.surveillance.has_value());
  ASSERT_TRUE(recovered.value().replaced_unclosed_owner_epoch.has_value());
  EXPECT_EQ(*recovered.value().replaced_unclosed_owner_epoch, epoch);
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest,
     MonitoringCheckpointsUseExplicitProjectionTimeAndRetainABoundedRestartableTail) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-surveillance");
  const ResourceId resource("robot.gripper");
  const std::vector<ResourceId> inventory{resource};
  std::optional<StopPostTerminalSurveillanceDTO> latest;
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    const FullStopIntentDTO intent{PublicRequestContextDTO{RequestId("request.stop.surveillance"),
                                                           epoch, "journal-test", std::nullopt},
                                   StopGeneration(0U), RequestAttributionDTO{}};
    auto acceptance = StopAcceptanceRecord(epoch, intent.request_context.request_id.value(),
                                           "stop.surveillance", 1U, 10);
    acceptance.normalized_request_digest = ComputeNormalizedStopIntentDigest(intent).value();
    const EmergencyStopProjectedIntentDTO projected_intent(intent);
    const auto affected = StopAffectedState({}, inventory);
    const auto accepted = StopAcceptedProjection(acceptance, projected_intent, affected);
    ASSERT_EQ(journal.RecordStopAccepted(epoch, acceptance, projected_intent, accepted, affected)
                  .projection_state,
              EmergencyStopProjectionStateDTO::kRecorded);
    auto terminal = MonitoringTerminalProjection(accepted, resource);
    ASSERT_EQ(journal
                  .RecordStopTerminal(epoch, terminal.stop_status, terminal.certificate,
                                      terminal.fault_latch)
                  .state,
              StopForensicProjectionStateDTO::kRecorded);
    ASSERT_TRUE(terminal.fault_latch.has_value());
    auto durable_latch = *terminal.fault_latch;
    durable_latch.terminal_projection = StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};

    const auto invalid_time = journal.RecordStopSurveillanceCheckpoint(
        epoch, MonitoringCheckpoint(terminal, durable_latch, resource, 1U, 2U), durable_latch, -1);
    EXPECT_EQ(invalid_time.state, StopForensicProjectionStateDTO::kFailed);

    for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
      latest = MonitoringCheckpoint(terminal, durable_latch, resource, generation, 2U);
      const auto projected = journal.RecordStopSurveillanceCheckpoint(
          epoch, *latest, durable_latch, static_cast<std::int64_t>(100U + generation));
      ASSERT_EQ(projected.state, StopForensicProjectionStateDTO::kRecorded)
          << (projected.error.has_value() ? projected.error->message : std::string{});
    }

    const auto events = journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
    ASSERT_TRUE(events.ok()) << events.error().message;
    std::vector<std::int64_t> checkpoint_times;
    for (const auto& event : events.value().events) {
      if (event.record_kind == "stop_surveillance_checkpoint") {
        checkpoint_times.push_back(event.occurred_at_ns);
      }
    }
    EXPECT_EQ(checkpoint_times, (std::vector<std::int64_t>{103, 104}));
    EXPECT_FALSE(journal.CloseOwnerEpoch(epoch, 200).ok());
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered = replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.stop-surveillance.replacement"), 300, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().recovered_stops.size(), 1U);
  ASSERT_TRUE(recovered.value().recovered_stops.front().surveillance.has_value());
  ASSERT_TRUE(latest.has_value());
  auto expected = *latest;
  expected.checkpoint_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  EXPECT_EQ(*recovered.value().recovered_stops.front().surveillance, expected);
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest,
     FaultClearAtomicallyBridgesOwnerAssessmentAndRestartsWithExactAuthority) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-fault-clear");
  const ResourceId resource("robot.gripper");
  const std::vector<ResourceId> inventory{resource};
  std::optional<StopStatusDTO> expected_status;
  std::optional<StopCertificateDTO> expected_certificate;
  std::optional<StopFaultLatchDTO> expected_latch;
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    const FullStopIntentDTO stop_intent{
        PublicRequestContextDTO{RequestId("request.stop.fault-clear"), epoch, "journal-test",
                                std::nullopt},
        StopGeneration(0U), RequestAttributionDTO{}};
    auto acceptance = StopAcceptanceRecord(epoch, stop_intent.request_context.request_id.value(),
                                           "stop.fault-clear", 1U, 10);
    acceptance.normalized_request_digest = ComputeNormalizedStopIntentDigest(stop_intent).value();
    const EmergencyStopProjectedIntentDTO projected_intent(stop_intent);
    const auto affected = StopAffectedState({}, inventory);
    const auto accepted = StopAcceptedProjection(acceptance, projected_intent, affected);
    ASSERT_EQ(journal.RecordStopAccepted(epoch, acceptance, projected_intent, accepted, affected)
                  .projection_state,
              EmergencyStopProjectionStateDTO::kRecorded);

    auto terminal = MonitoringTerminalProjection(accepted, resource);
    ASSERT_EQ(journal
                  .RecordStopTerminal(epoch, terminal.stop_status, terminal.certificate,
                                      terminal.fault_latch)
                  .state,
              StopForensicProjectionStateDTO::kRecorded);
    ASSERT_TRUE(terminal.fault_latch.has_value());

    auto cleared_latch = *terminal.fault_latch;
    cleared_latch.terminal_projection = StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
    cleared_latch.clear_projection = StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
    cleared_latch.state = StopFaultLatchStateDTO::kCleared;
    cleared_latch.recovery_assessment_revision = 3U;
    cleared_latch.recovery_status = StopFaultRecoveryStatusDTO::kCleared;
    cleared_latch.reason_code = "stop_fault_recovery_cleared";
    cleared_latch.evidence_refs = {
        StopFaultRecoveryEvidenceRefDTO{StopFaultRecoveryEvidenceKindDTO::kSettlement, Digest('e')},
        StopFaultRecoveryEvidenceRefDTO{StopFaultRecoveryEvidenceKindDTO::kConfiguration,
                                        Digest('f')}};

    const StopFaultRecoveryIntentDTO recovery_intent{
        PublicRequestContextDTO{RequestId("request.stop-fault-clear.1"), epoch, "journal-test",
                                std::nullopt},
        cleared_latch.latch_id,
        cleared_latch.latch_generation,
        2U,
        terminal.certificate.stop_certificate_id,
        terminal.certificate.stop_certificate_revision,
        cleared_latch.cell_id,
        cleared_latch.retained_bundle_id,
        cleared_latch.graph_revision,
        {StopFaultRecoveryConfigurationExpectationDTO{
            ControlDomainId("domain.1"), ActiveControlConfigurationId("configuration.1"),
            ActiveControlConfigurationGeneration(1U)}},
        std::string("operator verified cell")};
    auto resulting_status = terminal.stop_status;
    resulting_status.terminal_projection = StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
    resulting_status.fault_latch = cleared_latch;
    const StopFaultRecoveryResultDTO recovery_result{recovery_intent.request_context.request_id,
                                                     RequestReplayStatusDTO::kAcceptedNew, true,
                                                     cleared_latch, std::nullopt};

    const auto first = journal.RecordStopFaultCleared(epoch, recovery_intent, recovery_result,
                                                      cleared_latch, resulting_status);
    ASSERT_TRUE(first.ok()) << first.error().message;
    const auto replay = journal.RecordStopFaultCleared(epoch, recovery_intent, recovery_result,
                                                       cleared_latch, resulting_status);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    EXPECT_EQ(replay.value(), first.value());

    auto conflicting_intent = recovery_intent;
    conflicting_intent.reason = "different recovery authority";
    const auto conflict = journal.RecordStopFaultCleared(epoch, conflicting_intent, recovery_result,
                                                         cleared_latch, resulting_status);
    EXPECT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error().code, "operation_journal_stop_projection_conflict");

    const auto events = journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
    ASSERT_TRUE(events.ok()) << events.error().message;
    EXPECT_EQ(
        std::count_if(events.value().events.begin(), events.value().events.end(),
                      [](const auto& event) { return event.record_kind == "stop_fault_cleared"; }),
        1);
    ASSERT_TRUE(journal.CloseOwnerEpoch(epoch, 100).ok());
    expected_status = resulting_status;
    expected_certificate = terminal.certificate;
    expected_latch = cleared_latch;
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered = replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.stop-fault-clear.replacement"), 200, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().recovered_stops.size(), 1U);
  ASSERT_TRUE(expected_status.has_value());
  ASSERT_TRUE(expected_certificate.has_value());
  ASSERT_TRUE(expected_latch.has_value());
  const auto& recovered_stop = recovered.value().recovered_stops.front();
  EXPECT_EQ(recovered_stop.accepted, expected_status->accepted);
  EXPECT_EQ(recovered_stop.terminal_certificate, expected_certificate);
  EXPECT_EQ(recovered_stop.fault_latch, expected_latch);
  EXPECT_FALSE(recovered_stop.surveillance.has_value());
  EXPECT_FALSE(recovered.value().replaced_unclosed_owner_epoch.has_value());
  EXPECT_TRUE(recovered.value().unreconciled_resources.empty());
}

TEST(SqliteOperationJournalTest, ReusedFixedFullStopIdentityRetainsDistinctTerminalGenerations) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.reused-full-stop");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  std::vector<StopJournalTerminalProjectionDTO> terminals;
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    for (std::uint64_t generation = 1U; generation <= 2U; ++generation) {
      const FullStopIntentDTO intent{
          PublicRequestContextDTO{RequestId("request.stop.reused." + std::to_string(generation)),
                                  epoch, "journal-test", std::nullopt},
          StopGeneration(generation - 1U), RequestAttributionDTO{}};
      auto acceptance =
          StopAcceptanceRecord(epoch, intent.request_context.request_id.value(), "stop.fixed-full",
                               generation, static_cast<std::int64_t>(generation * 10U));
      acceptance.normalized_request_digest = ComputeNormalizedStopIntentDigest(intent).value();
      const EmergencyStopProjectedIntentDTO projected_intent(intent);
      const auto affected = StopAffectedState({}, inventory);
      const auto accepted = StopAcceptedProjection(acceptance, projected_intent, affected);
      const auto acceptance_result =
          journal.RecordStopAccepted(epoch, acceptance, projected_intent, accepted, affected);
      ASSERT_EQ(acceptance_result.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
          << (acceptance_result.error.has_value() ? acceptance_result.error->message
                                                  : std::string{});
      terminals.push_back(EmptyTerminalProjection(accepted));
      const auto terminal =
          journal.RecordStopTerminal(epoch, terminals.back().stop_status,
                                     terminals.back().certificate, terminals.back().fault_latch);
      ASSERT_EQ(terminal.state, StopForensicProjectionStateDTO::kRecorded)
          << (terminal.error.has_value() ? terminal.error->message : std::string{});
    }
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered = replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.reused-full-stop.replacement"), 30, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().recovered_stops.size(), terminals.size());
  for (std::size_t index = 0U; index < terminals.size(); ++index) {
    EXPECT_EQ(recovered.value().recovered_stops[index].accepted,
              terminals[index].stop_status.accepted);
    ASSERT_TRUE(recovered.value().recovered_stops[index].terminal_certificate.has_value());
    EXPECT_EQ(*recovered.value().recovered_stops[index].terminal_certificate,
              terminals[index].certificate);
  }
}

TEST(SqliteOperationJournalTest,
     ReplacementRetainsHistoryAndFencesOnlyPreviouslyNonterminalResources) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto intent = CommandIntent("request.command.1", first_epoch.value());
  const auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    const auto begun = journal.BeginOwnerEpoch(first_epoch, 1, 4U, inventory);
    ASSERT_TRUE(begun.ok()) << begun.error().message;
    EXPECT_TRUE(begun.value().historical_operations.empty());
    const auto recorded =
        journal.RecordOperationAccepted(first_epoch, intent, Acceptance(intent, state), state);
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.2"), 20, 4U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  const auto& historical = recovered.value().historical_operations.front();
  EXPECT_EQ(historical.operation_id, state.operation_id);
  EXPECT_EQ(historical.state, OperationStateKind::kInterventionRequired);
  EXPECT_EQ(historical.updated_at_ns, 20);
  ASSERT_TRUE(historical.participants.front().terminal_error.has_value());
  EXPECT_EQ(historical.participants.front().terminal_error->details.at("reason"),
            "unreconciled_external_or_orphan_goal");
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
  replacement.Close();

  SqliteOperationJournal second_replacement(database, cell_id, 8U);
  const auto recovered_again = second_replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.3"), 30, 4U, inventory);
  ASSERT_TRUE(recovered_again.ok()) << recovered_again.error().message;
  ASSERT_EQ(recovered_again.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered_again.value().historical_operations.front(), historical);
  EXPECT_EQ(recovered_again.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest,
     CleanlyClosedTerminalHistoryDoesNotFenceResourcesAndOwnerEpochsCannotBeReused) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto intent = CommandIntent("request.command.1", first_epoch.value());
  auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(first_epoch, 1, 4U, inventory).ok());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(first_epoch, intent, Acceptance(intent, state), state)
            .ok());
    state.state = OperationStateKind::kCompleted;
    state.participants.front().state = OperationStateKind::kCompleted;
    state.participants.front().progress = 1.0;
    state.updated_at_ns = 15;
    ASSERT_TRUE(
        journal.RecordOperationState(first_epoch, state, nullptr, "subordinate_completed").ok());
    ASSERT_TRUE(journal.CloseOwnerEpoch(first_epoch, 16).ok());
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.2"), 20, 4U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front().state, OperationStateKind::kCompleted);
  EXPECT_TRUE(recovered.value().unreconciled_resources.empty());
  const auto reused = replacement.BeginOwnerEpoch(first_epoch, 30, 4U, inventory);
  ASSERT_FALSE(reused.ok());
  EXPECT_EQ(reused.error().code, "operation_journal_owner_epoch_reused");
}

TEST(SqliteOperationJournalTest, TypedControlScaleAndStopRecordsRoundTripAsTerminalHistory) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto intent = CommandIntent("request.command.1", first_epoch.value());
  auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  state.runtime_scale_generation = RuntimeScaleGeneration(0U);

  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(first_epoch, 1, 8U, inventory).ok());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(first_epoch, intent, Acceptance(intent, state), state)
            .ok());

    const OperationControlRequestDTO pause_request{
        PublicRequestContextDTO{RequestId("request.pause.1"), first_epoch, "journal-test",
                                std::nullopt},
        state.operation_id, state.operation_revision, std::nullopt};
    const auto prior_revision = state.operation_revision;
    state.operation_revision = OperationRevision(2U);
    state.updated_at_ns = 11;
    const OperationControlAcceptedDTO pause_accepted{pause_request.request_context.request_id,
                                                     RequestReplayStatusDTO::kAcceptedNew,
                                                     state.operation_id,
                                                     prior_revision,
                                                     OperationControlKind::kPause,
                                                     state.operation_revision,
                                                     state.updated_at_ns};
    ASSERT_TRUE(
        journal.RecordControlAccepted(first_epoch, pause_request, pause_accepted, state).ok());

    const RuntimeScaleRequestDTO scale_request{
        PublicRequestContextDTO{RequestId("request.scale.1"), first_epoch, "journal-test",
                                std::nullopt},
        state.operation_id, state.operation_revision, RuntimeScaleGeneration(0U), 0.5};
    state.runtime_scale_generation = RuntimeScaleGeneration(1U);
    state.updated_at_ns = 12;
    const RuntimeScaleAcceptedDTO scale_accepted{scale_request.request_context.request_id,
                                                 RequestReplayStatusDTO::kAcceptedNew,
                                                 state.operation_id,
                                                 state.operation_revision,
                                                 scale_request.scale,
                                                 *state.runtime_scale_generation,
                                                 state.updated_at_ns};
    const auto& participant = state.participants.front();
    const RuntimeScaleStatusDTO scale_status{
        state.operation_id,
        state.operation_revision,
        *state.runtime_scale_generation,
        scale_request.scale,
        scale_request.scale,
        {RuntimeScaleParticipantStatusDTO{participant.participant_id,
                                          participant.controllable_component_id,
                                          participant.command_target_id,
                                          participant.capability_id,
                                          participant.control_domain_id,
                                          participant.control_domain_revision,
                                          participant.control_binding_id,
                                          participant.control_binding_revision,
                                          participant.controller_goal_id,
                                          participant.controller_goal_generation,
                                          ControllerTopologyId("topology.1"),
                                          TopologyRevision("topology-revision"),
                                          ControlRouteId("route.1"),
                                          ScalingDomainId("scale.1"),
                                          std::nullopt,
                                          participant.resources,
                                          ActiveControlConfigurationId("configuration.1"),
                                          ActiveControlConfigurationGeneration(1U),
                                          *state.runtime_scale_generation,
                                          scale_request.scale,
                                          scale_request.scale,
                                          0.0,
                                          RuntimeEpoch("runtime.epoch.1"),
                                          1U,
                                          state.updated_at_ns,
                                          true,
                                          RuntimeScaleApplicationState::kApplied,
                                          std::nullopt}},
        RuntimeScaleApplicationState::kApplied,
        state.updated_at_ns};
    ASSERT_TRUE(journal
                    .RecordRuntimeScaleAccepted(first_epoch, scale_request, scale_accepted, state,
                                                scale_status)
                    .ok());

    const StopIntentDTO stop_request{
        PublicRequestContextDTO{RequestId("request.stop.1"), first_epoch, "journal-test",
                                std::nullopt},
        std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
            state.operation_id, state.operation_revision, StopGeneration(0U)}},
        std::nullopt};
    state.operation_revision = OperationRevision(3U);
    state.state = OperationStateKind::kStopping;
    state.participants.front().state = OperationStateKind::kStopping;
    state.active_stop_id = StopId("stop:execution-host.epoch.1:1");
    state.active_stop_generation = StopGeneration(1U);
    state.updated_at_ns = 13;
    const auto stop_acceptance = StopAcceptanceRecord(
        first_epoch, stop_request.request_context.request_id.value(), state.active_stop_id->value(),
        state.active_stop_generation->value(), state.updated_at_ns);
    const auto stop_projection = ProjectStopAccepted(
        journal, first_epoch, stop_acceptance, EmergencyStopProjectedIntentDTO(stop_request),
        StopAffectedState({state}, {ResourceId("robot.gripper")}));
    EXPECT_EQ(stop_projection.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
    EXPECT_TRUE(stop_projection.journal_sequence.has_value());
    EXPECT_FALSE(stop_projection.error.has_value());

    state.state = OperationStateKind::kStopped;
    state.participants.front().state = OperationStateKind::kStopped;
    state.updated_at_ns = 14;
    ASSERT_TRUE(
        journal.RecordOperationState(first_epoch, state, nullptr, "subordinate_stopped").ok());
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.2"), 20, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front(), state);
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest,
     AlreadyLinearizedFullStopProjectsCanonicalMultiOperationStateAndRecoversIt) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-full");
  const std::vector<ResourceId> inventory{ResourceId("robot.arm"), ResourceId("robot.gripper")};
  auto first = OperationState("operation:1", OperationStateKind::kExecuting, 10);
  auto second = OperationState("operation:2", OperationStateKind::kExecuting, 11);
  second.participants.front().resources = {ResourceId("robot.arm")};

  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    const auto first_intent = CommandIntent("request.operation.1", epoch.value());
    const auto second_intent = CommandIntent("request.operation.2", epoch.value());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, first_intent, Acceptance(first_intent, first), first)
            .ok());
    ASSERT_TRUE(journal
                    .RecordOperationAccepted(epoch, second_intent,
                                             Acceptance(second_intent, second), second)
                    .ok());

    const StopId predecessor_stop_id("stop:execution-host.epoch.stop-full:predecessor");
    const StopId stop_id("stop:execution-host.epoch.stop-full:aggregate");
    for (auto* state : {&first, &second}) {
      state->operation_revision = OperationRevision(2U);
      state->state = OperationStateKind::kStopping;
      state->participants.front().state = OperationStateKind::kStopping;
      state->active_stop_id = stop_id;
      state->active_stop_generation = StopGeneration(2U);
      state->updated_at_ns = 20;
    }
    first.active_stop_id = predecessor_stop_id;
    first.active_stop_generation = StopGeneration(1U);
    const FullStopIntentDTO intent{
        PublicRequestContextDTO{RequestId("request.stop.full"), epoch, "journal-test",
                                std::nullopt},
        StopGeneration(0U),
        RequestAttributionDTO{},
    };
    auto acceptance = StopAcceptanceRecord(
        epoch, intent.request_context.request_id.value(), stop_id.value(), 2U, 20,
        StopResolutionDispositionDTO::kExact, EmergencyStopAcceptancePathDTO::kFreshReserved);
    const EmergencyStopProjectedIntentDTO projected_intent(intent);
    const auto affected_state = StopAffectedState({first, second}, inventory);
    acceptance.normalized_request_digest = ComputeNormalizedStopIntentDigest(intent).value();
    auto accepted = StopAcceptedProjection(acceptance, projected_intent, affected_state);
    accepted.joined_predecessor_stops.push_back(StopPredecessorAcceptanceRefDTO{
        predecessor_stop_id, StopGeneration(1U), Digest('e'), 1'000'000'000});
    accepted.participant_budget_allocations.front().origin_stop_id = predecessor_stop_id;
    accepted.participant_budget_allocations.front().origin_stop_generation = StopGeneration(1U);
    const auto projected =
        journal.RecordStopAccepted(epoch, acceptance, projected_intent, accepted, affected_state);
    ASSERT_EQ(projected.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
    ASSERT_TRUE(projected.journal_sequence.has_value());

    const auto page = journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
    ASSERT_TRUE(page.ok()) << page.error().message;
    ASSERT_EQ(page.value().events.size(), 3U);
    const auto& event = page.value().events.back();
    EXPECT_EQ(event.sequence, *projected.journal_sequence);
    EXPECT_EQ(event.record_kind, "stop_acceptance_projected");
    EXPECT_FALSE(event.operation_id.has_value());
    EXPECT_FALSE(event.operation_revision.has_value());
    EXPECT_TRUE(event.effect_pending);
    EXPECT_NE(
        event.payload_json.find("\"api_version\":\"manyforge.stop_acceptance_projection.v2\""),
        std::string::npos);
    EXPECT_NE(event.payload_json.find("\"intent_kind\":\"full_stop\""), std::string::npos);
    EXPECT_NE(event.payload_json.find("\"joined_predecessor_stops\":[{"), std::string::npos);
    EXPECT_NE(event.payload_json.find(predecessor_stop_id.value()), std::string::npos);
    EXPECT_NE(event.payload_json.find("\"origin_stop_id\":\"" + predecessor_stop_id.value() + "\""),
              std::string::npos);
    EXPECT_NE(event.payload_json.find("\"operation_id\":\"operation:1\""), std::string::npos);
    EXPECT_NE(event.payload_json.find("\"operation_id\":\"operation:2\""), std::string::npos);
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered = replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.stop-full.replacement"), 30, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 2U);
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest,
     ExactStopProjectionReplayReturnsOriginalSequenceAndConflictAppendsNothing) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-replay");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  SqliteOperationJournal journal(database, cell_id, 8U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 1U, inventory).ok());

  auto acceptance = StopAcceptanceRecord(epoch, "request.stop.malformed",
                                         "stop:execution-host.epoch.stop-replay:1", 1U, 10,
                                         StopResolutionDispositionDTO::kPromotedToFull,
                                         EmergencyStopAcceptancePathDTO::kCoalescedFull);
  acceptance.normalized_request_digest.reset();
  acceptance.malformed_ingress_reason = "recognized malformed Stop selector";
  const EmergencyStopProjectedIntentDTO intent(std::monostate{});
  const auto affected = StopAffectedState({}, inventory);

  const auto first = ProjectStopAccepted(journal, epoch, acceptance, intent, affected);
  ASSERT_EQ(first.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
  ASSERT_TRUE(first.journal_sequence.has_value());
  const auto replay = ProjectStopAccepted(journal, epoch, acceptance, intent, affected);
  ASSERT_EQ(replay.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
  ASSERT_TRUE(replay.journal_sequence.has_value());
  EXPECT_EQ(replay.journal_sequence, first.journal_sequence);

  acceptance.malformed_ingress_reason = "different recognized malformed Stop selector";
  const auto conflict = ProjectStopAccepted(journal, epoch, acceptance, intent, affected);
  ASSERT_EQ(conflict.projection_state, EmergencyStopProjectionStateDTO::kFailed);
  EXPECT_FALSE(conflict.journal_sequence.has_value());
  ASSERT_TRUE(conflict.error.has_value());
  EXPECT_EQ(conflict.error->code, "operation_journal_stop_projection_conflict");

  const auto page = journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 1U);
  EXPECT_EQ(page.value().events.front().record_kind, "stop_acceptance_projected");
}

TEST(SqliteOperationJournalTest,
     StopProjectionFailureIsNonThrowingAndNeverConsumesOrdinaryRequestCapacity) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-failure");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  SqliteOperationJournal journal(database, cell_id, 8U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 1U, inventory).ok());

  const FullStopIntentDTO intent{
      PublicRequestContextDTO{RequestId("request.stop.full"), epoch, "journal-test", std::nullopt},
      StopGeneration(0U),
      RequestAttributionDTO{},
  };
  auto acceptance = StopAcceptanceRecord(epoch, intent.request_context.request_id.value(),
                                         "stop:execution-host.epoch.stop-failure:1", 1U, 10);
  acceptance.projection_state = EmergencyStopProjectionStateDTO::kRecorded;
  const auto invalid =
      ProjectStopAccepted(journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(intent),
                          StopAffectedState({}, inventory));
  ASSERT_EQ(invalid.projection_state, EmergencyStopProjectionStateDTO::kFailed);
  EXPECT_FALSE(invalid.journal_sequence.has_value());
  EXPECT_TRUE(invalid.error.has_value());

  acceptance.projection_state = EmergencyStopProjectionStateDTO::kPending;
  const auto projected =
      ProjectStopAccepted(journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(intent),
                          StopAffectedState({}, inventory));
  ASSERT_EQ(projected.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
  EXPECT_TRUE(projected.journal_sequence.has_value());
  journal.Close();
  EmergencyStopAcceptanceProjectionResultDTO closed;
  EXPECT_NO_THROW(closed = ProjectStopAccepted(journal, epoch, acceptance,
                                               EmergencyStopProjectedIntentDTO(intent),
                                               StopAffectedState({}, inventory)));
  EXPECT_EQ(closed.projection_state, EmergencyStopProjectionStateDTO::kFailed);
  EXPECT_FALSE(closed.journal_sequence.has_value());
  ASSERT_TRUE(closed.error.has_value());
  EXPECT_EQ(closed.error->code, "operation_journal_closed");

  SqliteOperationJournal reopened(database, cell_id, 8U);
  const auto command = CommandIntent("request.command.after-stop-failure", epoch.value());
  const auto state =
      OperationState("operation:after-stop-failure", OperationStateKind::kExecuting, 20);
  const auto ordinary =
      reopened.RecordOperationAccepted(epoch, command, Acceptance(command, state), state);
  ASSERT_TRUE(ordinary.ok()) << ordinary.error().message;
}

TEST(SqliteOperationJournalTest,
     StopProjectionDoesNotWaitForOccupiedProductionJournalCriticalSection) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-contention");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  std::promise<void> entered_promise;
  auto entered = entered_promise.get_future();
  std::promise<void> release_promise;
  auto release = release_promise.get_future().share();
  SqliteOperationJournal journal(
      database, CellId("cell.1"), 8U, {},
      [&](const std::string_view operation) {
        if (operation == "cell_admission_acquire") {
          entered_promise.set_value();
          release.wait();
        }
      });
  auto recovery = journal.BeginOwnerEpoch(epoch, 1, 8U, inventory);
  ASSERT_TRUE(recovery.ok());
  const CellAdmissionClaimDTO claim{
      CellAdmissionClaimIntentDTO{
          CellAdmissionClaimKindDTO::kAuthoringMutation,
          CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          RequestOwnerEpoch("authoring.owner.1"),
          "transaction.1",
          RequestId("request.cell-claim.1"),
          Digest('e'),
          recovery.value().cell_admission_generation_high_water,
          {},
          std::nullopt,
          std::nullopt},
      CellAdmissionClaimGeneration(1U),
      recovery.value().cell_admission_generation_high_water,
      2};
  auto admission = std::async(std::launch::async, [&] {
    return journal.RecordCellAdmissionClaimAcquired(epoch, claim);
  });
  entered.wait();

  const FullStopIntentDTO intent{
      PublicRequestContextDTO{RequestId("request.stop.contended"), epoch, "journal-test",
                              std::nullopt},
      StopGeneration(0U),
      RequestAttributionDTO{},
  };
  const auto acceptance =
      StopAcceptanceRecord(epoch, intent.request_context.request_id.value(),
                           "stop:execution-host.epoch.stop-contention:1", 1U, 10);
  const auto projected =
      ProjectStopAccepted(journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(intent),
                          StopAffectedState({}, inventory));
  release_promise.set_value();
  const auto admission_result = admission.get();
  ASSERT_EQ(projected.projection_state, EmergencyStopProjectionStateDTO::kFailed);
  ASSERT_TRUE(projected.error.has_value());
  EXPECT_EQ(projected.error->code, "operation_journal_stop_projection_unavailable");
  EXPECT_EQ(projected.error->details.at("retryable"), "true");
  EXPECT_TRUE(admission_result.ok());
}

TEST(SqliteOperationJournalTest, RejectsNoncanonicalAffectedStateWithoutAppendingProjection) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-noncanonical");
  const std::vector<ResourceId> inventory{ResourceId("robot.arm"), ResourceId("robot.gripper")};
  SqliteOperationJournal journal(database, cell_id, 8U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
  const FullStopIntentDTO intent{
      PublicRequestContextDTO{RequestId("request.stop.noncanonical"), epoch, "journal-test",
                              std::nullopt},
      StopGeneration(0U),
      RequestAttributionDTO{},
  };
  const auto acceptance =
      StopAcceptanceRecord(epoch, intent.request_context.request_id.value(),
                           "stop:execution-host.epoch.stop-noncanonical:1", 1U, 10);

  const auto unsorted_resources = ProjectStopAccepted(
      journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(intent),
      StopAffectedState({}, {ResourceId("robot.gripper"), ResourceId("robot.arm")}));
  EXPECT_EQ(unsorted_resources.projection_state, EmergencyStopProjectionStateDTO::kFailed);

  auto first = OperationState("operation:1", OperationStateKind::kStopping, 10);
  auto second = OperationState("operation:2", OperationStateKind::kStopping, 10);
  for (auto* state : {&first, &second}) {
    state->active_stop_id = acceptance.stop_id;
    state->active_stop_generation = acceptance.stop_generation;
  }
  second.participants.front().resources = {ResourceId("robot.arm")};
  const auto unsorted_operations =
      ProjectStopAccepted(journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(intent),
                          StopAffectedState({second, first}, inventory));
  EXPECT_EQ(unsorted_operations.projection_state, EmergencyStopProjectionStateDTO::kFailed);

  const auto page = journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
  ASSERT_TRUE(page.ok()) << page.error().message;
  EXPECT_TRUE(page.value().events.empty());
}

TEST(SqliteOperationJournalTest,
     LateStopAcceptanceProjectionCannotRegressNewerTerminalOperationSnapshot) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.stop-late");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  auto state = OperationState("operation:late", OperationStateKind::kExecuting, 10);
  SqliteOperationJournal journal(database, cell_id, 8U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
  const auto operation_intent = CommandIntent("request.operation.late", epoch.value());
  ASSERT_TRUE(journal
                  .RecordOperationAccepted(epoch, operation_intent,
                                           Acceptance(operation_intent, state), state)
                  .ok());

  auto captured = state;
  captured.operation_revision = OperationRevision(2U);
  captured.state = OperationStateKind::kStopping;
  captured.participants.front().state = OperationStateKind::kStopping;
  captured.active_stop_id = StopId("stop:execution-host.epoch.stop-late:1");
  captured.active_stop_generation = StopGeneration(1U);
  captured.updated_at_ns = 11;

  state = captured;
  state.operation_revision = OperationRevision(3U);
  state.state = OperationStateKind::kStopped;
  state.participants.front().state = OperationStateKind::kStopped;
  state.updated_at_ns = 12;
  ASSERT_TRUE(journal.RecordOperationState(epoch, state, nullptr, "stop_terminal").ok());

  const StopIntentDTO intent{
      PublicRequestContextDTO{RequestId("request.stop.late"), epoch, "journal-test", std::nullopt},
      std::vector<StopOperationTargetDTO>{
          StopOperationTargetDTO{captured.operation_id, OperationRevision(1U), StopGeneration(0U)}},
      std::nullopt,
  };
  const auto acceptance =
      StopAcceptanceRecord(epoch, intent.request_context.request_id.value(),
                           captured.active_stop_id->value(), 1U, captured.updated_at_ns);
  const auto projection =
      ProjectStopAccepted(journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(intent),
                          StopAffectedState({captured}, inventory));
  ASSERT_EQ(projection.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
  journal.Close();

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered = replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.stop-late.replacement"), 20, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front(), state);
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest, UnresolvedOrphanFenceSurvivesBoundedHistoryCompaction) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.arm"), ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto orphan_intent = CommandIntent("request.command.1", first_epoch.value());
  const auto orphan_state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell_id, 1U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(first_epoch, 1, 4U, inventory).ok());
    ASSERT_TRUE(journal
                    .RecordOperationAccepted(first_epoch, orphan_intent,
                                             Acceptance(orphan_intent, orphan_state), orphan_state)
                    .ok());
    journal.Close();
  }

  const RequestOwnerEpoch second_epoch("execution-host.epoch.2");
  {
    SqliteOperationJournal replacement(database, cell_id, 1U);
    const auto recovered = replacement.BeginOwnerEpoch(second_epoch, 20, 4U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    EXPECT_EQ(recovered.value().unreconciled_resources, inventory);

    const auto terminal_intent = CommandIntent("request.command.2", second_epoch.value());
    auto terminal_state =
        OperationState("operation:execution-host.epoch.2:1", OperationStateKind::kExecuting, 21);
    terminal_state.participants.front().resources = {ResourceId("robot.arm")};
    ASSERT_TRUE(replacement
                    .RecordOperationAccepted(second_epoch, terminal_intent,
                                             Acceptance(terminal_intent, terminal_state),
                                             terminal_state)
                    .ok());
    terminal_state.state = OperationStateKind::kCompleted;
    terminal_state.participants.front().state = OperationStateKind::kCompleted;
    terminal_state.participants.front().progress = 1.0;
    terminal_state.updated_at_ns = 22;
    ASSERT_TRUE(
        replacement
            .RecordOperationState(second_epoch, terminal_state, nullptr, "subordinate_completed")
            .ok());
    replacement.Close();
  }

  const RequestOwnerEpoch third_epoch("execution-host.epoch.3");
  {
    SqliteOperationJournal replacement(database, cell_id, 1U);
    const auto recovered = replacement.BeginOwnerEpoch(third_epoch, 30, 4U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
    EXPECT_EQ(recovered.value().historical_operations.front().operation_id,
              orphan_state.operation_id);
    EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
    replacement.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 1U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.4"), 40, 4U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest, BoundedEventPagesRetainStableWallTimeAndFenceOwnerEpoch) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.events");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  std::uint64_t wall_generation = 0U;
  SqliteOperationJournal journal(database, cell_id, 8U, [&wall_generation]() {
    ++wall_generation;
    return "2026-08-07T10:00:00.00000000" + std::to_string(wall_generation) + "Z";
  });
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 4U, inventory).ok());
  const auto intent = CommandIntent("request.event.1", epoch.value());
  auto state =
      OperationState("operation:execution-host.epoch.events:1", OperationStateKind::kExecuting, 10);
  ASSERT_TRUE(
      journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
  state.state = OperationStateKind::kCompleted;
  state.participants.front().state = OperationStateKind::kCompleted;
  state.participants.front().progress = 1.0;
  state.updated_at_ns = 11;
  ASSERT_TRUE(journal.RecordOperationState(epoch, state, nullptr, "subordinate_completed").ok());

  const auto first = journal.ReadEvents(epoch, 0U, 1U);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_EQ(first.value().events.size(), 1U);
  EXPECT_TRUE(first.value().has_more);
  EXPECT_EQ(first.value().events.front().record_kind, "operation_accepted");
  EXPECT_EQ(first.value().events.front().occurred_at_wall, "2026-08-07T10:00:00.000000001Z");
  EXPECT_EQ(first.value().next_after_sequence, first.value().events.front().sequence);

  const auto second = journal.ReadEvents(epoch, first.value().next_after_sequence, 1U);
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_EQ(second.value().events.size(), 1U);
  EXPECT_FALSE(second.value().has_more);
  EXPECT_EQ(second.value().events.front().record_kind, "state_observed");
  EXPECT_EQ(second.value().events.front().occurred_at_wall, "2026-08-07T10:00:00.000000002Z");
  EXPECT_FALSE(second.value().events.front().payload_digest.empty());

  const auto replay = journal.ReadEvents(epoch, 0U, 2U);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_EQ(replay.value().events.size(), 2U);
  EXPECT_EQ(replay.value().events.front().occurred_at_wall,
            first.value().events.front().occurred_at_wall);
  EXPECT_EQ(replay.value().events.back().payload_digest,
            second.value().events.front().payload_digest);
  const auto stale = journal.ReadEvents(RequestOwnerEpoch("execution-host.epoch.stale"), 0U, 1U);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "operation_journal_owner_epoch_stale");
}

TEST(SqliteOperationJournalTest, RequestCapacityAndFilesystemIdentityFailClosedBeforeMutation) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch epoch("execution-host.epoch.1");
  SqliteOperationJournal journal(database, cell_id, 4U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 1U, inventory).ok());
  const auto first_intent = CommandIntent("request.command.1", epoch.value());
  const auto first_state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  ASSERT_TRUE(journal
                  .RecordOperationAccepted(epoch, first_intent,
                                           Acceptance(first_intent, first_state), first_state)
                  .ok());
  const auto second_intent = CommandIntent("request.command.2", epoch.value());
  const auto second_state =
      OperationState("operation:execution-host.epoch.1:2", OperationStateKind::kExecuting, 11);
  const auto full = journal.RecordOperationAccepted(
      epoch, second_intent, Acceptance(second_intent, second_state), second_state);
  ASSERT_FALSE(full.ok());
  EXPECT_EQ(full.error().code, "request_ledger_full");
  journal.Close();

  const auto symlink = temporary.path() / "linked-journal.sqlite3";
  std::filesystem::create_symlink(database, symlink);
  ASSERT_TRUE(std::filesystem::is_symlink(symlink));
  EXPECT_THROW(SqliteOperationJournal(symlink, cell_id, 4U), std::runtime_error);
  EXPECT_THROW(SqliteOperationJournal(database, CellId("another-cell"), 4U), std::runtime_error);
}

}  // namespace
