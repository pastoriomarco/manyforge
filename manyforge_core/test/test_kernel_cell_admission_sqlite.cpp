#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <manyforge_core/emergency_stop_acceptance_reserve.hpp>
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
          CellAdmissionClaimKindDTO::kAuthoringMutation, CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          RequestOwnerEpoch("authoring.owner.1"), "document.transaction.1", RequestId("request.1"),
          ContentDigest(kDigest), AdmissionGeneration(1U),
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
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      CellAdmissionSettlementEvidenceDTO{settlement, ContentDigest(kDigest)}};
}

ContentDigest Digest(char digit) {
  return ContentDigest("sha256:" + std::string(64U, digit));
}

ManagerLivenessRequiredSetDTO EmptyLivenessSet() {
  ManagerLivenessRequiredSetDTO result{CellId("cell.1"),
                                       RetainedBundleId("bundle.1"),
                                       ContentDigest(kDigest),
                                       RuntimeUnitId("manager"),
                                       RuntimeEpoch("manager.epoch.1"),
                                       RequestOwnerEpoch("manager.owner.1"),
                                       {}};
  result.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(result.required_receivers);
  return result;
}

CellAdmissionReadinessSnapshotDTO Ready() {
  return CellAdmissionReadinessSnapshotDTO{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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

CellAdmissionReconcileValidationDTO RetainAll(const CellAdmissionReconcileDTO& reconcile) {
  return CellAdmissionReconcileValidationDTO{reconcile.current_owner_epochs, {}};
}

CellAdmissionReconcileValidationDTO Settle(const CellAdmissionReconcileDTO& reconcile,
                                           const CellAdmissionClaimDTO& predecessor,
                                           RequestOwnerEpoch successor) {
  return CellAdmissionReconcileValidationDTO{
      reconcile.current_owner_epochs,
      {{predecessor, predecessor.intent.owner_role, std::move(successor),
        CellAdmissionSettlementKindDTO::kReconciled, reconcile.reconciliation_evidence_digest}}};
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
                                         std::string epoch, std::string request) {
  auto claim = Claim();
  claim.intent.claim_kind = CellAdmissionClaimKindDTO::kManagerApply;
  claim.intent.owner_role = CellAdmissionOwnerRoleDTO::kManager;
  claim.intent.owner_epoch = RequestOwnerEpoch(std::move(epoch));
  claim.intent.owner_transaction_id = request + ".transaction";
  claim.intent.request_id = RequestId(std::move(request));
  claim.intent.scope = {};
  claim.intent.expected_admission_generation = AdmissionGeneration(admission);
  claim.claim_generation = CellAdmissionClaimGeneration(generation);
  claim.acquired_admission_generation = AdmissionGeneration(admission);
  return claim;
}

// Use SQLite's real commit hook and authorizer, without a product fault injector.
// The hook rolls back COMMIT; the optional authorizer then makes outcome readback
// unavailable. Installation/removal is scoped to this serial test invocation.
struct ConfigurationCommitFailure final {
  bool armed{};
  bool fail_readback{};
  bool deny_reads{};
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
        failure.deny_reads = failure.fail_readback;
        return 1;
      },
      control);
  return sqlite3_set_authorizer(
      database,
      [](void* value, int action, const char*, const char*, const char*, const char*) {
        const auto& failure = *static_cast<ConfigurationCommitFailure*>(value);
        return failure.deny_reads && (action == SQLITE_SELECT || action == SQLITE_READ)
                   ? SQLITE_DENY
                   : SQLITE_OK;
      },
      control);
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

TEST(KernelCellAdmissionSqlite, ConfigurationCommitWithUnavailableReadbackClosesOnlyNewAdmission) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  ConfigurationCommitFailure failure;
  ScopedConfigurationCommitFailure hook(failure);
  SqliteOperationJournal journal(database, CellId("cell.1"));
  const RequestOwnerEpoch kernel("kernel.1");
  auto recovery = journal.BeginOwnerEpoch(kernel, 1, 8U, {});
  ASSERT_TRUE(recovery.ok());
  KernelCellAdmissionOwner owner(
      kernel, 8U, clock, journal, {}, recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {}, EmptyLivenessSet());
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto intent = ConfigurationClaim(1U, owner.GetAuthoringAdmission().admission_generation.value(),
                                   EmptyLivenessSet().manager_owner_epoch.value(), "unknown.commit")
                    .intent;
  failure.armed = true;
  failure.fail_readback = true;
  const auto result = owner.AcquireConfigurationApplyClaim(
      intent, [](const auto&, auto, const auto&, const auto&) { return Result<void>::Ok(); });
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(failure.commit_failures, 1);
  EXPECT_EQ(result.error().code, "persistence_outcome_unknown");
  ASSERT_TRUE(owner.persistence_outcome_unknown());
  EXPECT_EQ(owner.persistence_outcome_unknown()->details.at("request_id"), "unknown.commit");
  EXPECT_FALSE(owner.persistence_outcome_unknown()->details.at("commit_error").empty());
  EXPECT_FALSE(owner.persistence_outcome_unknown()->details.at("readback_error").empty());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_EQ(
      owner
          .AcquireConfigurationApplyClaim(intent, [](const auto&, auto, const auto&,
                                                     const auto&) { return Result<void>::Ok(); })
          .error()
          .code,
      "persistence_outcome_unknown");
  EXPECT_FALSE(owner.CheckManualCellAdmissionReadiness(std::nullopt).ok());
  EmergencyStopAcceptanceReserve stop_reserve(1U);
  const EmergencyStopAcceptanceCandidate stop{
      .request_id = "request.stop.after-unknown",
      .normalized_request_digest = std::string_view(kDigest),
      .transport = RequestTransportDTO::kInternal,
      .assurance = RequestAssuranceDTO::kUnverified,
      .principal_id = std::nullopt,
      .malformed_reason = std::nullopt,
      .proposed_stop_id = "stop.after-unknown",
      .proposed_stop_generation = StopGeneration(1U),
      .accepted_at_ns = 11,
      .disposition = StopResolutionDispositionDTO::kPromotedToFull,
      .captured_scope_digest = kDigest,
      .full_stop_scope_digest = kDigest,
      .overlapping_stop_id = std::nullopt,
      .full_stop = true};
  EXPECT_TRUE(stop_reserve.reserve_or_join(stop).accepted());
  // Removing the injected read fault is not an in-process clear.
  failure.deny_reads = false;
  EXPECT_FALSE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  EXPECT_TRUE(owner.persistence_outcome_unknown());
  journal.Close();
  SqliteOperationJournal restarted(database, CellId("cell.1"));
  const auto recovered = restarted.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 12, 8U, {});
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_TRUE(
      recovered.value()
          .recovered_cell_admission_claims.empty());  // Actual rollback proved after restart.
}

