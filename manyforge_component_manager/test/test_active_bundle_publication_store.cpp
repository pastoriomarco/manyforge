#include "manyforge_component_manager/active_bundle_publication_store.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <barrier>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_persistence/sqlite_record_writer.hpp>

namespace manyforge::component_manager {
namespace {

using namespace manyforge::core;

constexpr std::string_view kBundleDigest =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kGraphDigest =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kSceneDigest =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr std::string_view kSceneIdDigest =
    "sha256:abababababababababababababababababababababababababababababababab";
constexpr std::string_view kMaterializationDigest =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr std::string_view kOtherDigest =
    "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
constexpr std::string_view kBootstrapPointerGolden =
    "sha256:72c91b770116284cfe0fee2cd1c90478d445155806f228209794ae5036e20b9c";

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-publication-store-test-XXXXXX")
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

ProjectionRevisionDTO Projection(
    const std::string_view bundle = kBundleDigest,
    const std::string_view graph = kGraphDigest,
    const std::uint64_t scene_revision = 1U) {
  return ProjectionRevisionDTO{
      RetainedBundleId{std::string(bundle)},
      GraphRevision{std::string(graph)},
      SceneRevision{
          SceneId{std::string(kSceneIdDigest)},
          scene_revision,
          ContentDigest{std::string(kSceneDigest)}},
  };
}

ActiveBundlePublicationRequestDTO BootstrapRequest() {
  return ActiveBundlePublicationRequestDTO{
      BootstrapPublicationTransactionDTO{
          BootstrapId{"bootstrap-1"}, BootstrapGeneration{1U}},
      Projection(),
      RetainedBundleId{std::string(kBundleDigest)},
      GraphRevision{std::string(kGraphDigest)},
      std::nullopt,
      ContentDigest{std::string(kMaterializationDigest)},
  };
}

ActiveBundlePublicationRequestDTO ApplyRequest(
    const std::uint64_t apply_id,
    const std::uint64_t expected_generation,
    const std::uint64_t scene_revision = 2U) {
  const auto projection =
      Projection(kOtherDigest, kSceneDigest, scene_revision);
  return ActiveBundlePublicationRequestDTO{
      NamedApplyPublicationTransactionDTO{
          ApplyId{apply_id},
          ApplyGeneration{1U}},
      projection,
      projection.retained_bundle_id,
      projection.graph_revision,
      ActiveBundlePublicationGeneration{expected_generation},
      ContentDigest{std::string(kMaterializationDigest)},
  };
}

void SqliteExec(sqlite3* database, const std::string& sql) {
  char* error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  const std::string detail =
      error == nullptr ? sqlite3_errmsg(database) : error;
  sqlite3_free(error);
  ASSERT_EQ(result, SQLITE_OK) << detail;
}

void SqliteExecPath(
    const std::filesystem::path& path,
    const std::string& sql) {
  sqlite3* database = nullptr;
  ASSERT_EQ(
      sqlite3_open_v2(
          path.string().c_str(),
          &database,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
              SQLITE_OPEN_FULLMUTEX,
          nullptr),
      SQLITE_OK);
  SqliteExec(database, sql);
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

// One integer read straight from the file, as the session report reads it.
std::int64_t QueryInteger(
    const std::filesystem::path& path,
    const std::string& sql) {
  sqlite3* raw = nullptr;
  EXPECT_EQ(
      sqlite3_open_v2(
          path.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr),
      SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  EXPECT_EQ(
      sqlite3_prepare_v2(raw, sql.c_str(), -1, &statement, nullptr),
      SQLITE_OK);
  EXPECT_EQ(sqlite3_step(statement), SQLITE_ROW);
  const auto value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(raw);
  return value;
}

TEST(ActiveBundlePublicationStoreIntegrity,
     RejectsSchemaEmptyDatabaseWithForeignSqliteIdentity) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "publication.sqlite3";
  sqlite3* database = nullptr;
  ASSERT_EQ(
      sqlite3_open_v2(
          path.string().c_str(),
          &database,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
              SQLITE_OPEN_FULLMUTEX,
          nullptr),
      SQLITE_OK);
  SqliteExec(
      database,
      "PRAGMA application_id=424242;"
      "PRAGMA user_version=77;");
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  const auto before = ReadFileBytes(path);
  ExpectNoSqliteSidecars(path);

  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(path, CellId{"cell-a"}),
      ActiveBundlePublicationIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), before);
  ExpectNoSqliteSidecars(path);
}

TEST(
    ActiveBundlePublicationStoreIntegrity,
    RejectsForeignSchemaAndCellWithoutChangingAtRestSource) {
  TemporaryDirectory directory;
  const auto schema_path = directory.path() / "foreign-schema.sqlite3";
  SqliteExecPath(schema_path, "CREATE TABLE foreign_table(value TEXT);");
  const auto schema_before = ReadFileBytes(schema_path);
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(
          schema_path, CellId{"cell-a"}),
      ActiveBundlePublicationIntegrityError);
  EXPECT_EQ(ReadFileBytes(schema_path), schema_before);
  ExpectNoSqliteSidecars(schema_path);

