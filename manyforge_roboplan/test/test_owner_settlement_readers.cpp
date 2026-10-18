#include "cell_admission_command_codec.hpp"
#include "owner_settlement_readers.hpp"
#include "applied_program_owner_reader.hpp"
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>

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
                           std::string_view program_run_id,
                           std::string_view prepared_scene =
                               "{\"schema_version\":"
                               "\"manyforge.prepared_program_scene.v1\"}") {
  return "{\"claim\":" + std::string(claim_json) +
         ",\"kind\":\"proven_no_effect\",\"prepared_scene\":" +
         std::string(prepared_scene) + ",\"program_run_id\":\"" +
         std::string(program_run_id) +
         "\",\"schema_version\":"
         "\"manyforge.program_cycle_start_settlement.v1\"}";
}

TEST(OwnerSettlementReaders,
     AppliedProgramOwnerReadRequiresActualNamedApplyWithoutCreatingState) {
  TemporaryDatabase name;
  name.Close();
  const auto root = std::filesystem::path(name.path().string() + ".state");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{root};
  const core::CellId cell("cell.applied-reader");
  const auto absent = ReadAppliedProgramOwnerReadOnly(
      root, cell, core::ActiveBundlePublicationGeneration(1U));
  ASSERT_FALSE(absent.ok());
  EXPECT_FALSE(std::filesystem::exists(root));
  ASSERT_TRUE(std::filesystem::create_directory(root));
  component_manager::SqliteActiveBundlePublicationStore store(
      root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      cell);
  const core::ProjectionRevisionDTO projection{
      core::RetainedBundleId(std::string(kDigestA)),
      core::GraphRevision(std::string(kDigestB)),
      core::SceneRevision{core::SceneId(std::string(kDigestA)), 1U,
                          core::ContentDigest(std::string(kDigestB))}};
  const auto published = store.PublishActiveBundle(
      {core::BootstrapPublicationTransactionDTO{
           core::BootstrapId("bootstrap.reader"),
           core::BootstrapGeneration(1U)},
       projection, projection.retained_bundle_id, projection.graph_revision,
       std::nullopt, core::ContentDigest(std::string(kDigestA))});
  const auto unbound = ReadAppliedProgramOwnerReadOnly(
      root, cell, published.publication_generation);
  ASSERT_FALSE(unbound.ok());
  EXPECT_EQ(unbound.error().details.at("reason"),
            "program_bb_applied_program_unbound");
  EXPECT_FALSE(std::filesystem::exists(
      root / std::string(component_manager::kManagerApplyJournalFilename)));
  const auto stale = ReadAppliedProgramOwnerReadOnly(
      root, cell, core::ActiveBundlePublicationGeneration(2U));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"),
            "program_bb_applied_publication_stale");
}

