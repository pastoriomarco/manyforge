#include "cell_admission_command_codec.hpp"
#include "owner_settlement_readers.hpp"

#include <gtest/gtest.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace manyforge::roboplan_backend::detail {
namespace {
namespace core = manyforge::core;
constexpr std::string_view kDigestA =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kDigestB =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

class TemporaryDatabase final {
public:
  TemporaryDatabase() {
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-owner-settlement-" + std::to_string(::getpid()) + "-" +
             std::to_string(++sequence_) + ".sqlite3");
    std::filesystem::remove(path_);
    if (sqlite3_open(path_.string().c_str(), &database_) != SQLITE_OK)
      throw std::runtime_error("open temporary SQLite database");
  }
  ~TemporaryDatabase() {
    if (database_ != nullptr)
      sqlite3_close(database_);
    std::filesystem::remove(path_);
  }
  void Exec(const std::string &sql) {
    char *message = nullptr;
    const int result =
        sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &message);
    const std::string detail = message == nullptr ? "" : message;
    if (message != nullptr)
      sqlite3_free(message);
    if (result != SQLITE_OK)
      throw std::runtime_error("SQLite fixture failure: " + detail);
  }
  void Close() {
    if (database_ != nullptr) {
      sqlite3_close(database_);
      database_ = nullptr;
    }
  }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  inline static unsigned sequence_{};
  std::filesystem::path path_;
  sqlite3 *database_{};
};

[[nodiscard]] std::string ClaimJson(std::string_view kind,
                                    std::string_view role,
                                    std::string_view owner_epoch,
                                    std::string_view transaction,
                                    std::string_view request) {
  const std::string program_evidence =
      kind == "program_cycle"
          ? "{\"applied_program_revision\":\"" + std::string(kDigestA) +
                "\",\"draft_program_revision\":\"" + std::string(kDigestA) +
                "\"}"
          : "null";
  return "{\"accepted_at_ns\":1,\"acquired_admission_generation\":2,"
         "\"claim_generation\":3,\"intent\":{\"claim_kind\":\"" +
         std::string(kind) +
         "\",\"expected_admission_generation\":1,"
         "\"intervention_evidence\":null,\"owner_epoch\":\"" +
         std::string(owner_epoch) + "\",\"owner_role\":\"" + std::string(role) +
         "\",\"owner_transaction_id\":\"" + std::string(transaction) +
         "\",\"program_cycle_evidence\":" + program_evidence +
         ",\"request_id\":\"" + std::string(request) +
         "\",\"request_payload_digest\":\"" + std::string(kDigestA) +
         "\",\"scope\":{\"configurations\":[],\"resources\":[]}}}";
}

[[nodiscard]] std::string
ReleaseJson(std::string_view role, std::string_view owner_epoch,
            std::string_view transaction, std::string_view request,
            std::string_view settlement, std::string_view evidence) {
  return "{\"acquired_admission_generation\":2,\"claim_generation\":3,"
         "\"owner_epoch\":\"" +
         std::string(owner_epoch) + "\",\"owner_role\":\"" + std::string(role) +
         "\",\"owner_transaction_id\":\"" + std::string(transaction) +
         "\",\"request_id\":\"" + std::string(request) +
         "\",\"request_payload_digest\":\"" + std::string(kDigestA) +
         "\",\"settlement\":{\"evidence_digest\":\"" + std::string(evidence) +
         "\",\"settlement_kind\":\"" + std::string(settlement) + "\"}}";
}

[[nodiscard]] std::string
ProgramStartSettlementJson(std::string_view claim_json,
                           std::string_view program_run_id) {
  return "{\"claim\":" + std::string(claim_json) +
         ",\"kind\":\"proven_no_effect\",\"prepared_scene\":{"
         "\"schema_version\":\"manyforge.prepared_program_scene.v1\"},"
         "\"program_run_id\":\"" +
         std::string(program_run_id) +
         "\",\"schema_version\":"
         "\"manyforge.program_cycle_start_settlement.v1\"}";
}

