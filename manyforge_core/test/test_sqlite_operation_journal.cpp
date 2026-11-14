#include <manyforge_core/testing/request_key_fixture.hpp>
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
#include <iterator>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/stop_event_codec.hpp>
#include <manyforge_core/stop_state.hpp>
#include <manyforge_core/types/operation_member_id_json.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace manyforge::core;

/// The application-start identity of every journal in these tests.
constexpr ApplicationStartId kJournalStartId{0x3f9a00000000beefULL};

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

[[nodiscard]] OperationIntentDTO CommandIntent(std::string request_id) {
  return OperationIntentDTO{
      PublicRequestContextDTO{TestRequestKey(std::move(request_id)), "journal-test"},
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      ProgramRunNumber(1U),
      GraphRevision("graph-revision"),
      RetainedBundleId("retained-bundle"),
      std::nullopt,
      {OperationParticipantIntentDTO{
          {ControllableComponentId("robot.1"), CommandTargetId("gripper")}, CapabilityId("tool.gripper.command"),
          std::nullopt,
          CapabilityCommandIntentDTO{
              CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
              CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
              GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
              2.0,
              {CommandOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}}},
      OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] OperationStateDTO OperationState(std::uint64_t operation_id, OperationStateKind kind,
                                               std::int64_t updated_at_ns) {
  const OperationNumber id(operation_id);
  return OperationStateDTO{
      id,
      OperationRevision(1U),
      ProgramBindingId("program-binding"),
      ProgramRunNumber(1U),
      OperationCoordinationPolicy::kIndependent,
      kind,
      {OperationParticipantStateDTO{OperationParticipantId{id, 1U},
                                    ControllerGoalId{id, 1U, 1U},
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
      std::nullopt};
}

[[nodiscard]] OperationStateDTO PreparedMotionState(std::uint64_t operation_id,
                                                    std::int64_t updated_at_ns) {
  const OperationNumber id(operation_id);
  return OperationStateDTO{
      id,
      OperationRevision(1U),
      ProgramBindingId("program-binding"),
      ProgramRunNumber(1U),
      OperationCoordinationPolicy::kIndependent,
      OperationStateKind::kPrepared,
      {OperationParticipantStateDTO{OperationParticipantId{id, 1U},
                                    ControllerGoalId{id, 1U, 1U},
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
      std::nullopt};
}

[[nodiscard]] OperationAcceptedDTO Acceptance(const OperationIntentDTO& intent,
                                              const OperationStateDTO& state) {
  return OperationAcceptedDTO{
      intent.request_context.request_key,
      RequestReplayStatusDTO::kAcceptedNew,
      state.operation_id,
      state.operation_revision,
      {OperationParticipantAssignmentDTO{0U, state.participants.front().participant_id}},
      state.updated_at_ns};
}

[[nodiscard]] ContentDigest Digest(char digit) {
  return ContentDigest("sha256:" + std::string(64U, digit));
}

template <std::size_t Capacity>
void SetStopText(BoundedStopText<Capacity>& text, const std::string_view value) {
  std::copy(value.begin(), value.end(), text.bytes.begin());
  text.length = static_cast<std::uint16_t>(value.size());
}

/// One accepted Stop event as the Kernel's journal writer copies it: a
/// Stop all from an authenticated HTTP operator that started one participant.
[[nodiscard]] StopEventRecord AcceptedStopEvent(const std::uint64_t event_id,
                                                const std::int64_t accepted_at_ns) {
  StopEventRecord record;
  record.covered.resize(4U);
  record.stale.resize(4U);
  record.covered_lanes.resize(4U);
  record.event_id = StopEventId(event_id);
  record.request_key = TestRequestKey("request.stop-event." + std::to_string(event_id));
  record.requested = StopKind::kStopAll;
  record.applied = StopKind::kStopAll;
  record.transport = RequestTransportDTO::kHttp;
  record.assurance = RequestAssuranceDTO::kExternallyAuthenticated;
  SetStopText(record.principal, "operator.1");
  SetStopText(record.source_label, "composer");
  SetStopText(record.reason, "operator Stop");
  auto& entry = record.covered.front();
  entry.operation_id = OperationNumber(10U);
  entry.participant_ordinal = 1U;
  entry.lane_index = 0U;
  entry.lane_generation = 1U;
  entry.started = true;
  record.covered_count = 1U;
  record.covered_lanes.front() = 1U;
  record.outstanding = 1U;
  record.accepted_at_ns = accepted_at_ns;
  return record;
}

/// The same event once its participant stopped at the nominal stage.
[[nodiscard]] StopEventRecord CompletedStopEvent(StopEventRecord record,
                                                 const std::int64_t completed_at_ns) {
  auto& entry = record.covered.front();
  entry.done = true;
  entry.phase = StopPhase::kStopped;
  entry.containing_stage = 0U;
  entry.containing_stage_kind = StopEscalationStageKind::kNominal;
  record.covered_lanes.front() = 0U;
  record.outstanding = 0U;
  record.result = StopEventResult::kStopped;
  record.completed_at_ns = completed_at_ns;
  return record;
}

/// Plain sqlite3 reader for test evidence (no writes).
class RawJournal final {
 public:
  explicit RawJournal(const std::filesystem::path& path) {
    if (sqlite3_open(path.c_str(), &database_) != SQLITE_OK) {
      sqlite3_close(database_);
      throw std::runtime_error("test journal open failed");
    }
  }
  ~RawJournal() { sqlite3_close(database_); }
  RawJournal(const RawJournal&) = delete;
  RawJournal& operator=(const RawJournal&) = delete;

  /// Every row of `sql` as text columns (NULL is nullopt); `text` binds ?1.
  [[nodiscard]] std::vector<std::vector<std::optional<std::string>>> Rows(
      const std::string& sql, const std::optional<std::string>& text = std::nullopt) const {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
      sqlite3_finalize(statement);
      throw std::runtime_error("test query failed: " + sql);
    }
    if (text.has_value()) {
      sqlite3_bind_text(statement, 1, text->data(), static_cast<int>(text->size()),
                        SQLITE_TRANSIENT);
    }
    std::vector<std::vector<std::optional<std::string>>> rows;
    while (sqlite3_step(statement) == SQLITE_ROW) {
      auto& row = rows.emplace_back();
      for (int column = 0; column < sqlite3_column_count(statement); ++column) {
        if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
          row.emplace_back(std::nullopt);
        } else {
          row.emplace_back(std::string(
              reinterpret_cast<const char*>(sqlite3_column_text(statement, column)),
              static_cast<std::size_t>(sqlite3_column_bytes(statement, column))));
        }
      }
    }
    sqlite3_finalize(statement);
    return rows;
  }

 private:
  sqlite3* database_{nullptr};
};

/// `payloads` as one batch of the journal writer.
JournalBatchResult Write(SqliteOperationJournal& journal,
                         std::vector<JournalRecordPayload> payloads,
                         const std::uint64_t records_dropped = 0U) {
  std::vector<JournalRecord> records;
  records.reserve(payloads.size());
  for (auto& payload : payloads) records.push_back(JournalRecord{std::move(payload), "test"});
  return journal.WriteBatch(records, records_dropped);
}

/// One record through the writer's batch path: Ok, or that record's error.
Result<void> WriteOne(SqliteOperationJournal& journal, JournalRecordPayload payload) {
  const auto result = Write(journal, {std::move(payload)});
  if (result.failures.empty()) return Result<void>::Ok();
  return Result<void>::Err(result.failures.front().error);
}

Result<void> Accept(SqliteOperationJournal& journal, const OperationIntentDTO& intent,
                    const OperationAcceptedDTO& accepted, const OperationStateDTO& state) {
  return WriteOne(journal, JournalOperationAccepted{intent, accepted, state});
}

Result<void> Control(SqliteOperationJournal& journal, const OperationControlRequestDTO& request,
                     const OperationControlAcceptedDTO& accepted,
                     const OperationStateDTO& state) {
  return WriteOne(journal, JournalControlAccepted{request, accepted, state});
}

JournalOperationState StateRecord(
    const OperationStateDTO& state, const RuntimeScaleStatusDTO* scale, const char* reason,
    std::vector<PreparedMotionDiagnosticsDTO> diagnostics = {}) {
  return JournalOperationState{
      state, scale == nullptr ? std::nullopt : std::optional(*scale), reason,
      std::move(diagnostics)};
}

Result<void> Observe(SqliteOperationJournal& journal, const OperationStateDTO& state,
                     const RuntimeScaleStatusDTO* scale, const char* reason,
                     std::vector<PreparedMotionDiagnosticsDTO> diagnostics = {}) {
  return WriteOne(journal, StateRecord(state, scale, reason, std::move(diagnostics)));
}

JournalStopEventRow StopRow(const StopEventRecord& record, const bool completed) {
  return JournalStopEventRow{record.event_id, 0U, completed,
                             std::make_shared<const StopEventRecord>(record)};
}

Result<void> StopAccepted(SqliteOperationJournal& journal, const StopEventRecord& record) {
  return WriteOne(journal, StopRow(record, false));
}

Result<void> StopCompleted(SqliteOperationJournal& journal, const StopEventRecord& record) {
  return WriteOne(journal, StopRow(record, true));
}

Result<void> RuleChanged(SqliteOperationJournal& journal, const IoRuleStateChangeDTO& change) {
  return WriteOne(journal, change);
}

TEST(SqliteOperationJournalTest, ExactLongServiceTimesSurviveOperationRecords) {
  constexpr std::int64_t boundary = 9'007'199'254'740'991LL;
  for (const auto timestamp :
       std::array<std::int64_t, 5>{boundary, boundary + 1, boundary + 2, 105LL * 86400 * 1000000000,
                                   31LL * 366 * 86400 * 1000000000}) {
    SCOPED_TRACE(timestamp);
    ScopedTemporaryDirectory directory;
    const auto database = directory.path() / "journal.sqlite3";
    const CellId cell("cell.test");
    auto state =
        OperationState(10U, OperationStateKind::kExecuting, timestamp);
    SqliteOperationJournal journal(database, cell);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, timestamp, 16U).ok());
    const auto command = CommandIntent("request.long-service");
    ASSERT_TRUE(
        Accept(journal, command, Acceptance(command, state), state).ok());
    state.updated_at_ns = timestamp + 1;
    state.operation_revision = OperationRevision(2U);
    state.state = OperationStateKind::kCompleted;
    state.participants.front().state = OperationStateKind::kCompleted;
    ASSERT_TRUE(
        Observe(journal, state, nullptr, "subordinate_completed").ok());
    const auto events = journal.ReadOperationalEvents(0U);
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
  }
}

TEST(SqliteOperationJournalTest, CorruptV9CutoverLeavesIdentityAndEvidenceUntouched) {
  for (const auto corruption : {
           "UPDATE journal_metadata SET started_at_ns=9007199254740992",
           "UPDATE operation_snapshots SET updated_at_ns=9007199254740992",
           "UPDATE journal_records SET occurred_at_ns=9007199254740992",
           "UPDATE journal_records SET payload_digest='corrupt'",
       }) {
    SCOPED_TRACE(corruption);
    ScopedTemporaryDirectory directory;
    const auto database = directory.path() / "journal.sqlite3";
    const CellId cell("cell.test");
    {
      SqliteOperationJournal journal(database, cell);
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 16U).ok());
      auto state = OperationState(11U, OperationStateKind::kExecuting, 2);
      const auto command = CommandIntent("request.legacy");
      ASSERT_TRUE(
          Accept(journal, command, Acceptance(command, state), state).ok());
    }
    sqlite3* legacy = nullptr;
    ASSERT_EQ(sqlite3_open(database.c_str(), &legacy), SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(
            legacy,
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
    {
      SqliteOperationJournal journal(database, cell);
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 16U).ok());
      auto state = OperationState(11U, OperationStateKind::kExecuting, 2);
      state.journal_capture_order = 1U;
      const auto command = CommandIntent("request.legacy");
      ASSERT_TRUE(
          Accept(journal, command, Acceptance(command, state), state).ok());
    }
    sqlite3* legacy = nullptr;
    ASSERT_EQ(sqlite3_open(database.c_str(), &legacy), SQLITE_OK);
    const auto downgrade =
        "PRAGMA user_version=11; UPDATE journal_metadata SET schema_version=11,"
        "schema_id='manyforge.internal.execution.operation-journal.o1.5.v11';"
        "UPDATE operation_snapshots SET journal_capture_order=" +
        std::to_string(order);
    ASSERT_EQ(sqlite3_exec(legacy, downgrade.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_THROW(SqliteOperationJournal(database, cell), std::runtime_error);
    sqlite3_stmt* version = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(legacy,
                                 "SELECT schema_version FROM journal_metadata",
                                 -1, &version, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(version, 0), 11);
    sqlite3_finalize(version);
    sqlite3_close(legacy);
  }
}

TEST(SqliteOperationJournalTest,
     PreparedMotionStateRetainsBoundedMetricsAndExactSpeedProvenance) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "operation.sqlite3", CellId("cell.test"));
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  const auto state = PreparedMotionState(12U, 10);

  const auto missing =
      Observe(journal, state, nullptr, "subordinate_motion_prepared");
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
          CartesianFidelityMetricsDTO{0.005, 0.05, 0.001, 0.01, 0.0005, 0.002, 0.0015, 0.012}},
      0.25,
      0.15,
      0.15}};
  auto incorrect = diagnostics;
  incorrect.front().admitted_tcp_linear_speed_mps = 0.12;
  EXPECT_FALSE(Observe(journal, state, nullptr,
      "subordinate_motion_prepared", incorrect).ok());
  const auto recorded =
      Observe(journal, state, nullptr, "subordinate_motion_prepared", diagnostics);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;

  auto observed = state;
  observed.operation_revision = OperationRevision(2U);
  observed.state = OperationStateKind::kExecuting;
  observed.participants.front().state = OperationStateKind::kExecuting;
  observed.participants.front().progress = 0.25;
  observed.updated_at_ns = 11;
  ASSERT_TRUE(
      Observe(journal, observed, nullptr, "subordinate_state_observed").ok());

  const auto page = journal.ReadOperationalEvents(0U, kMaximumOperationJournalEventPage);
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
  EXPECT_EQ(payload.find("\"runtime_policy\""), std::string::npos);
  EXPECT_NE(payload.find("\"requested_tcp_linear_speed_mps\":0.25"), std::string::npos);
  EXPECT_NE(payload.find("\"deployment_maximum_tcp_linear_speed_mps\":0.15"),
            std::string::npos);
  EXPECT_NE(payload.find("\"admitted_tcp_linear_speed_mps\":0.15"), std::string::npos);

  const auto reopened = [&]() {
    journal.Close();
    SqliteOperationJournal recovered(directory.path() / "operation.sqlite3", CellId("cell.test"));
    return recovered.ReadOperationalEvents(0U, kMaximumOperationJournalEventPage);
  }();
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  ASSERT_EQ(reopened.value().events.size(), 2U);
  EXPECT_EQ(reopened.value().events.front().record_kind, "operation_prepared");
  EXPECT_NE(reopened.value().events.front().payload_json.find("\"prepared_motion_diagnostics\":[{"),
            std::string::npos);
}

