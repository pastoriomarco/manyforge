#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <stdexcept>
#include <system_error>

namespace {

using namespace manyforge::core;

constexpr const char* kDigest =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char* kDigestB =
    "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

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
          CellAdmissionClaimKindDTO::kAuthoringMutation,
          CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          RequestOwnerEpoch("authoring.owner.1"),
          "document.transaction.1",
          RequestId("request.1"),
          ContentDigest(kDigest),
          AdmissionGeneration(1U),
          CellAdmissionScopeDTO{
              {ResourceId("resource.1")},
              {CellAdmissionConfigurationScopeDTO{
                  ControlDomainId("domain.1"), ActiveControlConfigurationId("configuration.1"),
                  ActiveControlConfigurationGeneration(1U)}}},
          std::nullopt,
          std::nullopt},
      CellAdmissionClaimGeneration(1U),
      AdmissionGeneration(1U),
      10};
}

CellAdmissionClaimReleaseDTO Release(const CellAdmissionClaimDTO& claim) {
  const auto settlement =
      claim.intent.claim_kind == CellAdmissionClaimKindDTO::kAuthoringMutation
          ? CellAdmissionSettlementKindDTO::kOwnerCommit
          : CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
  return CellAdmissionClaimReleaseDTO{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      CellAdmissionSettlementEvidenceDTO{
          settlement, ContentDigest(kDigest)}};
}

CellAdmissionReconcileValidationDTO RetainAll(
    const CellAdmissionReconcileDTO& reconcile) {
  return CellAdmissionReconcileValidationDTO{reconcile.current_owner_epochs, {}};
}

CellAdmissionReconcileValidationDTO Settle(
    const CellAdmissionReconcileDTO& reconcile, const CellAdmissionClaimDTO& predecessor,
    RequestOwnerEpoch successor) {
  return CellAdmissionReconcileValidationDTO{
      reconcile.current_owner_epochs,
      {{predecessor, predecessor.intent.owner_role, std::move(successor),
        CellAdmissionSettlementKindDTO::kReconciled,
        reconcile.reconciliation_evidence_digest}}};
}

std::int64_t CountRows(const std::filesystem::path& database_path,
                       const char* table) {
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

TEST(KernelCellAdmissionSqlite, AcquireAndReleaseCrashWindowsRehydrateExactTruth) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const auto claim = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U,
                                        {ResourceId("resource.1")})
                    .ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
    auto blocked = journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.1"), 2);
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().code, "operation_journal_owner_close_blocked");
  }
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 3, 8U,
                                            {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    ASSERT_EQ(recovery.value().recovered_cell_admission_claims.size(), 1U);
    EXPECT_EQ(recovery.value().recovered_cell_admission_claims.front().claim, claim);
    EXPECT_FALSE(recovery.value().recovered_cell_admission_claims.front().release.has_value());
    ASSERT_TRUE(journal.RecordCellAdmissionClaimReleased(
                           RequestOwnerEpoch("kernel.2"), claim, Release(claim),
                           AdmissionGeneration(4U))
                    .ok());
  }
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.3"), 4, 8U,
                                            {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    EXPECT_TRUE(recovery.value().recovered_cell_admission_claims.empty());
    ASSERT_EQ(recovery.value().recovered_cell_admission_membership.size(), 1U);
    EXPECT_EQ(recovery.value().recovered_cell_admission_membership.front().request_id,
              claim.intent.request_id);
    EXPECT_TRUE(journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.3"), 5).ok());
  }
}