TEST(OwnerSettlementReaders,
     ActiveProgramRunRecoveryReturnsItsExactCapturedProduct) {
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "program_run_id TEXT,reconciliation_required INTEGER);"
      "CREATE TABLE program_run_history(program_run_id TEXT PRIMARY KEY,"
      "snapshot_json TEXT,terminal INTEGER);"
      "INSERT INTO program_cycle VALUES(1,'running','run.7',1);"
      "INSERT INTO program_run_history VALUES('run.7',"
      "'{\"capture\":{\"graph_revision\":\"" +
      std::string(kDigestA) +
      "\",\"program_run_id\":\"run.7\",\"retained_bundle_id\":\"" +
      std::string(kDigestB) + "\"},\"state\":\"running\"}',0)");
  fixture.Close();

  const auto result =
      ReadActiveProgramRunProductForReconciliation(fixture.path());

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result.value().has_value());
  EXPECT_EQ(result.value()->program_run_id, core::ProgramRunId("run.7"));
  EXPECT_EQ(result.value()->retained_bundle_id,
            core::RetainedBundleId(std::string(kDigestB)));
  EXPECT_EQ(result.value()->graph_revision,
            core::GraphRevision(std::string(kDigestA)));
}

TEST(OwnerSettlementReaders,
     ActiveProgramRunRecoveryReadsTheCurrentJournalSchemaOnly) {
  // Development-only cutover: exactly the runner's current journal schema is
  // read; an older or newer journal is refused, never migrated.
  const auto journal = [](int schema_version) {
    TemporaryDatabase fixture;
    fixture.Exec(
        "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
        "schema_version INTEGER); INSERT INTO program_cycle_metadata "
        "VALUES(1," +
        std::to_string(schema_version) +
        ");"
        "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
        "program_run_id TEXT,reconciliation_required INTEGER);"
        "CREATE TABLE program_run_history(program_run_id TEXT PRIMARY KEY,"
        "snapshot_json TEXT,terminal INTEGER);"
        "INSERT INTO program_cycle VALUES(1,'running','run.7',1);"
        "INSERT INTO program_run_history VALUES('run.7',"
        "'{\"capture\":{\"graph_revision\":\"" +
        std::string(kDigestA) +
        "\",\"program_run_id\":\"run.7\",\"retained_bundle_id\":\"" +
        std::string(kDigestB) +
        "\"},\"details\":null,\"origin\":null,"
        "\"schema_version\":\"manyforge.program_run_status.v3\","
        "\"state\":\"running\"}',0)");
    fixture.Close();
    return ReadActiveProgramRunProductForReconciliation(fixture.path());
  };

  const auto current = journal(7);
  ASSERT_TRUE(current.ok());
  ASSERT_TRUE(current.value().has_value());
  EXPECT_EQ(current.value()->program_run_id, core::ProgramRunId("run.7"));
  EXPECT_FALSE(journal(6).ok());
  EXPECT_FALSE(journal(3).ok());
  EXPECT_FALSE(journal(8).ok());
}

TEST(OwnerSettlementReaders, AbsentProgramJournalHasNoRecoveryProduct) {
  const auto absent = std::filesystem::temp_directory_path() /
                      ("manyforge-absent-program-cycle-" +
                       std::to_string(::getpid()) + ".sqlite3");
  std::filesystem::remove(absent);

  const auto result = ReadActiveProgramRunProductForReconciliation(absent);

  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().has_value());
}

TEST(OwnerSettlementReaders,
     CurrentOrTerminalProgramRunNeedsNoRecoveryProduct) {
  TemporaryDatabase current;
  current.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "program_run_id TEXT,reconciliation_required INTEGER);"
      "INSERT INTO program_cycle VALUES(1,'running','run.7',0)");
  current.Close();
  const auto current_result =
      ReadActiveProgramRunProductForReconciliation(current.path());
  ASSERT_TRUE(current_result.ok());
  EXPECT_FALSE(current_result.value().has_value());

  TemporaryDatabase terminal;
  terminal.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "program_run_id TEXT,reconciliation_required INTEGER);"
      "INSERT INTO program_cycle "
      "VALUES(1,'terminal_awaiting_reset','run.7',1)");
  terminal.Close();
  const auto terminal_result =
      ReadActiveProgramRunProductForReconciliation(terminal.path());
  ASSERT_TRUE(terminal_result.ok());
  EXPECT_FALSE(terminal_result.value().has_value());
}