TEST(KernelCellAdmissionSqlite, ConfigurationCommitWithProvenRollbackDoesNotCloseAdmission) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  ConfigurationCommitFailure failure;
  ScopedConfigurationCommitFailure hook(failure);
  SqliteOperationJournal journal(database, CellId("cell.1"));
  const RequestOwnerEpoch kernel("kernel.1");
  auto recovery = journal.BeginOwnerEpoch(kernel, 1, 8U, {});
  ASSERT_TRUE(recovery.ok());
  KernelCellAdmissionOwner owner(
      kernel, 8U, clock, journal, {}, recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {}, EmptyLivenessSet());
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto before = owner.GetAuthoringAdmission();
  auto intent =
      ConfigurationClaim(1U, before.admission_generation.value(),
                         EmptyLivenessSet().manager_owner_epoch.value(), "rolled-back.commit")
          .intent;
  failure.armed = true;
  const auto result = owner.AcquireConfigurationApplyClaim(
      intent, [](const auto&, auto, const auto&, const auto&) { return Result<void>::Ok(); });
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(failure.commit_failures, 1);
  EXPECT_NE(result.error().code, "persistence_outcome_unknown");
  EXPECT_EQ(owner.GetAuthoringAdmission(), before);
  EXPECT_FALSE(owner.persistence_outcome_unknown());
  EXPECT_FALSE(owner.has_unresolved_claims());
  EXPECT_EQ(CountRows(database, "cell_admission_claims"), 0);
}

TEST(KernelCellAdmissionSqlite, ConfigurationApplyRestartsManagerWithoutReleasingTerminalMarkers) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const RequestOwnerEpoch kernel("kernel.1");
  SqliteOperationJournal journal(database, CellId("cell.1"));
  ASSERT_TRUE(journal.BeginOwnerEpoch(kernel, 1, 8U, {}).ok());
  const auto old = ConfigurationClaim(1U, 1U, "manager.old", "apply.old");
  ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(kernel, old).ok());
  auto fresh = ConfigurationClaim(2U, 2U, "manager.new", "apply.fresh");
  const std::vector<CellAdmissionClaimDTO> old_held{old};
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> old_owners{
      {CellAdmissionOwnerRoleDTO::kManager, old.intent.owner_epoch}};
  const CellAdmissionCurrentOwnerEpochDTO manager{CellAdmissionOwnerRoleDTO::kManager,
                                                  fresh.intent.owner_epoch};
  EXPECT_FALSE(
      journal.RecordConfigurationApplyClaimAcquired(kernel, fresh, manager, {}, old_owners).ok());
  auto foreign_owners = old_owners;
  foreign_owners.front().owner_epoch = RequestOwnerEpoch("foreign.owner");
  EXPECT_FALSE(
      journal
          .RecordConfigurationApplyClaimAcquired(kernel, fresh, manager, old_held, foreign_owners)
          .ok());
  const auto acquired =
      journal.RecordConfigurationApplyClaimAcquired(kernel, fresh, manager, old_held, old_owners);
  ASSERT_TRUE(acquired.ok()) << acquired.error().message;
  EXPECT_EQ(acquired.value(), fresh);
  auto later_retry = fresh;
  later_retry.accepted_at_ns = 123456;  // An unknown-commit retry must recover the first timestamp.
  const auto replay = journal.RecordConfigurationApplyClaimAcquired(kernel, later_retry, manager,
                                                                    old_held, old_owners);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value(), fresh);
  EXPECT_EQ(CountRows(database, "cell_admission_claims"), 2);
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> current_owners{manager};
  const std::vector<CellAdmissionClaimDTO> held{old, fresh};
  // Ordinary stale-owner release remains forbidden; only proven fresh success can settle it.
  EXPECT_FALSE(
      journal.RecordCellAdmissionClaimReleased(kernel, old, Release(old), AdmissionGeneration(4U))
          .ok());
  EXPECT_FALSE(journal
                   .RecordTerminalApplyClaimsReleasedBySuccess(kernel, fresh, Release(fresh),
                                                               {fresh}, current_owners,
                                                               AdmissionGeneration(3U))
                   .ok());
  const auto settled = journal.RecordTerminalApplyClaimsReleasedBySuccess(
      kernel, fresh, Release(fresh), held, current_owners, AdmissionGeneration(3U));
  ASSERT_TRUE(settled.ok()) << settled.error().message;
  EXPECT_EQ(settled.value(), AdmissionGeneration(4U));
  const auto settled_replay = journal.RecordTerminalApplyClaimsReleasedBySuccess(
      kernel, fresh, Release(fresh), held, current_owners, AdmissionGeneration(3U));
  ASSERT_TRUE(settled_replay.ok());
  EXPECT_EQ(settled_replay.value(), settled.value());
  auto conflicting_success = Release(fresh);
  conflicting_success.settlement.evidence_digest = ContentDigest(kDigestB);
  EXPECT_FALSE(journal
                   .RecordTerminalApplyClaimsReleasedBySuccess(kernel, fresh, conflicting_success,
                                                               held, current_owners,
                                                               AdmissionGeneration(3U))
                   .ok());
  EXPECT_EQ(CountRows(database, "cell_admission_claims"), 2);  // Original identities retained.
  journal.Close();
  SqliteOperationJournal reopened(database, CellId("cell.1"));
  const auto recovery = reopened.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {});
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().recovered_cell_admission_claims.size(), 2U);
  EXPECT_EQ(recovery.value().recovered_cell_admission_claims[0].claim, old);
  EXPECT_EQ(recovery.value().recovered_cell_admission_claims[0].release, Release(old));
  EXPECT_EQ(recovery.value().recovered_cell_admission_claims[1].claim, fresh);
  EXPECT_FALSE(recovery.value().recovered_cell_admission_claims[1].release);
  EXPECT_EQ(recovery.value().recovered_cell_admission_current_owners, current_owners);
}

