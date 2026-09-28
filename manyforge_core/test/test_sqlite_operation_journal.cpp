#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <manyforge_content_identity/canonical_value.hpp>
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

[[nodiscard]] OperationStateDTO PreparedMotionState(std::string operation_id,
                                                    std::int64_t updated_at_ns) {
  const OperationId id(std::move(operation_id));
  return OperationStateDTO{
      id,
      OperationRevision(1U),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      OperationCoordinationPolicy::kIndependent,
      OperationStateKind::kPrepared,
      {OperationParticipantStateDTO{OperationParticipantId(id.value() + ":participant:1"),
                                    ControllerGoalId(id.value() + ":controller-goal:1"),
                                    ControllerGoalGeneration(1U),
                                    ControllableComponentId("robot.1"),
                                    CommandTargetId("arm"),
                                    CapabilityId("execution.trajectory.execute"),
                                    ControlDomainId("domain.arm"),
                                    ControlDomainRevision("domain-revision"),
                                    ControlBindingId("binding.arm"),
                                    ControlBindingRevision("binding-revision"),
                                    std::nullopt,
                                    std::nullopt,
                                    OperationParticipantPayloadKind::kMotion,
                                    OperationStateKind::kPrepared,
                                    0.0,
                                    {ResourceId("robot.joint1")},
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

TEST(SqliteOperationJournalTest, EmptyRuntimeOwnerEpochCanStartAndCloseWithoutResourceRows) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "operation.sqlite3", CellId("cell.test"));
  const RequestOwnerEpoch epoch("execution-host.empty.1");
  const auto recovery = journal.BeginOwnerEpoch(epoch, 1, 8U, {});
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  EXPECT_TRUE(recovery.value().unreconciled_resources.empty());
  EXPECT_TRUE(journal.CloseOwnerEpoch(epoch, 2).ok());
}

TEST(SqliteOperationJournalTest, ExactLongServiceTimesSurviveOperationStopAndRestart) {
  constexpr std::int64_t boundary = 9'007'199'254'740'991LL;
  for (const auto timestamp :
       std::array<std::int64_t, 5>{boundary, boundary + 1, boundary + 2, 105LL * 86400 * 1000000000,
                                   31LL * 366 * 86400 * 1000000000}) {
    SCOPED_TRACE(timestamp);
    ScopedTemporaryDirectory directory;
    const auto database = directory.path() / "journal.sqlite3";
    const CellId cell("cell.test");
    const RequestOwnerEpoch epoch("owner.original");
    const std::vector<ResourceId> resources{ResourceId("robot.gripper")};
    auto state =
        OperationState("operation.long-service", OperationStateKind::kExecuting, timestamp);
    std::optional<StopJournalTerminalProjectionDTO> terminal;
    {
      SqliteOperationJournal journal(database, cell);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, timestamp, 16U, resources).ok());
      const auto command = CommandIntent("request.long-service", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
      state.updated_at_ns = timestamp + 1;
      state.operation_revision = OperationRevision(2U);
      state.state = OperationStateKind::kCompleted;
      state.participants.front().state = OperationStateKind::kCompleted;
      ASSERT_TRUE(
          journal.RecordOperationState(epoch, state, nullptr, "subordinate_completed").ok());
      const auto events = journal.ReadEvents(epoch, 0U);
      ASSERT_TRUE(events.ok());
      ASSERT_EQ(events.value().events.size(), 2U);
      EXPECT_EQ(events.value().events.back().occurred_at_ns, timestamp + 1);
      const auto payload = manyforge::content_identity::parse_canonical_json(
          events.value().events.back().payload_json);
      const auto* operation = payload.find_object_member("operation_state");
      ASSERT_NE(operation, nullptr);
      const auto* observed = operation->find_object_member("updated_at_ns");
      ASSERT_NE(observed, nullptr);
      EXPECT_EQ(manyforge::content_identity::parse_nonnegative_int64(*observed), timestamp + 1);

      const FullStopIntentDTO intent{
          PublicRequestContextDTO{RequestId("request.stop"), epoch, "journal-test", std::nullopt},
          StopGeneration(0U), RequestAttributionDTO{}};
      auto acceptance =
          StopAcceptanceRecord(epoch, "request.stop", "stop.long-service", 1U, timestamp + 2);
      acceptance.normalized_request_digest = ComputeNormalizedStopIntentDigest(intent).value();
      const EmergencyStopProjectedIntentDTO projected(intent);
      const auto affected = StopAffectedState({}, resources);
      const auto accepted = StopAcceptedProjection(acceptance, projected, affected);
      const auto stop =
          journal.RecordStopAccepted(epoch, acceptance, projected, accepted, affected);
      ASSERT_EQ(stop.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
          << (stop.error ? stop.error->message : "");
      terminal = EmptyTerminalProjection(accepted);
      ASSERT_EQ(journal
                    .RecordStopTerminal(epoch, terminal->stop_status, terminal->certificate,
                                        terminal->fault_latch)
                    .state,
                StopForensicProjectionStateDTO::kRecorded);
    }
    SqliteOperationJournal replacement(database, cell);
    const auto recovered = replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.replacement"),
                                                       timestamp + 4, 16U, resources);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
    EXPECT_EQ(recovered.value().historical_operations.front().updated_at_ns, timestamp + 1);
    ASSERT_EQ(recovered.value().recovered_stops.size(), 1U);
    EXPECT_EQ(recovered.value().recovered_stops.front().terminal_certificate,
              terminal->certificate);
  }
}

TEST(SqliteOperationJournalTest, V9ThroughV11CutoverPreservesHistoricalPayloadBytesAndDigests) {
  for (const int version : {9, 10, 11}) {
    SCOPED_TRACE(version);
    ScopedTemporaryDirectory directory;
    const auto database = directory.path() / "journal.sqlite3";
    const CellId cell("cell.test");
    const RequestOwnerEpoch epoch("owner.original");
    std::string original_payload;
    std::string original_digest;
    {
      SqliteOperationJournal journal(database, cell);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 16U, {}).ok());
      auto state = OperationState("operation.legacy", OperationStateKind::kExecuting, 2);
      const auto command = CommandIntent("request.legacy", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
      // The v9 writer admitted records up to 64 MiB, larger than the shared
      // canonical parser's 16 MiB default. Exercise actual admission, not a
      // directly fabricated SQLite payload, across the format-only migration.
      state.operation_revision = OperationRevision(2U);
      const std::string retained_reason(17U * 1024U * 1024U, 'x');
      ASSERT_TRUE(
          journal.RecordOperationState(epoch, state, nullptr, retained_reason.c_str()).ok());
      const auto page = journal.ReadEvents(epoch, 1U);
      ASSERT_TRUE(page.ok());
      original_payload = page.value().events.front().payload_json;
      original_digest = page.value().events.front().payload_digest;
    }
    // Reconstruct the actual v9 table shape; retain its original payload bytes.
    sqlite3* legacy = nullptr;
    ASSERT_EQ(sqlite3_open(database.c_str(), &legacy), SQLITE_OK);
    const auto legacy_schema =
        std::string(
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_runner_owner_epoch;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_run_id;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_instance_id;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_attempt_id;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_owner_api;"
            "ALTER TABLE operation_participants DROP COLUMN execution_node_instance_id;"
            "ALTER TABLE operation_participants DROP COLUMN execution_node_attempt_id;") +
        (version < 11 ? "ALTER TABLE operation_snapshots DROP COLUMN journal_capture_order;" : "") +
        "PRAGMA user_version=" + std::to_string(version) +
        "; UPDATE journal_metadata SET schema_version=" + std::to_string(version) +
        ",schema_id='manyforge.internal.execution.operation-journal.o1.5.v" +
        std::to_string(version) + "' WHERE singleton=1;";
    const auto downgraded = sqlite3_exec(legacy, legacy_schema.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(legacy);
    ASSERT_EQ(downgraded, SQLITE_OK);
    SqliteOperationJournal upgraded(database, cell);
    const auto page = upgraded.ReadEvents(epoch, 1U);
    ASSERT_TRUE(page.ok()) << page.error().message;
    ASSERT_EQ(page.value().events.size(), 1U);
    EXPECT_EQ(page.value().events.front().payload_json, original_payload);
    EXPECT_EQ(page.value().events.front().payload_digest, original_digest);
  }
}

TEST(SqliteOperationJournalTest, CorruptV9CutoverLeavesIdentityAndEvidenceUntouched) {
  for (const auto corruption : {
           "UPDATE owner_epochs SET started_at_ns=9007199254740992",
           "UPDATE operation_snapshots SET updated_at_ns=9007199254740992",
           "UPDATE journal_records SET occurred_at_ns=9007199254740992",
           "UPDATE journal_records SET payload_digest='corrupt'",
       }) {
    SCOPED_TRACE(corruption);
    ScopedTemporaryDirectory directory;
    const auto database = directory.path() / "journal.sqlite3";
    const CellId cell("cell.test");
    const RequestOwnerEpoch epoch("owner.original");
    {
      SqliteOperationJournal journal(database, cell);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 16U, {}).ok());
      auto state = OperationState("operation.legacy", OperationStateKind::kExecuting, 2);
      const auto command = CommandIntent("request.legacy", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
    }
    sqlite3* legacy = nullptr;
    ASSERT_EQ(sqlite3_open(database.c_str(), &legacy), SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(
            legacy,
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_runner_owner_epoch;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_run_id;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_instance_id;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_attempt_id;"
            "ALTER TABLE cell_admission_claims DROP COLUMN program_node_owner_api;"
            "ALTER TABLE operation_participants DROP COLUMN execution_node_instance_id;"
            "ALTER TABLE operation_participants DROP COLUMN execution_node_attempt_id;"
            "ALTER TABLE operation_snapshots DROP COLUMN journal_capture_order;"
            "PRAGMA user_version=9; UPDATE journal_metadata SET schema_version=9,"
            "schema_id='manyforge.internal.execution.operation-journal.o1.5.v9' WHERE singleton=1;",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(legacy, corruption, nullptr, nullptr, nullptr), SQLITE_OK);
    const auto retained = [&]() {
      sqlite3_stmt* row = nullptr;
      if (sqlite3_prepare_v2(legacy,
                             "SELECT schema_id,schema_version,payload_json,payload_digest "
                             "FROM journal_metadata CROSS JOIN journal_records",
                             -1, &row, nullptr) != SQLITE_OK ||
          sqlite3_step(row) != SQLITE_ROW) {
        sqlite3_finalize(row);
        throw std::runtime_error("test evidence query failed");
      }
      std::vector<std::string> values;
      for (int column = 0; column < 4; ++column) {
        values.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(row, column)));
      }
      sqlite3_finalize(row);
      return values;
    };
    const auto before = retained();
    EXPECT_THROW(SqliteOperationJournal(database, cell), std::runtime_error);
    EXPECT_EQ(retained(), before);
    sqlite3_stmt* version = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(legacy, "PRAGMA user_version", -1, &version, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(version, 0), 9);
    sqlite3_finalize(version);
    sqlite3_close(legacy);
  }
}