TEST(OwnerSettlementReaders, ActiveProgramRunRecoveryRejectsACrossRunCapture) {
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "program_run_id TEXT,reconciliation_required INTEGER);"
      "CREATE TABLE program_run_history(program_run_id TEXT PRIMARY KEY,"
      "snapshot_json TEXT,terminal INTEGER);"
      "INSERT INTO program_cycle VALUES(1,'running','run.7',1);"
      "INSERT INTO program_run_history VALUES('run.7',"
      "'{\"capture\":{\"graph_revision\":\"" +
      std::string(kDigestA) +
      "\",\"program_run_id\":\"run.other\",\"retained_bundle_id\":\"" +
      std::string(kDigestB) + "\"},\"state\":\"running\"}',0)");
  fixture.Close();

  const auto result =
      ReadActiveProgramRunProductForReconciliation(fixture.path());

  EXPECT_FALSE(result.ok());
}

TEST(OwnerSettlementReaders,
     RecoveryNeverTreatsPartialOrCrossedRunRecordsAsNoRecoveryProduct) {
  struct Case {
    const char *cycle_state;
    const char *run_state;
    const char *captured_id;
    int terminal;
    bool row_present;
    bool valid;
  };
  for (const auto &item :
       {Case{"running", "running", "run.7", 0, false, false},
        Case{"running", "running", "run.7", 1, true, false},
        Case{"stopping", "stopped", "run.7", 0, true, false},
        Case{"resetting", "unknown", "run.7", 0, true, false},
        Case{"recovery_required", "succeeded", "run.other", 1, true, false},
        Case{"starting", "running", "run.7", 2, true, false},
        Case{"recovery_required", "full_stopped", "run.7", 0, true, true},
        Case{"recovery_required", "stopped", "run.7", 1, true, true}}) {
    SCOPED_TRACE(std::string(item.cycle_state) + "/" + item.run_state + "/" +
                 std::to_string(item.terminal));
    TemporaryDatabase fixture;
    fixture.Exec(
        "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY "
        "KEY,schema_version INTEGER);"
        "INSERT INTO program_cycle_metadata VALUES(1,7);"
        "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state "
        "TEXT,program_run_id TEXT,reconciliation_required INTEGER);"
        "CREATE TABLE program_run_history(program_run_id TEXT PRIMARY "
        "KEY,snapshot_json TEXT,terminal INTEGER);"
        "INSERT INTO program_cycle VALUES(1,'" +
        std::string(item.cycle_state) + "','run.7',1)");
    if (item.row_present) {
      fixture.Exec("INSERT INTO program_run_history "
                   "VALUES('run.7','{\"capture\":{\"graph_revision\":\"" +
                   std::string(kDigestA) + "\",\"program_run_id\":\"" +
                   item.captured_id + "\",\"retained_bundle_id\":\"" +
                   std::string(kDigestB) + "\"},\"state\":\"" + item.run_state +
                   "\"}'," + std::to_string(item.terminal) + ")");
    }
    fixture.Close();
    const auto result =
        ReadActiveProgramRunProductForReconciliation(fixture.path());
    ASSERT_EQ(result.ok(), item.valid);
    if (item.valid)
      EXPECT_EQ(result.value().has_value(), item.terminal == 0);
    else
      EXPECT_EQ(result.error().details.at("reason"),
                "program_run_recovery_product_unavailable");
  }
}

TEST(OwnerSettlementReaders,
     ProgramResetRequiresExactDurableTerminalClaimAndReleaseIntent) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const std::string start_settlement =
      "{\"claim\":" + claim_json +
      ",\"kind\":\"proven_no_effect\",\"prepared_scene\":{"
      "\"schema_version\":\"manyforge.prepared_program_scene.v1\"},"
      "\"program_run_id\":\"cycle.1\",\"schema_version\":"
      "\"manyforge.program_cycle_start_settlement.v1\"}";
  const auto evidence = content_identity::sha256_digest(start_settlement);
  const auto release_json = ReleaseJson("program_runner", "runner.1", "cycle.1",
                                        "start.1", "explicit_reset", evidence);
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto release = DecodeCellAdmissionClaimRelease(release_json);
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "terminal_reference TEXT,start_settlement_json TEXT,"
      "release_intent_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reset_phase TEXT);");
  fixture.Exec("INSERT INTO program_cycle VALUES(1,'resetting',"
               "'runner.1','cycle.1','" +
               claim_json + "','" + evidence + "','" + start_settlement +
               "','" + release_json + "',0,NULL,'releasing')");
  fixture.Close();
  EXPECT_TRUE(
      ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release)
          .ok());

  TemporaryDatabase missing_intent;
  missing_intent.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "terminal_reference TEXT,start_settlement_json TEXT,"
      "release_intent_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reset_phase TEXT);"
      "INSERT INTO program_cycle VALUES(1,'resetting','runner.1','cycle.1','" +
      claim_json + "','" + evidence + "','" + start_settlement +
      "',NULL,0,NULL,'releasing')");
  missing_intent.Close();
  EXPECT_FALSE(ValidateProgramCycleSettlementReadOnly(missing_intent.path(),
                                                      claim, release)
                   .ok());
}

