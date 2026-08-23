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
            ("manyforge-owner-settlement-" + std::to_string(::getpid()) +
             "-" + std::to_string(++sequence_) + ".sqlite3");
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
                "\",\"draft_program_revision\":\"" +
                std::string(kDigestA) + "\"}"
          : "null";
  return "{\"accepted_at_ns\":1,\"acquired_admission_generation\":2,"
         "\"claim_generation\":3,\"intent\":{\"claim_kind\":\"" +
         std::string(kind) +
         "\",\"expected_admission_generation\":1,"
         "\"intervention_evidence\":null,\"owner_epoch\":\"" +
         std::string(owner_epoch) + "\",\"owner_role\":\"" +
         std::string(role) + "\",\"owner_transaction_id\":\"" +
         std::string(transaction) +
         "\",\"program_cycle_evidence\":" + program_evidence +
         ",\"request_id\":\"" +
         std::string(request) + "\",\"request_payload_digest\":\"" +
         std::string(kDigestA) +
         "\",\"scope\":{\"configurations\":[],\"resources\":[]}}}";
}

[[nodiscard]] std::string ReleaseJson(std::string_view role,
                                      std::string_view owner_epoch,
                                      std::string_view transaction,
                                      std::string_view request,
                                      std::string_view settlement,
                                      std::string_view evidence) {
  return "{\"acquired_admission_generation\":2,\"claim_generation\":3,"
         "\"owner_epoch\":\"" + std::string(owner_epoch) +
         "\",\"owner_role\":\"" + std::string(role) +
         "\",\"owner_transaction_id\":\"" + std::string(transaction) +
         "\",\"request_id\":\"" + std::string(request) +
         "\",\"request_payload_digest\":\"" + std::string(kDigestA) +
         "\",\"settlement\":{\"evidence_digest\":\"" +
         std::string(evidence) + "\",\"settlement_kind\":\"" +
         std::string(settlement) + "\"}}";
}

TEST(OwnerSettlementReaders,
     ProgramResetRequiresExactDurableTerminalClaimAndReleaseIntent) {
  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const auto release_json =
      ReleaseJson("program_runner", "runner.1", "cycle.1", "start.1",
                  "explicit_reset", kDigestB);
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto release = DecodeCellAdmissionClaimRelease(release_json);
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,1);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,claim_json TEXT,terminal_reference TEXT,"
      "release_intent_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT);");
  fixture.Exec("INSERT INTO program_cycle VALUES(1,'terminal_awaiting_reset',"
               "'runner.1','" + claim_json + "','" + std::string(kDigestB) +
               "','" + release_json + "',0,NULL)");
  fixture.Close();
  EXPECT_TRUE(
      ValidateProgramCycleSettlementReadOnly(fixture.path(), claim, release)
          .ok());

  TemporaryDatabase missing_intent;
  missing_intent.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,1);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,claim_json TEXT,terminal_reference TEXT,"
      "release_intent_json TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT); INSERT INTO program_cycle VALUES(1,"
      "'terminal_awaiting_reset','runner.1','" + claim_json + "','" +
      std::string(kDigestB) + "',NULL,0,NULL)");
  missing_intent.Close();
  EXPECT_FALSE(ValidateProgramCycleSettlementReadOnly(missing_intent.path(),
                                                       claim, release)
                   .ok());
}

TEST(OwnerSettlementReaders,
     AuthoringCommitRequiresExactOutcomeDigestAndNoPrematureReceipt) {
  const auto claim_json = ClaimJson("authoring_mutation", "authoring_owner",
                                    "author.1", "author.tx.1", "author.tx.1");
  const std::string outcome =
      "{\"body\":{\"status\":\"saved\"},\"kind\":\"mapping\"}";
  const std::string preimage =
      "{\"outcome\":" + outcome +
      ",\"owner_transaction_id\":\"author.tx.1\","
      "\"settlement_kind\":\"owner_commit\"}";
  const auto evidence = content_identity::sha256_digest(preimage);
  const auto release_json =
      ReleaseJson("authoring_owner", "author.1", "author.tx.1",
                  "author.tx.1", "owner_commit", evidence);
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto release = DecodeCellAdmissionClaimRelease(release_json);
  const std::string release_intent =
      "{\"evidence_digest\":\"" + evidence +
      "\",\"settlement_kind\":\"owner_commit\"}";
  TemporaryDatabase fixture;
  fixture.Exec(
      "PRAGMA user_version=1; CREATE TABLE transactions(owner_transaction_id "
      "TEXT,owner_epoch TEXT,claim_json TEXT,effect_intent_json TEXT,"
      "outcome_json TEXT,post_image_json TEXT,release_intent_json TEXT,"
      "release_receipt_json TEXT); INSERT INTO transactions VALUES("
      "'author.tx.1','author.1','" + claim_json +
      "','{\"effect_class\":\"local\"}','" + outcome +
      "','{\"draft_revision\":2}','" + release_intent + "',NULL)");
  fixture.Close();
  EXPECT_TRUE(
      ValidateAuthoringSettlementReadOnly(fixture.path(), claim, release).ok());

  TemporaryDatabase premature_receipt;
  premature_receipt.Exec(
      "PRAGMA user_version=1; CREATE TABLE transactions(owner_transaction_id "
      "TEXT,owner_epoch TEXT,claim_json TEXT,effect_intent_json TEXT,"
      "outcome_json TEXT,post_image_json TEXT,release_intent_json TEXT,"
      "release_receipt_json TEXT); INSERT INTO transactions VALUES("
      "'author.tx.1','author.1','" + claim_json +
      "','{\"effect_class\":\"local\"}','" + outcome +
      "','{\"draft_revision\":2}','" + release_intent +
      "','{\"status\":\"released_exact\"}')");
  premature_receipt.Close();
  EXPECT_FALSE(ValidateAuthoringSettlementReadOnly(premature_receipt.path(),
                                                    claim, release)
                   .ok());
}

