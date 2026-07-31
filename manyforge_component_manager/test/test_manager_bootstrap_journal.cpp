#include "manyforge_component_manager/durable_store_set.hpp"
#include "manyforge_component_manager/manager_bootstrap_journal.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <manyforge_deployment/canonical_empty_admission.hpp>

namespace manyforge::component_manager {
namespace {

constexpr std::string_view kBundleDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kGraphDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kSceneDigest =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr std::string_view kOtherSceneDigest =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";
constexpr std::string_view kSceneIdDigest =
    "sha256:abababababababababababababababababababababababababababababababab";

deployment::CanonicalEmptyRetainedBundleAdmission
LoadCanonicalEmptyAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-bootstrap-journal-test-XXXXXX")
            .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto* created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

core::ProjectionNativeRuntimeBindingDTO Binding(
    const std::string& suffix) {
  return {
      core::CapabilityProviderBindingId("provider-" + suffix),
      core::CapabilityProviderBindingRevision("provider-revision-" + suffix),
      core::RuntimeUnitId("runtime-" + suffix),
      core::RuntimeEpoch("epoch-" + suffix),
  };
}

ManagerBootstrapIntentDTO Intent(
    const std::uint64_t generation = 1U) {
  return {
      core::BootstrapId("bootstrap-1"),
      core::BootstrapGeneration(generation),
      core::CellId("cell-a"),
      core::RetainedBundleId(std::string(kBundleDigest)),
      core::GraphRevision(std::string(kGraphDigest)),
      {Binding("a"), Binding("b")},
  };
}

ManagerBootstrapAcceptedTargetDTO AcceptedTarget(
    const std::string_view scene_digest = kSceneDigest,
    const std::uint64_t generation = 1U) {
  return {
      core::BootstrapId("bootstrap-1"),
      core::BootstrapGeneration(generation),
      core::ProjectionRevisionDTO{
          core::RetainedBundleId(std::string(kBundleDigest)),
          core::GraphRevision(std::string(kGraphDigest)),
          core::SceneRevision{
              core::SceneId(std::string(kSceneIdDigest)),
              1U,
              core::ContentDigest(std::string(scene_digest)),
          },
      },
  };
}

void RawExec(
    const std::filesystem::path& database_path,
    const std::string& sql) {
  sqlite3* database = nullptr;
  ASSERT_EQ(
      sqlite3_open(database_path.string().c_str(), &database),
      SQLITE_OK);
  char* error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string detail =
        error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    sqlite3_close(database);
    FAIL() << detail;
  }
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
}

std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open test file");
  }
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

void ExpectNoSqliteSidecars(const std::filesystem::path& path) {
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-journal"));
}

TEST(ManagerBootstrapJournalIntegrity,
     RejectsSchemaEmptyDatabaseWithForeignSqliteIdentity) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "bootstrap.sqlite3";
  RawExec(
      path,
      "PRAGMA application_id=424242;"
      "PRAGMA user_version=77;");
  const auto before = ReadFileBytes(path);
  ExpectNoSqliteSidecars(path);

  EXPECT_THROW(
      SqliteManagerBootstrapJournal(path, core::CellId("cell-a")),
      ManagerBootstrapJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), before);
  ExpectNoSqliteSidecars(path);
}

TEST(
    ManagerBootstrapJournalIntegrity,
    RejectsForeignSchemaAndCellWithoutChangingAtRestSource) {
  TemporaryDirectory directory;
  const auto schema_path = directory.path() / "foreign-schema.sqlite3";
  RawExec(schema_path, "CREATE TABLE foreign_table(value TEXT);");
  const auto schema_before = ReadFileBytes(schema_path);
  EXPECT_THROW(
      SqliteManagerBootstrapJournal(
          schema_path, core::CellId("cell-a")),
      ManagerBootstrapJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(schema_path), schema_before);
  ExpectNoSqliteSidecars(schema_path);

  const auto cell_path = directory.path() / "foreign-cell.sqlite3";
  {
    SqliteManagerBootstrapJournal owner(
        cell_path, core::CellId("foreign-cell"));
  }
  ExpectNoSqliteSidecars(cell_path);
  const auto cell_before = ReadFileBytes(cell_path);
  EXPECT_THROW(
      SqliteManagerBootstrapJournal(cell_path, core::CellId("cell-a")),
      ManagerBootstrapJournalConflictError);
  EXPECT_EQ(ReadFileBytes(cell_path), cell_before);
  ExpectNoSqliteSidecars(cell_path);
}

