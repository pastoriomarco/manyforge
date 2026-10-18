#include "manyforge_component_manager/active_bundle_publication_store.hpp"

#include "embedded_active_bundle_publication_schema.hpp"
#include "sqlite_store_startup_internal.hpp"
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

#include <openssl/evp.h>
#include <sqlite3.h>

#if SQLITE_VERSION_NUMBER < 3037000
#error "active-bundle publication storage requires SQLite 3.37.0 or newer"
#endif

#ifndef SQLITE_OPEN_NOFOLLOW
#error "active-bundle publication storage requires SQLITE_OPEN_NOFOLLOW"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::component_manager {
namespace {

using manyforge::core::ActiveBundlePublicationGeneration;
using manyforge::core::ActiveBundlePublicationReceiptDTO;
using manyforge::core::ActiveBundlePublicationRequestDTO;
using manyforge::core::BootstrapPublicationTransactionDTO;
using manyforge::core::ContentDigest;
using manyforge::core::ErrorDTO;
using manyforge::core::NamedApplyPublicationTransactionDTO;
using manyforge::core::Result;

constexpr std::string_view kDigestPrefix =
    "manyforge.active-bundle-publication-store.v1";
constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kWalCheckpointTargetBytes =
    16LL * 1024LL * 1024LL;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 8;

[[noreturn]] void ThrowSqlite(
    sqlite3* database,
    const int result,
    const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL || result == SQLITE_TOOBIG) {
    throw ActiveBundlePublicationLimitError(
        std::string(context) + ": SQLite storage ceiling exhausted: " +
        detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED ||
      result == SQLITE_CONSTRAINT) {
    throw ActiveBundlePublicationConflictError(
        std::string(context) + ": " + detail);
  }
  throw ActiveBundlePublicationStoreError(
      std::string(context) + ": " + detail);
}

void Check(
    sqlite3* database,
    const int result,
    const std::string_view context) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
    ThrowSqlite(database, result, context);
  }
}

void Exec(sqlite3* database, const std::string_view sql) {
  char* error = nullptr;
  const auto result = sqlite3_exec(
      database, std::string(sql).c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string detail =
        error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    if (result == SQLITE_FULL || result == SQLITE_TOOBIG) {
      throw ActiveBundlePublicationLimitError(detail);
    }
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED ||
        result == SQLITE_CONSTRAINT) {
      throw ActiveBundlePublicationConflictError(detail);
    }
    throw ActiveBundlePublicationStoreError(detail);
  }
}

class Statement final {
 public:
  Statement(sqlite3* database, const std::string_view sql)
      : database_(database) {
    Check(
        database_,
        sqlite3_prepare_v2(
            database_, std::string(sql).c_str(), -1, &statement_, nullptr),
        "prepare SQLite statement");
  }

  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void Bind(const int index, const std::string_view value) {
    Check(
        database_,
        sqlite3_bind_text(
            statement_,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT),
        "bind SQLite text");
  }

  void Bind(const int index, const std::int64_t value) {
    Check(
        database_,
        sqlite3_bind_int64(statement_, index, value),
        "bind SQLite integer");
  }

  [[nodiscard]] bool Step() {
    const auto result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    ThrowSqlite(database_, result, "step SQLite statement");
  }

  [[nodiscard]] std::string Text(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
      throw ActiveBundlePublicationIntegrityError(
          "stored publication text column has an invalid SQLite type");
    }
    const auto* text = sqlite3_column_text(statement_, column);
    if (text == nullptr) {
      throw ActiveBundlePublicationIntegrityError(
          "stored publication text column is null");
    }
    return {
        reinterpret_cast<const char*>(text),
        static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)),
    };
  }

  [[nodiscard]] std::int64_t Integer(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
      throw ActiveBundlePublicationIntegrityError(
          "stored publication integer column has an invalid SQLite type");
    }
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] int Changes() const noexcept {
    return sqlite3_changes(database_);
  }

 private:
  sqlite3* database_;
  sqlite3_stmt* statement_{};
};

class Transaction final {
 public:
  Transaction(sqlite3* database, const std::string_view begin)
      : database_(database) {
    Exec(database_, begin);
  }

  ~Transaction() {
    if (!finished_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void Commit() {
    Exec(database_, "COMMIT");
    finished_ = true;
  }

 private:
  sqlite3* database_;
  bool finished_{};
};

std::int64_t ScalarInteger(sqlite3* database, const std::string_view sql) {
  Statement statement(database, sql);
  if (!statement.Step()) {
    throw ActiveBundlePublicationIntegrityError(
        "SQLite scalar row is missing");
  }
  const auto result = statement.Integer(0);
  if (statement.Step()) {
    throw ActiveBundlePublicationIntegrityError(
        "SQLite scalar query returned multiple rows");
  }
  return result;
}

std::string NormalizedSql(const std::string_view value) {
  std::istringstream input{std::string(value)};
  std::string word;
  std::string result;
  while (input >> word) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += word;
  }
  return result;
}

using SchemaObjects =
    std::map<std::pair<std::string, std::string>, std::string>;

SchemaObjects ReadSchemaObjects(sqlite3* database) {
  Statement statement(
      database,
      "SELECT type,name,sql FROM sqlite_master "
      "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name LIMIT ?");
  statement.Bind(1, kExpectedSchemaObjectCardinality + 1);
  SchemaObjects objects;
  std::int64_t count = 0;
  while (statement.Step()) {
    ++count;
    if (count > kExpectedSchemaObjectCardinality) {
      throw ActiveBundlePublicationIntegrityError(
          "publication-store schema object cardinality exceeds the "
          "versioned artifact");
    }
    objects.emplace(
        std::make_pair(statement.Text(0), statement.Text(1)),
        NormalizedSql(statement.Text(2)));
  }
  return objects;
}

const SchemaObjects& ExpectedSchemaObjects() {
  static const SchemaObjects expected = [] {
    sqlite3* database = nullptr;
    const auto result = sqlite3_open_v2(
        ":memory:",
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr);
    if (result != SQLITE_OK) {
      const std::string detail =
          database == nullptr ? sqlite3_errstr(result) :
                                sqlite3_errmsg(database);
      if (database != nullptr) {
        sqlite3_close(database);
      }
      throw ActiveBundlePublicationStoreError(
          "open in-memory publication schema validator: " + detail);
    }
    try {
      Exec(database, detail::kEmbeddedActiveBundlePublicationSchema);
      auto objects = ReadSchemaObjects(database);
      if (objects.size() !=
          static_cast<std::size_t>(kExpectedSchemaObjectCardinality)) {
        throw ActiveBundlePublicationStoreError(
            "embedded publication schema object cardinality is invalid");
      }
      sqlite3_close(database);
      return objects;
    } catch (...) {
      sqlite3_close(database);
      throw;
    }
  }();
  return expected;
}

void ValidateBoundedIdentity(
    const std::string_view value,
    const std::string_view label) {
  if (value.empty() ||
      value.size() > kMaximumActiveBundlePublicationIdentifierBytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(
        std::string(label) +
        " must be a bounded non-empty printable ASCII identity");
  }
}

bool IsSha256Digest(const std::string_view value) {
  return value.size() == 71U && value.starts_with("sha256:") &&
      std::all_of(
             value.begin() + 7,
             value.end(),
             [](const char character) {
               return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
             });
}

void ValidateDigest(
    const std::string_view value,
    const std::string_view label) {
  if (!IsSha256Digest(value)) {
    throw std::invalid_argument(
        std::string(label) + " must be a lowercase sha256 digest");
  }
}

std::int64_t CheckedSigned(
    const std::uint64_t value,
    const std::string_view label) {
  if (value == 0U ||
      value >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument(
        std::string(label) +
        " must fit the positive signed SQLite int64 range");
  }
  return static_cast<std::int64_t>(value);
}

std::string Hex(const std::span<const unsigned char> bytes) {
  constexpr std::array<char, 16> kHex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0x0FU]);
  }
  return result;
}

