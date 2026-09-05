#include "manyforge_headless/event_historian.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr int kHistorianApplicationId = 0x4D464548;
constexpr int kHistorianUserVersion = 1;
constexpr int kBusyTimeoutMilliseconds = 1000;
constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumRecordRetention = 1000000U;
constexpr std::int64_t kMaximumDatabasePageImageBytes = 512LL * 1024LL * 1024LL;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE historian_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  schema_id TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  record_retention INTEGER NOT NULL
) STRICT;
CREATE TABLE historian_events (
  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id TEXT NOT NULL UNIQUE,
  component TEXT NOT NULL,
  event_type TEXT NOT NULL,
  severity TEXT NOT NULL,
  occurred_at_ns INTEGER NOT NULL,
  occurred_at_wall TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  payload_digest TEXT NOT NULL,
  event_digest TEXT NOT NULL
) STRICT;
CREATE INDEX historian_events_time_idx
  ON historian_events(occurred_at_ns DESC, sequence DESC);
CREATE INDEX historian_events_component_idx
  ON historian_events(component, sequence DESC);
CREATE INDEX historian_events_type_idx
  ON historian_events(event_type, sequence DESC);
)sql";

[[noreturn]] void StoreFail(const std::string &code,
                            const std::string &message) {
  throw EventHistorianError(code, message);
}

void Require(const bool condition, const std::string &code,
             const std::string &message) {
  if (!condition) {
    StoreFail(code, message);
  }
}

std::string SqliteDetail(sqlite3 *database, const int result) {
  return database == nullptr ? sqlite3_errstr(result)
                             : sqlite3_errmsg(database);
}

void Check(sqlite3 *database, const int result,
           const std::string_view operation) {
  if (result != SQLITE_OK) {
    StoreFail("historian_storage_failure",
              std::string(operation) + ": " + SqliteDetail(database, result));
  }
}

void Exec(sqlite3 *database, const std::string &sql) {
  char *error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string detail =
        error == nullptr ? SqliteDetail(database, result) : error;
    sqlite3_free(error);
    StoreFail("historian_storage_failure",
              "execute SQLite statement: " + detail);
  }
}