TEST(SqliteOperationJournalTest, CorruptV11CaptureOrderCannotCommitTheSchemaUpgrade) {
  for (const auto order : {0, 100}) {
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "corrupt-v11.sqlite3";
    const CellId cell("cell.test");
    const RequestOwnerEpoch epoch("owner.v11");
    {
      SqliteOperationJournal journal(database, cell);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 16U, {}).ok());
      auto state = OperationState("operation.legacy", OperationStateKind::kExecuting, 2);
      state.journal_capture_order = 1U;
      const auto command = CommandIntent("request.legacy", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
    }
    sqlite3* legacy = nullptr;
    ASSERT_EQ(sqlite3_open(database.c_str(), &legacy), SQLITE_OK);
    const auto downgrade =
        "ALTER TABLE cell_admission_claims DROP COLUMN program_node_runner_owner_epoch;"
        "ALTER TABLE cell_admission_claims DROP COLUMN program_node_run_id;"
        "ALTER TABLE cell_admission_claims DROP COLUMN program_node_instance_id;"
        "ALTER TABLE cell_admission_claims DROP COLUMN program_node_attempt_id;"
        "ALTER TABLE cell_admission_claims DROP COLUMN program_node_owner_api;"
        "ALTER TABLE operation_participants DROP COLUMN execution_node_instance_id;"
        "ALTER TABLE operation_participants DROP COLUMN execution_node_attempt_id;"
        "PRAGMA user_version=11; UPDATE journal_metadata SET schema_version=11,"
        "schema_id='manyforge.internal.execution.operation-journal.o1.5.v11';"
        "UPDATE operation_snapshots SET journal_capture_order=" +
        std::to_string(order);
    ASSERT_EQ(sqlite3_exec(legacy, downgrade.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_THROW(SqliteOperationJournal(database, cell), std::runtime_error);
    sqlite3_stmt* version = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(legacy,
                                 "SELECT schema_version,(SELECT COUNT(*) FROM "
                                 "pragma_table_info('operation_participants') "
                                 "WHERE name='execution_node_instance_id') FROM journal_metadata",
                                 -1, &version, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(version, 0), 11);
    EXPECT_EQ(sqlite3_column_int(version, 1), 0);
    sqlite3_finalize(version);
    sqlite3_close(legacy);
  }
}

TEST(SqliteOperationJournalTest,
     PreparedMotionStateRetainsBoundedMetricsAndExactRuntimePolicyProvenance) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "operation.sqlite3", CellId("cell.test"));
  const RequestOwnerEpoch epoch("execution-host.prepared.1");
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, {ResourceId("robot.joint1")}).ok());
  const auto state = PreparedMotionState("operation:prepared:1", 10);

  const auto missing =
      journal.RecordOperationState(epoch, state, nullptr, "subordinate_motion_prepared");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, "operation_journal_record_invalid");

  const std::vector<PreparedMotionDiagnosticsDTO> diagnostics{PreparedMotionDiagnosticsDTO{
      state.participants.front().participant_id,
      Digest('1'),
      MoveId("move.pick"),
      {"joint1"},
      TrajectoryValidationMetricsDTO{
          2.5,
          {0.4},
          {1.2},
          0.15,
          CartesianFidelityMetricsDTO{0.005, 0.05, 0.001, 0.01, 0.0005, 0.002, 0.0015, 0.012, 0.01,
                                      0.005, 2U}},
      PreparedRuntimePolicyRefDTO{
          RuntimePolicyCatalogRecordRefDTO{
              RuntimePolicyId("manyforge.runtime_motion.cell"),
              RuntimePolicyRevision(Digest('2').value()), SchemaId("manyforge.runtime_policy.v1"),
              SchemaRevision(Digest('3').value()), Digest('4'),
              ContentAddressedStoreKey("catalog/runtime-policy/cell/record")}},
      0.25,
      0.15,
      0.15}};
  auto incorrect = diagnostics;
  incorrect.front().admitted_tcp_linear_speed_mps = 0.12;
  EXPECT_FALSE(journal.RecordOperationState(epoch, state, nullptr,
      "subordinate_motion_prepared", incorrect).ok());
  const auto recorded = journal.RecordOperationState(epoch, state, nullptr,
                                                     "subordinate_motion_prepared", diagnostics);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;

  auto observed = state;
  observed.operation_revision = OperationRevision(2U);
  observed.state = OperationStateKind::kExecuting;
  observed.participants.front().state = OperationStateKind::kExecuting;
  observed.participants.front().progress = 0.25;
  observed.updated_at_ns = 11;
  ASSERT_TRUE(
      journal.RecordOperationState(epoch, observed, nullptr, "subordinate_state_observed").ok());

  const auto page = journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 2U);
  EXPECT_EQ(page.value().events.front().record_kind, "operation_prepared");
  const auto& payload = page.value().events.front().payload_json;
  EXPECT_NE(payload.find("\"prepared_motion_diagnostics\":[{"), std::string::npos);
  EXPECT_NE(payload.find("\"move_id\":\"move.pick\""), std::string::npos);
  EXPECT_NE(payload.find("\"trajectory_duration_s\":2.5"), std::string::npos);
  EXPECT_NE(payload.find("\"peak_abs_joint_velocity_rad_s\":[0.4]"), std::string::npos);
  EXPECT_NE(payload.find("\"peak_abs_joint_acceleration_rad_s2\":[1.2]"), std::string::npos);
  EXPECT_NE(payload.find("\"peak_tcp_linear_speed_mps\":0.15"), std::string::npos);
  EXPECT_NE(payload.find("\"cartesian_fidelity_metrics\":{"), std::string::npos);
  EXPECT_NE(payload.find("\"certified_max_translation_error_m\":0.0015"), std::string::npos);
  EXPECT_NE(payload.find("\"accepted_max_blend_deviation_rad\":0.005"), std::string::npos);
  EXPECT_NE(payload.find("\"planning_attempt_count\":2"), std::string::npos);
  EXPECT_NE(payload.find("\"runtime_policy_id\":\"manyforge.runtime_motion.cell\""),
            std::string::npos);
  EXPECT_NE(payload.find("\"content_addressed_store_key\":"
                         "\"catalog/runtime-policy/cell/record\""),
            std::string::npos);
  EXPECT_NE(payload.find("\"requested_tcp_linear_speed_mps\":0.25"), std::string::npos);
  EXPECT_NE(payload.find("\"deployment_maximum_tcp_linear_speed_mps\":0.15"),
            std::string::npos);
  EXPECT_NE(payload.find("\"admitted_tcp_linear_speed_mps\":0.15"), std::string::npos);

  const auto reopened = [&]() {
    journal.Close();
    SqliteOperationJournal recovered(directory.path() / "operation.sqlite3", CellId("cell.test"));
    return recovered.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
  }();
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  ASSERT_EQ(reopened.value().events.size(), 2U);
  EXPECT_EQ(reopened.value().events.front().record_kind, "operation_prepared");
  EXPECT_NE(reopened.value().events.front().payload_json.find("\"prepared_motion_diagnostics\":[{"),
            std::string::npos);
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
    ASSERT_EQ(operational.value().events.size(), 2U);
    EXPECT_EQ(operational.value().events.front().record_kind, "stop_acceptance_projected");
    EXPECT_EQ(operational.value().events.back().record_kind, "stop_terminal_projected");
    EXPECT_EQ(operational.value().events.back().sequence, *first.journal_sequence);
    EXPECT_FALSE(operational.value().has_more);

    const auto forensic = journal.ReadEvents(epoch, 0U, kMaximumOperationJournalEventPage);
    ASSERT_TRUE(forensic.ok()) << forensic.error().message;
    ASSERT_EQ(forensic.value().events.size(), 2U);
    EXPECT_EQ(forensic.value().events.back().record_kind, "stop_terminal_projected");
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
    const auto operational_events =
        journal.ReadOperationalEvents(epoch, first.value() - 1U, kMaximumOperationJournalEventPage);
    ASSERT_TRUE(operational_events.ok()) << operational_events.error().message;
    ASSERT_EQ(operational_events.value().events.size(), 1U);
    EXPECT_EQ(operational_events.value().events.front().sequence, first.value());
    EXPECT_EQ(operational_events.value().events.front().record_kind, "stop_fault_cleared");
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
  auto intent = CommandIntent("request.command.1", first_epoch.value());
  auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  const ProgramNodeExecutionScopeDTO scope{ProgramNodeInstanceId("grasp"), "grasp.attempt.1"};
  intent.participants.front().execution_scope = scope;
  state.participants.front().execution_scope = scope;
  state.journal_capture_order = 1U;
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
  EXPECT_EQ(historical.participants.front().execution_scope, scope);
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