TEST(
    ManagerBootstrapJournalIntegrity,
    RejectsForeignCellFromWalCrashResidueWithoutChangingSource) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "wal-residue.sqlite3";
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      auto* owner = new SqliteManagerBootstrapJournal(
          path, core::CellId("foreign-cell"));
      static_cast<void>(owner);
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  const auto wal_path =
      std::filesystem::path(path.string() + "-wal");
  const auto shm_path =
      std::filesystem::path(path.string() + "-shm");
  ASSERT_TRUE(std::filesystem::exists(wal_path));
  std::filesystem::remove(shm_path);
  const auto database_before = ReadFileBytes(path);
  const auto wal_before = ReadFileBytes(wal_path);

  EXPECT_THROW(
      SqliteManagerBootstrapJournal(path, core::CellId("cell-a")),
      ManagerBootstrapJournalConflictError);
  EXPECT_EQ(ReadFileBytes(path), database_before);
  EXPECT_EQ(ReadFileBytes(wal_path), wal_before);
  EXPECT_FALSE(std::filesystem::exists(shm_path));

  EXPECT_NO_THROW({
    SqliteManagerBootstrapJournal recovered(
        path, core::CellId("foreign-cell"));
    EXPECT_EQ(recovered.cell_id(), core::CellId("foreign-cell"));
  });
}

TEST(ManagerBootstrapJournalValues, EnforcesClosedTypedShape) {
  auto invalid = Intent();
  invalid.native_runtime_bindings = {Binding("b"), Binding("a")};
  EXPECT_FALSE(invalid.valid());

  invalid = Intent();
  invalid.native_runtime_bindings.push_back(Binding("c"));
  while (invalid.native_runtime_bindings.size() <=
         kMaximumBootstrapNativeRuntimeBindings) {
    const auto suffix =
        std::string("z") +
        std::to_string(
            1000U + invalid.native_runtime_bindings.size());
    invalid.native_runtime_bindings.push_back(Binding(suffix));
  }
  EXPECT_FALSE(invalid.valid());

  auto invalid_target = AcceptedTarget();
  invalid_target.bootstrap_generation = core::BootstrapGeneration(0U);
  EXPECT_FALSE(invalid_target.valid());

  ManagerBootstrapJournalSnapshot empty{
      core::CellId("cell-a"),
      ManagerBootstrapJournalPhase::kEmpty,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_TRUE(empty.valid());
  empty.phase = ManagerBootstrapJournalPhase::kAcceptedTargetPersisted;
  EXPECT_FALSE(empty.valid());
}

TEST(ManagerBootstrapJournalValues, RejectsInvalidDurableInputs) {
  TemporaryDirectory directory;
  SqliteManagerBootstrapJournal journal(
      directory.path() / "journal.sqlite3", core::CellId("cell-a"));

  auto bad_bundle = Intent();
  bad_bundle.canonical_empty_retained_bundle_id =
      core::RetainedBundleId("not-a-digest");
  EXPECT_THROW(
      static_cast<void>(journal.EnsureIntent(bad_bundle)),
      std::invalid_argument);

  auto too_large = Intent();
  too_large.bootstrap_generation = core::BootstrapGeneration(
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
      1U);
  EXPECT_THROW(
      static_cast<void>(journal.EnsureIntent(too_large)),
      std::invalid_argument);

  auto oversized_identity = Intent();
  oversized_identity.bootstrap_id = core::BootstrapId(
      std::string(kMaximumBootstrapJournalIdentifierBytes + 1U, 'a'));
  EXPECT_THROW(
      static_cast<void>(journal.EnsureIntent(oversized_identity)),
      std::invalid_argument);

  auto non_printable_identity = Intent();
  non_printable_identity.bootstrap_id =
      core::BootstrapId("bootstrap\ninvalid");
  EXPECT_THROW(
      static_cast<void>(journal.EnsureIntent(non_printable_identity)),
      std::invalid_argument);
  EXPECT_EQ(
      journal.VerifyIntegrity().bootstrap_attempt_count,
      0);
}

TEST(
    ManagerBootstrapJournal,
    IntentAndAcceptedTargetReplayExactlyAcrossReopen) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  std::string intent_digest;
  std::string accepted_digest;
  {
    SqliteManagerBootstrapJournal journal(
        database, core::CellId("cell-a"));
    const auto empty = journal.ReadSnapshot();
    EXPECT_TRUE(empty.valid());
    EXPECT_EQ(empty.phase, ManagerBootstrapJournalPhase::kEmpty);

    const auto intent = journal.EnsureIntent(Intent());
    EXPECT_TRUE(intent.inserted);
    intent_digest = intent.record.intent_digest.value();
    const auto intent_replay = journal.EnsureIntent(Intent());
    EXPECT_FALSE(intent_replay.inserted);
    EXPECT_EQ(intent_replay.record.intent_digest.value(), intent_digest);

    const auto accepted = journal.RecordAcceptedTarget(AcceptedTarget());
    EXPECT_TRUE(accepted.inserted);
    accepted_digest = accepted.record.accepted_target_digest.value();
    const auto accepted_replay =
        journal.RecordAcceptedTarget(AcceptedTarget());
    EXPECT_FALSE(accepted_replay.inserted);
    EXPECT_EQ(
        accepted_replay.record.accepted_target_digest.value(),
        accepted_digest);
  }

  SqliteManagerBootstrapJournal reopened(
      database, core::CellId("cell-a"));
  const auto snapshot = reopened.ReadSnapshot();
  ASSERT_TRUE(snapshot.valid());
  EXPECT_EQ(
      snapshot.phase,
      ManagerBootstrapJournalPhase::kAcceptedTargetPersisted);
  ASSERT_TRUE(snapshot.intent.has_value());
  ASSERT_TRUE(snapshot.accepted_target.has_value());
  EXPECT_EQ(snapshot.intent->intent_digest.value(), intent_digest);
  EXPECT_EQ(
      snapshot.accepted_target->accepted_target_digest.value(),
      accepted_digest);
  EXPECT_EQ(
      reopened.VerifyIntegrity().native_runtime_binding_row_count,
      2);
}