class Statement final {
public:
  Statement(sqlite3 *database, const std::string &sql) : database_(database) {
    Check(database_,
          sqlite3_prepare_v3(database_, sql.c_str(),
                             static_cast<int>(sql.size()),
                             SQLITE_PREPARE_PERSISTENT, &statement_, nullptr),
          "prepare SQLite statement");
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  void Bind(const int index, const std::string_view value) {
    Check(database_,
          sqlite3_bind_text(statement_, index, value.data(),
                            static_cast<int>(value.size()), SQLITE_TRANSIENT),
          "bind SQLite text");
  }

  void Bind(const int index, const std::int64_t value) {
    Check(database_, sqlite3_bind_int64(statement_, index, value),
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
    StoreFail(result == SQLITE_BUSY || result == SQLITE_LOCKED
                  ? "historian_storage_busy"
                  : "historian_storage_failure",
              "step SQLite statement: " + SqliteDetail(database_, result));
  }

  [[nodiscard]] std::string Text(const int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_TEXT,
            "historian_storage_corrupt",
            "historian text column has an invalid type");
    const auto *bytes = sqlite3_column_text(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    Require(bytes != nullptr && size >= 0, "historian_storage_corrupt",
            "historian text column is unreadable");
    return std::string(reinterpret_cast<const char *>(bytes),
                       static_cast<std::size_t>(size));
  }

  [[nodiscard]] std::int64_t Integer(const int column) const {
    Require(sqlite3_column_type(statement_, column) == SQLITE_INTEGER,
            "historian_storage_corrupt",
            "historian integer column has an invalid type");
    return sqlite3_column_int64(statement_, column);
  }

private:
  sqlite3 *database_{};
  sqlite3_stmt *statement_{};
};

class Transaction final {
public:
  explicit Transaction(sqlite3 *database) : database_(database) {
    Exec(database_, "BEGIN IMMEDIATE");
  }
  ~Transaction() {
    if (!committed_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }
  void Commit() {
    Exec(database_, "COMMIT");
    committed_ = true;
  }

private:
  sqlite3 *database_{};
  bool committed_{};
};

std::int64_t ScalarInteger(sqlite3 *database, const std::string &sql) {
  Statement statement(database, sql);
  Require(statement.Step(), "historian_storage_corrupt",
          "SQLite scalar query returned no row");
  const auto result = statement.Integer(0);
  Require(!statement.Step(), "historian_storage_corrupt",
          "SQLite scalar query returned multiple rows");
  return result;
}

std::filesystem::path ValidatePath(std::filesystem::path path) {
  Require(!path.empty() && path.filename() != "." && path.filename() != "..",
          "historian_path_invalid", "historian database path is invalid");
  std::error_code error;
  path = std::filesystem::absolute(path, error).lexically_normal();
  Require(!error, "historian_path_invalid",
          "historian database path cannot be resolved");
  const auto parent_status =
      std::filesystem::symlink_status(path.parent_path(), error);
  Require(!error && std::filesystem::is_directory(parent_status) &&
              !std::filesystem::is_symlink(parent_status),
          "historian_path_invalid",
          "historian parent must be an existing non-symlink directory");
  for (const auto &candidate :
       {path, std::filesystem::path(path.string() + "-wal"),
        std::filesystem::path(path.string() + "-shm")}) {
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      continue;
    }
    Require(!error && !std::filesystem::is_symlink(status),
            "historian_path_invalid",
            "historian database or sidecar must not be a symlink");
    if (std::filesystem::exists(status)) {
      Require(std::filesystem::is_regular_file(status),
              "historian_path_invalid",
              "historian database or sidecar must be a regular file");
    }
  }
  return path;
}

std::set<std::string> SchemaObjectNames(sqlite3 *database) {
  Statement statement(database,
                      "SELECT type || ':' || name FROM sqlite_schema "
                      "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name");
  std::set<std::string> result;
  while (statement.Step()) {
    result.insert(statement.Text(0));
  }
  return result;
}

void ValidateQuery(const HistorianQuery &query) {
  Require(query.limit <= kMaximumHistorianPageSize, "historian_query_invalid",
          "history page limit exceeds 10000");
  Require(query.components.size() <= 256U && query.event_types.size() <= 256U,
          "historian_query_invalid", "history filter exceeds 256 values");
  if (query.start_ns.has_value()) {
    Require(*query.start_ns >= 0, "historian_query_invalid",
            "history start_ns must be nonnegative");
  }
  if (query.end_ns.has_value()) {
    Require(*query.end_ns >= 0, "historian_query_invalid",
            "history end_ns must be nonnegative");
  }
  if (query.start_ns.has_value() && query.end_ns.has_value()) {
    Require(*query.start_ns <= *query.end_ns, "historian_query_invalid",
            "history start_ns must not exceed end_ns");
  }
  if (query.before_sequence.has_value()) {
    Require(*query.before_sequence != 0U &&
                *query.before_sequence <= kMaximumSafeInteger,
            "historian_query_invalid", "history before_sequence is invalid");
  }
  for (const auto &value : query.components) {
    Require(!value.empty() && value.size() <= 1024U &&
                content_identity::is_utf8_nfc(value),
            "historian_query_invalid", "history component filter is invalid");
  }
  for (const auto &value : query.event_types) {
    Require(!value.empty() && value.size() <= 1024U &&
                content_identity::is_utf8_nfc(value),
            "historian_query_invalid", "history event filter is invalid");
  }
}

std::string Placeholders(const std::size_t count) {
  std::string result;
  for (std::size_t index = 0U; index < count; ++index) {
    if (index != 0U) {
      result += ',';
    }
    result += '?';
  }
  return result;
}

} // namespace

class SqliteEventHistoryStore::Impl final {
public:
  Impl(std::filesystem::path database_path, const std::size_t record_retention)
      : database_path_(ValidatePath(std::move(database_path))),
        record_retention_(record_retention) {
    Require(record_retention_ != 0U &&
                record_retention_ <= kMaximumRecordRetention,
            "historian_configuration_invalid",
            "historian retention must be between 1 and 1000000 records");
    Require(sqlite3_libversion_number() >= 3037000,
            "historian_storage_unsupported",
            "historian requires SQLite 3.37.0 or newer");
    const auto result =
        sqlite3_open_v2(database_path_.string().c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
                        nullptr);
    if (result != SQLITE_OK) {
      const auto detail = SqliteDetail(database_, result);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      StoreFail("historian_storage_failure",
                "open historian database: " + detail);
    }
    try {
      Configure();
      InitializeOrValidate();
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

  IEventHistoryStore::AppendStatus Append(const HistorianEvent &event) {
    const auto event_json =
        content_identity::canonical_json(HistorianPersistenceValue(event));
    const auto payload_json = content_identity::canonical_json(event.payload);
    const auto payload_digest = content_identity::sha256_digest(payload_json);
    const auto event_digest = content_identity::sha256_digest(event_json);
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction transaction(database_);
    Statement insert(
        database_, "INSERT OR IGNORE INTO historian_events("
                   "event_id,component,event_type,severity,occurred_at_ns,"
                   "occurred_at_wall,payload_json,payload_digest,event_digest) "
                   "VALUES(?,?,?,?,?,?,?,?,?)");
    insert.Bind(1, event.event_id);
    insert.Bind(2, event.component);
    insert.Bind(3, event.event_type);
    insert.Bind(4, event.severity);
    insert.Bind(5, event.occurred_at_ns);
    insert.Bind(6, event.occurred_at_wall);
    insert.Bind(7, payload_json);
    insert.Bind(8, payload_digest);
    insert.Bind(9, event_digest);
    Require(!insert.Step(), "historian_storage_corrupt",
            "historian insert returned an unexpected row");
    const bool stored = sqlite3_changes(database_) == 1;
    if (!stored) {
      Statement existing(
          database_,
          "SELECT event_digest FROM historian_events WHERE event_id=?");
      existing.Bind(1, event.event_id);
      Require(existing.Step() && existing.Text(0) == event_digest &&
                  !existing.Step(),
              "historian_event_identity_conflict",
              "stored historian identity has conflicting content");
    } else {
      Statement prune(
          database_,
          "DELETE FROM historian_events WHERE sequence <= COALESCE(("
          "SELECT sequence FROM historian_events ORDER BY sequence DESC "
          "LIMIT 1 OFFSET ?),0)");
      prune.Bind(1, static_cast<std::int64_t>(record_retention_));
      Require(!prune.Step(), "historian_storage_corrupt",
              "historian prune returned an unexpected row");
    }
    transaction.Commit();
    return stored ? IEventHistoryStore::AppendStatus::kStored
                  : IEventHistoryStore::AppendStatus::kDuplicate;
  }

  HistorianPage Query(const HistorianQuery &query) {
    ValidateQuery(query);
    if (query.limit == 0U) {
      return {{}, false, std::nullopt};
    }
    std::string sql =
        "SELECT sequence,event_id,component,event_type,severity,"
        "occurred_at_ns,occurred_at_wall,payload_json,payload_digest,"
        "event_digest FROM historian_events WHERE 1=1";
    if (query.start_ns.has_value()) {
      sql += " AND occurred_at_ns>=?";
    }
    if (query.end_ns.has_value()) {
      sql += " AND occurred_at_ns<=?";
    }
    if (query.before_sequence.has_value()) {
      sql += " AND sequence<?";
    }
    if (!query.components.empty()) {
      sql +=
          " AND component IN (" + Placeholders(query.components.size()) + ")";
    }
    if (!query.event_types.empty()) {
      sql +=
          " AND event_type IN (" + Placeholders(query.event_types.size()) + ")";
    }
    sql += " ORDER BY sequence DESC LIMIT ?";

    std::lock_guard lock(mutex_);
    RequireOpen();
    Statement statement(database_, sql);
    int parameter = 1;
    if (query.start_ns.has_value()) {
      statement.Bind(parameter++, *query.start_ns);
    }
    if (query.end_ns.has_value()) {
      statement.Bind(parameter++, *query.end_ns);
    }
    if (query.before_sequence.has_value()) {
      statement.Bind(parameter++,
                     static_cast<std::int64_t>(*query.before_sequence));
    }
    for (const auto &component : query.components) {
      statement.Bind(parameter++, component);
    }
    for (const auto &event_type : query.event_types) {
      statement.Bind(parameter++, event_type);
    }
    statement.Bind(parameter, static_cast<std::int64_t>(query.limit + 1U));

    std::vector<HistorianStoredEvent> events;
    events.reserve(query.limit + 1U);
    while (statement.Step()) {
      const auto sequence = statement.Integer(0);
      const auto event_id = statement.Text(1);
      const auto component = statement.Text(2);
      const auto event_type = statement.Text(3);
      const auto severity = statement.Text(4);
      const auto occurred_at_ns = statement.Integer(5);
      const auto occurred_at_wall = statement.Text(6);
      const auto payload_json = statement.Text(7);
      const auto payload_digest = statement.Text(8);
      const auto event_digest = statement.Text(9);
      Require(sequence > 0 &&
                  static_cast<std::uint64_t>(sequence) <= kMaximumSafeInteger,
              "historian_storage_corrupt",
              "historian event sequence is invalid");
      Require(content_identity::sha256_digest(payload_json) == payload_digest,
              "historian_storage_corrupt",
              "historian payload digest does not match stored bytes");
      CanonicalValue payload;
      try {
        const auto decoded =
            deployment::parse_strict_json_model_yaml(payload_json);
        payload = decoded.root();
      } catch (const std::exception &error) {
        StoreFail("historian_storage_corrupt",
                  "historian payload is not strict canonical JSON: " +
                      std::string(error.what()));
      }
      auto event =
          MakeHistorianEvent(component, event_type, severity, occurred_at_ns,
                             occurred_at_wall, std::move(payload));
      Require(
          event.event_id == event_id &&
              content_identity::sha256_digest(content_identity::canonical_json(
                  HistorianPersistenceValue(event))) == event_digest,
          "historian_storage_corrupt",
          "historian event identity or digest is invalid");
      events.push_back(
          {static_cast<std::uint64_t>(sequence), std::move(event)});
    }
    const bool truncated = events.size() > query.limit;
    if (truncated) {
      events.resize(query.limit);
    }
    const auto next = truncated && !events.empty()
                          ? std::optional<std::uint64_t>(events.back().sequence)
                          : std::nullopt;
    return {std::move(events), truncated, next};
  }

  std::optional<HistorianStoredEvent> Read(const std::uint64_t sequence) {
    Require(sequence > 0U && sequence <= kMaximumSafeInteger,
            "historian_query_invalid", "history detail sequence is invalid");
    const auto page = Query(HistorianQuery{
        .start_ns = std::nullopt,
        .end_ns = std::nullopt,
        .components = {},
        .event_types = {},
        .before_sequence = sequence + 1U,
        .limit = 1U,
    });
    if (page.events.empty() || page.events.front().sequence != sequence) {
      return std::nullopt;
    }
    return page.events.front();
  }

  void Close() {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                              nullptr, nullptr);
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      StoreFail("historian_storage_failure",
                "close historian database: " + SqliteDetail(database_, result));
    }
    database_ = nullptr;
    closed_ = true;
  }

private:
  void RequireOpen() const {
    Require(!closed_ && database_ != nullptr, "historian_closed",
            "historian database is closed");
  }

