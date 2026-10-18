#include "io_rule_outcome_fixture.hpp"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <mutex>
#include <optional>
#include <set>
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
  affected_operations.reserve(affected_state.operations.size());
  for (const auto& operation : affected_state.operations) {
    affected_operations.push_back({operation.operation_id, operation.operation_revision});
  }
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
      acceptance.accepted_at_ns,
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

TEST(SqliteOperationJournalTest, EarlierJournalFormatsAreRefusedAndLeftUntouched) {
  // Earlier Stop/controller-only/common-goal formats are refused, never
  // converted, and their retained bytes stay as they were.
  for (const int version : {9, 10, 11, 12, 13, 14, 15}) {
    SCOPED_TRACE(version);
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
    const auto downgrade = "PRAGMA user_version=" + std::to_string(version) +
                           "; UPDATE journal_metadata SET schema_version=" +
                           std::to_string(version) +
                           ",schema_id='manyforge.internal.execution.operation-journal.o1.5.v" +
                           std::to_string(version) + "' WHERE singleton=1;";
    ASSERT_EQ(sqlite3_exec(legacy, downgrade.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
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
    try {
      SqliteOperationJournal refused(database, cell);
      ADD_FAILURE() << "an earlier journal format was opened";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find("common command identity format"), std::string::npos)
          << error.what();
    }
    EXPECT_EQ(retained(), before);
    sqlite3_stmt* user_version = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(legacy, "PRAGMA user_version", -1, &user_version, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(user_version), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(user_version, 0), version);
    sqlite3_finalize(user_version);
    sqlite3_close(legacy);
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
     PreparedMotionStateRetainsBoundedMetricsAndExactSpeedProvenance) {
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
  EXPECT_EQ(payload.find("\"runtime_policy\""), std::string::npos);
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

TEST(SqliteOperationJournalTest, AcceptedBooleanOutputRecordsExactRoleAndFalseWithoutCoercion) {
  for (const bool value : {false, true}) {
    SCOPED_TRACE(value);
    ScopedTemporaryDirectory temporary;
    const RequestOwnerEpoch epoch("execution-host.io-write");
    SqliteOperationJournal journal(temporary.path() / "io-write.sqlite3", CellId("cell.1"), 8U);
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 4U, inventory).ok());
    auto intent = CommandIntent("io-write", epoch.value());
    auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
    command.command_schema_id = CommandSchemaId("io-write-schema");
    command.command_schema_revision = CommandSchemaRevision("io-write-schema-revision");
    command.command_value = IoSignalWriteCommandValueDTO{ResourceRoleId("valve_enable"), value};
    const auto state = OperationState("operation.io-write", OperationStateKind::kPrepared, 10);
    // A caller budget cannot replace the sealed output mechanism policy.
    ASSERT_TRUE(command.completion_timeout_s.has_value());
    EXPECT_FALSE(
        journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
    command.completion_timeout_s.reset();
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
    const auto events = journal.ReadEvents(epoch, 0U, 1U);
    ASSERT_TRUE(events.ok()) << events.error().message;
    ASSERT_EQ(events.value().events.size(), 1U);
    const auto& payload = events.value().events.front().payload_json;
    EXPECT_NE(payload.find(std::string("\"command_value\":{\"kind\":\"io_signal_write\",") +
                           "\"resource_role_id\":\"valve_enable\",\"value\":" +
                           (value ? "true}" : "false}")),
              std::string::npos)
        << payload;
    EXPECT_EQ(payload.find("\"max_effort\""), std::string::npos);
    EXPECT_EQ(payload.find("\"position\""), std::string::npos);
    EXPECT_EQ(payload.find("\"completion_timeout_s\""), std::string::npos);
  }
}

TEST(SqliteOperationJournalTest, AcceptedMotionRecordsAbsoluteGoalFrameAndRobotQualifier) {
  ScopedTemporaryDirectory temporary;
  const RequestOwnerEpoch epoch("execution-host.goal-frame");
  SqliteOperationJournal journal(temporary.path() / "goal-frame.sqlite3", CellId("cell.1"), 8U);
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 4U, inventory).ok());
  auto intent = CommandIntent("goal-frame-motion", epoch.value());
  Pose3DTO goal;
  goal.position_m = {0.1, 0.2, 0.3};
  goal.orientation.w = 1.0;
  MotionParticipantIntentDTO motion{MoveId("move.framed"), FrameRoleId("base"),
                                    FrameRoleId("tcp"),    TolerancesDTO{0.01, 0.001, 0.01},
                                    SpeedProfileDTO{},     TimeoutsDTO{},
                                    CacheMode::kDefault,   PlanningProfileDTO::kBalanced,
                                    MotionTargetModeDTO::kAbsolute,
                                    std::nullopt,          std::nullopt,
                                    PoseGoalDTO{goal},     std::nullopt,
                                    {},                    {}};
  motion.goal_frame_reference =
      ReferenceDTO(FrameRoleId("tcp"),
                   ReferenceTargetDTO{TargetRoleId("peer_arm"),
                                      ControllableComponentId("robot.2"), CommandTargetId("arm")});
  intent.participants.front().payload = motion;
  const auto state =
      OperationState("operation.goal-frame", OperationStateKind::kExecuting, 10);
  ASSERT_TRUE(
      journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
  const auto events = journal.ReadEvents(epoch, 0U, 1U);
  ASSERT_TRUE(events.ok()) << events.error().message;
  ASSERT_EQ(events.value().events.size(), 1U);
  const auto& payload = events.value().events.front().payload_json;
  EXPECT_NE(payload.find("\"target_mode\":\"absolute\""), std::string::npos);
  EXPECT_NE(payload.find("\"goal_frame_reference\":{\"id\":\"tcp\",\"pose\":null,"
                         "\"target\":{\"command_target_id\":\"arm\","
                         "\"controllable_component_id\":\"robot.2\","
                         "\"target_role_id\":\"peer_arm\"},\"type\":\"frame_role\"}"),
            std::string::npos)
      << payload;
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

    const OperationControlRequestDTO reset_request{
        PublicRequestContextDTO{RequestId("request.reset.1"), first_epoch, "journal-test",
                                std::nullopt},
        state.operation_id, state.operation_revision, std::nullopt};
    const auto prior_revision = state.operation_revision;
    state.operation_revision = OperationRevision(2U);
    state.updated_at_ns = 11;
    const OperationControlAcceptedDTO reset_accepted{reset_request.request_context.request_id,
                                                     RequestReplayStatusDTO::kAcceptedNew,
                                                     state.operation_id,
                                                     prior_revision,
                                                     OperationControlKind::kReset,
                                                     state.operation_revision,
                                                     state.updated_at_ns};
    ASSERT_TRUE(
        journal.RecordControlAccepted(first_epoch, reset_request, reset_accepted, state).ok());

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
        {RuntimeScaleParticipantStatusDTO{
            participant.participant_id,
            participant.controllable_component_id,
            participant.command_target_id,
            participant.capability_id,
            participant.binding_identity.control_domain_id,
            participant.binding_identity.control_domain_revision,
            participant.binding_identity.controller()->control_binding_id,
            participant.binding_identity.controller()->control_binding_revision,
            participant.command_id,
            participant.command_generation,
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
        predecessor_stop_id, StopGeneration(1U), Digest('e')});
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
        event.payload_json.find("\"api_version\":\"manyforge.stop_acceptance_projection.v8\""),
        std::string::npos);
    EXPECT_NE(event.payload_json.find("\"intent_kind\":\"full_stop\""), std::string::npos);
    EXPECT_NE(event.payload_json.find("\"joined_predecessor_stops\":[{"), std::string::npos);
    EXPECT_NE(event.payload_json.find(predecessor_stop_id.value()), std::string::npos);
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
     StopProjectionWaitsForOccupiedProductionJournalCriticalSectionAndRecords) {
  // A production ordinary write (cell admission) owns the journal writer when
  // the Stop projection arrives. The Stop registers as waiting and records as
  // soon as that one write finishes; it is not failed by the occupied writer.
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
                                   if (operation == "stop_projection_writer_wait") {
                                     release_promise.set_value();
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
  // The admission write is released only by the Stop's own registration as a
  // waiter, so the projection below can succeed only by waiting for it.
  const auto projected =
      ProjectStopAccepted(journal, epoch, acceptance, EmergencyStopProjectedIntentDTO(intent),
                          StopAffectedState({}, inventory));
  const auto admission_result = admission.get();
  ASSERT_EQ(projected.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
      << (projected.error.has_value() ? projected.error->message : std::string{});
  EXPECT_TRUE(admission_result.ok()) << admission_result.error().message;
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
    EXPECT_NE(bytes.find("manyforge.stop_acceptance_projection.v8"), std::string::npos);
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

TEST(SqliteOperationJournalTest, JoinedStopReceiptKeepsCreatorCaptureAndRecoveryProjection) {
  // Live incident 2026-10-02: a Program-run Stop joined a resident operation
  // Stop. Its receipt carries the creator's exact operation capture (same
  // capture order) in different record bytes; it must be durable in either
  // projection order without re-anchoring the capture or claiming the Stop's
  // recovery projection.
  for (const bool joined_first : {false, true}) {
    SCOPED_TRACE(joined_first ? "joined receipt projected first" : "creator projected first");
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "joined-stop.sqlite3";
    const CellId cell("cell.test");
    const RequestOwnerEpoch epoch("owner.joined-stop");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    auto state = OperationState("operation.joined-stop", OperationStateKind::kPlanning, 10);
    state.journal_capture_order = 1U;
    state.participants.front().execution_scope =
        ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("move"), "move.attempt.1"};
    {
      SqliteOperationJournal journal(database, cell, 8U);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
      auto command = CommandIntent("joined-command", epoch.value());
      command.participants.front().execution_scope = state.participants.front().execution_scope;
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
      state.operation_revision = OperationRevision(2U);
      state.state = OperationStateKind::kStopping;
      state.participants.front().state = OperationStateKind::kStopping;
      state.journal_capture_order = 2U;
      state.active_stop_id = StopId("stop.joined");
      state.active_stop_generation = StopGeneration(1U);
      const auto affected = StopAffectedState({state}, inventory);

      const StopIntentDTO creator_intent{
          {RequestId("creator-stop"), epoch, "test", {}},
          std::vector<StopOperationTargetDTO>{
              StopOperationTargetDTO{state.operation_id, OperationRevision(1U), StopGeneration(0U)}},
          std::nullopt};
      const StopIntentDTO joined_intent{
          {RequestId("joined-stop"), epoch, "test", {}},
          std::vector<StopProgramRunTargetDTO>{StopProgramRunTargetDTO{ProgramRunId("run.1")}},
          std::nullopt};
      const auto creator = StopAcceptanceRecord(epoch, "creator-stop", "stop.joined", 1U, 20);
      const auto joined =
          StopAcceptanceRecord(epoch, "joined-stop", "stop.joined", 1U, 25,
                               StopResolutionDispositionDTO::kExact,
                               EmergencyStopAcceptancePathDTO::kJoinedResident);
      const auto project_creator = [&] {
        return ProjectStopAccepted(journal, epoch, creator,
                                   EmergencyStopProjectedIntentDTO(creator_intent), affected);
      };
      const auto project_joined = [&] {
        return ProjectStopAccepted(journal, epoch, joined,
                                   EmergencyStopProjectedIntentDTO(joined_intent), affected);
      };
      const auto first = joined_first ? project_joined() : project_creator();
      ASSERT_EQ(first.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
          << (first.error.has_value() ? first.error->message : std::string{});
      const auto second = joined_first ? project_creator() : project_joined();
      ASSERT_EQ(second.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
          << (second.error.has_value() ? second.error->message : std::string{});
      const auto& joined_result = joined_first ? first : second;

      // An exact joined retry is idempotent; a receipt naming another
      // physical scope for the same Stop is a conflict.
      const auto replay = project_joined();
      ASSERT_EQ(replay.projection_state, EmergencyStopProjectionStateDTO::kRecorded);
      EXPECT_EQ(replay.journal_sequence, joined_result.journal_sequence);
      auto foreign = StopAcceptanceRecord(epoch, "foreign-stop", "stop.joined", 1U, 30,
                                          StopResolutionDispositionDTO::kExact,
                                          EmergencyStopAcceptancePathDTO::kJoinedResident);
      foreign.captured_scope_digest = Digest('e');
      const StopIntentDTO foreign_intent{
          {RequestId("foreign-stop"), epoch, "test", {}},
          std::vector<StopProgramRunTargetDTO>{StopProgramRunTargetDTO{ProgramRunId("run.1")}},
          std::nullopt};
      const auto rejected = ProjectStopAccepted(
          journal, epoch, foreign, EmergencyStopProjectedIntentDTO(foreign_intent), affected);
      EXPECT_EQ(rejected.projection_state, EmergencyStopProjectionStateDTO::kFailed);

      const auto events = journal.ReadEvents(epoch, 0U, 8U);
      ASSERT_TRUE(events.ok());
      EXPECT_EQ(std::count_if(events.value().events.begin(), events.value().events.end(),
                              [](const auto& event) {
                                return event.record_kind == "stop_acceptance_projected";
                              }),
                2);
      journal.Close();
    }
    SqliteOperationJournal replacement(database, cell, 8U);
    const auto recovered =
        replacement.BeginOwnerEpoch(RequestOwnerEpoch("owner.joined-stop.2"), 30, 8U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().recovered_stops.size(), 1U);
    EXPECT_EQ(recovered.value().recovered_stops.front().accepted.request_id,
              RequestId("creator-stop"));
    ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
    EXPECT_EQ(recovered.value().historical_operations.front().journal_capture_order, 3U);
    EXPECT_EQ(recovered.value().historical_operations.front().state,
              OperationStateKind::kInterventionRequired);
  }
}

TEST(SqliteOperationJournalTest, StopProjectionRecordsWhileAnEventReaderHoldsItsSnapshot) {
  // A Stop projection never waits for the journal writer lock. An event
  // reader (the history pump) must therefore not hold that lock: a Stop
  // projected while a reader is inside its snapshot still records, and the
  // reader's page stays the snapshot it opened.
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "reader-stop.sqlite3";
  const CellId cell("cell.test");
  const RequestOwnerEpoch epoch("owner.reader-stop");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  SqliteOperationJournal* owner = nullptr;
  std::function<EmergencyStopAcceptanceProjectionResultDTO()> project_during_read;
  std::optional<EmergencyStopAcceptanceProjectionResultDTO> projected_during_read;
  SqliteOperationJournal journal(
      database, cell, 8U, [] { return std::string("2026-10-02T17:58:03.000000001Z"); },
      [&](std::string_view section) {
        if (section == "read_events" && project_during_read && !projected_during_read) {
          projected_during_read = project_during_read();
        }
      });
  owner = &journal;
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
  auto state = OperationState("operation.reader-stop", OperationStateKind::kExecuting, 10);
  state.journal_capture_order = 1U;
  const auto command = CommandIntent("reader-command", epoch.value());
  ASSERT_TRUE(
      journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
  state.operation_revision = OperationRevision(2U);
  state.state = OperationStateKind::kStopping;
  state.participants.front().state = OperationStateKind::kStopping;
  state.journal_capture_order = 2U;
  state.active_stop_id = StopId("stop.reader");
  state.active_stop_generation = StopGeneration(1U);
  const FullStopIntentDTO stop{{RequestId("reader-stop"), epoch, "test", {}}, StopGeneration(0U),
                               {}};
  project_during_read = [&] {
    return ProjectStopAccepted(*owner, epoch,
                               StopAcceptanceRecord(epoch, "reader-stop", "stop.reader", 1U, 20),
                               EmergencyStopProjectedIntentDTO(stop),
                               StopAffectedState({state}, inventory));
  };

  const auto page = journal.ReadEvents(epoch, 0U, 8U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_TRUE(projected_during_read.has_value());
  EXPECT_EQ(projected_during_read->projection_state, EmergencyStopProjectionStateDTO::kRecorded)
      << (projected_during_read->error.has_value() ? projected_during_read->error->message
                                                   : std::string{});
  EXPECT_EQ(page.value().events.size(), 1U);
  project_during_read = nullptr;
  const auto after = journal.ReadEvents(epoch, 0U, 8U);
  ASSERT_TRUE(after.ok()) << after.error().message;
  ASSERT_EQ(after.value().events.size(), 2U);
  EXPECT_EQ(after.value().events.back().record_kind, "stop_acceptance_projected");

  // Closing retires the reader: reads fail closed, never on a stale handle.
  journal.Close();
  EXPECT_FALSE(journal.ReadEvents(epoch, 0U, 8U).ok());
}

TEST(SqliteOperationJournalTest, StopProjectionPrecedesQueuedOrdinaryWritersAfterWriteInProgress) {
  // Strict writer handoff: several ordinary writers are already queued for
  // the journal writer while one ordinary write is in progress when a Stop
  // projection registers. The Stop must acquire the writer immediately after
  // that in-progress write, ahead of every queued writer, however long the
  // in-progress write takes (there is no wall-clock bound to exceed). If the
  // in-progress write fails, it releases the writer and the Stop still
  // records. Ordering is established only through critical-section
  // observations; nothing sleeps.
  constexpr std::size_t kQueuedWriters = 3U;
  constexpr auto kTestGuard = std::chrono::seconds(30);
  for (const bool in_progress_write_fails : {false, true}) {
    SCOPED_TRACE(in_progress_write_fails ? "in-progress write fails" : "in-progress write commits");
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "writer-stop.sqlite3";
    const CellId cell("cell.test");
    const RequestOwnerEpoch epoch("owner.writer-stop");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    std::mutex observed_mutex;
    std::condition_variable observed_changed;
    std::size_t ordinary_writers_waiting = 0U;
    bool stop_waiting = false;
    std::function<void()> during_ordinary_write;
    SqliteOperationJournal journal(
        database, cell, 8U, [] { return std::string("2026-10-02T17:58:03.000000001Z"); },
        [&](std::string_view section) {
          if (section == "ordinary_writer_wait" || section == "stop_projection_writer_wait") {
            {
              std::lock_guard lock(observed_mutex);
              if (section == "ordinary_writer_wait") {
                ++ordinary_writers_waiting;
              } else {
                stop_waiting = true;
              }
            }
            observed_changed.notify_all();
          }
          // Only ever invoked while the observing thread holds the writer.
          if (section == "ordinary_write" && during_ordinary_write) {
            auto run = std::exchange(during_ordinary_write, nullptr);
            run();
          }
        });
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    auto state = OperationState("operation.writer-stop", OperationStateKind::kExecuting, 10);
    state.journal_capture_order = 1U;
    const auto command = CommandIntent("writer-command", epoch.value());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, command, Acceptance(command, state), state).ok());
    std::vector<OperationStateDTO> queued_states;
    for (std::size_t index = 0U; index < kQueuedWriters; ++index) {
      const auto suffix = std::to_string(index);
      auto queued = OperationState("operation.queued." + suffix, OperationStateKind::kExecuting, 10);
      queued.journal_capture_order = 1U;
      const auto queued_command = CommandIntent("queued-command." + suffix, epoch.value());
      ASSERT_TRUE(journal
                      .RecordOperationAccepted(epoch, queued_command,
                                               Acceptance(queued_command, queued), queued)
                      .ok());
      queued.journal_capture_order = 2U;
      queued.updated_at_ns = 12;
      queued_states.push_back(queued);
    }
    const std::size_t accepted_events = 1U + kQueuedWriters;
    auto stopping = state;
    stopping.operation_revision = OperationRevision(2U);
    stopping.state = OperationStateKind::kStopping;
    stopping.participants.front().state = OperationStateKind::kStopping;
    stopping.journal_capture_order = 3U;
    stopping.active_stop_id = StopId("stop.writer");
    stopping.active_stop_generation = StopGeneration(1U);
    const FullStopIntentDTO stop{{RequestId("writer-stop"), epoch, "test", {}},
                                 StopGeneration(0U), {}};

    std::vector<std::future<Result<std::uint64_t>>> queued_writes;
    std::future<EmergencyStopAcceptanceProjectionResultDTO> projected;
    during_ordinary_write = [&] {
      // This thread now holds the writer. Queue several ordinary writers.
      for (const auto& queued : queued_states) {
        queued_writes.push_back(std::async(std::launch::async, [&journal, &epoch, &queued] {
          return journal.RecordOperationState(epoch, queued, nullptr,
                                              "subordinate_state_observed");
        }));
      }
      {
        std::unique_lock lock(observed_mutex);
        ASSERT_TRUE(observed_changed.wait_for(lock, kTestGuard, [&] {
          return ordinary_writers_waiting == kQueuedWriters;
        }));
      }
      // Only now does the Stop projection arrive and register.
      projected = std::async(std::launch::async, [&] {
        return ProjectStopAccepted(journal, epoch,
                                   StopAcceptanceRecord(epoch, "writer-stop", "stop.writer", 1U, 20),
                                   EmergencyStopProjectedIntentDTO(stop),
                                   StopAffectedState({stopping}, inventory));
      });
      {
        std::unique_lock lock(observed_mutex);
        ASSERT_TRUE(observed_changed.wait_for(lock, kTestGuard, [&] { return stop_waiting; }));
      }
      if (in_progress_write_fails) {
        throw std::runtime_error("injected in-progress write failure");
      }
    };
    auto observed = state;
    observed.journal_capture_order = 2U;
    observed.updated_at_ns = 11;
    const auto in_progress =
        journal.RecordOperationState(epoch, observed, nullptr, "subordinate_state_observed");
    EXPECT_EQ(in_progress.ok(), !in_progress_write_fails);
    ASSERT_TRUE(projected.valid());
    const auto result = projected.get();
    ASSERT_EQ(queued_writes.size(), kQueuedWriters);
    for (auto& queued_write : queued_writes) {
      const auto written = queued_write.get();
      EXPECT_TRUE(written.ok()) << written.error().message;
    }
    ASSERT_EQ(result.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
        << (result.error.has_value() ? result.error->message : std::string{});

    // Journal order is writer-acquisition order: the in-progress write (when
    // it committed), then the Stop, then every queued ordinary writer.
    const auto events = journal.ReadEvents(epoch, 0U, 16U);
    ASSERT_TRUE(events.ok()) << events.error().message;
    const std::size_t stop_index = accepted_events + (in_progress_write_fails ? 0U : 1U);
    ASSERT_EQ(events.value().events.size(), stop_index + 1U + kQueuedWriters);
    if (!in_progress_write_fails) {
      EXPECT_EQ(events.value().events[stop_index - 1U].record_kind, "state_observed");
      EXPECT_EQ(events.value().events[stop_index - 1U].operation_id, state.operation_id);
    }
    EXPECT_EQ(events.value().events[stop_index].record_kind, "stop_acceptance_projected");
    ASSERT_TRUE(result.journal_sequence.has_value());
    EXPECT_EQ(events.value().events[stop_index].sequence, *result.journal_sequence);
    for (std::size_t index = stop_index + 1U; index < events.value().events.size(); ++index) {
      EXPECT_EQ(events.value().events[index].record_kind, "state_observed");
      EXPECT_NE(events.value().events[index].operation_id, state.operation_id);
    }
  }
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
        state.operation_revision = OperationRevision(state.operation_revision.value() + 1U);
        state.state = OperationStateKind::kExecuting;
        state.participants.front().state = state.state;
        ++state.updated_at_ns;
        const OperationControlAcceptedDTO control{
            request.request_context.request_id,
            RequestReplayStatusDTO::kAcceptedNew,
            state.operation_id,
            prior_revision,
            OperationControlKind::kReset,
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
    first.operation_revision = OperationRevision(first.operation_revision.value() + 1U);
    first.state = OperationStateKind::kExecuting;
    first.participants.front().state = first.state;
    ++first.updated_at_ns;
    const OperationControlAcceptedDTO accepted{
        request.request_context.request_id,
        RequestReplayStatusDTO::kAcceptedNew,
        first.operation_id,
        prior_revision,
        OperationControlKind::kReset,
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

TEST(SqliteOperationJournalTest, CapturedScopeAndFailureAreImmutableAcrossOwnerReplacement) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "retry.sqlite3";
  const CellId cell("cell.1");
  const RequestOwnerEpoch epoch("execution-host.retry");
  const std::vector<ResourceId> inventory{ResourceId("robot.joint1")};
  auto intent = CommandIntent("retry.context", epoch.value());
  intent.participants.front().required_capability_id = CapabilityId("execution.trajectory.execute");
  intent.participants.front().payload = MotionParticipantIntentDTO{MoveId("move"),
                                                                   FrameRoleId("base"),
                                                                   FrameRoleId("tcp"),
                                                                   TolerancesDTO{},
                                                                   SpeedProfileDTO{},
                                                                   TimeoutsDTO{},
                                                                   CacheMode::kDefault,
                                                                   PlanningProfileDTO::kBalanced,
                                                                   MotionTargetModeDTO::kAbsolute,
                                                                   std::nullopt,
                                                                   std::nullopt,
                                                                   JointGoalDTO{{"joint1"}, {0.25}},
                                                                   std::nullopt,
                                                                   {},
                                                                   {}};
  auto state = PreparedMotionState("operation.retry", 10);
  state.journal_capture_order = 1U;
  state.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("move"), "attempt.1"};
  intent.participants.front().execution_scope = state.participants.front().execution_scope;
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 4U, inventory).ok());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
    for (const bool remove : {false, true}) {
      auto changed = state;
      changed.journal_capture_order = 2U;
      changed.updated_at_ns = 11;
      if (remove)
        changed.participants.front().execution_scope.reset();
      else
        changed.participants.front().execution_scope->node_attempt_id = "attempt.changed";
      EXPECT_FALSE(journal.RecordOperationState(epoch, changed, nullptr, "changed-origin").ok());
    }
    state.journal_capture_order = 2U;
    state.updated_at_ns = 12;
    state.state = OperationStateKind::kFailed;
    state.participants.front().state = OperationStateKind::kFailed;
    state.participants.front().terminal_error = ErrorDTO{"planning_failed", "transient", {}};
    ASSERT_TRUE(journal.RecordOperationState(epoch, state, nullptr, "planning-failure").ok());
    const auto events = journal.ReadEvents(epoch, 0U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 2U);
    EXPECT_NE(events.value().events.front().payload_json.find(
                  "manyforge.operation_journal_record.v8"), std::string::npos);
    EXPECT_EQ(events.value().events.front().payload_json.find("automatic_retry_context"),
              std::string::npos);
    EXPECT_NE(events.value().events.back().payload_json.find("planning_failed"),
              std::string::npos);
    journal.Close();
  }
  SqliteOperationJournal replacement(database, cell, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("replacement"), 20, 4U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  const auto& historical = recovered.value().historical_operations.front();
  EXPECT_EQ(historical.participants.front().execution_scope,
            state.participants.front().execution_scope);
  EXPECT_EQ(historical.participants.front().terminal_error,
            state.participants.front().terminal_error);
}

TEST(SqliteOperationJournalTest, ExactParticipantFactsSurviveCaptureAndReopenWithoutColumns) {
  for (const auto effect : {std::optional<ParticipantEffectKnowledge>{},
                            std::optional(ParticipantEffectKnowledge::kNoEffect),
                            std::optional(ParticipantEffectKnowledge::kEffectSettled),
                            std::optional(ParticipantEffectKnowledge::kEffectUncertain)}) {
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "participant-fact.sqlite3";
    const RequestOwnerEpoch epoch("owner.fact");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    auto initial = OperationState("operation.fact", OperationStateKind::kExecuting, 10);
    initial.journal_capture_order = 1U;
    auto failed = initial;
    failed.state = OperationStateKind::kFailed;
    failed.participants.front().state = OperationStateKind::kFailed;
    failed.journal_capture_order = 2U;
    const ErrorDTO cause{"command_failed", "exact failure", {{"phase", "delivery"}}};
    failed.participants.front().terminal_error = cause;
    if (effect)
      failed.participants.front().terminal_fact =
          ParticipantExecutionFact{ParticipantExecutionFactKind::kLocalFailure, *effect, cause};
    {
      SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
      const auto intent = CommandIntent("request.fact", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, initial), initial)
              .ok());
      ASSERT_TRUE(journal.RecordOperationState(epoch, failed, nullptr, "exact_failure").ok());
      const auto events = journal.ReadEvents(epoch, 0U, 8U);
      ASSERT_TRUE(events.ok());
      const auto& bytes = events.value().events.back().payload_json;
      EXPECT_EQ(bytes.find("terminal_fact") != std::string::npos, effect.has_value());
      if (effect) {
        EXPECT_NE(bytes.find("manyforge.operation_journal_record.v8"), std::string::npos);
      }
      if (effect) {
        auto mismatch = failed;
        mismatch.journal_capture_order = 3U;
        mismatch.participants.front().terminal_error =
            ErrorDTO{"resource_slot_state_invalid", "selected resource transition failed", {}};
        ASSERT_TRUE(journal.RecordOperationState(epoch, mismatch, nullptr, "mismatched_fact").ok());
        failed = mismatch;
        mismatch.journal_capture_order = 4U;
        mismatch.participants.front().state = OperationStateKind::kExecuting;
        EXPECT_FALSE(
            journal.RecordOperationState(epoch, mismatch, nullptr, "nonterminal_fact").ok());
      }
    }
    SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
    const auto recovered =
        reopened.BeginOwnerEpoch(RequestOwnerEpoch("owner.fact.2"), 20, 8U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
    EXPECT_EQ(recovered.value().historical_operations.front(), failed);
  }
}

TEST(SqliteOperationJournalTest,
     PreCutCommandIdentityIsRejectedAfterIntegrityCheckedRead) {
  for (const bool legacy_members : {false, true}) {
    SCOPED_TRACE(legacy_members);
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "empty-fact-version.sqlite3";
    const RequestOwnerEpoch epoch("owner.empty-fact");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    auto state = OperationState("operation.empty-fact", OperationStateKind::kCompleted, 10);
    state.journal_capture_order = 1U;
    {
      SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
      const auto intent = CommandIntent("request.empty-fact", epoch.value());
      ASSERT_TRUE(
          journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
    }
    sqlite3* connection = nullptr;
    ASSERT_EQ(sqlite3_open(database.c_str(), &connection), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  connection,
                  "SELECT sequence,payload_json FROM journal_records ORDER BY sequence DESC LIMIT 1",
                  -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto sequence = sqlite3_column_int64(statement, 0);
    std::string bytes(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
    ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
    if (legacy_members) {
      for (const auto& [current, legacy] :
           std::vector<std::pair<std::string, std::string>>{
               {"\"command_id\"", "\"controller_goal_id\""},
               {"\"command_generation\"", "\"controller_goal_generation\""}}) {
        const auto at = bytes.find(current);
        ASSERT_NE(at, std::string::npos);
        bytes.replace(at, current.size(), legacy);
      }
    } else {
      const std::string version = "manyforge.operation_journal_record.v8";
      const auto at = bytes.find(version);
      ASSERT_NE(at, std::string::npos);
      bytes.replace(at, version.size(), "manyforge.operation_journal_record.v5");
    }
    const auto digest = manyforge::content_identity::sha256_digest(bytes);
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection, "UPDATE journal_records SET payload_json=?,payload_digest=? WHERE sequence=?",
            -1, &statement, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(statement, 1, bytes.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(statement, 2, digest.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int64(statement, 3, sequence), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(connection), SQLITE_OK);
    SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
    const auto recovered =
        reopened.BeginOwnerEpoch(RequestOwnerEpoch("owner.empty-fact.2"), 20, 8U, inventory);
    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code, "operation_journal_corrupt");
  }
}

TEST(SqliteOperationJournalTest, LogicalPreemptionPersistsWithoutClaimingPhysicalCompletion) {
  for (const auto kind : {OperationPreemptionKind::kPreempted,
                          OperationPreemptionKind::kSupersededUnsent}) {
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "preemption.sqlite3";
    const RequestOwnerEpoch epoch("owner.preemption");
    const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
    auto initial = OperationState("operation.preemption", OperationStateKind::kExecuting, 10);
    initial.journal_capture_order = 1U;
    initial.participants.front().execution_scope =
        ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("arm.node"), "attempt.1"};
    auto preempted = initial;
    preempted.journal_capture_order = 2U;
    preempted.preemption = OperationPreemptionDTO{
        kind, {OperationId("operation.winner"), ProgramNodeInstanceId("winner.node"), "attempt.2"}};
    {
      SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
      ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
      auto intent = CommandIntent("request.preemption", epoch.value());
      intent.participants.front().execution_scope = initial.participants.front().execution_scope;
      ASSERT_TRUE(journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, initial), initial)
                      .ok());
      ASSERT_TRUE(journal.RecordOperationState(epoch, preempted, nullptr, "motion_preempted").ok());
      const auto events = journal.ReadEvents(epoch, 0U, 8U);
      ASSERT_TRUE(events.ok());
      EXPECT_NE(events.value().events.back().payload_json.find(
                    "manyforge.operation_journal_record.v8"), std::string::npos);
      EXPECT_EQ(preempted.participants.front().state, OperationStateKind::kExecuting);
      EXPECT_FALSE(preempted.participants.front().terminal_fact);
      auto erased = preempted;
      erased.journal_capture_order = 3U;
      erased.preemption.reset();
      EXPECT_FALSE(journal.RecordOperationState(epoch, erased, nullptr, "lost_preemption").ok());
      auto changed = preempted;
      changed.journal_capture_order = 3U;
      changed.preemption->by.node_attempt_id = "changed";
      EXPECT_FALSE(journal.RecordOperationState(epoch, changed, nullptr, "changed_preemption").ok());
    }
    SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
    const auto recovered =
        reopened.BeginOwnerEpoch(RequestOwnerEpoch("owner.preemption.2"), 20, 8U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
    const auto& historical = recovered.value().historical_operations.front();
    EXPECT_EQ(historical.preemption, preempted.preemption);
    EXPECT_EQ(historical.state, OperationStateKind::kInterventionRequired);
    EXPECT_NE(historical.participants.front().state, OperationStateKind::kCompleted);
  }
}

IoRuleStateChangeDTO GlobalRuleChange(RequestOwnerEpoch owner, IoGlobalRuleState state,
                                      TimePoint at = 1) {
  return {{std::move(owner), UINT64_MAX, ActiveBundlePublicationGeneration(3U),
           RetainedBundleId("retained.global"), GraphRevision("graph.global")},
          {InterruptPolicyId("door.guard"), state, std::nullopt},
          at};
}

TEST(SqliteOperationJournalTest, InterruptRuleStateUsesExistingOrderedOperationalStreamAndReopens) {
  ScopedTemporaryDirectory directory;
  const auto database = directory.path() / "rule-state.sqlite3";
  const RequestOwnerEpoch owner("global.owner");
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 8U, {}).ok());
    auto change = GlobalRuleChange(owner, IoGlobalRuleState::kTripped);
    const auto first = journal.RecordInterruptRuleStateChanged(change);
    ASSERT_TRUE(first.ok());
    first_sequence = first.value();
    change.rule.state = IoGlobalRuleState::kLatched;
    change.rule.triggering_acquisition =
        IoRuleAcquisitionKeyDTO{InterruptSourceId("door.source"),
                                IoSourceIncarnation("door.epoch"),
                                UINT64_MAX,
                                {owner, UINT64_MAX, InterruptSourceId("door.source"), UINT64_MAX}};
    change.occurred_at_ns = 2;
    const auto last = journal.RecordInterruptRuleStateChanged(change);
    ASSERT_TRUE(last.ok());
    last_sequence = last.value();
    EXPECT_GT(last_sequence, first_sequence);
    const auto events = journal.ReadOperationalEvents(owner, 0U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 2U);
    const auto& event = events.value().events.back();
    EXPECT_EQ(event.record_kind, "interrupt_rule_state_changed");
    EXPECT_FALSE(event.operation_id);
    EXPECT_FALSE(event.request_id);
    EXPECT_FALSE(event.effect_pending);
    EXPECT_EQ(event.occurred_at_ns, 2);
    const auto payload = manyforge::content_identity::parse_canonical_json(event.payload_json);
    ASSERT_EQ(payload.as_object().size(), 8U);
    EXPECT_EQ(payload.find_object_member("catalog_generation")->as_string(),
              std::to_string(UINT64_MAX));
    EXPECT_EQ(payload.find_object_member("active_publication_generation")->as_number(), 3.0);
    EXPECT_EQ(payload.find_object_member("state")->as_string(), "latched");
    EXPECT_EQ(payload.find_object_member("triggering_acquisition")
                  ->find_object_member("source_sequence")
                  ->as_string(),
              std::to_string(UINT64_MAX));
    EXPECT_TRUE(manyforge::content_identity::parse_canonical_json(
                    events.value().events.front().payload_json)
                    .find_object_member("triggering_acquisition")
                    ->is_null());
  }
  SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
  const auto replay = reopened.ReadOperationalEvents(owner, first_sequence);
  ASSERT_TRUE(replay.ok());
  ASSERT_EQ(replay.value().events.size(), 1U);
  EXPECT_EQ(replay.value().events.front().sequence, last_sequence);
  EXPECT_EQ(replay.value().next_after_sequence, last_sequence);
}

TEST(SqliteOperationJournalTest, InterruptRuleStateRefusesInvalidEvidenceWithoutConsumingSequence) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "rule-invalid.sqlite3", CellId("cell.test"),
                                 8U);
  const RequestOwnerEpoch owner("global.owner");
  ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 8U, {}).ok());
  auto change = GlobalRuleChange(owner, IoGlobalRuleState::kClear);
  const auto first = journal.RecordInterruptRuleStateChanged(change);
  ASSERT_TRUE(first.ok());
  change.rule.triggering_acquisition =
      IoRuleAcquisitionKeyDTO{InterruptSourceId("door.source"),
                              IoSourceIncarnation("door.epoch"),
                              1U,
                              {owner, UINT64_MAX, InterruptSourceId("door.source"), 2U}};
  EXPECT_FALSE(journal.RecordInterruptRuleStateChanged(change).ok());  // No fabricated clear cause.
  change.rule.state = static_cast<IoGlobalRuleState>(255);
  EXPECT_FALSE(journal.RecordInterruptRuleStateChanged(change).ok());
  change.rule.state = IoGlobalRuleState::kTripped;
  change.rule.triggering_acquisition->source_cursor.owner_epoch = RequestOwnerEpoch("foreign");
  EXPECT_FALSE(journal.RecordInterruptRuleStateChanged(change).ok());
  change.rule.triggering_acquisition.reset();
  change.context.active_publication_generation = ActiveBundlePublicationGeneration(0U);
  EXPECT_FALSE(journal.RecordInterruptRuleStateChanged(change).ok());
  change.context.active_publication_generation = ActiveBundlePublicationGeneration(3U);
  change.occurred_at_ns = -1;
  EXPECT_FALSE(journal.RecordInterruptRuleStateChanged(change).ok());
  change.occurred_at_ns = 2;
  const auto last = journal.RecordInterruptRuleStateChanged(change);
  ASSERT_TRUE(last.ok());
  EXPECT_EQ(last.value(), first.value() + 1U);
  const auto all = journal.ReadOperationalEvents(owner, 0U);
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all.value().events.size(), 2U);
}

