#include "manyforge_component_manager/persistence.hpp"

#include "embedded_persistence_schema.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>

#if SQLITE_VERSION_NUMBER < 3037000
#error "manyforge_component_manager requires SQLite 3.37.0 or newer"
#endif

#ifndef SQLITE_OPEN_NOFOLLOW
#error "manyforge_component_manager requires SQLITE_OPEN_NOFOLLOW"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace manyforge::component_manager {
namespace {

constexpr std::string_view kDigestPrefix =
    "manyforge.component-manager-persistence-fixture.v1";
constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kWalCheckpointTargetBytes =
    64LL * 1024LL * 1024LL;

void ValidateIdentity(
    const std::string_view value,
    const std::string_view label,
    const bool optional = false) {
  if (optional && value.empty()) {
    return;
  }
  if (value.empty() || value.size() > kMaximumPersistenceIdentifierBytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(
        std::string(label) + " must be a bounded printable ASCII identity");
  }
}

void ValidatePositive(
    const std::int64_t value,
    const std::string_view label) {
  if (value <= 0) {
    throw std::invalid_argument(
        std::string(label) + " must be a positive signed SQLite int64");
  }
}

void ValidateNonnegative(
    const std::int64_t value,
    const std::string_view label) {
  if (value < 0) {
    throw std::invalid_argument(
        std::string(label) +
        " must be a non-negative signed SQLite int64");
  }
}

bool IsSha256Digest(const std::string_view value) {
  if (value.size() != 71U || !value.starts_with("sha256:")) {
    return false;
  }
  return std::all_of(
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
    throw PersistenceIntegrityError("cannot allocate SHA-256 context");
  }
  const auto release = [&]() { EVP_MD_CTX_free(context); };
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &output_length) != 1) {
    release();
    throw PersistenceIntegrityError("SHA-256 computation failed");
  }
  release();
  return "sha256:" +
      Hex(std::span<const unsigned char>(output.data(), output_length));
}