TEST(KernelCellAdmissionSqlite, ReplacementReconcileIsInventoryBoundAndReplaySafe) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const auto claim = Claim();
  SqliteOperationJournal journal(database, CellId("cell.1"));
  ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U,
                                      {ResourceId("resource.1")})
                  .ok());
  ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
  journal.Close();

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery = replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U,
                                              {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  CellAdmissionReconcileDTO incomplete{
      RequestOwnerEpoch("kernel.2"),
      {},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner,
        RequestOwnerEpoch("authoring.owner.1")}},
      ContentDigest(kDigest)};
  EXPECT_FALSE(replacement
                   .RecordCellAdmissionClaimsReconciled(
                       RequestOwnerEpoch("kernel.2"), incomplete,
                       RetainAll(incomplete),
                       recovery.value().cell_admission_generation_high_water)
                   .ok());
  auto byte_different_claim = claim;
  byte_different_claim.accepted_at_ns += 1;
  CellAdmissionReconcileDTO byte_different{
      RequestOwnerEpoch("kernel.2"),
      {byte_different_claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner,
        RequestOwnerEpoch("authoring.owner.1")}},
      ContentDigest(kDigest)};
  EXPECT_FALSE(replacement
                   .RecordCellAdmissionClaimsReconciled(
                       RequestOwnerEpoch("kernel.2"), byte_different,
                       RetainAll(byte_different),
                       recovery.value().cell_admission_generation_high_water)
                   .ok());
  CellAdmissionReconcileDTO blocked_rotation{
      RequestOwnerEpoch("kernel.2"),
      {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner,
        RequestOwnerEpoch("authoring.owner.2")}},
      ContentDigest(kDigest)};
  const auto blocked = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), blocked_rotation, RetainAll(blocked_rotation),
      AdmissionGeneration(
          recovery.value().cell_admission_generation_high_water.value() + 1U));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().code, "operation_journal_owner_close_blocked");
  CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.2"),
      {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.1")}},
      ContentDigest(kDigest)};
  auto first = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), reconcile, RetainAll(reconcile),
      recovery.value().cell_admission_generation_high_water);
  ASSERT_TRUE(first.ok());
  auto replay = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), reconcile, RetainAll(reconcile),
      recovery.value().cell_admission_generation_high_water);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), first.value());
  CellAdmissionReconcileDTO successor{
      RequestOwnerEpoch("kernel.2"),
      {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner,
        RequestOwnerEpoch("authoring.owner.2")}},
      ContentDigest(kDigestB)};
  const AdmissionGeneration successor_generation(
      recovery.value().cell_admission_generation_high_water.value() + 1U);
  auto second = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), successor,
      Settle(successor, claim, RequestOwnerEpoch("authoring.owner.2")),
      successor_generation);
  ASSERT_TRUE(second.ok());
  EXPECT_NE(second.value(), first.value());
  auto second_replay = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), successor,
      Settle(successor, claim, RequestOwnerEpoch("authoring.owner.2")),
      successor_generation);
  ASSERT_TRUE(second_replay.ok());
  EXPECT_EQ(second_replay.value(), second.value());
}

TEST(KernelCellAdmissionSqlite, SuccessorEvidenceDurablySettlesExactPredecessorClaim) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const auto claim = Claim();
  auto retained = Claim();
  retained.intent.claim_kind = CellAdmissionClaimKindDTO::kManagerApply;
  retained.intent.owner_role = CellAdmissionOwnerRoleDTO::kManager;
  retained.intent.owner_epoch = RequestOwnerEpoch("manager.owner.1");
  retained.intent.owner_transaction_id = "apply.transaction.1";
  retained.intent.request_id = RequestId("request.2");
  retained.intent.expected_admission_generation = AdmissionGeneration(2U);
  retained.claim_generation = CellAdmissionClaimGeneration(2U);
  retained.acquired_admission_generation = AdmissionGeneration(2U);
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 4U,
                                        {ResourceId("resource.1")})
                    .ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), retained).ok());
  }
  SqliteOperationJournal successor(database, CellId("cell.1"));
  auto recovery = successor.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 4U,
                                            {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.2"),
      {claim, retained},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.2")},
       {CellAdmissionOwnerRoleDTO::kManager, RequestOwnerEpoch("manager.owner.1")}},
      ContentDigest(kDigest)};
  const AdmissionGeneration settled_generation(
      recovery.value().cell_admission_generation_high_water.value() + 1U);
  ASSERT_TRUE(successor
                  .RecordCellAdmissionClaimsReconciled(
                      RequestOwnerEpoch("kernel.2"), reconcile,
                      Settle(reconcile, claim, RequestOwnerEpoch("authoring.owner.2")),
                      settled_generation)
                  .ok());
  successor.Close();
  SqliteOperationJournal restarted(database, CellId("cell.1"));
  ASSERT_TRUE(restarted
                  .RecordCellAdmissionClaimsReconciled(
                      RequestOwnerEpoch("kernel.2"), reconcile,
                      Settle(reconcile, claim, RequestOwnerEpoch("authoring.owner.2")),
                      settled_generation)
                  .ok());
  EXPECT_FALSE(restarted
                   .RecordCellAdmissionClaimsReconciled(
                       RequestOwnerEpoch("kernel.2"), reconcile,
                       RetainAll(reconcile), settled_generation)
                   .ok());
  const auto predecessor_release = restarted.RecordCellAdmissionClaimReleased(
      RequestOwnerEpoch("kernel.2"), claim, Release(claim),
      AdmissionGeneration(settled_generation.value() + 1U));
  ASSERT_FALSE(predecessor_release.ok());
  EXPECT_EQ(predecessor_release.error().code, "operation_journal_owner_epoch_stale");
  const auto retained_release = restarted.RecordCellAdmissionClaimReleased(
      RequestOwnerEpoch("kernel.2"), retained, Release(retained),
      AdmissionGeneration(settled_generation.value() + 1U));
  EXPECT_TRUE(retained_release.ok());
}