TEST(OwnerSettlementReaders,
     ProgramResetJoinsTheExactRetainedTerminalRunSnapshot) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const std::string snapshot = "{\"capture\":{\"program_run_id\":\"run.1\"},"
                               "\"run_revision\":2,\"state\":\"stopped\"}";
  const auto evidence = content_identity::sha256_digest(
      "{\"program_run_id\":\"run.1\",\"run_revision\":2,"
      "\"state\":\"stopped\"}");
  const auto release_json = ReleaseJson("program_runner", "runner.1", "cycle.1",
                                        "start.1", "explicit_reset", evidence);
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto release = DecodeCellAdmissionClaimRelease(release_json);
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "terminal_reference TEXT,start_settlement_json TEXT,"
      "release_intent_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reset_phase TEXT);"
      "CREATE TABLE program_run_history(program_run_id TEXT PRIMARY KEY,"
      "run_revision INTEGER,snapshot_json TEXT,terminal INTEGER);");
  fixture.Exec("INSERT INTO program_cycle VALUES(1,'resetting',"
               "'runner.1','run.1','" +
               claim_json + "','" + evidence + "',NULL,'" + release_json +
               "',0,NULL,'releasing');"
               "INSERT INTO program_run_history VALUES('run.1',2,'" +
               snapshot + "',1)");
  fixture.Close();
  EXPECT_TRUE(
      ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release)
          .ok());
}

TEST(OwnerSettlementReaders,
     ProgramResetRequiresTheExactResettingReleasePhase) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "run.1", "start.1");
  const std::string snapshot = "{\"capture\":{\"program_run_id\":\"run.1\"},"
                               "\"run_revision\":2,\"state\":\"stopped\"}";
  const auto evidence = content_identity::sha256_digest(
      "{\"program_run_id\":\"run.1\",\"run_revision\":2,"
      "\"state\":\"stopped\"}");
  const auto release_json = ReleaseJson("program_runner", "runner.1", "run.1",
                                        "start.1", "explicit_reset", evidence);
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto release = DecodeCellAdmissionClaimRelease(release_json);
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "terminal_reference TEXT,start_settlement_json TEXT,"
      "release_intent_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reset_phase TEXT);"
      "CREATE TABLE program_run_history(program_run_id TEXT PRIMARY KEY,"
      "run_revision INTEGER,snapshot_json TEXT,terminal INTEGER);"
      "INSERT INTO program_cycle VALUES(1,'resetting','runner.1','run.1','" +
      claim_json + "','" + evidence + "',NULL,'" + release_json +
      "',0,NULL,'releasing');"
      "INSERT INTO program_run_history VALUES('run.1',2,'" +
      snapshot + "',1)");
  EXPECT_TRUE(
      ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release)
          .ok());

  fixture.Exec("UPDATE program_cycle SET reset_phase='stopping'");
  fixture.Close();
  EXPECT_FALSE(
      ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release)
          .ok());
}