TEST(OwnerSettlementReaders,
     AuthoringIdleRotationRequiresExactActiveSuccessorAndRoleEvidence) {
  TemporaryDatabase fixture;
  fixture.Exec(
      "PRAGMA user_version=1;"
      "CREATE TABLE owner_epochs(owner_epoch TEXT,active INTEGER);"
      "INSERT INTO owner_epochs VALUES('author.2',1);"
      "CREATE TABLE transactions(owner_epoch TEXT,release_receipt_json TEXT);");
  fixture.Close();
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> current{
      {core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
       core::RequestOwnerEpoch("author.1")}};
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> requested{
      {core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
       core::RequestOwnerEpoch("author.2")}};
  const auto evidence =
      core::CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          core::RequestOwnerEpoch("author.2"));
  ASSERT_TRUE(evidence.ok());
  core::CellAdmissionReconcileDTO reconcile{
      core::RequestOwnerEpoch("kernel.1"), {}, requested, evidence.value()};
  EXPECT_TRUE(ValidateAuthoringOwnerReconciliationReadOnly(
                  fixture.path(), {}, current, reconcile)
                  .ok());

  reconcile.reconciliation_evidence_digest =
      core::ContentDigest(std::string(kDigestA));
  EXPECT_FALSE(ValidateAuthoringOwnerReconciliationReadOnly(
                   fixture.path(), {}, current, reconcile)
                   .ok());
}

TEST(OwnerSettlementReaders,
     ProgramRunnerIdleRotationCannotBeAuthorizedByAuthoringJournal) {
  TemporaryDatabase fixture;
  fixture.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,1);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,claim_json TEXT,release_intent_json TEXT,"
      "terminal_reference TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT);"
      "INSERT INTO program_cycle VALUES(1,'idle_after_reset','runner.2',"
      "NULL,NULL,NULL,0,NULL)");
  fixture.Close();
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
      core::RequestOwnerEpoch("kernel.1"), {}, requested, evidence.value()};
  EXPECT_TRUE(ValidateProgramRunnerOwnerReconciliationReadOnly(
                  fixture.path(), {}, current, reconcile)
                  .ok());
  EXPECT_FALSE(ValidateAuthoringOwnerReconciliationReadOnly(
                   fixture.path(), {}, current, reconcile)
                   .ok());

  const auto claim_json = ClaimJson("program_cycle", "program_runner",
                                    "runner.1", "cycle.1", "start.1");
  const auto claim = DecodeCellAdmissionClaim(claim_json);
  const auto release_json =
      ReleaseJson("program_runner", "runner.1", "cycle.1", "start.1",
                  "explicit_reset", kDigestB);
  TemporaryDatabase terminal;
  terminal.Exec(
      "CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY,"
      "schema_version INTEGER); INSERT INTO program_cycle_metadata VALUES(1,1);"
      "CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY,state TEXT,"
      "owner_epoch TEXT,claim_json TEXT,release_intent_json TEXT,"
      "terminal_reference TEXT,reconciliation_required INTEGER,"
      "acquisition_pending_json TEXT);"
      "INSERT INTO program_cycle VALUES(1,'terminal_awaiting_reset','runner.2','" +
      claim_json + "','" + release_json + "','" + std::string(kDigestB) +
      "',0,NULL)");
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
} // namespace
} // namespace manyforge::roboplan_backend::detail