TEST(KernelCellAdmissionSqlite, ConfigurationSuccessReleaseRollsBackWholeBatchOnWriteFailure) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const RequestOwnerEpoch kernel("kernel.1");
  SqliteOperationJournal journal(database, CellId("cell.1"));
  ASSERT_TRUE(journal.BeginOwnerEpoch(kernel, 1, 8U, {}).ok());
  const auto first = ConfigurationClaim(1U, 1U, "manager.owner", "apply.first");
  const auto second = ConfigurationClaim(2U, 2U, "manager.owner", "apply.second");
  const auto fresh = ConfigurationClaim(3U, 3U, "manager.owner", "apply.fresh");
  ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(kernel, first).ok());
  ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(kernel, second).ok());
  ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(kernel, fresh).ok());
  const std::vector<CellAdmissionClaimDTO> held{first, second, fresh};
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> owners{
      {CellAdmissionOwnerRoleDTO::kManager, fresh.intent.owner_epoch}};
  sqlite3* writer = nullptr;
  ASSERT_EQ(sqlite3_open(database.c_str(), &writer), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(writer,
                   "CREATE TRIGGER fail_second_release BEFORE UPDATE OF release_owner_role ON "
                   "cell_admission_claims "
                   "WHEN OLD.claim_generation=2 BEGIN SELECT RAISE(ABORT,'injected failure'); END;",
                   nullptr, nullptr, nullptr),
      SQLITE_OK);
  sqlite3_close(writer);
  EXPECT_FALSE(journal
                   .RecordTerminalApplyClaimsReleasedBySuccess(kernel, fresh, Release(fresh), held,
                                                               owners, AdmissionGeneration(4U))
                   .ok());
  ASSERT_EQ(sqlite3_open(database.c_str(), &writer), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(writer, "DROP TRIGGER fail_second_release", nullptr, nullptr, nullptr),
            SQLITE_OK);
  sqlite3_close(writer);
  // Exact original inventory/generation must still match after the failed second update.
  const auto result = journal.RecordTerminalApplyClaimsReleasedBySuccess(
      kernel, fresh, Release(fresh), held, owners, AdmissionGeneration(4U));
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value(), AdmissionGeneration(5U));
}

TEST(KernelCellAdmissionSqlite, ProgramNodeAttributionSurvivesOwnerReplacement) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  auto claim = Claim();
  claim.intent.claim_kind = CellAdmissionClaimKindDTO::kAuthoredScene;
  claim.intent.owner_role = CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner;
  claim.intent.program_node_evidence =
      ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch("program.owner"),
                                      ProgramRunId("run.1"),
                                      CellAdmissionClaimGeneration(7U),
                                      "attach",
                                      std::string(512U, 'a'),
                                      "scene.carried_attachment"};
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {}).ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
  }
  SqliteOperationJournal journal(database, CellId("cell.1"));
  const auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {});
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().recovered_cell_admission_claims.size(), 1U);
  EXPECT_EQ(recovery.value().recovered_cell_admission_claims.front().claim, claim);
  EXPECT_EQ(recovery.value().recovered_cell_admission_membership.front().semantic_intent_digest,
            CellAdmissionClaimIntentDigest(claim.intent).value());
}

TEST(KernelCellAdmissionSqlite, HeldManualParentAndOfferSurviveOwnerReplacement) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  auto claim = Claim();
  claim.intent.claim_kind = CellAdmissionClaimKindDTO::kManualOperation;
  claim.intent.owner_role = CellAdmissionOwnerRoleDTO::kManualOperationOwner;
  claim.intent.owner_epoch = RequestOwnerEpoch("kernel.1");
  claim.intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
      claim.intent.owner_role, claim.intent.owner_epoch, claim.intent.owner_transaction_id,
      ContentDigest(kDigestB), CellAdmissionClaimGeneration(7U)};
  ASSERT_TRUE(ValidateCellAdmissionClaimIntent(claim.intent).ok());
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {}).ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
  }
  SqliteOperationJournal journal(database, CellId("cell.1"));
  const auto recovery = journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {});
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().recovered_cell_admission_claims.size(), 1U);
  const auto& recovered = recovery.value().recovered_cell_admission_claims.front();
  EXPECT_EQ(recovered.claim, claim);
  EXPECT_FALSE(recovered.release.has_value());
  EXPECT_FALSE(recovered.reconcile_settlement.has_value());
  EXPECT_EQ(recovery.value().recovered_cell_admission_membership.front().semantic_intent_digest,
            CellAdmissionClaimIntentDigest(claim.intent).value());
}