std::string RawDigest(const std::span<const std::byte> bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned int output_length = 0;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    throw ActiveBundlePublicationIntegrityError(
        "cannot allocate SHA-256 context");
  }
  const auto release = [&]() { EVP_MD_CTX_free(context); };
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &output_length) != 1) {
    release();
    throw ActiveBundlePublicationIntegrityError(
        "SHA-256 computation failed");
  }
  release();
  return "sha256:" +
      Hex(std::span<const unsigned char>(output.data(), output_length));
}

std::string RawDigest(const std::string_view value) {
  return RawDigest(
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

std::string JsonString(const std::string_view value) {
  std::string result{"\""};
  result.reserve(value.size() + 2U);
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      result.push_back('\\');
    }
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

using JsonField = std::pair<std::string, std::string>;

std::string JsonObject(std::vector<JsonField> fields) {
  std::sort(
      fields.begin(),
      fields.end(),
      [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
      });
  std::string result{"{"};
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0U) {
      result.push_back(',');
    }
    result += JsonString(fields[index].first);
    result.push_back(':');
    result += fields[index].second;
  }
  result.push_back('}');
  return result;
}

JsonField StringField(std::string name, const std::string_view value) {
  return {std::move(name), JsonString(value)};
}

JsonField IntegerField(std::string name, const std::int64_t value) {
  return {std::move(name), std::to_string(value)};
}

std::string SemanticDigest(
    const std::string_view domain,
    const std::string& canonical_payload) {
  std::string preimage;
  preimage.reserve(
      kDigestPrefix.size() + domain.size() + canonical_payload.size() + 2U);
  preimage.append(kDigestPrefix);
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(canonical_payload);
  return RawDigest(preimage);
}

enum class TransactionKind {
  kBootstrap,
  kNamedApply,
};

struct RequestEncoding final {
  TransactionKind kind;
  std::string transaction_id;
  std::int64_t transaction_generation;
  std::int64_t projection_stage_generation;
  std::int64_t expected_previous_generation;
  std::int64_t scene_revision_id;
};

std::string_view ToString(const TransactionKind kind) {
  switch (kind) {
    case TransactionKind::kBootstrap:
      return "bootstrap";
    case TransactionKind::kNamedApply:
      return "named_apply";
  }
  throw std::invalid_argument("unknown publication transaction kind");
}

TransactionKind ParseTransactionKind(const std::string_view value) {
  if (value == "bootstrap") {
    return TransactionKind::kBootstrap;
  }
  if (value == "named_apply") {
    return TransactionKind::kNamedApply;
  }
  throw ActiveBundlePublicationIntegrityError(
      "stored publication transaction kind is invalid");
}

RequestEncoding ValidateAndEncodeRequest(
    const ActiveBundlePublicationRequestDTO& request) {
  if (!request.valid()) {
    throw std::invalid_argument(
        "active-bundle publication request is structurally invalid");
  }
  ValidateDigest(
      request.candidate_retained_bundle_id.value(),
      "candidate_retained_bundle_id");
  ValidateDigest(
      request.candidate_graph_revision.value(),
      "candidate_graph_revision");
  ValidateBoundedIdentity(
      request.candidate_projection_revision.scene_revision.scene_id.value(),
      "candidate_scene_id");
  ValidateDigest(
      request.candidate_projection_revision.scene_revision.content_hash.value(),
      "candidate_scene_content_hash");
  ValidateDigest(
      request.private_materialization_content_digest.value(),
      "private_materialization_content_digest");
  RequestEncoding encoded{
      TransactionKind::kBootstrap,
      {},
      0,
      0,
      0,
      CheckedSigned(
          request.candidate_projection_revision.scene_revision.revision_id,
          "candidate_scene_revision_id"),
  };
  std::visit(
      [&encoded](const auto& transaction) {
        using Transaction = std::decay_t<decltype(transaction)>;
        if constexpr (
            std::is_same_v<Transaction,
                           NamedApplyPublicationTransactionDTO>) {
          encoded.kind = TransactionKind::kNamedApply;
          encoded.transaction_id = transaction.apply_id.value();
          encoded.transaction_generation = CheckedSigned(
              transaction.apply_generation.value(), "apply_generation");
          // Version-1 storage used a per-Stage coordinate. Idle maintenance
          // has one initialization, so the compatibility coordinate is fixed.
          encoded.projection_stage_generation = 1;
        } else {
          encoded.kind = TransactionKind::kBootstrap;
          encoded.transaction_id = transaction.bootstrap_id.value();
          encoded.transaction_generation = CheckedSigned(
              transaction.bootstrap_generation.value(),
              "bootstrap_generation");
        }
      },
      request.transaction_ref);
  ValidateBoundedIdentity(encoded.transaction_id, "transaction_id");
  if (request.expected_previous_publication_generation.has_value()) {
    encoded.expected_previous_generation = CheckedSigned(
        request.expected_previous_publication_generation->value(),
        "expected_previous_publication_generation");
  }
  return encoded;
}