TEST(SqliteOperationJournalTest, InterruptRuleStateUsesBoundedHistoryAndRejectsRetiredOwner) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "rule-history.sqlite3", CellId("cell.test"),
                                 2U);
  const RequestOwnerEpoch owner("global.owner");
  ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 2U, {}).ok());
  std::uint64_t last{};
  for (std::uint64_t index = 0U; index < 5U; ++index) {
    const auto recorded = journal.RecordInterruptRuleStateChanged(GlobalRuleChange(
        owner, index % 2U ? IoGlobalRuleState::kClear : IoGlobalRuleState::kTripped,
        static_cast<TimePoint>(index)));
    ASSERT_TRUE(recorded.ok());
    last = recorded.value();
  }
  const auto retained = journal.ReadOperationalEvents(owner, 0U);
  ASSERT_TRUE(retained.ok());
  ASSERT_EQ(retained.value().events.size(), 2U);
  EXPECT_EQ(retained.value().events.front().sequence, last - 1U);
  EXPECT_EQ(retained.value().events.back().sequence, last);
  ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("global.replacement"), 6, 2U, {}).ok());
  EXPECT_FALSE(
      journal.RecordInterruptRuleStateChanged(GlobalRuleChange(owner, IoGlobalRuleState::kClear, 7))
          .ok());
  EXPECT_FALSE(journal.ReadOperationalEvents(owner, 0U).ok());
}

