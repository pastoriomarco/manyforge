#include "manyforge_component_manager/manager_bootstrap_journal.hpp"

#include "embedded_manager_bootstrap_journal_schema.hpp"
#include "sqlite_store_startup_internal.hpp"
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

#include <openssl/evp.h>
#include <sqlite3.h>

#if SQLITE_VERSION_NUMBER < 3037000
#error "manager bootstrap journal requires SQLite 3.37.0 or newer"
#endif

#ifndef SQLITE_OPEN_NOFOLLOW
#error "manager bootstrap journal requires SQLITE_OPEN_NOFOLLOW"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace manyforge::component_manager {
namespace {

using manyforge::core::BootstrapGeneration;
using manyforge::core::BootstrapId;
using manyforge::core::CapabilityProviderBindingId;
using manyforge::core::CapabilityProviderBindingRevision;
using manyforge::core::CellId;
using manyforge::core::ContentDigest;
using manyforge::core::GraphRevision;
using manyforge::core::ProjectionExecutionHostBindingDTO;
using manyforge::core::ProjectionRevisionDTO;
using manyforge::core::RetainedBundleId;
using manyforge::core::RuntimeEpoch;
using manyforge::core::RuntimeUnitId;
using manyforge::core::SceneId;
using manyforge::core::SceneRevision;

constexpr std::string_view kDigestPrefix =
    "manyforge.component-manager-bootstrap-journal.v1";
constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kWalCheckpointTargetBytes =
    16LL * 1024LL * 1024LL;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 12;

[[noreturn]] void ThrowSqlite(
    sqlite3* database,
    const int result,
    const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL || result == SQLITE_TOOBIG) {
    throw ManagerBootstrapJournalLimitError(
        std::string(context) + ": SQLite storage ceiling exhausted: " +
        detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
    throw ManagerBootstrapJournalBusyError(
        std::string(context) + ": " + detail);
  }
  if (result == SQLITE_CONSTRAINT) {
    throw ManagerBootstrapJournalConflictError(
        std::string(context) + ": " + detail);
  }
  throw ManagerBootstrapJournalError(
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
      throw ManagerBootstrapJournalLimitError(detail);
    }
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
      throw ManagerBootstrapJournalBusyError(detail);
    }
    if (result == SQLITE_CONSTRAINT) {
      throw ManagerBootstrapJournalConflictError(detail);
    }
    throw ManagerBootstrapJournalError(detail);
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
      throw ManagerBootstrapJournalIntegrityError(
          "stored bootstrap-journal text column has an invalid SQLite type");
    }
    const auto* text = sqlite3_column_text(statement_, column);
    if (text == nullptr) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored bootstrap-journal text column is null");
    }
    return {
        reinterpret_cast<const char*>(text),
        static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)),
    };
  }

  [[nodiscard]] std::int64_t Integer(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored bootstrap-journal integer column has an invalid SQLite "
          "type");
    }
    return sqlite3_column_int64(statement_, column);
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
    throw ManagerBootstrapJournalIntegrityError(
        "SQLite scalar row is missing");
  }
  const auto result = statement.Integer(0);
  if (statement.Step()) {
    throw ManagerBootstrapJournalIntegrityError(
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
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap-journal schema object cardinality exceeds the "
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
      throw ManagerBootstrapJournalError(
          "open in-memory bootstrap schema validator: " + detail);
    }
    try {
      Exec(database, detail::kEmbeddedManagerBootstrapJournalSchema);
      auto objects = ReadSchemaObjects(database);
      if (objects.size() !=
          static_cast<std::size_t>(kExpectedSchemaObjectCardinality)) {
        throw ManagerBootstrapJournalError(
            "embedded bootstrap schema object cardinality is invalid");
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
      value.size() > kMaximumBootstrapJournalIdentifierBytes ||
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
    throw ManagerBootstrapJournalIntegrityError(
        "cannot allocate SHA-256 context");
  }
  const auto release = [&]() { EVP_MD_CTX_free(context); };
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &output_length) != 1) {
    release();
    throw ManagerBootstrapJournalIntegrityError(
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
    const std::string_view canonical_payload) {
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

std::string NativeBindingsProjection(
    const std::vector<ProjectionExecutionHostBindingDTO>& bindings) {
  std::string result{"["};
  for (std::size_t index = 0U; index < bindings.size(); ++index) {
    if (index != 0U) {
      result.push_back(',');
    }
    const auto& binding = bindings[index];
    result += JsonObject({
        StringField(
            "provider_binding_id", binding.provider_binding_id.value()),
        StringField(
            "provider_binding_revision",
            binding.provider_binding_revision.value()),
        StringField("runtime_epoch", binding.runtime_epoch.value()),
        StringField("runtime_unit_id", binding.runtime_unit_id.value()),
    });
  }
  result.push_back(']');
  return result;
}

struct EncodedIntent final {
  std::int64_t generation;
  std::string native_bindings_digest;
  std::string intent_digest;
};

EncodedIntent EncodeIntent(const ManagerBootstrapIntentDTO& intent) {
  if (!intent.valid()) {
    throw std::invalid_argument(
        "manager bootstrap intent is structurally invalid");
  }
  ValidateBoundedIdentity(intent.bootstrap_id.value(), "bootstrap_id");
  ValidateBoundedIdentity(intent.cell_id.value(), "cell_id");
  ValidateDigest(
      intent.canonical_empty_retained_bundle_id.value(),
      "canonical_empty_retained_bundle_id");
  ValidateDigest(
      intent.canonical_empty_graph_revision.value(),
      "canonical_empty_graph_revision");
  for (const auto& binding : intent.execution_host_bindings) {
    ValidateBoundedIdentity(
        binding.provider_binding_id.value(), "provider_binding_id");
    ValidateBoundedIdentity(
        binding.provider_binding_revision.value(),
        "provider_binding_revision");
    ValidateBoundedIdentity(
        binding.runtime_unit_id.value(), "runtime_unit_id");
    ValidateBoundedIdentity(binding.runtime_epoch.value(), "runtime_epoch");
  }
  const auto generation =
      CheckedSigned(intent.bootstrap_generation.value(), "bootstrap_generation");
  const auto bindings_projection =
      NativeBindingsProjection(intent.execution_host_bindings);
  const auto bindings_digest =
      SemanticDigest("execution-host-binding-set", bindings_projection);
  const auto intent_digest = SemanticDigest(
      "bootstrap-intent",
      JsonObject({
          StringField("bootstrap_id", intent.bootstrap_id.value()),
          IntegerField("bootstrap_generation", generation),
          StringField(
              "canonical_empty_graph_revision",
              intent.canonical_empty_graph_revision.value()),
          StringField(
              "canonical_empty_retained_bundle_id",
              intent.canonical_empty_retained_bundle_id.value()),
          StringField("cell_id", intent.cell_id.value()),
          StringField(
              "execution_host_binding_set_digest", bindings_digest),
      }));
  return {
      generation,
      std::move(bindings_digest),
      std::move(intent_digest),
  };
}

struct EncodedAcceptedTarget final {
  std::int64_t bootstrap_generation;
  std::int64_t scene_revision_id;
  std::string accepted_target_digest;
};

EncodedAcceptedTarget EncodeAcceptedTarget(
    const CellId& cell_id,
    const ManagerBootstrapAcceptedTargetDTO& accepted_target) {
  if (!accepted_target.valid()) {
    throw std::invalid_argument(
        "manager bootstrap accepted target is structurally invalid");
  }
  ValidateBoundedIdentity(
      accepted_target.bootstrap_id.value(), "bootstrap_id");
  ValidateDigest(
      accepted_target.canonical_empty_target.retained_bundle_id.value(),
      "candidate_retained_bundle_id");
  ValidateDigest(
      accepted_target.canonical_empty_target.graph_revision.value(),
      "candidate_graph_revision");
  ValidateBoundedIdentity(
      accepted_target.canonical_empty_target.scene_revision.scene_id.value(),
      "candidate_scene_id");
  ValidateDigest(
      accepted_target.canonical_empty_target.scene_revision.content_hash
          .value(),
      "candidate_scene_content_hash");
  const auto bootstrap_generation = CheckedSigned(
      accepted_target.bootstrap_generation.value(), "bootstrap_generation");
  const auto scene_revision_id = CheckedSigned(
      accepted_target.canonical_empty_target.scene_revision.revision_id,
      "candidate_scene_revision_id");
  return {
      bootstrap_generation,
      scene_revision_id,
      SemanticDigest(
          "accepted-target",
          JsonObject({
              StringField(
                  "bootstrap_id", accepted_target.bootstrap_id.value()),
              IntegerField(
                  "bootstrap_generation", bootstrap_generation),
              StringField("cell_id", cell_id.value()),
              StringField(
                  "candidate_graph_revision",
                  accepted_target.canonical_empty_target.graph_revision
                      .value()),
              StringField(
                  "candidate_retained_bundle_id",
                  accepted_target.canonical_empty_target.retained_bundle_id
                      .value()),
              StringField(
                  "candidate_scene_content_hash",
                  accepted_target.canonical_empty_target.scene_revision
                      .content_hash.value()),
              StringField(
                  "candidate_scene_id",
                  accepted_target.canonical_empty_target.scene_revision
                      .scene_id.value()),
              IntegerField(
                  "candidate_scene_revision_id", scene_revision_id),
          })),
  };
}

std::filesystem::path ValidateDatabasePath(std::filesystem::path path) {
  if (path.empty() || path == ":memory:") {
    throw std::invalid_argument(
        "a durable filesystem bootstrap-journal path is required");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error &&
      error != std::make_error_code(std::errc::no_such_file_or_directory)) {
    throw std::invalid_argument(
        "cannot inspect bootstrap-journal database path");
  }
  if (!error && std::filesystem::is_symlink(status)) {
    throw std::invalid_argument(
        "bootstrap-journal database path cannot be a symlink");
  }
  if (!error && std::filesystem::exists(status) &&
      !std::filesystem::is_regular_file(status)) {
    throw std::invalid_argument(
        "bootstrap-journal database path must be a regular file");
  }
  if (!error && std::filesystem::exists(status)) {
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
      throw std::invalid_argument(
          "cannot inspect bootstrap-journal database size");
    }
    if (size >
        static_cast<std::uintmax_t>(
            kMaximumBootstrapJournalDatabasePageImageBytes)) {
      throw ManagerBootstrapJournalLimitError(
          "bootstrap-journal database file exceeds its ceiling");
    }
  }
  error.clear();
  const auto parent =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  if (!std::filesystem::exists(parent, error) ||
      !std::filesystem::is_directory(parent, error)) {
    throw std::invalid_argument(
        "bootstrap-journal database parent must exist");
  }
  return path;
}

void ValidateBootstrapJournalMetadata(
    sqlite3* database,
    const CellId& cell_id) {
  Statement row(database, "SELECT * FROM bootstrap_journal_metadata");
  if (!row.Step()) {
    throw ManagerBootstrapJournalIntegrityError(
        "bootstrap-journal metadata singleton is missing");
  }
  if (row.Integer(0) != 1 ||
      row.Text(1) != kManagerBootstrapJournalSchemaId ||
      row.Integer(2) != kManagerBootstrapJournalSchemaVersion ||
      row.Text(3) !=
          RawDigest(detail::kEmbeddedManagerBootstrapJournalSchema)) {
    throw ManagerBootstrapJournalIntegrityError(
        "bootstrap-journal metadata mismatch");
  }
  if (row.Text(4) != cell_id.value()) {
    throw ManagerBootstrapJournalConflictError(
        "bootstrap journal belongs to a different cell");
  }
  if (row.Integer(5) !=
          static_cast<std::int64_t>(kMaximumBootstrapAttempts) ||
      row.Integer(6) !=
          static_cast<std::int64_t>(
              kMaximumBootstrapExecutionHostBindings) ||
      row.Integer(7) !=
          kMaximumBootstrapJournalDatabasePageImageBytes) {
    throw ManagerBootstrapJournalIntegrityError(
        "bootstrap-journal resource-limit metadata mismatch");
  }
  if (row.Step()) {
    throw ManagerBootstrapJournalIntegrityError(
        "bootstrap-journal metadata singleton is invalid");
  }
}

void ValidateBootstrapJournalIdentityAndSchema(
    sqlite3* database,
    const CellId& cell_id) {
  const auto existing = ReadSchemaObjects(database);
  const auto application_id =
      ScalarInteger(database, "PRAGMA application_id");
  const auto user_version =
      ScalarInteger(database, "PRAGMA user_version");
  if (existing.empty()) {
    if (application_id != 0 || user_version != 0) {
      throw ManagerBootstrapJournalIntegrityError(
          "schema-empty bootstrap-journal database has foreign SQLite "
          "identity");
    }
    return;
  }
  if (application_id != kManagerBootstrapJournalSqliteApplicationId) {
    throw ManagerBootstrapJournalIntegrityError(
        "bootstrap-journal SQLite application ID mismatch");
  }
  if (user_version != kManagerBootstrapJournalSchemaVersion) {
    throw ManagerBootstrapJournalIntegrityError(
        "bootstrap-journal SQLite user version mismatch");
  }
  if (existing != ExpectedSchemaObjects()) {
    throw ManagerBootstrapJournalIntegrityError(
        "SQLite schema objects differ from the versioned bootstrap "
        "artifact");
  }
  ValidateBootstrapJournalMetadata(database, cell_id);
}

void PreflightBootstrapJournal(
    const std::filesystem::path& database_path,
    const CellId& cell_id) {
  try {
    static_cast<void>(persistence::PreflightExistingSqliteReadOnly(
        database_path,
        {
            static_cast<std::uintmax_t>(
                kMaximumBootstrapJournalDatabasePageImageBytes),
            static_cast<std::uintmax_t>(
                kMaximumBootstrapJournalDatabasePageImageBytes),
        },
        [&cell_id](sqlite3* database) {
          ValidateBootstrapJournalIdentityAndSchema(database, cell_id);
        }));
  } catch (const persistence::SqliteReadOnlyPreflightError& error) {
    throw ManagerBootstrapJournalIntegrityError(
        "bootstrap-journal read-only preflight failed: " +
        std::string(error.what()));
  }
}

}  // namespace