std::string PublicationRequestDigest(
    const core::CellId& cell_id,
    const ActiveBundlePublicationRequestDTO& request,
    const RequestEncoding& encoded,
    const ContentDigest& compatibility_retirement_digest) {
  ValidateDigest(compatibility_retirement_digest.value(),
                 "stored compatibility retirement digest");
  return SemanticDigest(
      "publication-request",
      JsonObject({
          StringField(
              "candidate_graph_revision",
              request.candidate_graph_revision.value()),
          StringField(
              "candidate_retained_bundle_id",
              request.candidate_retained_bundle_id.value()),
          StringField(
              "candidate_scene_content_hash",
              request.candidate_projection_revision.scene_revision
                  .content_hash.value()),
          StringField(
              "candidate_scene_id",
              request.candidate_projection_revision.scene_revision.scene_id
                  .value()),
          IntegerField(
              "candidate_scene_revision_id", encoded.scene_revision_id),
          StringField("cell_id", cell_id.value()),
          IntegerField(
              "expected_previous_publication_generation",
              encoded.expected_previous_generation),
          StringField(
              "obsolete_managed_command_surfaces_retired_evidence_digest",
              compatibility_retirement_digest.value()),
          StringField(
              "private_materialization_content_digest",
              request.private_materialization_content_digest.value()),
          IntegerField(
              "projection_stage_generation",
              encoded.projection_stage_generation),
          IntegerField(
              "transaction_generation", encoded.transaction_generation),
          StringField("transaction_id", encoded.transaction_id),
          StringField("transaction_kind", ToString(encoded.kind)),
      }));
}

std::string PublicPointerDigest(
    const core::CellId& cell_id,
    const std::int64_t publication_generation,
    const std::string_view request_digest) {
  return SemanticDigest(
      "public-pointer",
      JsonObject({
          StringField("cell_id", cell_id.value()),
          IntegerField("publication_generation", publication_generation),
          StringField("request_digest", request_digest),
      }));
}

std::filesystem::path ValidateDatabasePath(std::filesystem::path path) {
  if (path.empty() || path == ":memory:") {
    throw std::invalid_argument(
        "a durable filesystem publication-store path is required");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error &&
      error != std::make_error_code(std::errc::no_such_file_or_directory)) {
    throw std::invalid_argument(
        "cannot inspect publication-store database path");
  }
  if (!error && std::filesystem::is_symlink(status)) {
    throw std::invalid_argument(
        "publication-store database path cannot be a symlink");
  }
  if (!error && std::filesystem::exists(status) &&
      !std::filesystem::is_regular_file(status)) {
    throw std::invalid_argument(
        "publication-store database path must be a regular file");
  }
  if (!error && std::filesystem::exists(status)) {
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
      throw std::invalid_argument(
          "cannot inspect publication-store database size");
    }
    if (size >
        static_cast<std::uintmax_t>(
            kMaximumActiveBundlePublicationDatabasePageImageBytes)) {
      throw ActiveBundlePublicationLimitError(
          "publication-store database file exceeds its ceiling");
    }
  }
  error.clear();
  const auto parent =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  if (!std::filesystem::exists(parent, error) ||
      !std::filesystem::is_directory(parent, error)) {
    throw std::invalid_argument(
        "publication-store database parent must exist");
  }
  return path;
}

void ValidatePublicationStoreMetadata(
    sqlite3* database,
    const core::CellId& cell_id) {
  Statement row(database, "SELECT * FROM publication_store_metadata");
  if (!row.Step() || row.Integer(0) != 1 ||
      row.Text(1) != kActiveBundlePublicationStoreSchemaId ||
      row.Integer(2) != kActiveBundlePublicationStoreSchemaVersion ||
      row.Text(3) !=
          RawDigest(detail::kEmbeddedActiveBundlePublicationSchema)) {
    throw ActiveBundlePublicationIntegrityError(
        "publication-store metadata mismatch");
  }
  if (row.Text(4) != cell_id.value()) {
    throw ActiveBundlePublicationConflictError(
        "active-bundle publication store belongs to a different cell");
  }
  if (row.Integer(5) != kMaximumActiveBundlePublicationReceipts ||
      row.Integer(6) !=
          kMaximumActiveBundlePublicationDatabasePageImageBytes) {
    throw ActiveBundlePublicationIntegrityError(
        "publication-store resource-limit metadata mismatch");
  }
  if (row.Step()) {
    throw ActiveBundlePublicationIntegrityError(
        "publication-store metadata singleton is invalid");
  }
}

void ValidatePublicationStoreIdentityAndSchema(
    sqlite3* database,
    const core::CellId& cell_id) {
  const auto existing = ReadSchemaObjects(database);
  const auto application_id =
      ScalarInteger(database, "PRAGMA application_id");
  const auto user_version =
      ScalarInteger(database, "PRAGMA user_version");
  if (existing.empty()) {
    if (application_id != 0 || user_version != 0) {
      throw ActiveBundlePublicationIntegrityError(
          "schema-empty publication-store database has foreign SQLite "
          "identity");
    }
    return;
  }
  if (application_id != kActiveBundlePublicationStoreSqliteApplicationId) {
    throw ActiveBundlePublicationIntegrityError(
        "publication-store SQLite application ID mismatch");
  }
  if (user_version != kActiveBundlePublicationStoreSchemaVersion) {
    throw ActiveBundlePublicationIntegrityError(
        "publication-store SQLite user version mismatch");
  }
  if (existing != ExpectedSchemaObjects()) {
    throw ActiveBundlePublicationIntegrityError(
        "publication-store schema objects differ from the versioned "
        "artifact");
  }
  ValidatePublicationStoreMetadata(database, cell_id);
}

void PreflightPublicationStore(
    const std::filesystem::path& database_path,
    const core::CellId& cell_id) {
  try {
    static_cast<void>(persistence::PreflightExistingSqliteReadOnly(
        database_path,
        {
            static_cast<std::uintmax_t>(
                kMaximumActiveBundlePublicationDatabasePageImageBytes),
            static_cast<std::uintmax_t>(
                kMaximumActiveBundlePublicationDatabasePageImageBytes),
        },
        [&cell_id](sqlite3* database) {
          ValidatePublicationStoreIdentityAndSchema(database, cell_id);
        }));
  } catch (const persistence::SqliteReadOnlyPreflightError& error) {
    throw ActiveBundlePublicationIntegrityError(
        "publication-store read-only preflight failed: " +
        std::string(error.what()));
  }
}