  const auto cell_path = directory.path() / "foreign-cell.sqlite3";
  {
    SqliteActiveBundlePublicationStore owner(
        cell_path, CellId{"foreign-cell"});
  }
  ExpectNoSqliteSidecars(cell_path);
  const auto cell_before = ReadFileBytes(cell_path);
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(cell_path, CellId{"cell-a"}),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(ReadFileBytes(cell_path), cell_before);
  ExpectNoSqliteSidecars(cell_path);
}

TEST(
    ActiveBundlePublicationStoreIntegrity,
    RejectsForeignCellFromWalCrashResidueWithoutChangingSource) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "wal-residue.sqlite3";
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      auto* owner = new SqliteActiveBundlePublicationStore(
          path, CellId{"foreign-cell"});
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
      SqliteActiveBundlePublicationStore(path, CellId{"cell-a"}),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(ReadFileBytes(path), database_before);
  EXPECT_EQ(ReadFileBytes(wal_path), wal_before);
  EXPECT_FALSE(std::filesystem::exists(shm_path));

  EXPECT_NO_THROW({
    SqliteActiveBundlePublicationStore recovered(
        path, CellId{"foreign-cell"});
    EXPECT_EQ(recovered.cell_id(), CellId{"foreign-cell"});
  });
}

TEST(ActiveBundlePublicationStoreValues, RejectsInvalidDurableInputs) {
  TemporaryDirectory directory;
  SqliteActiveBundlePublicationStore store(
      directory.path() / "publication.sqlite3", CellId{"cell-a"});

  auto invalid = BootstrapRequest();
  invalid.private_materialization_content_digest =
      ContentDigest{"not-a-digest"};
  ASSERT_TRUE(invalid.valid());
  EXPECT_THROW(
      store.PublishActiveBundle(invalid), std::invalid_argument);

  invalid = BootstrapRequest();
  invalid.transaction_ref = BootstrapPublicationTransactionDTO{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) +
          1U}};
  ASSERT_TRUE(invalid.valid());
  EXPECT_THROW(
      store.PublishActiveBundle(invalid), std::invalid_argument);

  const auto zero =
      store.ReadActiveBundlePublication(
          ActiveBundlePublicationGeneration{0U});
  ASSERT_FALSE(zero.ok());
  EXPECT_EQ(zero.error().code, "invalid_request");
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 0);
}

TEST(
    ActiveBundlePublicationStore,
    BootstrapRereadAndRestartPreserveExactDurableReceipt) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  ActiveBundlePublicationReceiptDTO expected = [&] {
    SqliteActiveBundlePublicationStore writer(
        database, CellId{"cell-a"});
    const auto receipt =
        writer.PublishActiveBundle(BootstrapRequest());
    EXPECT_EQ(receipt.publication_generation.value(), 1U);
    EXPECT_TRUE(receipt.valid());
    EXPECT_EQ(
        receipt.durable_public_pointer_content_digest.value(),
        kBootstrapPointerGolden);

    // A second store object on the open file would not see this store's
    // records; the reader capability reads through the store itself.
    const IActiveBundlePublicationReader& reader = writer;
    const auto reread =
        reader.ReadActiveBundlePublication(receipt.publication_generation);
    EXPECT_TRUE(reread.ok());
    if (reread.ok()) {
      EXPECT_EQ(reread.value(), receipt);
    }
    return receipt;
  }();

  SqliteActiveBundlePublicationStore reopened(
      database, CellId{"cell-a"});
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.receipt_count, 1);
  EXPECT_EQ(report.current_generation.value(), 1U);
  EXPECT_EQ(
      report.current_pointer_digest,
      expected.durable_public_pointer_content_digest.value());
  const auto reread = reopened.ReadActiveBundlePublication(
      ActiveBundlePublicationGeneration{1U});
  ASSERT_TRUE(reread.ok());
  EXPECT_EQ(reread.value(), expected);
}