TEST(KernelCellAdmissionSqlite,
     CleanEpochRetiresReleasedMembershipAndPreservesMonotonicHighWaters) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  auto first_claim = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 1U,
                                            {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    EXPECT_EQ(recovery.value().cell_admission_generation_high_water,
              AdmissionGeneration(1U));
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), first_claim).ok());
    ASSERT_TRUE(journal.RecordCellAdmissionClaimReleased(
                           RequestOwnerEpoch("kernel.1"), first_claim, Release(first_claim),
                           AdmissionGeneration(3U))
                    .ok());
    const auto expired = journal.RecordCellAdmissionClaimAcquired(
        RequestOwnerEpoch("kernel.1"), first_claim);
    ASSERT_FALSE(expired.ok());
    EXPECT_EQ(expired.error().code, "request_result_expired");
    auto conflicting = first_claim;
    conflicting.intent.owner_transaction_id = "different.transaction";
    const auto conflict = journal.RecordCellAdmissionClaimAcquired(
        RequestOwnerEpoch("kernel.1"), conflicting);
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error().code, "request_id_conflict");
    auto unseen = first_claim;
    unseen.intent.request_id = RequestId("request.unseen");
    unseen.intent.expected_admission_generation = AdmissionGeneration(3U);
    unseen.acquired_admission_generation = AdmissionGeneration(3U);
    unseen.claim_generation = CellAdmissionClaimGeneration(2U);
    const auto full = journal.RecordCellAdmissionClaimAcquired(
        RequestOwnerEpoch("kernel.1"), unseen);
    ASSERT_FALSE(full.ok());
    EXPECT_EQ(full.error().code, "request_ledger_full");
    ASSERT_TRUE(journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.1"), 2).ok());
  }

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery = replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 3, 1U,
                                              {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  EXPECT_TRUE(recovery.value().recovered_cell_admission_claims.empty());
  EXPECT_EQ(recovery.value().cell_admission_generation_high_water,
            AdmissionGeneration(4U));
  CellAdmissionReconcileDTO rotate{
      RequestOwnerEpoch("kernel.2"), {},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner,
        RequestOwnerEpoch("authoring.owner.2")}},
      ContentDigest(kDigest)};
  const AdmissionGeneration rotated_generation(
      recovery.value().cell_admission_generation_high_water.value() + 1U);
  ASSERT_TRUE(replacement
                  .RecordCellAdmissionClaimsReconciled(
                      RequestOwnerEpoch("kernel.2"), rotate, RetainAll(rotate),
                      rotated_generation)
                  .ok());
  auto fresh = first_claim;
  fresh.intent.owner_epoch = RequestOwnerEpoch("authoring.owner.2");
  fresh.intent.request_id = RequestId("request.2");
  fresh.intent.expected_admission_generation = rotated_generation;
  fresh.acquired_admission_generation = rotated_generation;
  fresh.claim_generation = CellAdmissionClaimGeneration(2U);
  EXPECT_TRUE(
      replacement.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.2"), fresh).ok());
  const auto stale_exact = replacement.RecordCellAdmissionClaimAcquired(
      RequestOwnerEpoch("kernel.2"), first_claim);
  ASSERT_FALSE(stale_exact.ok());
  EXPECT_EQ(stale_exact.error().code, "stale_request_owner_epoch");
  auto stale = first_claim;
  stale.intent.request_id = RequestId("request.old.unseen");
  stale.claim_generation = CellAdmissionClaimGeneration(3U);
  stale.intent.expected_admission_generation =
      AdmissionGeneration(rotated_generation.value() + 1U);
  stale.acquired_admission_generation = stale.intent.expected_admission_generation;
  const auto stale_result =
      replacement.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.2"), stale);
  ASSERT_FALSE(stale_result.ok());
  EXPECT_EQ(stale_result.error().code, "stale_request_owner_epoch");
}