TEST(
    ManagerBootstrapJournal,
    DurableHighWaterAdvancesContiguouslyAndFencesStaleAcceptance) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  {
    SqliteManagerBootstrapJournal journal(
        database, core::CellId("cell-a"));
    EXPECT_FALSE(
        journal
            .ReadBootstrapGenerationHighWater(
                core::BootstrapId("bootstrap-1"))
            .has_value());

    static_cast<void>(journal.EnsureIntent(Intent(1U)));
    static_cast<void>(
        journal.RecordAcceptedTarget(AcceptedTarget(kSceneDigest, 1U)));
    ASSERT_TRUE(
        journal
            .ReadBootstrapGenerationHighWater(
                core::BootstrapId("bootstrap-1"))
            .has_value());
    EXPECT_EQ(
        journal
            .ReadBootstrapGenerationHighWater(
                core::BootstrapId("bootstrap-1"))
            ->value(),
        1U);

    const auto second = journal.EnsureIntent(Intent(2U));
    EXPECT_TRUE(second.inserted);
    EXPECT_FALSE(journal.EnsureIntent(Intent(1U)).inserted);
    EXPECT_FALSE(
        journal
            .RecordAcceptedTarget(AcceptedTarget(kSceneDigest, 1U))
            .inserted);

    EXPECT_THROW(
        static_cast<void>(journal.EnsureIntent(Intent(4U))),
        ManagerBootstrapJournalConflictError);
    auto changed_identity = Intent(3U);
    changed_identity.bootstrap_id =
        core::BootstrapId("bootstrap-other");
    EXPECT_THROW(
        static_cast<void>(journal.EnsureIntent(changed_identity)),
        ManagerBootstrapJournalConflictError);

    static_cast<void>(journal.EnsureIntent(Intent(3U)));
    EXPECT_THROW(
        static_cast<void>(
            journal.RecordAcceptedTarget(
                AcceptedTarget(kSceneDigest, 2U))),
        ManagerBootstrapJournalConflictError);
    static_cast<void>(
        journal.RecordAcceptedTarget(AcceptedTarget(kSceneDigest, 3U)));
    EXPECT_THROW(
        static_cast<void>(
            journal.ReadBootstrapGenerationHighWater(
                core::BootstrapId("bootstrap-other"))),
        ManagerBootstrapJournalConflictError);
  }

  SqliteManagerBootstrapJournal reopened(
      database, core::CellId("cell-a"));
  const auto report = reopened.VerifyIntegrity();
  ASSERT_TRUE(report.bootstrap_id.has_value());
  ASSERT_TRUE(report.bootstrap_generation_high_water.has_value());
  EXPECT_EQ(report.bootstrap_id->value(), "bootstrap-1");
  EXPECT_EQ(report.bootstrap_generation_high_water->value(), 3U);
  EXPECT_EQ(report.bootstrap_attempt_count, 3);
  EXPECT_EQ(report.accepted_target_count, 2);
  const auto snapshot = reopened.ReadSnapshot();
  ASSERT_TRUE(snapshot.intent.has_value());
  EXPECT_EQ(snapshot.intent->intent.bootstrap_generation.value(), 3U);
  EXPECT_EQ(
      snapshot.phase,
      ManagerBootstrapJournalPhase::kAcceptedTargetPersisted);
}