TEST(
    ActiveBundlePublicationStore,
    ExactReplayCasConflictAndStaleFocusedReadHaveNoExtraEffect) {
  TemporaryDirectory directory;
  SqliteActiveBundlePublicationStore store(
      directory.path() / "publication.sqlite3", CellId{"cell-a"});

  const auto bootstrap =
      store.PublishActiveBundle(BootstrapRequest());
  EXPECT_EQ(store.PublishActiveBundle(BootstrapRequest()), bootstrap);
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 1);

  auto conflicting_replay = BootstrapRequest();
  conflicting_replay.private_materialization_content_digest =
      ContentDigest{std::string(kOtherDigest)};
  EXPECT_THROW(
      store.PublishActiveBundle(conflicting_replay),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 1);

  auto stale = ApplyRequest(9U, 3U);
  ASSERT_TRUE(stale.valid());
  EXPECT_THROW(
      store.PublishActiveBundle(stale),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 1);

  const auto apply =
      store.PublishActiveBundle(ApplyRequest(1U, 1U));
  EXPECT_EQ(apply.publication_generation.value(), 2U);
  EXPECT_EQ(
      store.PublishActiveBundle(ApplyRequest(1U, 1U)),
      apply);
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 2);

  // Immutable historical receipts exist only for Publish replay and store
  // integrity. The scene-owner read capability proves the current pointer.
  EXPECT_EQ(store.PublishActiveBundle(BootstrapRequest()), bootstrap);
  const auto old_read = store.ReadActiveBundlePublication(
      bootstrap.publication_generation);
  ASSERT_FALSE(old_read.ok());
  EXPECT_EQ(old_read.error().code, "stale_generation");
  EXPECT_EQ(old_read.error().details.at("current_generation"), "2");

  const auto current_read =
      store.ReadActiveBundlePublication(apply.publication_generation);
  ASSERT_TRUE(current_read.ok());
  EXPECT_EQ(current_read.value(), apply);
}

// The file is verified completely once, at open. While the store is open it
// answers from memory and never reads the file; a foreign change to the file
// is found at the next open.
TEST(
    ActiveBundlePublicationStoreIntegrity,
    VerifiesTheFileAtOpenAndAnswersFromMemoryAfterwards) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "publication.sqlite3";
  {
    SqliteActiveBundlePublicationStore store(path, CellId{"cell-a"});
    EXPECT_EQ(store.complete_verification_count(), 1U);
    static_cast<void>(store.PublishActiveBundle(BootstrapRequest()));
    const auto first = store.VerifyIntegrity();
    static_cast<void>(store.PublishActiveBundle(ApplyRequest(1U, 1U)));
    const auto second = store.VerifyIntegrity();
    EXPECT_EQ(first.current_generation.value(), 1U);
    EXPECT_EQ(second.current_generation.value(), 2U);
    EXPECT_EQ(second.receipt_count, 2);
    EXPECT_EQ(second.schema_artifact_digest, first.schema_artifact_digest);
    EXPECT_EQ(store.complete_verification_count(), 1U);
  }
  SqliteExecPath(path, "CREATE TABLE unexpected(value TEXT) STRICT;");
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(path, CellId{"cell-a"}),
      ActiveBundlePublicationIntegrityError);
  SqliteExecPath(path, "DROP TABLE unexpected;");
  SqliteActiveBundlePublicationStore reopened(path, CellId{"cell-a"});
  EXPECT_EQ(reopened.VerifyIntegrity().current_generation.value(), 2U);
}

// Publish decides under the store's lock: concurrent duplicate bootstraps
// return one receipt, and the next compare-and-swap follows it.
TEST(
    ActiveBundlePublicationStore,
    ConcurrentPublishersSerializeDuplicateBootstrapAndNextCas) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  SqliteActiveBundlePublicationStore store(database, CellId{"cell-a"});

  auto first_write = std::async(
      std::launch::async,
      [&store] {
        return store.PublishActiveBundle(BootstrapRequest());
      });
  auto second_write = std::async(
      std::launch::async,
      [&store] {
        return store.PublishActiveBundle(BootstrapRequest());
      });
  const auto first_receipt = first_write.get();
  const auto second_receipt = second_write.get();
  EXPECT_EQ(first_receipt, second_receipt);
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 1);

  const auto next = store.PublishActiveBundle(ApplyRequest(1U, 1U));
  EXPECT_EQ(next.publication_generation.value(), 2U);
  EXPECT_EQ(store.VerifyIntegrity().current_generation.value(), 2U);
  store.Close();
  EXPECT_EQ(
      QueryInteger(
          database,
          "SELECT COUNT(*) FROM active_bundle_publication_receipts"),
      2);
}

TEST(
    ActiveBundlePublicationStore,
    ConcurrentFirstOpenConvergesOneExactEmptyStore) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  std::barrier open_together(4);
  std::vector<std::future<ActiveBundlePublicationStoreIntegrityReport>>
      openings;
  openings.reserve(4U);
  try {
    for (std::size_t index = 0; index < 4U; ++index) {
      openings.push_back(std::async(
          std::launch::async,
          [&database, &open_together, index, start] {
            start.get();
            open_together.arrive_and_wait();
            SqliteActiveBundlePublicationStore store(
                database, CellId{"cell-a"});
            const auto report = store.VerifyIntegrity();
            if (index % 2U == 0U) {
              store.Close();
            }
            return report;
          }));
    }
  } catch (...) {
    const auto launch_error = std::current_exception();
    start_promise.set_exception(launch_error);
    for (auto& opening : openings) {
      try {
        static_cast<void>(opening.get());
      } catch (...) {
      }
    }
    std::rethrow_exception(launch_error);
  }
  start_promise.set_value();
  std::string schema_digest;
  for (auto& opening : openings) {
    const auto report = opening.get();
    EXPECT_EQ(report.receipt_count, 0);
    EXPECT_EQ(report.current_generation.value(), 0U);
    if (schema_digest.empty()) {
      schema_digest = report.schema_artifact_digest;
    } else {
      EXPECT_EQ(report.schema_artifact_digest, schema_digest);
    }
  }
  ExpectNoSqliteSidecars(database);
  SqliteActiveBundlePublicationStore reopened(
      database, CellId{"cell-a"});
  EXPECT_EQ(reopened.VerifyIntegrity().receipt_count, 0);
}