TEST(SqliteOperationJournalTest, InterruptRuleStateBaselineRequiresCanonicalActualContextRows) {
  const auto change =
      GlobalRuleChange(RequestOwnerEpoch("global.owner"), IoGlobalRuleState::kClear);
  IoRuleStateBaselineDTO baseline{change.context, 0U, {change.rule}};
  EXPECT_FALSE(io_rule_state_baseline_field_error(baseline));
  baseline.states.push_back(change.rule);
  EXPECT_EQ(io_rule_state_baseline_field_error(baseline), "io_rule_state_rows_not_canonical");
  baseline.states.back().rule_id = InterruptPolicyId("aaa.first");
  EXPECT_EQ(io_rule_state_baseline_field_error(baseline), "io_rule_state_rows_not_canonical");
  baseline.states.clear();
  EXPECT_FALSE(io_rule_state_baseline_field_error(baseline));  // Actual qualified empty catalog.
  baseline.through_sequence = UINT64_MAX;
  EXPECT_EQ(io_rule_state_baseline_field_error(baseline), "io_rule_state_sequence_invalid");
}

}  // namespace

namespace {
TEST(SqliteOperationJournalTest, PreemptedWithoutStopRetainsFactAndSelectedCauseAcrossReopen) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "preempted-fact.sqlite3";
  const RequestOwnerEpoch epoch("owner.preempted");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  auto state = OperationState("operation.preempted", OperationStateKind::kInterventionRequired, 10);
  state.journal_capture_order = 1U;
  const ErrorDTO selected{
      "success", "unexpected subordinate terminal", {{"reason", "subordinate_terminal_error"}}};
  const ParticipantExecutionFact reported{ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                                          ParticipantEffectKnowledge::kEffectSettled, std::nullopt};
  state.participants.front().terminal_error = selected;
  state.participants.front().terminal_fact = reported;
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    const auto intent = CommandIntent("request.preempted", epoch.value());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
    const auto events = journal.ReadEvents(epoch, 0U, 8U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 1U);
    const auto payload = manyforge::content_identity::parse_canonical_json(
        events.value().events.front().payload_json);
    const auto& participant = payload.find_object_member("operation_state")
                                  ->find_object_member("participants")
                                  ->as_array()
                                  .front();
    const auto* fact = participant.find_object_member("terminal_fact");
    ASSERT_NE(fact, nullptr);
    EXPECT_EQ(fact->find_object_member("kind")->as_string(), "preempted_by_addressed_stop");
    EXPECT_EQ(fact->find_object_member("effect")->as_string(), "effect_settled");
    EXPECT_TRUE(fact->find_object_member("error")->is_null());
    EXPECT_EQ(
        participant.find_object_member("terminal_error")->find_object_member("code")->as_string(),
        selected.code);
  }
  SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
  const auto recovered =
      reopened.BeginOwnerEpoch(RequestOwnerEpoch("owner.preempted.2"), 20, 8U, inventory);
  ASSERT_TRUE(recovered.ok());
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front(), state);
}