TEST(ManagerBootstrapJournal, BootstrapAttemptInventoryIsBounded) {
  TemporaryDirectory directory;
  SqliteManagerBootstrapJournal journal(
      directory.path() / "journal.sqlite3", core::CellId("cell-a"));
  for (std::uint64_t generation = 1U;
       generation <= kMaximumBootstrapAttempts;
       ++generation) {
    auto intent = Intent(generation);
    intent.native_runtime_bindings.clear();
    EXPECT_TRUE(journal.EnsureIntent(intent).inserted);
  }
  auto exhausted = Intent(kMaximumBootstrapAttempts + 1U);
  exhausted.native_runtime_bindings.clear();
  EXPECT_THROW(
      static_cast<void>(journal.EnsureIntent(exhausted)),
      ManagerBootstrapJournalLimitError);
  EXPECT_EQ(
      journal.VerifyIntegrity().bootstrap_attempt_count,
      static_cast<std::int64_t>(kMaximumBootstrapAttempts));
}

TEST(ManagerBootstrapJournal, ConflictsNeverAdvanceDurablePhase) {
  TemporaryDirectory directory;
  SqliteManagerBootstrapJournal journal(
      directory.path() / "journal.sqlite3", core::CellId("cell-a"));
  static_cast<void>(journal.EnsureIntent(Intent()));

  auto conflicting_intent = Intent();
  conflicting_intent.bootstrap_id = core::BootstrapId("bootstrap-2");
  EXPECT_THROW(
      static_cast<void>(journal.EnsureIntent(conflicting_intent)),
      ManagerBootstrapJournalConflictError);
  EXPECT_EQ(
      journal.ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kIntentPersisted);

  auto conflicting_target = AcceptedTarget(kOtherSceneDigest);
  static_cast<void>(journal.RecordAcceptedTarget(AcceptedTarget()));
  EXPECT_THROW(
      static_cast<void>(journal.RecordAcceptedTarget(conflicting_target)),
      ManagerBootstrapJournalConflictError);
  EXPECT_EQ(
      journal.ReadSnapshot()
          .accepted_target->accepted_target.canonical_empty_target
          .scene_revision.content_hash.value(),
      kSceneDigest);
}

TEST(
    ManagerBootstrapJournal,
    AcceptedTargetCannotPrecedeOrContradictIntent) {
  TemporaryDirectory directory;
  SqliteManagerBootstrapJournal journal(
      directory.path() / "journal.sqlite3", core::CellId("cell-a"));
  EXPECT_THROW(
      static_cast<void>(journal.RecordAcceptedTarget(AcceptedTarget())),
      ManagerBootstrapJournalConflictError);
  static_cast<void>(journal.EnsureIntent(Intent()));

  auto wrong_bundle = AcceptedTarget();
  wrong_bundle.canonical_empty_target.retained_bundle_id =
      core::RetainedBundleId(
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  EXPECT_THROW(
      static_cast<void>(journal.RecordAcceptedTarget(wrong_bundle)),
      ManagerBootstrapJournalConflictError);
  EXPECT_EQ(
      journal.ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kIntentPersisted);
}

TEST(
    ManagerBootstrapJournal,
    IndependentWritersSerializeOneIntentAndAcceptedTarget) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  SqliteManagerBootstrapJournal first(
      database, core::CellId("cell-a"));
  SqliteManagerBootstrapJournal second(
      database, core::CellId("cell-a"));

  auto first_intent = std::async(
      std::launch::async,
      [&first] { return first.EnsureIntent(Intent()); });
  auto second_intent = std::async(
      std::launch::async,
      [&second] { return second.EnsureIntent(Intent()); });
  const auto first_intent_result = first_intent.get();
  const auto second_intent_result = second_intent.get();
  EXPECT_EQ(
      static_cast<int>(first_intent_result.inserted) +
          static_cast<int>(second_intent_result.inserted),
      1);

  auto first_target = std::async(
      std::launch::async,
      [&first] { return first.RecordAcceptedTarget(AcceptedTarget()); });
  auto second_target = std::async(
      std::launch::async,
      [&second] { return second.RecordAcceptedTarget(AcceptedTarget()); });
  const auto first_target_result = first_target.get();
  const auto second_target_result = second_target.get();
  EXPECT_EQ(
      static_cast<int>(first_target_result.inserted) +
          static_cast<int>(second_target_result.inserted),
      1);
  EXPECT_EQ(first.VerifyIntegrity().accepted_target_count, 1);
  EXPECT_EQ(second.VerifyIntegrity().accepted_target_count, 1);
}

TEST(ManagerBootstrapJournal, ConcurrentFirstOpenConvergesExactSchema) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  std::vector<std::future<ManagerBootstrapJournalIntegrityReport>> openings;
  for (std::size_t index = 0U; index < 4U; ++index) {
    openings.push_back(std::async(
        std::launch::async,
        [&database] {
          SqliteManagerBootstrapJournal journal(
              database, core::CellId("cell-a"));
          return journal.VerifyIntegrity();
        }));
  }
  std::string schema_digest;
  for (auto& opening : openings) {
    const auto report = opening.get();
    EXPECT_EQ(report.phase, ManagerBootstrapJournalPhase::kEmpty);
    if (schema_digest.empty()) {
      schema_digest = report.schema_artifact_digest;
    } else {
      EXPECT_EQ(report.schema_artifact_digest, schema_digest);
    }
  }
}