struct StoredReceipt final {
  ActiveBundlePublicationReceiptDTO receipt;
  std::string request_digest;
};

}  // namespace

class SqliteActiveBundlePublicationStore::Impl final {
 public:
  Impl(std::filesystem::path database_path, core::CellId cell_id)
      : database_path_(ValidateDatabasePath(std::move(database_path))),
        cell_id_(std::move(cell_id)) {
    ValidateBoundedIdentity(cell_id_.value(), "cell_id");
    if (sqlite3_libversion_number() < 3037000) {
      throw ActiveBundlePublicationStoreError(
          "active-bundle publication storage requires SQLite 3.37.0 or "
          "newer");
    }
    auto startup_lock =
        sqlite_startup_internal::TryAcquireManagerSqliteStoreStartupLock(
            std::chrono::milliseconds(kBusyTimeoutMilliseconds));
    if (!startup_lock.owns_lock()) {
      throw ActiveBundlePublicationConflictError(
          "timed out waiting for process-local publication-store startup");
    }
    PreflightPublicationStore(database_path_, cell_id_);
    const int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_NOFOLLOW;
    const auto result = sqlite3_open_v2(
        database_path_.string().c_str(), &database_, flags, nullptr);
    if (result != SQLITE_OK) {
      const std::string detail =
          database_ == nullptr ? sqlite3_errstr(result) :
                                 sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      throw ActiveBundlePublicationStoreError(
          "cannot open active-bundle publication store: " + detail);
    }
    try {
      Check(
          database_,
          sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure SQLite busy timeout");
      Configure();
      // Read schema and SQLite identity from one serialized WAL snapshot.  A
      // non-transactional first-open check can otherwise interleave those
      // reads with a peer's schema commit and misclassify that legitimate
      // initializer as a foreign database.
      Transaction validation(database_, "BEGIN EXCLUSIVE");
      ValidatePublicationStoreIdentityAndSchema(database_, cell_id_);
      validation.Commit();
      InitializeOrValidateSchema();
      Transaction transaction(database_, "BEGIN");
      static_cast<void>(VerifyIntegrityLocked());
      transaction.Commit();
    } catch (...) {
      sqlite3_close(database_);
      database_ = nullptr;
      throw;
    }
  }

  // Non-owning read-only view, matching the Apply journal's existing reader.
  Impl(sqlite3 *borrowed_database, const core::CellId &cell_id)
      : cell_id_(cell_id), database_(borrowed_database), borrowed_(true) {}

  ~Impl() {
    if (borrowed_)
      return;
    try {
      Close();
    } catch (...) {
    }
  }

  void RequireOpen() const {
    if (closed_ || database_ == nullptr) {
      throw ActiveBundlePublicationStoreClosedError(
          "active-bundle publication store is closed");
    }
  }

  void Close() {
    auto startup_lock =
        sqlite_startup_internal::
            AcquireManagerSqliteStoreStartupLockForClose();
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    sqlite3_wal_checkpoint_v2(
        database_,
        nullptr,
        SQLITE_CHECKPOINT_TRUNCATE,
        nullptr,
        nullptr);
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      ThrowSqlite(
          database_, result, "close active-bundle publication store");
    }
    database_ = nullptr;
    closed_ = true;
  }

