#include "manyforge_component_manager/active_bundle_publication_store.hpp"

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
#include <string_view>
#include <vector>

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
constexpr std::string_view kRetirementDigest =
    "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
constexpr std::string_view kOtherDigest =
    "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
constexpr std::string_view kBootstrapPointerGolden =
    "sha256:0915805d1a1f92c31cc8281518b5c4732b174d2490c555b37d5342648250a00f";

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
      ContentDigest{std::string(kRetirementDigest)},
  };
}

ActiveBundlePublicationRequestDTO ApplyRequest(
    const std::string& apply_id,
    const std::uint64_t expected_generation,
    const std::uint64_t scene_revision = 2U) {
  const auto projection =
      Projection(kOtherDigest, kSceneDigest, scene_revision);
  return ActiveBundlePublicationRequestDTO{
      NamedApplyPublicationTransactionDTO{
          ApplyId{apply_id},
          ApplyGeneration{1U},
          ProjectionStageGeneration{1U}},
      projection,
      projection.retained_bundle_id,
      projection.graph_revision,
      ActiveBundlePublicationGeneration{expected_generation},
      ContentDigest{std::string(kMaterializationDigest)},
      ContentDigest{std::string(kRetirementDigest)},
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

    SqliteActiveBundlePublicationStore independent_reader(
        database, CellId{"cell-a"});
    const IActiveBundlePublicationReader& reader = independent_reader;
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

  auto stale = ApplyRequest("apply-stale", 3U);
  ASSERT_TRUE(stale.valid());
  EXPECT_THROW(
      store.PublishActiveBundle(stale),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(store.VerifyIntegrity().receipt_count, 1);

  const auto apply =
      store.PublishActiveBundle(ApplyRequest("apply-1", 1U));
  EXPECT_EQ(apply.publication_generation.value(), 2U);
  EXPECT_EQ(
      store.PublishActiveBundle(ApplyRequest("apply-1", 1U)),
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

TEST(
    ActiveBundlePublicationStore,
    IndependentWritersSerializeDuplicateBootstrapAndNextCas) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  SqliteActiveBundlePublicationStore first(database, CellId{"cell-a"});
  SqliteActiveBundlePublicationStore second(database, CellId{"cell-a"});

  auto first_write = std::async(
      std::launch::async,
      [&first] {
        return first.PublishActiveBundle(BootstrapRequest());
      });
  auto second_write = std::async(
      std::launch::async,
      [&second] {
        return second.PublishActiveBundle(BootstrapRequest());
      });
  const auto first_receipt = first_write.get();
  const auto second_receipt = second_write.get();
  EXPECT_EQ(first_receipt, second_receipt);
  EXPECT_EQ(first.VerifyIntegrity().receipt_count, 1);
  EXPECT_EQ(second.VerifyIntegrity().receipt_count, 1);

  const auto next =
      second.PublishActiveBundle(ApplyRequest("apply-1", 1U));
  EXPECT_EQ(next.publication_generation.value(), 2U);
  EXPECT_EQ(first.VerifyIntegrity().current_generation.value(), 2U);
}

TEST(
    ActiveBundlePublicationStore,
    ConcurrentFirstOpenConvergesOneExactEmptyStore) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  std::vector<std::future<ActiveBundlePublicationStoreIntegrityReport>>
      openings;
  for (std::size_t index = 0; index < 4U; ++index) {
    openings.push_back(std::async(
        std::launch::async,
        [&database] {
          SqliteActiveBundlePublicationStore store(
              database, CellId{"cell-a"});
          return store.VerifyIntegrity();
        }));
  }
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
  SqliteActiveBundlePublicationStore reopened(
      database, CellId{"cell-a"});
  EXPECT_EQ(reopened.VerifyIntegrity().receipt_count, 0);
}

TEST(
    ActiveBundlePublicationStore,
    ConcurrentDifferentRequestsAtOneExpectedGenerationHaveOneCasWinner) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  SqliteActiveBundlePublicationStore first(database, CellId{"cell-a"});
  SqliteActiveBundlePublicationStore second(database, CellId{"cell-a"});
  static_cast<void>(first.PublishActiveBundle(BootstrapRequest()));

  struct Attempt final {
    bool committed;
    std::string apply_id;
  };
  const auto publish = [](SqliteActiveBundlePublicationStore& store,
                          const std::string& apply_id) {
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
      [&] { return publish(first, "apply-a"); });
  auto second_write = std::async(
      std::launch::async,
      [&] { return publish(second, "apply-b"); });
  const auto first_attempt = first_write.get();
  const auto second_attempt = second_write.get();
  EXPECT_EQ(
      static_cast<int>(first_attempt.committed) +
          static_cast<int>(second_attempt.committed),
      1);
  const auto report = first.VerifyIntegrity();
  EXPECT_EQ(report.receipt_count, 2);
  EXPECT_EQ(report.current_generation.value(), 2U);
  const auto current = second.ReadActiveBundlePublication(
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
      first.PublishActiveBundle(ApplyRequest(loser, 1U)),
      ActiveBundlePublicationConflictError);
  EXPECT_EQ(first.VerifyIntegrity().receipt_count, 2);
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
    CorruptedHeadFailsClosedOnReopenAndFocusedRead) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "publication.sqlite3";
  SqliteActiveBundlePublicationStore store(
      database, CellId{"cell-a"});
  const auto receipt =
      store.PublishActiveBundle(BootstrapRequest());

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
  SqliteExec(
      raw,
      "UPDATE active_bundle_publication_head SET "
      "durable_public_pointer_content_digest='sha256:"
      "9999999999999999999999999999999999999999999999999999999999999999'");
  ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);

  const auto read =
      store.ReadActiveBundlePublication(receipt.publication_generation);
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error().code, "internal_error");
  EXPECT_THROW(
      store.VerifyIntegrity(),
      ActiveBundlePublicationIntegrityError);
  store.Close();
  EXPECT_THROW(
      SqliteActiveBundlePublicationStore(database, CellId{"cell-a"}),
      ActiveBundlePublicationIntegrityError);
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
      ") SELECT value,'named_apply',printf('apply-%d',value),1,1,value-1,'" +
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

}  // namespace
}  // namespace manyforge::component_manager