bool ManagerBootstrapIntentDTO::valid() const noexcept {
  if (bootstrap_id.empty() || !bootstrap_generation.nonzero() ||
      cell_id.empty() || canonical_empty_retained_bundle_id.empty() ||
      canonical_empty_graph_revision.empty() ||
      execution_host_bindings.size() >
          kMaximumBootstrapExecutionHostBindings) {
    return false;
  }
  for (std::size_t index = 0U; index < execution_host_bindings.size();
       ++index) {
    if (!execution_host_bindings[index].valid()) {
      return false;
    }
    if (index != 0U &&
        !manyforge::core::CanonicalProjectionNativeBindingLess(
            execution_host_bindings[index - 1U],
            execution_host_bindings[index])) {
      return false;
    }
  }
  return true;
}

bool ManagerBootstrapAcceptedTargetDTO::valid() const noexcept {
  return !bootstrap_id.empty() && bootstrap_generation.nonzero() &&
      canonical_empty_target.valid();
}

bool ManagerBootstrapJournalSnapshot::valid() const noexcept {
  if (cell_id.empty()) {
    return false;
  }
  switch (phase) {
    case ManagerBootstrapJournalPhase::kEmpty:
      return !intent.has_value() && !accepted_target.has_value();
    case ManagerBootstrapJournalPhase::kIntentPersisted:
      return intent.has_value() && intent->intent.valid() &&
          intent->intent.cell_id == cell_id &&
          !intent->intent_digest.empty() && !accepted_target.has_value();
    case ManagerBootstrapJournalPhase::kAcceptedTargetPersisted:
      return intent.has_value() && intent->intent.valid() &&
          intent->intent.cell_id == cell_id &&
          !intent->intent_digest.empty() && accepted_target.has_value() &&
          accepted_target->accepted_target.valid() &&
          !accepted_target->accepted_target_digest.empty() &&
          accepted_target->accepted_target.bootstrap_id ==
              intent->intent.bootstrap_id &&
          accepted_target->accepted_target.bootstrap_generation ==
              intent->intent.bootstrap_generation &&
          accepted_target->accepted_target.canonical_empty_target
                  .retained_bundle_id ==
              intent->intent.canonical_empty_retained_bundle_id &&
          accepted_target->accepted_target.canonical_empty_target
                  .graph_revision ==
              intent->intent.canonical_empty_graph_revision;
  }
  return false;
}

