#include <manyforge_core/testing/request_key_fixture.hpp>
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using namespace manyforge::core;

constexpr const char* kDigest =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-cell-admission-sqlite-" +
             std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
             std::to_string(next.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create cell-admission SQLite test root");
    }
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class FixedClock final : public IClock {
 public:
  TimePoint now() const override { return 10; }
};

CellAdmissionClaimDTO Claim() {
  return CellAdmissionClaimDTO{
      CellAdmissionClaimIntentDTO{
          CellAdmissionClaimKindDTO::kAuthoringMutation, ApplicationRole::kAuthoringOwner,
          std::nullopt, TestRequestKey("request.1"), AdmissionGeneration(1U),
          CellAdmissionScopeDTO{
              {ResourceId("resource.1")},
              {CellAdmissionConfigurationScopeDTO{ControlDomainId("domain.1"),
                                                  ActiveControlConfigurationId("configuration.1"),
                                                  ActiveControlConfigurationGeneration(1U)}}},
          std::nullopt, std::nullopt},
      CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U), 10};
}

CellAdmissionClaimReleaseDTO Release(const CellAdmissionClaimDTO& claim) {
  const auto settlement =
      claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle ||
              claim.intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation
          ? CellAdmissionSettlementKindDTO::kExplicitReset
          : (claim.intent.claim_kind == CellAdmissionClaimKindDTO::kAuthoringMutation ||
                     claim.intent.claim_kind == CellAdmissionClaimKindDTO::kAuthoredScene
                 ? CellAdmissionSettlementKindDTO::kOwnerCommit
                 : CellAdmissionSettlementKindDTO::kTerminalEffectsSettled);
  return CellAdmissionClaimReleaseDTO{
      claim.intent.owner_role,
      claim.intent.owner_transaction_id,
      claim.intent.request_key,
      claim.acquired_admission_generation,
      claim.claim_generation,
      CellAdmissionSettlementEvidenceDTO{settlement, ContentDigest(kDigest)}};
}

ManagerLivenessRequiredSetDTO EmptyLivenessSet() {
  ManagerLivenessRequiredSetDTO result{CellId("cell.1"),
                                       RetainedBundleId("bundle.1"),
                                       ContentDigest(kDigest),
                                       RuntimeUnitId("manager"),
                                       {}};
  result.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(result.required_receivers);
  return result;
}

CellAdmissionReadinessSnapshotDTO Ready() {
  return CellAdmissionReadinessSnapshotDTO{
      true,
      ProgramCycleStatusDTO{1U,
                            ProgramCycleStateDTO::kIdleAfterReset, std::nullopt, std::nullopt,
                            std::nullopt},
      true,
      1U,
      1U,
      true,
      true,
      0U,
      0U,
      0U};
}

std::int64_t CountRows(const std::filesystem::path& database_path, const char* table) {
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(database_path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    throw std::runtime_error("failed to open cell-admission database for row count");
  }
  sqlite3_stmt* statement = nullptr;
  const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
      sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    sqlite3_close(database);
    throw std::runtime_error("failed to count cell-admission rows");
  }
  const auto result = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return result;
}

CellAdmissionClaimDTO ConfigurationClaim(std::uint64_t generation, std::uint64_t admission,
                                         std::string request) {
  auto claim = Claim();
  claim.intent.claim_kind = CellAdmissionClaimKindDTO::kManagerApply;
  claim.intent.owner_role = ApplicationRole::kManager;
  claim.intent.owner_transaction_id = ApplyId(generation);
  claim.intent.request_key = TestRequestKey(std::move(request));
  claim.intent.scope = {};
  claim.intent.expected_admission_generation = AdmissionGeneration(admission);
  claim.claim_generation = CellAdmissionClaimGeneration(generation);
  claim.acquired_admission_generation = AdmissionGeneration(admission);
  return claim;
}