TEST(ManagerBootstrapJournalCrash, CommittedChildWriteSurvivesProcessExit) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  {
    SqliteManagerBootstrapJournal initialized(
        database, core::CellId("cell-a"));
  }
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      SqliteManagerBootstrapJournal journal(
          database, core::CellId("cell-a"));
      static_cast<void>(journal.EnsureIntent(Intent()));
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  SqliteManagerBootstrapJournal reopened(
      database, core::CellId("cell-a"));
  EXPECT_EQ(
      reopened.ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kIntentPersisted);
}

TEST(ManagerBootstrapJournalCrash, UncommittedAcceptedTargetDisappears) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  {
    SqliteManagerBootstrapJournal journal(
        database, core::CellId("cell-a"));
    static_cast<void>(journal.EnsureIntent(Intent()));
  }

  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(database.string().c_str(), &raw) != SQLITE_OK) {
      _exit(1);
    }
    const auto result = sqlite3_exec(
        raw,
        "PRAGMA foreign_keys=ON;"
        "BEGIN IMMEDIATE;"
        "INSERT INTO manager_bootstrap_accepted_targets ("
        "bootstrap_id,bootstrap_generation,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash,accepted_target_digest"
        ") VALUES ('bootstrap-1',1,'"
        "sha256:1111111111111111111111111111111111111111111111111111111111111111',"
        "'sha256:2222222222222222222222222222222222222222222222222222222222222222',"
        "'sha256:abababababababababababababababababababababababababababababababab',1,'"
        "sha256:3333333333333333333333333333333333333333333333333333333333333333',"
        "'sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');",
        nullptr,
        nullptr,
        nullptr);
    _exit(result == SQLITE_OK ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  SqliteManagerBootstrapJournal reopened(
      database, core::CellId("cell-a"));
  EXPECT_EQ(
      reopened.ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kIntentPersisted);
}

TEST(ManagerBootstrapJournalIntegrity, WrongStoredDigestFailsReopen) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  {
    SqliteManagerBootstrapJournal journal(
        database, core::CellId("cell-a"));
  }
  RawExec(
      database,
      "INSERT INTO manager_bootstrap_intents ("
      "bootstrap_id,bootstrap_generation,"
      "canonical_empty_retained_bundle_id,"
      "canonical_empty_graph_revision,"
      "native_runtime_binding_set_digest,intent_digest"
      ") VALUES ('bootstrap-1',1,'"
      "sha256:1111111111111111111111111111111111111111111111111111111111111111',"
      "'sha256:2222222222222222222222222222222222222222222222222222222222222222',"
      "'sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
      "'sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb');");
  EXPECT_THROW(
      SqliteManagerBootstrapJournal(
          database, core::CellId("cell-a")),
      ManagerBootstrapJournalIntegrityError);
}

TEST(ManagerBootstrapJournalIntegrity, UnexpectedSchemaObjectFailsReopen) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  {
    SqliteManagerBootstrapJournal journal(
        database, core::CellId("cell-a"));
  }
  RawExec(
      database,
      "CREATE VIEW injected_bootstrap_view AS "
      "SELECT bootstrap_id FROM manager_bootstrap_intents;");
  EXPECT_THROW(
      SqliteManagerBootstrapJournal(
          database, core::CellId("cell-a")),
      ManagerBootstrapJournalIntegrityError);
}

TEST(ManagerBootstrapJournalIntegrity, DurableRowsRejectRawMutation) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  {
    SqliteManagerBootstrapJournal journal(
        database, core::CellId("cell-a"));
    static_cast<void>(journal.EnsureIntent(Intent()));
    static_cast<void>(journal.RecordAcceptedTarget(AcceptedTarget()));
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  const std::vector<std::string> forbidden_mutations{
      "UPDATE manager_bootstrap_intents "
      "SET bootstrap_id='bootstrap-replaced' "
      "WHERE bootstrap_id='bootstrap-1' AND bootstrap_generation=1;",
      "DELETE FROM manager_bootstrap_native_runtime_bindings "
      "WHERE bootstrap_id='bootstrap-1' AND bootstrap_generation=1;",
      "UPDATE manager_bootstrap_accepted_targets "
      "SET candidate_scene_id='scene-replaced' "
      "WHERE bootstrap_id='bootstrap-1' AND bootstrap_generation=1;",
  };
  for (const auto& sql : forbidden_mutations) {
    char* error = nullptr;
    EXPECT_EQ(
        sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, &error),
        SQLITE_CONSTRAINT);
    sqlite3_free(error);
  }
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

  SqliteManagerBootstrapJournal reopened(
      database, core::CellId("cell-a"));
  EXPECT_EQ(
      reopened.ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kAcceptedTargetPersisted);
}