TEST(SqliteOperationJournalTest, LaterSnapshotsCannotChangeCapturedProgramOrigin) {
  ScopedTemporaryDirectory temporary;
  const RequestOwnerEpoch epoch("execution-host.captured");
  SqliteOperationJournal journal(temporary.path() / "captured.sqlite3", CellId("cell.1"), 8U);
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 4U, inventory).ok());
  auto intent = CommandIntent("captured-command", epoch.value());
  auto initial = OperationState("operation.captured", OperationStateKind::kExecuting, 10);
  initial.journal_capture_order = 3U;
  initial.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("grasp"), "grasp.attempt.1"};
  intent.participants.front().execution_scope = initial.participants.front().execution_scope;
  ASSERT_TRUE(
      journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, initial), initial).ok());
  for (const bool remove_origin : {false, true}) {
    for (const auto order : {2U, 4U}) {
      auto changed = initial;
      changed.journal_capture_order = order;
      changed.updated_at_ns = 11;
      if (remove_origin)
        changed.participants.front().execution_scope.reset();
      else
        changed.participants.front().execution_scope->node_instance_id =
            ProgramNodeInstanceId("other");
      EXPECT_FALSE(journal.RecordOperationState(epoch, changed, nullptr, "changed-origin").ok());
    }
  }
}

