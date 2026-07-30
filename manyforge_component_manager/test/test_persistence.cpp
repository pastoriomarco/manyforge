#include "manyforge_component_manager/persistence.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <vector>

namespace manyforge::component_manager {
namespace {

constexpr std::string_view kDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

std::vector<std::byte> Bytes(const std::string_view value) {
  return {
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()),
  };
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-manager-test-XXXXXX")
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

BootstrapIdentity Bootstrap() {
  return BootstrapIdentity(
      "bootstrap-1",
      1,
      std::string(kDigest),
      "empty-bundle",
      "empty-graph");
}

ApplyIdentity Apply() {
  return ApplyIdentity(
      "apply-1",
      1,
      "request-1",
      "manager-epoch-1",
      std::string(kDigest),
      1,
      "empty-bundle",
      "empty-graph",
      "cell-bundle",
      "cell-graph");
}

std::string ShellQuote(const std::string& value) {
  std::string result{"'"};
  for (const char character : value) {
    if (character == '\'') {
      result += "'\\''";
    } else {
      result.push_back(character);
    }
  }
  result.push_back('\'');
  return result;
}

void RunPythonParity(
    const std::string& operation,
    const std::filesystem::path& database) {
  const auto source_root = std::filesystem::path(MANYFORGE_SOURCE_ROOT);
  const auto helper =
      source_root /
      "manyforge_component_manager/testdata/persistence_cross_language.py";
  const auto command =
      "PYTHONPATH=" + ShellQuote(source_root.string()) + " python3 " +
      ShellQuote(helper.string()) + " " + ShellQuote(operation) + " " +
      ShellQuote(database.string());
  ASSERT_EQ(std::system(command.c_str()), 0);
}

template <typename Callable>
void ExpectPersistenceIntegrityMessage(
    Callable&& callable,
    const std::string_view expected_message) {
  try {
    callable();
    FAIL() << "expected PersistenceIntegrityError";
  } catch (const PersistenceIntegrityError& error) {
    EXPECT_EQ(error.what(), expected_message);
  } catch (const std::exception& error) {
    FAIL() << "expected PersistenceIntegrityError, got: " << error.what();
  }
}

TEST(PersistenceValues, RejectInvalidInputsAndOversizedPayload) {
  EXPECT_THROW(
      OperationRef(OperationKind::kBootstrap, "", 1),
      std::invalid_argument);
  EXPECT_THROW(
      ApplyIdentity(
          "apply",
          1,
          "request",
          "epoch",
          "not-a-digest",
          1,
          "bundle",
          "graph",
          "target",
          "target-graph"),
      std::invalid_argument);
  EXPECT_NO_THROW(JournalEvent(
      "entry",
      "event",
      std::vector<std::byte>(kMaximumJournalPayloadBytes)));
  EXPECT_THROW(
      JournalEvent(
          "entry",
          "event",
          std::vector<std::byte>(kMaximumJournalPayloadBytes + 1U)),
      PersistenceLimitError);
}

TEST(PersistenceValues, RejectFinalDatabasePathSymlink) {
  TemporaryDirectory directory;
  const auto target = directory.path() / "target.sqlite3";
  const auto alias = directory.path() / "alias.sqlite3";
  {
    std::ofstream stream(target);
    ASSERT_TRUE(stream);
  }
  std::filesystem::create_symlink(target, alias);
  EXPECT_THROW(
      ManagerPersistence(alias, "cell-a"),
      std::invalid_argument);
}

TEST(PersistenceValues, RejectOversizedExistingDatabaseBeforeSchemaReads) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "oversized.sqlite3";
  {
    std::ofstream stream(database);
    ASSERT_TRUE(stream);
  }
  std::filesystem::resize_file(
      database,
      static_cast<std::uintmax_t>(
          kMaximumMainDatabasePageImageBytes) +
          1U);
  EXPECT_THROW(
      ManagerPersistence(database, "cell-a"),
      PersistenceLimitError);
}

TEST(Persistence, InitializeAppendAdvanceAndReopenDeterministically) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  std::string operation_digest;
  std::string receipt_digest;
  {
    ManagerPersistence store(database, "cell-a");
    const auto empty = store.VerifyIntegrity();
    EXPECT_EQ(empty.operation_count, 0);
    EXPECT_EQ(empty.journal_entry_count, 0);
    EXPECT_EQ(empty.foundation_receipt_count, 0);
    EXPECT_FALSE(store.FoundationHeadValue().has_value());

    const auto bootstrap = store.EnsureBootstrap(Bootstrap());
    EXPECT_TRUE(bootstrap.inserted);
    operation_digest = bootstrap.record.identity_digest();
    const auto duplicate = store.EnsureBootstrap(Bootstrap());
    EXPECT_FALSE(duplicate.inserted);
    EXPECT_EQ(duplicate.record.identity_digest(), operation_digest);

    const auto journal = store.AppendJournalEvent(
        bootstrap.record.ref(),
        JournalEvent("entry-1", "intent_recorded", Bytes("payload")));
    EXPECT_TRUE(journal.inserted);
    EXPECT_EQ(journal.entry.sequence(), 1);
    const auto journal_replay = store.AppendJournalEvent(
        bootstrap.record.ref(),
        JournalEvent("entry-1", "intent_recorded", Bytes("payload")));
    EXPECT_FALSE(journal_replay.inserted);
    EXPECT_EQ(journal_replay.entry.entry_digest(), journal.entry.entry_digest());

    const auto advance = store.AdvanceFoundationHead(
        FoundationAdvanceIntent(
            "advance-1",
            bootstrap.record.ref(),
            bootstrap.record.identity_digest()));
    EXPECT_TRUE(advance.inserted);
    receipt_digest = advance.receipt.receipt_digest();
    const auto advance_replay = store.AdvanceFoundationHead(
        FoundationAdvanceIntent(
            "advance-1",
            bootstrap.record.ref(),
            bootstrap.record.identity_digest()));
    EXPECT_FALSE(advance_replay.inserted);
    EXPECT_EQ(advance_replay.receipt.receipt_digest(), receipt_digest);
  }
  {
    ManagerPersistence reopened(database, "cell-a");
    const auto report = reopened.VerifyIntegrity();
    EXPECT_EQ(report.operation_count, 1);
    EXPECT_EQ(report.journal_entry_count, 1);
    EXPECT_EQ(report.foundation_receipt_count, 1);
    EXPECT_EQ(report.foundation_head_generation, 1);
    EXPECT_EQ(report.foundation_head_receipt_digest, receipt_digest);
    const auto head = reopened.FoundationHeadValue();
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ(head->bundle_identity(), "empty-bundle");
    EXPECT_EQ(head->graph_revision(), "empty-graph");
    EXPECT_EQ(
        reopened.Operation(
            OperationRef(OperationKind::kBootstrap, "bootstrap-1", 1))
            ->identity_digest(),
        operation_digest);
  }
}