TEST(ManagerBootstrapJournalValues, RejectsUnsafeDatabasePaths) {
  TemporaryDirectory directory;
  const auto target = directory.path() / "target.sqlite3";
  const auto alias = directory.path() / "alias.sqlite3";
  {
    std::ofstream stream(target);
    ASSERT_TRUE(stream);
  }
  std::filesystem::create_symlink(target, alias);
  EXPECT_THROW(
      SqliteManagerBootstrapJournal(alias, core::CellId("cell-a")),
      std::invalid_argument);

  const auto oversized = directory.path() / "oversized.sqlite3";
  {
    std::ofstream stream(oversized);
    ASSERT_TRUE(stream);
  }
  std::filesystem::resize_file(
      oversized,
      static_cast<std::uintmax_t>(
          kMaximumBootstrapJournalDatabasePageImageBytes) +
          1U);
  EXPECT_THROW(
      SqliteManagerBootstrapJournal(
          oversized, core::CellId("cell-a")),
      ManagerBootstrapJournalLimitError);
}

TEST(ManagerBootstrapJournal, DifferentCellCannotReopenStore) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "journal.sqlite3";
  {
    SqliteManagerBootstrapJournal journal(
        database, core::CellId("cell-a"));
  }
  EXPECT_THROW(
      SqliteManagerBootstrapJournal(
          database, core::CellId("cell-b")),
      ManagerBootstrapJournalConflictError);
}

TEST(ManagerBootstrapJournal, ClosedStoreRejectsUse) {
  TemporaryDirectory directory;
  SqliteManagerBootstrapJournal journal(
      directory.path() / "journal.sqlite3", core::CellId("cell-a"));
  journal.Close();
  EXPECT_THROW(
      static_cast<void>(journal.ReadSnapshot()),
      ManagerBootstrapJournalClosedError);
  EXPECT_NO_THROW(journal.Close());
}

TEST(
    ExternallyProvisionedManagerStores,
    OpensSameCellStoresAndDoesNotPublishOrCreateBootstrapIntent) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  {
    ExternallyProvisionedManagerStores stores(
        directory.path(), admission);
    const auto inventory = stores.StartupInventory();
    EXPECT_EQ(
        inventory.bootstrap_journal.phase,
        ManagerBootstrapJournalPhase::kEmpty);
    EXPECT_EQ(inventory.active_bundle_publication.receipt_count, 0);
    EXPECT_EQ(
        inventory.active_bundle_publication.current_generation.value(),
        0U);
    EXPECT_EQ(
        stores.bootstrap_journal().database_path().filename(),
        kManagerBootstrapJournalFilename);
    EXPECT_EQ(
        stores.active_bundle_publication_store()
            .database_path().filename(),
        kActiveBundlePublicationStoreFilename);
    EXPECT_EQ(
        stores.apply_journal().database_path().filename(),
        kManagerApplyJournalFilename);
    EXPECT_EQ(inventory.apply_journal.apply_intent_count, 0);
    EXPECT_EQ(stores.cell_id(), admission.cell_id());
  }

  ExternallyProvisionedManagerStores reopened(
      directory.path(), admission);
  const auto inventory = reopened.StartupInventory();
  EXPECT_EQ(
      inventory.bootstrap_journal.bootstrap_attempt_count,
      0);
  EXPECT_EQ(inventory.active_bundle_publication.receipt_count, 0);
  EXPECT_EQ(inventory.apply_journal.apply_intent_count, 0);
}

TEST(
    ExternallyProvisionedManagerStores,
    RejectsEveryHeaderInitializedForeignPeerBeforeCreatingMissingStores) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  const std::vector<std::string_view> filenames{
      kManagerBootstrapJournalFilename,
      kActiveBundlePublicationStoreFilename,
      kManagerApplyJournalFilename,
  };

  for (std::size_t index = 0U; index < filenames.size(); ++index) {
    SCOPED_TRACE(filenames[index]);
    const auto stable =
        directory.path() / ("header-foreign-" + std::to_string(index));
    std::filesystem::create_directory(stable);
    RawExec(
        stable / std::string(filenames[index]),
        "PRAGMA application_id=424242;"
        "PRAGMA user_version=77;");
    const auto foreign_path = stable / std::string(filenames[index]);
    const auto foreign_before = ReadFileBytes(foreign_path);
    ExpectNoSqliteSidecars(foreign_path);

    EXPECT_ANY_THROW(
        ExternallyProvisionedManagerStores(stable, admission));
    EXPECT_EQ(ReadFileBytes(foreign_path), foreign_before);
    ExpectNoSqliteSidecars(foreign_path);
    for (std::size_t peer = 0U; peer < filenames.size(); ++peer) {
      if (peer != index) {
        EXPECT_FALSE(std::filesystem::exists(
            stable / std::string(filenames[peer])));
      }
    }
  }
}