TEST(KernelCellAdmissionSqlite,
     ReplacementGenerationMakesPredecessorReadinessGenerationStaleWithoutHeldClaim) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  AdmissionGeneration predecessor_public(0U);
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 4U,
                                            {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.1"), 4U, clock, journal, {},
        recovery.value().cell_admission_generation_high_water);
    auto ready = CellAdmissionReadinessSnapshotDTO{
        true,
        ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
                              ProgramCycleStateDTO::kIdleAfterReset, std::nullopt,
                              std::nullopt, std::nullopt},
        true, true, true, 1U, 1U, true, true, 0U, 0U, 0U};
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
    predecessor_public = owner.GetAuthoringAdmission().admission_generation;
    ASSERT_TRUE(journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.1"), 2).ok());
  }
  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery = replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 3, 4U,
                                              {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  EXPECT_GT(recovery.value().cell_admission_generation_high_water.value(),
            predecessor_public.value());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 4U, clock, replacement, {},
      recovery.value().cell_admission_generation_high_water);
  auto stale = Claim().intent;
  stale.expected_admission_generation = predecessor_public;
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(stale).error().code, "stale_generation");
}

TEST(KernelCellAdmissionSqlite, RepeatedCleanOwnerRotationsKeepMembershipRowsBounded) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  AdmissionGeneration expected_generation(0U);
  CellAdmissionClaimGeneration claim_generation(1U);
  for (std::uint64_t index = 1U; index <= 5U; ++index) {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    const RequestOwnerEpoch kernel_epoch("kernel." + std::to_string(index));
    auto recovery = journal.BeginOwnerEpoch(
        kernel_epoch, static_cast<std::int64_t>(index * 10U), 1U,
        {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    expected_generation =
        recovery.value().cell_admission_generation_high_water;
    const RequestOwnerEpoch request_epoch("authoring.owner." +
                                          std::to_string(index));
    if (index > 1U) {
      CellAdmissionReconcileDTO rotate{
          kernel_epoch, {},
          {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, request_epoch}},
          ContentDigest(kDigest)};
      expected_generation =
          AdmissionGeneration(expected_generation.value() + 1U);
      ASSERT_TRUE(journal
                      .RecordCellAdmissionClaimsReconciled(
                          kernel_epoch, rotate, RetainAll(rotate),
                          expected_generation)
                      .ok());
    }
    auto claim = Claim();
    claim.intent.owner_epoch = request_epoch;
    claim.intent.request_id =
        RequestId("request." + std::to_string(index));
    claim.intent.expected_admission_generation = expected_generation;
    claim.acquired_admission_generation = expected_generation;
    claim.claim_generation = claim_generation;
    ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(kernel_epoch, claim).ok());
    expected_generation =
        AdmissionGeneration(expected_generation.value() + 2U);
    ASSERT_TRUE(journal.RecordCellAdmissionClaimReleased(
                           kernel_epoch, claim, Release(claim),
                           expected_generation)
                    .ok());
    ASSERT_TRUE(journal.CloseOwnerEpoch(
                           kernel_epoch, static_cast<std::int64_t>(index * 10U + 1U))
                    .ok());
    claim_generation =
        CellAdmissionClaimGeneration(claim_generation.value() + 1U);
  }
  EXPECT_EQ(CountRows(database, "cell_admission_request_membership"), 1);
  EXPECT_EQ(CountRows(database, "cell_admission_claims"), 0);
  EXPECT_EQ(CountRows(database, "cell_admission_current_owners"), 1);
  EXPECT_LE(CountRows(database, "cell_admission_reconciliations"), 1);
}

}  // namespace