  void Configure() {
    Exec(database_, "PRAGMA foreign_keys = ON");
    if (ScalarInteger(database_, "PRAGMA foreign_keys") != 1) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite foreign-key enforcement is required");
    }
    Check(
        database_,
        sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
        "configure SQLite busy timeout");
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kBusyTimeoutMilliseconds);
    while (true) {
      try {
        Statement journal_mode(database_, "PRAGMA journal_mode = WAL");
        if (!journal_mode.Step() || journal_mode.Text(0) != "wal") {
          throw ActiveBundlePublicationIntegrityError(
              "SQLite WAL journal mode is required");
        }
        break;
      } catch (const ActiveBundlePublicationConflictError&) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    Exec(database_, "PRAGMA synchronous = FULL");
    if (ScalarInteger(database_, "PRAGMA synchronous") != 2) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite FULL synchronous mode is required");
    }
    Exec(database_, "PRAGMA trusted_schema = OFF");
    if (ScalarInteger(database_, "PRAGMA trusted_schema") != 0) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite untrusted-schema mode is required");
    }
    Exec(database_, "PRAGMA recursive_triggers = OFF");
    if (ScalarInteger(database_, "PRAGMA recursive_triggers") != 0) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite recursive triggers must remain disabled");
    }
    Exec(database_, "PRAGMA wal_autocheckpoint = 1000");
    if (ScalarInteger(database_, "PRAGMA wal_autocheckpoint") != 1000) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite WAL autocheckpoint policy is required");
    }
    const auto journal_size_limit = ScalarInteger(
        database_,
        "PRAGMA journal_size_limit = " +
            std::to_string(kWalCheckpointTargetBytes));
    if (journal_size_limit != kWalCheckpointTargetBytes) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite WAL journal-size policy is required");
    }
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_size <= 0) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite page size is invalid");
    }
    const auto maximum_pages = std::max<std::int64_t>(
        1,
        kMaximumActiveBundlePublicationDatabasePageImageBytes / page_size);
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    if (page_count < 0 || page_count > maximum_pages) {
      throw ActiveBundlePublicationLimitError(
          "SQLite publication database page image exceeds its ceiling");
    }
    const auto effective_maximum_pages = ScalarInteger(
        database_,
        "PRAGMA max_page_count = " + std::to_string(maximum_pages));
    if (effective_maximum_pages != maximum_pages) {
      throw ActiveBundlePublicationLimitError(
          "SQLite publication database page ceiling could not be enforced");
    }
    sqlite3_limit(
        database_,
        SQLITE_LIMIT_LENGTH,
        static_cast<int>(
            16U * kMaximumActiveBundlePublicationIdentifierBytes +
            64U * 1024U));
  }

  void InitializeOrValidateSchema() {
    std::lock_guard lock(mutex_);
    Transaction transaction(database_, "BEGIN EXCLUSIVE");
    const auto existing = ReadSchemaObjects(database_);
    if (existing.empty()) {
      if (ScalarInteger(database_, "PRAGMA application_id") != 0 ||
          ScalarInteger(database_, "PRAGMA user_version") != 0) {
        throw ActiveBundlePublicationIntegrityError(
            "schema-empty publication-store database has foreign SQLite "
            "identity");
      }
      Exec(database_, detail::kEmbeddedActiveBundlePublicationSchema);
      Statement metadata(
          database_,
          "INSERT INTO publication_store_metadata ("
          "singleton,schema_id,schema_version,schema_artifact_digest,cell_id,"
          "maximum_receipts,maximum_main_database_page_image_bytes"
          ") VALUES (1,?,?,?,?,?,?)");
      metadata.Bind(1, kActiveBundlePublicationStoreSchemaId);
      metadata.Bind(2, kActiveBundlePublicationStoreSchemaVersion);
      metadata.Bind(
          3, RawDigest(detail::kEmbeddedActiveBundlePublicationSchema));
      metadata.Bind(4, cell_id_.value());
      metadata.Bind(5, kMaximumActiveBundlePublicationReceipts);
      metadata.Bind(
          6, kMaximumActiveBundlePublicationDatabasePageImageBytes);
      static_cast<void>(metadata.Step());
      Exec(
          database_,
          "INSERT INTO active_bundle_publication_head ("
          "singleton,publication_generation,request_digest,"
          "durable_public_pointer_content_digest"
          ") VALUES (1,0,'','')");
    } else if (
        !existing.contains({"table", "publication_store_metadata"})) {
      throw ActiveBundlePublicationIntegrityError(
          "non-empty database lacks publication-store metadata");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw ActiveBundlePublicationIntegrityError(
          "publication-store schema objects differ from the versioned "
          "artifact");
    }
    transaction.Commit();
  }

  void ValidateMetadataLocked() const {
    if (ScalarInteger(database_, "PRAGMA application_id") !=
        kActiveBundlePublicationStoreSqliteApplicationId) {
      throw ActiveBundlePublicationIntegrityError(
          "publication-store SQLite application ID mismatch");
    }
    if (ScalarInteger(database_, "PRAGMA user_version") !=
        kActiveBundlePublicationStoreSchemaVersion) {
      throw ActiveBundlePublicationIntegrityError(
          "publication-store SQLite user version mismatch");
    }
    ValidatePublicationStoreMetadata(database_, cell_id_);
  }

  [[nodiscard]] StoredReceipt ReceiptFromRow(Statement& row) const {
    const auto publication_generation = row.Integer(0);
    const auto transaction_kind = ParseTransactionKind(row.Text(1));
    const auto transaction_id = row.Text(2);
    const auto transaction_generation = row.Integer(3);
    const auto projection_stage_generation = row.Integer(4);
    const auto expected_previous_generation = row.Integer(5);
    const auto candidate_retained_bundle_id = row.Text(6);
    const auto candidate_graph_revision = row.Text(7);
    const auto candidate_scene_id = row.Text(8);
    const auto candidate_scene_revision_id = row.Integer(9);
    const auto candidate_scene_content_hash = row.Text(10);
    const auto private_materialization_digest = row.Text(11);
    const auto compatibility_retirement_digest = row.Text(12);
    const auto request_digest = row.Text(13);
    const auto public_pointer_digest = row.Text(14);
    if (publication_generation <= 0 || transaction_generation <= 0 ||
        candidate_scene_revision_id <= 0 ||
        expected_previous_generation < 0 || projection_stage_generation < 0) {
      throw ActiveBundlePublicationIntegrityError(
          "stored publication contains an invalid generation");
    }

    core::ActiveBundlePublicationTransactionRefDTO transaction_ref =
        transaction_kind == TransactionKind::kBootstrap
        ? core::ActiveBundlePublicationTransactionRefDTO{
              BootstrapPublicationTransactionDTO{
                  core::BootstrapId{transaction_id},
                  core::BootstrapGeneration{
                      static_cast<std::uint64_t>(transaction_generation)}}}
        : core::ActiveBundlePublicationTransactionRefDTO{
              NamedApplyPublicationTransactionDTO{
                  core::ApplyId{transaction_id},
                  core::ApplyGeneration{
                      static_cast<std::uint64_t>(transaction_generation)}}};
    std::optional<ActiveBundlePublicationGeneration> expected_previous;
    if (expected_previous_generation != 0) {
      expected_previous = ActiveBundlePublicationGeneration{
          static_cast<std::uint64_t>(expected_previous_generation)};
    }
    const ActiveBundlePublicationRequestDTO request{
        std::move(transaction_ref),
        core::ProjectionRevisionDTO{
            core::RetainedBundleId{candidate_retained_bundle_id},
            core::GraphRevision{candidate_graph_revision},
            core::SceneRevision{
                core::SceneId{candidate_scene_id},
                static_cast<std::uint64_t>(candidate_scene_revision_id),
                ContentDigest{candidate_scene_content_hash}}},
        core::RetainedBundleId{candidate_retained_bundle_id},
        core::GraphRevision{candidate_graph_revision},
        expected_previous,
        ContentDigest{private_materialization_digest},
    };
    RequestEncoding encoded;
    try {
      encoded = ValidateAndEncodeRequest(request);
    } catch (const std::invalid_argument& error) {
      throw ActiveBundlePublicationIntegrityError(
          std::string("stored publication request is invalid: ") +
          error.what());
    }
    if ((encoded.kind == TransactionKind::kBootstrap &&
         projection_stage_generation != 0) ||
        (encoded.kind == TransactionKind::kNamedApply &&
         projection_stage_generation <= 0)) {
      throw ActiveBundlePublicationIntegrityError(
          "stored compatibility stage coordinate is invalid");
    }
    encoded.projection_stage_generation = projection_stage_generation;
    const auto expected_request_digest =
        PublicationRequestDigest(cell_id_, request, encoded,
                                 ContentDigest{compatibility_retirement_digest});
    const auto expected_pointer_digest = PublicPointerDigest(
        cell_id_, publication_generation, expected_request_digest);
    if (request_digest != expected_request_digest ||
        public_pointer_digest != expected_pointer_digest) {
      throw ActiveBundlePublicationIntegrityError(
          "stored active-bundle publication digest is inconsistent");
    }
    return StoredReceipt{
        ActiveBundlePublicationReceiptDTO{
            request,
            ActiveBundlePublicationGeneration{
                static_cast<std::uint64_t>(publication_generation)},
            ContentDigest{public_pointer_digest}},
        request_digest,
    };
  }

  [[nodiscard]] std::optional<StoredReceipt> ReceiptByGenerationLocked(
      const std::int64_t publication_generation) const {
    Statement row(
        database_,
        "SELECT publication_generation,transaction_kind,transaction_id,"
        "transaction_generation,projection_stage_generation,"
        "expected_previous_publication_generation,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash,private_materialization_content_digest,"
        "obsolete_managed_command_surfaces_retired_evidence_digest,"
        "request_digest,durable_public_pointer_content_digest "
        "FROM active_bundle_publication_receipts "
        "WHERE publication_generation=?");
    row.Bind(1, publication_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    auto result = ReceiptFromRow(row);
    if (row.Step()) {
      throw ActiveBundlePublicationIntegrityError(
          "publication generation is not unique");
    }
    return result;
  }

  [[nodiscard]] std::optional<StoredReceipt> ReceiptByTransactionLocked(
      const RequestEncoding& encoded) const {
    Statement row(
        database_,
        "SELECT publication_generation,transaction_kind,transaction_id,"
        "transaction_generation,projection_stage_generation,"
        "expected_previous_publication_generation,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash,private_materialization_content_digest,"
        "obsolete_managed_command_surfaces_retired_evidence_digest,"
        "request_digest,durable_public_pointer_content_digest "
        "FROM active_bundle_publication_receipts "
        "WHERE transaction_kind=? AND transaction_id=? "
        "AND transaction_generation=?");
    row.Bind(1, ToString(encoded.kind));
    row.Bind(2, encoded.transaction_id);
    row.Bind(3, encoded.transaction_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    auto result = ReceiptFromRow(row);
    if (row.Step()) {
      throw ActiveBundlePublicationIntegrityError(
          "publication transaction identity is not unique");
    }
    return result;
  }

  struct Head final {
    std::int64_t generation;
    std::string request_digest;
    std::string pointer_digest;
  };

  [[nodiscard]] Head HeadLocked() const {
    Statement row(
        database_,
        "SELECT singleton,publication_generation,request_digest,"
        "durable_public_pointer_content_digest "
        "FROM active_bundle_publication_head");
    if (!row.Step() || row.Integer(0) != 1) {
      throw ActiveBundlePublicationIntegrityError(
          "active-bundle publication head singleton is invalid");
    }
    const Head result{row.Integer(1), row.Text(2), row.Text(3)};
    if (row.Step()) {
      throw ActiveBundlePublicationIntegrityError(
          "active-bundle publication head singleton is invalid");
    }
    return result;
  }

  [[nodiscard]] ActiveBundlePublicationStoreIntegrityReport
  VerifyIntegrityLocked() const {
    Statement integrity(database_, "PRAGMA integrity_check");
    if (!integrity.Step() || integrity.Text(0) != "ok" ||
        integrity.Step()) {
      throw ActiveBundlePublicationIntegrityError(
          "publication-store SQLite integrity_check failed");
    }
    Statement foreign_keys(database_, "PRAGMA foreign_key_check");
    if (foreign_keys.Step()) {
      throw ActiveBundlePublicationIntegrityError(
          "publication-store SQLite foreign-key integrity check failed");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw ActiveBundlePublicationIntegrityError(
          "publication-store schema objects differ from the versioned "
          "artifact");
    }
    const auto count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM active_bundle_publication_receipts");
    if (count < 0 || count > kMaximumActiveBundlePublicationReceipts) {
      throw ActiveBundlePublicationIntegrityError(
          "publication receipt cardinality exceeds the fixed limit");
    }

    Statement rows(
        database_,
        "SELECT publication_generation,transaction_kind,transaction_id,"
        "transaction_generation,projection_stage_generation,"
        "expected_previous_publication_generation,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash,private_materialization_content_digest,"
        "obsolete_managed_command_surfaces_retired_evidence_digest,"
        "request_digest,durable_public_pointer_content_digest "
        "FROM active_bundle_publication_receipts "
        "ORDER BY publication_generation LIMIT ?");
    rows.Bind(1, kMaximumActiveBundlePublicationReceipts + 1);
    std::optional<StoredReceipt> last;
    std::int64_t observed_count = 0;
    while (rows.Step()) {
      ++observed_count;
      if (observed_count > kMaximumActiveBundlePublicationReceipts) {
        throw ActiveBundlePublicationIntegrityError(
            "publication receipt cardinality exceeds the fixed limit");
      }
      auto receipt = ReceiptFromRow(rows);
      const auto generation = static_cast<std::int64_t>(
          receipt.receipt.publication_generation.value());
      const bool bootstrap =
          std::holds_alternative<BootstrapPublicationTransactionDTO>(
              receipt.receipt.request.transaction_ref);
      if (generation != observed_count ||
          bootstrap != (observed_count == 1) ||
          receipt.receipt.request
                  .expected_previous_publication_generation.has_value() !=
              (observed_count != 1) ||
          (observed_count != 1 &&
           receipt.receipt.request
                   .expected_previous_publication_generation->value() !=
               static_cast<std::uint64_t>(observed_count - 1))) {
        throw ActiveBundlePublicationIntegrityError(
            "publication receipt chain is not contiguous");
      }
      last = std::move(receipt);
    }
    if (observed_count != count) {
      throw ActiveBundlePublicationIntegrityError(
          "publication receipt inventory cardinality is inconsistent");
    }

    const auto head = HeadLocked();
    if (!last.has_value()) {
      if (head.generation != 0 || !head.request_digest.empty() ||
          !head.pointer_digest.empty()) {
        throw ActiveBundlePublicationIntegrityError(
            "empty publication history has a non-empty head");
      }
    } else if (
        head.generation !=
            static_cast<std::int64_t>(
                last->receipt.publication_generation.value()) ||
        head.request_digest != last->request_digest ||
        head.pointer_digest !=
            last->receipt.durable_public_pointer_content_digest.value()) {
      throw ActiveBundlePublicationIntegrityError(
          "active-bundle publication head does not match the last receipt");
    }

    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    if (page_size <= 0 || page_count < 0 ||
        page_count >
            kMaximumActiveBundlePublicationDatabasePageImageBytes /
                page_size) {
      throw ActiveBundlePublicationIntegrityError(
          "SQLite publication database page image exceeds its ceiling");
    }
    return ActiveBundlePublicationStoreIntegrityReport{
        cell_id_,
        std::string(kActiveBundlePublicationStoreSchemaId),
        kActiveBundlePublicationStoreSchemaVersion,
        RawDigest(detail::kEmbeddedActiveBundlePublicationSchema),
        count,
        ActiveBundlePublicationGeneration{
            static_cast<std::uint64_t>(head.generation)},
        head.pointer_digest,
    };
  }

  [[nodiscard]] ActiveBundlePublicationReceiptDTO Publish(
      const ActiveBundlePublicationRequestDTO& request) {
    const auto encoded = ValidateAndEncodeRequest(request);
    // Version-1 persisted receipts contain a retired-surface digest. New idle
    // maintenance has no retirement phase, so the already-authoritative
    // materialization digest is repeated in that compatibility-only column.
    const auto compatibility_retirement_digest =
        request.private_materialization_content_digest;
    const auto request_digest =
        PublicationRequestDigest(
            cell_id_, request, encoded, compatibility_retirement_digest);
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction transaction(database_, "BEGIN IMMEDIATE");
    const auto integrity = VerifyIntegrityLocked();

    if (const auto existing = ReceiptByTransactionLocked(encoded);
        existing.has_value()) {
      if (existing->receipt.request != request) {
        throw ActiveBundlePublicationConflictError(
            "publication transaction identity conflicts with its durable "
            "request");
      }
      transaction.Commit();
      return existing->receipt;
    }
    if (integrity.receipt_count >=
        kMaximumActiveBundlePublicationReceipts) {
      throw ActiveBundlePublicationLimitError(
          "active-bundle publication receipt limit is exhausted");
    }
    const auto current_generation =
        static_cast<std::int64_t>(integrity.current_generation.value());
    if (encoded.expected_previous_generation != current_generation) {
      throw ActiveBundlePublicationConflictError(
          "active-bundle publication compare-and-swap generation "
          "conflict");
    }
    if ((current_generation == 0) !=
        (encoded.kind == TransactionKind::kBootstrap)) {
      throw ActiveBundlePublicationConflictError(
          "canonical-empty bootstrap must be the first publication");
    }
    if (current_generation ==
        std::numeric_limits<std::int64_t>::max()) {
      throw ActiveBundlePublicationLimitError(
          "active-bundle publication generation is exhausted");
    }
    const auto publication_generation = current_generation + 1;
    const auto pointer_digest = PublicPointerDigest(
        cell_id_, publication_generation, request_digest);

    Statement insert(
        database_,
        "INSERT INTO active_bundle_publication_receipts ("
        "publication_generation,transaction_kind,transaction_id,"
        "transaction_generation,projection_stage_generation,"
        "expected_previous_publication_generation,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash,private_materialization_content_digest,"
        "obsolete_managed_command_surfaces_retired_evidence_digest,"
        "request_digest,durable_public_pointer_content_digest"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    insert.Bind(1, publication_generation);
    insert.Bind(2, ToString(encoded.kind));
    insert.Bind(3, encoded.transaction_id);
    insert.Bind(4, encoded.transaction_generation);
    insert.Bind(5, encoded.projection_stage_generation);
    insert.Bind(6, encoded.expected_previous_generation);
    insert.Bind(7, request.candidate_retained_bundle_id.value());
    insert.Bind(8, request.candidate_graph_revision.value());
    insert.Bind(
        9,
        request.candidate_projection_revision.scene_revision.scene_id
            .value());
    insert.Bind(10, encoded.scene_revision_id);
    insert.Bind(
        11,
        request.candidate_projection_revision.scene_revision.content_hash
            .value());
    insert.Bind(
        12, request.private_materialization_content_digest.value());
    insert.Bind(13, compatibility_retirement_digest.value());
    insert.Bind(14, request_digest);
    insert.Bind(15, pointer_digest);
    static_cast<void>(insert.Step());

    Statement update(
        database_,
        "UPDATE active_bundle_publication_head SET "
        "publication_generation=?,request_digest=?,"
        "durable_public_pointer_content_digest=? "
        "WHERE singleton=1 AND publication_generation=?");
    update.Bind(1, publication_generation);
    update.Bind(2, request_digest);
    update.Bind(3, pointer_digest);
    update.Bind(4, current_generation);
    static_cast<void>(update.Step());
    if (update.Changes() != 1) {
      throw ActiveBundlePublicationConflictError(
          "active-bundle publication head compare-and-swap failed");
    }

    const ActiveBundlePublicationReceiptDTO receipt{
        request,
        ActiveBundlePublicationGeneration{
            static_cast<std::uint64_t>(publication_generation)},
        ContentDigest{pointer_digest},
    };
    if (!receipt.valid()) {
      throw ActiveBundlePublicationIntegrityError(
          "publication store produced an invalid receipt");
    }
    transaction.Commit();
    return receipt;
  }

  [[nodiscard]] Result<ActiveBundlePublicationReceiptDTO> Read(
      const ActiveBundlePublicationGeneration generation) const {
    if (!generation.nonzero() ||
        generation.value() >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "invalid_request",
          "publication generation must fit the positive durable range",
          {{"publication_generation", std::to_string(generation.value())}},
      });
    }
    try {
      std::lock_guard lock(mutex_);
      RequireOpen();
      Transaction transaction(database_, "BEGIN");
      ValidateMetadataLocked();
      if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
        throw ActiveBundlePublicationIntegrityError(
            "publication-store schema objects differ from the versioned "
            "artifact");
      }
      const auto head = HeadLocked();
      const auto receipt_count = ScalarInteger(
          database_,
          "SELECT COUNT(*) FROM active_bundle_publication_receipts");
      if (receipt_count < 0 ||
          receipt_count > kMaximumActiveBundlePublicationReceipts ||
          receipt_count != head.generation) {
        throw ActiveBundlePublicationIntegrityError(
            "active-bundle publication head and receipt cardinality "
            "disagree");
      }
      std::optional<StoredReceipt> current_receipt;
      if (head.generation != 0) {
        current_receipt = ReceiptByGenerationLocked(head.generation);
        if (!current_receipt.has_value() ||
            current_receipt->request_digest != head.request_digest ||
            current_receipt->receipt
                    .durable_public_pointer_content_digest.value() !=
                head.pointer_digest) {
          throw ActiveBundlePublicationIntegrityError(
              "active-bundle publication head is inconsistent");
        }
      }
      const auto receipt = ReceiptByGenerationLocked(
          static_cast<std::int64_t>(generation.value()));
      transaction.Commit();
      if (!receipt.has_value()) {
        return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
            "missing_reference",
            "active-bundle publication generation is not present",
            {{"reference_type",
              "active_bundle_publication_generation"},
             {"reference_id", std::to_string(generation.value())}},
        });
      }
      if (head.generation !=
          static_cast<std::int64_t>(generation.value())) {
        return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
            "stale_generation",
            "requested publication generation is no longer the active "
            "public pointer",
            {{"expected_generation", std::to_string(generation.value())},
             {"current_generation", std::to_string(head.generation)}},
        });
      }
      return Result<ActiveBundlePublicationReceiptDTO>::Ok(
          receipt->receipt);
    } catch (const std::exception& error) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "internal_error",
          "durable active-bundle publication read failed",
          {{"reason", error.what()}},
      });
    }
  }

  [[nodiscard]] ActiveBundlePublicationStoreIntegrityReport
  VerifyIntegrity() const {
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction transaction(database_, "BEGIN");
    // Reading data_version opens this transaction's read snapshot. A commit
    // through any other connection changes it; a write through this
    // connection changes its total change count. Only a provably unchanged
    // database reuses the last complete verification.
    const auto data_version = ScalarInteger(database_, "PRAGMA data_version");
    const auto own_total_changes = sqlite3_total_changes64(database_);
    if (verified_.has_value() && verified_->data_version == data_version &&
        verified_->own_total_changes == own_total_changes) {
      auto report = verified_->report;
      transaction.Commit();
      return report;
    }
    verified_.reset();
    auto report = VerifyIntegrityLocked();
    transaction.Commit();
    ++complete_verifications_;
    verified_.emplace(VerifiedIntegrity{data_version, own_total_changes, report});
    return report;
  }

  struct VerifiedIntegrity final {
    std::int64_t data_version{};
    sqlite3_int64 own_total_changes{};
    ActiveBundlePublicationStoreIntegrityReport report;
  };

  std::filesystem::path database_path_;
  core::CellId cell_id_;
  sqlite3* database_{};
  mutable std::mutex mutex_;
  bool closed_{};
  bool borrowed_{};
  /// Last complete integrity verification and the change identity it held
  /// under. Guarded by `mutex_`; discarded before every re-verification.
  mutable std::optional<VerifiedIntegrity> verified_;
  mutable std::uint64_t complete_verifications_{};
};