TEST(KernelCellAdmissionSqlite, V12ClaimsAreRefusedNotConvertedAfterTheStopTimingFormat) {
  // A journal of an earlier format (here v12, before the attribution
  // columns and the per-robot Stop timing) is refused, never migrated.
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const auto claim = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {}).ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
  }
  sqlite3* legacy = nullptr;
  ASSERT_EQ(sqlite3_open(database.c_str(), &legacy), SQLITE_OK);
  const auto downgraded =
      sqlite3_exec(legacy,
                   "ALTER TABLE cell_admission_claims DROP COLUMN program_node_runner_owner_epoch;"
                   "ALTER TABLE cell_admission_claims DROP COLUMN program_node_run_id;"
                   "ALTER TABLE cell_admission_claims DROP COLUMN program_node_instance_id;"
                   "ALTER TABLE cell_admission_claims DROP COLUMN program_node_attempt_id;"
                   "ALTER TABLE cell_admission_claims DROP COLUMN program_node_owner_api;"
                   "PRAGMA user_version=12; UPDATE journal_metadata SET schema_version=12,"
                   "schema_id='manyforge.internal.execution.operation-journal.o1.5.v12';",
                   nullptr, nullptr, nullptr);
  sqlite3_close(legacy);
  ASSERT_EQ(downgraded, SQLITE_OK);
  EXPECT_THROW(SqliteOperationJournal(database, CellId("cell.1")), std::runtime_error);
}

TEST(KernelCellAdmissionSqlite, AcquireAndReleaseCrashWindowsRehydrateExactTruth) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  auto claim = Claim();
  // An effect claim (authoring and Apply claims never block an owner close).
  claim.intent.claim_kind = CellAdmissionClaimKindDTO::kAuthoredScene;
  claim.intent.owner_role = CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner;
  claim.intent.owner_epoch = RequestOwnerEpoch("scene.owner.1");
  claim.intent.owner_transaction_id = "scene.1";
  claim.intent.request_id = RequestId("scene.request.1");
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    ASSERT_TRUE(
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {ResourceId("resource.1")})
            .ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
    auto blocked = journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.1"), 2);
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().code, "operation_journal_owner_close_blocked");
  }
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 3, 8U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    ASSERT_EQ(recovery.value().recovered_cell_admission_claims.size(), 1U);
    EXPECT_EQ(recovery.value().recovered_cell_admission_claims.front().claim, claim);
    EXPECT_FALSE(recovery.value().recovered_cell_admission_claims.front().release.has_value());
    ASSERT_TRUE(journal
                    .RecordCellAdmissionClaimReleased(RequestOwnerEpoch("kernel.2"), claim,
                                                      Release(claim), AdmissionGeneration(4U))
                    .ok());
  }
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.3"), 4, 8U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    EXPECT_EQ(recovery.value().cell_claim_generation_high_water, CellAdmissionClaimGeneration(1U));
    ASSERT_EQ(recovery.value().recovered_cell_admission_claims.size(), 1U);
    EXPECT_EQ(recovery.value().recovered_cell_admission_claims.front().claim, claim);
    EXPECT_EQ(recovery.value().recovered_cell_admission_claims.front().release, Release(claim));
    EXPECT_FALSE(
        recovery.value().recovered_cell_admission_claims.front().reconcile_settlement.has_value());
    ASSERT_EQ(recovery.value().recovered_cell_admission_membership.size(), 1U);
    EXPECT_EQ(recovery.value().recovered_cell_admission_membership.front().request_id,
              claim.intent.request_id);
    FixedClock clock;
    KernelCellAdmissionOwner owner(RequestOwnerEpoch("kernel.3"), 8U, clock, journal,
                                   recovery.value().recovered_cell_admission_claims,
                                   recovery.value().cell_admission_generation_high_water, {}, {},
                                   {}, recovery.value().recovered_cell_admission_membership,
                                   recovery.value().recovered_cell_admission_current_owners);
    const auto receipt =
        owner.GetCellAdmissionRecoveryReceipt({claim.intent, claim, Release(claim)});
    ASSERT_TRUE(receipt.ok());
    EXPECT_EQ(receipt.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
    EXPECT_EQ(receipt.value().matched_release, Release(claim));
    EXPECT_TRUE(receipt.value().held_claims.empty());
    EXPECT_TRUE(journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.3"), 5).ok());
  }
}

TEST(KernelCellAdmissionSqlite, UncleanKernelReplacementAllowsProvenEmptyLifecycleOwnerRotation) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  auto predecessor = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.1"), 8U, clock, journal, {},
        recovery.value().cell_admission_generation_high_water, {},
        [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {},
        EmptyLivenessSet());
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    auto intent = Claim().intent;
    intent.claim_kind = CellAdmissionClaimKindDTO::kAuthoringMutation;
    intent.owner_role = CellAdmissionOwnerRoleDTO::kAuthoringOwner;
    intent.owner_epoch = RequestOwnerEpoch("authoring.owner.1");
    intent.owner_transaction_id = "authoring.1";
    intent.request_id = RequestId("authoring.request.1");
    intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
    auto acquired = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_TRUE(acquired.ok());
    predecessor = acquired.value();
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(predecessor)).ok());
  }

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 8U, clock, replacement,
      recovery.value().recovered_cell_admission_claims,
      recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(RetainAll(reconcile));
      },
      recovery.value().recovered_cell_admission_membership,
      recovery.value().recovered_cell_admission_current_owners, EmptyLivenessSet());
  const CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.2"),
      {},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.2")}},
      Digest('c')};
  const auto rotated = owner.ReconcileCellAdmissionClaims(reconcile);
  ASSERT_TRUE(rotated.ok()) << rotated.error().code << ": " << rotated.error().message;
  const auto retired = owner.GetCellAdmissionRecoveryReceipt(
      {predecessor.intent, predecessor, Release(predecessor)});
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  EXPECT_EQ(retired.value().current_owner_epochs, reconcile.current_owner_epochs);
  EXPECT_EQ(CountRows(database, "cell_admission_request_membership"), 0);
}