TEST(SqliteOperationJournalTest, AcceptedMotionRecordsAbsoluteGoalFrameAndRobotQualifier) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "goal-frame.sqlite3", CellId("cell.1"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 4U).ok());
  auto intent = CommandIntent("goal-frame-motion");
  Pose3DTO goal;
  goal.position_m = {0.1, 0.2, 0.3};
  goal.orientation.w = 1.0;
  MotionParticipantIntentDTO motion{
      MoveId("move.framed"),
      {ControllableComponentId("robot.1"), FrameRoleId("base")},
      CurrentTcpDTO{},
      TolerancesDTO{0.01, 0.001, 0.01},
      SpeedProfileDTO{},
      TimeoutsDTO{},
      CacheMode::kDefault,
      PlannerOverrideDTO{},
      MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      PoseGoalDTO{goal},
      std::nullopt,
      {},
      {}};
  motion.goal_frame_reference =
      ReferenceDTO(ProgramFrameReferenceDTO{ControllableComponentId("robot.2"),
                                            FrameRoleId("tcp")});
  intent.participants.front().payload = motion;
  const auto state =
      OperationState(15U, OperationStateKind::kExecuting, 10);
  ASSERT_TRUE(
      Accept(journal, intent, Acceptance(intent, state), state).ok());
  const auto events = journal.ReadOperationalEvents(0U, 1U);
  ASSERT_TRUE(events.ok()) << events.error().message;
  ASSERT_EQ(events.value().events.size(), 1U);
  const auto& payload = events.value().events.front().payload_json;
  EXPECT_NE(payload.find("\"target_mode\":\"absolute\""), std::string::npos);
  EXPECT_NE(payload.find("\"goal_frame_reference\":{\"id\":{\"component_id\":\"robot.2\","
                         "\"frame_id\":\"tcp\"},\"pose\":null,\"type\":\"component_frame\"}"),
            std::string::npos)
      << payload;
}

TEST(SqliteOperationJournalTest, AcceptedHistoryRecordsManualHandoverContext) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  auto intent = CommandIntent("request.command.1");
  intent.manual_handover = ManualProgramHandoverContextDTO{ProgramRunNumber(136U),
                                                           CellAdmissionClaimGeneration(7U), 9U};
  // A manual handover has no run of its own: stored as null, read back absent.
  intent.program_run_id.reset();
  auto state =
      OperationState(13U, OperationStateKind::kExecuting, 10);
  state.program_run_id.reset();
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 4U).ok());
    ASSERT_TRUE(
        Accept(journal, intent, Acceptance(intent, state), state)
            .ok());
    const auto history = journal.ReadOperationalEvents(0U);
    ASSERT_TRUE(history.ok());
    EXPECT_TRUE(std::any_of(
        history.value().events.begin(), history.value().events.end(), [](const auto& event) {
          return event.payload_json.find(
                     "\"manual_handover\":{\"claim_generation\":7,\"hold_revision\":9,"
                     "\"program_run_id\":136}") != std::string::npos &&
                 event.payload_json.find("\"program_run_id\":null") != std::string::npos;
        }));
    state.state = OperationStateKind::kCompleted;
    state.participants.front().state = OperationStateKind::kCompleted;
    state.participants.front().progress = 1.0;
    state.updated_at_ns = 15;
    ASSERT_TRUE(
        Observe(journal, state, nullptr, "subordinate_completed").ok());
    journal.Close();
  }
}

TEST(SqliteOperationJournalTest, TypedControlScaleAndStopRecordsRoundTripAsTerminalHistory) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const auto intent = CommandIntent("request.command.1");
  auto state =
      OperationState(13U, OperationStateKind::kExecuting, 10);
  state.runtime_scale_generation = RuntimeScaleGeneration(0U);

  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    ASSERT_TRUE(
        Accept(journal, intent, Acceptance(intent, state), state)
            .ok());

    const OperationControlRequestDTO reset_request{
        PublicRequestContextDTO{TestRequestKey("request.reset.1"), "journal-test"},
        state.operation_id, state.operation_revision, std::nullopt};
    const auto prior_revision = state.operation_revision;
    state.operation_revision = OperationRevision(2U);
    state.updated_at_ns = 11;
    const OperationControlAcceptedDTO reset_accepted{reset_request.request_context.request_key,
                                                     RequestReplayStatusDTO::kAcceptedNew,
                                                     state.operation_id,
                                                     prior_revision,
                                                     OperationControlKind::kReset,
                                                     state.operation_revision,
                                                     state.updated_at_ns};
    ASSERT_TRUE(
        Control(journal, reset_request, reset_accepted, state).ok());

    const RuntimeScaleRequestDTO scale_request{
        PublicRequestContextDTO{TestRequestKey("request.scale.1"), "journal-test"},
        state.operation_id, state.operation_revision, RuntimeScaleGeneration(0U), 0.5};
    state.runtime_scale_generation = RuntimeScaleGeneration(1U);
    state.updated_at_ns = 12;
    const RuntimeScaleAcceptedDTO scale_accepted{scale_request.request_context.request_key,
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
            participant.binding_identity.controller.control_binding_id,
            participant.binding_identity.controller.control_binding_revision,
            participant.command_id,
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
    ASSERT_TRUE(WriteOne(journal, JournalRuntimeScaleAccepted{scale_request, scale_accepted,
                                                               state, scale_status})
                    .ok());

    state.operation_revision = OperationRevision(3U);
    state.state = OperationStateKind::kStopping;
    state.participants.front().state = OperationStateKind::kStopping;
    state.stop_event_id = StopEventId(1U);
    state.updated_at_ns = 13;
    auto stop_event = AcceptedStopEvent(1U, state.updated_at_ns);
    stop_event.covered.front().operation_id = state.operation_id;
    const auto stop_row = StopAccepted(journal, stop_event);
    EXPECT_TRUE(stop_row.ok()) << stop_row.error().message;

    state.state = OperationStateKind::kStopped;
    state.participants.front().state = OperationStateKind::kStopped;
    state.updated_at_ns = 14;
    ASSERT_TRUE(
        Observe(journal, state, nullptr, "subordinate_stopped").ok());
    journal.Close();
  }

  // Read the stored rows back (no reopening adopts them): the acceptance, the
  // Stop acceptance and the newest state remain; control and scale rows are
  // coalesced into the newest state, which the snapshot names.
  RawJournal raw(database);
  const auto kinds = raw.Rows("SELECT record_kind FROM journal_records ORDER BY sequence");
  ASSERT_GE(kinds.size(), 3U);
  EXPECT_EQ(kinds.front().front(), std::optional<std::string>("operation_accepted"));
  EXPECT_EQ(kinds.back().front(), std::optional<std::string>("state_observed"));
  EXPECT_EQ(raw.Rows("SELECT COUNT(*) FROM journal_records "
                     "WHERE record_kind='stop_event_accepted' AND stop_event_id=1 "
                     "AND request_key IS NULL AND operation_id IS NULL")
                .front()
                .front(),
            std::optional<std::string>("1"));
  const auto snapshot = raw.Rows(
      "SELECT operation_revision,state,stop_event_id,"
      "runtime_scale_generation,snapshot_sequence=(SELECT MAX(sequence) FROM journal_records) "
      "FROM operation_snapshots WHERE operation_id=13");
  ASSERT_EQ(snapshot.size(), 1U);
  EXPECT_EQ(snapshot.front(),
            (std::vector<std::optional<std::string>>{"3", "stopped", "1", "1", "1"}));
  // The newest state record carries the stored control and scale outcome.
  const auto payload = manyforge::content_identity::parse_canonical_json(
      *raw.Rows("SELECT payload_json FROM journal_records ORDER BY sequence DESC LIMIT 1")
           .front()
           .front());
  EXPECT_EQ(payload.find_object_member("api_version")->as_string(),
            "manyforge.operation_journal_record.v10");
  const auto* operation_state = payload.find_object_member("operation_state");
  ASSERT_NE(operation_state, nullptr);
  EXPECT_EQ(operation_state->find_object_member("operation_revision")->as_number(), 3.0);
  EXPECT_EQ(operation_state->find_object_member("runtime_scale_generation")->as_number(), 1.0);
  EXPECT_EQ(operation_state->find_object_member("stop_event_id")->as_number(), 1.0);
}

TEST(SqliteOperationJournalTest,
     StopEventFailureIsNonThrowingAndNeverConsumesOrdinaryRequestCapacity) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  SqliteOperationJournal journal(database, cell_id, 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 1U).ok());

  // An empty record cannot be encoded: a typed error, no row.
  auto invalid = AcceptedStopEvent(1U, 10);
  invalid.event_id = StopEventId(0U);
  const auto refused = StopAccepted(journal, invalid);
  ASSERT_FALSE(refused.ok());

  const auto recorded = StopAccepted(journal, AcceptedStopEvent(1U, 10));
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  const auto command = CommandIntent("request.command.after-stop-failure");
  const auto state =
      OperationState(16U, OperationStateKind::kExecuting, 20);
  const auto ordinary =
      Accept(journal, command, Acceptance(command, state), state);
  ASSERT_TRUE(ordinary.ok()) << ordinary.error().message;
  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  EXPECT_EQ(page.value().events.size(), 2U);
  journal.Close();
  const auto closed = StopAccepted(journal, AcceptedStopEvent(2U, 30));
  ASSERT_FALSE(closed.ok());
  EXPECT_EQ(closed.error().code, "operation_journal_closed");
}

TEST(SqliteOperationJournalTest, CleanupFaultAndStopProjectionKeepCausalRevisionAtEqualClocks) {
  for (const bool fault_is_newer : {false, true}) {
    for (const bool newer_arrives_first : {false, true}) {
      SCOPED_TRACE(fault_is_newer);
      SCOPED_TRACE(newer_arrives_first);
      ScopedTemporaryDirectory temporary;
      const auto database = temporary.path() / "operation.sqlite3";
      const CellId cell("cell.test");
      auto initial = OperationState(18U, OperationStateKind::kExecuting, 10);
      auto stopped = initial;
      stopped.operation_revision = OperationRevision(fault_is_newer ? 2U : 3U);
      stopped.state = OperationStateKind::kStopped;
      stopped.participants.front().state = OperationStateKind::kStopped;
      stopped.stop_event_id = StopEventId(1003U);
      auto fault = initial;
      fault.operation_revision = OperationRevision(fault_is_newer ? 3U : 2U);
      fault.state = OperationStateKind::kInterventionRequired;
      fault.participants.front().state = OperationStateKind::kInterventionRequired;
      fault.participants.front().terminal_error = ErrorDTO{"cleanup_failed", "cleanup failed", {}};
      if (fault_is_newer) {
        fault.stop_event_id = stopped.stop_event_id;
      } else {
        stopped.participants.front().terminal_error = fault.participants.front().terminal_error;
      }
      const auto& newer = fault_is_newer ? fault : stopped;
      const auto& older = fault_is_newer ? stopped : fault;
      {
        SqliteOperationJournal journal(database, cell, 8U);
        ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
        const auto intent = CommandIntent("request.fault-order");
        ASSERT_TRUE(
            Accept(journal, intent, Acceptance(intent, initial), initial)
                .ok());
        // Legal observations can share both revision and an unchanged clock.
        auto completed = initial;
        completed.state = OperationStateKind::kCompleted;
        completed.participants.front().state = OperationStateKind::kCompleted;
        ASSERT_TRUE(
            Observe(journal, completed, nullptr, "subordinate_completed").ok());
        ASSERT_TRUE(Observe(journal, newer_arrives_first ? newer : older, nullptr,
                            "first_projection")
                        .ok());
        ASSERT_TRUE(Observe(journal, newer_arrives_first ? older : newer, nullptr,
                            "last_projection")
                        .ok());
        const auto events = journal.ReadOperationalEvents(0U, 32U);
        ASSERT_TRUE(events.ok());
        EXPECT_LE(events.value().events.size(), 3U);
        journal.Close();
      }
      // The stored snapshot keeps the causally newer revision whichever
      // arrived last, at equal clocks.
      RawJournal raw(database);
      EXPECT_EQ(raw.Rows("SELECT operation_revision,state FROM operation_snapshots "
                         "WHERE operation_id=18"),
                (std::vector<std::vector<std::optional<std::string>>>{
                    {"3", fault_is_newer ? "intervention_required" : "stopped"}}));
      EXPECT_EQ(raw.Rows("SELECT json_extract(r.payload_json,'$.operation_state.operation_revision')"
                         " FROM operation_snapshots s JOIN journal_records r "
                         "ON r.sequence=s.snapshot_sequence WHERE s.operation_id=18")
                    .front()
                    .front(),
                std::optional<std::string>("3"));
    }
  }
}