TEST(
    ActiveBundlePublicationStore,
    ConcurrentDifferentRequestsAtOneExpectedGenerationHaveOneCasWinner) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  SqliteActiveBundlePublicationStore store(database, CellId{"cell-a"});
  static_cast<void>(store.PublishActiveBundle(BootstrapRequest()));

  struct Attempt final {
    bool committed;
    std::uint64_t apply_id;
  };
  const auto publish = [](SqliteActiveBundlePublicationStore& store,
                          const std::uint64_t apply_id) {
    try {
      const auto receipt =
          store.PublishActiveBundle(ApplyRequest(apply_id, 1U));
      const auto& transaction =
          std::get<NamedApplyPublicationTransactionDTO>(
              receipt.request.transaction_ref);
      return Attempt{true, transaction.apply_id.value()};
    } catch (const ActiveBundlePublicationConflictError&) {
      return Attempt{false, apply_id};
    }
  };
  auto first_write = std::async(
      std::launch::async,
      [&] { return publish(store, 1U); });
  auto second_write = std::async(
      std::launch::async,
      [&] { return publish(store, 2U); });
  const auto first_attempt = first_write.get();
  const auto second_attempt = second_write.get();
  EXPECT_EQ(
      static_cast<int>(first_attempt.committed) +
          static_cast<int>(second_attempt.committed),
      1);
  const auto report = store.VerifyIntegrity();
  EXPECT_EQ(report.receipt_count, 2);
  EXPECT_EQ(report.current_generation.value(), 2U);
  const auto current = store.ReadActiveBundlePublication(
      ActiveBundlePublicationGeneration{2U});
  ASSERT_TRUE(current.ok());
  const auto& current_transaction =
      std::get<NamedApplyPublicationTransactionDTO>(
          current.value().request.transaction_ref);
  const auto& winner =
      first_attempt.committed ? first_attempt.apply_id :
                                second_attempt.apply_id;
  const auto& loser =
      first_attempt.committed ? second_attempt.apply_id :
                                first_attempt.apply_id;
  EXPECT_EQ(current_transaction.apply_id.value(), winner);
  EXPECT_THROW(
      store.PublishActiveBundle(ApplyRequest(loser, 1U)),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 2);
}

TEST(
    ActiveBundlePublicationStoreCrashWindows,
    UncommittedReceiptAndHeadDisappearOnReopen) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  {
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"});
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  const auto digest = std::string(kBundleDigest);
  SqliteExec(
      raw,
      std::string(
          "BEGIN IMMEDIATE;"
          "INSERT INTO active_bundle_publication_receipts ("
          "publication_generation,transaction_kind,transaction_id,"
          "transaction_generation,projection_stage_generation,"
          "expected_previous_publication_generation,"
          "candidate_retained_bundle_id,candidate_graph_revision,"
          "candidate_scene_id,candidate_scene_revision_id,"
          "candidate_scene_content_hash,"
          "private_materialization_content_digest,"
          "obsolete_managed_command_surfaces_retired_evidence_digest,"
          "request_digest,durable_public_pointer_content_digest"
          ") VALUES (1,'bootstrap','bootstrap-uncommitted',1,0,0,'") +
          digest + "','" + digest + "','" + std::string(kSceneIdDigest) +
          "',1,'" + digest + "','" +
          digest + "','" + digest +
          "','sha256:"
          "1111111111111111111111111111111111111111111111111111111111111111',"
          "'sha256:"
          "2222222222222222222222222222222222222222222222222222222222222222');"
      "UPDATE active_bundle_publication_head SET "
      "publication_generation=1,"
      "request_digest='sha256:"
      "1111111111111111111111111111111111111111111111111111111111111111',"
      "durable_public_pointer_content_digest='sha256:"
      "2222222222222222222222222222222222222222222222222222222222222222'"
      " WHERE singleton=1;");
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

  SqliteActiveBundlePublicationStore reopened(
      database, CellId{"cell-a"});
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.receipt_count, 0);
  EXPECT_EQ(report.current_generation.value(), 0U);
}