TEST(KernelCellAdmissionSqlite, UncleanKernelReplacementAllowsProvenEmptyAuthoringOwnerRotation) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  auto predecessor = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.1"), 8U, clock, journal, {},
        recovery.value().cell_admission_generation_high_water, {},
        [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {},
        EmptyLivenessSet());
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    auto intent = Claim().intent;
    intent.claim_kind = CellAdmissionClaimKindDTO::kAuthoringMutation;
    intent.owner_role = CellAdmissionOwnerRoleDTO::kAuthoringOwner;
    intent.owner_epoch = RequestOwnerEpoch("authoring.owner.1");
    intent.owner_transaction_id = "authoring.1";
    intent.request_id = RequestId("authoring.request.1");
    intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
    auto acquired = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_TRUE(acquired.ok());
    predecessor = acquired.value();
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(predecessor)).ok());
  }

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 8U, clock, replacement,
      recovery.value().recovered_cell_admission_claims,
      recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(RetainAll(reconcile));
      },
      recovery.value().recovered_cell_admission_membership,
      recovery.value().recovered_cell_admission_current_owners, EmptyLivenessSet());
  const CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.2"),
      {},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.2")}},
      Digest('c')};
  const auto rotated = owner.ReconcileCellAdmissionClaims(reconcile);
  ASSERT_TRUE(rotated.ok()) << rotated.error().code << ": " << rotated.error().message;
  const auto retired = owner.GetCellAdmissionRecoveryReceipt(
      {predecessor.intent, predecessor, Release(predecessor)});
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  EXPECT_EQ(retired.value().current_owner_epochs, reconcile.current_owner_epochs);
}

TEST(KernelCellAdmissionSqlite, UncleanKernelReplacementAllowsProvenEmptyProgramRunnerRotation) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  auto predecessor = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.1"), 8U, clock, journal, {},
        recovery.value().cell_admission_generation_high_water, {},
        [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {},
        EmptyLivenessSet());
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    auto intent = Claim().intent;
    intent.claim_kind = CellAdmissionClaimKindDTO::kProgramCycle;
    intent.owner_role = CellAdmissionOwnerRoleDTO::kProgramRunner;
    intent.owner_epoch = RequestOwnerEpoch("program.owner.1");
    intent.owner_transaction_id = "program-cycle.1";
    intent.request_id = RequestId("program.request.1");
    intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
    intent.program_cycle_evidence = ProgramCycleAdmissionEvidenceDTO{
        ProgramRevision("program.revision"), ProgramRevision("program.revision")};
    auto acquired = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_TRUE(acquired.ok()) << acquired.error().code << ": " << acquired.error().message;
    predecessor = acquired.value();
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(predecessor)).ok());
  }

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 8U, clock, replacement,
      recovery.value().recovered_cell_admission_claims,
      recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(RetainAll(reconcile));
      },
      recovery.value().recovered_cell_admission_membership,
      recovery.value().recovered_cell_admission_current_owners, EmptyLivenessSet());
  const CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.2"),
      {},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("program.owner.2")}},
      Digest('c')};
  const auto rotated = owner.ReconcileCellAdmissionClaims(reconcile);
  ASSERT_TRUE(rotated.ok()) << rotated.error().code << ": " << rotated.error().message;
  const auto retired = owner.GetCellAdmissionRecoveryReceipt(
      {predecessor.intent, predecessor, Release(predecessor)});
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  EXPECT_EQ(retired.value().current_owner_epochs, reconcile.current_owner_epochs);
  EXPECT_EQ(CountRows(database, "cell_admission_request_membership"), 0);
}

TEST(KernelCellAdmissionSqlite, UncleanKernelReplacementAllowsProvenEmptyManualOwnerRotation) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  auto predecessor = Claim();
  std::optional<CellAdmissionClaimReleaseDTO> predecessor_release;
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.1"), 8U, clock, journal, {},
        recovery.value().cell_admission_generation_high_water, {},
        [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {},
        EmptyLivenessSet());
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    auto intent = Claim().intent;
    intent.claim_kind = CellAdmissionClaimKindDTO::kManualOperation;
    intent.owner_role = CellAdmissionOwnerRoleDTO::kManualOperationOwner;
    intent.owner_epoch = RequestOwnerEpoch("kernel.1");
    intent.owner_transaction_id = "manual.1";
    intent.request_id = RequestId("manual.request.1");
    intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
    auto acquired = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_TRUE(acquired.ok());
    predecessor = acquired.value();
    predecessor_release = Release(predecessor);
    predecessor_release->settlement.settlement_kind =
        CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(*predecessor_release).ok());
  }

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 8U, clock, replacement,
      recovery.value().recovered_cell_admission_claims,
      recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(RetainAll(reconcile));
      },
      recovery.value().recovered_cell_admission_membership,
      recovery.value().recovered_cell_admission_current_owners, EmptyLivenessSet());
  const CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.2"),
      {},
      {{CellAdmissionOwnerRoleDTO::kManualOperationOwner, RequestOwnerEpoch("kernel.2")}},
      Digest('c')};
  const auto rotated = owner.ReconcileCellAdmissionClaims(reconcile);
  ASSERT_TRUE(rotated.ok()) << rotated.error().code << ": " << rotated.error().message;
  ASSERT_TRUE(predecessor_release.has_value());
  const auto retired =
      owner.GetCellAdmissionRecoveryReceipt({predecessor.intent, predecessor, predecessor_release});
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  EXPECT_EQ(retired.value().current_owner_epochs, reconcile.current_owner_epochs);
  EXPECT_EQ(CountRows(database, "cell_admission_request_membership"), 0);
}