TEST(SqliteOperationJournalTest,
     HostReplacementRetainsEndedSiblingFactWithSelectedReplacementCause) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "sibling-fact.sqlite3";
  const RequestOwnerEpoch epoch("owner.siblings");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper"),
                                          ResourceId("robot2.gripper")};
  auto state = OperationState("operation.siblings", OperationStateKind::kExecuting, 10);
  auto sibling = state.participants.front();
  sibling.participant_id = OperationParticipantId("participant.2");
  sibling.command_id = ControllerGoalId("goal.2");
  sibling.controllable_component_id = ControllableComponentId("robot.2");
  sibling.resources = {inventory.back()};
  state.participants.push_back(sibling);
  const ParticipantExecutionFact reported{ParticipantExecutionFactKind::kComputedCompletion,
                                          ParticipantEffectKnowledge::kEffectSettled, std::nullopt};
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    auto intent = CommandIntent("request.siblings", epoch.value());
    intent.participants.push_back(intent.participants.front());
    intent.participants.back().component_role_id = ComponentRoleId("robot2");
    auto accepted = Acceptance(intent, state);
    accepted.participant_assignments.push_back({1U, sibling.participant_id});
    ASSERT_TRUE(journal.RecordOperationAccepted(epoch, intent, accepted, state).ok());
    state.participants.front().state = OperationStateKind::kCompleted;
    state.participants.front().terminal_fact = reported;
    state.updated_at_ns = 11;
    ASSERT_TRUE(journal.RecordOperationState(epoch, state, nullptr, "sibling_completed").ok());
  }
  SqliteOperationJournal replacement(database, CellId("cell.test"), 8U);
  const RequestOwnerEpoch next("owner.siblings.2");
  const auto recovered = replacement.BeginOwnerEpoch(next, 20, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  const auto& historical = recovered.value().historical_operations.front();
  EXPECT_EQ(historical.state, OperationStateKind::kInterventionRequired);
  EXPECT_EQ(historical.participants.front().terminal_fact, std::optional(reported));
  ASSERT_TRUE(historical.participants.front().terminal_error);
  EXPECT_EQ(historical.participants.front().terminal_error->code, "execution_host_replaced");
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
  const auto events = replacement.ReadEvents(next, 0U, 8U);
  ASSERT_TRUE(events.ok());
  const auto& bytes = events.value().events.back().payload_json;
  EXPECT_NE(bytes.find("computed_completion"), std::string::npos);
  EXPECT_NE(bytes.find("execution_host_replaced"), std::string::npos);
}