TEST(
    ActiveBundlePublicationStoreCrashWindows,
    CommittedPointerSurvivesWithoutAnyManagerJournalWrite) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  ActiveBundlePublicationReceiptDTO receipt = [&] {
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"});
    return store.PublishActiveBundle(BootstrapRequest());
  }();

  SqliteActiveBundlePublicationStore reopened(
      database, CellId{"cell-a"});
  const auto read =
      reopened.ReadActiveBundlePublication(receipt.publication_generation);
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(read.value(), receipt);
}

TEST(
    ActiveBundlePublicationStoreIntegrity,
    CorruptedHeadFailsClosedOnReopen) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  SqliteActiveBundlePublicationStore store(
      database, CellId{"cell-a"});
  static_cast<void>(store.PublishActiveBundle(BootstrapRequest()));
  // The file is checked at open only; corrupt it once its record is written.
  store.Close();

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  SqliteExec(
      raw,
      "UPDATE active_bundle_publication_head SET "
      "durable_public_pointer_content_digest='sha256:"
      "9999999999999999999999999999999999999999999999999999999999999999'");
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(database, CellId{"cell-a"}),
      ActiveBundlePublicationIntegrityError);
}

// The complete receipt verification runs at open. A stored receipt whose
// fields no longer match its digests is detected there.
TEST(
    ActiveBundlePublicationStoreIntegrity,
    CorruptedReceiptFailsClosedOnReopen) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.sqlite3";
  const auto database = directory.path() / "publication.sqlite3";
  {
    SqliteActiveBundlePublicationStore writer(source, CellId{"cell-a"});
    static_cast<void>(writer.PublishActiveBundle(BootstrapRequest()));
  }
  {
    SqliteActiveBundlePublicationStore store(database, CellId{"cell-a"});
  }

  // Copy the valid receipt and head, changing one receipt field.
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  SqliteExec(
      raw,
      "ATTACH DATABASE '" + source.string() + "' AS source;"
      "BEGIN IMMEDIATE;"
      "INSERT INTO main.active_bundle_publication_receipts "
      "SELECT publication_generation,transaction_kind,transaction_id,"
      "transaction_generation,projection_stage_generation,"
      "expected_previous_publication_generation,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id+1,"
      "candidate_scene_content_hash,private_materialization_content_digest,"
      "obsolete_managed_command_surfaces_retired_evidence_digest,"
      "request_digest,durable_public_pointer_content_digest "
      "FROM source.active_bundle_publication_receipts;"
      "UPDATE main.active_bundle_publication_head SET "
      "(publication_generation,request_digest,"
      "durable_public_pointer_content_digest)=(SELECT "
      "publication_generation,request_digest,"
      "durable_public_pointer_content_digest "
      "FROM source.active_bundle_publication_head) WHERE singleton=1;"
      "COMMIT;"
      "DETACH DATABASE source;");
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

  const auto expect_digest_mismatch = [](const auto& action) {
    try {
      action();
      ADD_FAILURE() << "corrupted receipt was not detected";
    } catch (const ActiveBundlePublicationIntegrityError& error) {
      EXPECT_NE(
          std::string_view(error.what()).find("digest is inconsistent"),
          std::string_view::npos)
          << error.what();
    }
  };
  expect_digest_mismatch([&] {
    SqliteActiveBundlePublicationStore reopened(database, CellId{"cell-a"});
  });
}

TEST(
    ActiveBundlePublicationStoreIntegrity,
    UnexpectedPersistentSchemaObjectIsRejected) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  {
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"});
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  SqliteExec(
      raw,
      "CREATE VIEW injected_publication_view AS "
      "SELECT publication_generation FROM active_bundle_publication_head");
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(database, CellId{"cell-a"}),
      ActiveBundlePublicationIntegrityError);
}

TEST(
    ActiveBundlePublicationStoreIntegrity,
    OversizedReceiptInventoryIsRejectedBeforeRowMaterialization) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  {
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"});
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  const auto digest = std::string(kBundleDigest);
  const auto insert =
      std::string(
          "BEGIN IMMEDIATE;"
          "INSERT INTO active_bundle_publication_receipts ("
          "publication_generation,transaction_kind,transaction_id,"
          "transaction_generation,projection_stage_generation,"
          "expected_previous_publication_generation,"
          "candidate_retained_bundle_id,candidate_graph_revision,"
          "candidate_scene_id,candidate_scene_revision_id,"
          "candidate_scene_content_hash,"
          "private_materialization_content_digest,"
          "obsolete_managed_command_surfaces_retired_evidence_digest,"
          "request_digest,durable_public_pointer_content_digest"
          ") VALUES (1,'bootstrap','bootstrap-raw',1,0,0,'") +
      digest + "','" + digest + "','" + std::string(kSceneIdDigest) +
      "',1,'" + digest + "','" +
      digest + "','" + digest + "','" + digest + "','" + digest + "');"
      "WITH RECURSIVE generation(value) AS ("
      "VALUES(2) UNION ALL SELECT value+1 FROM generation WHERE value<4097"
      ") INSERT INTO active_bundle_publication_receipts ("
      "publication_generation,transaction_kind,transaction_id,"
      "transaction_generation,projection_stage_generation,"
      "expected_previous_publication_generation,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id,"
      "candidate_scene_content_hash,private_materialization_content_digest,"
      "obsolete_managed_command_surfaces_retired_evidence_digest,"
      "request_digest,durable_public_pointer_content_digest"
      ") SELECT value,'named_apply',printf('%d',value),1,1,value-1,'" +
      digest + "','" + digest + "','" + std::string(kSceneIdDigest) +
      "',value,'" + digest + "','" +
      digest + "','" + digest +
      "','sha256:'||printf('%064x',value),"
      "'sha256:'||printf('%064x',value+5000) FROM generation;"
      "COMMIT;";
  SqliteExec(raw, insert);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(database, CellId{"cell-a"}),
      ActiveBundlePublicationIntegrityError);
}