TEST(Persistence, ConflictsDoNotMutateDurableState) {
  TemporaryDirectory directory;
  ManagerPersistence store(
      directory.path() / "manager.sqlite3",
      "cell-a");
  const auto bootstrap = store.EnsureBootstrap(Bootstrap()).record;
  EXPECT_THROW(
      store.EnsureBootstrap(BootstrapIdentity(
          "bootstrap-1",
          1,
          std::string(kDigest),
          "different-bundle",
          "empty-graph")),
      PersistenceConflictError);
  const auto first = store.AppendJournalEvent(
      bootstrap.ref(),
      JournalEvent("entry-1", "event", Bytes("one")));
  EXPECT_TRUE(first.inserted);
  EXPECT_THROW(
      store.AppendJournalEvent(
          bootstrap.ref(),
          JournalEvent("entry-1", "event", Bytes("two"))),
      PersistenceConflictError);
  EXPECT_THROW(
      store.AdvanceFoundationHead(FoundationAdvanceIntent(
          "advance",
          bootstrap.ref(),
          std::string(kDigest))),
      PersistenceConflictError);
  const auto report = store.VerifyIntegrity();
  EXPECT_EQ(report.operation_count, 1);
  EXPECT_EQ(report.journal_entry_count, 1);
  EXPECT_EQ(report.foundation_receipt_count, 0);
}