std::string_view ToString(
    const ManagerBootstrapJournalPhase phase) noexcept {
  switch (phase) {
    case ManagerBootstrapJournalPhase::kEmpty:
      return "empty";
    case ManagerBootstrapJournalPhase::kIntentPersisted:
      return "intent_persisted";
    case ManagerBootstrapJournalPhase::kAcceptedTargetPersisted:
      return "accepted_target_persisted";
  }
  return {};
}

class SqliteManagerBootstrapJournal::Impl final {
 public:
  Impl(std::filesystem::path database_path, CellId cell_id)
      : database_path_(ValidateDatabasePath(std::move(database_path))),
        cell_id_(std::move(cell_id)) {
    ValidateBoundedIdentity(cell_id_.value(), "cell_id");
    if (sqlite3_libversion_number() < 3037000) {
      throw ManagerBootstrapJournalError(
          "manyforge_component_manager requires SQLite 3.37.0 or newer");
    }
    auto startup_lock =
        sqlite_startup_internal::TryAcquireManagerSqliteStoreStartupLock(
            std::chrono::milliseconds(kBusyTimeoutMilliseconds));
    if (!startup_lock.owns_lock()) {
      throw ManagerBootstrapJournalBusyError(
          "timed out waiting for process-local bootstrap-journal startup");
    }
    PreflightBootstrapJournal(database_path_, cell_id_);
    const int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_NOFOLLOW;
    const auto result = sqlite3_open_v2(
        database_path_.string().c_str(), &database_, flags, nullptr);
    if (result != SQLITE_OK) {
      const std::string error =
          database_ == nullptr ? sqlite3_errstr(result) :
                                 sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      throw ManagerBootstrapJournalError(
          "cannot open bootstrap-journal database: " + error);
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
      ValidateBootstrapJournalIdentityAndSchema(database_, cell_id_);
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

  ~Impl() {
    try {
      Close();
    } catch (...) {
    }
  }

  void RequireOpen() const {
    if (closed_ || database_ == nullptr) {
      throw ManagerBootstrapJournalClosedError(
          "manager bootstrap journal is closed");
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
      ThrowSqlite(database_, result, "close bootstrap-journal database");
    }
    database_ = nullptr;
    closed_ = true;
  }

  void Configure() {
    Exec(database_, "PRAGMA foreign_keys = ON");
    if (ScalarInteger(database_, "PRAGMA foreign_keys") != 1) {
      throw ManagerBootstrapJournalIntegrityError(
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
          throw ManagerBootstrapJournalIntegrityError(
              "SQLite WAL journal mode is required");
        }
        break;
      } catch (const ManagerBootstrapJournalBusyError&) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    Exec(database_, "PRAGMA synchronous = FULL");
    if (ScalarInteger(database_, "PRAGMA synchronous") != 2) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite FULL synchronous mode is required");
    }
    Exec(database_, "PRAGMA trusted_schema = OFF");
    if (ScalarInteger(database_, "PRAGMA trusted_schema") != 0) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite untrusted-schema mode is required");
    }
    Exec(database_, "PRAGMA recursive_triggers = OFF");
    if (ScalarInteger(database_, "PRAGMA recursive_triggers") != 0) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite recursive triggers must remain disabled");
    }
    Exec(database_, "PRAGMA wal_autocheckpoint = 1000");
    if (ScalarInteger(database_, "PRAGMA wal_autocheckpoint") != 1000) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite WAL autocheckpoint policy is required");
    }
    const auto journal_size_limit = ScalarInteger(
        database_,
        "PRAGMA journal_size_limit = " +
            std::to_string(kWalCheckpointTargetBytes));
    if (journal_size_limit != kWalCheckpointTargetBytes) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite WAL journal-size policy is required");
    }
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_size <= 0) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite page size is invalid");
    }
    const auto maximum_pages = std::max<std::int64_t>(
        1,
        kMaximumBootstrapJournalDatabasePageImageBytes / page_size);
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    if (page_count < 0 || page_count > maximum_pages) {
      throw ManagerBootstrapJournalLimitError(
          "SQLite main database page image exceeds its ceiling");
    }
    const auto effective_maximum_pages = ScalarInteger(
        database_,
        "PRAGMA max_page_count = " + std::to_string(maximum_pages));
    if (effective_maximum_pages != maximum_pages) {
      throw ManagerBootstrapJournalLimitError(
          "SQLite main database page ceiling could not be enforced");
    }
    sqlite3_limit(database_, SQLITE_LIMIT_LENGTH, 1024 * 1024);
  }

  void InitializeOrValidateSchema() {
    std::lock_guard lock(mutex_);
    Transaction transaction(database_, "BEGIN EXCLUSIVE");
    const auto existing = ReadSchemaObjects(database_);
    if (existing.empty()) {
      if (ScalarInteger(database_, "PRAGMA application_id") != 0 ||
          ScalarInteger(database_, "PRAGMA user_version") != 0) {
        throw ManagerBootstrapJournalIntegrityError(
            "schema-empty bootstrap-journal database has foreign SQLite "
            "identity");
      }
      Exec(database_, detail::kEmbeddedManagerBootstrapJournalSchema);
      Statement metadata(
          database_,
          "INSERT INTO bootstrap_journal_metadata ("
          "singleton,schema_id,schema_version,schema_artifact_digest,cell_id,"
          "maximum_bootstrap_attempts,"
          "maximum_execution_host_bindings_per_attempt,"
          "maximum_main_database_page_image_bytes"
          ") VALUES (1,?,?,?,?,?,?,?)");
      metadata.Bind(1, kManagerBootstrapJournalSchemaId);
      metadata.Bind(2, kManagerBootstrapJournalSchemaVersion);
      metadata.Bind(
          3, RawDigest(detail::kEmbeddedManagerBootstrapJournalSchema));
      metadata.Bind(4, cell_id_.value());
      metadata.Bind(
          5,
          static_cast<std::int64_t>(kMaximumBootstrapAttempts));
      metadata.Bind(
          6,
          static_cast<std::int64_t>(
              kMaximumBootstrapExecutionHostBindings));
      metadata.Bind(7, kMaximumBootstrapJournalDatabasePageImageBytes);
      static_cast<void>(metadata.Step());
    } else if (
        !existing.contains({"table", "bootstrap_journal_metadata"})) {
      throw ManagerBootstrapJournalIntegrityError(
          "non-empty database lacks bootstrap-journal metadata");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite schema objects differ from the versioned bootstrap "
          "artifact");
    }
    transaction.Commit();
  }

  void ValidateMetadataLocked() const {
    if (ScalarInteger(database_, "PRAGMA application_id") !=
        kManagerBootstrapJournalSqliteApplicationId) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap-journal SQLite application ID mismatch");
    }
    if (ScalarInteger(database_, "PRAGMA user_version") !=
        kManagerBootstrapJournalSchemaVersion) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap-journal SQLite user version mismatch");
    }
    ValidateBootstrapJournalMetadata(database_, cell_id_);
  }

  std::optional<BootstrapId> StoredBootstrapIdLocked() const {
    Statement row(
        database_,
        "SELECT bootstrap_id FROM manager_bootstrap_intents "
        "ORDER BY bootstrap_generation LIMIT 1");
    if (!row.Step()) {
      return std::nullopt;
    }
    auto bootstrap_id = BootstrapId(row.Text(0));
    if (row.Step()) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap identity query returned multiple rows");
    }
    return bootstrap_id;
  }

  std::optional<std::int64_t> GenerationHighWaterLocked(
      const BootstrapId& bootstrap_id) const {
    Statement row(
        database_,
        "SELECT bootstrap_generation FROM manager_bootstrap_intents "
        "WHERE bootstrap_id=? ORDER BY bootstrap_generation DESC LIMIT 1");
    row.Bind(1, bootstrap_id.value());
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto generation = row.Integer(0);
    if (generation <= 0 || row.Step()) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap generation high-water row is invalid");
    }
    return generation;
  }

  std::optional<PersistedManagerBootstrapIntent> IntentLocked(
      const BootstrapId& requested_bootstrap_id,
      const std::int64_t requested_generation) const {
    Statement row(
        database_,
        "SELECT bootstrap_id,bootstrap_generation,"
        "canonical_empty_retained_bundle_id,"
        "canonical_empty_graph_revision,"
        "execution_host_binding_set_digest,intent_digest "
        "FROM manager_bootstrap_intents "
        "WHERE bootstrap_id=? AND bootstrap_generation=?");
    row.Bind(1, requested_bootstrap_id.value());
    row.Bind(2, requested_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto bootstrap_id = row.Text(0);
    const auto generation = row.Integer(1);
    const auto retained_bundle_id = row.Text(2);
    const auto graph_revision = row.Text(3);
    const auto stored_binding_digest = row.Text(4);
    const auto stored_intent_digest = row.Text(5);
    if (row.Step()) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap intent identity is not unique");
    }
    if (generation <= 0) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored bootstrap generation is invalid");
    }

    Statement binding_rows(
        database_,
        "SELECT ordinal,provider_binding_id,provider_binding_revision,"
        "runtime_unit_id,runtime_epoch "
        "FROM manager_bootstrap_execution_host_bindings "
        "WHERE bootstrap_id=? AND bootstrap_generation=? "
        "ORDER BY ordinal LIMIT ?");
    binding_rows.Bind(
        1,
        requested_bootstrap_id.value());
    binding_rows.Bind(2, requested_generation);
    binding_rows.Bind(
        3,
        static_cast<std::int64_t>(
            kMaximumBootstrapExecutionHostBindings + 1U));
    std::vector<ProjectionExecutionHostBindingDTO> bindings;
    while (binding_rows.Step()) {
      if (bindings.size() >= kMaximumBootstrapExecutionHostBindings) {
        throw ManagerBootstrapJournalIntegrityError(
            "stored execution host binding count exceeds its ceiling");
      }
      if (binding_rows.Integer(0) !=
          static_cast<std::int64_t>(bindings.size())) {
        throw ManagerBootstrapJournalIntegrityError(
            "stored execution host binding ordinals are not contiguous");
      }
      bindings.push_back(ProjectionExecutionHostBindingDTO{
          CapabilityProviderBindingId(binding_rows.Text(1)),
          CapabilityProviderBindingRevision(binding_rows.Text(2)),
          RuntimeUnitId(binding_rows.Text(3)),
          RuntimeEpoch(binding_rows.Text(4)),
      });
    }

    ManagerBootstrapIntentDTO intent{
        BootstrapId(bootstrap_id),
        BootstrapGeneration(static_cast<std::uint64_t>(generation)),
        cell_id_,
        RetainedBundleId(retained_bundle_id),
        GraphRevision(graph_revision),
        std::move(bindings),
    };
    EncodedIntent encoded;
    try {
      encoded = EncodeIntent(intent);
    } catch (const std::exception&) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored bootstrap intent is invalid");
    }
    if (encoded.native_bindings_digest != stored_binding_digest ||
        encoded.intent_digest != stored_intent_digest) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored bootstrap intent digest is inconsistent");
    }
    return PersistedManagerBootstrapIntent{
        std::move(intent), ContentDigest(stored_intent_digest)};
  }

  std::optional<PersistedManagerBootstrapAcceptedTarget>
  AcceptedTargetLocked(
      const PersistedManagerBootstrapIntent& intent) const {
    Statement row(
        database_,
        "SELECT bootstrap_id,bootstrap_generation,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash,accepted_target_digest "
        "FROM manager_bootstrap_accepted_targets "
        "WHERE bootstrap_id=? AND bootstrap_generation=?");
    row.Bind(1, intent.intent.bootstrap_id.value());
    row.Bind(
        2,
        CheckedSigned(
            intent.intent.bootstrap_generation.value(),
            "bootstrap_generation"));
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto bootstrap_id = row.Text(0);
    const auto generation = row.Integer(1);
    const auto retained_bundle_id = row.Text(2);
    const auto graph_revision = row.Text(3);
    const auto scene_id = row.Text(4);
    const auto scene_revision_id = row.Integer(5);
    const auto scene_content_hash = row.Text(6);
    const auto stored_digest = row.Text(7);
    if (row.Step()) {
      throw ManagerBootstrapJournalIntegrityError(
          "accepted bootstrap target identity is not unique");
    }
    if (generation <= 0 || scene_revision_id <= 0) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored accepted bootstrap target generation is invalid");
    }
    ManagerBootstrapAcceptedTargetDTO accepted_target{
        BootstrapId(bootstrap_id),
        BootstrapGeneration(static_cast<std::uint64_t>(generation)),
        ProjectionRevisionDTO{
            RetainedBundleId(retained_bundle_id),
            GraphRevision(graph_revision),
            SceneRevision{
                SceneId(scene_id),
                static_cast<std::uint64_t>(scene_revision_id),
                ContentDigest(scene_content_hash),
            },
        },
    };
    EncodedAcceptedTarget encoded;
    try {
      encoded = EncodeAcceptedTarget(cell_id_, accepted_target);
    } catch (const std::exception&) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored accepted bootstrap target is invalid");
    }
    if (accepted_target.bootstrap_id != intent.intent.bootstrap_id ||
        accepted_target.bootstrap_generation !=
            intent.intent.bootstrap_generation ||
        accepted_target.canonical_empty_target.retained_bundle_id !=
            intent.intent.canonical_empty_retained_bundle_id ||
        accepted_target.canonical_empty_target.graph_revision !=
            intent.intent.canonical_empty_graph_revision ||
        encoded.accepted_target_digest != stored_digest) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored accepted bootstrap target is inconsistent");
    }
    return PersistedManagerBootstrapAcceptedTarget{
        std::move(accepted_target), ContentDigest(stored_digest)};
  }

  ManagerBootstrapJournalSnapshot SnapshotLocked() const {
    std::optional<PersistedManagerBootstrapIntent> intent;
    std::optional<PersistedManagerBootstrapAcceptedTarget> accepted;
    const auto bootstrap_id = StoredBootstrapIdLocked();
    if (bootstrap_id.has_value()) {
      const auto high_water = GenerationHighWaterLocked(*bootstrap_id);
      if (!high_water.has_value()) {
        throw ManagerBootstrapJournalIntegrityError(
            "bootstrap identity exists without a generation high-water mark");
      }
      intent = IntentLocked(*bootstrap_id, *high_water);
      if (!intent.has_value()) {
        throw ManagerBootstrapJournalIntegrityError(
            "bootstrap generation high-water intent is missing");
      }
      accepted = AcceptedTargetLocked(*intent);
    }
    ManagerBootstrapJournalSnapshot snapshot{
        cell_id_,
        ManagerBootstrapJournalPhase::kEmpty,
        intent,
        accepted,
    };
    if (accepted.has_value()) {
      snapshot.phase =
          ManagerBootstrapJournalPhase::kAcceptedTargetPersisted;
    } else if (intent.has_value()) {
      snapshot.phase = ManagerBootstrapJournalPhase::kIntentPersisted;
    }
    if (!snapshot.valid()) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap-journal snapshot is internally inconsistent");
    }
    return snapshot;
  }

  ManagerBootstrapJournalIntegrityReport VerifyIntegrityLocked() const {
    Statement integrity(database_, "PRAGMA integrity_check");
    if (!integrity.Step() || integrity.Text(0) != "ok" ||
        integrity.Step()) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap-journal SQLite integrity_check failed");
    }
    Statement foreign_keys(database_, "PRAGMA foreign_key_check");
    if (foreign_keys.Step()) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap-journal SQLite foreign-key integrity check failed");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw ManagerBootstrapJournalIntegrityError(
          "SQLite schema objects differ from the versioned bootstrap "
          "artifact");
    }
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_count < 0 || page_size <= 0 ||
        page_count >
            kMaximumBootstrapJournalDatabasePageImageBytes / page_size) {
      throw ManagerBootstrapJournalLimitError(
          "SQLite main database page image exceeds its ceiling");
    }
    const auto intent_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_bootstrap_intents");
    const auto binding_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_bootstrap_execution_host_bindings");
    const auto accepted_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_bootstrap_accepted_targets");
    constexpr auto kMaximumBindingRows =
        static_cast<std::int64_t>(kMaximumBootstrapAttempts) *
        static_cast<std::int64_t>(
            kMaximumBootstrapExecutionHostBindings);
    if (intent_count < 0 ||
        intent_count >
            static_cast<std::int64_t>(kMaximumBootstrapAttempts) ||
        accepted_count < 0 || accepted_count > intent_count ||
        binding_count < 0 ||
        binding_count > kMaximumBindingRows ||
        (intent_count == 0 && (binding_count != 0 || accepted_count != 0))) {
      throw ManagerBootstrapJournalIntegrityError(
          "stored bootstrap-journal cardinality is invalid");
    }

    Statement attempts(
        database_,
        "SELECT bootstrap_id,bootstrap_generation "
        "FROM manager_bootstrap_intents "
        "ORDER BY bootstrap_generation LIMIT ?");
    attempts.Bind(
        1,
        static_cast<std::int64_t>(kMaximumBootstrapAttempts + 1U));
    std::optional<BootstrapId> bootstrap_id;
    std::int64_t expected_generation = 1;
    std::int64_t observed_binding_count = 0;
    std::int64_t observed_accepted_count = 0;
    while (attempts.Step()) {
      const auto row_bootstrap_id = BootstrapId(attempts.Text(0));
      const auto row_generation = attempts.Integer(1);
      if (!bootstrap_id.has_value()) {
        bootstrap_id = row_bootstrap_id;
      }
      if (row_bootstrap_id != *bootstrap_id ||
          row_generation != expected_generation) {
        throw ManagerBootstrapJournalIntegrityError(
            "bootstrap attempts do not form one contiguous generation "
            "sequence");
      }
      const auto intent =
          IntentLocked(row_bootstrap_id, row_generation);
      if (!intent.has_value()) {
        throw ManagerBootstrapJournalIntegrityError(
            "bootstrap attempt inventory references a missing intent");
      }
      observed_binding_count += static_cast<std::int64_t>(
          intent->intent.execution_host_bindings.size());
      if (AcceptedTargetLocked(*intent).has_value()) {
        ++observed_accepted_count;
      }
      ++expected_generation;
    }
    if (expected_generation - 1 != intent_count ||
        observed_binding_count != binding_count ||
        observed_accepted_count != accepted_count) {
      throw ManagerBootstrapJournalIntegrityError(
          "bootstrap attempt inventory is inconsistent");
    }

    const auto snapshot = SnapshotLocked();
    std::optional<BootstrapGeneration> high_water;
    if (bootstrap_id.has_value()) {
      high_water = BootstrapGeneration(
          static_cast<std::uint64_t>(expected_generation - 1));
    }
    return {
        cell_id_,
        std::string(kManagerBootstrapJournalSchemaId),
        kManagerBootstrapJournalSchemaVersion,
        RawDigest(detail::kEmbeddedManagerBootstrapJournalSchema),
        intent_count,
        binding_count,
        accepted_count,
        bootstrap_id,
        high_water,
        snapshot.phase,
    };
  }

  std::filesystem::path database_path_;
  CellId cell_id_;
  sqlite3* database_{};
  mutable std::mutex mutex_;
  bool closed_{};
};