TEST(
    ExternallyProvisionedManagerStores,
    RejectsEveryOrphanSidecarBeforeCreatingAnyDatabasePeer) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  const std::vector<std::string_view> filenames{
      kManagerBootstrapJournalFilename,
      kActiveBundlePublicationStoreFilename,
      kManagerApplyJournalFilename,
  };
  const std::vector<std::string_view> suffixes{
      "-wal",
      "-shm",
      "-journal",
  };

  std::size_t case_index = 0U;
  for (const auto filename : filenames) {
    for (const auto suffix : suffixes) {
      SCOPED_TRACE(std::string(filename) + std::string(suffix));
      const auto stable =
          directory.path() / ("orphan-" + std::to_string(case_index++));
      std::filesystem::create_directory(stable);
      const auto main_path = stable / std::string(filename);
      const auto orphan_path =
          std::filesystem::path(main_path.string() + std::string(suffix));
      {
        std::ofstream orphan(orphan_path, std::ios::binary);
        ASSERT_TRUE(orphan);
        orphan << "orphan-sidecar";
      }
      const auto orphan_before = ReadFileBytes(orphan_path);

      EXPECT_ANY_THROW(
          ExternallyProvisionedManagerStores(stable, admission));
      EXPECT_EQ(ReadFileBytes(orphan_path), orphan_before);
      for (const auto peer : filenames) {
        EXPECT_FALSE(std::filesystem::exists(stable / std::string(peer)));
      }
    }
  }
}