TEST(OwnerSettlementReaders,
     ProgramRunnerIdleRotationRequiresExactProgramEvidence) {
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> current{
      {core::CellAdmissionOwnerRoleDTO::kProgramRunner,
       core::RequestOwnerEpoch("runner.1")}};
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> requested{
      {core::CellAdmissionOwnerRoleDTO::kProgramRunner,
       core::RequestOwnerEpoch("runner.2")}};
  const auto evidence =
      core::CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          core::CellAdmissionOwnerRoleDTO::kProgramRunner,
          core::RequestOwnerEpoch("runner.2"));
  ASSERT_TRUE(evidence.ok());
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "release_intent_json TEXT,terminal_reference TEXT,"
      "start_settlement_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reconciliation_evidence_digest TEXT);"
      "INSERT INTO program_cycle VALUES(1,'idle_after_reset','runner.1',"
      "NULL,NULL,NULL,NULL,NULL,1,NULL,'" +
      evidence.value().value() + "')");
  fixture.Close();
  const core::CellAdmissionReconcileDTO reconcile{
      core::RequestOwnerEpoch("kernel.1"), {}, requested, evidence.value()};
  EXPECT_TRUE(ValidateProgramRunnerOwnerReconciliationReadOnly(
                  fixture.path(), {}, current, reconcile)
                  .ok());
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto start_settlement =
      ProgramStartSettlementJson(claim_json, "cycle.1");
  const auto terminal_evidence =
      content_identity::sha256_digest(start_settlement);
  const auto release_json =
      ReleaseJson("program_runner", "runner.1", "cycle.1", "start.1",
                  "explicit_reset", terminal_evidence);
  TemporaryDatabase terminal;
  terminal.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "release_intent_json TEXT,terminal_reference TEXT,"
      "start_settlement_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reconciliation_evidence_digest TEXT);"
      "INSERT INTO program_cycle "
      "VALUES(1,'terminal_awaiting_reset','runner.1','cycle.1','" +
      claim_json + "','" + release_json + "','" + terminal_evidence + "','" +
      start_settlement + "',1,NULL,'" + evidence.value().value() + "')");
  terminal.Close();
  const auto validation = ValidateProgramRunnerOwnerReconciliationReadOnly(
      terminal.path(), {}, current, reconcile);
  ASSERT_TRUE(validation.ok());
  EXPECT_TRUE(validation.value().settlements.empty());

  core::CellAdmissionReconcileDTO held_reconcile = reconcile;
  held_reconcile.retained_claims = {claim};
  EXPECT_FALSE(ValidateProgramRunnerOwnerReconciliationReadOnly(
                   terminal.path(), {claim}, current, held_reconcile)
                   .ok());
}

TEST(OwnerSettlementReaders,
     ProgramRunnerTerminalHeldClaimAuthorizesOneExactSuccessorSettlement) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto start_settlement =
      ProgramStartSettlementJson(claim_json, "cycle.1");
  const auto terminal_evidence =
      content_identity::sha256_digest(start_settlement);
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> current{
      {core::CellAdmissionOwnerRoleDTO::kProgramRunner,
       core::RequestOwnerEpoch("runner.1")}};
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> requested{
      {core::CellAdmissionOwnerRoleDTO::kProgramRunner,
       core::RequestOwnerEpoch("runner.2")}};
  const auto evidence =
      core::CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          core::CellAdmissionOwnerRoleDTO::kProgramRunner,
          core::RequestOwnerEpoch("runner.2"));
  ASSERT_TRUE(evidence.ok());
  TemporaryDatabase terminal;
  terminal.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,7);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "release_intent_json TEXT,terminal_reference TEXT,"
      "start_settlement_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reconciliation_evidence_digest TEXT);"
      "INSERT INTO program_cycle "
      "VALUES(1,'terminal_awaiting_reset','runner.1','cycle.1','" +
      claim_json + "',NULL,'" + terminal_evidence + "','" + start_settlement +
      "',1,NULL,'" + evidence.value().value() + "')");
  const core::CellAdmissionReconcileDTO reconcile{
      core::RequestOwnerEpoch("kernel.1"),
      {claim},
      requested,
      evidence.value()};

  const auto validation = ValidateProgramRunnerOwnerReconciliationReadOnly(
      terminal.path(), {claim}, current, reconcile);

  ASSERT_TRUE(validation.ok());
  ASSERT_EQ(validation.value().settlements.size(), 1U);
  EXPECT_EQ(validation.value().settlements.front(),
            (core::CellAdmissionReconcileSettlementDTO{
                claim, core::CellAdmissionOwnerRoleDTO::kProgramRunner,
                core::RequestOwnerEpoch("runner.2"),
                core::CellAdmissionSettlementKindDTO::kReconciled,
                evidence.value()}));

  terminal.Exec("UPDATE program_cycle SET terminal_reference='" +
                std::string(kDigestB) + "'");
  terminal.Close();
  EXPECT_FALSE(ValidateProgramRunnerOwnerReconciliationReadOnly(
                   terminal.path(), {claim}, current, reconcile)
                   .ok());
}
} // namespace
} // namespace manyforge::roboplan_backend::detail