TEST(SqliteOperationJournalTest, PrivateCaptureOrderFencesEqualClockAndPublicRevision) {
  for (const bool newest_first : {false, true}) {
    SCOPED_TRACE(newest_first);
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "ordered.sqlite3";
    const CellId cell("cell.test");
    auto initial = OperationState(19U, OperationStateKind::kExecuting, 10);
    initial.journal_capture_order = 1U;
    auto older = initial;
    older.journal_capture_order = 2U;
    older.stop_event_id = StopEventId(1000U);
    older.state = OperationStateKind::kStopping;
    older.participants.front().state = OperationStateKind::kStopping;
    auto newer = older;
    newer.journal_capture_order = 3U;
    newer.state = OperationStateKind::kStopped;
    newer.participants.front().state = OperationStateKind::kStopped;
    {
      SqliteOperationJournal journal(database, cell, 8U);
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
      const auto intent = CommandIntent("request.capture-order");
      ASSERT_TRUE(
          Accept(journal, intent, Acceptance(intent, initial), initial)
              .ok());
      ASSERT_TRUE(
          Observe(journal, newest_first ? newer : older, nullptr, "first_projection").ok());
      ASSERT_TRUE(
          Observe(journal, newest_first ? older : newer, nullptr, "second_projection").ok());
      ASSERT_TRUE(Observe(journal, newer, nullptr,
                          newest_first ? "first_projection" : "second_projection")
                      .ok());
    }
  }
}

TEST(SqliteOperationJournalTest, StopEventRecordsWhileAnEventReaderHoldsItsSnapshot) {
  // An event reader (the history pump) never holds the write lock: a Stop row
  // written while a reader is inside its snapshot still records, and the
  // reader's page stays the snapshot it opened.
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "reader-stop.sqlite3";
  const CellId cell("cell.test");
  SqliteOperationJournal* owner = nullptr;
  std::function<Result<void>()> record_during_read;
  std::optional<Result<void>> recorded_during_read;
  SqliteOperationJournal journal(
      database, cell, 8U,
      [](std::int64_t) { return std::string("2026-10-02T17:58:03.000000001Z"); },
      [&](std::string_view section) {
        if (section == "read_events" && record_during_read && !recorded_during_read) {
          recorded_during_read = record_during_read();
        }
      });
  owner = &journal;
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  auto state = OperationState(25U, OperationStateKind::kExecuting, 10);
  state.journal_capture_order = 1U;
  const auto command = CommandIntent("reader-command");
  ASSERT_TRUE(
      Accept(journal, command, Acceptance(command, state), state).ok());
  record_during_read = [&] { return StopAccepted(*owner, AcceptedStopEvent(1U, 20)); };

  const auto page = journal.ReadOperationalEvents(0U, 8U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_TRUE(recorded_during_read.has_value());
  EXPECT_TRUE(recorded_during_read->ok()) << recorded_during_read->error().message;
  EXPECT_EQ(page.value().events.size(), 1U);
  record_during_read = nullptr;
  const auto after = journal.ReadOperationalEvents(0U, 8U);
  ASSERT_TRUE(after.ok()) << after.error().message;
  ASSERT_EQ(after.value().events.size(), 2U);
  EXPECT_EQ(after.value().events.back().record_kind, "stop_event_accepted");

  // Closing retires the reader: reads fail closed, never on a stale handle.
  journal.Close();
  EXPECT_FALSE(journal.ReadOperationalEvents(0U, 8U).ok());
}

TEST(SqliteOperationJournalTest, BoundedEventPagesRetainStableWallTime) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  std::uint64_t wall_generation = 0U;
  SqliteOperationJournal journal(database, cell_id, 8U, [&wall_generation](std::int64_t) {
    ++wall_generation;
    return "2026-08-07T10:00:00.00000000" + std::to_string(wall_generation) + "Z";
  });
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 4U).ok());
  const auto intent = CommandIntent("request.event.1");
  auto state =
      OperationState(28U, OperationStateKind::kExecuting, 10);
  ASSERT_TRUE(
      Accept(journal, intent, Acceptance(intent, state), state).ok());
  state.state = OperationStateKind::kCompleted;
  state.participants.front().state = OperationStateKind::kCompleted;
  state.participants.front().progress = 1.0;
  state.updated_at_ns = 11;
  ASSERT_TRUE(Observe(journal, state, nullptr, "subordinate_completed").ok());

  const auto first = journal.ReadOperationalEvents(0U, 1U);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_EQ(first.value().events.size(), 1U);
  EXPECT_TRUE(first.value().has_more);
  EXPECT_EQ(first.value().events.front().record_kind, "operation_accepted");
  EXPECT_EQ(first.value().events.front().occurred_at_wall, "2026-08-07T10:00:00.000000001Z");
  EXPECT_EQ(first.value().next_after_sequence, first.value().events.front().sequence);

  const auto second = journal.ReadOperationalEvents(first.value().next_after_sequence, 1U);
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_EQ(second.value().events.size(), 1U);
  EXPECT_FALSE(second.value().has_more);
  EXPECT_EQ(second.value().events.front().record_kind, "state_observed");
  EXPECT_EQ(second.value().events.front().occurred_at_wall, "2026-08-07T10:00:00.000000002Z");
  EXPECT_FALSE(second.value().events.front().payload_digest.empty());

  const auto replay = journal.ReadOperationalEvents(0U, 2U);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_EQ(replay.value().events.size(), 2U);
  EXPECT_EQ(replay.value().events.front().occurred_at_wall,
            first.value().events.front().occurred_at_wall);
  EXPECT_EQ(replay.value().events.back().payload_digest,
            second.value().events.front().payload_digest);
}

// A row's wall time is its action's: the injected clock receives the row's
// steady-clock time.
TEST(SqliteOperationJournalTest, InjectedWallTimeReceivesTheActionTime) {
  ScopedTemporaryDirectory temporary;
  std::vector<std::int64_t> asked;
  SqliteOperationJournal journal(temporary.path() / "wall.sqlite3", CellId("cell.1"), 8U,
                                 [&asked](const std::int64_t occurred_at_ns) {
                                   asked.push_back(occurred_at_ns);
                                   return "2026-08-07T10:00:00." +
                                          std::to_string(1'000'000'000LL + occurred_at_ns)
                                              .substr(1U) +
                                          "Z";
                                 });
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 4U).ok());
  const auto intent = CommandIntent("request.wall.1");
  auto state = OperationState(50U, OperationStateKind::kExecuting, 10);
  auto stopped = state;
  stopped.state = OperationStateKind::kStopped;
  stopped.participants.front().state = OperationStateKind::kStopped;
  stopped.updated_at_ns = 25;
  const auto result = Write(journal, {JournalOperationAccepted{intent, Acceptance(intent, state),
                                                               state},
                                      StopRow(AcceptedStopEvent(1U, 25), false),
                                      StateRecord(stopped, nullptr, "subordinate_stopped")});
  ASSERT_TRUE(result.failures.empty());
  EXPECT_EQ(asked, (std::vector<std::int64_t>{10, 25, 25}));
  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 3U);
  EXPECT_EQ(page.value().events[0].occurred_at_wall, "2026-08-07T10:00:00.000000010Z");
  EXPECT_EQ(page.value().events[1].occurred_at_wall, "2026-08-07T10:00:00.000000025Z");
  EXPECT_EQ(page.value().events[2].occurred_at_wall, page.value().events[1].occurred_at_wall);
}

// Without an injected clock the wall time is derived from the action's
// steady-clock time with one offset per batch: rows of one action carry the
// same text and rows one second apart carry texts one second apart.
TEST(SqliteOperationJournalTest, WallTimeIsDerivedFromTheActionTime) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "derived-wall.sqlite3", CellId("cell.1"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 4U).ok());
  constexpr std::int64_t kAction = 5'000'000'123LL;
  const auto intent = CommandIntent("request.wall.2");
  const auto state = OperationState(51U, OperationStateKind::kExecuting, kAction);
  auto later = state;
  later.updated_at_ns = kAction + 1'000'000'000LL;
  const auto result =
      Write(journal, {JournalOperationAccepted{intent, Acceptance(intent, state), state},
                      StopRow(AcceptedStopEvent(1U, kAction), false),
                      StateRecord(later, nullptr, "subordinate_state_observed")});
  ASSERT_TRUE(result.failures.empty());
  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 3U);
  const auto& accepted = page.value().events[0].occurred_at_wall;
  const auto& stop = page.value().events[1].occurred_at_wall;
  const auto& observed = page.value().events[2].occurred_at_wall;
  EXPECT_EQ(accepted, stop);
  ASSERT_EQ(accepted.size(), 30U);
  ASSERT_EQ(observed.size(), 30U);
  // Same nanoseconds of the second; the whole seconds differ.
  EXPECT_EQ(accepted.substr(19U), observed.substr(19U));
  EXPECT_NE(accepted.substr(0U, 19U), observed.substr(0U, 19U));
}

TEST(SqliteOperationJournalTest, FilesystemIdentityFailsClosedAndRequestCountDoesNotRefuseRows) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  SqliteOperationJournal journal(database, cell_id, 4U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 1U).ok());
  const auto first_intent = CommandIntent("request.command.1");
  const auto first_state =
      OperationState(13U, OperationStateKind::kExecuting, 10);
  ASSERT_TRUE(Accept(journal, first_intent, Acceptance(first_intent, first_state), first_state)
                  .ok());
  // The request ledger's bound is the Kernel's, in memory; the journal
  // records what the Kernel accepted.
  const auto second_intent = CommandIntent("request.command.2");
  const auto second_state =
      OperationState(29U, OperationStateKind::kExecuting, 11);
  const auto second =
      Accept(journal, second_intent, Acceptance(second_intent, second_state), second_state);
  ASSERT_TRUE(second.ok()) << second.error().message;
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

  {
    SqliteOperationJournal journal(database, cell_id, 2U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 2U).ok());
    for (std::uint64_t cycle = 1U; cycle <= 8U; ++cycle) {
      const std::uint64_t operation_id = 3000U + cycle;
      auto state = OperationState(operation_id, OperationStateKind::kExecuting,
                                  static_cast<std::int64_t>(cycle * 10U));
      const auto intent =
          CommandIntent("request.bounded-lifetime." + std::to_string(cycle));
      const auto accepted = Accept(journal, intent, Acceptance(intent, state), state);
      ASSERT_TRUE(accepted.ok()) << "cycle " << cycle << ": " << accepted.error().message;

      for (std::uint64_t update = 1U; update <= 4U; ++update) {
        const OperationControlRequestDTO request{
            PublicRequestContextDTO{TestRequestKey("request.bounded-lifetime.control." +
                                              std::to_string(cycle) + "." + std::to_string(update)), "journal-test"},
            state.operation_id, state.operation_revision, std::nullopt};
        const auto prior_revision = state.operation_revision;
        state.operation_revision = OperationRevision(state.operation_revision.value() + 1U);
        state.state = OperationStateKind::kExecuting;
        state.participants.front().state = state.state;
        ++state.updated_at_ns;
        const OperationControlAcceptedDTO control{
            request.request_context.request_key,
            RequestReplayStatusDTO::kAcceptedNew,
            state.operation_id,
            prior_revision,
            OperationControlKind::kReset,
            state.operation_revision,
            state.updated_at_ns};
        const auto controlled = Control(journal, request, control, state);
        ASSERT_TRUE(controlled.ok())
            << "cycle " << cycle << ", update " << update << ": " << controlled.error().message;
      }

      state.state = OperationStateKind::kCompleted;
      state.participants.front().state = OperationStateKind::kCompleted;
      state.participants.front().progress = 1.0;
      ++state.updated_at_ns;
      const auto completed =
          Observe(journal, state, nullptr, "subordinate_completed");
      ASSERT_TRUE(completed.ok()) << "cycle " << cycle << ": " << completed.error().message;
    }
  }
  // Each acceptance removed the finished operations beyond the bound of two.
  RawJournal raw(database);
  EXPECT_EQ(raw.Rows("SELECT operation_id FROM operation_snapshots ORDER BY operation_id"),
            (std::vector<std::vector<std::optional<std::string>>>{{"3007"}, {"3008"}}));
  EXPECT_EQ(raw.Rows("SELECT COUNT(*) FROM journal_records WHERE operation_id<3007")
                .front()
                .front(),
            std::optional<std::string>("0"));
}

TEST(SqliteOperationJournalTest, ActiveOperationControlUpdatesConsumeOneDurableAdmissionSlot) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  SqliteOperationJournal journal(database, cell_id, 2U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 2U).ok());

  auto first = OperationState(30U,
                              OperationStateKind::kExecuting, 10);
  const auto first_intent = CommandIntent("request.concurrent.first");
  ASSERT_TRUE(Accept(journal, first_intent, Acceptance(first_intent, first), first).ok());

  for (std::uint64_t update = 1U; update <= 4U; ++update) {
    const OperationControlRequestDTO request{
        PublicRequestContextDTO{
            TestRequestKey("request.concurrent.first.control." + std::to_string(update)),
            "journal-test"},
        first.operation_id, first.operation_revision, std::nullopt};
    const auto prior_revision = first.operation_revision;
    first.operation_revision = OperationRevision(first.operation_revision.value() + 1U);
    first.state = OperationStateKind::kExecuting;
    first.participants.front().state = first.state;
    ++first.updated_at_ns;
    const OperationControlAcceptedDTO accepted{
        request.request_context.request_key,
        RequestReplayStatusDTO::kAcceptedNew,
        first.operation_id,
        prior_revision,
        OperationControlKind::kReset,
        first.operation_revision,
        first.updated_at_ns};
    ASSERT_TRUE(Control(journal, request, accepted, first).ok());
  }

  auto second = OperationState(31U,
                               OperationStateKind::kExecuting, 20);
  const auto second_intent = CommandIntent("request.concurrent.second");
  const auto second_accepted =
      Accept(journal, second_intent, Acceptance(second_intent, second), second);
  ASSERT_TRUE(second_accepted.ok()) << second_accepted.error().message;

  first.state = OperationStateKind::kCompleted;
  first.participants.front().state = OperationStateKind::kCompleted;
  first.participants.front().progress = 1.0;
  ++first.updated_at_ns;
  ASSERT_TRUE(Observe(journal, first, nullptr, "subordinate_completed").ok());
  second.state = OperationStateKind::kCompleted;
  second.participants.front().state = OperationStateKind::kCompleted;
  second.participants.front().progress = 1.0;
  ++second.updated_at_ns;
  ASSERT_TRUE(Observe(journal, second, nullptr, "subordinate_completed").ok());
}