TEST(SqliteOperationJournalTest,
     CleanlyClosedTerminalHistoryDoesNotFenceResourcesAndOwnerEpochsCannotBeReused) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  auto intent = CommandIntent("request.command.1", first_epoch.value());
  intent.manual_handover = ManualProgramHandoverContextDTO{RequestOwnerEpoch("program.owner"),
                                                           ProgramRunId("held.program"),
                                                           CellAdmissionClaimGeneration(7U), 9U};
  auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(first_epoch, 1, 4U, inventory).ok());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(first_epoch, intent, Acceptance(intent, state), state)
            .ok());
    const auto history = journal.ReadEvents(first_epoch, 0U);
    ASSERT_TRUE(history.ok());
    EXPECT_TRUE(std::any_of(
        history.value().events.begin(), history.value().events.end(), [](const auto& event) {
          return event.payload_json.find(
                     "\"manual_handover\":{\"claim_generation\":7,\"program_run_id\":\"held."
                     "program\",\"program_runner_owner_epoch\":\"program.owner\",\"selection_"
                     "revision\":9}") != std::string::npos;
        }));
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
  SqliteOperationJournal journal(database, CellId("cell.1"), 8U, {},
                                 [&](const std::string_view operation) {
                                   if (operation == "cell_admission_acquire") {
                                     entered_promise.set_value();
                                     release.wait();
                                   }
                                 });
  auto recovery = journal.BeginOwnerEpoch(epoch, 1, 8U, inventory);
  ASSERT_TRUE(recovery.ok());
  const CellAdmissionClaimDTO claim{
      CellAdmissionClaimIntentDTO{CellAdmissionClaimKindDTO::kAuthoringMutation,
                                  CellAdmissionOwnerRoleDTO::kAuthoringOwner,
                                  RequestOwnerEpoch("authoring.owner.1"),
                                  "transaction.1",
                                  RequestId("request.cell-claim.1"),
                                  Digest('e'),
                                  recovery.value().cell_admission_generation_high_water,
                                  {},
                                  std::nullopt,
                                  std::nullopt},
      CellAdmissionClaimGeneration(1U), recovery.value().cell_admission_generation_high_water, 2};
  auto admission = std::async(
      std::launch::async, [&] { return journal.RecordCellAdmissionClaimAcquired(epoch, claim); });
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