TEST(
    ActiveBundlePublicationStoreValues,
    RejectsSymlinkOversizeWrongCellMissingAndClosedAccess) {
  TemporaryDirectory directory;
  const auto target = directory.path() / "target.sqlite3";
  const auto alias = directory.path() / "alias.sqlite3";
  {
    SqliteActiveBundlePublicationStore store(
        target, CellId{"cell-a"});
    const auto missing = store.ReadActiveBundlePublication(
        ActiveBundlePublicationGeneration{1U});
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, "missing_reference");
  }
  std::filesystem::create_symlink(target, alias);
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(alias, CellId{"cell-a"}),
      std::invalid_argument);
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(target, CellId{"cell-b"}),
      ActiveBundlePublicationConflictError);

  const auto oversized = directory.path() / "oversized.sqlite3";
  {
    std::ofstream stream(oversized);
    ASSERT_TRUE(stream);
  }
  std::filesystem::resize_file(
      oversized,
      static_cast<std::uintmax_t>(
          kMaximumActiveBundlePublicationDatabasePageImageBytes) +
          1U);
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(
          oversized, CellId{"cell-a"}),
      ActiveBundlePublicationLimitError);

  SqliteActiveBundlePublicationStore closed(
      directory.path() / "closed.sqlite3", CellId{"cell-a"});
  closed.Close();
  const auto closed_read = closed.ReadActiveBundlePublication(
      ActiveBundlePublicationGeneration{1U});
  ASSERT_FALSE(closed_read.ok());
  EXPECT_EQ(closed_read.error().code, "internal_error");
  EXPECT_THROW(
      closed.VerifyIntegrity(),
      ActiveBundlePublicationStoreClosedError);
  EXPECT_NO_THROW(closed.Close());
}