TEST(SqliteOperationJournalTest, CapturedScopeAndFailureAreRecorded) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "retry.sqlite3";
  const CellId cell("cell.1");
  auto intent = CommandIntent("retry.context");
  intent.participants.front().required_capability_id = CapabilityId("execution.trajectory.execute");
  intent.participants.front().payload = MotionParticipantIntentDTO{
      MoveId("move"),
      {ControllableComponentId("robot.1"), FrameRoleId("base")},
      CurrentTcpDTO{},
      TolerancesDTO{},
      SpeedProfileDTO{},
      TimeoutsDTO{},
      CacheMode::kDefault,
      PlannerOverrideDTO{},
      MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      JointGoalDTO{{"joint1"}, {0.25}},
      std::nullopt,
      {},
      {}};
  auto state = PreparedMotionState(32U, 10);
  state.journal_capture_order = 1U;
  state.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("move"), NodeAttemptNumber(1U)};
  intent.participants.front().execution_scope = state.participants.front().execution_scope;
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 4U).ok());
    ASSERT_TRUE(
        Accept(journal, intent, Acceptance(intent, state), state).ok());
    state.journal_capture_order = 2U;
    state.updated_at_ns = 12;
    state.state = OperationStateKind::kFailed;
    state.participants.front().state = OperationStateKind::kFailed;
    state.participants.front().terminal_error = ErrorDTO{"planning_failed", "transient", {}};
    ASSERT_TRUE(Observe(journal, state, nullptr, "planning-failure").ok());
    const auto events = journal.ReadOperationalEvents(0U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 2U);
    EXPECT_NE(events.value().events.front().payload_json.find(
                  "manyforge.operation_journal_record.v10"), std::string::npos);
    EXPECT_EQ(events.value().events.front().payload_json.find("automatic_retry_context"),
              std::string::npos);
    EXPECT_NE(events.value().events.back().payload_json.find("planning_failed"),
              std::string::npos);
    journal.Close();
  }
}

TEST(SqliteOperationJournalTest, ExactParticipantFactsSurviveCaptureWithoutColumns) {
  for (const auto effect : {std::optional<ParticipantEffectKnowledge>{},
                            std::optional(ParticipantEffectKnowledge::kNoEffect),
                            std::optional(ParticipantEffectKnowledge::kEffectSettled),
                            std::optional(ParticipantEffectKnowledge::kEffectUncertain)}) {
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "participant-fact.sqlite3";
    auto initial = OperationState(33U, OperationStateKind::kExecuting, 10);
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
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
      const auto intent = CommandIntent("request.fact");
      ASSERT_TRUE(
          Accept(journal, intent, Acceptance(intent, initial), initial)
              .ok());
      ASSERT_TRUE(Observe(journal, failed, nullptr, "exact_failure").ok());
      const auto events = journal.ReadOperationalEvents(0U, 8U);
      ASSERT_TRUE(events.ok());
      const auto& bytes = events.value().events.back().payload_json;
      EXPECT_EQ(bytes.find("terminal_fact") != std::string::npos, effect.has_value());
      if (effect) {
        EXPECT_NE(bytes.find("manyforge.operation_journal_record.v10"), std::string::npos);
      }
      if (effect) {
        auto mismatch = failed;
        mismatch.journal_capture_order = 3U;
        mismatch.participants.front().terminal_error =
            ErrorDTO{"resource_slot_state_invalid", "selected resource transition failed", {}};
        ASSERT_TRUE(Observe(journal, mismatch, nullptr, "mismatched_fact").ok());
        failed = mismatch;
        mismatch.journal_capture_order = 4U;
        mismatch.participants.front().state = OperationStateKind::kExecuting;
        EXPECT_FALSE(
            Observe(journal, mismatch, nullptr, "nonterminal_fact").ok());
      }
    }
  }
}

TEST(SqliteOperationJournalTest, LogicalPreemptionPersistsWithoutClaimingPhysicalCompletion) {
  for (const auto kind : {OperationPreemptionKind::kPreempted,
                          OperationPreemptionKind::kSupersededUnsent}) {
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "preemption.sqlite3";
    auto initial = OperationState(35U, OperationStateKind::kExecuting, 10);
    initial.journal_capture_order = 1U;
    initial.participants.front().execution_scope =
        ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("arm.node"), NodeAttemptNumber(1U)};
    auto preempted = initial;
    preempted.journal_capture_order = 2U;
    preempted.preemption = OperationPreemptionDTO{
        kind, {OperationNumber(1001U), ProgramNodeInstanceId("winner.node"), NodeAttemptNumber(2U)}};
    {
      SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
      auto intent = CommandIntent("request.preemption");
      intent.participants.front().execution_scope = initial.participants.front().execution_scope;
      ASSERT_TRUE(Accept(journal, intent, Acceptance(intent, initial), initial)
                      .ok());
      ASSERT_TRUE(Observe(journal, preempted, nullptr, "motion_preempted").ok());
      const auto events = journal.ReadOperationalEvents(0U, 8U);
      ASSERT_TRUE(events.ok());
      EXPECT_NE(events.value().events.back().payload_json.find(
                    "manyforge.operation_journal_record.v10"), std::string::npos);
      EXPECT_EQ(preempted.participants.front().state, OperationStateKind::kExecuting);
      EXPECT_FALSE(preempted.participants.front().terminal_fact);
    }
  }
}

IoRuleStateChangeDTO GlobalRuleChange(IoGlobalRuleState state, TimePoint at = 1) {
  return {{UINT64_MAX, ActiveBundlePublicationGeneration(3U),
           RetainedBundleId("retained.global"), GraphRevision("graph.global")},
          {InterruptPolicyId("door.guard"), state, std::nullopt},
          at};
}

TEST(SqliteOperationJournalTest, InterruptRuleStateUsesExistingOrderedOperationalStreamAndReopens) {
  ScopedTemporaryDirectory directory;
  const auto database = directory.path() / "rule-state.sqlite3";
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 0, 8U).ok());
    auto change = GlobalRuleChange(IoGlobalRuleState::kInactive);
    ASSERT_TRUE(RuleChanged(journal, change).ok());
    change.rule.state = IoGlobalRuleState::kTripped;
    change.rule.triggering_acquisition =
        IoRuleAcquisitionKeyDTO{InterruptSourceId("door.source"),
                                IoSourceIncarnation("door.epoch"),
                                UINT64_MAX,
                                {UINT64_MAX, InterruptSourceId("door.source"), UINT64_MAX}};
    change.occurred_at_ns = 2;
    ASSERT_TRUE(RuleChanged(journal, change).ok());
    const auto events = journal.ReadOperationalEvents(0U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 2U);
    first_sequence = events.value().events.front().sequence;
    last_sequence = events.value().events.back().sequence;
    EXPECT_GT(last_sequence, first_sequence);
    const auto& event = events.value().events.back();
    EXPECT_EQ(event.record_kind, "interrupt_rule_state_changed");
    EXPECT_FALSE(event.operation_id);
    EXPECT_FALSE(event.request_key);
    EXPECT_EQ(event.occurred_at_ns, 2);
    const auto payload = manyforge::content_identity::parse_canonical_json(event.payload_json);
    ASSERT_EQ(payload.as_object().size(), 7U);
    EXPECT_EQ(payload.find_object_member("catalog_generation")->as_string(),
              std::to_string(UINT64_MAX));
    EXPECT_EQ(payload.find_object_member("active_publication_generation")->as_number(), 3.0);
    EXPECT_EQ(payload.find_object_member("state")->as_string(), "tripped");
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
  const auto replay = reopened.ReadOperationalEvents(first_sequence);
  ASSERT_TRUE(replay.ok());
  ASSERT_EQ(replay.value().events.size(), 1U);
  EXPECT_EQ(replay.value().events.front().sequence, last_sequence);
  EXPECT_EQ(replay.value().next_after_sequence, last_sequence);
}

// Active TCP: one confirmed change is an operational
// `active_tcp_changed` row (the Runner's mirror and History read it) with
// robot, previous and new entry (null: none), source, offset sent,
// registration generation and sequence; an invalid change consumes no
// sequence. It is a batch record of the journal writer like every other row
// (OD-220: posted to the queue, never written directly).
TEST(SqliteOperationJournalTest, ActiveTcpChangeIsAnOperationalRow) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "active-tcp.sqlite3", CellId("cell.test"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 0, 8U).ok());
  const TcpEntryReferenceDTO tool{{ControllableComponentId("robot"), CommandTargetId("arm")},
                                  {ControllableComponentId("gripper"), FrameRoleId("tcp")}};
  ActiveTcpChangeDTO change{ControllableComponentId("robot"), std::nullopt, tool,
                            ActiveTcpChangeSource::kRobotControl, true, 2U, 3U, 7};
  const auto recorded = WriteOne(journal, change);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  auto invalid = change;
  invalid.registration_generation = 0U;
  const auto refused = WriteOne(journal, invalid);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "operation_journal_record_invalid");
  auto next = change;
  next.sequence = 4U;
  ASSERT_TRUE(WriteOne(journal, next).ok());
  const auto events = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(events.ok());
  ASSERT_EQ(events.value().events.size(), 2U);
  const auto& event = events.value().events.front();
  EXPECT_EQ(events.value().events.back().sequence, event.sequence + 1U);
  EXPECT_EQ(event.record_kind, "active_tcp_changed");
  EXPECT_FALSE(event.operation_id);
  EXPECT_FALSE(event.request_key);
  EXPECT_EQ(event.occurred_at_ns, 7);
  const auto payload = manyforge::content_identity::parse_canonical_json(event.payload_json);
  EXPECT_EQ(payload.find_object_member("api_version")->as_string(),
            "manyforge.active_tcp_change.v3");
  EXPECT_EQ(payload.find_object_member("component_id")->as_string(), "robot");
  EXPECT_TRUE(payload.find_object_member("previous")->is_null());
  EXPECT_EQ(payload.find_object_member("entry")->find_object_member("frame")->find_object_member("component_id")->as_string(),
            "gripper");
  EXPECT_EQ(payload.find_object_member("source")->as_string(), "robot_control");
  EXPECT_TRUE(payload.find_object_member("offset_sent")->as_bool());
  EXPECT_EQ(payload.find_object_member("owner_epoch"), nullptr);
  EXPECT_EQ(payload.find_object_member("registration_generation")->as_number(), 2.0);
  EXPECT_EQ(payload.find_object_member("sequence")->as_number(), 3.0);
}

TEST(SqliteOperationJournalTest, NotifyUnknownRetainsTypedUnavailableReason) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "notify.sqlite3", CellId("cell.test"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 0, 8U).ok());
  auto change = GlobalRuleChange(IoGlobalRuleState::kUnknown);
  change.rule.unavailable_reason = ErrorDTO{"io_watch_bound_exceeded", "watch unavailable", {{"limit", "4"}}};
  ASSERT_TRUE(RuleChanged(journal, change).ok());
  const auto events = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(events.ok());
  ASSERT_EQ(events.value().events.size(), 1U);
  const auto payload = manyforge::content_identity::parse_canonical_json(events.value().events[0].payload_json);
  EXPECT_EQ(payload.find_object_member("state")->as_string(), "unknown");
  EXPECT_TRUE(payload.find_object_member("triggering_acquisition")->is_null());
  const auto &reason = *payload.find_object_member("unavailable_reason");
  EXPECT_EQ(reason.find_object_member("code")->as_string(), "io_watch_bound_exceeded");
  EXPECT_EQ(reason.find_object_member("details")->find_object_member("limit")->as_string(), "4");
  change.rule.unavailable_reason.reset();
  EXPECT_FALSE(RuleChanged(journal, change).ok());
}

TEST(SqliteOperationJournalTest, InterruptRuleStateRefusesInvalidEvidenceWithoutConsumingSequence) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "rule-invalid.sqlite3", CellId("cell.test"),
                                 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 0, 8U).ok());
  auto change = GlobalRuleChange(IoGlobalRuleState::kClear);
  ASSERT_TRUE(RuleChanged(journal, change).ok());
  change.rule.triggering_acquisition =
      IoRuleAcquisitionKeyDTO{InterruptSourceId("door.source"),
                              IoSourceIncarnation("door.epoch"),
                              1U,
                              {UINT64_MAX, InterruptSourceId("door.source"), 2U}};
  EXPECT_FALSE(RuleChanged(journal, change).ok());  // No fabricated clear cause.
  change.rule.state = static_cast<IoGlobalRuleState>(255);
  EXPECT_FALSE(RuleChanged(journal, change).ok());
  change.rule.state = IoGlobalRuleState::kTripped;
  change.rule.triggering_acquisition->source_cursor.catalog_generation = 1U;
  EXPECT_FALSE(RuleChanged(journal, change).ok());
  change.rule.triggering_acquisition.reset();
  change.context.active_publication_generation = ActiveBundlePublicationGeneration(0U);
  EXPECT_FALSE(RuleChanged(journal, change).ok());
  change.context.active_publication_generation = ActiveBundlePublicationGeneration(3U);
  change.occurred_at_ns = -1;
  EXPECT_FALSE(RuleChanged(journal, change).ok());
  change.occurred_at_ns = 2;
  ASSERT_TRUE(RuleChanged(journal, change).ok());
  const auto all = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all.value().events.size(), 2U);
  EXPECT_EQ(all.value().events.back().sequence, all.value().events.front().sequence + 1U);
}