TEST(SqliteOperationJournalTest, CleanExecutionCloseRetainsIndependentAuthoringClaims) {
  for (const auto kind :
       {CellAdmissionClaimKindDTO::kAuthoringMutation, CellAdmissionClaimKindDTO::kManagerApply,
        CellAdmissionClaimKindDTO::kManualOperation}) {
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / std::string(kOperationJournalFilename);
    const RequestOwnerEpoch epoch("execution-host.epoch.clean-apply");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    const auto role = kind == CellAdmissionClaimKindDTO::kAuthoringMutation
                          ? CellAdmissionOwnerRoleDTO::kAuthoringOwner
                      : kind == CellAdmissionClaimKindDTO::kManagerApply
                          ? CellAdmissionOwnerRoleDTO::kManager
                          : CellAdmissionOwnerRoleDTO::kManualOperationOwner;
    SqliteOperationJournal journal(database, CellId("cell.1"), 8U);
    const auto initial = journal.BeginOwnerEpoch(epoch, 1, 8U, inventory);
    ASSERT_TRUE(initial.ok());
    const CellAdmissionClaimDTO claim{{kind,
                                       role,
                                       RequestOwnerEpoch("external.owner"),
                                       "transaction.1",
                                       RequestId("request.claim.1"),
                                       Digest('e'),
                                       initial.value().cell_admission_generation_high_water,
                                       {},
                                       std::nullopt,
                                       std::nullopt},
                                      CellAdmissionClaimGeneration(1U),
                                      initial.value().cell_admission_generation_high_water,
                                      2};
    ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(epoch, claim).ok());
    const bool clean = kind != CellAdmissionClaimKindDTO::kManualOperation;
    EXPECT_EQ(journal.CloseOwnerEpoch(epoch, 3).ok(), clean);
    journal.Close();
    SqliteOperationJournal successor(database, CellId("cell.1"), 8U);
    const auto recovered = successor.BeginOwnerEpoch(
        RequestOwnerEpoch("execution-host.epoch.successor"), 4, 8U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    EXPECT_EQ(recovered.value().unreconciled_resources.empty(), clean);
    EXPECT_EQ(recovered.value().replaced_unclosed_owner_epoch.has_value(), !clean);
    ASSERT_EQ(recovered.value().recovered_cell_admission_claims.size(), 1U);
    const auto& retained = recovered.value().recovered_cell_admission_claims.front();
    EXPECT_EQ(retained.claim, claim);
    EXPECT_FALSE(retained.release.has_value());
    EXPECT_FALSE(retained.reconcile_settlement.has_value());
  }
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

TEST(SqliteOperationJournalTest, CleanupFaultAndStopProjectionKeepCausalRevisionAtEqualClocks) {
  for (const bool fault_is_newer : {false, true}) {
    for (const bool newer_arrives_first : {false, true}) {
      SCOPED_TRACE(fault_is_newer);
      SCOPED_TRACE(newer_arrives_first);
      ScopedTemporaryDirectory temporary;
      const auto database = temporary.path() / "operation.sqlite3";
      const RequestOwnerEpoch epoch("owner.fault-order");
      const CellId cell("cell.test");
      const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
      auto initial = OperationState("operation.fault-order", OperationStateKind::kExecuting, 10);
      auto stopped = initial;
      stopped.operation_revision = OperationRevision(fault_is_newer ? 2U : 3U);
      stopped.state = OperationStateKind::kStopped;
      stopped.participants.front().state = OperationStateKind::kStopped;
      stopped.active_stop_id = StopId("stop.fault-order");
      stopped.active_stop_generation = StopGeneration(1U);
      auto fault = initial;
      fault.operation_revision = OperationRevision(fault_is_newer ? 3U : 2U);
      fault.state = OperationStateKind::kInterventionRequired;
      fault.participants.front().state = OperationStateKind::kInterventionRequired;
      fault.participants.front().terminal_error = ErrorDTO{"cleanup_failed", "cleanup failed", {}};
      if (fault_is_newer) {
        fault.active_stop_id = stopped.active_stop_id;
        fault.active_stop_generation = stopped.active_stop_generation;
      } else {
        stopped.participants.front().terminal_error = fault.participants.front().terminal_error;
      }
      const auto& newer = fault_is_newer ? fault : stopped;
      const auto& older = fault_is_newer ? stopped : fault;
      {
        SqliteOperationJournal journal(database, cell, 8U);
        ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
        const auto intent = CommandIntent("request.fault-order", epoch.value());
        ASSERT_TRUE(
            journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, initial), initial)
                .ok());
        // Legal observations can share both revision and an unchanged clock.
        auto completed = initial;
        completed.state = OperationStateKind::kCompleted;
        completed.participants.front().state = OperationStateKind::kCompleted;
        ASSERT_TRUE(
            journal.RecordOperationState(epoch, completed, nullptr, "subordinate_completed").ok());
        ASSERT_TRUE(journal
                        .RecordOperationState(epoch, newer_arrives_first ? newer : older, nullptr,
                                              "first_projection")
                        .ok());
        ASSERT_TRUE(journal
                        .RecordOperationState(epoch, newer_arrives_first ? older : newer, nullptr,
                                              "last_projection")
                        .ok());
        const auto events = journal.ReadEvents(epoch, 0U, 32U);
        ASSERT_TRUE(events.ok());
        EXPECT_LE(events.value().events.size(), 3U);
        journal.Close();
      }
      SqliteOperationJournal replacement(database, cell, 8U);
      const auto recovered =
          replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.fault-order.2"), 10, 8U, inventory);
      ASSERT_TRUE(recovered.ok()) << recovered.error().message;
      ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
      EXPECT_EQ(recovered.value().historical_operations.front(), newer);
      EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
    }
  }
}