TEST(KernelCellAdmissionSqlite, ReplacementReconcileIsInventoryBoundAndReplaySafe) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const auto claim = Claim();
  SqliteOperationJournal journal(database, CellId("cell.1"));
  ASSERT_TRUE(
      journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 8U, {ResourceId("resource.1")})
          .ok());
  ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
  journal.Close();

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 8U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  CellAdmissionReconcileDTO incomplete{
      RequestOwnerEpoch("kernel.2"),
      {},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.1")}},
      ContentDigest(kDigest)};
  EXPECT_FALSE(replacement
                   .RecordCellAdmissionClaimsReconciled(
                       RequestOwnerEpoch("kernel.2"), incomplete, RetainAll(incomplete),
                       recovery.value().cell_admission_generation_high_water)
                   .ok());
  auto byte_different_claim = claim;
  byte_different_claim.accepted_at_ns += 1;
  CellAdmissionReconcileDTO byte_different{
      RequestOwnerEpoch("kernel.2"),
      {byte_different_claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.1")}},
      ContentDigest(kDigest)};
  EXPECT_FALSE(replacement
                   .RecordCellAdmissionClaimsReconciled(
                       RequestOwnerEpoch("kernel.2"), byte_different, RetainAll(byte_different),
                       recovery.value().cell_admission_generation_high_water)
                   .ok());
  CellAdmissionReconcileDTO blocked_rotation{
      RequestOwnerEpoch("kernel.2"),
      {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.2")}},
      ContentDigest(kDigest)};
  const auto blocked = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), blocked_rotation, RetainAll(blocked_rotation),
      AdmissionGeneration(recovery.value().cell_admission_generation_high_water.value() + 1U));
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
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.2")}},
      ContentDigest(kDigestB)};
  const AdmissionGeneration successor_generation(
      recovery.value().cell_admission_generation_high_water.value() + 1U);
  auto second = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), successor,
      Settle(successor, claim, RequestOwnerEpoch("authoring.owner.2")), successor_generation);
  ASSERT_TRUE(second.ok());
  EXPECT_NE(second.value(), first.value());
  auto second_replay = replacement.RecordCellAdmissionClaimsReconciled(
      RequestOwnerEpoch("kernel.2"), successor,
      Settle(successor, claim, RequestOwnerEpoch("authoring.owner.2")), successor_generation);
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
    ASSERT_TRUE(
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 4U, {ResourceId("resource.1")})
            .ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), claim).ok());
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), retained).ok());
  }
  SqliteOperationJournal successor(database, CellId("cell.1"));
  auto recovery =
      successor.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 2, 4U, {ResourceId("resource.1")});
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
                   .RecordCellAdmissionClaimsReconciled(RequestOwnerEpoch("kernel.2"), reconcile,
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
  restarted.Close();

  SqliteOperationJournal receipt_journal(database, CellId("cell.1"));
  auto receipt_recovery = receipt_journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.3"), 3, 4U,
                                                          {ResourceId("resource.1")});
  ASSERT_TRUE(receipt_recovery.ok());
  FixedClock clock;
  KernelCellAdmissionOwner receipt_owner(
      RequestOwnerEpoch("kernel.3"), 4U, clock, receipt_journal,
      receipt_recovery.value().recovered_cell_admission_claims,
      receipt_recovery.value().cell_admission_generation_high_water, {}, {}, {},
      receipt_recovery.value().recovered_cell_admission_membership,
      receipt_recovery.value().recovered_cell_admission_current_owners);
  const auto reconciled_receipt =
      receipt_owner.GetCellAdmissionRecoveryReceipt({claim.intent, claim, std::nullopt});
  ASSERT_TRUE(reconciled_receipt.ok());
  EXPECT_EQ(reconciled_receipt.value().status,
            CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact);
  ASSERT_TRUE(reconciled_receipt.value().matched_reconcile_settlement.has_value());
  EXPECT_EQ(reconciled_receipt.value().matched_reconcile_settlement->reconciliation_evidence_digest,
            reconcile.reconciliation_evidence_digest);
  const auto released_receipt =
      receipt_owner.GetCellAdmissionRecoveryReceipt({retained.intent, retained, Release(retained)});
  ASSERT_TRUE(released_receipt.ok());
  EXPECT_EQ(released_receipt.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
  EXPECT_TRUE(released_receipt.value().held_claims.empty());
}

TEST(KernelCellAdmissionSqlite,
     CleanEpochRetiresReleasedMembershipAndPreservesMonotonicHighWaters) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  auto first_claim = Claim();
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 1U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    EXPECT_EQ(recovery.value().cell_admission_generation_high_water, AdmissionGeneration(1U));
    ASSERT_TRUE(
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), first_claim).ok());
    ASSERT_TRUE(journal
                    .RecordCellAdmissionClaimReleased(RequestOwnerEpoch("kernel.1"), first_claim,
                                                      Release(first_claim), AdmissionGeneration(3U))
                    .ok());
    const auto expired =
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), first_claim);
    ASSERT_FALSE(expired.ok());
    EXPECT_EQ(expired.error().code, "request_result_expired");
    auto conflicting = first_claim;
    conflicting.intent.owner_transaction_id = "different.transaction";
    const auto conflict =
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), conflicting);
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error().code, "request_id_conflict");
    auto unseen = first_claim;
    unseen.intent.request_id = RequestId("request.unseen");
    unseen.intent.expected_admission_generation = AdmissionGeneration(3U);
    unseen.acquired_admission_generation = AdmissionGeneration(3U);
    unseen.claim_generation = CellAdmissionClaimGeneration(2U);
    const auto successor =
        journal.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.1"), unseen);
    ASSERT_TRUE(successor.ok());
    ASSERT_EQ(CountRows(database, "cell_admission_request_membership"), 1);
    ASSERT_EQ(CountRows(database, "cell_admission_claims"), 1);
    ASSERT_TRUE(journal
                    .RecordCellAdmissionClaimReleased(RequestOwnerEpoch("kernel.1"), unseen,
                                                      Release(unseen), AdmissionGeneration(5U))
                    .ok());
    first_claim = unseen;
    ASSERT_TRUE(journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.1"), 2).ok());
  }

  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 3, 1U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  ASSERT_EQ(recovery.value().recovered_cell_admission_claims.size(), 1U);
  EXPECT_EQ(recovery.value().recovered_cell_admission_claims.front().claim, first_claim);
  EXPECT_EQ(recovery.value().recovered_cell_admission_claims.front().release, Release(first_claim));
  EXPECT_GT(recovery.value().cell_admission_generation_high_water,
            first_claim.acquired_admission_generation);
  CellAdmissionReconcileDTO rotate{
      RequestOwnerEpoch("kernel.2"),
      {},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("authoring.owner.2")}},
      ContentDigest(kDigest)};
  const AdmissionGeneration rotated_generation(
      recovery.value().cell_admission_generation_high_water.value() + 1U);
  ASSERT_TRUE(replacement
                  .RecordCellAdmissionClaimsReconciled(RequestOwnerEpoch("kernel.2"), rotate,
                                                       RetainAll(rotate), rotated_generation)
                  .ok());
  auto fresh = first_claim;
  fresh.intent.owner_epoch = RequestOwnerEpoch("authoring.owner.2");
  fresh.intent.request_id = RequestId("request.2");
  fresh.intent.expected_admission_generation = rotated_generation;
  fresh.acquired_admission_generation = rotated_generation;
  fresh.claim_generation = CellAdmissionClaimGeneration(first_claim.claim_generation.value() + 1U);
  EXPECT_TRUE(
      replacement.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.2"), fresh).ok());
  const auto stale_exact =
      replacement.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.2"), first_claim);
  ASSERT_FALSE(stale_exact.ok());
  EXPECT_EQ(stale_exact.error().code, "stale_request_owner_epoch");
  auto stale = first_claim;
  stale.intent.request_id = RequestId("request.old.unseen");
  stale.claim_generation = CellAdmissionClaimGeneration(3U);
  stale.intent.expected_admission_generation = AdmissionGeneration(rotated_generation.value() + 1U);
  stale.acquired_admission_generation = stale.intent.expected_admission_generation;
  const auto stale_result =
      replacement.RecordCellAdmissionClaimAcquired(RequestOwnerEpoch("kernel.2"), stale);
  ASSERT_FALSE(stale_result.ok());
  EXPECT_EQ(stale_result.error().code, "stale_request_owner_epoch");
}