TEST(Persistence, ApplyRowsAndFoundationCasFormOneOrderedChain) {
  TemporaryDirectory directory;
  ManagerPersistence store(
      directory.path() / "manager.sqlite3",
      "cell-a");
  const auto bootstrap = store.EnsureBootstrap(Bootstrap()).record;
  const auto bootstrap_advance = store.AdvanceFoundationHead(
      FoundationAdvanceIntent(
          "advance-1",
          bootstrap.ref(),
          bootstrap.identity_digest()));
  EXPECT_EQ(bootstrap_advance.receipt.committed_generation(), 1);

  const auto apply = store.EnsureApply(Apply()).record;
  const auto apply_advance = store.AdvanceFoundationHead(
      FoundationAdvanceIntent(
          "advance-2",
          apply.ref(),
          apply.identity_digest()));
  EXPECT_EQ(apply_advance.receipt.expected_generation(), 1);
  EXPECT_EQ(apply_advance.receipt.committed_generation(), 2);
  const auto head = store.FoundationHeadValue();
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(head->generation(), 2);
  EXPECT_EQ(head->bundle_identity(), "cell-bundle");
  EXPECT_EQ(head->graph_revision(), "cell-graph");

  const auto stale = store.EnsureApply(ApplyIdentity(
      "apply-stale",
      1,
      "request-stale",
      "manager-epoch-1",
      std::string(kDigest),
      1,
      "empty-bundle",
      "empty-graph",
      "stale-bundle",
      "stale-graph")).record;
  EXPECT_THROW(
      store.AdvanceFoundationHead(FoundationAdvanceIntent(
          "stale",
          stale.ref(),
          stale.identity_digest())),
      PersistenceConflictError);
}

TEST(Persistence, IndependentAdaptersSerializeOneDurableWriter) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  ManagerPersistence first(database, "cell-a");
  ManagerPersistence second(database, "cell-a");

  auto first_write = std::async(
      std::launch::async,
      [&first] { return first.EnsureBootstrap(Bootstrap()); });
  auto second_write = std::async(
      std::launch::async,
      [&second] { return second.EnsureBootstrap(Bootstrap()); });
  const auto first_result = first_write.get();
  const auto second_result = second_write.get();
  EXPECT_EQ(
      static_cast<int>(first_result.inserted) +
          static_cast<int>(second_result.inserted),
      1);
  EXPECT_EQ(
      first_result.record.identity_digest(),
      second_result.record.identity_digest());

  const auto event = JournalEvent(
      "shared-entry",
      "shared-event",
      Bytes("same"));
  auto first_append = std::async(
      std::launch::async,
      [&first, &first_result, &event] {
        return first.AppendJournalEvent(first_result.record.ref(), event);
      });
  auto second_append = std::async(
      std::launch::async,
      [&second, &second_result, &event] {
        return second.AppendJournalEvent(second_result.record.ref(), event);
      });
  const auto first_journal = first_append.get();
  const auto second_journal = second_append.get();
  EXPECT_EQ(
      static_cast<int>(first_journal.inserted) +
          static_cast<int>(second_journal.inserted),
      1);
  EXPECT_EQ(first.VerifyIntegrity().journal_entry_count, 1);
  EXPECT_EQ(second.VerifyIntegrity().journal_entry_count, 1);
}

TEST(Persistence, ConcurrentFirstOpenConvergesOneExactStore) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  std::vector<std::future<IntegrityReport>> openings;
  for (std::size_t index = 0; index < 4U; ++index) {
    openings.push_back(std::async(
        std::launch::async,
        [&database] {
          ManagerPersistence store(database, "cell-a");
          return store.VerifyIntegrity();
        }));
  }
  std::string schema_digest;
  for (auto& opening : openings) {
    const auto report = opening.get();
    EXPECT_EQ(report.operation_count, 0);
    if (schema_digest.empty()) {
      schema_digest = report.schema_artifact_digest;
    } else {
      EXPECT_EQ(report.schema_artifact_digest, schema_digest);
    }
  }
  ManagerPersistence reopened(database, "cell-a");
  EXPECT_EQ(reopened.VerifyIntegrity().operation_count, 0);
}