TEST(SqliteOperationJournalTest, PrivateCaptureOrderFencesEqualClockAndPublicRevisionOnReopen) {
  for (const bool newest_first : {false, true}) {
    SCOPED_TRACE(newest_first);
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "ordered.sqlite3";
    const RequestOwnerEpoch epoch("owner.capture-order");
    const CellId cell("cell.test");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    auto initial = OperationState("operation.capture-order", OperationStateKind::kExecuting, 10);
    initial.journal_capture_order = 1U;
    auto older = initial;
    older.journal_capture_order = 2U;
    older.active_stop_id = StopId("stop.capture-order");
    older.active_stop_generation = StopGeneration(1U);
    older.state = OperationStateKind::kStopping;
    older.participants.front().state = OperationStateKind::kStopping;
    auto newer = older;
    newer.journal_capture_order = 3U;
    newer.state = OperationStateKind::kStopped;
    newer.participants.front().state = OperationStateKind::kStopped;
    {
      SqliteOperationJournal journal(database, cell, 8U);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
      const auto intent = CommandIntent("request.capture-order", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, initial), initial)
              .ok());
      ASSERT_TRUE(journal
                      .RecordOperationState(epoch, newest_first ? newer : older, nullptr,
                                            "first_projection")
                      .ok());
      ASSERT_TRUE(journal
                      .RecordOperationState(epoch, newest_first ? older : newer, nullptr,
                                            "second_projection")
                      .ok());
      ASSERT_TRUE(journal
                      .RecordOperationState(epoch, newer, nullptr,
                                            newest_first ? "first_projection" : "second_projection")
                      .ok());
      const auto changed_reason =
          journal.RecordOperationState(epoch, newer, nullptr, "different_immutable_reason");
      ASSERT_FALSE(changed_reason.ok());
      EXPECT_EQ(changed_reason.error().code, "operation_journal_corrupt");
      auto conflict = newer;
      conflict.participants.front().terminal_error = ErrorDTO{"invented", "not captured", {}};
      const auto rejected =
          journal.RecordOperationState(epoch, conflict, nullptr, "changed_capture");
      ASSERT_FALSE(rejected.ok());
      EXPECT_EQ(rejected.error().code, "operation_journal_corrupt");
    }
    SqliteOperationJournal replacement(database, cell, 8U);
    const auto recovered =
        replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.capture-order.2"), 20, 8U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
    EXPECT_EQ(recovered.value().historical_operations.front(), newer);
  }
}

TEST(SqliteOperationJournalTest, DelayedLegacyStopCaptureCannotReplaceAnOrderedCurrentState) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "delayed-legacy.sqlite3";
  const RequestOwnerEpoch epoch("owner.delayed-legacy");
  const CellId cell("cell.test");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  auto current = OperationState("operation.delayed-legacy", OperationStateKind::kStopped, 10);
  current.active_stop_id = StopId("stop.delayed-legacy");
  current.active_stop_generation = StopGeneration(1U);
  auto legacy = current;
  legacy.state = OperationStateKind::kStopping;
  legacy.participants.front().state = OperationStateKind::kStopping;
  current.journal_capture_order = 2U;
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    const auto intent = CommandIntent("request.delayed-legacy", epoch.value());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, current), current).ok());
    const FullStopIntentDTO stop{
        {RequestId("request.delayed-legacy.stop"), epoch, "test", {}}, StopGeneration(0U), {}};
    const auto acceptance = StopAcceptanceRecord(epoch, stop.request_context.request_id.value(),
                                                 current.active_stop_id->value(), 1U, 10);
    const auto projected =
        ProjectStopAccepted(journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(stop),
                            StopAffectedState({legacy}, inventory));
    EXPECT_EQ(projected.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
  }
  SqliteOperationJournal replacement(database, cell, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.delayed-legacy.2"), 20, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front(), current);
}