TEST(KernelCellAdmissionSqlite, SameOwnerCapacityPlusOneCyclesRetireReleasedResultAcrossRestart) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  std::optional<CellAdmissionClaimDTO> latest;
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 1U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.1"), 1U, clock, journal, {},
        recovery.value().cell_admission_generation_high_water, {},
        [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {},
        EmptyLivenessSet());
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    for (std::size_t index = 0; index != 3U; ++index) {
      auto intent = Claim().intent;
      intent.request_id = RequestId("cycle." + std::to_string(index));
      intent.owner_transaction_id = "transaction." + std::to_string(index);
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
    ASSERT_TRUE(journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.1"), 2).ok());
  }
  SqliteOperationJournal journal(database, CellId("cell.1"));
  auto recovery =
      journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 3, 1U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 1U, clock, journal,
      recovery.value().recovered_cell_admission_claims,
      recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); }, {},
      recovery.value().recovered_cell_admission_membership,
      recovery.value().recovered_cell_admission_current_owners, EmptyLivenessSet());
  const auto recovered =
      owner.GetCellAdmissionRecoveryReceipt({latest->intent, *latest, Release(*latest)});
  ASSERT_TRUE(recovered.ok());
  EXPECT_EQ(recovered.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto successor_intent = Claim().intent;
  successor_intent.request_id = RequestId("cycle.after.restart");
  successor_intent.owner_transaction_id = "transaction.after.restart";
  successor_intent.expected_admission_generation =
      owner.GetAuthoringAdmission().admission_generation;
  const auto successor = owner.AcquireCellAdmissionClaim(successor_intent);
  ASSERT_TRUE(successor.ok());
  const auto retired =
      owner.GetCellAdmissionRecoveryReceipt({latest->intent, *latest, Release(*latest)});
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
}

TEST(KernelCellAdmissionSqlite,
     ReplacementGenerationAdvancesAndPredecessorGenerationIsOnlyClaimEvidence) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  AdmissionGeneration predecessor_public(0U);
  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 1, 4U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(RequestOwnerEpoch("kernel.1"), 4U, clock, journal, {},
                                   recovery.value().cell_admission_generation_high_water);
    auto ready = CellAdmissionReadinessSnapshotDTO{
        true,
        ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
    predecessor_public = owner.GetAuthoringAdmission().admission_generation;
    ASSERT_TRUE(journal.CloseOwnerEpoch(RequestOwnerEpoch("kernel.1"), 2).ok());
  }
  SqliteOperationJournal replacement(database, CellId("cell.1"));
  auto recovery =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.2"), 3, 4U, {ResourceId("resource.1")});
  ASSERT_TRUE(recovery.ok());
  EXPECT_GT(recovery.value().cell_admission_generation_high_water.value(),
            predecessor_public.value());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 4U, clock, replacement,
      recovery.value().recovered_cell_admission_claims,
      recovery.value().cell_admission_generation_high_water, {},
      [](const auto&, const auto&) { return Result<void>::Ok(); }, {},
      recovery.value().recovered_cell_admission_membership,
      recovery.value().recovered_cell_admission_current_owners, EmptyLivenessSet());
  auto stale = Claim().intent;
  stale.expected_admission_generation = predecessor_public;
  // Without current readiness the replacement refuses on that fact.
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(stale).error().code, "authoring_not_ready");
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  // The predecessor's generation is claim evidence only: once the cell is
  // ready the claim is accepted and durably records both generations.
  const auto accepted = owner.AcquireCellAdmissionClaim(stale);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().intent.expected_admission_generation, predecessor_public);
  EXPECT_GT(accepted.value().acquired_admission_generation.value(), predecessor_public.value());
}