TEST(SqliteOperationJournalTest, InterruptRuleStateUsesBoundedHistory) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "rule-history.sqlite3", CellId("cell.test"),
                                 2U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 0, 2U).ok());
  for (std::uint64_t index = 0U; index < 5U; ++index) {
    ASSERT_TRUE(RuleChanged(journal, GlobalRuleChange(index % 2U ? IoGlobalRuleState::kClear
                                                                  : IoGlobalRuleState::kTripped,
                                                      static_cast<TimePoint>(index)))
                    .ok());
  }
  // A fresh journal numbers its rows from 1: the newest two are 4 and 5.
  const auto retained = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(retained.ok());
  ASSERT_EQ(retained.value().events.size(), 2U);
  EXPECT_EQ(retained.value().events.front().sequence, 4U);
  EXPECT_EQ(retained.value().events.back().sequence, 5U);
}

TEST(SqliteOperationJournalTest, InterruptRuleStateBaselineRequiresCanonicalActualContextRows) {
  const auto change =
      GlobalRuleChange(IoGlobalRuleState::kClear);
  IoRuleStateBaselineDTO baseline{change.context, {change.rule}};
  EXPECT_FALSE(io_rule_state_baseline_field_error(baseline));
  baseline.states.push_back(change.rule);
  EXPECT_EQ(io_rule_state_baseline_field_error(baseline), "io_rule_state_rows_not_canonical");
  baseline.states.back().rule_id = InterruptPolicyId("aaa.first");
  EXPECT_EQ(io_rule_state_baseline_field_error(baseline), "io_rule_state_rows_not_canonical");
  baseline.states.clear();
  EXPECT_FALSE(io_rule_state_baseline_field_error(baseline));  // Actual qualified empty catalog.
}

}  // namespace

namespace {
TEST(SqliteOperationJournalTest, PreemptedWithoutStopRetainsFactAndSelectedCause) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "preempted-fact.sqlite3";
  auto state = OperationState(36U, OperationStateKind::kInterventionRequired, 10);
  state.journal_capture_order = 1U;
  const ErrorDTO selected{
      "success", "unexpected subordinate terminal", {{"reason", "subordinate_terminal_error"}}};
  const ParticipantExecutionFact reported{ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                                          ParticipantEffectKnowledge::kEffectSettled, std::nullopt};
  state.participants.front().terminal_error = selected;
  state.participants.front().terminal_fact = reported;
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    const auto intent = CommandIntent("request.preempted");
    ASSERT_TRUE(
        Accept(journal, intent, Acceptance(intent, state), state).ok());
    const auto events = journal.ReadOperationalEvents(0U, 8U);
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
}

}  // namespace

namespace {
TEST(SqliteOperationJournalTest, ControlAckKeepsHistoricalRuleOrigin) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "historical-rule-ack.sqlite3";
  const CellId cell("cell.rule.ack");
  auto state = OperationState(38U, OperationStateKind::kCompleted, 10);
  state.participants.front().io_rule_outcomes = {rule_codec_test::Outcome(state)};
  const auto original = state.participants.front().io_rule_outcomes;
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    const auto intent = CommandIntent("request.rule.initial");
    ASSERT_TRUE(Accept(journal, intent, Acceptance(intent, state), state).ok());
    const OperationControlRequestDTO request{
        PublicRequestContextDTO{TestRequestKey("request.rule.ack"), "operator.fixture"},
        state.operation_id, state.operation_revision, std::nullopt};
    const auto before = state.operation_revision;
    state.operation_revision = OperationRevision(before.value() + 1U);
    ++state.updated_at_ns;
    // Typed accepted-control persistence fixture: no new Kernel ACK authority.
    const OperationControlAcceptedDTO accepted{
        request.request_context.request_key, RequestReplayStatusDTO::kAcceptedNew,
        state.operation_id, before, OperationControlKind::kAcknowledgeCompleted,
        state.operation_revision, state.updated_at_ns};
    const auto stored = Control(journal, request, accepted, state);
    ASSERT_TRUE(stored.ok()) << stored.error().message;
    const auto events = journal.ReadOperationalEvents(0U, 8U);
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
}
TEST(SqliteOperationJournalTest, CommonCommandIdentityUsesOneNeutralStoredShape) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "neutral-command.sqlite3";
  const CellId cell("cell.command");
  const auto state = OperationState(39U, OperationStateKind::kCompleted, 10);
  {
    SqliteOperationJournal journal(database, cell, 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    const auto intent = CommandIntent("request.command");
    ASSERT_TRUE(Accept(journal, intent, Acceptance(intent, state), state).ok());
    const auto events = journal.ReadOperationalEvents(0U);
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events.value().events.size(), 1U);
    const auto& event = events.value().events.front();
    EXPECT_EQ(event.payload_digest,
              manyforge::content_identity::sha256_digest(event.payload_json));
    const auto payload = manyforge::content_identity::parse_canonical_json(event.payload_json);
    EXPECT_EQ(payload.find_object_member("api_version")->as_string(),
              "manyforge.operation_journal_record.v10");
    const auto& participant = payload.find_object_member("operation_state")
                                  ->find_object_member("participants")->as_array().front();
    ASSERT_NE(participant.find_object_member("command_id"), nullptr);
    EXPECT_EQ(*participant.find_object_member("command_id"),
              ControllerGoalIdValue(state.participants.front().command_id));
    EXPECT_EQ(*participant.find_object_member("participant_id"),
              OperationParticipantIdValue(state.participants.front().participant_id));
    EXPECT_EQ(participant.find_object_member("command_generation"), nullptr);
    EXPECT_EQ(participant.find_object_member("controller_goal_id"), nullptr);
    EXPECT_EQ(participant.find_object_member("controller_goal_generation"), nullptr);
  }
}
TEST(SqliteOperationJournalTest, RuleEvidenceSurvivesRealJournalCoalescing) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "rule-evidence.sqlite3";
  auto state = OperationState(40U, OperationStateKind::kStopped, 10);
  state.participants.front().io_rule_outcomes.push_back(rule_codec_test::Outcome(state));
  {
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    const auto intent = CommandIntent("request.rules");
    ASSERT_TRUE(Accept(journal, intent, Acceptance(intent, state), state).ok());
    for (auto disposition : {IoRuleArbitrationDispositionDTO::kQueued, IoRuleArbitrationDispositionDTO::kSuperseded}) {
      state.participants.front().io_rule_outcomes.front().arbitration_disposition = disposition;
      state.updated_at_ns += 1;
      ASSERT_TRUE(Observe(journal, state, nullptr, "rule_evidence_observed").ok());
    }
    const auto events = journal.ReadOperationalEvents(0U, 8U);
    ASSERT_TRUE(events.ok()); ASSERT_EQ(events.value().events.size(), 2U);
    const auto payload = manyforge::content_identity::parse_canonical_json(events.value().events.back().payload_json);
    EXPECT_EQ(payload.find_object_member("api_version")->as_string(), "manyforge.operation_journal_record.v10");
  }
  // After coalescing, the one stored current record still carries the newest
  // rule evidence exactly; read back from the row, not through a reopen.
  RawJournal raw(database);
  const auto rows = raw.Rows(
      "SELECT json_extract(r.payload_json,'$.operation_state.participants[0].io_rule_outcomes'),"
      "r.record_kind FROM operation_snapshots s JOIN journal_records r "
      "ON r.sequence=s.snapshot_sequence WHERE s.operation_id=40");
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().back(), std::optional<std::string>("state_observed"));
  ASSERT_TRUE(rows.front().front().has_value());
  const auto stored = DecodeIoRuleOutcomes(
      manyforge::content_identity::canonical_json(
          manyforge::content_identity::parse_canonical_json(*rows.front().front())));
  ASSERT_TRUE(stored.ok()) << stored.error().message;
  EXPECT_EQ(stored.value(), state.participants.front().io_rule_outcomes);
  EXPECT_EQ(raw.Rows("SELECT COUNT(*) FROM journal_records WHERE operation_id=40 "
                     "AND record_kind='state_observed'")
                .front()
                .front(),
            std::optional<std::string>("1"));
}
}  // namespace

namespace {
TEST(SqliteOperationJournalTest, TakeoverDecisionSurvivesCoalescingWithExactCause) {
  for (const auto path :
       {MotionTakeoverPathDTO::kMovingReplacement, MotionTakeoverPathDTO::kSpringBackHold,
        MotionTakeoverPathDTO::kOrdinaryContainment}) {
    ScopedTemporaryDirectory directory;
    const auto database = directory.path() / "takeover-history.sqlite3";
    auto state = OperationState(41U, OperationStateKind::kExecuting, 10);
    state.journal_capture_order = 1U;
    const ErrorDTO cause{"takeover_capture_failed",
                         "exact smooth refusal",
                         {{"expected_revision", "7"}, {"observed_revision", "8"}}};
    {
      SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
      const auto intent = CommandIntent("request.winner");
      ASSERT_TRUE(
          Accept(journal, intent, Acceptance(intent, state), state).ok());
      state.motion_takeover = MotionTakeoverDecisionDTO{OperationNumber(1002U),
                                                        path,
                                                        std::nullopt,
                                                        30,
                                                        20,
                                                        500'000'000,
                                                        std::nullopt,
                                                        400'000'000};
      if (path != MotionTakeoverPathDTO::kMovingReplacement) {
        state.motion_takeover->cause = cause;
        state.motion_takeover->stop_event_id = StopEventId(1002U);
      }
      for (const auto at : {30, 40}) {
        state.updated_at_ns = at;
        ++state.journal_capture_order;
        if (at == 40) {
          state.state = OperationStateKind::kCompleted;
          state.participants.front().state = OperationStateKind::kCompleted;
        }
        ASSERT_TRUE(Observe(journal, state, nullptr, "takeover_observed").ok());
      }
      const auto events = journal.ReadOperationalEvents(0U, 8U);
      ASSERT_TRUE(events.ok());
      ASSERT_EQ(events.value().events.size(), 2U);
      const auto accepted = manyforge::content_identity::parse_canonical_json(
          events.value().events.front().payload_json);
      EXPECT_EQ(
          accepted.find_object_member("operation_state")->find_object_member("motion_takeover"),
          nullptr);
      const auto latest = manyforge::content_identity::parse_canonical_json(
          events.value().events.back().payload_json);
      const auto* decision =
          latest.find_object_member("operation_state")->find_object_member("motion_takeover");
      ASSERT_NE(decision, nullptr);
      EXPECT_EQ(decision->find_object_member("path")->as_string(), to_string(path));
      EXPECT_EQ(decision->find_object_member("planning_budget_ns")->as_number(), 400'000'000.0);
      EXPECT_EQ(decision->find_object_member("cause")->is_null(),
                path == MotionTakeoverPathDTO::kMovingReplacement);
      if (path != MotionTakeoverPathDTO::kMovingReplacement) {
        EXPECT_EQ(decision->find_object_member("cause")
                      ->find_object_member("details")
                      ->find_object_member("observed_revision")
                      ->as_string(),
                  "8");
        EXPECT_EQ(decision->find_object_member("stop_event_id")->as_number(), 1002.0);
      } else {
        EXPECT_TRUE(decision->find_object_member("stop_event_id")->is_null());
      }
    }
  }
}

}  // namespace

namespace {
TEST(SqliteOperationJournalTest, InvalidTakeoverMetadataDoesNotAppendARecord) {
  ScopedTemporaryDirectory directory;
  SqliteOperationJournal journal(directory.path() / "takeover-invalid.sqlite3", CellId("cell.test"),
                                 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  auto state = OperationState(44U, OperationStateKind::kExecuting, 10);
  const auto intent = CommandIntent("request.invalid");
  ASSERT_TRUE(
      Accept(journal, intent, Acceptance(intent, state), state).ok());
  for (const auto invalidity : {0, 1, 2, 3, 4}) {
    state.motion_takeover = MotionTakeoverDecisionDTO{OperationNumber(1003U),
                                                      MotionTakeoverPathDTO::kSpringBackHold,
                                                      ErrorDTO{"smooth_failed", "exact reason", {}},
                                                      30,
                                                      20,
                                                      500'000'000,
                                                      std::nullopt,
                                                      400'000'000};
    if (invalidity == 0) state.motion_takeover->preparation_budget_ns = 0;
    if (invalidity == 1) state.motion_takeover->elapsed_since_acceptance_ns = 31;
    if (invalidity == 2) state.motion_takeover->incumbent_operation_id = state.operation_id;
    if (invalidity == 3) state.motion_takeover->cause.reset();
    if (invalidity == 4) state.motion_takeover->planning_budget_ns = -1;
    const auto rejected = Observe(journal, state, nullptr, "invalid_takeover");
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "operation_journal_record_invalid");
    const auto events = journal.ReadOperationalEvents(0U, 8U);
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events.value().events.size(), 1U);
  }
}

TEST(SqliteOperationJournalTest, AcceptedMotionRecordsItsRequiredOwnerQualifiedTcp) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "endpoint.sqlite3", CellId("cell.1"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  for (int selected = 0; selected < 3; ++selected) {
    auto intent = CommandIntent("endpoint-" + std::to_string(selected));
    MotionParticipantIntentDTO motion{
        MoveId("move.endpoint"),
        {ControllableComponentId("robot.1"), FrameRoleId("base")},
        CurrentTcpDTO{},
        TolerancesDTO{0.01, 0.001, 0.01},
        SpeedProfileDTO{},
        TimeoutsDTO{},
        CacheMode::kDefault,
        PlannerOverrideDTO{},
        MotionTargetModeDTO::kAbsolute,
        std::nullopt,
        std::nullopt,
        JointGoalDTO{{"joint"}, {0.2}},
        std::nullopt,
        {},
        {}};
    if (selected) {
      motion.tcp_endpoint = TcpEntryReferenceDTO{
          {ControllableComponentId("robot.1"), CommandTargetId("arm")},
          {ControllableComponentId("lite_gripper"), FrameRoleId(selected == 1 ? "left_tip" : "right_tip")}};
    }
    intent.participants.front().payload = motion;
    const auto state = OperationState(4000U + static_cast<std::uint64_t>(selected),
                                      OperationStateKind::kExecuting, 10 + selected);
    ASSERT_TRUE(
        Accept(journal, intent, Acceptance(intent, state), state).ok());
  }
  const auto events = journal.ReadOperationalEvents(0U, 8U);
  ASSERT_TRUE(events.ok()) << events.error().message;
  ASSERT_EQ(events.value().events.size(), 3U);
  const auto& own = events.value().events[0].payload_json;
  EXPECT_NE(own.find("\"tcp_endpoint\":{\"kind\":\"current\"}"), std::string::npos);
  EXPECT_EQ(own.find("tcp_frame_role_id"), std::string::npos);
  for (std::size_t index = 1; index < 3; ++index) {
    const auto& bytes = events.value().events[index].payload_json;
    const std::string expected =
        std::string("\"tcp_endpoint\":{\"frame\":{\"component_id\":\"lite_gripper\","
                    "\"frame_id\":\"") +
        (index == 1 ? "left_tip" : "right_tip") +
        "\"},\"kind\":\"entry\",\"target\":{\"command_target_id\":\"arm\",\"component_id\":\"robot.1\"}}";
    EXPECT_NE(bytes.find(expected), std::string::npos);
    EXPECT_EQ(bytes.find("tcp_frame_role_id"), std::string::npos);
  }
  EXPECT_NE(events.value().events[1].payload_digest, events.value().events[2].payload_digest);
}

}  // namespace