TEST(SqliteOperationJournalTest,
     ProjectionOrderCorruptionCannotSuppressNewCapturesOrSurviveReopen) {
  for (const auto corrupted : {0, 100}) {
    SCOPED_TRACE(corrupted);
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "order-corruption.sqlite3";
    const RequestOwnerEpoch epoch("owner.capture-integrity");
    const CellId cell("cell.test");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    auto state = OperationState("operation.capture-integrity", OperationStateKind::kCompleted, 10);
    state.journal_capture_order = 1U;
    {
      SqliteOperationJournal journal(database, cell, 8U);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
      const auto intent = CommandIntent("request.capture-integrity", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
      sqlite3* corrupt = nullptr;
      ASSERT_EQ(sqlite3_open(database.c_str(), &corrupt), SQLITE_OK);
      const auto update =
          "UPDATE operation_snapshots SET journal_capture_order=" + std::to_string(corrupted);
      ASSERT_EQ(sqlite3_exec(corrupt, update.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
      ASSERT_EQ(sqlite3_close(corrupt), SQLITE_OK);
      state.journal_capture_order = 2U;
      const auto rejected = journal.RecordOperationState(epoch, state, nullptr, "next_capture");
      ASSERT_FALSE(rejected.ok());
      EXPECT_EQ(rejected.error().code, "operation_journal_corrupt");
    }
    SqliteOperationJournal replacement(database, cell, 8U);
    const auto recovered = replacement.BeginOwnerEpoch(
        RequestOwnerEpoch("owner.capture-integrity.2"), 20, 8U, inventory);
    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code, "operation_journal_corrupt");
  }
}

TEST(SqliteOperationJournalTest, AuthoredOriginCorruptionFailsOnWriteAndReopen) {
  for (const auto* update :
       {"UPDATE operation_participants SET "
        "execution_node_instance_id='other',execution_node_attempt_id='other.attempt'",
        "UPDATE operation_participants SET "
        "execution_node_instance_id=NULL,execution_node_attempt_id=NULL",
        "UPDATE operation_snapshots SET program_run_id='other.run'",
        "UPDATE operation_snapshots SET program_binding_id='other.binding'"}) {
    SCOPED_TRACE(update);
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "origin-corruption.sqlite3";
    const RequestOwnerEpoch epoch("owner.origin-integrity");
    const CellId cell("cell.test");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    auto state = OperationState("operation.origin-integrity", OperationStateKind::kCompleted, 10);
    state.journal_capture_order = 1U;
    state.participants.front().execution_scope =
        ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("grasp"), "grasp.attempt.1"};
    {
      SqliteOperationJournal journal(database, cell, 8U);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
      auto intent = CommandIntent("request.origin-integrity", epoch.value());
      intent.participants.front().execution_scope = state.participants.front().execution_scope;
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
      sqlite3* corrupt = nullptr;
      ASSERT_EQ(sqlite3_open(database.c_str(), &corrupt), SQLITE_OK);
      ASSERT_EQ(sqlite3_exec(corrupt, update, nullptr, nullptr, nullptr), SQLITE_OK);
      ASSERT_EQ(sqlite3_close(corrupt), SQLITE_OK);
      state.journal_capture_order = 2U;
      const auto rejected = journal.RecordOperationState(epoch, state, nullptr, "next_capture");
      ASSERT_FALSE(rejected.ok());
      EXPECT_EQ(rejected.error().code, "operation_journal_corrupt");
    }
    SqliteOperationJournal replacement(database, cell, 8U);
    const auto recovered = replacement.BeginOwnerEpoch(
        RequestOwnerEpoch("owner.origin-integrity.2"), 20, 8U, inventory);
    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code, "operation_journal_corrupt");
  }
}

TEST(SqliteOperationJournalTest, ScopedStopProjectionRetainsOriginAndVersionOnReopen) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "scoped-stop.sqlite3";
  const CellId cell("cell.test");
  const RequestOwnerEpoch epoch("owner.scoped-stop");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  auto state = OperationState("operation.scoped-stop", OperationStateKind::kExecuting, 10);
  state.journal_capture_order = 1U;
  state.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("grasp"), "grasp.attempt.1"};
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    auto command = CommandIntent("scoped-command", epoch.value());
    command.participants.front().execution_scope = state.participants.front().execution_scope;
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
    state.state = OperationStateKind::kStopping;
    state.participants.front().state = OperationStateKind::kStopping;
    state.journal_capture_order = 2U;
    state.active_stop_id = StopId("stop.scoped");
    state.active_stop_generation = StopGeneration(1U);
    const FullStopIntentDTO stop{
        {RequestId("scoped-stop"), epoch, "test", {}}, StopGeneration(0U), {}};
    const auto accepted = StopAcceptanceRecord(epoch, "scoped-stop", "stop.scoped", 1U, 20);
    const auto projected =
        ProjectStopAccepted(journal, epoch, accepted, EmergencyStopProjectedIntentDTO(stop),
                            StopAffectedState({state}, inventory));
    ASSERT_EQ(projected.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
    const auto events = journal.ReadEvents(epoch, 0U, 8U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 2U);
    const auto& bytes = events.value().events.back().payload_json;
    EXPECT_NE(bytes.find("manyforge.stop_acceptance_projection.v3"), std::string::npos);
    EXPECT_NE(bytes.find("grasp.attempt.1"), std::string::npos);
  }
  SqliteOperationJournal replacement(database, cell, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.scoped-stop.2"), 30, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front().participants.front().execution_scope,
            state.participants.front().execution_scope);
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