TEST(KernelCellAdmissionSqlite, RepeatedCleanOwnerRotationsKeepMembershipRowsBounded) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  AdmissionGeneration expected_generation(0U);
  CellAdmissionClaimGeneration claim_generation(1U);
  for (std::uint64_t index = 1U; index <= 5U; ++index) {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    const RequestOwnerEpoch kernel_epoch("kernel." + std::to_string(index));
    auto recovery = journal.BeginOwnerEpoch(kernel_epoch, static_cast<std::int64_t>(index * 10U),
                                            1U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    expected_generation = recovery.value().cell_admission_generation_high_water;
    const RequestOwnerEpoch request_epoch("authoring.owner." + std::to_string(index));
    if (index > 1U) {
      CellAdmissionReconcileDTO rotate{
          kernel_epoch,
          {},
          {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, request_epoch}},
          ContentDigest(kDigest)};
      expected_generation = AdmissionGeneration(expected_generation.value() + 1U);
      ASSERT_TRUE(journal
                      .RecordCellAdmissionClaimsReconciled(kernel_epoch, rotate, RetainAll(rotate),
                                                           expected_generation)
                      .ok());
    }
    auto claim = Claim();
    claim.intent.owner_epoch = request_epoch;
    claim.intent.request_id = RequestId("request." + std::to_string(index));
    claim.intent.expected_admission_generation = expected_generation;
    claim.acquired_admission_generation = expected_generation;
    claim.claim_generation = claim_generation;
    ASSERT_TRUE(journal.RecordCellAdmissionClaimAcquired(kernel_epoch, claim).ok());
    expected_generation = AdmissionGeneration(expected_generation.value() + 2U);
    ASSERT_TRUE(journal
                    .RecordCellAdmissionClaimReleased(kernel_epoch, claim, Release(claim),
                                                      expected_generation)
                    .ok());
    ASSERT_TRUE(
        journal.CloseOwnerEpoch(kernel_epoch, static_cast<std::int64_t>(index * 10U + 1U)).ok());
    claim_generation = CellAdmissionClaimGeneration(claim_generation.value() + 1U);
  }
  EXPECT_EQ(CountRows(database, "cell_admission_request_membership"), 1);
  EXPECT_EQ(CountRows(database, "cell_admission_claims"), 1);
  EXPECT_EQ(CountRows(database, "cell_admission_current_owners"), 1);
  EXPECT_LE(CountRows(database, "cell_admission_reconciliations"), 1);
}

TEST(KernelCellAdmissionSqlite, RepeatedCrashReplacementRetainsOnlyLatestReceiptAndHeldSuccessor) {
  TemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  FixedClock clock;
  auto predecessor = Claim();
  std::optional<CellAdmissionClaimDTO> obsolete;

  {
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery =
        journal.BeginOwnerEpoch(RequestOwnerEpoch("kernel.1"), 10, 2U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok());
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.1"), 2U, clock, journal, {},
        recovery.value().cell_admission_generation_high_water, {},
        [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {},
        EmptyLivenessSet());
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    auto intent = Claim().intent;
    intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
    auto acquired = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_TRUE(acquired.ok());
    predecessor = acquired.value();
  }

  for (std::uint64_t index = 2U; index <= 8U; ++index) {
    const RequestOwnerEpoch kernel_epoch("kernel." + std::to_string(index));
    const RequestOwnerEpoch successor_epoch("authoring.owner." + std::to_string(index));
    SqliteOperationJournal journal(database, CellId("cell.1"));
    auto recovery = journal.BeginOwnerEpoch(kernel_epoch, static_cast<std::int64_t>(index * 10U),
                                            2U, {ResourceId("resource.1")});
    ASSERT_TRUE(recovery.ok()) << index;
    KernelCellAdmissionOwner owner(
        kernel_epoch, 2U, clock, journal, recovery.value().recovered_cell_admission_claims,
        recovery.value().cell_admission_generation_high_water, {},
        [](const auto&, const auto&) { return Result<void>::Ok(); },
        [](const auto& held, const auto&, const auto& reconcile) {
          return Result<CellAdmissionReconcileValidationDTO>::Ok(
              {reconcile.current_owner_epochs,
               {{held.front(), CellAdmissionOwnerRoleDTO::kAuthoringOwner,
                 reconcile.current_owner_epochs.front().owner_epoch,
                 CellAdmissionSettlementKindDTO::kReconciled,
                 reconcile.reconciliation_evidence_digest}}});
        },
        recovery.value().recovered_cell_admission_membership,
        recovery.value().recovered_cell_admission_current_owners, EmptyLivenessSet());

    if (obsolete.has_value()) {
      const auto recovered =
          owner.GetCellAdmissionRecoveryReceipt({obsolete->intent, *obsolete, std::nullopt});
      ASSERT_TRUE(recovered.ok());
      EXPECT_EQ(recovered.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact);
    }
    const auto held =
        owner.GetCellAdmissionRecoveryReceipt({predecessor.intent, predecessor, std::nullopt});
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);

    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    const CellAdmissionReconcileDTO reconcile{
        kernel_epoch,
        {predecessor},
        {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, successor_epoch}},
        Digest(static_cast<char>('0' + index))};
    ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(reconcile).ok()) << index;
    if (obsolete.has_value()) {
      const auto retired =
          owner.GetCellAdmissionRecoveryReceipt({obsolete->intent, *obsolete, std::nullopt});
      ASSERT_TRUE(retired.ok());
      EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
    }
    const auto latest =
        owner.GetCellAdmissionRecoveryReceipt({predecessor.intent, predecessor, std::nullopt});
    ASSERT_TRUE(latest.ok());
    EXPECT_EQ(latest.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact);

    auto next_intent = Claim().intent;
    next_intent.owner_epoch = successor_epoch;
    next_intent.owner_transaction_id = "document.transaction." + std::to_string(index);
    next_intent.request_id = RequestId("request." + std::to_string(index));
    next_intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
    auto next = owner.AcquireCellAdmissionClaim(next_intent);
    ASSERT_TRUE(next.ok()) << index;
    obsolete = predecessor;
    predecessor = next.value();
  }

  EXPECT_EQ(CountRows(database, "cell_admission_claims"), 2);
  EXPECT_EQ(CountRows(database, "cell_admission_request_membership"), 2);
  EXPECT_EQ(CountRows(database, "cell_admission_reconcile_settlements"), 1);
  EXPECT_EQ(CountRows(database, "cell_admission_reconciliations"), 1);
}

}  // namespace