namespace {

TEST(SqliteOperationJournalTest, FailedPreflightPathLookupRefusesStorageWithoutOpening) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  std::filesystem::create_symlink(database.filename(), database);
  try {
    SqliteOperationJournal refused(database, CellId("cell.1"), 8U);
    ADD_FAILURE() << "a cyclic journal path was opened";
  } catch (const TypedError& error) {
    EXPECT_EQ(error.code(), "operation_journal_path_invalid");
  }
  EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(database)));
}

// Stop event rows (stop state model §7.1, contract §5.11): one acceptance and
// one completion row per event, payload `manyforge.stop_event.v1`.
TEST(SqliteOperationJournalTest, StopEventRowsCarryTheStopEventPayload) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "stop-event.sqlite3", CellId("cell.1"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  const auto accepted = AcceptedStopEvent(1U, 100);
  const auto completed = CompletedStopEvent(accepted, 250);
  const auto accepted_row = StopAccepted(journal, accepted);
  ASSERT_TRUE(accepted_row.ok()) << accepted_row.error().message;
  const auto completed_row = StopCompleted(journal, completed);
  ASSERT_TRUE(completed_row.ok()) << completed_row.error().message;

  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 2U);
  const auto& acceptance = page.value().events.front();
  const auto& completion = page.value().events.back();
  EXPECT_LT(acceptance.sequence, completion.sequence);
  EXPECT_EQ(acceptance.record_kind, "stop_event_accepted");
  EXPECT_EQ(completion.record_kind, "stop_event_completed");
  for (const auto* row : {&acceptance, &completion}) {
    EXPECT_FALSE(row->request_key.has_value());
    EXPECT_FALSE(row->operation_id.has_value());
    EXPECT_FALSE(row->operation_revision.has_value());
  }
  EXPECT_EQ(acceptance.occurred_at_ns, 100);
  EXPECT_EQ(completion.occurred_at_ns, 250);
  EXPECT_EQ(acceptance.payload_json, EncodeStopEventAccepted(accepted));
  EXPECT_EQ(completion.payload_json, EncodeStopEventCompleted(completed));

  const auto accepted_payload =
      manyforge::content_identity::parse_canonical_json(acceptance.payload_json);
  EXPECT_EQ(accepted_payload.find_object_member("schema_version")->as_string(),
            "manyforge.stop_event.v1");
  EXPECT_EQ(accepted_payload.find_object_member("row")->as_string(), "accepted");
  EXPECT_EQ(accepted_payload.find_object_member("event_id")->as_number(), 1.0);
  EXPECT_EQ(accepted_payload.find_object_member("requested")->as_string(), "stop_all");
  EXPECT_EQ(accepted_payload.find_object_member("attribution")
                ->find_object_member("principal_id")
                ->as_string(),
            "operator.1");
  ASSERT_EQ(accepted_payload.find_object_member("covered")->as_array().size(), 1U);

  const auto completed_payload =
      manyforge::content_identity::parse_canonical_json(completion.payload_json);
  EXPECT_EQ(completed_payload.find_object_member("schema_version")->as_string(),
            "manyforge.stop_event.v1");
  EXPECT_EQ(completed_payload.find_object_member("row")->as_string(), "completed");
  EXPECT_EQ(completed_payload.find_object_member("event_id")->as_number(), 1.0);
  EXPECT_EQ(completed_payload.find_object_member("result")->as_string(), "stopped");
  const auto& covered = completed_payload.find_object_member("covered")->as_array();
  ASSERT_EQ(covered.size(), 1U);
  EXPECT_EQ(covered.front().find_object_member("phase")->as_string(), "stopped");
  EXPECT_EQ(covered.front().find_object_member("containing_stage_index")->as_number(), 0.0);
  EXPECT_EQ(covered.front().find_object_member("containing_stage_kind")->as_string(), "nominal");
}

// Build spec section 6 ("Kernel journal Stop rows"): after every Stop row the
// writer keeps both rows of the newest 64 finished events and drops older
// finished ones.
TEST(SqliteOperationJournalTest, FinishedStopEventsKeepTheNewest64) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  {
    SqliteOperationJournal journal(database, CellId("cell.1"), 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    // One writer batch: the same rule applies after every Stop row.
    std::vector<JournalRecordPayload> rows;
    for (std::uint64_t event = 1U; event <= 100U; ++event) {
      const auto accepted = AcceptedStopEvent(event, static_cast<std::int64_t>(event) * 10);
      rows.push_back(StopRow(accepted, false));
      rows.push_back(StopRow(
          CompletedStopEvent(accepted, static_cast<std::int64_t>(event) * 10 + 5), true));
    }
    ASSERT_TRUE(Write(journal, std::move(rows)).failures.empty());
  }
  RawJournal raw(database);
  EXPECT_EQ(raw.Rows("SELECT COUNT(*),COUNT(DISTINCT stop_event_id),MIN(stop_event_id),"
                     "MAX(stop_event_id),SUM(record_kind='stop_event_completed') "
                     "FROM journal_records WHERE stop_event_id IS NOT NULL"),
            (std::vector<std::vector<std::optional<std::string>>>{
                {"128", "64", "37", "100", "64"}}));
}

// An event is also finished once more than request_capacity (the Kernel's
// event ring size) newer events exist: its completion row can never come.
// With capacity 8 and 100 events none completed, after the last row events
// 93..100 are unfinished and 1..92 finished; the newest 64 finished (29..92)
// keep their row, so ids 29..100 remain: 72 acceptance rows.
TEST(SqliteOperationJournalTest, AcceptanceWithoutCompletionIsDroppedOnceTheRingOverwroteIt) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  {
    SqliteOperationJournal journal(database, CellId("cell.1"), 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    for (std::uint64_t event = 1U; event <= 100U; ++event) {
      ASSERT_TRUE(
          StopAccepted(journal, AcceptedStopEvent(event, static_cast<std::int64_t>(event) * 10))
              .ok())
          << event;
    }
  }
  RawJournal raw(database);
  EXPECT_EQ(raw.Rows("SELECT COUNT(*),MIN(stop_event_id),MAX(stop_event_id),"
                     "SUM(record_kind='stop_event_accepted') "
                     "FROM journal_records WHERE stop_event_id IS NOT NULL"),
            (std::vector<std::vector<std::optional<std::string>>>{{"72", "29", "100", "72"}}));
}

TEST(SqliteOperationJournalTest, OperationStopEventAndParticipantStopSurviveTheSnapshot) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "operation-stop.sqlite3", CellId("cell.1"),
                                 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  auto state = OperationState(45U, OperationStateKind::kExecuting, 10);
  state.journal_capture_order = 1U;
  const auto command = CommandIntent("request.operation-stop");
  ASSERT_TRUE(
      Accept(journal, command, Acceptance(command, state), state).ok());

  state.operation_revision = OperationRevision(2U);
  state.state = OperationStateKind::kStopped;
  state.participants.front().state = OperationStateKind::kStopped;
  state.updated_at_ns = 20;
  state.journal_capture_order = 2U;
  state.stop_event_id = StopEventId(7U);
  ParticipantStopObservationDTO stop;
  stop.phase = StopPhase::kStopped;
  stop.attempt = 1U;
  stop.stop_event_id = StopEventId(7U);
  stop.mode = StopMode::kOrdinary;
  stop.attribution_class = StopAttributionClass::kOperator;
  stop.plan_digest = Digest('a');
  stop.containing_stage_id = StopEscalationStageId("stage.nominal");
  stop.containing_stage_index = 0U;
  stop.escalated = false;
  state.participants.front().stop = stop;
  const auto recorded = Observe(journal, state, nullptr, "subordinate_stopped");
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;

  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 2U);
  const auto payload =
      manyforge::content_identity::parse_canonical_json(page.value().events.back().payload_json);
  const auto* operation = payload.find_object_member("operation_state");
  ASSERT_NE(operation, nullptr);
  EXPECT_EQ(operation->find_object_member("stop_event_id")->as_number(), 7.0);
  const auto* observed =
      operation->find_object_member("participants")->as_array().front().find_object_member("stop");
  ASSERT_NE(observed, nullptr);
  EXPECT_EQ(observed->find_object_member("phase")->as_string(), "stopped");
  EXPECT_EQ(observed->find_object_member("attempt")->as_number(), 1.0);
  EXPECT_EQ(observed->find_object_member("stop_event_id")->as_number(), 7.0);
  EXPECT_EQ(observed->find_object_member("mode")->as_string(), "ordinary");
  EXPECT_EQ(observed->find_object_member("attribution_class")->as_string(), "operator");
  EXPECT_EQ(observed->find_object_member("plan_digest")->as_string(), Digest('a').value());
  EXPECT_EQ(observed->find_object_member("containing_stage_id")->as_string(), "stage.nominal");
  EXPECT_EQ(observed->find_object_member("containing_stage_index")->as_number(), 0.0);
  EXPECT_FALSE(observed->find_object_member("escalated")->as_bool());

  const auto repeated = Observe(journal, state, nullptr, "subordinate_stopped");
  EXPECT_TRUE(repeated.ok()) << repeated.error().message;
}

/// The participant Stop observation of event 7's first attempt in `phase`.
[[nodiscard]] ParticipantStopObservationDTO EventSevenStop(const StopPhase phase) {
  ParticipantStopObservationDTO stop;
  stop.phase = phase;
  stop.attempt = 1U;
  stop.stop_event_id = StopEventId(7U);
  stop.mode = StopMode::kOrdinary;
  stop.attribution_class = StopAttributionClass::kOperator;
  stop.plan_digest = Digest('a');
  if (phase == StopPhase::kStopped) {
    stop.containing_stage_id = StopEscalationStageId("stage.nominal");
    stop.containing_stage_index = 0U;
  }
  return stop;
}

// The Kernel's journal writer records an operation's states around a
// Stop as ordinary state rows with these transition reasons.
TEST(SqliteOperationJournalTest, StopStateRowsAreRecordedWithTheirReasons) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  {
    SqliteOperationJournal journal(database, CellId("cell.1"), 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    auto state = OperationState(46U, OperationStateKind::kExecuting, 10);
    state.journal_capture_order = 1U;
    const auto command = CommandIntent("request.stop-states");
    ASSERT_TRUE(
        Accept(journal, command, Acceptance(command, state), state).ok());
    state.stop_event_id = StopEventId(7U);
    for (const auto* reason :
         {"stop_accepted", "stop_participant_completed", "stop_terminal_committed"}) {
      SCOPED_TRACE(reason);
      const std::string_view current(reason);
      const auto phase = current == "stop_accepted" ? StopPhase::kStopping : StopPhase::kStopped;
      state.operation_revision = OperationRevision(state.operation_revision.value() + 1U);
      ++state.journal_capture_order;
      ++state.updated_at_ns;
      state.participants.front().state = phase == StopPhase::kStopping
                                             ? OperationStateKind::kStopping
                                             : OperationStateKind::kStopped;
      state.state = current == "stop_terminal_committed" ? OperationStateKind::kStopped
                                                         : OperationStateKind::kStopping;
      state.participants.front().stop = EventSevenStop(phase);
      const auto recorded = Observe(journal, state, nullptr, reason);
      ASSERT_TRUE(recorded.ok()) << recorded.error().message;

      const auto page = journal.ReadOperationalEvents(0U);
      ASSERT_TRUE(page.ok()) << page.error().message;
      const auto& row = page.value().events.back();
      EXPECT_EQ(row.record_kind, "state_observed");
      const auto payload = manyforge::content_identity::parse_canonical_json(row.payload_json);
      EXPECT_EQ(payload.find_object_member("intent")
                    ->find_object_member("transition_reason")
                    ->as_string(),
                current);
      const auto* operation = payload.find_object_member("operation_state");
      EXPECT_EQ(operation->find_object_member("stop_event_id")->as_number(), 7.0);
      const auto* stop = operation->find_object_member("participants")
                             ->as_array()
                             .front()
                             .find_object_member("stop");
      ASSERT_NE(stop, nullptr);
      EXPECT_EQ(stop->find_object_member("phase")->as_string(), to_string(phase));
      EXPECT_EQ(stop->find_object_member("stop_event_id")->as_number(), 7.0);
    }
    journal.Close();
  }
  RawJournal raw(database);
  EXPECT_EQ(raw.Rows("SELECT s.state,s.stop_event_id,s.journal_capture_order,r.record_kind,"
                     "json_extract(r.payload_json,'$.intent.transition_reason') "
                     "FROM operation_snapshots s JOIN journal_records r "
                     "ON r.sequence=s.snapshot_sequence WHERE s.operation_id=46"),
            (std::vector<std::vector<std::optional<std::string>>>{
                {"stopped", "7", "4", "state_observed", "stop_terminal_committed"}}));
}