TEST(SqliteOperationJournalTest,
     ResolvedStateConditionedOperationsRotateWithoutOwnerRestartOrCommandCountExhaustion) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.bounded-lifetime");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};

  {
    SqliteOperationJournal journal(database, cell_id, 2U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 2U, inventory).ok());
    for (std::uint64_t cycle = 1U; cycle <= 8U; ++cycle) {
      const auto operation_id =
          "operation:execution-host.epoch.bounded-lifetime:" + std::to_string(cycle);
      auto state = OperationState(operation_id, OperationStateKind::kExecuting,
                                  static_cast<std::int64_t>(cycle * 10U));
      const std::array resident{state.operation_id};
      const auto intent =
          CommandIntent("request.bounded-lifetime." + std::to_string(cycle), epoch.value());
      const auto accepted = journal.RecordOperationAccepted(
          epoch, intent, Acceptance(intent, state), state, resident);
      ASSERT_TRUE(accepted.ok()) << "cycle " << cycle << ": " << accepted.error().message;

      for (std::uint64_t update = 1U; update <= 4U; ++update) {
        const OperationControlRequestDTO request{
            PublicRequestContextDTO{RequestId("request.bounded-lifetime.control." +
                                              std::to_string(cycle) + "." + std::to_string(update)),
                                    epoch, "journal-test", std::nullopt},
            state.operation_id, state.operation_revision, std::nullopt};
        const auto prior_revision = state.operation_revision;
        const bool pause = update % 2U == 1U;
        state.operation_revision = OperationRevision(state.operation_revision.value() + 1U);
        state.state = pause ? OperationStateKind::kPaused : OperationStateKind::kExecuting;
        state.participants.front().state = state.state;
        ++state.updated_at_ns;
        const OperationControlAcceptedDTO control{
            request.request_context.request_id,
            RequestReplayStatusDTO::kAcceptedNew,
            state.operation_id,
            prior_revision,
            pause ? OperationControlKind::kPause : OperationControlKind::kResume,
            state.operation_revision,
            state.updated_at_ns};
        const auto controlled = journal.RecordControlAccepted(epoch, request, control, state);
        ASSERT_TRUE(controlled.ok())
            << "cycle " << cycle << ", update " << update << ": " << controlled.error().message;
      }

      state.state = OperationStateKind::kCompleted;
      state.participants.front().state = OperationStateKind::kCompleted;
      state.participants.front().progress = 1.0;
      ++state.updated_at_ns;
      const auto completed =
          journal.RecordOperationState(epoch, state, nullptr, "subordinate_completed");
      ASSERT_TRUE(completed.ok()) << "cycle " << cycle << ": " << completed.error().message;
    }
    ASSERT_TRUE(journal.CloseOwnerEpoch(epoch, 100).ok());
  }

  SqliteOperationJournal replacement(database, cell_id, 2U);
  const auto recovered = replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.bounded-lifetime.replacement"), 200, 2U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 2U);
  EXPECT_TRUE(std::all_of(recovered.value().historical_operations.begin(),
                          recovered.value().historical_operations.end(),
                          [](const OperationStateDTO& state) {
                            return state.state == OperationStateKind::kCompleted;
                          }));
  EXPECT_TRUE(recovered.value().unreconciled_resources.empty());
}

TEST(SqliteOperationJournalTest, ActiveOperationControlUpdatesConsumeOneDurableAdmissionSlot) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const RequestOwnerEpoch epoch("execution-host.epoch.concurrent-capacity");
  const std::vector<ResourceId> inventory{ResourceId("robot.arm"), ResourceId("robot.gripper")};
  SqliteOperationJournal journal(database, cell_id, 2U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 2U, inventory).ok());

  auto first = OperationState("operation:execution-host.epoch.concurrent-capacity:1",
                              OperationStateKind::kExecuting, 10);
  const std::array first_resident{first.operation_id};
  const auto first_intent = CommandIntent("request.concurrent.first", epoch.value());
  ASSERT_TRUE(journal
                  .RecordOperationAccepted(epoch, first_intent, Acceptance(first_intent, first),
                                           first, first_resident)
                  .ok());

  for (std::uint64_t update = 1U; update <= 4U; ++update) {
    const OperationControlRequestDTO request{
        PublicRequestContextDTO{
            RequestId("request.concurrent.first.control." + std::to_string(update)), epoch,
            "journal-test", std::nullopt},
        first.operation_id, first.operation_revision, std::nullopt};
    const auto prior_revision = first.operation_revision;
    const bool pause = update % 2U == 1U;
    first.operation_revision = OperationRevision(first.operation_revision.value() + 1U);
    first.state = pause ? OperationStateKind::kPaused : OperationStateKind::kExecuting;
    first.participants.front().state = first.state;
    ++first.updated_at_ns;
    const OperationControlAcceptedDTO accepted{
        request.request_context.request_id,
        RequestReplayStatusDTO::kAcceptedNew,
        first.operation_id,
        prior_revision,
        pause ? OperationControlKind::kPause : OperationControlKind::kResume,
        first.operation_revision,
        first.updated_at_ns};
    ASSERT_TRUE(journal.RecordControlAccepted(epoch, request, accepted, first).ok());
  }

  auto second = OperationState("operation:execution-host.epoch.concurrent-capacity:2",
                               OperationStateKind::kExecuting, 20);
  const std::array both_resident{first.operation_id, second.operation_id};
  const auto second_intent = CommandIntent("request.concurrent.second", epoch.value());
  const auto second_accepted = journal.RecordOperationAccepted(
      epoch, second_intent, Acceptance(second_intent, second), second, both_resident);
  ASSERT_TRUE(second_accepted.ok()) << second_accepted.error().message;

  first.state = OperationStateKind::kCompleted;
  first.participants.front().state = OperationStateKind::kCompleted;
  first.participants.front().progress = 1.0;
  ++first.updated_at_ns;
  ASSERT_TRUE(journal.RecordOperationState(epoch, first, nullptr, "subordinate_completed").ok());
  second.state = OperationStateKind::kCompleted;
  second.participants.front().state = OperationStateKind::kCompleted;
  second.participants.front().progress = 1.0;
  ++second.updated_at_ns;
  ASSERT_TRUE(journal.RecordOperationState(epoch, second, nullptr, "subordinate_completed").ok());
  ASSERT_TRUE(journal.CloseOwnerEpoch(epoch, 100).ok());
}

}  // namespace