TEST(PersistenceCrashWindows, UncommittedSchemaAndJournalWritesDisappear) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(
                raw,
                "BEGIN EXCLUSIVE;"
                "CREATE TABLE transient_uncommitted(value INTEGER);",
                nullptr,
                nullptr,
                nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  raw = nullptr;

  {
    ManagerPersistence store(database, "cell-a");
    const auto bootstrap = store.EnsureBootstrap(Bootstrap()).record;
    EXPECT_EQ(store.VerifyIntegrity().operation_count, 1);
    store.Close();

    ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(
                  raw,
                  "PRAGMA foreign_keys=ON;"
                  "BEGIN IMMEDIATE;"
                  "INSERT INTO manager_journal_entries ("
                  "operation_kind,operation_id,operation_generation,sequence,"
                  "entry_id,event_kind,payload,payload_digest,"
                  "previous_entry_digest,entry_digest) VALUES ("
                  "'bootstrap','bootstrap-1',1,1,'uncommitted','event',"
                  "X'00','sha256:"
                  "1111111111111111111111111111111111111111111111111111111111111111',"
                  "'sha256:"
                  "1111111111111111111111111111111111111111111111111111111111111111',"
                  "'sha256:"
                  "1111111111111111111111111111111111111111111111111111111111111111'"
                  ");",
                  nullptr,
                  nullptr,
                  nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    raw = nullptr;

    ManagerPersistence reopened(database, "cell-a");
    EXPECT_TRUE(reopened.Journal(bootstrap.ref()).empty());
    EXPECT_EQ(reopened.VerifyIntegrity().journal_entry_count, 0);
  }
}

TEST(PersistenceCrashWindows, CommittedHeadSurvivesBeforeLaterJournal) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  OperationRef bootstrap_ref(OperationKind::kBootstrap, "bootstrap-1", 1);
  {
    ManagerPersistence store(database, "cell-a");
    const auto bootstrap = store.EnsureBootstrap(Bootstrap()).record;
    static_cast<void>(store.AdvanceFoundationHead(
        FoundationAdvanceIntent(
            "advance-1",
            bootstrap.ref(),
            bootstrap.identity_digest())));
  }
  {
    ManagerPersistence reopened(database, "cell-a");
    ASSERT_TRUE(reopened.FoundationHeadValue().has_value());
    EXPECT_EQ(reopened.FoundationHeadValue()->generation(), 1);
    EXPECT_TRUE(reopened.Journal(bootstrap_ref).empty());
    static_cast<void>(reopened.AppendJournalEvent(
        bootstrap_ref,
        JournalEvent("after-head", "later_record", Bytes("later"))));
  }
  {
    ManagerPersistence replay(database, "cell-a");
    EXPECT_EQ(replay.FoundationHeadValue()->generation(), 1);
    EXPECT_EQ(replay.Journal(bootstrap_ref).size(), 1U);
    EXPECT_EQ(replay.VerifyIntegrity().journal_entry_count, 1);
  }
}