// A row whose capture is older than the snapshot's is written but does not
// regress the snapshot.
TEST(SqliteOperationJournalTest, LateStopAcceptedRowDoesNotRegressANewerSnapshot) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  {
    SqliteOperationJournal journal(database, CellId("cell.1"), 8U);
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    auto initial = OperationState(47U, OperationStateKind::kExecuting, 10);
    initial.journal_capture_order = 1U;
    const auto command = CommandIntent("request.late-stop-accepted");
    ASSERT_TRUE(
        Accept(journal, command, Acceptance(command, initial), initial).ok());

    auto stopped = initial;
    stopped.operation_revision = OperationRevision(3U);
    stopped.state = OperationStateKind::kStopped;
    stopped.participants.front().state = OperationStateKind::kStopped;
    stopped.participants.front().stop = EventSevenStop(StopPhase::kStopped);
    stopped.stop_event_id = StopEventId(7U);
    stopped.journal_capture_order = 5U;
    stopped.updated_at_ns = 30;
    ASSERT_TRUE(Observe(journal, stopped, nullptr, "subordinate_stopped").ok());

    auto stopping = initial;
    stopping.operation_revision = OperationRevision(2U);
    stopping.state = OperationStateKind::kStopping;
    stopping.participants.front().state = OperationStateKind::kStopping;
    stopping.participants.front().stop = EventSevenStop(StopPhase::kStopping);
    stopping.stop_event_id = StopEventId(7U);
    stopping.journal_capture_order = 3U;
    stopping.updated_at_ns = 20;
    const auto late = Observe(journal, stopping, nullptr, "stop_accepted");
    ASSERT_TRUE(late.ok()) << late.error().message;
    const auto page = journal.ReadOperationalEvents(0U);
    ASSERT_TRUE(page.ok()) << page.error().message;
    // The late row is the newest row; the snapshot keeps the newer capture.
    EXPECT_NE(page.value().events.back().payload_json.find("\"transition_reason\":\"stop_accepted\""),
              std::string::npos);
    journal.Close();
  }
  RawJournal raw(database);
  EXPECT_EQ(raw.Rows("SELECT s.state,s.journal_capture_order,"
                     "json_extract(r.payload_json,'$.intent.transition_reason') "
                     "FROM operation_snapshots s JOIN journal_records r "
                     "ON r.sequence=s.snapshot_sequence WHERE s.operation_id=47"),
            (std::vector<std::vector<std::optional<std::string>>>{
                {"stopped", "5", "subordinate_stopped"}}));
}

TEST(SqliteOperationJournalTest, EveryRowCarriesTheParticipantStopMember) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "no-stop.sqlite3", CellId("cell.1"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  auto state = OperationState(48U, OperationStateKind::kExecuting, 10);
  const auto command = CommandIntent("request.never-stopped");
  ASSERT_TRUE(
      Accept(journal, command, Acceptance(command, state), state).ok());
  state.state = OperationStateKind::kCompleted;
  state.participants.front().state = OperationStateKind::kCompleted;
  state.participants.front().progress = 1.0;
  state.updated_at_ns = 11;
  ASSERT_TRUE(Observe(journal, state, nullptr, "subordinate_completed").ok());
  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 2U);
  for (const auto& row : page.value().events) {
    SCOPED_TRACE(row.record_kind);
    const auto payload = manyforge::content_identity::parse_canonical_json(row.payload_json);
    const auto* operation = payload.find_object_member("operation_state");
    EXPECT_TRUE(operation->find_object_member("stop_event_id")->is_null());
    const auto* stop = operation->find_object_member("participants")
                           ->as_array()
                           .front()
                           .find_object_member("stop");
    ASSERT_NE(stop, nullptr);
    EXPECT_TRUE(stop->is_null());
  }
}

[[nodiscard]] OperationStateDTO CompletedState(OperationStateDTO state) {
  state.state = OperationStateKind::kCompleted;
  state.participants.front().state = OperationStateKind::kCompleted;
  state.participants.front().progress = 1.0;
  ++state.updated_at_ns;
  return state;
}

[[nodiscard]] JournalOperationAccepted AcceptedRecord(const std::uint64_t operation_id,
                                                      const std::int64_t at) {
  const auto intent = CommandIntent("request.bounded." + std::to_string(operation_id));
  const auto state = OperationState(operation_id, OperationStateKind::kExecuting, at);
  return JournalOperationAccepted{intent, Acceptance(intent, state), state};
}

[[nodiscard]] std::string Scalar(RawJournal& raw, const std::string& sql) {
  return raw.Rows(sql).front().front().value_or("");
}

// The writer removes old finished operations from its own memory; an
// operation whose written state is not terminal is never removed. Every
// third operation never finishes; the others finish one batch later.
TEST(SqliteOperationJournalTest, CompactionNeverRemovesAnUnfinishedOperation) {
  constexpr std::uint64_t kFirst = 10'000U;
  constexpr std::uint64_t kOperations = kMaximumOperationJournalHistory + 120U;
  constexpr std::uint64_t kPerBatch = 64U;
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "compaction.sqlite3";
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, kMaximumOperationJournalHistory).ok());
    std::vector<OperationStateDTO> finishing;
    for (std::uint64_t first = 0U; first < kOperations; first += kPerBatch) {
      std::vector<JournalRecordPayload> batch;
      for (std::uint64_t index = first; index < std::min(kOperations, first + kPerBatch);
           ++index) {
        auto accepted = AcceptedRecord(kFirst + index, static_cast<std::int64_t>(10U + index));
        if (index % 3U != 0U) finishing.push_back(accepted.state);
        batch.push_back(std::move(accepted));
        // The previous batch's operations finish between these acceptances.
        if (!finishing.empty() && finishing.front().operation_id.value() < kFirst + first) {
          batch.push_back(StateRecord(CompletedState(finishing.front()), nullptr,
                                      "subordinate_completed"));
          finishing.erase(finishing.begin());
        }
      }
      ASSERT_TRUE(Write(journal, std::move(batch)).failures.empty());
    }
    std::vector<JournalRecordPayload> rest;
    for (const auto& state : finishing)
      rest.push_back(StateRecord(CompletedState(state), nullptr, "subordinate_completed"));
    ASSERT_TRUE(Write(journal, std::move(rest)).failures.empty());
  }
  RawJournal raw(database);
  const auto unfinished = std::to_string((kOperations + 2U) / 3U);
  EXPECT_EQ(Scalar(raw, "SELECT COUNT(*) FROM operation_snapshots WHERE (operation_id-10000)%3=0 "
                        "AND state='executing'"),
            unfinished);
  EXPECT_EQ(Scalar(raw, "SELECT COUNT(*) FROM journal_records WHERE "
                        "record_kind='operation_accepted' AND (operation_id-10000)%3=0"),
            unfinished);
  EXPECT_LE(std::stoul(Scalar(raw, "SELECT COUNT(*) FROM operation_snapshots")),
            kMaximumOperationJournalHistory);
  // The oldest finished operations went with all their rows.
  EXPECT_EQ(Scalar(raw, "SELECT COUNT(*) FROM journal_records WHERE operation_id=10001"), "0");
  EXPECT_EQ(Scalar(raw, "SELECT COUNT(*) FROM operation_snapshots WHERE operation_id=" +
                            std::to_string(kFirst + kOperations - 1U)),
            "1");
}

// Only after a record was dropped or lost are unfinished operations bounded
// too, oldest first: the journal may never see their terminal state.
TEST(SqliteOperationJournalTest, AfterALostRecordUnfinishedOperationsAreBoundedOldestFirst) {
  for (const bool dropped : {true, false}) {
    SCOPED_TRACE(dropped ? "dropped by the queue" : "lost by a failed write");
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "unfinished-bound.sqlite3";
    {
      SqliteOperationJournal journal(database, CellId("cell.1"), 4U);
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 4U).ok());
      std::vector<JournalRecordPayload> six;
      for (std::uint64_t id = 101U; id <= 106U; ++id)
        six.push_back(AcceptedRecord(id, static_cast<std::int64_t>(id)));
      ASSERT_TRUE(Write(journal, std::move(six)).failures.empty());
      {
        RawJournal raw(database);
        EXPECT_EQ(Scalar(raw, "SELECT COUNT(*) FROM operation_snapshots"), "6");
      }
      if (!dropped) {
        auto invalid = OperationState(106U, OperationStateKind::kExecuting, 200);
        invalid.participants.clear();
        ASSERT_EQ(Write(journal, {StateRecord(invalid, nullptr, "invalid")}).failures.size(), 1U);
      }
      ASSERT_TRUE(Write(journal, {AcceptedRecord(107U, 300)}, dropped ? 1U : 0U).failures.empty());
    }
    RawJournal raw(database);
    EXPECT_EQ(raw.Rows("SELECT operation_id FROM operation_snapshots ORDER BY operation_id"),
              (std::vector<std::vector<std::optional<std::string>>>{
                  {"104"}, {"105"}, {"106"}, {"107"}}));
    EXPECT_EQ(Scalar(raw, "SELECT COUNT(*) FROM journal_records WHERE operation_id<104"), "0");
  }
}

// A record that fails is rolled back alone (after its row was inserted, here)
// and reported with its index; the others of the batch are written in order.
TEST(SqliteOperationJournalTest, AFailingRecordLosesOnlyItself) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "failing-record.sqlite3", CellId("cell.1"),
                                 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  auto first = AcceptedRecord(60U, 10);
  first.state.operation_revision = OperationRevision(2U);
  first.state.journal_capture_order = 1U;
  first.accepted.operation_revision = first.state.operation_revision;
  // A newer capture with an older public revision: refused after its insert.
  auto regressed = first.state;
  regressed.operation_revision = OperationRevision(1U);
  regressed.journal_capture_order = 2U;
  regressed.updated_at_ns = 11;
  const auto result = Write(journal, {first, StateRecord(regressed, nullptr, "regressed"),
                                      AcceptedRecord(61U, 12)});
  ASSERT_EQ(result.failures.size(), 1U);
  EXPECT_EQ(result.failures.front().index, 1U);
  EXPECT_EQ(result.failures.front().error.code, "operation_journal_corrupt");
  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 2U);
  EXPECT_EQ(page.value().events[0].operation_id, OperationNumber(60U));
  EXPECT_EQ(page.value().events[1].operation_id, OperationNumber(61U));
  EXPECT_EQ(page.value().events[1].sequence, page.value().events[0].sequence + 1U);
}

std::vector<sqlite3*>* opened_connections = nullptr;
int RecordOpenedConnection(sqlite3* database, char**, const sqlite3_api_routines*) {
  if (opened_connections != nullptr) opened_connections->push_back(database);
  return SQLITE_OK;
}

[[nodiscard]] std::string PragmaText(sqlite3* database, const char* pragma) {
  sqlite3_stmt* statement = nullptr;
  std::string value;
  if (sqlite3_prepare_v2(database, pragma, -1, &statement, nullptr) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW)
    value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  return value;
}

// The writer connection runs WAL with NORMAL sync (1).
TEST(SqliteOperationJournalTest, WriterConnectionUsesWalWithNormalSync) {
  ScopedTemporaryDirectory temporary;
  std::vector<sqlite3*> connections;
  opened_connections = &connections;
  ASSERT_EQ(sqlite3_auto_extension(reinterpret_cast<void (*)()>(RecordOpenedConnection)),
            SQLITE_OK);
  SqliteOperationJournal journal(temporary.path() / "pragma.sqlite3", CellId("cell.1"), 8U);
  sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(RecordOpenedConnection));
  opened_connections = nullptr;
  ASSERT_EQ(connections.size(), 2U);
  ASSERT_EQ(sqlite3_db_readonly(connections.front(), "main"), 0);
  EXPECT_EQ(PragmaText(connections.front(), "PRAGMA synchronous"), "1");
  EXPECT_EQ(PragmaText(connections.front(), "PRAGMA journal_mode"), "wal");
}

// Rows are inserted in queue order, over several transactions of one batch;
// the page reader walks them with a strictly increasing cursor. Claim rows
// are not on the operational page.
TEST(SqliteOperationJournalTest, PageReaderSeesStrictlyIncreasingSequencesInQueueOrder) {
  ScopedTemporaryDirectory temporary;
  SqliteOperationJournal journal(temporary.path() / "pages.sqlite3", CellId("cell.1"));
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, kMaximumOperationJournalHistory).ok());
  constexpr std::uint64_t kPairs = 150U;
  std::vector<JournalRecordPayload> batch;
  for (std::uint64_t index = 0U; index < kPairs; ++index) {
    batch.push_back(AcceptedRecord(500U + index, static_cast<std::int64_t>(index)));
    batch.push_back(GlobalRuleChange(index % 2U ? IoGlobalRuleState::kClear
                                                : IoGlobalRuleState::kTripped,
                                     static_cast<TimePoint>(index)));
    if (index == kPairs / 2U) {
      batch.push_back(JournalCellClaimAcquired{CellAdmissionClaimDTO{
          CellAdmissionClaimIntentDTO{CellAdmissionClaimKindDTO::kAuthoringMutation,
                                      ApplicationRole::kAuthoringOwner, std::nullopt,
                                      TestRequestKey("request.page-claim"),
                                      AdmissionGeneration(1U), {}, std::nullopt, std::nullopt},
          CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U), 5}});
    }
  }
  ASSERT_GT(batch.size(), 256U);
  ASSERT_TRUE(Write(journal, std::move(batch)).failures.empty());

  std::vector<OperationJournalEventDTO> events;
  std::uint64_t after = 0U;
  for (bool more = true; more;) {
    const auto page = journal.ReadOperationalEvents(after);
    ASSERT_TRUE(page.ok()) << page.error().message;
    for (const auto& event : page.value().events) {
      EXPECT_GT(event.sequence, after);
      after = event.sequence;
      events.push_back(event);
    }
    more = page.value().has_more;
  }
  ASSERT_EQ(events.size(), 2U * kPairs);
  for (std::uint64_t index = 0U; index < kPairs; ++index) {
    EXPECT_EQ(events[2U * index].record_kind, "operation_accepted");
    EXPECT_EQ(events[2U * index].operation_id, OperationNumber(500U + index));
    EXPECT_EQ(events[2U * index + 1U].record_kind, "interrupt_rule_state_changed");
  }
  // The claim follows the rule row of pair kPairs / 2 (operational index kPairs + 1).
  const RawJournal raw(temporary.path() / "pages.sqlite3");
  EXPECT_EQ(raw.Rows("SELECT record_kind FROM journal_records WHERE sequence>" +
                     std::to_string(events[kPairs + 1U].sequence) +
                     " ORDER BY sequence LIMIT 1"),
            (std::vector<std::vector<std::optional<std::string>>>{{"cell_claim_acquired"}}));
}