// Use SQLite's real commit hook, without a product fault injector: the armed
// hook rolls back the next COMMIT. Installation/removal is scoped to this
// serial test invocation.
struct ConfigurationCommitFailure final {
  bool armed{};
  int commit_failures{};
};
thread_local ConfigurationCommitFailure* configuration_commit_failure = nullptr;
int InstallConfigurationCommitFailure(sqlite3* database, char**, const sqlite3_api_routines*) {
  auto* control = configuration_commit_failure;
  if (!control) return SQLITE_OK;
  sqlite3_commit_hook(
      database,
      [](void* value) {
        auto& failure = *static_cast<ConfigurationCommitFailure*>(value);
        if (!failure.armed) return 0;
        failure.armed = false;
        ++failure.commit_failures;
        return 1;
      },
      control);
  return SQLITE_OK;
}
class ScopedConfigurationCommitFailure final {
 public:
  explicit ScopedConfigurationCommitFailure(ConfigurationCommitFailure& value) {
    configuration_commit_failure = &value;
    if (sqlite3_auto_extension(reinterpret_cast<void (*)()>(InstallConfigurationCommitFailure)) !=
        SQLITE_OK)
      throw std::runtime_error("cannot install scoped SQLite commit-failure hook");
  }
  ~ScopedConfigurationCommitFailure() {
    sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(InstallConfigurationCommitFailure));
    configuration_commit_failure = nullptr;
  }
};

/// Hands each posted record to the journal at once, as the Kernel's writer
/// later would; the writer's outcome never reaches the owner.
std::function<void(std::span<JournalRecord>)> WriteThrough(SqliteOperationJournal& journal,
                                                           JournalBatchResult* last = nullptr) {
  return [&journal, last](const std::span<JournalRecord> records) {
    auto result = journal.WriteBatch(records, 0U);
    if (last != nullptr) *last = std::move(result);
  };
}

// The claim takes effect in memory; its record is written behind, and a
// failed write does not refuse it.
TEST(KernelCellAdmissionSqlite, ConfigurationApplyClaimIsNeverRefusedForStorage) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  ConfigurationCommitFailure failure;
  ScopedConfigurationCommitFailure hook(failure);
  SqliteOperationJournal journal(database, CellId("cell.1"));
  ASSERT_TRUE(journal.BeginStart(kTestApplicationStartId, 1, 8U).ok());
  JournalBatchResult written;
  KernelCellAdmissionOwner owner(
      8U, clock, WriteThrough(journal, &written), {},
      [](const auto&, const auto&) { return Result<void>::Ok(); }, EmptyLivenessSet());
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto intent =
      ConfigurationClaim(1U, owner.GetAuthoringAdmission().admission_generation.value(),
                         "failed.commit")
          .intent;
  failure.armed = true;
  const auto result = owner.AcquireConfigurationApplyClaim(
      intent, [](const auto&, auto, const auto&) { return Result<void>::Ok(); });
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(failure.commit_failures, 1);
  ASSERT_EQ(written.failures.size(), 1U);
  EXPECT_TRUE(owner.has_unresolved_claims());
  journal.Close();
  EXPECT_EQ(CountRows(database, "journal_records"), 0);
}

TEST(KernelCellAdmissionSqlite, V12ClaimsAreRefusedNotConvertedAfterTheStopTimingFormat) {
  // A journal of an earlier format (here v12, before the attribution
  // columns and the per-robot Stop timing) is refused, never migrated.
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const auto claim = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginStart(kTestApplicationStartId, 1, 8U).ok());
    std::array records{JournalRecord{JournalCellClaimAcquired{claim}, "test"}};
    ASSERT_TRUE(journal.WriteBatch(records, 0U).failures.empty());
  }
  sqlite3* legacy = nullptr;
  ASSERT_EQ(sqlite3_open(database.c_str(), &legacy), SQLITE_OK);
  const auto downgraded =
      sqlite3_exec(legacy,
                   "PRAGMA user_version=12; UPDATE journal_metadata SET schema_version=12,"
                   "schema_id='manyforge.internal.execution.operation-journal.v12';",
                   nullptr, nullptr, nullptr);
  sqlite3_close(legacy);
  ASSERT_EQ(downgraded, SQLITE_OK);
  EXPECT_THROW(SqliteOperationJournal(database, CellId("cell.1")), std::runtime_error);
}