  void Configure() {
    Check(database_, sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure historian busy timeout");
    Exec(database_, "PRAGMA journal_mode=WAL");
    Exec(database_, "PRAGMA synchronous=FULL");
    Exec(database_, "PRAGMA foreign_keys=ON");
    Exec(database_, "PRAGMA trusted_schema=OFF");
    Exec(database_, "PRAGMA recursive_triggers=OFF");
    Exec(database_, "PRAGMA wal_autocheckpoint=1000");
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    Require(page_size > 0, "historian_storage_corrupt",
            "historian SQLite page size is invalid");
    const auto maximum_pages =
        std::max<std::int64_t>(1, kMaximumDatabasePageImageBytes / page_size);
    Require(ScalarInteger(database_, "PRAGMA max_page_count=" +
                                         std::to_string(maximum_pages)) ==
                maximum_pages,
            "historian_storage_failure",
            "historian SQLite page ceiling could not be configured");
    sqlite3_limit(database_, SQLITE_LIMIT_LENGTH,
                  static_cast<int>(512U * 1024U));
  }

  void InitializeOrValidate() {
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    const auto objects = SchemaObjectNames(database_);
    if (objects.empty()) {
      Require(ScalarInteger(database_, "PRAGMA application_id") == 0 &&
                  ScalarInteger(database_, "PRAGMA user_version") == 0,
              "historian_storage_foreign",
              "schema-empty historian database has foreign identity");
      Exec(database_, std::string(kSchema));
      Exec(database_,
           "PRAGMA application_id=" + std::to_string(kHistorianApplicationId));
      Exec(database_,
           "PRAGMA user_version=" + std::to_string(kHistorianUserVersion));
      Statement metadata(database_,
                         "INSERT INTO historian_metadata("
                         "singleton,schema_id,schema_version,record_retention) "
                         "VALUES(1,'manyforge.event_historian',1,?)");
      metadata.Bind(1, static_cast<std::int64_t>(record_retention_));
      Require(!metadata.Step(), "historian_storage_corrupt",
              "historian metadata insert returned a row");
    }
    const std::set<std::string> expected{
        "index:historian_events_component_idx",
        "index:historian_events_time_idx",
        "index:historian_events_type_idx",
        "table:historian_events",
        "table:historian_metadata",
    };
    Require(SchemaObjectNames(database_) == expected,
            "historian_storage_foreign",
            "historian database contains an unexpected schema");
    Require(ScalarInteger(database_, "PRAGMA application_id") ==
                    kHistorianApplicationId &&
                ScalarInteger(database_, "PRAGMA user_version") ==
                    kHistorianUserVersion,
            "historian_storage_foreign",
            "historian SQLite identity is invalid");
    Statement metadata(database_,
                       "SELECT schema_id,schema_version,record_retention "
                       "FROM historian_metadata WHERE singleton=1");
    Require(metadata.Step() &&
                metadata.Text(0) == "manyforge.event_historian" &&
                metadata.Integer(1) == 1 &&
                metadata.Integer(2) ==
                    static_cast<std::int64_t>(record_retention_) &&
                !metadata.Step(),
            "historian_storage_foreign",
            "historian metadata is invalid or configuration changed");
    transaction.Commit();
  }

  std::filesystem::path database_path_;
  std::size_t record_retention_;
  std::mutex mutex_;
  sqlite3 *database_{};
  bool closed_{};
};

SqliteEventHistoryStore::SqliteEventHistoryStore(
    std::filesystem::path database_path, const std::size_t record_retention)
    : impl_(
          std::make_unique<Impl>(std::move(database_path), record_retention)) {}

SqliteEventHistoryStore::~SqliteEventHistoryStore() = default;

IEventHistoryStore::AppendStatus
SqliteEventHistoryStore::Append(const HistorianEvent &event) {
  return impl_->Append(event);
}

HistorianPage SqliteEventHistoryStore::Query(const HistorianQuery &query) {
  return impl_->Query(query);
}

std::optional<HistorianStoredEvent>
SqliteEventHistoryStore::Read(const std::uint64_t sequence) {
  return impl_->Read(sequence);
}

void SqliteEventHistoryStore::Close() { impl_->Close(); }

} // namespace manyforge::headless