TEST(
    ExternallyProvisionedManagerStores,
    RejectsMissingSymlinkOrConflictingCellDirectoryState) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  const auto foreign_cell = core::CellId("foreign-cell");
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          directory.path() / "missing", admission),
      std::invalid_argument);

  const auto actual = directory.path() / "actual";
  const auto alias = directory.path() / "alias";
  std::filesystem::create_directory(actual);
  std::filesystem::create_directory_symlink(actual, alias);
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          alias, admission),
      std::invalid_argument);

  {
    SqliteManagerBootstrapJournal journal(
        actual / std::string(kManagerBootstrapJournalFilename),
        foreign_cell);
    SqliteActiveBundlePublicationStore publication(
        actual / std::string(kActiveBundlePublicationStoreFilename),
        foreign_cell);
  }
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          actual, admission),
      ManagerBootstrapJournalConflictError);

  const auto publication_only = directory.path() / "publication-only";
  std::filesystem::create_directory(publication_only);
  {
    SqliteActiveBundlePublicationStore publication(
        publication_only /
            std::string(kActiveBundlePublicationStoreFilename),
        foreign_cell);
  }
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          publication_only, admission),
      ActiveBundlePublicationConflictError);
  EXPECT_FALSE(std::filesystem::exists(
      publication_only /
      std::string(kManagerBootstrapJournalFilename)));
  EXPECT_FALSE(std::filesystem::exists(
      publication_only /
      std::string(kManagerApplyJournalFilename)));

  const auto journal_only = directory.path() / "journal-only";
  std::filesystem::create_directory(journal_only);
  {
    SqliteManagerBootstrapJournal journal(
        journal_only / std::string(kManagerBootstrapJournalFilename),
        foreign_cell);
  }
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          journal_only, admission),
      ManagerBootstrapJournalConflictError);
  EXPECT_FALSE(std::filesystem::exists(
      journal_only /
      std::string(kActiveBundlePublicationStoreFilename)));
  EXPECT_FALSE(std::filesystem::exists(
      journal_only /
      std::string(kManagerApplyJournalFilename)));

  const auto apply_only = directory.path() / "apply-only";
  std::filesystem::create_directory(apply_only);
  {
    SqliteManagerApplyJournal apply(
        apply_only / std::string(kManagerApplyJournalFilename),
        foreign_cell);
  }
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(apply_only, admission),
      ManagerApplyJournalConflictError);
  EXPECT_FALSE(std::filesystem::exists(
      apply_only /
      std::string(kManagerBootstrapJournalFilename)));
  EXPECT_FALSE(std::filesystem::exists(
      apply_only /
      std::string(kActiveBundlePublicationStoreFilename)));

  const auto foreign_empty_journal_peer =
      directory.path() / "foreign-empty-journal-peer";
  std::filesystem::create_directory(foreign_empty_journal_peer);
  {
    SqliteActiveBundlePublicationStore publication(
        foreign_empty_journal_peer /
            std::string(kActiveBundlePublicationStoreFilename),
        foreign_cell);
  }
  {
    std::ofstream empty(
        foreign_empty_journal_peer /
        std::string(kManagerBootstrapJournalFilename));
    ASSERT_TRUE(empty);
  }
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          foreign_empty_journal_peer, admission),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(
      std::filesystem::file_size(
          foreign_empty_journal_peer /
          std::string(kManagerBootstrapJournalFilename)),
      0U);
  EXPECT_FALSE(std::filesystem::exists(
      foreign_empty_journal_peer /
      std::string(kManagerApplyJournalFilename)));

  const auto foreign_empty_publication_peer =
      directory.path() / "foreign-empty-publication-peer";
  std::filesystem::create_directory(foreign_empty_publication_peer);
  {
    SqliteManagerBootstrapJournal journal(
        foreign_empty_publication_peer /
            std::string(kManagerBootstrapJournalFilename),
        foreign_cell);
  }
  {
    std::ofstream empty(
        foreign_empty_publication_peer /
        std::string(kActiveBundlePublicationStoreFilename));
    ASSERT_TRUE(empty);
  }
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          foreign_empty_publication_peer, admission),
      ManagerBootstrapJournalConflictError);
  EXPECT_EQ(
      std::filesystem::file_size(
          foreign_empty_publication_peer /
          std::string(kActiveBundlePublicationStoreFilename)),
      0U);
  EXPECT_FALSE(std::filesystem::exists(
      foreign_empty_publication_peer /
      std::string(kManagerApplyJournalFilename)));

  const auto foreign_empty_apply_peer =
      directory.path() / "foreign-empty-apply-peer";
  std::filesystem::create_directory(foreign_empty_apply_peer);
  {
    SqliteManagerBootstrapJournal journal(
        foreign_empty_apply_peer /
            std::string(kManagerBootstrapJournalFilename),
        foreign_cell);
  }
  {
    std::ofstream empty(
        foreign_empty_apply_peer /
        std::string(kManagerApplyJournalFilename));
    ASSERT_TRUE(empty);
  }
  EXPECT_THROW(
      ExternallyProvisionedManagerStores(
          foreign_empty_apply_peer, admission),
      ManagerBootstrapJournalConflictError);
  EXPECT_EQ(
      std::filesystem::file_size(
          foreign_empty_apply_peer /
          std::string(kManagerApplyJournalFilename)),
      0U);
  EXPECT_FALSE(std::filesystem::exists(
      foreign_empty_apply_peer /
      std::string(kActiveBundlePublicationStoreFilename)));

  const auto empty_journal_peer =
      directory.path() / "empty-journal-peer";
  std::filesystem::create_directory(empty_journal_peer);
  {
    SqliteActiveBundlePublicationStore publication(
        empty_journal_peer /
            std::string(kActiveBundlePublicationStoreFilename),
        admission.cell_id());
  }
  {
    std::ofstream empty(
        empty_journal_peer /
        std::string(kManagerBootstrapJournalFilename));
    ASSERT_TRUE(empty);
  }
  EXPECT_NO_THROW(
      ExternallyProvisionedManagerStores(
          empty_journal_peer, admission));

  const auto empty_publication_peer =
      directory.path() / "empty-publication-peer";
  std::filesystem::create_directory(empty_publication_peer);
  {
    SqliteManagerBootstrapJournal journal(
        empty_publication_peer /
            std::string(kManagerBootstrapJournalFilename),
        admission.cell_id());
  }
  {
    std::ofstream empty(
        empty_publication_peer /
        std::string(kActiveBundlePublicationStoreFilename));
    ASSERT_TRUE(empty);
  }
  EXPECT_NO_THROW(
      ExternallyProvisionedManagerStores(
          empty_publication_peer, admission));

  const auto empty_apply_peer =
      directory.path() / "empty-apply-peer";
  std::filesystem::create_directory(empty_apply_peer);
  {
    SqliteManagerBootstrapJournal journal(
        empty_apply_peer /
            std::string(kManagerBootstrapJournalFilename),
        admission.cell_id());
  }
  {
    SqliteActiveBundlePublicationStore publication(
        empty_apply_peer /
            std::string(kActiveBundlePublicationStoreFilename),
        admission.cell_id());
  }
  {
    std::ofstream empty(
        empty_apply_peer /
        std::string(kManagerApplyJournalFilename));
    ASSERT_TRUE(empty);
  }
  EXPECT_NO_THROW(
      ExternallyProvisionedManagerStores(
          empty_apply_peer, admission));
}

}  // namespace
}  // namespace manyforge::component_manager