TEST(PersistenceIntegrity, CorruptedFoundationHeadIsRejectedOnReopen) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  {
    ManagerPersistence store(database, "cell-a");
    const auto bootstrap = store.EnsureBootstrap(Bootstrap()).record;
    static_cast<void>(store.AdvanceFoundationHead(
        FoundationAdvanceIntent(
            "advance-1",
            bootstrap.ref(),
            bootstrap.identity_digest())));
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(
          raw,
          "UPDATE fixture_cas_head SET bundle_identity='corrupted'",
          nullptr,
          nullptr,
          nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  EXPECT_THROW(
      ManagerPersistence(database, "cell-a"),
      PersistenceIntegrityError);
}

TEST(PersistenceIntegrity, UnexpectedPersistentSchemaObjectIsRejected) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  {
    ManagerPersistence store(database, "cell-a");
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(
          raw,
          "CREATE VIEW injected_schema_object AS "
          "SELECT operation_id FROM manager_operations",
          nullptr,
          nullptr,
          nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  ExpectPersistenceIntegrityMessage(
      [&] { ManagerPersistence reopened(database, "cell-a"); },
      "SQLite schema object cardinality exceeds the versioned artifact");
}

TEST(
    PersistenceIntegrity,
    OversizedOperationInventoryIsRejectedBeforeRowMaterialization) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  {
    ManagerPersistence store(database, "cell-a");
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  const auto insert =
      std::string(
          "BEGIN IMMEDIATE;"
          "WITH RECURSIVE digits(value) AS ("
          "VALUES(0) UNION ALL SELECT value+1 FROM digits WHERE value<9"
          "), numbers(value) AS ("
          "SELECT 1+a.value+10*b.value+100*c.value+1000*d.value "
          "FROM digits AS a CROSS JOIN digits AS b "
          "CROSS JOIN digits AS c CROSS JOIN digits AS d "
          "WHERE 1+a.value+10*b.value+100*c.value+1000*d.value<=4097"
          ") INSERT INTO manager_operations ("
          "operation_kind,operation_id,operation_generation,request_id,"
          "manager_owner_epoch,intent_digest,expected_fixture_generation,"
          "predecessor_bundle_identity,predecessor_graph_revision,"
          "target_bundle_identity,target_graph_revision,identity_digest,"
          "created_sequence) SELECT "
          "'apply',CASE WHEN value=1 THEN '' ELSE printf('apply-%d',value) END,"
          "1,printf('request-%d',value),'epoch-1','") +
      std::string(kDigest) +
      "',1,'predecessor-bundle','predecessor-graph','target-bundle',"
      "'target-graph','" +
      std::string(kDigest) +
      "',value FROM numbers;"
      "COMMIT;";
  ASSERT_EQ(
      sqlite3_exec(raw, insert.c_str(), nullptr, nullptr, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

  ExpectPersistenceIntegrityMessage(
      [&] { ManagerPersistence reopened(database, "cell-a"); },
      "stored persistence cardinality exceeds a fixed limit");
}

TEST(
    PersistenceIntegrity,
    OversizedJournalGroupIsRejectedBeforeEntryMaterialization) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  {
    ManagerPersistence store(database, "cell-a");
    static_cast<void>(store.EnsureBootstrap(Bootstrap()));
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  const auto insert =
      std::string(
          "PRAGMA foreign_keys=ON;"
          "BEGIN IMMEDIATE;"
          "WITH RECURSIVE sequence(value) AS ("
          "VALUES(1) UNION ALL SELECT value+1 FROM sequence WHERE value<513"
          ") INSERT INTO manager_journal_entries ("
          "operation_kind,operation_id,operation_generation,sequence,"
          "entry_id,event_kind,payload,payload_digest,"
          "previous_entry_digest,entry_digest) SELECT "
          "'bootstrap','bootstrap-1',1,value,printf('entry-%d',value),"
          "'event',X'',CASE WHEN value=1 THEN 'invalid' ELSE '") +
      std::string(kDigest) +
      "' END,'" + std::string(kDigest) + "','" + std::string(kDigest) +
      "' FROM sequence;"
      "COMMIT;";
  ASSERT_EQ(
      sqlite3_exec(raw, insert.c_str(), nullptr, nullptr, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

  ExpectPersistenceIntegrityMessage(
      [&] { ManagerPersistence reopened(database, "cell-a"); },
      "per-operation journal count exceeds limit");
}

TEST(PersistenceParity, PythonAndCppOpenAndVerifyEachOthersBytes) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "manager.sqlite3";
  RunPythonParity("create-python", database);

  {
    ManagerPersistence store(database, "cell-a");
    const auto initial = store.VerifyIntegrity();
    EXPECT_EQ(initial.operation_count, 1);
    EXPECT_EQ(initial.journal_entry_count, 1);
    EXPECT_EQ(initial.foundation_receipt_count, 1);

    const auto apply = store.EnsureApply(Apply()).record;
    static_cast<void>(store.AppendJournalEvent(
        apply.ref(),
        JournalEvent("entry-2", "cpp_recorded", Bytes("cpp-payload"))));
    static_cast<void>(store.AdvanceFoundationHead(
        FoundationAdvanceIntent(
            "advance-2",
            apply.ref(),
            apply.identity_digest())));
    EXPECT_EQ(store.VerifyIntegrity().foundation_head_generation, 2);
  }
  RunPythonParity("verify-cpp", database);
}

TEST(PersistenceParity, CppOwnsAnExactCopyOfReferenceSchemaDuringPort) {
  const auto root = std::filesystem::path(MANYFORGE_SOURCE_ROOT);
  std::ifstream cpp_schema(
      root /
      "manyforge_component_manager/resources/"
      "001_component_manager_foundation.sql",
      std::ios::binary);
  std::ifstream python_schema(
      root /
      "manyforge_supervision/persistence/schema/"
      "001_component_manager_fixture_persistence.sql",
      std::ios::binary);
  ASSERT_TRUE(cpp_schema);
  ASSERT_TRUE(python_schema);
  const std::string cpp_bytes{
      std::istreambuf_iterator<char>(cpp_schema),
      std::istreambuf_iterator<char>()};
  const std::string python_bytes{
      std::istreambuf_iterator<char>(python_schema),
      std::istreambuf_iterator<char>()};
  EXPECT_EQ(cpp_bytes, python_bytes);
}

TEST(Persistence, ClosedStoreRejectsFurtherUse) {
  TemporaryDirectory directory;
  ManagerPersistence store(
      directory.path() / "manager.sqlite3",
      "cell-a");
  store.Close();
  EXPECT_THROW(store.VerifyIntegrity(), PersistenceClosedError);
  EXPECT_NO_THROW(store.Close());
}

}  // namespace
}  // namespace manyforge::component_manager