Result<ActiveBundlePublicationReceiptDTO>
ReadActiveBundlePublicationReadOnly(const std::filesystem::path &database_path,
                                    const core::CellId &cell_id) {
  using Reply = Result<ActiveBundlePublicationReceiptDTO>;
  const auto rejected = [] {
    return Reply::Err(
        {"active_bundle_publication_unavailable",
         "The current active publication cannot be verified read-only",
         {{"reason", "active_bundle_publication_read_invalid"}}});
  };
  sqlite3 *raw = nullptr;
  try {
    ValidateBoundedIdentity(cell_id.value(), "cell_id");
    const auto path = ValidateDatabasePath(database_path);
    PreflightPublicationStore(path, cell_id);
    if (sqlite3_open_v2(path.string().c_str(), &raw,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
                            SQLITE_OPEN_NOFOLLOW,
                        nullptr) != SQLITE_OK) {
      if (raw)
        sqlite3_close(raw);
      raw = nullptr;
      return rejected();
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw,
                                                                sqlite3_close);
    raw = nullptr;
    Check(database.get(),
          sqlite3_busy_timeout(database.get(), kBusyTimeoutMilliseconds),
          "configure read-only publication timeout");
    Exec(database.get(), "PRAGMA query_only=ON");
    Transaction snapshot(database.get(), "BEGIN");
    ValidatePublicationStoreIdentityAndSchema(database.get(), cell_id);
    SqliteActiveBundlePublicationStore::Impl view(database.get(), cell_id);
    const auto integrity = view.VerifyIntegrityLocked();
    if (!integrity.current_generation.nonzero())
      return rejected();
    const auto receipt = view.ReceiptByGenerationLocked(
        static_cast<std::int64_t>(integrity.current_generation.value()));
    if (!receipt)
      return rejected();
    snapshot.Commit();
    return Reply::Ok(receipt->receipt);
  } catch (const std::exception &) {
    if (raw)
      sqlite3_close(raw);
    return rejected();
  }
}