TEST(SqliteOperationJournalTest, ProviderIdentityAndCorrelatedOutputEvidenceSurviveReopen) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "provider-output.sqlite3";
  const RequestOwnerEpoch epoch("owner.output");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  auto state = OperationState("operation.output", OperationStateKind::kFailed, 10);
  auto& participant = state.participants.front();
  participant.binding_identity = ParticipantBindingIdentityDTO{
      ControlDomainId("domain.output"), ControlDomainRevision("provider-domain-revision"),
      CapabilityProviderBindingIdentityDTO{CapabilityProviderBindingId("provider.output"),
                                           CapabilityProviderBindingRevision("provider-revision")}};
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    auto intent = CommandIntent("request.output", epoch.value());
    auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
    command.command_schema_id = CommandSchemaId("io-write-schema");
    command.command_schema_revision = CommandSchemaRevision("io-write-schema-revision");
    command.command_value = IoSignalWriteCommandValueDTO{ResourceRoleId("valve_enable"), false};
    command.completion_timeout_s.reset();
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
    for (const auto evidence :
         {IoOutputEvidence::kNotSubmitted, IoOutputEvidence::kSubmitted,
          IoOutputEvidence::kSourceAccepted, IoOutputEvidence::kSourceRejected,
          IoOutputEvidence::kAcceptanceUnknown}) {
      participant.output_evidence = evidence;
      ++state.updated_at_ns;
      ASSERT_TRUE(journal.RecordOperationState(epoch, state, nullptr, "output_observed").ok());
      // State records intentionally coalesce. Inspect each actual retained
      // latest event before the next observation supersedes it.
      const auto current = journal.ReadEvents(epoch, 0U, 8U);
      ASSERT_TRUE(current.ok()) << current.error().message;
      ASSERT_EQ(current.value().events.size(), 2U);
      const auto payload = manyforge::content_identity::parse_canonical_json(
          current.value().events.back().payload_json);
      const auto& row = payload.find_object_member("operation_state")
                            ->find_object_member("participants")->as_array().front();
      EXPECT_EQ(row.find_object_member("output_evidence")->as_string(),
                std::string(to_string(evidence)));
    }
    const auto events = journal.ReadEvents(epoch, 0U, 8U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 2U);
    for (const auto& event : events.value().events) {
      const auto payload = manyforge::content_identity::parse_canonical_json(event.payload_json);
      EXPECT_EQ(payload.find_object_member("api_version")->as_string(),
                "manyforge.operation_journal_record.v8");
      const auto& row = payload.find_object_member("operation_state")
                            ->find_object_member("participants")
                            ->as_array()
                            .front();
      EXPECT_EQ(row.find_object_member("command_id")->as_string(), participant.command_id.value());
      EXPECT_EQ(row.find_object_member("command_generation")->as_number(),
                static_cast<double>(participant.command_generation.value()));
      EXPECT_EQ(row.find_object_member("controller_goal_id"), nullptr);
      EXPECT_EQ(row.find_object_member("controller_goal_generation"), nullptr);
      EXPECT_TRUE(row.find_object_member("control_binding_id")->is_null());
      EXPECT_TRUE(row.find_object_member("control_binding_revision")->is_null());
      EXPECT_EQ(row.find_object_member("capability_provider_binding_id")->as_string(),
                "provider.output");
      EXPECT_EQ(row.find_object_member("control_domain_id")->as_string(), "domain.output");
    }
    EXPECT_NE(events.value().events.back().payload_json.find("acceptance_unknown"),
              std::string::npos);
    auto invalid = state;
    invalid.participants.front().output_evidence = static_cast<IoOutputEvidence>(999);
    EXPECT_FALSE(journal.RecordOperationState(epoch, invalid, nullptr, "invalid_evidence").ok());
  }
  SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
  const auto recovered =
      reopened.BeginOwnerEpoch(RequestOwnerEpoch("owner.output.2"), 30, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front(), state);
}