SqliteManagerBootstrapJournal::SqliteManagerBootstrapJournal(
    std::filesystem::path database_path,
    CellId cell_id)
    : impl_(std::make_unique<Impl>(
          std::move(database_path), std::move(cell_id))) {}

SqliteManagerBootstrapJournal::~SqliteManagerBootstrapJournal() = default;

SqliteManagerBootstrapJournal::SqliteManagerBootstrapJournal(
    SqliteManagerBootstrapJournal&&) noexcept = default;

SqliteManagerBootstrapJournal&
SqliteManagerBootstrapJournal::operator=(
    SqliteManagerBootstrapJournal&&) noexcept = default;

const std::filesystem::path&
SqliteManagerBootstrapJournal::database_path() const noexcept {
  return impl_->database_path_;
}

const CellId& SqliteManagerBootstrapJournal::cell_id() const noexcept {
  return impl_->cell_id_;
}

ManagerBootstrapIntentWriteResult
SqliteManagerBootstrapJournal::EnsureIntent(
    const ManagerBootstrapIntentDTO& intent) {
  const auto encoded = EncodeIntent(intent);
  if (intent.cell_id != impl_->cell_id_) {
    throw ManagerBootstrapJournalConflictError(
        "bootstrap intent belongs to a different cell");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  if (const auto existing =
          impl_->IntentLocked(intent.bootstrap_id, encoded.generation);
      existing.has_value()) {
    if (existing->intent != intent ||
        existing->intent_digest.value() != encoded.intent_digest) {
      throw ManagerBootstrapJournalConflictError(
          "bootstrap intent conflicts with the durable intent");
    }
    transaction.Commit();
    return {*existing, false};
  }

  const auto stored_bootstrap_id = impl_->StoredBootstrapIdLocked();
  if (stored_bootstrap_id.has_value() &&
      *stored_bootstrap_id != intent.bootstrap_id) {
    throw ManagerBootstrapJournalConflictError(
        "bootstrap intent changes the durable bootstrap identity");
  }
  const auto high_water =
      impl_->GenerationHighWaterLocked(intent.bootstrap_id);
  if (high_water.has_value() &&
      *high_water == std::numeric_limits<std::int64_t>::max()) {
    throw ManagerBootstrapJournalLimitError(
        "bootstrap generation high-water mark is exhausted");
  }
  const auto expected_generation =
      high_water.has_value() ? *high_water + 1 : 1;
  if (encoded.generation != expected_generation) {
    throw ManagerBootstrapJournalConflictError(
        "bootstrap generation must advance the durable high-water mark "
        "by exactly one");
  }
  const auto attempt_count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_bootstrap_intents");
  if (attempt_count >=
      static_cast<std::int64_t>(kMaximumBootstrapAttempts)) {
    throw ManagerBootstrapJournalLimitError(
        "bootstrap attempt inventory reached its fixed limit");
  }

  Statement insert(
      impl_->database_,
      "INSERT INTO manager_bootstrap_intents ("
      "bootstrap_id,bootstrap_generation,"
      "canonical_empty_retained_bundle_id,"
      "canonical_empty_graph_revision,"
      "execution_host_binding_set_digest,intent_digest"
      ") VALUES (?,?,?,?,?,?)");
  insert.Bind(1, intent.bootstrap_id.value());
  insert.Bind(2, encoded.generation);
  insert.Bind(3, intent.canonical_empty_retained_bundle_id.value());
  insert.Bind(4, intent.canonical_empty_graph_revision.value());
  insert.Bind(5, encoded.native_bindings_digest);
  insert.Bind(6, encoded.intent_digest);
  static_cast<void>(insert.Step());

  for (std::size_t index = 0U;
       index < intent.execution_host_bindings.size();
       ++index) {
    const auto& binding = intent.execution_host_bindings[index];
    Statement binding_insert(
        impl_->database_,
        "INSERT INTO manager_bootstrap_execution_host_bindings ("
        "bootstrap_id,bootstrap_generation,ordinal,provider_binding_id,"
        "provider_binding_revision,runtime_unit_id,runtime_epoch"
        ") VALUES (?,?,?,?,?,?,?)");
    binding_insert.Bind(1, intent.bootstrap_id.value());
    binding_insert.Bind(2, encoded.generation);
    binding_insert.Bind(3, static_cast<std::int64_t>(index));
    binding_insert.Bind(4, binding.provider_binding_id.value());
    binding_insert.Bind(5, binding.provider_binding_revision.value());
    binding_insert.Bind(6, binding.runtime_unit_id.value());
    binding_insert.Bind(7, binding.runtime_epoch.value());
    static_cast<void>(binding_insert.Step());
  }
  const auto stored =
      impl_->IntentLocked(intent.bootstrap_id, encoded.generation);
  if (!stored.has_value() || stored->intent != intent ||
      stored->intent_digest.value() != encoded.intent_digest) {
    throw ManagerBootstrapJournalIntegrityError(
        "inserted bootstrap intent cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

ManagerBootstrapAcceptedTargetWriteResult
SqliteManagerBootstrapJournal::RecordAcceptedTarget(
    const ManagerBootstrapAcceptedTargetDTO& accepted_target) {
  const auto encoded =
      EncodeAcceptedTarget(impl_->cell_id_, accepted_target);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto intent = impl_->IntentLocked(
      accepted_target.bootstrap_id, encoded.bootstrap_generation);
  if (!intent.has_value()) {
    throw ManagerBootstrapJournalConflictError(
        "accepted bootstrap target cannot precede the durable intent");
  }
  if (accepted_target.bootstrap_id != intent->intent.bootstrap_id ||
      accepted_target.bootstrap_generation !=
          intent->intent.bootstrap_generation ||
      accepted_target.canonical_empty_target.retained_bundle_id !=
          intent->intent.canonical_empty_retained_bundle_id ||
      accepted_target.canonical_empty_target.graph_revision !=
          intent->intent.canonical_empty_graph_revision) {
    throw ManagerBootstrapJournalConflictError(
        "accepted bootstrap target does not match the durable intent");
  }
  if (const auto existing = impl_->AcceptedTargetLocked(*intent);
      existing.has_value()) {
    if (existing->accepted_target != accepted_target ||
        existing->accepted_target_digest.value() !=
            encoded.accepted_target_digest) {
      throw ManagerBootstrapJournalConflictError(
          "accepted bootstrap target conflicts with the durable target");
    }
    transaction.Commit();
    return {*existing, false};
  }

  const auto high_water =
      impl_->GenerationHighWaterLocked(accepted_target.bootstrap_id);
  if (!high_water.has_value() ||
      *high_water != encoded.bootstrap_generation) {
    throw ManagerBootstrapJournalConflictError(
        "accepted bootstrap target belongs to a stale generation");
  }

  Statement insert(
      impl_->database_,
      "INSERT INTO manager_bootstrap_accepted_targets ("
      "bootstrap_id,bootstrap_generation,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id,"
      "candidate_scene_content_hash,accepted_target_digest"
      ") VALUES (?,?,?,?,?,?,?,?)");
  insert.Bind(1, accepted_target.bootstrap_id.value());
  insert.Bind(2, encoded.bootstrap_generation);
  insert.Bind(
      3, accepted_target.canonical_empty_target.retained_bundle_id.value());
  insert.Bind(
      4, accepted_target.canonical_empty_target.graph_revision.value());
  insert.Bind(
      5,
      accepted_target.canonical_empty_target.scene_revision.scene_id.value());
  insert.Bind(6, encoded.scene_revision_id);
  insert.Bind(
      7,
      accepted_target.canonical_empty_target.scene_revision.content_hash
          .value());
  insert.Bind(8, encoded.accepted_target_digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->AcceptedTargetLocked(*intent);
  if (!stored.has_value() ||
      stored->accepted_target != accepted_target ||
      stored->accepted_target_digest.value() !=
          encoded.accepted_target_digest) {
    throw ManagerBootstrapJournalIntegrityError(
        "inserted accepted bootstrap target cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<BootstrapGeneration>
SqliteManagerBootstrapJournal::ReadBootstrapGenerationHighWater(
    const BootstrapId& bootstrap_id) const {
  ValidateBoundedIdentity(bootstrap_id.value(), "bootstrap_id");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto stored_bootstrap_id = impl_->StoredBootstrapIdLocked();
  if (stored_bootstrap_id.has_value() &&
      *stored_bootstrap_id != bootstrap_id) {
    throw ManagerBootstrapJournalConflictError(
        "bootstrap high-water query names a different bootstrap identity");
  }
  const auto high_water =
      impl_->GenerationHighWaterLocked(bootstrap_id);
  transaction.Commit();
  if (!high_water.has_value()) {
    return std::nullopt;
  }
  return BootstrapGeneration(
      static_cast<std::uint64_t>(*high_water));
}

ManagerBootstrapJournalSnapshot
SqliteManagerBootstrapJournal::ReadSnapshot() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto snapshot = impl_->SnapshotLocked();
  transaction.Commit();
  return snapshot;
}

ManagerBootstrapJournalIntegrityReport
SqliteManagerBootstrapJournal::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto report = impl_->VerifyIntegrityLocked();
  transaction.Commit();
  return report;
}

void SqliteManagerBootstrapJournal::Close() {
  impl_->Close();
}

}  // namespace manyforge::component_manager