SqliteActiveBundlePublicationStore::SqliteActiveBundlePublicationStore(
    std::filesystem::path database_path,
    core::CellId cell_id)
    : impl_(std::make_unique<Impl>(
          std::move(database_path), std::move(cell_id))) {}

SqliteActiveBundlePublicationStore::~SqliteActiveBundlePublicationStore() =
    default;

SqliteActiveBundlePublicationStore::SqliteActiveBundlePublicationStore(
    SqliteActiveBundlePublicationStore&&) noexcept = default;

SqliteActiveBundlePublicationStore&
SqliteActiveBundlePublicationStore::operator=(
    SqliteActiveBundlePublicationStore&&) noexcept = default;

const std::filesystem::path&
SqliteActiveBundlePublicationStore::database_path() const noexcept {
  return impl_->database_path_;
}

const core::CellId&
SqliteActiveBundlePublicationStore::cell_id() const noexcept {
  return impl_->cell_id_;
}

ActiveBundlePublicationReceiptDTO
SqliteActiveBundlePublicationStore::PublishActiveBundle(
    const ActiveBundlePublicationRequestDTO& request) {
  return impl_->Publish(request);
}

Result<ActiveBundlePublicationReceiptDTO>
SqliteActiveBundlePublicationStore::ReadActiveBundlePublication(
    const ActiveBundlePublicationGeneration publication_generation) const {
  return impl_->Read(publication_generation);
}

ActiveBundlePublicationStoreIntegrityReport
SqliteActiveBundlePublicationStore::VerifyIntegrity() const {
  return impl_->VerifyIntegrity();
}

std::uint64_t
SqliteActiveBundlePublicationStore::complete_verification_count() const {
  std::lock_guard lock(impl_->mutex_);
  return impl_->complete_verifications_;
}

void SqliteActiveBundlePublicationStore::Close() {
  impl_->Close();
}

}  // namespace manyforge::component_manager