// R3: the receipt inventory is bounded by eviction, never refusal. Publishing
// past the fixed limit keeps succeeding; the oldest receipts go, the head and
// a contiguous window survive, an evicted generation still reads as stale,
// and the store reopens and verifies.
TEST(ActiveBundlePublicationStoreBound, PublishingPastTheLimitEvictsOldest) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  constexpr auto kPublications =
      static_cast<std::uint64_t>(kMaximumActiveBundlePublicationReceipts) + 8U;
  std::optional<ActiveBundlePublicationReceiptDTO> last;
  {
    // Every publication is posted before the writer catches up; the queue
    // holds them all, so the file receives each one.
    persistence::SqliteRecordWriter writer(
        {2U * kPublications, 64U * 1024U * 1024U});
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"}, &writer);
    last = store.PublishActiveBundle(BootstrapRequest());
    for (std::uint64_t generation = 2U; generation <= kPublications;
         ++generation) {
      last = store.PublishActiveBundle(ApplyRequest(
          generation, generation - 1U));
      ASSERT_EQ(last->publication_generation.value(), generation);
    }
    // Exact replay of the head transaction still replays.
    EXPECT_EQ(store.PublishActiveBundle(ApplyRequest(
                  kPublications, kPublications - 1U)),
              *last);
    // A replay of an evicted transaction cannot republish.
    EXPECT_THROW(store.PublishActiveBundle(BootstrapRequest()),
                 ActiveBundlePublicationConflictError);
    const auto report = store.VerifyIntegrity();
    EXPECT_EQ(report.receipt_count, kMaximumActiveBundlePublicationReceipts);
    EXPECT_EQ(report.current_generation.value(), kPublications);
    const auto head = store.ReadActiveBundlePublication(
        ActiveBundlePublicationGeneration{kPublications});
    ASSERT_TRUE(head.ok()) << head.error().message;
    EXPECT_EQ(head.value(), *last);
    const auto evicted =
        store.ReadActiveBundlePublication(ActiveBundlePublicationGeneration{1U});
    ASSERT_FALSE(evicted.ok());
    EXPECT_EQ(evicted.error().code, "stale_generation");
    writer.DrainAndStop();
    store.Close();
    EXPECT_EQ(writer.records_dropped(), 0U);
    EXPECT_EQ(writer.records_not_written(), 0U);
  }
  SqliteActiveBundlePublicationStore reopened(database, CellId{"cell-a"});
  EXPECT_EQ(reopened.VerifyIntegrity().receipt_count,
            kMaximumActiveBundlePublicationReceipts);
  const auto reopened_head = reopened.ReadActiveBundlePublication(
      ActiveBundlePublicationGeneration{kPublications});
  ASSERT_TRUE(reopened_head.ok());
  EXPECT_EQ(reopened_head.value(), *last);
  // The head receipt itself can never be deleted.
  reopened.Close();
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  EXPECT_NE(sqlite3_exec(raw,
                         ("DELETE FROM active_bundle_publication_receipts "
                          "WHERE publication_generation=" +
                          std::to_string(kPublications))
                             .c_str(),
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
}

// Holds the record writer's thread inside a statement until released.
struct WriterGate final {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released{release.get_future().share()};
  sqlite3* connection{};

  WriterGate() {
    if (sqlite3_open(":memory:", &connection) != SQLITE_OK ||
        sqlite3_create_function(
            connection, "gate", 0, SQLITE_UTF8, this, &Wait, nullptr,
            nullptr) != SQLITE_OK) {
      throw std::runtime_error("cannot install writer gate");
    }
  }
  ~WriterGate() { sqlite3_close(connection); }
  WriterGate(const WriterGate&) = delete;
  WriterGate& operator=(const WriterGate&) = delete;

  void Hold(persistence::SqliteRecordWriter& writer) {
    const auto slot = writer.RegisterConnection(connection, "gate", "SELECT 1");
    persistence::SqliteRecord record;
    record.Statement("SELECT gate()");
    ASSERT_TRUE(writer.Post(slot, std::move(record)));
    entered.get_future().wait();
  }

  static void Wait(sqlite3_context* context, int, sqlite3_value**) {
    auto* gate = static_cast<WriterGate*>(sqlite3_user_data(context));
    gate->entered.set_value();
    gate->released.wait();
    sqlite3_result_int(context, 1);
  }
};

std::int64_t ReceiptRows(const std::filesystem::path& database) {
  return QueryInteger(
      database, "SELECT COUNT(*) FROM active_bundle_publication_receipts");
}

std::int64_t HeadGeneration(const std::filesystem::path& database) {
  return QueryInteger(
      database,
      "SELECT publication_generation FROM active_bundle_publication_head");
}

TEST(
    ActiveBundlePublicationStoreWrittenAfterwards,
    AnswersWhileTheWriterIsBlocked) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  persistence::SqliteRecordWriter writer;
  WriterGate gate;
  gate.Hold(writer);
  std::optional<ActiveBundlePublicationReceiptDTO> apply;
  {
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"}, &writer);
    const auto bootstrap = store.PublishActiveBundle(BootstrapRequest());
    EXPECT_EQ(store.PublishActiveBundle(BootstrapRequest()), bootstrap);
    // The compare-and-swap is decided from memory.
    EXPECT_THROW(
        store.PublishActiveBundle(ApplyRequest(9U, 3U)),
        ActiveBundlePublicationConflictError);
    apply = store.PublishActiveBundle(ApplyRequest(1U, 1U));
    EXPECT_EQ(apply->publication_generation.value(), 2U);
    const auto head =
        store.ReadActiveBundlePublication(apply->publication_generation);
    ASSERT_TRUE(head.ok());
    EXPECT_EQ(head.value(), *apply);
    const auto old =
        store.ReadActiveBundlePublication(bootstrap.publication_generation);
    ASSERT_FALSE(old.ok());
    EXPECT_EQ(old.error().code, "stale_generation");
    const auto report = store.VerifyIntegrity();
    EXPECT_EQ(report.receipt_count, 2);
    EXPECT_EQ(report.current_generation.value(), 2U);
    EXPECT_EQ(
        report.current_pointer_digest,
        apply->durable_public_pointer_content_digest.value());
    // Nothing has reached the file yet.
    EXPECT_EQ(ReceiptRows(database), 0);
    gate.release.set_value();
    writer.DrainAndStop();
    store.Close();
  }
  // Checked before the read-only queries below: a read-only connection
  // cannot remove the sidecars it opens.
  ExpectNoSqliteSidecars(database);
  EXPECT_EQ(writer.records_dropped(), 0U);
  EXPECT_EQ(writer.records_not_written(), 0U);
  EXPECT_EQ(ReceiptRows(database), 2);
  EXPECT_EQ(HeadGeneration(database), 2);
  SqliteActiveBundlePublicationStore reopened(database, CellId{"cell-a"});
  const auto reread =
      reopened.ReadActiveBundlePublication(apply->publication_generation);
  ASSERT_TRUE(reread.ok());
  EXPECT_EQ(reread.value(), *apply);
}