TEST(OwnerSettlementReaders,
     ActiveProgramRunRecoveryReturnsItsExactCapturedProduct) {
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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

  const auto current = journal(8);
  ASSERT_TRUE(current.ok());
  ASSERT_TRUE(current.value().has_value());
  EXPECT_EQ(current.value()->program_run_id, core::ProgramRunId("run.7"));
  EXPECT_FALSE(journal(6).ok());
  EXPECT_FALSE(journal(3).ok());
  EXPECT_FALSE(journal(7).ok());
  EXPECT_FALSE(journal(9).ok());
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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
        "INSERT INTO program_cycle_metadata VALUES(1,8);"
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
     NoSceneFailedStartResetAcceptsOnlyEmptyOrLegacySceneEvidence) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  struct Case {
    const char *scene;
    bool accepted;
  };
  for (const auto &item :
       {Case{"{}", true},
        Case{"{\"schema_version\":\"manyforge.prepared_program_scene.v1\"}", true},
        Case{"{\"unexpected\":true}", false},
        Case{"{\"schema_version\":\"foreign\"}", false},
        Case{"[]", false}, Case{"null", false}}) {
    SCOPED_TRACE(item.scene);
    const auto settlement =
        ProgramStartSettlementJson(claim_json, "cycle.1", item.scene);
    // Each case has its own authentic digest, so malformed scene evidence
    // cannot be rejected merely because the terminal reference is stale.
    const auto evidence = content_identity::sha256_digest(settlement);
    const auto release_json =
        ReleaseJson("program_runner", "runner.1", "cycle.1", "start.1",
                    "explicit_reset", evidence);
    const auto release = DecodeCellAdmissionClaimRelease(release_json);
    TemporaryDatabase fixture;
    fixture.Exec(
        "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
        "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
        "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
        "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
        "terminal_reference TEXT,start_settlement_json TEXT,"
        "release_intent_json TEXT,reconciliation_required INTEGER,"
        "acquisition_pending_json TEXT,reset_phase TEXT);"
        "INSERT INTO program_cycle VALUES(1,'resetting','runner.1','cycle.1','" +
        claim_json + "','" + evidence + "','" + settlement + "','" +
        release_json + "',0,NULL,'releasing')");
    const auto result =
        ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release);
    EXPECT_EQ(result.ok(), item.accepted);
    if (item.accepted) {
      const auto foreign_release = DecodeCellAdmissionClaimRelease(
          ReleaseJson("program_runner", "runner.1", "cycle.1", "foreign.start",
                      "explicit_reset", evidence));
      EXPECT_FALSE(ValidateProgramCycleSettlementReadOnly(
                       fixture.path(), claim, foreign_release)
                       .ok());
      fixture.Exec("UPDATE program_cycle SET terminal_reference='" +
                   std::string(kDigestB) + "'");
      EXPECT_FALSE(ValidateProgramCycleSettlementReadOnly(
                       fixture.path(), claim, release)
                       .ok());
    }
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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

  fixture.Exec("UPDATE program_cycle_metadata SET schema_version=7");
  EXPECT_FALSE(
      ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release)
          .ok());
  fixture.Exec("UPDATE program_cycle_metadata SET schema_version=8");

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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
      "release_intent_json TEXT,terminal_reference TEXT,"
      "start_settlement_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT,reconciliation_evidence_digest TEXT);"
      "INSERT INTO program_cycle VALUES(1,'idle_after_reset','runner.1',"
      "NULL,NULL,NULL,NULL,NULL,1,NULL,'" +
      evidence.value().value() + "')");
  const core::CellAdmissionReconcileDTO reconcile{
      core::RequestOwnerEpoch("kernel.1"), {}, requested, evidence.value()};
  EXPECT_TRUE(ValidateProgramRunnerOwnerReconciliationReadOnly(
                  fixture.path(), {}, current, reconcile)
                  .ok());
  fixture.Exec("UPDATE program_cycle_metadata SET schema_version=7");
  EXPECT_FALSE(ValidateProgramRunnerOwnerReconciliationReadOnly(
                   fixture.path(), {}, current, reconcile)
                   .ok());
  fixture.Close();
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,8);"
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

TEST(OwnerSettlementReaders,
     NoSceneOwnerReconciliationRequiresExactEmbeddedClaimAndRun) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const auto foreign_claim = ClaimJson("program_cycle", "program_runner",
                                       "runner.1", "cycle.1", "foreign.start");
  const auto claim = DecodeCellAdmissionClaim(claim_json);
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
  const core::CellAdmissionReconcileDTO reconcile{
      core::RequestOwnerEpoch("kernel.1"),
      {claim},
      requested,
      evidence.value()};
  for (const auto *kind : {"proven_no_effect", "terminal_effects_settled"}) {
    for (const auto *variant : {"exact", "foreign_claim", "foreign_run",
                                "malformed_scene", "foreign_terminal"}) {
      SCOPED_TRACE(kind);
      SCOPED_TRACE(variant);
      const std::string which(variant);
      const std::string state(kind);
      if (which == "foreign_terminal" && state == "proven_no_effect")
        continue;
      const auto settlement =
          "{\"claim\":" +
          (which == "foreign_claim" ? foreign_claim : claim_json) +
          ",\"kind\":\"" + state + "\",\"prepared_scene\":" +
          (which == "malformed_scene" ? "{\"unexpected\":true}" : "{}") +
          ",\"program_run_id\":\"" +
          (which == "foreign_run" ? "foreign.cycle" : "cycle.1") +
          "\",\"schema_version\":\"manyforge.program_cycle_start_settlement."
          "v1\"" +
          (state == "terminal_effects_settled"
               ? ",\"terminal_run\":{\"capture\":{\"program_run_id\":\"" +
                     std::string(which == "foreign_terminal" ? "foreign.cycle"
                                                             : "cycle.1") +
                     "\"},\"state\":\"stopped\"}"
               : "") +
          "}";
      // Recompute the genuine reference: every negative targets its embedded
      // identity/scene grammar, rather than failing on a stale outer digest.
      const auto reference = content_identity::sha256_digest(settlement);
      TemporaryDatabase fixture;
      fixture.Exec(
          "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
          "schema_version INTEGER); INSERT INTO program_cycle_metadata "
          "VALUES(1,8);"
          "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
          "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
          "release_intent_json TEXT,terminal_reference TEXT,"
          "start_settlement_json TEXT,reconciliation_required INTEGER,"
          "acquisition_pending_json TEXT,reconciliation_evidence_digest TEXT);"
          "INSERT INTO program_cycle VALUES(1,'terminal_awaiting_reset',"
          "'runner.1','cycle.1','" +
          claim_json + "',NULL,'" + reference + "','" + settlement +
          "',1,NULL,'" + evidence.value().value() + "')");
      const auto validation = ValidateProgramRunnerOwnerReconciliationReadOnly(
          fixture.path(), {claim}, current, reconcile);
      const bool accepted = which == "exact";
      ASSERT_EQ(validation.ok(), accepted);
      if (accepted) {
        ASSERT_EQ(validation.value().settlements.size(), 1U);
        EXPECT_EQ(validation.value().settlements.front(),
                  (core::CellAdmissionReconcileSettlementDTO{
                      claim, core::CellAdmissionOwnerRoleDTO::kProgramRunner,
                      core::RequestOwnerEpoch("runner.2"),
                      core::CellAdmissionSettlementKindDTO::kReconciled,
                      evidence.value()}));
      }
    }
  }
}