std::string RawDigest(const std::string_view value) {
  return RawDigest(
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(value.data()),
          value.size()));
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
      [](const auto& left, const auto& right) {
        return left.first < right.first;
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

JsonField StringField(
    std::string name,
    const std::string_view value) {
  return {std::move(name), JsonString(value)};
}

JsonField IntegerField(
    std::string name,
    const std::int64_t value) {
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

std::string OperationIdentityDigest(
    const std::string& cell_id,
    const OperationKind kind,
    const std::string& operation_id,
    const std::int64_t generation,
    const std::string& request_id,
    const std::string& manager_owner_epoch,
    const std::string& intent_digest,
    const std::int64_t expected_generation,
    const std::string& predecessor_bundle_identity,
    const std::string& predecessor_graph_revision,
    const std::string& target_bundle_identity,
    const std::string& target_graph_revision) {
  return SemanticDigest(
      "operation-identity",
      JsonObject({
          StringField("cell_id", cell_id),
          IntegerField("expected_fixture_generation", expected_generation),
          StringField("intent_digest", intent_digest),
          StringField("manager_owner_epoch", manager_owner_epoch),
          IntegerField("operation_generation", generation),
          StringField("operation_id", operation_id),
          StringField("operation_kind", ToString(kind)),
          StringField(
              "predecessor_bundle_identity",
              predecessor_bundle_identity),
          StringField(
              "predecessor_graph_revision",
              predecessor_graph_revision),
          StringField("request_id", request_id),
          StringField("target_bundle_identity", target_bundle_identity),
          StringField("target_graph_revision", target_graph_revision),
      }));
}

std::string JournalEntryDigest(
    const std::string& cell_id,
    const OperationRef& ref,
    const std::int64_t sequence,
    const std::string& entry_id,
    const std::string& event_kind,
    const std::string& payload_digest,
    const std::string& previous_entry_digest) {
  return SemanticDigest(
      "journal-entry",
      JsonObject({
          StringField("cell_id", cell_id),
          StringField("entry_id", entry_id),
          StringField("event_kind", event_kind),
          IntegerField("operation_generation", ref.generation()),
          StringField("operation_id", ref.operation_id()),
          StringField("operation_kind", ToString(ref.kind())),
          StringField("payload_digest", payload_digest),
          StringField("previous_entry_digest", previous_entry_digest),
          IntegerField("sequence", sequence),
      }));
}

std::string FoundationIntentDigest(
    const std::string& cell_id,
    const FoundationAdvanceIntent& intent) {
  return SemanticDigest(
      "fixture-cas-intent",
      JsonObject({
          StringField("cell_id", cell_id),
          StringField("fixture_cas_id", intent.advance_id()),
          IntegerField(
              "operation_generation",
              intent.operation_ref().generation()),
          StringField(
              "operation_id",
              intent.operation_ref().operation_id()),
          StringField(
              "operation_identity_digest",
              intent.operation_identity_digest()),
          StringField(
              "operation_kind",
              ToString(intent.operation_ref().kind())),
      }));
}

std::string FoundationReceiptDigest(
    const std::string& cell_id,
    const std::string& advance_id,
    const std::string& advance_intent_digest,
    const OperationRecord& operation,
    const std::int64_t committed_generation) {
  return SemanticDigest(
      "fixture-cas-receipt",
      JsonObject({
          StringField("cell_id", cell_id),
          IntegerField("committed_fixture_generation", committed_generation),
          IntegerField(
              "expected_fixture_generation",
              operation.expected_foundation_generation()),
          StringField("fixture_cas_id", advance_id),
          StringField(
              "fixture_cas_intent_digest",
              advance_intent_digest),
          IntegerField(
              "operation_generation",
              operation.ref().generation()),
          StringField("operation_id", operation.ref().operation_id()),
          StringField(
              "operation_identity_digest",
              operation.identity_digest()),
          StringField("operation_kind", ToString(operation.ref().kind())),
          StringField(
              "predecessor_bundle_identity",
              operation.predecessor_bundle_identity()),
          StringField(
              "predecessor_graph_revision",
              operation.predecessor_graph_revision()),
          StringField(
              "target_bundle_identity",
              operation.target_bundle_identity()),
          StringField(
              "target_graph_revision",
              operation.target_graph_revision()),
      }));
}

[[noreturn]] void ThrowSqlite(
    sqlite3* database,
    const int result,
    const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL) {
    throw PersistenceLimitError(
        std::string(context) + ": SQLite storage ceiling exhausted: " +
        detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
    throw PersistenceConflictError(
        std::string(context) + ": SQLite writer is busy: " + detail);
  }
  throw PersistenceError(
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
      database,
      std::string(sql).c_str(),
      nullptr,
      nullptr,
      &error);
  if (result != SQLITE_OK) {
    const std::string detail =
        error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    if (result == SQLITE_FULL) {
      throw PersistenceLimitError(detail);
    }
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
      throw PersistenceConflictError(detail);
    }
    throw PersistenceError(detail);
  }
}

class Statement final {
 public:
  Statement(sqlite3* database, const std::string_view sql)
      : database_(database) {
    Check(
        database_,
        sqlite3_prepare_v2(
            database_,
            std::string(sql).c_str(),
            -1,
            &statement_,
            nullptr),
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

  void Bind(const int index, const std::span<const std::byte> value) {
    static constexpr std::byte kEmptyBlobSentinel{};
    const void* data =
        value.empty() ? static_cast<const void*>(&kEmptyBlobSentinel)
                      : static_cast<const void*>(value.data());
    Check(
        database_,
        sqlite3_bind_blob64(
            statement_,
            index,
            data,
            value.size(),
            SQLITE_TRANSIENT),
        "bind SQLite blob");
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
    const auto* text = sqlite3_column_text(statement_, column);
    if (text == nullptr) {
      return {};
    }
    return {
        reinterpret_cast<const char*>(text),
        static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)),
    };
  }

  [[nodiscard]] std::int64_t Integer(const int column) const {
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] std::vector<std::byte> Blob(const int column) const {
    const auto size = sqlite3_column_bytes(statement_, column);
    const auto* data = static_cast<const std::byte*>(
        sqlite3_column_blob(statement_, column));
    if (size == 0) {
      return {};
    }
    if (data == nullptr || size < 0) {
      throw PersistenceIntegrityError("SQLite blob row is invalid");
    }
    return {data, data + size};
  }

  [[nodiscard]] int Changes() const {
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

using SchemaObjects = std::map<std::pair<std::string, std::string>, std::string>;

// The versioned artifact currently owns exactly this many persistent SQLite
// objects.  Keeping the cardinality explicit makes an artifact change fail
// closed until both the SQL and its ingress bound are reviewed together.
constexpr std::int64_t kExpectedSchemaObjectCardinality = 15;

SchemaObjects ReadSchemaObjects(sqlite3* database) {
  Statement statement(
      database,
      "SELECT type, name, sql FROM sqlite_master "
      "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name LIMIT ?");
  statement.Bind(1, kExpectedSchemaObjectCardinality + 1);
  SchemaObjects objects;
  std::int64_t row_count = 0;
  while (statement.Step()) {
    ++row_count;
    if (row_count > kExpectedSchemaObjectCardinality) {
      throw PersistenceIntegrityError(
          "SQLite schema object cardinality exceeds the versioned artifact");
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
      throw PersistenceError(
          "open in-memory schema validator: " + detail);
    }
    try {
      Exec(database, "PRAGMA foreign_keys = ON");
      Exec(database, detail::kEmbeddedPersistenceSchema);
      auto objects = ReadSchemaObjects(database);
      if (objects.size() !=
          static_cast<std::size_t>(kExpectedSchemaObjectCardinality)) {
        throw PersistenceError(
            "embedded persistence schema object cardinality is invalid");
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

std::int64_t ScalarInteger(sqlite3* database, const std::string_view sql) {
  Statement statement(database, sql);
  if (!statement.Step()) {
    throw PersistenceIntegrityError("SQLite scalar row is missing");
  }
  return statement.Integer(0);
}

OperationKind ParseOperationKind(const std::string_view value) {
  if (value == "bootstrap") {
    return OperationKind::kBootstrap;
  }
  if (value == "apply") {
    return OperationKind::kApply;
  }
  throw PersistenceIntegrityError("stored operation kind is invalid");
}

std::filesystem::path ValidateDatabasePath(std::filesystem::path path) {
  if (path.empty() || path == ":memory:") {
    throw std::invalid_argument(
        "a durable filesystem database path is required");
  }
  std::error_code error;
  const auto path_status = std::filesystem::symlink_status(path, error);
  if (error &&
      error != std::make_error_code(std::errc::no_such_file_or_directory)) {
    throw std::invalid_argument("cannot inspect persistence database path");
  }
  if (!error && std::filesystem::is_symlink(path_status)) {
    throw std::invalid_argument(
        "persistence database path cannot be a symlink");
  }
  if (!error && std::filesystem::exists(path_status) &&
      !std::filesystem::is_regular_file(path_status)) {
    throw std::invalid_argument(
        "persistence database path must be a regular file");
  }
  if (!error && std::filesystem::exists(path_status)) {
    const auto file_size = std::filesystem::file_size(path, error);
    if (error) {
      throw std::invalid_argument(
          "cannot inspect persistence database file size");
    }
    if (file_size >
        static_cast<std::uintmax_t>(
            kMaximumMainDatabasePageImageBytes)) {
      throw PersistenceLimitError(
          "SQLite main database file exceeds its ceiling");
    }
  }
  error.clear();
  const auto parent =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  if (!std::filesystem::exists(parent, error) ||
      !std::filesystem::is_directory(parent, error)) {
    throw std::invalid_argument("persistence database parent must exist");
  }
  return path;
}

}  // namespace

OperationRef::OperationRef(
    const OperationKind kind,
    std::string operation_id,
    const std::int64_t generation)
    : kind_(kind),
      operation_id_(std::move(operation_id)),
      generation_(generation) {
  static_cast<void>(ToString(kind_));
  ValidateIdentity(operation_id_, "operation_id");
  ValidatePositive(generation_, "operation generation");
}

OperationKind OperationRef::kind() const noexcept {
  return kind_;
}

const std::string& OperationRef::operation_id() const noexcept {
  return operation_id_;
}

std::int64_t OperationRef::generation() const noexcept {
  return generation_;
}

BootstrapIdentity::BootstrapIdentity(
    std::string bootstrap_id,
    const std::int64_t generation,
    std::string intent_digest,
    std::string target_bundle_identity,
    std::string target_graph_revision)
    : bootstrap_id_(std::move(bootstrap_id)),
      generation_(generation),
      intent_digest_(std::move(intent_digest)),
      target_bundle_identity_(std::move(target_bundle_identity)),
      target_graph_revision_(std::move(target_graph_revision)) {
  ValidateIdentity(bootstrap_id_, "bootstrap_id");
  ValidatePositive(generation_, "bootstrap generation");
  ValidateDigest(intent_digest_, "intent_digest");
  ValidateIdentity(target_bundle_identity_, "target_bundle_identity");
  ValidateIdentity(target_graph_revision_, "target_graph_revision");
}

const std::string& BootstrapIdentity::bootstrap_id() const noexcept {
  return bootstrap_id_;
}

std::int64_t BootstrapIdentity::generation() const noexcept {
  return generation_;
}

const std::string& BootstrapIdentity::intent_digest() const noexcept {
  return intent_digest_;
}

const std::string& BootstrapIdentity::target_bundle_identity() const noexcept {
  return target_bundle_identity_;
}

const std::string& BootstrapIdentity::target_graph_revision() const noexcept {
  return target_graph_revision_;
}

ApplyIdentity::ApplyIdentity(
    std::string apply_id,
    const std::int64_t generation,
    std::string request_id,
    std::string manager_owner_epoch,
    std::string intent_digest,
    const std::int64_t expected_foundation_generation,
    std::string predecessor_bundle_identity,
    std::string predecessor_graph_revision,
    std::string target_bundle_identity,
    std::string target_graph_revision)
    : apply_id_(std::move(apply_id)),
      generation_(generation),
      request_id_(std::move(request_id)),
      manager_owner_epoch_(std::move(manager_owner_epoch)),
      intent_digest_(std::move(intent_digest)),
      expected_foundation_generation_(expected_foundation_generation),
      predecessor_bundle_identity_(std::move(predecessor_bundle_identity)),
      predecessor_graph_revision_(std::move(predecessor_graph_revision)),
      target_bundle_identity_(std::move(target_bundle_identity)),
      target_graph_revision_(std::move(target_graph_revision)) {
  ValidateIdentity(apply_id_, "apply_id");
  ValidatePositive(generation_, "apply generation");
  ValidateIdentity(request_id_, "request_id");
  ValidateIdentity(manager_owner_epoch_, "manager_owner_epoch");
  ValidateDigest(intent_digest_, "intent_digest");
  ValidatePositive(
      expected_foundation_generation_,
      "expected_foundation_generation");
  ValidateIdentity(
      predecessor_bundle_identity_,
      "predecessor_bundle_identity");
  ValidateIdentity(
      predecessor_graph_revision_,
      "predecessor_graph_revision");
  ValidateIdentity(target_bundle_identity_, "target_bundle_identity");
  ValidateIdentity(target_graph_revision_, "target_graph_revision");
}

const std::string& ApplyIdentity::apply_id() const noexcept {
  return apply_id_;
}

std::int64_t ApplyIdentity::generation() const noexcept {
  return generation_;
}

const std::string& ApplyIdentity::request_id() const noexcept {
  return request_id_;
}

const std::string& ApplyIdentity::manager_owner_epoch() const noexcept {
  return manager_owner_epoch_;
}

const std::string& ApplyIdentity::intent_digest() const noexcept {
  return intent_digest_;
}

std::int64_t ApplyIdentity::expected_foundation_generation() const noexcept {
  return expected_foundation_generation_;
}

const std::string& ApplyIdentity::predecessor_bundle_identity()
    const noexcept {
  return predecessor_bundle_identity_;
}

const std::string& ApplyIdentity::predecessor_graph_revision()
    const noexcept {
  return predecessor_graph_revision_;
}

const std::string& ApplyIdentity::target_bundle_identity() const noexcept {
  return target_bundle_identity_;
}

const std::string& ApplyIdentity::target_graph_revision() const noexcept {
  return target_graph_revision_;
}

OperationRecord::OperationRecord(
    OperationRef ref,
    std::string request_id,
    std::string manager_owner_epoch,
    std::string intent_digest,
    const std::int64_t expected_foundation_generation,
    std::string predecessor_bundle_identity,
    std::string predecessor_graph_revision,
    std::string target_bundle_identity,
    std::string target_graph_revision,
    std::string identity_digest,
    const std::int64_t created_sequence)
    : ref_(std::move(ref)),
      request_id_(std::move(request_id)),
      manager_owner_epoch_(std::move(manager_owner_epoch)),
      intent_digest_(std::move(intent_digest)),
      expected_foundation_generation_(expected_foundation_generation),
      predecessor_bundle_identity_(std::move(predecessor_bundle_identity)),
      predecessor_graph_revision_(std::move(predecessor_graph_revision)),
      target_bundle_identity_(std::move(target_bundle_identity)),
      target_graph_revision_(std::move(target_graph_revision)),
      identity_digest_(std::move(identity_digest)),
      created_sequence_(created_sequence) {
  ValidateDigest(intent_digest_, "intent_digest");
  ValidateNonnegative(
      expected_foundation_generation_,
      "expected_foundation_generation");
  ValidateIdentity(request_id_, "request_id", true);
  ValidateIdentity(manager_owner_epoch_, "manager_owner_epoch", true);
  ValidateIdentity(
      predecessor_bundle_identity_,
      "predecessor_bundle_identity",
      true);
  ValidateIdentity(
      predecessor_graph_revision_,
      "predecessor_graph_revision",
      true);
  ValidateIdentity(target_bundle_identity_, "target_bundle_identity");
  ValidateIdentity(target_graph_revision_, "target_graph_revision");
  ValidateDigest(identity_digest_, "identity_digest");
  ValidatePositive(created_sequence_, "created_sequence");
  if (ref_.kind() == OperationKind::kBootstrap) {
    if (!request_id_.empty() || !manager_owner_epoch_.empty() ||
        expected_foundation_generation_ != 0 ||
        !predecessor_bundle_identity_.empty() ||
        !predecessor_graph_revision_.empty()) {
      throw PersistenceIntegrityError(
          "stored bootstrap operation shape is invalid");
    }
  } else if (
      request_id_.empty() || manager_owner_epoch_.empty() ||
      expected_foundation_generation_ <= 0 ||
      predecessor_bundle_identity_.empty() ||
      predecessor_graph_revision_.empty()) {
    throw PersistenceIntegrityError("stored Apply operation shape is invalid");
  }
}

const OperationRef& OperationRecord::ref() const noexcept {
  return ref_;
}

const std::string& OperationRecord::request_id() const noexcept {
  return request_id_;
}

const std::string& OperationRecord::manager_owner_epoch() const noexcept {
  return manager_owner_epoch_;
}

const std::string& OperationRecord::intent_digest() const noexcept {
  return intent_digest_;
}

std::int64_t OperationRecord::expected_foundation_generation() const noexcept {
  return expected_foundation_generation_;
}

const std::string& OperationRecord::predecessor_bundle_identity()
    const noexcept {
  return predecessor_bundle_identity_;
}

const std::string& OperationRecord::predecessor_graph_revision()
    const noexcept {
  return predecessor_graph_revision_;
}

const std::string& OperationRecord::target_bundle_identity() const noexcept {
  return target_bundle_identity_;
}

const std::string& OperationRecord::target_graph_revision() const noexcept {
  return target_graph_revision_;
}

const std::string& OperationRecord::identity_digest() const noexcept {
  return identity_digest_;
}

std::int64_t OperationRecord::created_sequence() const noexcept {
  return created_sequence_;
}

JournalEvent::JournalEvent(
    std::string entry_id,
    std::string event_kind,
    std::vector<std::byte> payload)
    : entry_id_(std::move(entry_id)),
      event_kind_(std::move(event_kind)),
      payload_(std::move(payload)) {
  ValidateIdentity(entry_id_, "entry_id");
  ValidateIdentity(event_kind_, "event_kind");
  if (payload_.size() > kMaximumJournalPayloadBytes) {
    throw PersistenceLimitError("journal payload exceeds fixed limit");
  }
}

const std::string& JournalEvent::entry_id() const noexcept {
  return entry_id_;
}

const std::string& JournalEvent::event_kind() const noexcept {
  return event_kind_;
}

const std::vector<std::byte>& JournalEvent::payload() const noexcept {
  return payload_;
}

JournalEntry::JournalEntry(
    OperationRef ref,
    const std::int64_t sequence,
    std::string entry_id,
    std::string event_kind,
    std::vector<std::byte> payload,
    std::string payload_digest,
    std::string previous_entry_digest,
    std::string entry_digest)
    : ref_(std::move(ref)),
      sequence_(sequence),
      entry_id_(std::move(entry_id)),
      event_kind_(std::move(event_kind)),
      payload_(std::move(payload)),
      payload_digest_(std::move(payload_digest)),
      previous_entry_digest_(std::move(previous_entry_digest)),
      entry_digest_(std::move(entry_digest)) {
  ValidatePositive(sequence_, "journal sequence");
  ValidateIdentity(entry_id_, "entry_id");
  ValidateIdentity(event_kind_, "event_kind");
  if (payload_.size() > kMaximumJournalPayloadBytes) {
    throw PersistenceIntegrityError("stored journal payload exceeds limit");
  }
  ValidateDigest(payload_digest_, "payload_digest");
  ValidateDigest(previous_entry_digest_, "previous_entry_digest");
  ValidateDigest(entry_digest_, "entry_digest");
}

const OperationRef& JournalEntry::ref() const noexcept {
  return ref_;
}

std::int64_t JournalEntry::sequence() const noexcept {
  return sequence_;
}

const std::string& JournalEntry::entry_id() const noexcept {
  return entry_id_;
}

const std::string& JournalEntry::event_kind() const noexcept {
  return event_kind_;
}

const std::vector<std::byte>& JournalEntry::payload() const noexcept {
  return payload_;
}

const std::string& JournalEntry::payload_digest() const noexcept {
  return payload_digest_;
}

const std::string& JournalEntry::previous_entry_digest() const noexcept {
  return previous_entry_digest_;
}

const std::string& JournalEntry::entry_digest() const noexcept {
  return entry_digest_;
}

FoundationAdvanceIntent::FoundationAdvanceIntent(
    std::string advance_id,
    OperationRef operation_ref,
    std::string operation_identity_digest)
    : advance_id_(std::move(advance_id)),
      operation_ref_(std::move(operation_ref)),
      operation_identity_digest_(std::move(operation_identity_digest)) {
  ValidateIdentity(advance_id_, "advance_id");
  ValidateDigest(
      operation_identity_digest_,
      "operation_identity_digest");
}

const std::string& FoundationAdvanceIntent::advance_id() const noexcept {
  return advance_id_;
}

const OperationRef& FoundationAdvanceIntent::operation_ref() const noexcept {
  return operation_ref_;
}

const std::string&
FoundationAdvanceIntent::operation_identity_digest() const noexcept {
  return operation_identity_digest_;
}

FoundationReceipt::FoundationReceipt(
    std::string advance_id,
    std::string intent_digest,
    OperationRef operation_ref,
    std::string operation_identity_digest,
    const std::int64_t expected_generation,
    std::string predecessor_bundle_identity,
    std::string predecessor_graph_revision,
    const std::int64_t committed_generation,
    std::string target_bundle_identity,
    std::string target_graph_revision,
    std::string receipt_digest,
    const std::int64_t created_sequence)
    : advance_id_(std::move(advance_id)),
      intent_digest_(std::move(intent_digest)),
      operation_ref_(std::move(operation_ref)),
      operation_identity_digest_(std::move(operation_identity_digest)),
      expected_generation_(expected_generation),
      predecessor_bundle_identity_(std::move(predecessor_bundle_identity)),
      predecessor_graph_revision_(std::move(predecessor_graph_revision)),
      committed_generation_(committed_generation),
      target_bundle_identity_(std::move(target_bundle_identity)),
      target_graph_revision_(std::move(target_graph_revision)),
      receipt_digest_(std::move(receipt_digest)),
      created_sequence_(created_sequence) {
  ValidateIdentity(advance_id_, "advance_id");
  ValidateDigest(intent_digest_, "intent_digest");
  ValidateDigest(
      operation_identity_digest_,
      "operation_identity_digest");
  ValidateNonnegative(expected_generation_, "expected_generation");
  ValidateIdentity(
      predecessor_bundle_identity_,
      "predecessor_bundle_identity",
      true);
  ValidateIdentity(
      predecessor_graph_revision_,
      "predecessor_graph_revision",
      true);
  ValidatePositive(committed_generation_, "committed_generation");
  ValidateIdentity(target_bundle_identity_, "target_bundle_identity");
  ValidateIdentity(target_graph_revision_, "target_graph_revision");
  ValidateDigest(receipt_digest_, "receipt_digest");
  ValidatePositive(created_sequence_, "created_sequence");
  if (expected_generation_ == std::numeric_limits<std::int64_t>::max() ||
      committed_generation_ != expected_generation_ + 1) {
    throw PersistenceIntegrityError(
        "stored foundation generation is not the CAS successor");
  }
}

const std::string& FoundationReceipt::advance_id() const noexcept {
  return advance_id_;
}

const std::string& FoundationReceipt::intent_digest() const noexcept {
  return intent_digest_;
}

const OperationRef& FoundationReceipt::operation_ref() const noexcept {
  return operation_ref_;
}

const std::string&
FoundationReceipt::operation_identity_digest() const noexcept {
  return operation_identity_digest_;
}

std::int64_t FoundationReceipt::expected_generation() const noexcept {
  return expected_generation_;
}

const std::string& FoundationReceipt::predecessor_bundle_identity()
    const noexcept {
  return predecessor_bundle_identity_;
}

const std::string& FoundationReceipt::predecessor_graph_revision()
    const noexcept {
  return predecessor_graph_revision_;
}

std::int64_t FoundationReceipt::committed_generation() const noexcept {
  return committed_generation_;
}

const std::string& FoundationReceipt::target_bundle_identity()
    const noexcept {
  return target_bundle_identity_;
}

const std::string& FoundationReceipt::target_graph_revision() const noexcept {
  return target_graph_revision_;
}

const std::string& FoundationReceipt::receipt_digest() const noexcept {
  return receipt_digest_;
}

std::int64_t FoundationReceipt::created_sequence() const noexcept {
  return created_sequence_;
}

FoundationHead::FoundationHead(
    const std::int64_t generation,
    std::string bundle_identity,
    std::string graph_revision,
    std::string advance_id,
    std::string receipt_digest,
    OperationRef operation_ref)
    : generation_(generation),
      bundle_identity_(std::move(bundle_identity)),
      graph_revision_(std::move(graph_revision)),
      advance_id_(std::move(advance_id)),
      receipt_digest_(std::move(receipt_digest)),
      operation_ref_(std::move(operation_ref)) {
  ValidatePositive(generation_, "foundation head generation");
  ValidateIdentity(bundle_identity_, "bundle_identity");
  ValidateIdentity(graph_revision_, "graph_revision");
  ValidateIdentity(advance_id_, "advance_id");
  ValidateDigest(receipt_digest_, "receipt_digest");
}

std::int64_t FoundationHead::generation() const noexcept {
  return generation_;
}

const std::string& FoundationHead::bundle_identity() const noexcept {
  return bundle_identity_;
}

const std::string& FoundationHead::graph_revision() const noexcept {
  return graph_revision_;
}

const std::string& FoundationHead::advance_id() const noexcept {
  return advance_id_;
}

const std::string& FoundationHead::receipt_digest() const noexcept {
  return receipt_digest_;
}

const OperationRef& FoundationHead::operation_ref() const noexcept {
  return operation_ref_;
}

class ManagerPersistence::Impl final {
 public:
  Impl(std::filesystem::path database_path, std::string cell_id)
      : database_path_(ValidateDatabasePath(std::move(database_path))),
        cell_id_(std::move(cell_id)) {
    ValidateIdentity(cell_id_, "cell_id");
    if (sqlite3_libversion_number() < 3037000) {
      throw PersistenceError(
          "manyforge_component_manager requires SQLite 3.37.0 or newer");
    }
    int open_flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_NOFOLLOW;
    const auto result = sqlite3_open_v2(
        database_path_.string().c_str(),
        &database_,
        open_flags,
        nullptr);
    if (result != SQLITE_OK) {
      const std::string error =
          database_ == nullptr ? sqlite3_errstr(result) :
                                 sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      throw PersistenceError("cannot open persistence database: " + error);
    }
    try {
      Configure();
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

  ~Impl() {
    try {
      Close();
    } catch (...) {
    }
  }

  void RequireOpen() const {
    if (closed_ || database_ == nullptr) {
      throw PersistenceClosedError("manager persistence is closed");
    }
  }

  void Close() {
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
      ThrowSqlite(database_, result, "close persistence database");
    }
    database_ = nullptr;
    closed_ = true;
  }

  void Configure() {
    Exec(database_, "PRAGMA foreign_keys = ON");
    if (ScalarInteger(database_, "PRAGMA foreign_keys") != 1) {
      throw PersistenceIntegrityError(
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
          throw PersistenceIntegrityError(
              "SQLite WAL journal mode is required");
        }
        break;
      } catch (const PersistenceConflictError&) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    Exec(database_, "PRAGMA synchronous = FULL");
    if (ScalarInteger(database_, "PRAGMA synchronous") != 2) {
      throw PersistenceIntegrityError(
          "SQLite FULL synchronous mode is required");
    }
    Exec(database_, "PRAGMA trusted_schema = OFF");
    if (ScalarInteger(database_, "PRAGMA trusted_schema") != 0) {
      throw PersistenceIntegrityError(
          "SQLite untrusted-schema mode is required");
    }
    Exec(database_, "PRAGMA recursive_triggers = OFF");
    if (ScalarInteger(database_, "PRAGMA recursive_triggers") != 0) {
      throw PersistenceIntegrityError(
          "SQLite recursive triggers must remain disabled");
    }
    Exec(database_, "PRAGMA wal_autocheckpoint = 1000");
    if (ScalarInteger(database_, "PRAGMA wal_autocheckpoint") != 1000) {
      throw PersistenceIntegrityError(
          "SQLite WAL autocheckpoint policy is required");
    }
    const auto journal_size_limit = ScalarInteger(
        database_,
        "PRAGMA journal_size_limit = " +
            std::to_string(kWalCheckpointTargetBytes));
    if (journal_size_limit != kWalCheckpointTargetBytes) {
      throw PersistenceIntegrityError(
          "SQLite WAL journal-size policy is required");
    }
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_size <= 0) {
      throw PersistenceIntegrityError("SQLite page size is invalid");
    }
    const auto maximum_pages = std::max<std::int64_t>(
        1,
        kMaximumMainDatabasePageImageBytes / page_size);
    const auto page_count =
        ScalarInteger(database_, "PRAGMA page_count");
    if (page_count < 0 || page_count > maximum_pages) {
      throw PersistenceLimitError(
          "SQLite main database page image exceeds its ceiling");
    }
    const auto effective_maximum_pages = ScalarInteger(
        database_,
        "PRAGMA max_page_count = " + std::to_string(maximum_pages));
    if (effective_maximum_pages != maximum_pages) {
      throw PersistenceLimitError(
          "SQLite main database page ceiling could not be enforced");
    }
    sqlite3_limit(
        database_,
        SQLITE_LIMIT_LENGTH,
        static_cast<int>(kMaximumJournalPayloadBytes + 64U * 1024U));
  }

  void InitializeOrValidateSchema() {
    std::lock_guard lock(mutex_);
    Transaction transaction(database_, "BEGIN EXCLUSIVE");
    const auto existing = ReadSchemaObjects(database_);
    if (existing.empty()) {
      Exec(database_, detail::kEmbeddedPersistenceSchema);
      Statement metadata(
          database_,
          "INSERT INTO persistence_metadata ("
          "singleton,schema_id,schema_version,schema_artifact_digest,cell_id,"
          "maximum_operations,maximum_journal_entries_per_operation,"
          "maximum_total_journal_entries,maximum_fixture_cas_receipts,"
          "maximum_payload_bytes,maximum_main_database_page_image_bytes"
          ") VALUES (1,?,?,?,?,?,?,?,?,?,?)");
      metadata.Bind(1, kPersistenceFoundationSchemaId);
      metadata.Bind(2, kPersistenceFoundationSchemaVersion);
      metadata.Bind(3, RawDigest(detail::kEmbeddedPersistenceSchema));
      metadata.Bind(4, cell_id_);
      metadata.Bind(5, kMaximumOperations);
      metadata.Bind(6, kMaximumJournalEntriesPerOperation);
      metadata.Bind(7, kMaximumTotalJournalEntries);
      metadata.Bind(8, kMaximumFoundationReceipts);
      metadata.Bind(
          9,
          static_cast<std::int64_t>(kMaximumJournalPayloadBytes));
      metadata.Bind(10, kMaximumMainDatabasePageImageBytes);
      static_cast<void>(metadata.Step());
      Exec(
          database_,
          "INSERT INTO fixture_cas_head ("
          "singleton,generation,bundle_identity,graph_revision,"
          "fixture_cas_id,receipt_digest,operation_kind,operation_id,"
          "operation_generation"
          ") VALUES (1,0,'','','','','','',0)");
    } else if (
        !existing.contains({"table", "persistence_metadata"})) {
      throw PersistenceIntegrityError(
          "non-empty database lacks persistence metadata");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw PersistenceIntegrityError(
          "SQLite schema objects differ from the versioned artifact");
    }
    transaction.Commit();
  }

  void ValidateMetadataLocked() const {
    if (ScalarInteger(database_, "PRAGMA application_id") !=
        kPersistenceSqliteApplicationId) {
      throw PersistenceIntegrityError("SQLite application ID mismatch");
    }
    if (ScalarInteger(database_, "PRAGMA user_version") !=
        kPersistenceFoundationSchemaVersion) {
      throw PersistenceIntegrityError("SQLite user version mismatch");
    }
    Statement row(database_, "SELECT * FROM persistence_metadata");
    if (!row.Step()) {
      throw PersistenceIntegrityError(
          "persistence metadata singleton is missing");
    }
    if (row.Integer(0) != 1 ||
        row.Text(1) != kPersistenceFoundationSchemaId ||
        row.Integer(2) != kPersistenceFoundationSchemaVersion ||
        row.Text(3) != RawDigest(detail::kEmbeddedPersistenceSchema)) {
      throw PersistenceIntegrityError("persistence metadata mismatch");
    }
    if (row.Text(4) != cell_id_) {
      throw PersistenceConflictError(
          "persistence store belongs to a different cell");
    }
    const std::array<std::int64_t, 6> expected{
        kMaximumOperations,
        kMaximumJournalEntriesPerOperation,
        kMaximumTotalJournalEntries,
        kMaximumFoundationReceipts,
        static_cast<std::int64_t>(kMaximumJournalPayloadBytes),
        kMaximumMainDatabasePageImageBytes,
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
      if (row.Integer(static_cast<int>(index + 5U)) != expected[index]) {
        throw PersistenceIntegrityError(
            "persistence resource limit metadata mismatch");
      }
    }
    if (row.Step()) {
      throw PersistenceIntegrityError(
          "persistence metadata singleton is invalid");
    }
  }

  [[nodiscard]] OperationRecord OperationFromRow(Statement& row) const {
    return OperationRecord(
        OperationRef(
            ParseOperationKind(row.Text(0)),
            row.Text(1),
            row.Integer(2)),
        row.Text(3),
        row.Text(4),
        row.Text(5),
        row.Integer(6),
        row.Text(7),
        row.Text(8),
        row.Text(9),
        row.Text(10),
        row.Text(11),
        row.Integer(12));
  }

  [[nodiscard]] std::optional<OperationRecord> FindOperationLocked(
      const OperationRef& ref) const {
    Statement row(
        database_,
        "SELECT operation_kind,operation_id,operation_generation,request_id,"
        "manager_owner_epoch,intent_digest,expected_fixture_generation,"
        "predecessor_bundle_identity,predecessor_graph_revision,"
        "target_bundle_identity,target_graph_revision,identity_digest,"
        "created_sequence FROM manager_operations "
        "WHERE operation_kind=? AND operation_id=? "
        "AND operation_generation=?");
    row.Bind(1, ToString(ref.kind()));
    row.Bind(2, ref.operation_id());
    row.Bind(3, ref.generation());
    if (!row.Step()) {
      return std::nullopt;
    }
    return OperationFromRow(row);
  }

  [[nodiscard]] OperationWriteResult EnsureOperation(
      const OperationKind kind,
      const std::string& operation_id,
      const std::int64_t generation,
      const std::string& request_id,
      const std::string& manager_owner_epoch,
      const std::string& intent_digest,
      const std::int64_t expected_generation,
      const std::string& predecessor_bundle_identity,
      const std::string& predecessor_graph_revision,
      const std::string& target_bundle_identity,
      const std::string& target_graph_revision) {
    const OperationRef ref(kind, operation_id, generation);
    const auto identity_digest = OperationIdentityDigest(
        cell_id_,
        kind,
        operation_id,
        generation,
        request_id,
        manager_owner_epoch,
        intent_digest,
        expected_generation,
        predecessor_bundle_identity,
        predecessor_graph_revision,
        target_bundle_identity,
        target_graph_revision);
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction transaction(database_, "BEGIN IMMEDIATE");
    if (kind == OperationKind::kApply) {
      Statement request(
          database_,
          "SELECT operation_kind,operation_id,operation_generation,"
          "request_id,manager_owner_epoch,intent_digest,"
          "expected_fixture_generation,predecessor_bundle_identity,"
          "predecessor_graph_revision,target_bundle_identity,"
          "target_graph_revision,identity_digest,created_sequence "
          "FROM manager_operations WHERE operation_kind='apply' "
          "AND manager_owner_epoch=? AND request_id=?");
      request.Bind(1, manager_owner_epoch);
      request.Bind(2, request_id);
      if (request.Step()) {
        auto record = OperationFromRow(request);
        if (!(record.ref() == ref) ||
            record.identity_digest() != identity_digest) {
          throw PersistenceConflictError(
              "Apply request identity already exists with different content");
        }
        transaction.Commit();
        return {std::move(record), false};
      }
    }
    if (auto existing = FindOperationLocked(ref)) {
      if (existing->identity_digest() != identity_digest) {
        throw PersistenceConflictError(
            "operation identity already exists with different content");
      }
      transaction.Commit();
      return {std::move(*existing), false};
    }
    if (kind == OperationKind::kBootstrap &&
        ScalarInteger(
            database_,
            "SELECT COUNT(*) FROM manager_operations "
            "WHERE operation_kind='bootstrap'") != 0) {
      throw PersistenceConflictError(
          "a different bootstrap identity is already fixed");
    }
    const auto count =
        ScalarInteger(database_, "SELECT COUNT(*) FROM manager_operations");
    if (count >= kMaximumOperations) {
      throw PersistenceLimitError("operation record limit is exhausted");
    }
    Statement insert(
        database_,
        "INSERT INTO manager_operations ("
        "operation_kind,operation_id,operation_generation,request_id,"
        "manager_owner_epoch,intent_digest,expected_fixture_generation,"
        "predecessor_bundle_identity,predecessor_graph_revision,"
        "target_bundle_identity,target_graph_revision,identity_digest,"
        "created_sequence) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
    insert.Bind(1, ToString(kind));
    insert.Bind(2, operation_id);
    insert.Bind(3, generation);
    insert.Bind(4, request_id);
    insert.Bind(5, manager_owner_epoch);
    insert.Bind(6, intent_digest);
    insert.Bind(7, expected_generation);
    insert.Bind(8, predecessor_bundle_identity);
    insert.Bind(9, predecessor_graph_revision);
    insert.Bind(10, target_bundle_identity);
    insert.Bind(11, target_graph_revision);
    insert.Bind(12, identity_digest);
    insert.Bind(13, count + 1);
    static_cast<void>(insert.Step());
    auto record = FindOperationLocked(ref);
    if (!record) {
      throw PersistenceIntegrityError(
          "inserted operation cannot be read back");
    }
    transaction.Commit();
    return {std::move(*record), true};
  }

  [[nodiscard]] JournalEntry JournalFromRow(Statement& row) const {
    return JournalEntry(
        OperationRef(
            ParseOperationKind(row.Text(0)),
            row.Text(1),
            row.Integer(2)),
        row.Integer(3),
        row.Text(4),
        row.Text(5),
        row.Blob(6),
        row.Text(7),
        row.Text(8),
        row.Text(9));
  }

  [[nodiscard]] FoundationReceipt ReceiptFromRow(Statement& row) const {
    return FoundationReceipt(
        row.Text(0),
        row.Text(1),
        OperationRef(
            ParseOperationKind(row.Text(2)),
            row.Text(3),
            row.Integer(4)),
        row.Text(5),
        row.Integer(6),
        row.Text(7),
        row.Text(8),
        row.Integer(9),
        row.Text(10),
        row.Text(11),
        row.Text(12),
        row.Integer(13));
  }

  [[nodiscard]] IntegrityReport VerifyIntegrityLocked() const {
    Statement integrity(database_, "PRAGMA integrity_check");
    if (!integrity.Step() || integrity.Text(0) != "ok" ||
        integrity.Step()) {
      throw PersistenceIntegrityError("SQLite integrity_check failed");
    }
    Statement foreign_keys(database_, "PRAGMA foreign_key_check");
    if (foreign_keys.Step()) {
      throw PersistenceIntegrityError(
          "SQLite foreign-key integrity check failed");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw PersistenceIntegrityError(
          "SQLite schema objects differ from the versioned artifact");
    }

    const auto operation_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_operations");
    const auto journal_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_journal_entries");
    const auto receipt_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM fixture_cas_receipts");
    if (operation_count > kMaximumOperations ||
        journal_count > kMaximumTotalJournalEntries ||
        receipt_count > kMaximumFoundationReceipts) {
      throw PersistenceIntegrityError(
          "stored persistence cardinality exceeds a fixed limit");
    }
    Statement oversized_journal_group(
        database_,
        "SELECT 1 FROM manager_journal_entries "
        "GROUP BY operation_kind,operation_id,operation_generation "
        "HAVING COUNT(*) > ? LIMIT 1");
    oversized_journal_group.Bind(
        1,
        kMaximumJournalEntriesPerOperation);
    if (oversized_journal_group.Step()) {
      throw PersistenceIntegrityError(
          "per-operation journal count exceeds limit");
    }

    const auto operations = OperationsLocked();
    if (operations.size() != static_cast<std::size_t>(operation_count)) {
      throw PersistenceIntegrityError(
          "operation inventory cardinality is inconsistent");
    }
    std::map<std::string, const OperationRecord*> operation_by_key;
    std::int64_t bootstrap_count = 0;
    for (std::size_t index = 0; index < operations.size(); ++index) {
      const auto& operation = operations[index];
      if (operation.created_sequence() !=
          static_cast<std::int64_t>(index + 1U)) {
        throw PersistenceIntegrityError(
            "operation creation sequence is not contiguous");
      }
      const auto expected_digest = OperationIdentityDigest(
          cell_id_,
          operation.ref().kind(),
          operation.ref().operation_id(),
          operation.ref().generation(),
          operation.request_id(),
          operation.manager_owner_epoch(),
          operation.intent_digest(),
          operation.expected_foundation_generation(),
          operation.predecessor_bundle_identity(),
          operation.predecessor_graph_revision(),
          operation.target_bundle_identity(),
          operation.target_graph_revision());
      if (operation.identity_digest() != expected_digest) {
        throw PersistenceIntegrityError(
            "operation identity digest mismatch");
      }
      if (operation.ref().kind() == OperationKind::kBootstrap) {
        ++bootstrap_count;
      }
      const auto key =
          std::string(ToString(operation.ref().kind())) + '\x1f' +
          operation.ref().operation_id() + '\x1f' +
          std::to_string(operation.ref().generation());
      operation_by_key.emplace(key, &operation);
      const auto entries = JournalLocked(operation.ref());
      if (entries.size() >
          static_cast<std::size_t>(kMaximumJournalEntriesPerOperation)) {
        throw PersistenceIntegrityError(
            "per-operation journal count exceeds limit");
      }
      auto previous = operation.identity_digest();
      for (std::size_t entry_index = 0;
           entry_index < entries.size();
           ++entry_index) {
        const auto& entry = entries[entry_index];
        if (entry.sequence() !=
            static_cast<std::int64_t>(entry_index + 1U) ||
            entry.payload_digest() !=
                RawDigest(std::span<const std::byte>(entry.payload())) ||
            entry.previous_entry_digest() != previous ||
            entry.entry_digest() != JournalEntryDigest(
                cell_id_,
                entry.ref(),
                entry.sequence(),
                entry.entry_id(),
                entry.event_kind(),
                entry.payload_digest(),
                entry.previous_entry_digest())) {
          throw PersistenceIntegrityError(
              "journal digest chain is inconsistent");
        }
        previous = entry.entry_digest();
      }
    }
    if (bootstrap_count > 1) {
      throw PersistenceIntegrityError(
          "more than one bootstrap operation exists");
    }

    const auto receipts = ReceiptsLocked();
    std::int64_t prior_generation = 0;
    std::string prior_bundle;
    std::string prior_graph;
    for (std::size_t index = 0; index < receipts.size(); ++index) {
      const auto& receipt = receipts[index];
      const auto sequence = static_cast<std::int64_t>(index + 1U);
      if (receipt.created_sequence() != sequence ||
          receipt.committed_generation() != sequence ||
          receipt.expected_generation() != prior_generation ||
          receipt.predecessor_bundle_identity() != prior_bundle ||
          receipt.predecessor_graph_revision() != prior_graph) {
        throw PersistenceIntegrityError(
            "foundation receipt chain is inconsistent");
      }
      if ((index == 0U &&
           receipt.operation_ref().kind() != OperationKind::kBootstrap) ||
          (index != 0U &&
           receipt.operation_ref().kind() == OperationKind::kBootstrap)) {
        throw PersistenceIntegrityError(
            "foundation bootstrap ordering is invalid");
      }
      const auto operation_key =
          std::string(ToString(receipt.operation_ref().kind())) + '\x1f' +
          receipt.operation_ref().operation_id() + '\x1f' +
          std::to_string(receipt.operation_ref().generation());
      const auto operation_found = operation_by_key.find(operation_key);
      if (operation_found == operation_by_key.end()) {
        throw PersistenceIntegrityError(
            "foundation receipt references a missing operation");
      }
      const auto& operation = *operation_found->second;
      FoundationAdvanceIntent intent(
          receipt.advance_id(),
          receipt.operation_ref(),
          receipt.operation_identity_digest());
      const auto intent_digest = FoundationIntentDigest(cell_id_, intent);
      if (receipt.operation_identity_digest() !=
              operation.identity_digest() ||
          receipt.target_bundle_identity() !=
              operation.target_bundle_identity() ||
          receipt.target_graph_revision() !=
              operation.target_graph_revision() ||
          receipt.expected_generation() !=
              operation.expected_foundation_generation() ||
          receipt.predecessor_bundle_identity() !=
              operation.predecessor_bundle_identity() ||
          receipt.predecessor_graph_revision() !=
              operation.predecessor_graph_revision() ||
          receipt.intent_digest() != intent_digest ||
          receipt.receipt_digest() != FoundationReceiptDigest(
              cell_id_,
              receipt.advance_id(),
              intent_digest,
              operation,
              receipt.committed_generation())) {
        throw PersistenceIntegrityError(
            "foundation receipt does not match its operation");
      }
      prior_generation = receipt.committed_generation();
      prior_bundle = receipt.target_bundle_identity();
      prior_graph = receipt.target_graph_revision();
    }

    Statement head(
        database_,
        "SELECT singleton,generation,bundle_identity,graph_revision,"
        "fixture_cas_id,receipt_digest,operation_kind,operation_id,"
        "operation_generation FROM fixture_cas_head");
    if (!head.Step() || head.Integer(0) != 1) {
      throw PersistenceIntegrityError(
          "foundation head singleton is invalid");
    }
    std::string head_receipt_digest;
    if (receipts.empty()) {
      if (head.Integer(1) != 0 || !head.Text(2).empty() ||
          !head.Text(3).empty() || !head.Text(4).empty() ||
          !head.Text(5).empty() || !head.Text(6).empty() ||
          !head.Text(7).empty() || head.Integer(8) != 0) {
        throw PersistenceIntegrityError(
            "empty foundation history has a non-empty head");
      }
    } else {
      const auto& last = receipts.back();
      if (head.Integer(1) != last.committed_generation() ||
          head.Text(2) != last.target_bundle_identity() ||
          head.Text(3) != last.target_graph_revision() ||
          head.Text(4) != last.advance_id() ||
          head.Text(5) != last.receipt_digest() ||
          head.Text(6) != ToString(last.operation_ref().kind()) ||
          head.Text(7) != last.operation_ref().operation_id() ||
          head.Integer(8) != last.operation_ref().generation()) {
        throw PersistenceIntegrityError(
            "foundation head does not match its last receipt");
      }
      head_receipt_digest = last.receipt_digest();
    }
    if (head.Step()) {
      throw PersistenceIntegrityError(
          "foundation head singleton is invalid");
    }
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    if (page_size <= 0 || page_count < 0 ||
        page_count >
            kMaximumMainDatabasePageImageBytes / page_size) {
      throw PersistenceIntegrityError(
          "SQLite main database page image exceeds its ceiling");
    }
    return IntegrityReport{
        cell_id_,
        std::string(kPersistenceFoundationSchemaId),
        kPersistenceFoundationSchemaVersion,
        RawDigest(detail::kEmbeddedPersistenceSchema),
        operation_count,
        journal_count,
        receipt_count,
        prior_generation,
        std::move(head_receipt_digest),
    };
  }

  [[nodiscard]] std::vector<OperationRecord> OperationsLocked() const {
    Statement rows(
        database_,
        "SELECT operation_kind,operation_id,operation_generation,request_id,"
        "manager_owner_epoch,intent_digest,expected_fixture_generation,"
        "predecessor_bundle_identity,predecessor_graph_revision,"
        "target_bundle_identity,target_graph_revision,identity_digest,"
        "created_sequence FROM manager_operations ORDER BY created_sequence");
    std::vector<OperationRecord> result;
    while (rows.Step()) {
      result.push_back(OperationFromRow(rows));
    }
    return result;
  }

  [[nodiscard]] std::vector<JournalEntry> JournalLocked(
      const OperationRef& ref) const {
    Statement rows(
        database_,
        "SELECT operation_kind,operation_id,operation_generation,sequence,"
        "entry_id,event_kind,payload,payload_digest,previous_entry_digest,"
        "entry_digest FROM manager_journal_entries "
        "WHERE operation_kind=? AND operation_id=? "
        "AND operation_generation=? ORDER BY sequence");
    rows.Bind(1, ToString(ref.kind()));
    rows.Bind(2, ref.operation_id());
    rows.Bind(3, ref.generation());
    std::vector<JournalEntry> result;
    while (rows.Step()) {
      result.push_back(JournalFromRow(rows));
    }
    return result;
  }

  [[nodiscard]] std::vector<FoundationReceipt> ReceiptsLocked() const {
    Statement rows(
        database_,
        "SELECT fixture_cas_id,fixture_cas_intent_digest,operation_kind,"
        "operation_id,operation_generation,operation_identity_digest,"
        "expected_fixture_generation,predecessor_bundle_identity,"
        "predecessor_graph_revision,committed_fixture_generation,"
        "target_bundle_identity,target_graph_revision,receipt_digest,"
        "created_sequence FROM fixture_cas_receipts "
        "ORDER BY committed_fixture_generation");
    std::vector<FoundationReceipt> result;
    while (rows.Step()) {
      result.push_back(ReceiptFromRow(rows));
    }
    return result;
  }

  mutable std::recursive_mutex mutex_;
  std::filesystem::path database_path_;
  std::string cell_id_;
  sqlite3* database_{};
  bool closed_{};
};

ManagerPersistence::ManagerPersistence(
    std::filesystem::path database_path,
    std::string cell_id)
    : impl_(std::make_unique<Impl>(
          std::move(database_path),
          std::move(cell_id))) {}

ManagerPersistence::~ManagerPersistence() = default;
ManagerPersistence::ManagerPersistence(ManagerPersistence&&) noexcept = default;
ManagerPersistence& ManagerPersistence::operator=(
    ManagerPersistence&&) noexcept = default;

const std::filesystem::path& ManagerPersistence::database_path()
    const noexcept {
  return impl_->database_path_;
}

const std::string& ManagerPersistence::cell_id() const noexcept {
  return impl_->cell_id_;
}

void ManagerPersistence::Close() {
  impl_->Close();
}

OperationWriteResult ManagerPersistence::EnsureBootstrap(
    const BootstrapIdentity& identity) {
  return impl_->EnsureOperation(
      OperationKind::kBootstrap,
      identity.bootstrap_id(),
      identity.generation(),
      {},
      {},
      identity.intent_digest(),
      0,
      {},
      {},
      identity.target_bundle_identity(),
      identity.target_graph_revision());
}

OperationWriteResult ManagerPersistence::EnsureApply(
    const ApplyIdentity& identity) {
  return impl_->EnsureOperation(
      OperationKind::kApply,
      identity.apply_id(),
      identity.generation(),
      identity.request_id(),
      identity.manager_owner_epoch(),
      identity.intent_digest(),
      identity.expected_foundation_generation(),
      identity.predecessor_bundle_identity(),
      identity.predecessor_graph_revision(),
      identity.target_bundle_identity(),
      identity.target_graph_revision());
}

std::optional<OperationRecord> ManagerPersistence::Operation(
    const OperationRef& ref) const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  return impl_->FindOperationLocked(ref);
}

std::vector<OperationRecord> ManagerPersistence::Operations() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  return impl_->OperationsLocked();
}

JournalAppendResult ManagerPersistence::AppendJournalEvent(
    const OperationRef& ref,
    const JournalEvent& event) {
  const auto payload_digest =
      RawDigest(std::span<const std::byte>(event.payload()));
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  const auto operation = impl_->FindOperationLocked(ref);
  if (!operation) {
    throw PersistenceConflictError(
        "journal event references an unknown operation");
  }
  Statement existing(
      impl_->database_,
      "SELECT operation_kind,operation_id,operation_generation,sequence,"
      "entry_id,event_kind,payload,payload_digest,previous_entry_digest,"
      "entry_digest FROM manager_journal_entries "
      "WHERE operation_kind=? AND operation_id=? "
      "AND operation_generation=? AND entry_id=?");
  existing.Bind(1, ToString(ref.kind()));
  existing.Bind(2, ref.operation_id());
  existing.Bind(3, ref.generation());
  existing.Bind(4, event.entry_id());
  if (existing.Step()) {
    auto entry = impl_->JournalFromRow(existing);
    if (entry.event_kind() != event.event_kind() ||
        entry.payload() != event.payload() ||
        entry.payload_digest() != payload_digest) {
      throw PersistenceConflictError(
          "journal entry ID already exists with different content");
    }
    transaction.Commit();
    return {std::move(entry), false};
  }
  Statement count(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_journal_entries "
      "WHERE operation_kind=? AND operation_id=? "
      "AND operation_generation=?");
  count.Bind(1, ToString(ref.kind()));
  count.Bind(2, ref.operation_id());
  count.Bind(3, ref.generation());
  if (!count.Step()) {
    throw PersistenceIntegrityError("journal count is missing");
  }
  const auto per_operation_count = count.Integer(0);
  if (per_operation_count >= kMaximumJournalEntriesPerOperation) {
    throw PersistenceLimitError(
        "per-operation journal limit is exhausted");
  }
  if (ScalarInteger(
          impl_->database_,
          "SELECT COUNT(*) FROM manager_journal_entries") >=
      kMaximumTotalJournalEntries) {
    throw PersistenceLimitError("total journal entry limit is exhausted");
  }
  std::string previous_digest = operation->identity_digest();
  Statement previous(
      impl_->database_,
      "SELECT entry_digest FROM manager_journal_entries "
      "WHERE operation_kind=? AND operation_id=? "
      "AND operation_generation=? ORDER BY sequence DESC LIMIT 1");
  previous.Bind(1, ToString(ref.kind()));
  previous.Bind(2, ref.operation_id());
  previous.Bind(3, ref.generation());
  if (previous.Step()) {
    previous_digest = previous.Text(0);
  }
  const auto sequence = per_operation_count + 1;
  const auto entry_digest = JournalEntryDigest(
      impl_->cell_id_,
      ref,
      sequence,
      event.entry_id(),
      event.event_kind(),
      payload_digest,
      previous_digest);
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_journal_entries ("
      "operation_kind,operation_id,operation_generation,sequence,entry_id,"
      "event_kind,payload,payload_digest,previous_entry_digest,entry_digest"
      ") VALUES (?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, ToString(ref.kind()));
  insert.Bind(2, ref.operation_id());
  insert.Bind(3, ref.generation());
  insert.Bind(4, sequence);
  insert.Bind(5, event.entry_id());
  insert.Bind(6, event.event_kind());
  insert.Bind(7, std::span<const std::byte>(event.payload()));
  insert.Bind(8, payload_digest);
  insert.Bind(9, previous_digest);
  insert.Bind(10, entry_digest);
  static_cast<void>(insert.Step());
  Statement inserted(
      impl_->database_,
      "SELECT operation_kind,operation_id,operation_generation,sequence,"
      "entry_id,event_kind,payload,payload_digest,previous_entry_digest,"
      "entry_digest FROM manager_journal_entries "
      "WHERE operation_kind=? AND operation_id=? "
      "AND operation_generation=? AND sequence=?");
  inserted.Bind(1, ToString(ref.kind()));
  inserted.Bind(2, ref.operation_id());
  inserted.Bind(3, ref.generation());
  inserted.Bind(4, sequence);
  if (!inserted.Step()) {
    throw PersistenceIntegrityError(
        "inserted journal entry cannot be read back");
  }
  auto result = impl_->JournalFromRow(inserted);
  transaction.Commit();
  return {std::move(result), true};
}

std::vector<JournalEntry> ManagerPersistence::Journal(
    const OperationRef& ref) const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  return impl_->JournalLocked(ref);
}

FoundationAdvanceResult ManagerPersistence::AdvanceFoundationHead(
    const FoundationAdvanceIntent& intent) {
  const auto intent_digest =
      FoundationIntentDigest(impl_->cell_id_, intent);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Statement existing(
      impl_->database_,
      "SELECT fixture_cas_id,fixture_cas_intent_digest,operation_kind,"
      "operation_id,operation_generation,operation_identity_digest,"
      "expected_fixture_generation,predecessor_bundle_identity,"
      "predecessor_graph_revision,committed_fixture_generation,"
      "target_bundle_identity,target_graph_revision,receipt_digest,"
      "created_sequence FROM fixture_cas_receipts WHERE fixture_cas_id=?");
  existing.Bind(1, intent.advance_id());
  if (existing.Step()) {
    auto receipt = impl_->ReceiptFromRow(existing);
    if (receipt.intent_digest() != intent_digest ||
        !(receipt.operation_ref() == intent.operation_ref()) ||
        receipt.operation_identity_digest() !=
            intent.operation_identity_digest()) {
      throw PersistenceConflictError(
          "foundation advance ID already exists with different content");
    }
    transaction.Commit();
    return {std::move(receipt), false};
  }
  const auto operation =
      impl_->FindOperationLocked(intent.operation_ref());
  if (!operation) {
    throw PersistenceConflictError(
        "foundation advance references an unknown operation");
  }
  if (operation->identity_digest() !=
      intent.operation_identity_digest()) {
    throw PersistenceConflictError(
        "foundation advance operation identity digest mismatch");
  }
  if (operation->expected_foundation_generation() ==
      std::numeric_limits<std::int64_t>::max()) {
    throw PersistenceLimitError(
        "foundation generation exhausted signed SQLite int64");
  }
  Statement head(
      impl_->database_,
      "SELECT generation,bundle_identity,graph_revision "
      "FROM fixture_cas_head WHERE singleton=1");
  if (!head.Step()) {
    throw PersistenceIntegrityError(
        "foundation head singleton is missing");
  }
  if (head.Integer(0) != operation->expected_foundation_generation() ||
      head.Text(1) != operation->predecessor_bundle_identity() ||
      head.Text(2) != operation->predecessor_graph_revision()) {
    throw PersistenceConflictError(
        "foundation head precondition failed");
  }
  const auto receipt_count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM fixture_cas_receipts");
  if (receipt_count >= kMaximumFoundationReceipts) {
    throw PersistenceLimitError(
        "foundation receipt limit is exhausted");
  }
  if (operation->ref().kind() == OperationKind::kApply &&
      receipt_count == 0) {
    throw PersistenceConflictError(
        "the first foundation advance must be bootstrap");
  }
  const auto committed_generation =
      operation->expected_foundation_generation() + 1;
  const auto receipt_digest = FoundationReceiptDigest(
      impl_->cell_id_,
      intent.advance_id(),
      intent_digest,
      *operation,
      committed_generation);
  Statement insert(
      impl_->database_,
      "INSERT INTO fixture_cas_receipts ("
      "fixture_cas_id,fixture_cas_intent_digest,operation_kind,"
      "operation_id,operation_generation,operation_identity_digest,"
      "expected_fixture_generation,predecessor_bundle_identity,"
      "predecessor_graph_revision,committed_fixture_generation,"
      "target_bundle_identity,target_graph_revision,receipt_digest,"
      "created_sequence) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, intent.advance_id());
  insert.Bind(2, intent_digest);
  insert.Bind(3, ToString(operation->ref().kind()));
  insert.Bind(4, operation->ref().operation_id());
  insert.Bind(5, operation->ref().generation());
  insert.Bind(6, operation->identity_digest());
  insert.Bind(7, operation->expected_foundation_generation());
  insert.Bind(8, operation->predecessor_bundle_identity());
  insert.Bind(9, operation->predecessor_graph_revision());
  insert.Bind(10, committed_generation);
  insert.Bind(11, operation->target_bundle_identity());
  insert.Bind(12, operation->target_graph_revision());
  insert.Bind(13, receipt_digest);
  insert.Bind(14, receipt_count + 1);
  static_cast<void>(insert.Step());
  Statement update(
      impl_->database_,
      "UPDATE fixture_cas_head SET generation=?,bundle_identity=?,"
      "graph_revision=?,fixture_cas_id=?,receipt_digest=?,operation_kind=?,"
      "operation_id=?,operation_generation=? WHERE singleton=1 "
      "AND generation=? AND bundle_identity=? AND graph_revision=?");
  update.Bind(1, committed_generation);
  update.Bind(2, operation->target_bundle_identity());
  update.Bind(3, operation->target_graph_revision());
  update.Bind(4, intent.advance_id());
  update.Bind(5, receipt_digest);
  update.Bind(6, ToString(operation->ref().kind()));
  update.Bind(7, operation->ref().operation_id());
  update.Bind(8, operation->ref().generation());
  update.Bind(9, operation->expected_foundation_generation());
  update.Bind(10, operation->predecessor_bundle_identity());
  update.Bind(11, operation->predecessor_graph_revision());
  static_cast<void>(update.Step());
  if (update.Changes() != 1) {
    throw PersistenceConflictError(
        "foundation head advance lost its race");
  }
  FoundationReceipt receipt(
      intent.advance_id(),
      intent_digest,
      operation->ref(),
      operation->identity_digest(),
      operation->expected_foundation_generation(),
      operation->predecessor_bundle_identity(),
      operation->predecessor_graph_revision(),
      committed_generation,
      operation->target_bundle_identity(),
      operation->target_graph_revision(),
      receipt_digest,
      receipt_count + 1);
  transaction.Commit();
  return {std::move(receipt), true};
}

std::optional<FoundationHead> ManagerPersistence::FoundationHeadValue()
    const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Statement head(
      impl_->database_,
      "SELECT generation,bundle_identity,graph_revision,fixture_cas_id,"
      "receipt_digest,operation_kind,operation_id,operation_generation "
      "FROM fixture_cas_head WHERE singleton=1");
  if (!head.Step()) {
    throw PersistenceIntegrityError(
        "foundation head singleton is missing");
  }
  if (head.Integer(0) == 0) {
    return std::nullopt;
  }
  return FoundationHead(
      head.Integer(0),
      head.Text(1),
      head.Text(2),
      head.Text(3),
      head.Text(4),
      OperationRef(
          ParseOperationKind(head.Text(5)),
          head.Text(6),
          head.Integer(7)));
}

std::optional<FoundationReceipt>
ManagerPersistence::FoundationReceiptValue(
    const std::string& advance_id) const {
  ValidateIdentity(advance_id, "advance_id");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Statement receipt(
      impl_->database_,
      "SELECT fixture_cas_id,fixture_cas_intent_digest,operation_kind,"
      "operation_id,operation_generation,operation_identity_digest,"
      "expected_fixture_generation,predecessor_bundle_identity,"
      "predecessor_graph_revision,committed_fixture_generation,"
      "target_bundle_identity,target_graph_revision,receipt_digest,"
      "created_sequence FROM fixture_cas_receipts WHERE fixture_cas_id=?");
  receipt.Bind(1, advance_id);
  if (!receipt.Step()) {
    return std::nullopt;
  }
  return impl_->ReceiptFromRow(receipt);
}

std::vector<FoundationReceipt> ManagerPersistence::FoundationReceipts()
    const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  return impl_->ReceiptsLocked();
}

IntegrityReport ManagerPersistence::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  auto report = impl_->VerifyIntegrityLocked();
  transaction.Commit();
  return report;
}

std::string_view ToString(const OperationKind value) {
  switch (value) {
    case OperationKind::kBootstrap:
      return "bootstrap";
    case OperationKind::kApply:
      return "apply";
  }
  throw std::invalid_argument("invalid OperationKind");
}

}  // namespace manyforge::component_manager