TEST(SqliteOperationJournalTest,
     ExplicitUncertainOutputAcknowledgementIsDurableAndAbsentByDefault) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "explicit-output-ack.sqlite3";
  const CellId cell("cell.ack");
  const RequestOwnerEpoch epoch("owner.ack");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  auto state = OperationState("operation.ack", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 8U, inventory).ok());
    const auto intent = CommandIntent("request.ack.initial", epoch.value());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(epoch, intent, Acceptance(intent, state), state).ok());
    // Journal codec fixture: the actual Kernel owns whether this explicit set
    // qualifies retirement. Persist exactly its accepted typed control fact.
    for (const bool explicit_ack : {false, true}) {
      OperationControlRequestDTO request{
          PublicRequestContextDTO{RequestId(explicit_ack ? "reset.explicit" : "reset.default"),
                                  epoch, "operator.fixture", std::nullopt},
          state.operation_id, state.operation_revision, std::nullopt};
      if (explicit_ack)
        request.acknowledged_uncertain_outputs = std::vector<UncertainOutputAcknowledgementDTO>{
            {state.operation_id, state.operation_revision}};
      const auto before = state.operation_revision;
      state.operation_revision = OperationRevision(before.value() + 1U);
      ++state.updated_at_ns;
      const OperationControlAcceptedDTO accepted{request.request_context.request_id,
                                                 RequestReplayStatusDTO::kAcceptedNew,
                                                 state.operation_id,
                                                 before,
                                                 OperationControlKind::kReset,
                                                 state.operation_revision,
                                                 state.updated_at_ns};
      ASSERT_TRUE(journal.RecordControlAccepted(epoch, request, accepted, state).ok());
      const auto events = journal.ReadEvents(epoch, 0U, 8U);
      ASSERT_TRUE(events.ok());
      const auto payload = manyforge::content_identity::parse_canonical_json(
          events.value().events.back().payload_json);
      const auto* encoded = payload.find_object_member("intent")->find_object_member(
          "acknowledged_uncertain_outputs");
      ASSERT_EQ(encoded != nullptr, explicit_ack);
      if (explicit_ack) {
        ASSERT_TRUE(encoded->is_array());
        ASSERT_EQ(encoded->as_array().size(), 1U);
        const auto& row = encoded->as_array().front();
        EXPECT_EQ(row.as_object().size(), 2U);
        EXPECT_EQ(row.find_object_member("operation_id")->as_string(), state.operation_id.value());
        EXPECT_EQ(row.find_object_member("operation_revision")->as_number(),
                  static_cast<double>(before.value()));
      }
    }
  }
  SqliteOperationJournal reopened(database, cell, 8U);
  const auto events = reopened.ReadEvents(epoch, 0U, 8U);
  ASSERT_TRUE(events.ok());
  const auto payload =
      manyforge::content_identity::parse_canonical_json(events.value().events.back().payload_json);
  ASSERT_NE(
      payload.find_object_member("intent")->find_object_member("acknowledged_uncertain_outputs"),
      nullptr);
}

}  // namespace

namespace {
TEST(SqliteOperationJournalTest, ControlAckKeepsHistoricalRuleOriginThroughJournalReopen) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "historical-rule-ack.sqlite3";
  const CellId cell("cell.rule.ack");
  const RequestOwnerEpoch owner("owner.rule.ack");
  const std::vector<ResourceId> resources{ResourceId("robot.gripper")};
  auto state = OperationState("operation.rule.ack", OperationStateKind::kCompleted, 10);
  state.participants.front().io_rule_outcomes = {rule_codec_test::Outcome(state, owner)};
  const auto original = state.participants.front().io_rule_outcomes;
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 1, 8U, resources).ok());
    const auto intent = CommandIntent("request.rule.initial", owner.value());
    ASSERT_TRUE(journal.RecordOperationAccepted(owner, intent, Acceptance(intent, state), state).ok());
    const OperationControlRequestDTO request{
        PublicRequestContextDTO{RequestId("request.rule.ack"), owner, "operator.fixture", std::nullopt},
        state.operation_id, state.operation_revision, std::nullopt};
    const auto before = state.operation_revision;
    state.operation_revision = OperationRevision(before.value() + 1U);
    ++state.updated_at_ns;
    // Typed accepted-control persistence fixture: no new Kernel ACK authority.
    const OperationControlAcceptedDTO accepted{
        request.request_context.request_id, RequestReplayStatusDTO::kAcceptedNew,
        state.operation_id, before, OperationControlKind::kAcknowledgeCompleted,
        state.operation_revision, state.updated_at_ns};
    const auto stored = journal.RecordControlAccepted(owner, request, accepted, state);
    ASSERT_TRUE(stored.ok()) << stored.error().message;
    const auto events = journal.ReadEvents(owner, 0U, 8U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 2U);
    EXPECT_EQ(events.value().events.back().operation_revision, state.operation_revision);
    const auto payload = manyforge::content_identity::parse_canonical_json(
        events.value().events.back().payload_json);
    const auto* rows = payload.find_object_member("operation_state")
                           ->find_object_member("participants");
    ASSERT_EQ(rows->as_array().size(), 1U);
    const auto* encoded = rows->as_array().front().find_object_member("io_rule_outcomes");
    ASSERT_NE(encoded, nullptr);
    const auto decoded = DecodeIoRuleOutcomes(manyforge::content_identity::canonical_json(*encoded));
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), original);
  }
  SqliteOperationJournal reopened(database, cell, 8U);
  const auto recovered = reopened.BeginOwnerEpoch(
      RequestOwnerEpoch("owner.rule.ack.replacement"), 20, 8U, resources);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  const auto& historical = recovered.value().historical_operations.front();
  EXPECT_EQ(historical.operation_revision, OperationRevision(2U));
  EXPECT_EQ(historical.participants.front().io_rule_outcomes, original);
  EXPECT_EQ(std::get<IoOperationRuleScopeDTO>(historical.participants.front()
                    .io_rule_outcomes.front().registration.scope).operation_revision,
            OperationRevision(1U));
}
TEST(SqliteOperationJournalTest, CommonCommandIdentityUsesOneNeutralStoredShape) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "neutral-command.sqlite3";
  const CellId cell("cell.command");
  const RequestOwnerEpoch owner("owner.command");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const auto state = OperationState("operation.command", OperationStateKind::kCompleted, 10);
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 1, 8U, inventory).ok());
    const auto intent = CommandIntent("request.command", owner.value());
    ASSERT_TRUE(journal.RecordOperationAccepted(owner, intent, Acceptance(intent, state), state).ok());
    const auto events = journal.ReadOperationalEvents(owner, 0U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 1U);
    const auto& event = events.value().events.front();
    EXPECT_EQ(event.payload_digest,
              manyforge::content_identity::sha256_digest(event.payload_json));
    const auto payload = manyforge::content_identity::parse_canonical_json(event.payload_json);
    EXPECT_EQ(payload.find_object_member("api_version")->as_string(),
              "manyforge.operation_journal_record.v8");
    const auto& participant = payload.find_object_member("operation_state")
                                  ->find_object_member("participants")->as_array().front();
    ASSERT_NE(participant.find_object_member("command_id"), nullptr);
    ASSERT_NE(participant.find_object_member("command_generation"), nullptr);
    EXPECT_EQ(participant.find_object_member("command_id")->as_string(),
              state.participants.front().command_id.value());
    EXPECT_EQ(participant.find_object_member("command_generation")->as_number(),
              static_cast<double>(state.participants.front().command_generation.value()));
    EXPECT_EQ(participant.find_object_member("controller_goal_id"), nullptr);
    EXPECT_EQ(participant.find_object_member("controller_goal_generation"), nullptr);
  }
  sqlite3* connection = nullptr;
  ASSERT_EQ(sqlite3_open(database.c_str(), &connection), SQLITE_OK);
  sqlite3_stmt* columns = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(connection, "PRAGMA table_info(operation_participants)",
                              -1, &columns, nullptr), SQLITE_OK);
  std::set<std::string> names;
  while (sqlite3_step(columns) == SQLITE_ROW)
    names.emplace(reinterpret_cast<const char*>(sqlite3_column_text(columns, 1)));
  ASSERT_EQ(sqlite3_finalize(columns), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(connection), SQLITE_OK);
  EXPECT_TRUE(names.contains("command_id"));
  EXPECT_TRUE(names.contains("command_generation"));
  EXPECT_FALSE(names.contains("controller_goal_id"));
  EXPECT_FALSE(names.contains("controller_goal_generation"));
  SqliteOperationJournal reopened(database, cell, 8U);
  const auto recovered = reopened.BeginOwnerEpoch(
      RequestOwnerEpoch("owner.command.replacement"), 20, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front().participants, state.participants);
}
TEST(SqliteOperationJournalTest, RuleEvidenceSurvivesRealJournalCoalescingAndReopen) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "rule-evidence.sqlite3";
  const RequestOwnerEpoch owner("owner.rules");
  const std::vector<ResourceId> resources{ResourceId("robot.gripper")};
  auto state = OperationState("operation.rules", OperationStateKind::kStopped, 10);
  state.participants.front().io_rule_outcomes.push_back(rule_codec_test::Outcome(state, owner));
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 1, 8U, resources).ok());
    const auto intent = CommandIntent("request.rules", owner.value());
    ASSERT_TRUE(journal.RecordOperationAccepted(owner, intent, Acceptance(intent, state), state).ok());
    for (auto disposition : {IoRuleArbitrationDispositionDTO::kQueued, IoRuleArbitrationDispositionDTO::kSuperseded}) {
      state.participants.front().io_rule_outcomes.front().arbitration_disposition = disposition;
      state.updated_at_ns += 1;
      ASSERT_TRUE(journal.RecordOperationState(owner, state, nullptr, "rule_evidence_observed").ok());
    }
    const auto events = journal.ReadEvents(owner, 0U, 8U);
    ASSERT_TRUE(events.ok()); ASSERT_EQ(events.value().events.size(), 2U);
    const auto payload = manyforge::content_identity::parse_canonical_json(events.value().events.back().payload_json);
    EXPECT_EQ(payload.find_object_member("api_version")->as_string(), "manyforge.operation_journal_record.v8");
  }
  SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
  const auto recovered = reopened.BeginOwnerEpoch(RequestOwnerEpoch("owner.rules.replacement"), 20, 8U, resources);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front().participants.front().io_rule_outcomes,
            state.participants.front().io_rule_outcomes);
}
}  // namespace