TEST(ActiveBundlePublicationStoreWrittenAfterwards, DroppedRecordIsCountedOnly) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  persistence::SqliteRecordWriter writer({1U, 1024U * 1024U});
  WriterGate gate;
  gate.Hold(writer);
  {
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"}, &writer);
    static_cast<void>(store.PublishActiveBundle(BootstrapRequest()));
    const auto apply = store.PublishActiveBundle(ApplyRequest(1U, 1U));
    EXPECT_EQ(writer.records_dropped(), 1U);
    // Memory is unchanged by the drop.
    EXPECT_EQ(store.VerifyIntegrity().current_generation.value(), 2U);
    const auto head =
        store.ReadActiveBundlePublication(apply.publication_generation);
    ASSERT_TRUE(head.ok());
    EXPECT_EQ(head.value(), apply);
    gate.release.set_value();
    writer.DrainAndStop();
    store.Close();
  }
  EXPECT_EQ(writer.records_not_written(), 0U);
  EXPECT_EQ(ReceiptRows(database), 1);
  EXPECT_EQ(HeadGeneration(database), 1);
  // The writer marked the file: records of this start were not written.
  EXPECT_EQ(
      QueryInteger(
          database, "SELECT records_lost FROM publication_store_metadata"),
      1);
}

TEST(
    ActiveBundlePublicationStoreWrittenAfterwards,
    FailedFileWriteIsCountedOnly) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  persistence::SqliteRecordWriter writer;
  WriterGate gate;
  gate.Hold(writer);
  {
    SqliteActiveBundlePublicationStore store(
        database, CellId{"cell-a"}, &writer);
    // A foreign row at generation 1 makes the store's own insert fail in the
    // file.
    const auto digest = std::string(kBundleDigest);
    SqliteExecPath(
        database,
        std::string(
            "INSERT INTO active_bundle_publication_receipts ("
            "publication_generation,transaction_kind,transaction_id,"
            "transaction_generation,projection_stage_generation,"
            "expected_previous_publication_generation,"
            "candidate_retained_bundle_id,candidate_graph_revision,"
            "candidate_scene_id,candidate_scene_revision_id,"
            "candidate_scene_content_hash,"
            "private_materialization_content_digest,"
            "obsolete_managed_command_surfaces_retired_evidence_digest,"
            "request_digest,durable_public_pointer_content_digest"
            ") VALUES (1,'bootstrap','bootstrap-foreign',1,0,0,'") +
            digest + "','" + digest + "','" + std::string(kSceneIdDigest) +
            "',1,'" + digest + "','" + digest + "','" + digest +
            "','sha256:"
            "1111111111111111111111111111111111111111111111111111111111111111',"
            "'sha256:"
            "2222222222222222222222222222222222222222222222222222222222222222')"
            ";");
    const auto bootstrap = store.PublishActiveBundle(BootstrapRequest());
    gate.release.set_value();
    writer.DrainAndStop();
    EXPECT_EQ(writer.records_not_written(), 1U);
    EXPECT_EQ(writer.records_dropped(), 0U);
    // The store still answers from memory.
    const auto head =
        store.ReadActiveBundlePublication(bootstrap.publication_generation);
    ASSERT_TRUE(head.ok());
    EXPECT_EQ(head.value(), bootstrap);
    store.Close();
  }
  EXPECT_EQ(HeadGeneration(database), 0);
}

TEST(ActiveBundlePublicationStoreWrittenAfterwards, FileHoldsEverythingAfterClose) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  std::optional<ActiveBundlePublicationReceiptDTO> last;
  {
    // Without a handed-in writer the store owns one and drains it at Close.
    SqliteActiveBundlePublicationStore store(database, CellId{"cell-a"});
    last = store.PublishActiveBundle(BootstrapRequest());
    for (std::uint64_t apply_id = 1U; apply_id <= 3U; ++apply_id) {
      last = store.PublishActiveBundle(ApplyRequest(apply_id, apply_id));
    }
  }
  // Checked before the read-only queries below: a read-only connection
  // cannot remove the sidecars it opens.
  ExpectNoSqliteSidecars(database);
  EXPECT_EQ(ReceiptRows(database), 4);
  EXPECT_EQ(HeadGeneration(database), 4);
  EXPECT_EQ(
      QueryInteger(
          database,
          "SELECT COUNT(*) FROM pragma_quick_check WHERE quick_check <> 'ok'"),
      0);
  SqliteActiveBundlePublicationStore reopened(database, CellId{"cell-a"});
  EXPECT_EQ(reopened.VerifyIntegrity().receipt_count, 4);
  const auto head =
      reopened.ReadActiveBundlePublication(last->publication_generation);
  ASSERT_TRUE(head.ok());
  EXPECT_EQ(head.value(), *last);
}

} // namespace
}  // namespace manyforge::component_manager