TEST(KernelCellAdmissionSqlite, SameOwnerCapacityPlusOneCyclesRetireReleasedResult) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  std::optional<CellAdmissionClaimDTO> latest;
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginStart(kTestApplicationStartId, 1, 1U).ok());
    KernelCellAdmissionOwner owner(
        1U, clock, WriteThrough(journal), {},
        [](const auto&, const auto&) { return Result<void>::Ok(); }, EmptyLivenessSet());
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    for (std::size_t index = 0; index != 3U; ++index) {
      auto intent = Claim().intent;
      intent.request_key = TestRequestKey("cycle." + std::to_string(index));
      intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
      auto claim = owner.AcquireCellAdmissionClaim(intent);
      ASSERT_TRUE(claim.ok());
      if (latest.has_value()) {
        const auto old =
            owner.GetCellAdmissionRecoveryReceipt({latest->intent, *latest, Release(*latest)});
        ASSERT_TRUE(old.ok());
        EXPECT_EQ(old.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
      }
      ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(claim.value())).ok());
      const auto exact = owner.GetCellAdmissionRecoveryReceipt(
          {claim.value().intent, claim.value(), Release(claim.value())});
      ASSERT_TRUE(exact.ok());
      EXPECT_EQ(exact.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
      latest = claim.value();
    }
  }
  // Every cycle wrote its acquired and released rows; none is unresolved.
  EXPECT_EQ(CountRows(database, "journal_records WHERE record_kind='cell_claim_acquired'"), 3);
  EXPECT_EQ(CountRows(database, "journal_records WHERE record_kind='cell_claim_released'"), 3);
}

// Contract 1.15.2: a release carries the key of the request that causes it.
// The release row names its claim by the claim's generation and records the
// release's own key; a release that does not match its claim is refused.
TEST(KernelCellAdmissionSqlite, ReleaseRecordsTheCausingRequestKey) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  SqliteOperationJournal journal(database, CellId("cell.1"));
  ASSERT_TRUE(journal.BeginStart(kTestApplicationStartId, 1, 4U).ok());
  const auto claim = Claim();
  auto release = Release(claim);
  release.request_key = TestRequestKey("causing.request");
  ASSERT_NE(release.request_key, claim.intent.request_key);
  auto wrong_generation = release;
  wrong_generation.claim_generation = CellAdmissionClaimGeneration(9U);
  std::array records{JournalRecord{JournalCellClaimAcquired{claim}, "test"},
                     JournalRecord{JournalCellClaimReleased{claim, wrong_generation, 11}, "test"},
                     JournalRecord{JournalCellClaimReleased{claim, release, 12}, "test"}};
  const auto written = journal.WriteBatch(records, 0U);
  ASSERT_EQ(written.failures.size(), 1U);
  EXPECT_EQ(written.failures.front().index, 1U);
  EXPECT_EQ(written.failures.front().error.code, "operation_journal_record_invalid");
  journal.Close();

  sqlite3* reader = nullptr;
  ASSERT_EQ(sqlite3_open_v2(database.c_str(), &reader, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(reader,
                               "SELECT json_extract(payload_json,'$.release_request_key'),"
                               "json_extract(payload_json,'$.claim_generation') "
                               "FROM journal_records WHERE record_kind='cell_claim_released'",
                               -1, &statement, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))),
            RequestKeyText(TestRequestKey("causing.request")));
  EXPECT_EQ(sqlite3_column_int64(statement, 1), 1);
  EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(reader);
}

}  // namespace