namespace {
IoGlobalRuleRecoveryRecordDTO GlobalRecoveryRecord(const RequestOwnerEpoch& owner,
                                                   std::string request_id = "global.recover") {
  auto state = rule_codec_test::State();
  state.updated_at_ns = 10;
  auto outcome = rule_codec_test::Outcome(state, owner);
  const IoRuleStateContextDTO context{owner, UINT64_MAX, ActiveBundlePublicationGeneration(11U),
                                      RetainedBundleId("retained.global"),
                                      GraphRevision("graph.global")};
  const IoActivationRuleScopeDTO scope{context.retained_bundle_id, context.graph_revision};
  outcome.registration.scope = scope;
  outcome.evidence.trip->registration = outcome.registration;
  for (auto& loss : outcome.evidence.source_losses) loss.registration = outcome.registration;
  auto successor = outcome.registration;
  successor.registration_id = 1U;
  return {{{RequestId(request_id), owner, "operator", std::string("correlation")},
           {context, outcome.registration.policy_id, outcome.registration.registration_id},
           std::string("Operator acknowledges the inactive condition")},
          {RequestId(request_id),
           RequestReplayStatusDTO::kAcceptedNew,
           true,
           context,
           {outcome.registration.policy_id, IoGlobalRuleState::kClear, std::nullopt},
           std::nullopt},
          {outcome.registration.policy_id, IoGlobalRuleState::kLatched,
           outcome.evidence.trip->triggering_acquisition},
          outcome.evidence,
          {false, 11},
          successor};
}

TEST(SqliteOperationJournalTest, GlobalRecoveryCodecRetainsActualActivationAndFullEvidence) {
  const auto record = GlobalRecoveryRecord(RequestOwnerEpoch("global.owner"));
  const auto encoded = EncodeIoGlobalRuleRecoveryRecord(record);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const std::string current_version = "manyforge.internal.io_global_rule_recovery_record.v2";
  const auto version_at = encoded.value().find(current_version);
  ASSERT_NE(version_at, std::string::npos);
  auto old_version = encoded.value();
  old_version.replace(version_at, current_version.size(),
                      "manyforge.internal.io_global_rule_recovery_record.v1");
  EXPECT_FALSE(DecodeIoGlobalRuleRecoveryRecord(old_version).ok());
  const auto decoded = DecodeIoGlobalRuleRecoveryRecord(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().intent, record.intent);
  EXPECT_EQ(decoded.value().result, record.result);
  EXPECT_EQ(decoded.value().prior_latched_state, record.prior_latched_state);
  EXPECT_EQ(decoded.value().prior_latched_evidence, record.prior_latched_evidence);
  EXPECT_EQ(decoded.value().successor_registration, record.successor_registration);
  EXPECT_EQ(decoded.value().inactive_observation.value, std::optional<bool>(false));
  EXPECT_EQ(decoded.value().inactive_observation.evaluated_at_ns, 11);
  EXPECT_EQ(EncodeIoGlobalRuleRecoveryRecord(decoded.value()).value(), encoded.value());
  EXPECT_EQ(decoded.value().intent.selector.expected_rule_registration_id, UINT64_MAX);
  ASSERT_TRUE(decoded.value().prior_latched_evidence.trip->blackboard);
  EXPECT_EQ(decoded.value().prior_latched_evidence.trip->blackboard->values.back()->value,
            record.prior_latched_evidence.trip->blackboard->values.back()->value);
  EXPECT_EQ(encoded.value().find("stop_id"), std::string::npos);
  EXPECT_EQ(encoded.value().find("operation_id"), std::string::npos);
  auto unknown = encoded.value();
  unknown.insert(unknown.size() - 1U, ",\"unknown\":0");
  EXPECT_FALSE(DecodeIoGlobalRuleRecoveryRecord(unknown).ok());
  EXPECT_FALSE(DecodeIoGlobalRuleRecoveryRecord(" " + encoded.value()).ok());

  // Generalizing shared registration evidence must not relax operation outcomes.
  auto operation =
      rule_codec_test::Outcome(rule_codec_test::State(), record.result.context.owner_epoch);
  ASSERT_TRUE(EncodeIoRuleOutcomes(std::span(&operation, 1U)).ok());
  operation.registration.scope = record.successor_registration.scope;
  operation.evidence.trip->registration = operation.registration;
  for (auto& loss : operation.evidence.source_losses) loss.registration = operation.registration;
  EXPECT_FALSE(EncodeIoRuleOutcomes(std::span(&operation, 1U)).ok());
}

TEST(SqliteOperationJournalTest, GlobalRecoveryRefusesMismatchedTupleAndUnknownCurrentCondition) {
  const auto original = GlobalRecoveryRecord(RequestOwnerEpoch("global.owner"));
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "global-invalid.sqlite3", CellId("cell.test"),
                                 8U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(original.result.context.owner_epoch, 0, 8U, {}).ok());
  std::vector<std::function<void(IoGlobalRuleRecoveryRecordDTO&)>> invalid{
      [](auto& r) { r.intent.request_context.request_owner_epoch = RequestOwnerEpoch("foreign"); },
      [](auto& r) { r.intent.request_context.request_id = RequestId(""); },
      [](auto& r) { r.result.request_id = RequestId("foreign"); },
      [](auto& r) { r.result.accepted = false; },
      [](auto& r) { r.result.replay_status = RequestReplayStatusDTO::kReplayedExact; },
      [](auto& r) { r.result.error = ErrorDTO{"error", "not accepted", {}}; },
      [](auto& r) { r.result.context.catalog_generation = 1U; },
      [](auto& r) { r.result.rule.state = IoGlobalRuleState::kLatched; },
      [](auto& r) {
        r.result.rule.triggering_acquisition = r.prior_latched_state.triggering_acquisition;
      },
      [](auto& r) { r.prior_latched_state.state = IoGlobalRuleState::kClear; },
      [](auto& r) {
        r.prior_latched_state.triggering_acquisition.reset();
        r.prior_latched_evidence =
            {IoRuleRegistrationState::kArmed, std::nullopt, {}, std::nullopt};
      },
      [](auto& r) { r.prior_latched_state.triggering_acquisition.reset(); },
      [](auto& r) { r.intent.selector.expected_rule_registration_id = 0U; },
      [](auto& r) { r.prior_latched_evidence.trip->registration.registration_id = 7U; },
      [](auto& r) {
        r.prior_latched_evidence.source_losses.front().registration.policy_id =
            InterruptPolicyId("foreign");
      },
      [](auto& r) {
        r.prior_latched_evidence.trip->source_cursors.front().owner_epoch =
            RequestOwnerEpoch("foreign");
      },
      [](auto& r) {
        r.prior_latched_evidence.trip->blackboard->scope.active_publication_generation =
            ActiveBundlePublicationGeneration(1U);
      },
      [](auto& r) { r.inactive_observation.value = true; },
      [](auto& r) { r.inactive_observation.value.reset(); },
      [](auto& r) { r.inactive_observation.evaluated_at_ns = -1; },
      [](auto& r) { r.inactive_observation.evaluated_at_ns = 9; },
      [](auto& r) { r.successor_registration.registration_id = UINT64_MAX; },
      [](auto& r) { r.successor_registration.registration_id = 0U; },
      [](auto& r) { r.successor_registration.owner_epoch = RequestOwnerEpoch("foreign"); },
      [](auto& r) {
        std::get<IoActivationRuleScopeDTO>(r.successor_registration.scope).graph_revision =
            GraphRevision("foreign");
      },
      [](auto& r) {
        r.successor_registration.scope =
            rule_codec_test::Outcome(rule_codec_test::State(), RequestOwnerEpoch("global.owner"))
                .registration.scope;
      },
      [](auto& r) {
        r.intent.selector.context.active_publication_generation =
            ActiveBundlePublicationGeneration(9007199254740992ULL);
      }};
  for (std::size_t index = 0U; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    auto changed = original;
    invalid[index](changed);
    EXPECT_FALSE(EncodeIoGlobalRuleRecoveryRecord(changed).ok());
    EXPECT_FALSE(journal.RecordGlobalIoRuleCleared(changed).ok());
  }
  const auto empty = journal.ReadEvents(original.result.context.owner_epoch, 0U);
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty.value().events.empty());
  ASSERT_TRUE(journal.RecordGlobalIoRuleCleared(original).ok());
  const auto stored = journal.ReadEvents(original.result.context.owner_epoch, 0U);
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored.value().events.size(), 1U);
  auto baseline = original;
  baseline.prior_latched_evidence.trip->triggering_acquisition.reset();
  baseline.prior_latched_state.triggering_acquisition.reset();
  EXPECT_TRUE(EncodeIoGlobalRuleRecoveryRecord(baseline).ok());  // No invented baseline cause.
}