// The two claim rows: the claim's fields under the former column names, and
// its release; `claim_generation` is a JSON number so the launcher report can
// pair them.
TEST(SqliteOperationJournalTest, CellClaimRowsCarryTheClaimAndItsRelease) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / "claims.sqlite3";
  const CellAdmissionClaimDTO claim{
      CellAdmissionClaimIntentDTO{
          CellAdmissionClaimKindDTO::kAuthoringMutation, ApplicationRole::kAuthoringOwner,
          std::nullopt, TestRequestKey("request.claim"), AdmissionGeneration(1U),
          CellAdmissionScopeDTO{
              {ResourceId("resource.1")},
              {CellAdmissionConfigurationScopeDTO{ControlDomainId("domain.1"),
                                                  ActiveControlConfigurationId("configuration.1"),
                                                  ActiveControlConfigurationGeneration(1U)}}},
          std::nullopt, std::nullopt},
      CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U), 10};
  const CellAdmissionClaimReleaseDTO release{
      ApplicationRole::kAuthoringOwner, std::nullopt, TestRequestKey("request.release"),
      AdmissionGeneration(1U), CellAdmissionClaimGeneration(1U),
      CellAdmissionSettlementEvidenceDTO{CellAdmissionSettlementKindDTO::kOwnerCommit,
                                         Digest('0')}};
  {
    SqliteOperationJournal journal(database, CellId("cell.1"), 8U,
                                   [](std::int64_t) { return std::string("wall"); });
    ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
    auto other = claim;
    other.claim_generation = CellAdmissionClaimGeneration(2U);
    ASSERT_TRUE(Write(journal, {JournalCellClaimAcquired{claim},
                                JournalCellClaimAcquired{other},
                                JournalCellClaimReleased{claim, release, 20}})
                    .failures.empty());
    auto mismatched = release;
    mismatched.claim_generation = CellAdmissionClaimGeneration(9U);
    const auto refused = WriteOne(journal, JournalCellClaimReleased{claim, mismatched, 21});
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "operation_journal_record_invalid");
    const auto page = journal.ReadOperationalEvents(0U);
    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(page.value().events.empty());
  }
  RawJournal raw(database);
  const auto rows = raw.Rows(
      "SELECT record_kind,request_key,operation_id,operation_revision,stop_event_id,"
      "occurred_at_ns,payload_json FROM journal_records ORDER BY sequence");
  ASSERT_EQ(rows.size(), 3U);
  const auto request = RequestKeyText(TestRequestKey("request.claim"));
  EXPECT_EQ(rows[0],
            (std::vector<std::optional<std::string>>{
                "cell_claim_acquired", std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                "10",
                "{\"accepted_at_ns\":10,\"acquired_admission_generation\":1,"
                "\"applied_program_revision\":null,\"claim_generation\":1,"
                "\"claim_kind\":\"authoring_mutation\",\"configurations\":[{"
                "\"active_configuration_generation\":1,"
                "\"active_configuration_id\":\"configuration.1\","
                "\"control_domain_id\":\"domain.1\"}],\"draft_program_revision\":null,"
                "\"expected_admission_generation\":1,\"offer_digest\":null,"
                "\"offer_transaction_id\":null,\"offering_owner_role\":null,"
                "\"owner_role\":\"authoring_owner\",\"owner_transaction_id\":null,"
                "\"parent_program_cycle_claim_generation\":null,"
                "\"program_node_attempt_id\":null,\"program_node_instance_id\":null,"
                "\"program_node_owner_api\":null,\"program_node_run_id\":null,"
                "\"request_key\":\"" +
                    request + "\",\"resources\":[\"resource.1\"]}"}));
  EXPECT_EQ(rows[2],
            (std::vector<std::optional<std::string>>{
                "cell_claim_released", std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                "20",
                "{\"claim_generation\":1,\"release_acquired_admission_generation\":1,"
                "\"release_evidence_digest\":\"" +
                    Digest('0').value() +
                    "\",\"release_owner_role\":\"authoring_owner\","
                    "\"release_owner_transaction_id\":null,\"release_request_key\":\"" +
                    RequestKeyText(TestRequestKey("request.release")) +
                    "\",\"release_settlement_kind\":\"owner_commit\",\"released_at_ns\":20}"}));
  EXPECT_EQ(Scalar(raw, "SELECT json_type(payload_json,'$.claim_generation') FROM journal_records "
                        "WHERE record_kind='cell_claim_released'"),
            "integer");
  // Acquired without a released row: claim 2 only.
  EXPECT_EQ(raw.Rows("SELECT json_extract(a.payload_json,'$.claim_generation') FROM "
                     "journal_records a WHERE a.record_kind='cell_claim_acquired' AND NOT "
                     "EXISTS (SELECT 1 FROM journal_records r WHERE "
                     "r.record_kind='cell_claim_released' AND "
                     "json_extract(r.payload_json,'$.claim_generation')="
                     "json_extract(a.payload_json,'$.claim_generation'))"),
            (std::vector<std::vector<std::optional<std::string>>>{{"2"}}));
}

// After every release the writer keeps both rows of the newest 64 released
// claims and deletes older released ones; a claim not released keeps its row
// whatever its age, and once released it falls under the same rule. The
// interleaved Stop rows keep their own newest 64 finished events.
TEST(SqliteOperationJournalTest, ReleasedClaimsKeepTheNewest64AndUnreleasedClaimsStay) {
  const auto claim = [](const std::uint64_t generation) {
    return CellAdmissionClaimDTO{
        CellAdmissionClaimIntentDTO{
            CellAdmissionClaimKindDTO::kAuthoringMutation, ApplicationRole::kAuthoringOwner,
            std::nullopt, TestRequestKey("request.claim." + std::to_string(generation)),
            AdmissionGeneration(1U),
            CellAdmissionScopeDTO{
                {ResourceId("resource.1")},
                {CellAdmissionConfigurationScopeDTO{
                    ControlDomainId("domain.1"), ActiveControlConfigurationId("configuration.1"),
                    ActiveControlConfigurationGeneration(1U)}}},
            std::nullopt, std::nullopt},
        CellAdmissionClaimGeneration(generation), AdmissionGeneration(1U),
        static_cast<std::int64_t>(generation) * 10};
  };
  const auto released = [&](const std::uint64_t generation) {
    return JournalCellClaimReleased{
        claim(generation),
        CellAdmissionClaimReleaseDTO{
            ApplicationRole::kAuthoringOwner, std::nullopt,
            TestRequestKey("request.release." + std::to_string(generation)),
            AdmissionGeneration(1U), CellAdmissionClaimGeneration(generation),
            CellAdmissionSettlementEvidenceDTO{CellAdmissionSettlementKindDTO::kOwnerCommit,
                                               Digest('0')}},
        static_cast<std::int64_t>(generation) * 10 + 5};
  };
  const std::string claim_rows =
      "SELECT SUM(record_kind='cell_claim_acquired'),SUM(record_kind='cell_claim_released'),"
      "MIN(json_extract(payload_json,'$.claim_generation')),"
      "MAX(json_extract(payload_json,'$.claim_generation')) FROM journal_records "
      "WHERE record_kind IN ('cell_claim_acquired','cell_claim_released') "
      "AND json_extract(payload_json,'$.claim_generation')>1";
  const std::string claim_one =
      "SELECT record_kind FROM journal_records WHERE record_kind IN "
      "('cell_claim_acquired','cell_claim_released') "
      "AND json_extract(payload_json,'$.claim_generation')=1 ORDER BY sequence";
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  SqliteOperationJournal journal(database, CellId("cell.1"), 8U);
  ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
  {
    // One writer batch: claim 1 held throughout; claims 2..101 acquired and
    // released, each next to one finished Stop event.
    std::vector<JournalRecordPayload> rows{JournalCellClaimAcquired{claim(1U)}};
    for (std::uint64_t generation = 2U; generation <= 101U; ++generation) {
      rows.push_back(JournalCellClaimAcquired{claim(generation)});
      rows.push_back(released(generation));
      const auto event = generation - 1U;
      const auto accepted = AcceptedStopEvent(event, static_cast<std::int64_t>(event) * 10);
      rows.push_back(StopRow(accepted, false));
      rows.push_back(StopRow(
          CompletedStopEvent(accepted, static_cast<std::int64_t>(event) * 10 + 5), true));
    }
    ASSERT_TRUE(Write(journal, std::move(rows)).failures.empty());
    RawJournal raw(database);
    // Claims 38..101 keep both rows; 2..37 are gone.
    EXPECT_EQ(raw.Rows(claim_rows), (std::vector<std::vector<std::optional<std::string>>>{
                                        {"64", "64", "38", "101"}}));
    EXPECT_EQ(raw.Rows(claim_one), (std::vector<std::vector<std::optional<std::string>>>{
                                       {"cell_claim_acquired"}}));
    // The Stop rows as without claims (FinishedStopEventsKeepTheNewest64).
    EXPECT_EQ(raw.Rows("SELECT COUNT(*),COUNT(DISTINCT stop_event_id),MIN(stop_event_id),"
                       "MAX(stop_event_id),SUM(record_kind='stop_event_completed') "
                       "FROM journal_records WHERE stop_event_id IS NOT NULL"),
              (std::vector<std::vector<std::optional<std::string>>>{
                  {"128", "64", "37", "100", "64"}}));
  }
  // Claim 1 released last: it is now among the newest 64, claim 38 goes.
  ASSERT_TRUE(WriteOne(journal, released(1U)).ok());
  {
    RawJournal raw(database);
    EXPECT_EQ(raw.Rows(claim_rows), (std::vector<std::vector<std::optional<std::string>>>{
                                        {"63", "63", "39", "101"}}));
    EXPECT_EQ(raw.Rows(claim_one), (std::vector<std::vector<std::optional<std::string>>>{
                                       {"cell_claim_acquired"}, {"cell_claim_released"}}));
  }
  // 64 newer releases: claim 1 is beyond the newest 64, both its rows go.
  std::vector<JournalRecordPayload> rows;
  for (std::uint64_t generation = 102U; generation <= 165U; ++generation) {
    rows.push_back(JournalCellClaimAcquired{claim(generation)});
    rows.push_back(released(generation));
  }
  ASSERT_TRUE(Write(journal, std::move(rows)).failures.empty());
  RawJournal raw(database);
  EXPECT_EQ(raw.Rows(claim_rows), (std::vector<std::vector<std::optional<std::string>>>{
                                      {"64", "64", "102", "165"}}));
  EXPECT_TRUE(raw.Rows(claim_one).empty());
}

// `journal_metadata.records_lost` is 0 on a clean journal, also after a clean
// Close; it becomes 1 after a batch that carries dropped records (with or
// without rows) or a record that fails, and stays 1 through later clean
// batches and the Close.
TEST(SqliteOperationJournalTest, RecordsLostMarksAStartThatLostRecordsAndStays) {
  const CellAdmissionClaimDTO claim{
      CellAdmissionClaimIntentDTO{
          CellAdmissionClaimKindDTO::kAuthoringMutation, ApplicationRole::kAuthoringOwner,
          std::nullopt, TestRequestKey("request.lost"), AdmissionGeneration(1U),
          CellAdmissionScopeDTO{
              {ResourceId("resource.1")},
              {CellAdmissionConfigurationScopeDTO{ControlDomainId("domain.1"),
                                                  ActiveControlConfigurationId("configuration.1"),
                                                  ActiveControlConfigurationGeneration(1U)}}},
          std::nullopt, std::nullopt},
      CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U), 10};
  const CellAdmissionClaimReleaseDTO mismatched{
      ApplicationRole::kAuthoringOwner, std::nullopt, TestRequestKey("request.lost.release"),
      AdmissionGeneration(1U), CellAdmissionClaimGeneration(9U),
      CellAdmissionSettlementEvidenceDTO{CellAdmissionSettlementKindDTO::kOwnerCommit,
                                         Digest('0')}};
  const auto records_lost = [](const std::filesystem::path& database) {
    RawJournal raw(database);
    return Scalar(raw, "SELECT records_lost FROM journal_metadata");
  };
  for (const auto scenario : {0, 1, 2, 3}) {
    SCOPED_TRACE(scenario);
    ScopedTemporaryDirectory temporary;
    const auto database = temporary.path() / "lost.sqlite3";
    {
      SqliteOperationJournal journal(database, CellId("cell.1"), 8U,
                                     [](std::int64_t) { return std::string("wall"); });
      ASSERT_TRUE(journal.BeginStart(kJournalStartId, 1, 8U).ok());
      EXPECT_EQ(records_lost(database), "0");
      if (scenario == 0) {
        ASSERT_TRUE(Write(journal, {JournalCellClaimAcquired{claim}}).failures.empty());
        EXPECT_EQ(records_lost(database), "0");
        journal.Close();
        EXPECT_EQ(records_lost(database), "0");
        continue;
      }
      if (scenario == 1) {
        ASSERT_TRUE(Write(journal, {JournalCellClaimAcquired{claim}}, 3U).failures.empty());
      }
      if (scenario == 2) {
        ASSERT_TRUE(Write(journal, {}, 1U).failures.empty());
      }
      if (scenario == 3) {
        ASSERT_EQ(Write(journal, {JournalCellClaimReleased{claim, mismatched, 20}}).failures.size(),
                  1U);
      }
      EXPECT_EQ(records_lost(database), "1");
      auto later = claim;
      later.claim_generation = CellAdmissionClaimGeneration(2U);
      ASSERT_TRUE(Write(journal, {JournalCellClaimAcquired{later}}).failures.empty());
      EXPECT_EQ(records_lost(database), "1");
      journal.Close();
    }
    EXPECT_EQ(records_lost(database), "1");
  }
}

}  // namespace