TEST(OwnerSettlementReaders,
     NoSceneTerminalEffectsResetRequiresExactClaimRunAndTerminal) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const auto foreign_claim = ClaimJson("program_cycle", "program_runner",
                                       "runner.1", "cycle.1", "foreign.start");
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  for (const auto *variant : {"stopped", "succeeded", "failed", "running",
                              "foreign_claim", "foreign_run", "foreign_capture",
                              "missing_capture", "malformed_scene"}) {
    SCOPED_TRACE(variant);
    const std::string which(variant);
    const auto terminal =
        which == "missing_capture"
            ? "{\"state\":\"stopped\"}"
            : "{\"capture\":{\"program_run_id\":\"" +
                  std::string(which == "foreign_capture" ? "foreign.cycle"
                                                         : "cycle.1") +
                  "\"},\"state\":\"" +
                  (which == "succeeded" || which == "failed" ||
                           which == "running"
                       ? which
                       : "stopped") +
                  "\"}";
    const auto settlement =
        "{\"claim\":" +
        (which == "foreign_claim" ? foreign_claim : claim_json) +
        ",\"kind\":\"terminal_effects_settled\",\"prepared_scene\":" +
        (which == "malformed_scene" ? "{\"unexpected\":true}" : "{}") +
        ",\"program_run_id\":\"" +
        (which == "foreign_run" ? "foreign.cycle" : "cycle.1") +
        "\",\"schema_version\":\"manyforge.program_cycle_start_settlement.v1\","
        "\"terminal_run\":" +
        terminal + "}";
    const auto reference = content_identity::sha256_digest(settlement);
    const auto release_json =
        ReleaseJson("program_runner", "runner.1", "cycle.1", "start.1",
                    "explicit_reset", reference);
    const auto release = DecodeCellAdmissionClaimRelease(release_json);
    TemporaryDatabase fixture;
    fixture.Exec(
        "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
        "schema_version INTEGER); INSERT INTO program_cycle_metadata "
        "VALUES(1,8);"
        "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
        "owner_epoch TEXT,program_run_id TEXT,claim_json TEXT,"
        "terminal_reference TEXT,start_settlement_json TEXT,"
        "release_intent_json TEXT,reconciliation_required INTEGER,"
        "acquisition_pending_json TEXT,reset_phase TEXT);"
        "INSERT INTO program_cycle "
        "VALUES(1,'resetting','runner.1','cycle.1','" +
        claim_json + "','" + reference + "','" + settlement + "','" +
        release_json + "',0,NULL,'releasing')");
    EXPECT_EQ(
        ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release)
            .ok(),
        which == "stopped" || which == "succeeded" || which == "failed");
  }
}
TEST(OwnerSettlementReaders, IdleResetReleaseRequiresItsExactAcceptedJournalOccurrence) {
  using Value = content_identity::CanonicalValue;
  for (const bool failed_start : {false, true}) {
  const Value original(Value::Object{
      {"request_owner_epoch", Value("operator.1")}, {"request_id", Value("reset.original")},
      {"expected_owner_epoch", Value("runner.1")}, {"expected_cycle_generation", Value(3.0)},
      {"program_run_id", failed_start ? Value("failed-start.1") : Value(nullptr)}, {"reload_initial_scene", Value(true)},
      {"reset_robot_position", Value(true)}});
  const auto request_json = content_identity::canonical_json(original);
  const auto request_digest = content_identity::sha256_digest(request_json);
  const auto start_settlement = content_identity::canonical_json(Value(Value::Object{
      {"schema_version", Value("manyforge.program_cycle_start_settlement.v1")},
      {"kind", Value("proven_no_effect")}, {"claim", Value(nullptr)},
      {"program_run_id", Value("failed-start.1")}, {"prepared_scene", Value(Value::Object{})}}));
  const auto terminal_reference = content_identity::sha256_digest(start_settlement);
  const core::ProgramCycleStatusDTO status{
      core::RequestOwnerEpoch("runner.1"), 3U, core::ProgramCycleStateDTO::kResetting,
      failed_start ? std::optional(core::ProgramRunId("failed-start.1")) : std::nullopt,
      std::nullopt, failed_start ? std::optional(core::ContentDigest(terminal_reference)) : std::nullopt,
      core::AcceptedProgramResetDTO{core::RequestOwnerEpoch("operator.1"),
          core::RequestId("reset.original"), core::ContentDigest(request_digest), true, true,
          core::CellAdmissionClaimGeneration(3U)}};
  const auto identity = core::ProgramResetClaimRequestId(status);
  ASSERT_TRUE(identity.ok());
  auto claim_json = ClaimJson("program_reset", "program_runner", "runner.1",
                             identity.value().value(), identity.value().value());
  claim_json.replace(claim_json.find(kDigestA), kDigestA.size(), request_digest);
  const std::string expected("\"expected_admission_generation\":1");
  claim_json.replace(claim_json.find(expected), expected.size(), "\"expected_admission_generation\":2");
  const auto claim_value = content_identity::parse_canonical_json(claim_json);
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto evidence = content_identity::sha256_digest(content_identity::canonical_json(Value(Value::Object{
      {"schema_version", Value("manyforge.program_reset_effect_settlement.v1")},
      {"cycle_generation", Value(3.0)}, {"request_owner_epoch", Value("operator.1")},
      {"request_id", Value("reset.original")}, {"request_payload_digest", Value(request_digest)},
      {"claim", claim_value}})));
  auto release_json = ReleaseJson("program_runner", "runner.1", identity.value().value(),
                                  identity.value().value(), "explicit_reset", evidence);
  release_json.replace(release_json.find(kDigestA), kDigestA.size(), request_digest);
  const auto release = DecodeCellAdmissionClaimRelease(release_json);
  const auto registered = content_identity::canonical_json(Value(Value::Object{
      {"intent", *claim_value.find_object_member("intent")}, {"claim", claim_value},
      {"release_intent", content_identity::parse_canonical_json(release_json)}}));
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,schema_version INTEGER);"
      "INSERT INTO program_cycle_metadata VALUES(1,8);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,owner_epoch TEXT,"
      "cycle_generation INTEGER,program_run_id TEXT,claim_json TEXT,terminal_reference TEXT,"
      "reconciliation_required INTEGER,acquisition_pending_json TEXT,reset_phase TEXT,reset_effect_claim_json TEXT,"
      "start_settlement_json TEXT,release_intent_json TEXT);"
      "INSERT INTO program_cycle VALUES(1,'resetting','runner.1',3," +
      (failed_start ? "'failed-start.1',NULL,'" + terminal_reference + "'" : "NULL,NULL,NULL") +
      ",0,NULL,'releasing','" + registered + "'," +
      (failed_start ? "'" + start_settlement + "'" : "NULL") + ",NULL);"
      "CREATE TABLE program_cycle_inflight_commands(request_owner_epoch TEXT,request_id TEXT,"
      "request_kind TEXT,intent_json TEXT,cycle_generation INTEGER);"
      "INSERT INTO program_cycle_inflight_commands VALUES('operator.1','reset.original','reset','" + request_json + "',3)");
  EXPECT_TRUE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
  if (failed_start) {
    fixture.Exec("UPDATE program_cycle SET start_settlement_json=NULL");
    EXPECT_FALSE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
    fixture.Exec("UPDATE program_cycle SET start_settlement_json='" + start_settlement + "',terminal_reference='" + std::string(kDigestA) + "'");
    EXPECT_FALSE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
    fixture.Exec("UPDATE program_cycle SET terminal_reference='" + terminal_reference + "'");
  }
  for (const auto* phase : {"restoring_scene", "restoring_position"}) {
    fixture.Exec("UPDATE program_cycle SET reset_phase='" + std::string(phase) + "'");
    EXPECT_FALSE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
  }
  fixture.Exec("UPDATE program_cycle SET reset_phase='releasing'; UPDATE program_cycle_metadata SET schema_version=7");
  EXPECT_FALSE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
  fixture.Exec("UPDATE program_cycle_metadata SET schema_version=8; UPDATE program_cycle SET reconciliation_required=1");
  EXPECT_FALSE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
  fixture.Exec("UPDATE program_cycle SET reconciliation_required=0; UPDATE program_cycle_inflight_commands SET request_id='foreign'");
  EXPECT_FALSE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
  fixture.Exec("UPDATE program_cycle_inflight_commands SET request_id='reset.original',cycle_generation=4");
  EXPECT_FALSE(ValidateProgramResetSettlementReadOnly(fixture.path(), claim, release).ok());
  }
}

} // namespace
} // namespace manyforge::roboplan_backend::detail