TEST(SqliteOperationJournalTest, GlobalRecoveryReplaysExactlyAcrossReopenWithoutMirrorClear) {
  ScopedTemporaryDirectory directory;
  const auto database = directory.path() / "global-recovery.sqlite3";
  const RequestOwnerEpoch owner("global.owner");
  const auto record = GlobalRecoveryRecord(owner);
  std::uint64_t sequence{};
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 8U, {}).ok());
    const auto first = journal.RecordGlobalIoRuleCleared(record);
    ASSERT_TRUE(first.ok()) << first.error().message;
    sequence = first.value();
    const auto retry = journal.RecordGlobalIoRuleCleared(record);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value(), sequence);
    auto changed = record;
    changed.intent.reason = "Different accepted history";
    const auto conflict = journal.RecordGlobalIoRuleCleared(changed);
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error().code, "request_id_conflict");
    const auto events = journal.ReadEvents(owner, 0U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 1U);
    const auto& event = events.value().events.front();
    EXPECT_EQ(event.record_kind, "global_io_rule_recovery_authorized");
    EXPECT_EQ(event.request_id, std::optional(record.intent.request_context.request_id));
    EXPECT_FALSE(event.operation_id);
    EXPECT_FALSE(event.operation_revision);
    EXPECT_FALSE(event.effect_pending);
    EXPECT_EQ(event.occurred_at_ns, 11);
    EXPECT_EQ(event.payload_json, EncodeIoGlobalRuleRecoveryRecord(record).value());
    const auto operational = journal.ReadOperationalEvents(owner, 0U);
    ASSERT_TRUE(operational.ok());
    EXPECT_TRUE(
        operational.value().events.empty());  // Core publishes actual Clear only after commit.
  }
  SqliteOperationJournal reopened(database, CellId("cell.test"), 8U);
  const auto retry = reopened.RecordGlobalIoRuleCleared(record);
  ASSERT_TRUE(retry.ok());
  EXPECT_EQ(retry.value(), sequence);
  const auto all = reopened.ReadEvents(owner, 0U);
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all.value().events.size(), 1U);
  EXPECT_EQ(all.value().events.front().sequence, sequence);
}

TEST(SqliteOperationJournalTest,
     GlobalRecoveryUsesBoundedHistoryAndExistingCrossKindRequestIdentity) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "global-history.sqlite3", CellId("cell.test"),
                                 2U);
  const RequestOwnerEpoch owner("global.owner");
  ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 8U, {ResourceId("robot.gripper")}).ok());
  auto intent = CommandIntent("ordinary", owner.value());
  const auto state = OperationState("operation.ordinary", OperationStateKind::kExecuting, 1);
  ASSERT_TRUE(
      journal.RecordOperationAccepted(owner, intent, Acceptance(intent, state), state).ok());
  auto record = GlobalRecoveryRecord(owner, "ordinary");
  const auto conflict = journal.RecordGlobalIoRuleCleared(record);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "request_id_conflict");
  std::uint64_t last{};
  for (std::uint64_t index = 1U; index <= 3U; ++index) {
    record = GlobalRecoveryRecord(owner, "recover." + std::to_string(index));
    record.successor_registration.registration_id = index;
    const auto result = journal.RecordGlobalIoRuleCleared(record);
    ASSERT_TRUE(result.ok());
    last = result.value();
  }
  const auto retained = journal.ReadEvents(owner, 0U);
  ASSERT_TRUE(retained.ok());
  EXPECT_EQ(std::count_if(retained.value().events.begin(), retained.value().events.end(),
                          [](const auto& e) {
                            return e.record_kind == "global_io_rule_recovery_authorized";
                          }),
            2);
  const auto retry = journal.RecordGlobalIoRuleCleared(record);
  ASSERT_TRUE(retry.ok());
  EXPECT_EQ(retry.value(), last);
  ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("replacement"), 20, 8U, {}).ok());
  const auto stale = journal.RecordGlobalIoRuleCleared(record);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "operation_journal_owner_epoch_stale");
}

TEST(SqliteOperationJournalTest, GlobalRecoveryFailedWriteKeepsExactPayloadRetryable) {
  ScopedTemporaryDirectory directory;
  bool fail = false;
  SqliteOperationJournal journal(directory.path() / "global-retry.sqlite3", CellId("cell.test"), 8U,
                                 {}, [&](std::string_view point) {
                                   if (fail && point == "ordinary_write")
                                     throw std::runtime_error("injected writer failure");
                                 });
  const RequestOwnerEpoch owner("global.owner");
  ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 8U, {}).ok());
  const auto record = GlobalRecoveryRecord(owner);
  fail = true;
  EXPECT_FALSE(journal.RecordGlobalIoRuleCleared(record).ok());
  fail = false;
  const auto empty = journal.ReadEvents(owner, 0U);
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty.value().events.empty());
  const auto stored = journal.RecordGlobalIoRuleCleared(record);
  ASSERT_TRUE(stored.ok());
  const auto retry = journal.RecordGlobalIoRuleCleared(record);
  ASSERT_TRUE(retry.ok());
  EXPECT_EQ(stored.value(), retry.value());
  // This is a known pre-commit failure, not a simulated unknown COMMIT result.
}

TEST(SqliteOperationJournalTest,
     GlobalRecoverySeparatelyRetainsActualLossObserverWithoutRelabelingLatch) {
  const RequestOwnerEpoch owner("global.owner");
  auto record = GlobalRecoveryRecord(owner);
  const auto old_state = record.prior_latched_state;
  const auto old_evidence = record.prior_latched_evidence;
  const auto legacy = EncodeIoGlobalRuleRecoveryRecord(record);
  ASSERT_TRUE(legacy.ok());
  EXPECT_EQ(legacy.value().find("source_loss_evidence"), std::string::npos);
  auto upgraded = old_evidence;
  upgraded.trip.reset();
  for (auto& loss : upgraded.source_losses) loss.registration.registration_id = 2U;
  record.source_loss_evidence = upgraded;
  const auto encoded = EncodeIoGlobalRuleRecoveryRecord(record);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodeIoGlobalRuleRecoveryRecord(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().prior_latched_state, old_state);
  EXPECT_EQ(decoded.value().prior_latched_evidence, old_evidence);
  EXPECT_EQ(decoded.value().source_loss_evidence, record.source_loss_evidence);
  EXPECT_EQ(decoded.value().successor_registration.registration_id, 1U);
  EXPECT_EQ(
      decoded.value().source_loss_evidence->source_losses.front().registration.registration_id, 2U);
  EXPECT_EQ(decoded.value().intent.selector.expected_rule_registration_id, UINT64_MAX);
  EXPECT_EQ(EncodeIoGlobalRuleRecoveryRecord(decoded.value()).value(), encoded.value());
  const auto decoded_legacy = DecodeIoGlobalRuleRecoveryRecord(legacy.value());
  ASSERT_TRUE(decoded_legacy.ok());
  EXPECT_FALSE(decoded_legacy.value().source_loss_evidence);

  std::vector<std::function<void(IoGlobalRuleRecoveryRecordDTO&)>> invalid{
      [](auto& r) { r.source_loss_evidence->source_losses.clear(); },
      [](auto& r) {
        r.source_loss_evidence->source_losses.front().registration.owner_epoch =
            RequestOwnerEpoch("foreign");
      },
      [](auto& r) {
        r.source_loss_evidence->source_losses.front().registration.catalog_generation = 3U;
      },
      [](auto& r) {
        r.source_loss_evidence->source_losses.front().registration.policy_id =
            InterruptPolicyId("foreign");
      },
      [](auto& r) {
        r.source_loss_evidence->source_losses.front().registration.registration_id = 0U;
      },
      [](auto& r) {
        r.source_loss_evidence->source_losses.front().registration.registration_id =
            r.successor_registration.registration_id;
      },
      [](auto& r) {
        std::get<IoActivationRuleScopeDTO>(
            r.source_loss_evidence->source_losses.front().registration.scope)
            .graph_revision = GraphRevision("foreign");
      },
      [](auto& r) {
        r.source_loss_evidence->source_losses.front().registration.scope =
            rule_codec_test::Outcome(rule_codec_test::State(), RequestOwnerEpoch("global.owner"))
                .registration.scope;
      },
      [](auto& r) { r.source_loss_evidence->source_losses.front().evaluated_at_ns = 12; },
      [](auto& r) {
        r.source_loss_evidence->source_losses.front()
            .blackboard->scope.active_publication_generation =
            ActiveBundlePublicationGeneration(3U);
      },
      [](auto& r) {
        auto other = r.source_loss_evidence->source_losses.front();
        other.source_id = InterruptSourceId("other.actual.source");
        other.cursor.source_id = other.source_id;
        other.blackboard.reset();
        other.registration.registration_id = 3U;
        r.source_loss_evidence->source_losses.push_back(other);
      }};
  for (std::size_t index = 0U; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    auto changed = record;
    invalid[index](changed);
    EXPECT_FALSE(EncodeIoGlobalRuleRecoveryRecord(changed).ok());
  }
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "global-upgrade.sqlite3", CellId("cell.test"),
                                 8U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 8U, {}).ok());
  const auto stored = journal.RecordGlobalIoRuleCleared(record);
  ASSERT_TRUE(stored.ok());
  const auto retry = journal.RecordGlobalIoRuleCleared(record);
  ASSERT_TRUE(retry.ok());
  EXPECT_EQ(stored.value(), retry.value());
  auto missing_actual_upgrade = record;
  missing_actual_upgrade.source_loss_evidence.reset();
  const auto conflict = journal.RecordGlobalIoRuleCleared(missing_actual_upgrade);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "request_id_conflict");
  const auto events = journal.ReadEvents(owner, 0U);
  ASSERT_TRUE(events.ok());
  ASSERT_EQ(events.value().events.size(), 1U);
  const auto persisted =
      DecodeIoGlobalRuleRecoveryRecord(events.value().events.front().payload_json);
  ASSERT_TRUE(persisted.ok());
  EXPECT_EQ(persisted.value().source_loss_evidence, record.source_loss_evidence);
  auto actual_loss_latch = GlobalRecoveryRecord(owner, "loss.original");
  actual_loss_latch.prior_latched_state.triggering_acquisition.reset();
  actual_loss_latch.prior_latched_evidence.trip.reset();
  EXPECT_TRUE(EncodeIoGlobalRuleRecoveryRecord(actual_loss_latch)
                  .ok());  // Retained loss, no fabricated trip.
}
}  // namespace
