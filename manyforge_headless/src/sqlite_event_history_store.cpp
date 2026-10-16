#include "manyforge_headless/event_historian.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr int kHistorianApplicationId = 0x4D464548;
constexpr int kHistorianUserVersion = 4;
/// A maximal record (256-KiB payload plus its bounded envelope) always fits
/// one capture, so a trigger is never dropped for its size.
constexpr std::int64_t kMinimumCaptureBytes = 512LL * 1024LL;
/// Log pages after which the writer checkpoints (SQLite's default cadence).
constexpr std::int64_t kWalCheckpointPages = 1000;
/// Unreclaimed log above this refuses new writes until a checkpoint succeeds.
constexpr std::int64_t kMaximumWalBacklogBytes = 64LL * 1024LL * 1024LL;
/// Record bytes after which a batch stops taking further events, so one
/// transaction's log overshoot past the reclaim threshold stays bounded.
constexpr std::int64_t kMaximumBatchRecordBytes = 4LL * 1024LL * 1024LL;
/// Log file size kept after a checkpoint resets the log.
constexpr std::int64_t kWalRetainedBytes = 4LL * 1024LL * 1024LL;
constexpr int kBusyTimeoutMilliseconds = 1000;
constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumRecordRetention = 1000000U;
constexpr std::int64_t kMaximumDatabasePageImageBytes = 512LL * 1024LL * 1024LL;
// Six <=1024-byte identities, a <=1024-code-point message, a <=128-byte
// wall clock and fixed fields fit below 48 KiB even with 6-byte JSON escapes.
constexpr std::size_t kMaximumSummaryBytes = 64U * 1024U;

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

// Secondary indexes live with the immutable event, never in another service.
// External-content FTS retains no second copy of payload_json.
constexpr std::string_view kQueryIndexSchema = R"sql(
ALTER TABLE historian_metadata ADD COLUMN prior_pruning_known INTEGER NOT NULL DEFAULT 0;
ALTER TABLE historian_events ADD COLUMN occurred_at_utc_ns INTEGER;
ALTER TABLE historian_events ADD COLUMN clock_owner TEXT;
ALTER TABLE historian_events ADD COLUMN wall_clock_regressed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE historian_events ADD COLUMN condition_severity TEXT NOT NULL DEFAULT 'INFO';
ALTER TABLE historian_events ADD COLUMN retention_class INTEGER NOT NULL DEFAULT 0;
ALTER TABLE historian_events ADD COLUMN record_bytes INTEGER NOT NULL DEFAULT 0;
CREATE INDEX historian_events_utc_idx ON historian_events(occurred_at_utc_ns,sequence DESC);
CREATE INDEX historian_events_clock_idx ON historian_events(clock_owner,occurred_at_ns DESC,sequence DESC);
CREATE INDEX historian_events_severity_idx ON historian_events(condition_severity,sequence DESC);
CREATE INDEX historian_events_retention_idx ON historian_events(retention_class,sequence);
CREATE INDEX historian_events_coverage_idx ON historian_events(retention_class,occurred_at_utc_ns);
CREATE TABLE historian_retention (
  partition INTEGER PRIMARY KEY CHECK(partition IN (0,1)),
  pruned_count INTEGER NOT NULL DEFAULT 0,
  pruned_bytes INTEGER NOT NULL DEFAULT 0,
  last_pruned_sequence INTEGER
) STRICT;
INSERT INTO historian_retention(partition) VALUES(0),(1);
CREATE TABLE historian_event_keys (
  sequence INTEGER NOT NULL REFERENCES historian_events(sequence) ON DELETE CASCADE,
  kind TEXT NOT NULL,
  value TEXT NOT NULL,
  PRIMARY KEY(sequence,kind,value)
) STRICT, WITHOUT ROWID;
CREATE INDEX historian_event_keys_lookup_idx ON historian_event_keys(kind,value,sequence DESC);
CREATE VIEW historian_search_content AS
  SELECT sequence,component,event_type,payload_json AS details FROM historian_events;
CREATE VIRTUAL TABLE historian_search USING fts5(
  component,event_type,details,content='historian_search_content',content_rowid='sequence',tokenize='trigram'
);
)sql";

// Storage revision 4: failure capture protects already-durable rows by
// reference, never by copying them. ``capture_refs`` is a derived index
// validated against the capture table on every open.
constexpr std::string_view kCaptureSchema = R"sql(
ALTER TABLE historian_events ADD COLUMN capture_refs INTEGER NOT NULL DEFAULT 0 CHECK(capture_refs >= 0);
CREATE INDEX historian_events_prune_idx ON historian_events(retention_class,capture_refs,sequence);
CREATE TABLE historian_captures (
  capture_id INTEGER PRIMARY KEY AUTOINCREMENT,
  state TEXT NOT NULL CHECK(state IN ('open','closed','truncated','closed_by_restart')),
  close_reason TEXT CHECK(close_reason IS NULL OR close_reason IN ('tail_rows','tail_duration','capture_bytes','restart')),
  first_sequence INTEGER NOT NULL,
  trigger_sequence INTEGER NOT NULL,
  last_sequence INTEGER NOT NULL,
  trigger_event_id TEXT NOT NULL,
  opened_utc_ns INTEGER,
  writer_epoch TEXT NOT NULL,
  tail_deadline_ns INTEGER NOT NULL,
  preceding_rows INTEGER NOT NULL CHECK(preceding_rows >= 0),
  preceding_limit TEXT NOT NULL CHECK(preceding_limit IN ('rows','bytes','history_start','pruned')),
  tail_rows INTEGER NOT NULL CHECK(tail_rows >= 0),
  capture_bytes INTEGER NOT NULL CHECK(capture_bytes > 0),
  trigger_count INTEGER NOT NULL CHECK(trigger_count >= 1),
  trigger_overflow INTEGER NOT NULL CHECK(trigger_overflow >= 0),
  loss_at_open INTEGER NOT NULL CHECK(loss_at_open >= 0),
  loss_at_last INTEGER NOT NULL CHECK(loss_at_last >= loss_at_open),
  spans_restart INTEGER NOT NULL CHECK(spans_restart IN (0,1)),
  CHECK(first_sequence > 0 AND first_sequence <= trigger_sequence AND trigger_sequence <= last_sequence)
) STRICT;
CREATE TABLE historian_capture_triggers (
  capture_id INTEGER NOT NULL REFERENCES historian_captures(capture_id) ON DELETE CASCADE,
  sequence INTEGER NOT NULL,
  event_id TEXT NOT NULL,
  PRIMARY KEY(capture_id,sequence)
) STRICT, WITHOUT ROWID;
CREATE TABLE historian_capture_retention (
  singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
  retired_count INTEGER NOT NULL DEFAULT 0,
  released_rows INTEGER NOT NULL DEFAULT 0,
  released_bytes INTEGER NOT NULL DEFAULT 0,
  last_retired_capture_id INTEGER
) STRICT;
INSERT INTO historian_capture_retention(singleton) VALUES(1);
)sql";

constexpr std::string_view kCorrelationNames =
    "'program_run_id','operation_id','action_span_id','node_instance_id','node_attempt_id',"
    "'intervention_session_id','contact_policy_id','incident_id','condition_id','alarm_condition_id',"
    "'activation_id','stop_id','resource_id','source_owner_epoch','phase','parent_node_attempt_id'";
// SQL list of retained Program diagnostic versions whose correlation
// attribution is producer-level only.
const std::string kProgramDiagnosticSchemaList = [] {
  std::string list;
  for (const auto version : kProgramDiagnosticSchemaVersions)
    list += (list.empty() ? "'" : ",'") + std::string(version) + "'";
  return list;
}();
const std::set<std::string> kCorrelationKeys{
    "program_run_id", "operation_id", "action_span_id", "node_instance_id", "node_attempt_id",
    "intervention_session_id", "contact_policy_id", "incident_id", "condition_id", "alarm_condition_id",
    "activation_id", "stop_id", "resource_id", "source_owner_epoch", "channel", "phase", "parent_node_attempt_id"};

std::optional<std::string> RecordedClockOwner(const HistorianEvent &event) {
  if (!event.payload.is_object()) return std::nullopt;
  const auto text = [&](std::string_view name) -> std::optional<std::string> {
    const auto *value = event.payload.find_object_member(name);
    if (value == nullptr || !std::holds_alternative<std::string>(value->storage())) return std::nullopt;
    const auto &result = value->as_string();
    return result.empty() || result.size() > 1024U ? std::nullopt : std::optional(result);
  };
  if (auto explicit_owner = text("clock_owner")) return explicit_owner;
  // Only this versioned native adapter proves which clock stamped the event.
  // Arbitrary nested owner/session IDs are not a clock-domain assertion.
  if (text("schema_version") == "manyforge.operation_history_event.v1") {
    if (auto epoch = text("source_owner_epoch")) return "kernel:" + *epoch;
  }
  return std::nullopt;
}

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
    StoreFail(result == SQLITE_BUSY || result == SQLITE_LOCKED ? "historian_storage_busy" : "historian_storage_failure",
              "execute SQLite statement: " + detail);
  }
}

sqlite3_stmt *Prepare(sqlite3 *database, const std::string &sql) {
  sqlite3_stmt *statement = nullptr;
  Check(database,
        sqlite3_prepare_v3(database, sql.c_str(), static_cast<int>(sql.size()),
                           SQLITE_PREPARE_PERSISTENT, &statement, nullptr),
        "prepare SQLite statement");
  return statement;
}

/// Prepared statements of the fixed write-path SQL, reused across appends.
/// A borrowed statement is reset and unbound when its ``Statement`` ends, so
/// it never keeps a read open or old values; a statement already in use is
/// never shared (a nested use prepares its own). ``Clear`` finalizes all and
/// must precede closing the connection.
class StatementCache final {
public:
  struct Entry {
    sqlite3_stmt *statement{};
    bool in_use{};
  };

  explicit StatementCache(sqlite3 *const &database) : database_(database) {}
  ~StatementCache() { Clear(); }
  StatementCache(const StatementCache &) = delete;
  StatementCache &operator=(const StatementCache &) = delete;

  Entry *Borrow(const std::string &sql) {
    auto found = entries_.find(sql);
    if (found == entries_.end()) {
      found = entries_.emplace(sql, Entry{Prepare(database_, sql), false}).first;
    }
    if (found->second.in_use) return nullptr;
    found->second.in_use = true;
    return &found->second;
  }

  [[nodiscard]] sqlite3 *database() const { return database_; }

  void Clear() {
    for (auto &[sql, entry] : entries_) sqlite3_finalize(entry.statement);
    entries_.clear();
  }

private:
  sqlite3 *const &database_;
  std::unordered_map<std::string, Entry> entries_;
};

class Statement final {
public:
  Statement(sqlite3 *database, const std::string &sql)
      : database_(database), statement_(Prepare(database, sql)) {}

  Statement(StatementCache &cache, const std::string &sql)
      : database_(cache.database()), borrowed_(cache.Borrow(sql)) {
    statement_ = borrowed_ != nullptr ? borrowed_->statement : Prepare(database_, sql);
  }

  ~Statement() {
    if (borrowed_ == nullptr) {
      sqlite3_finalize(statement_);
      return;
    }
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
    borrowed_->in_use = false;
  }
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

  void BindNull(const int index) {
    Check(database_, sqlite3_bind_null(statement_, index), "bind SQLite null");
  }

  [[nodiscard]] bool IsNull(const int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

  [[nodiscard]] bool Step() {
    const auto result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    StoreFail(result == SQLITE_INTERRUPT ? "historian_query_budget_exceeded" :
              result == SQLITE_BUSY || result == SQLITE_LOCKED
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
  StatementCache::Entry *borrowed_{};
  sqlite3_stmt *statement_{};
};

/// A batch holding an ERROR/CRITICAL condition or a previous-session report is synced to disk at
/// commit; routine batches keep synchronous=NORMAL. Restored after the
/// transaction ends (a failed restore only leaves later commits synced).
class SynchronousScope final {
public:
  SynchronousScope(sqlite3 *database, const bool full) : database_(full ? database : nullptr) {
    if (database_ != nullptr) Exec(database_, "PRAGMA synchronous=FULL");
  }
  ~SynchronousScope() {
    if (database_ != nullptr) {
      sqlite3_exec(database_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    }
  }
  SynchronousScope(const SynchronousScope &) = delete;
  SynchronousScope &operator=(const SynchronousScope &) = delete;

private:
  sqlite3 *database_{};
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

/// Execute one statement that returns no row.
template <typename Source>
void Run(Source &&database, const std::string &sql) {
  Statement statement(database, sql);
  Require(!statement.Step(), "historian_storage_corrupt", "SQLite statement returned a row");
}

template <typename Source>
std::int64_t ScalarInteger(Source &&database, const std::string &sql) {
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
  Require((!query.start_ns && !query.end_ns) || query.clock_owner.has_value(),
          "historian_query_invalid", "monotonic filters require an exact clock_owner; use UTC across owners");
  Require((!query.start_utc_ns || *query.start_utc_ns >= 0) &&
              (!query.end_utc_ns || *query.end_utc_ns >= 0) &&
              (!query.start_utc_ns || !query.end_utc_ns || *query.start_utc_ns <= *query.end_utc_ns),
          "historian_query_invalid", "history UTC bounds are invalid");
  for (const auto &severity : query.severities) {
    Require(std::set<std::string>{"DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"}.contains(severity),
            "historian_query_invalid", "unsupported history severity");
  }
  for (const auto &[kind, value] : query.correlations) {
    Require(kCorrelationKeys.contains(kind) && !value.empty() && value.size() <= 1024U &&
                content_identity::is_utf8_nfc(value),
            "historian_query_invalid", "unsupported or invalid history correlation");
  }
  Require(!query.clock_owner || (!query.clock_owner->empty() && query.clock_owner->size() <= 1024U &&
              content_identity::is_utf8_nfc(*query.clock_owner)),
          "historian_query_invalid", "invalid history clock owner");
  Require(!query.alarm_phase || std::set<std::string>{"all", "activated", "returned", "acknowledged", "raw"}.contains(*query.alarm_phase),
          "historian_query_invalid", "unsupported alarm history phase");
  if (!query.search.empty()) {
    const auto characters = std::count_if(query.search.begin(), query.search.end(),
        [](unsigned char byte) { return (byte & 0xc0U) != 0x80U; });
    Require(query.search.size() <= 256U && characters >= 3 && content_identity::is_utf8_nfc(query.search),
            "historian_query_invalid", "indexed search requires 3 or more Unicode characters and at most 256 bytes");
  }
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

std::int64_t RetentionPartition(const HistorianEvent &event) {
  const auto severity = HistorianConditionSeverity(event);
  return severity == "ERROR" || severity == "CRITICAL" ||
         event.event_type.find("stop") != std::string::npos ||
         event.event_type.find("acknowledged") != std::string::npos;
}

std::string SummaryDigest(std::int64_t sequence, const std::string& event_digest,
                          const std::string& summary) {
  return content_identity::sha256_digest("manyforge.historian-summary.v1\n" +
                                         std::to_string(sequence) + "\n" + event_digest + "\n" +
                                         summary);
}

std::string SummaryJson(const HistorianEvent& event) {
  // Clock regression is a later derived index flag, not immutable event data.
  return content_identity::canonical_json(HistorianSummaryEventValue(
      event, HistorianRecordedUtcNs(event.occurred_at_wall), RecordedClockOwner(event), false));
}

CanonicalValue ReadStoredSummary(const Statement& row) {
  const auto sequence = row.Integer(0);
  Require(sequence > 0 && static_cast<std::uint64_t>(sequence) <= kMaximumSafeInteger,
          "historian_storage_corrupt", "historian summary sequence is invalid");
  const auto text = row.Text(1);
  Require(text.size() <= kMaximumSummaryBytes &&
              SummaryDigest(sequence, row.Text(3), text) == row.Text(2),
          "historian_storage_corrupt", "historian summary digest differs from stored bytes");
  CanonicalValue value;
  try {
    value = content_identity::parse_canonical_json(
        text, content_identity::CanonicalJsonParseLimits{kMaximumSummaryBytes, 4U, 64U});
  } catch (const std::exception& error) {
    StoreFail("historian_storage_corrupt",
              "invalid historian summary: " + std::string(error.what()));
  }
  Require(value.is_object() && value.find_object_member("wall_clock_regressed") != nullptr &&
              (row.Integer(4) == 0 || row.Integer(4) == 1),
          "historian_storage_corrupt", "historian summary flag is invalid");
  auto fields = value.as_object();
  for (auto& [name, field] : fields) {
    if (name == "wall_clock_regressed") field = CanonicalValue(row.Integer(4) != 0);
  }
  return CanonicalValue(
      CanonicalValue::Object{{"sequence", CanonicalValue(static_cast<double>(sequence))},
                             {"event", CanonicalValue(std::move(fields))}});
}

HistorianStoredEvent ReadStoredEvent(const Statement &row, bool indexed = true) {
  const auto sequence = row.Integer(0);
  Require(sequence > 0 && static_cast<std::uint64_t>(sequence) <= kMaximumSafeInteger,
          "historian_storage_corrupt", "historian event sequence is invalid");
  const auto payload_json = row.Text(7);
  Require(content_identity::sha256_digest(payload_json) == row.Text(8),
          "historian_storage_corrupt", "historian payload digest does not match stored bytes");
  CanonicalValue payload;
  try {
    payload = content_identity::parse_canonical_json(payload_json,
        content_identity::CanonicalJsonParseLimits{kMaximumHistorianPayloadBytes, 65U, 250000U});
  } catch (const std::exception &error) {
    StoreFail("historian_storage_corrupt",
              "historian payload is not strict canonical JSON: " + std::string(error.what()));
  }
  auto event = MakeHistorianEvent(row.Text(2), row.Text(3), row.Text(4), row.Integer(5),
                                  row.Text(6), std::move(payload));
  const auto event_json = content_identity::canonical_json(HistorianPersistenceValue(event));
  Require(event.event_id == row.Text(1) &&
              content_identity::sha256_digest(event_json) == row.Text(9),
          "historian_storage_corrupt", "historian event identity or digest is invalid");
  const auto utc = HistorianRecordedUtcNs(event.occurred_at_wall);
  const auto owner = RecordedClockOwner(event);
  bool regressed = false;
  if (indexed) {
    const auto flag = row.Integer(12);
    Require(utc.has_value() != row.IsNull(10) && (!utc || *utc == row.Integer(10)) &&
                owner.has_value() != row.IsNull(11) && (!owner || *owner == row.Text(11)) &&
                (flag == 0 || flag == 1) && row.Text(13) == HistorianConditionSeverity(event) &&
                row.Integer(14) == RetentionPartition(event) &&
                row.Integer(15) == static_cast<std::int64_t>(event_json.size()),
            "historian_storage_corrupt", "historian query index does not match the original event");
    regressed = flag == 1;
  }
  return {static_cast<std::uint64_t>(sequence), std::move(event), utc, owner, regressed};
}

constexpr std::string_view kEventColumns =
    "sequence,event_id,component,event_type,severity,occurred_at_ns,occurred_at_wall,"
    "payload_json,payload_digest,event_digest";
const std::string kIndexedEventColumns = std::string(kEventColumns) +
    ",occurred_at_utc_ns,clock_owner,wall_clock_regressed,condition_severity,retention_class,record_bytes";

class QueryBudget final {
 public:
  explicit QueryBudget(sqlite3 *database) : database_(database),
      deadline_(std::chrono::steady_clock::now() + std::chrono::milliseconds(100)) {
    sqlite3_progress_handler(database_, 1000, [](void *self) {
      return std::chrono::steady_clock::now() >= static_cast<QueryBudget *>(self)->deadline_ ? 1 : 0;
    }, this);
  }
  ~QueryBudget() { sqlite3_progress_handler(database_, 0, nullptr, nullptr); }
  void Check() const {
    Require(std::chrono::steady_clock::now() < deadline_, "historian_query_budget_exceeded",
            "history query exceeded its 100 ms work budget; narrow the filters");
  }
  bool YieldExportPage() const {
    // Leave half the hard budget for a final already-started bounded record.
    // Pagination returns verified work; it never hides a failed verification.
    Check();
    return std::chrono::steady_clock::now() >= deadline_ - std::chrono::milliseconds(50);
  }

 private:
  sqlite3 *database_;
  std::chrono::steady_clock::time_point deadline_;
};

struct PartitionUsage {
  std::int64_t count{}, bytes{}, unknown_time{}, regressed_time{};
};
// Routine and important govern unprotected rows; bucket 2 is the failure
// capture budget. A protected row is charged there exactly once, however
// many overlapping captures reference it.
constexpr std::size_t kCaptureBucket = 2U;
using RetainedUsage = std::array<PartitionUsage, 3>;

std::size_t UsageBucket(const std::int64_t retention_class, const std::int64_t capture_refs) {
  return capture_refs > 0 ? kCaptureBucket : static_cast<std::size_t>(retention_class);
}

void AccumulateUsage(RetainedUsage &usage, const HistorianStoredEvent &stored, std::int64_t bytes,
                     std::int64_t capture_refs) {
  auto &part = usage.at(UsageBucket(RetentionPartition(stored.event), capture_refs));
  ++part.count;
  part.bytes += bytes;
  part.unknown_time += !stored.occurred_at_utc_ns;
  part.regressed_time += stored.wall_clock_regressed;
}

/// A failure is an event whose own severity is ERROR or CRITICAL. A client
/// incident's resolution record keeps its condition's severity for filtering
/// but is itself INFO: it neither opens nor joins a capture as a trigger.
bool IsFailureTrigger(const HistorianEvent &event) {
  return event.severity == "ERROR" || event.severity == "CRITICAL";
}

} // namespace

class SqliteEventHistoryStore::Impl final {
public:
  Impl(std::filesystem::path database_path, const std::size_t record_retention,
       HistorianCaptureLimits capture_limits, HistorianWriterClock writer_clock)
      : database_path_(ValidatePath(std::move(database_path))),
        record_retention_(record_retention),
        capture_limits_(capture_limits),
        writer_clock_(writer_clock ? std::move(writer_clock) : HistorianWriterClock([] {
          return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count());
        })),
        writer_epoch_(NewWriterEpoch()) {
    Require(record_retention_ != 0U &&
                record_retention_ <= kMaximumRecordRetention,
            "historian_configuration_invalid",
            "historian retention must be between 1 and 1000000 records");
    Require(capture_limits_.preceding_rows <= 100000U &&
                capture_limits_.tail_rows <= 100000U &&
                capture_limits_.tail_duration.count() >= 0 &&
                capture_limits_.tail_duration <= std::chrono::hours(24) &&
                capture_limits_.preceding_bytes > 0 &&
                capture_limits_.capture_bytes > capture_limits_.preceding_bytes &&
                capture_limits_.capture_bytes >= kMinimumCaptureBytes &&
                capture_limits_.protected_bytes >= capture_limits_.capture_bytes &&
                capture_limits_.protected_bytes <= kMaximumDatabasePageImageBytes / 4 &&
                capture_limits_.live_captures >= 1U && capture_limits_.live_captures <= 1024U &&
                capture_limits_.recorded_triggers >= 1U && capture_limits_.recorded_triggers <= 1024U,
            "historian_configuration_invalid",
            "historian failure capture limits are inconsistent");
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
      Require(sqlite3_db_readonly(database_, "main") == 0, "historian_storage_failure",
              "historian requires writable storage; read-only fallback is not healthy persistence");
      Configure();
      InitializeOrValidate();
      // A migration can leave a log as large as its transaction; start every
      // session from an empty log (no reader exists yet).
      Require(CheckpointWal(SQLITE_CHECKPOINT_TRUNCATE), "historian_storage_busy",
              "historian write-ahead log could not be reclaimed at open");
    } catch (...) {
      statements_.Clear();
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
    const auto result = AppendBatch(std::span<const HistorianEvent>(&event, 1U));
    if (result.failure) std::rethrow_exception(result.failure);
    return result.statuses.front();
  }

  /// One transaction for the batch and one savepoint per event: a failing
  /// event rolls back alone, the events before it commit, and the caller
  /// retries from it. A failure of the transaction itself is thrown and
  /// commits nothing.
  IEventHistoryStore::AppendBatchResult AppendBatch(const std::span<const HistorianEvent> events) {
    IEventHistoryStore::AppendBatchResult result;
    if (events.empty()) return result;
    const bool durable = std::any_of(events.begin(), events.end(), [](const auto &event) {
      return IsFailureTrigger(event) ||
             (event.component == "manyforge.workbench" &&
              event.event_type == "previous_session_report");
    });
    std::lock_guard lock(mutex_);
    RequireOpen();
    RequireReclaimableWal();
    SynchronousScope synchronous(database_, durable);
    Transaction transaction(database_);
    RequireUnchanged();
    auto usage = usage_;
    auto epoch_first = epoch_first_sequence_;
    std::int64_t batch_record_bytes = 0;
    for (const auto &event : events) {
      // The rest of the batch is left for the writer's next transaction.
      if (batch_record_bytes >= kMaximumBatchRecordBytes) break;
      Run(statements_, "SAVEPOINT historian_event");
      auto event_usage = usage;
      auto event_epoch_first = epoch_first;
      std::int64_t record_bytes = 0;
      try {
        result.statuses.push_back(AppendInTransaction(event, event_usage, event_epoch_first, record_bytes));
      } catch (...) {
        // SQLite may already have rolled back the whole transaction (a full
        // or failing disk): then nothing of the batch is committed.
        if (sqlite3_get_autocommit(database_) != 0) throw;
        Run(statements_, "ROLLBACK TO historian_event");
        Run(statements_, "RELEASE historian_event");
        result.failure = std::current_exception();
        break;
      }
      Run(statements_, "RELEASE historian_event");
      batch_record_bytes += record_bytes;
      usage = event_usage;
      epoch_first = event_epoch_first;
    }
    transaction.Commit();
    usage_ = usage;
    epoch_first_sequence_ = epoch_first;
    if (wal_pages_ >= kWalCheckpointPages) CheckpointWal(SQLITE_CHECKPOINT_PASSIVE);
    return result;
  }

  void ObserveDeliveryLoss(const std::uint64_t cumulative_losses) {
    std::lock_guard lock(mutex_);
    delivery_loss_ = std::max<std::int64_t>(
        delivery_loss_, static_cast<std::int64_t>(std::min<std::uint64_t>(
                            cumulative_losses, kMaximumSafeInteger)));
  }

  CanonicalValue Captures(const std::optional<std::uint64_t> before_capture_id,
                          const std::size_t limit) {
    Require(limit >= 1U && limit <= 32U, "historian_query_invalid",
            "capture page limit must be between 1 and 32");
    Require(!before_capture_id || (*before_capture_id > 0U && *before_capture_id <= kMaximumSafeInteger),
            "historian_query_invalid", "capture cursor is invalid");
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction snapshot(database_);
    RequireUnchanged();
    QueryBudget budget(database_);
    Statement rows(database_, "SELECT capture_id FROM historian_captures WHERE capture_id<? "
                              "ORDER BY capture_id DESC LIMIT ?");
    rows.Bind(1, before_capture_id ? static_cast<std::int64_t>(*before_capture_id)
                                   : std::numeric_limits<std::int64_t>::max());
    rows.Bind(2, static_cast<std::int64_t>(limit + 1U));
    std::vector<std::int64_t> identities;
    while (rows.Step()) identities.push_back(rows.Integer(0));
    const bool truncated = identities.size() > limit;
    if (truncated) identities.resize(limit);
    CanonicalValue::Array captures;
    for (const auto identity : identities) {
      captures.push_back(CaptureEvidence(identity));
      budget.Check();
    }
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue(std::string(kHistorianCapturePageSchemaVersion))},
        {"captures", CanonicalValue(std::move(captures))},
        {"truncated", CanonicalValue(truncated)},
        {"next_before_capture_id", truncated
            ? CanonicalValue(std::to_string(identities.back())) : CanonicalValue(nullptr)},
        {"retention", Retention()},
    });
  }

  HistorianPage Query(const HistorianQuery& query, CanonicalValue::Array* summaries = nullptr,
                      bool exporting = false) {
    ValidateQuery(query);
    if (query.limit == 0U) {
      return {{}, false, std::nullopt};
    }
    const std::string columns = summaries == nullptr ? kIndexedEventColumns
                                                     : "sequence,summary_json,summary_digest,"
                                                       "event_digest,wall_clock_regressed";
    std::string sql = "SELECT " + columns + " FROM historian_events WHERE 1=1";
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
      sql += " AND (component GLOB ?";
      for (std::size_t i = 1; i < query.components.size(); ++i) sql += " OR component GLOB ?";
      sql += ')';
    }
    if (!query.event_types.empty()) {
      sql +=
          " AND event_type IN (" + Placeholders(query.event_types.size()) + ")";
    }
    if (query.start_utc_ns) sql += " AND occurred_at_utc_ns>=?";
    if (query.end_utc_ns) sql += " AND occurred_at_utc_ns<=?";
    if (query.clock_owner) sql += " AND clock_owner=?";
    if (!query.severities.empty()) sql += " AND condition_severity IN (" + Placeholders(query.severities.size()) + ")";
    for (const auto &[kind, value] : query.correlations) {
      (void)value;
      sql += kind == "channel" ? " AND component=?"
          : " AND sequence IN (SELECT sequence FROM historian_event_keys WHERE kind=? AND value=?)";
    }
    if (query.alarm_phase) {
      sql += *query.alarm_phase == "raw" ? " AND component<>'manyforge.alarm'" : " AND component='manyforge.alarm'";
      if (*query.alarm_phase != "all" && *query.alarm_phase != "raw") {
        sql += *query.alarm_phase == "acknowledged"
            ? " AND event_type IN ('operator_alarm_acknowledged','operator_alarms_acknowledged','operator_alarm_bulk_acknowledged')"
            : " AND event_type=?";
      }
    }
    if (!query.search.empty()) sql += " AND sequence IN (SELECT rowid FROM historian_search WHERE historian_search MATCH ?)";
    if (query.capture_id) sql += " AND sequence BETWEEN ? AND ?";
    sql += " ORDER BY sequence DESC LIMIT ?";

    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction snapshot(database_);
    RequireUnchanged();
    QueryBudget budget(database_);
    std::optional<std::pair<std::int64_t, std::int64_t>> capture_range;
    CanonicalValue capture_evidence;
    if (query.capture_id) {
      Require(*query.capture_id > 0U && *query.capture_id <= kMaximumSafeInteger,
              "historian_query_invalid", "capture_id is invalid");
      Statement range(database_, "SELECT first_sequence,last_sequence FROM historian_captures WHERE capture_id=?");
      range.Bind(1, static_cast<std::int64_t>(*query.capture_id));
      Require(range.Step(), "historian_capture_not_found",
              "failure capture is not retained; it may have been retired");
      capture_range = std::pair{range.Integer(0), range.Integer(1)};
      capture_evidence = CaptureEvidence(static_cast<std::int64_t>(*query.capture_id));
    }
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
      std::string prefix;
      for (const auto character : component) {
        if (character == '*' || character == '?' || character == '[') prefix += '[' + std::string(1, character) + ']';
        else prefix += character;
      }
      statement.Bind(parameter++, prefix + '*');
    }
    for (const auto &event_type : query.event_types) {
      statement.Bind(parameter++, event_type);
    }
    if (query.start_utc_ns) statement.Bind(parameter++, *query.start_utc_ns);
    if (query.end_utc_ns) statement.Bind(parameter++, *query.end_utc_ns);
    if (query.clock_owner) statement.Bind(parameter++, *query.clock_owner);
    for (const auto &severity : query.severities) statement.Bind(parameter++, severity);
    for (const auto &[kind, value] : query.correlations) {
      if (kind != "channel") statement.Bind(parameter++, kind);
      statement.Bind(parameter++, value);
    }
    if (query.alarm_phase && *query.alarm_phase != "all" && *query.alarm_phase != "raw" && *query.alarm_phase != "acknowledged") {
      statement.Bind(parameter++, "operator_alarm_" + *query.alarm_phase);
    }
    if (!query.search.empty()) {
      const auto quote = [](std::string_view text) {
        std::string phrase = "\"";
        for (const auto c : text) {
          phrase += c;
          if (c == '"') phrase += '"';
        }
        return phrase + '"';
      };
      // Search original fields and JSON string contents without materializing
      // another decoded text copy. Quoting prevents FTS query syntax injection.
      auto phrase = quote(query.search);
      const auto encoded = content_identity::canonical_json(CanonicalValue(query.search));
      const auto escaped = encoded.substr(1, encoded.size() - 2);
      if (escaped != query.search) phrase += " OR " + quote(escaped);
      statement.Bind(parameter++, phrase);
    }
    if (capture_range) {
      statement.Bind(parameter++, capture_range->first);
      statement.Bind(parameter++, capture_range->second);
    }
    statement.Bind(parameter, static_cast<std::int64_t>(query.limit + 1U));

    std::vector<HistorianStoredEvent> events;
    if (summaries == nullptr)
      events.reserve(query.limit);
    else
      summaries->reserve(query.limit);
    std::size_t count = 0;
    std::optional<std::uint64_t> last;
    const auto retention = Retention();
    std::size_t export_bytes = content_identity::canonical_json(retention).size() + 512U;
    bool truncated = false;
    while (statement.Step()) {
      if (count == query.limit) {
        truncated = true;
        break;
      }
      if (exporting) {
        if (count != 0 && budget.YieldExportPage()) {
          truncated = true;
          break;
        }
        // Verified on startup/append; reject external changes before this scan.
        // The 8 KiB reserve covers the export wrapper, including a fully
        // escaped 1024-byte clock owner. Do not decode rows which cannot enter
        // the page.
        const auto record_bytes = statement.Integer(15);
        Require(record_bytes > 0, "historian_storage_corrupt", "invalid export record size");
        const auto next_bytes = static_cast<std::size_t>(record_bytes) + 8192U;
        if (count != 0 && (count == 32U || export_bytes + next_bytes > 1024U * 1024U)) {
          truncated = true;
          break;
        }
        export_bytes += next_bytes;
      }
      if (summaries == nullptr)
        events.push_back(ReadStoredEvent(statement));
      else
        summaries->push_back(ReadStoredSummary(statement));
      ++count;
      last = static_cast<std::uint64_t>(statement.Integer(0));
      budget.Check();
    }
    const auto next = truncated ? last : std::nullopt;
    return {std::move(events), truncated, next, retention, std::move(capture_evidence)};
  }

  CanonicalValue QuerySummaries(const HistorianQuery& query) {
    CanonicalValue::Array summaries;
    const auto page = Query(query, &summaries);
    return HistorianSummaryEnvelopeValue(std::move(summaries), page);
  }

  std::optional<HistorianStoredEvent> Read(const std::uint64_t sequence) {
    Require(sequence > 0U && sequence <= kMaximumSafeInteger,
            "historian_query_invalid", "history detail sequence is invalid");
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction snapshot(database_);
    RequireUnchanged();
    QueryBudget budget(database_);
    Statement row(database_, "SELECT " + kIndexedEventColumns +
                              " FROM historian_events WHERE sequence=?");
    row.Bind(1, static_cast<std::int64_t>(sequence));
    if (!row.Step()) {
      return std::nullopt;
    }
    auto stored = ReadStoredEvent(row);
    budget.Check();
    return stored;
  }

  CanonicalValue Discover(const std::string &kind, const std::string &after, std::size_t limit) {
    Require((kind == "component" || kind == "event_type" || kCorrelationKeys.contains(kind)) &&
                after.size() <= 1024U && content_identity::is_utf8_nfc(after) && limit > 0 && limit <= 32,
            "historian_query_invalid", "invalid bounded history discovery query");
    // Channel is the historical UI name for the producing component, not
    // an unrelated string nested in a producer's payload.
    const std::string field = kind == "channel" ? "component" : kind;
    const bool column = field == "component" || field == "event_type";
    // The only SQL identifiers are this closed, owner-defined pair. All
    // caller-supplied values remain bound parameters.
    const std::string sql = column
        ? "SELECT DISTINCT " + field + " FROM historian_events WHERE " + field + ">? AND length(CAST(" + field + " AS BLOB)) BETWEEN 1 AND 1024 ORDER BY " + field + " LIMIT ?"
        : "SELECT DISTINCT value FROM historian_event_keys WHERE kind=? AND value>? ORDER BY value LIMIT ?";
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction snapshot(database_);
    RequireUnchanged();
    QueryBudget budget(database_);
    Statement rows(database_, sql);
    int parameter = 1;
    if (!column) rows.Bind(parameter++, kind);
    rows.Bind(parameter++, after);
    rows.Bind(parameter, static_cast<std::int64_t>(limit + 1));
    CanonicalValue::Array values;
    while (rows.Step()) values.emplace_back(rows.Text(0));
    const bool truncated = values.size() > limit;
    if (truncated) values.resize(limit);
    const CanonicalValue next = truncated ? values.back() : CanonicalValue(nullptr);
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.historian_discovery.v1")},
        {"kind", CanonicalValue(kind)}, {"values", CanonicalValue(std::move(values))},
        {"truncated", CanonicalValue(truncated)}, {"next_after", next},
    });
  }

  void Close() {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    statements_.Clear();
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      StoreFail("historian_storage_failure",
                "close historian database: " + SqliteDetail(database_, result));
    }
    database_ = nullptr;
    closed_ = true;
  }

private:
  IEventHistoryStore::AppendStatus AppendInTransaction(const HistorianEvent &event, RetainedUsage &usage,
                                                       std::optional<std::int64_t> &epoch_first_sequence,
                                                       std::int64_t &encoded_bytes) {
    const auto event_json =
        content_identity::canonical_json(HistorianPersistenceValue(event));
    encoded_bytes = static_cast<std::int64_t>(event_json.size());
    const auto payload_json = content_identity::canonical_json(event.payload);
    const auto payload_digest = content_identity::sha256_digest(payload_json);
    const auto event_digest = content_identity::sha256_digest(event_json);
    const auto summary_json = SummaryJson(event);
    // Check before allocating a sequence. Duplicate delivery must neither
    // prune nor consume a lifetime sequence allowance.
    Statement existing(statements_, "SELECT event_digest FROM historian_events WHERE event_id=?");
    existing.Bind(1, event.event_id);
    if (existing.Step()) {
      Require(existing.Text(0) == event_digest && !existing.Step(), "historian_event_identity_conflict",
              "stored historian identity has conflicting content");
      return IEventHistoryStore::AppendStatus::kDuplicate;
    }
    Statement insert(
        statements_, "INSERT INTO historian_events("
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
    const auto sequence = sqlite3_last_insert_rowid(database_);
    Require(sequence > 0 && static_cast<std::uint64_t>(sequence) <= kMaximumSafeInteger,
            "historian_sequence_rollover_required", "historian sequence namespace requires replacement");
    const auto record_bytes = static_cast<std::int64_t>(event_json.size());
    IndexEvent(sequence, event, payload_json, record_bytes, usage);
    StoreSummary(sequence, event_digest, summary_json);
    const auto epoch_first = epoch_first_sequence.value_or(sequence);
    CaptureAppend(sequence, event, record_bytes, epoch_first, usage);
    EnforceCaptureBudget(usage);
    Prune(usage);
    epoch_first_sequence = epoch_first;
    return IEventHistoryStore::AppendStatus::kStored;
  }

  static int OnWalCommit(void *context, sqlite3 *, const char *, const int pages) {
    static_cast<Impl *>(context)->wal_pages_ = pages;
    return SQLITE_OK;
  }

  /// PASSIVE never waits for readers. RESTART waits (within the busy
  /// timeout) until the next commit can reuse the log from its start.
  bool CheckpointWal(const int mode) {
    int log_pages = 0;
    int checkpointed = 0;
    const auto result = sqlite3_wal_checkpoint_v2(database_, "main", mode, &log_pages, &checkpointed);
    if (result != SQLITE_OK || log_pages != checkpointed) return false;
    if (mode != SQLITE_CHECKPOINT_PASSIVE) wal_pages_ = 0;
    return true;
  }

  void RequireReclaimableWal() {
    const auto page_size = ScalarInteger(statements_, "PRAGMA page_size");
    if (wal_pages_ * page_size <= kMaximumWalBacklogBytes) return;
    Require(CheckpointWal(SQLITE_CHECKPOINT_RESTART), "historian_storage_busy",
            "a reader prevents reclaiming the historian write-ahead log");
  }

 void StoreSummary(std::int64_t sequence, const std::string& event_digest,
                   const std::string& summary_json) {
   Require(summary_json.size() <= kMaximumSummaryBytes, "historian_value_invalid",
           "historian summary is oversized");
   Statement update(statements_,
                    "UPDATE historian_events SET "
                    "summary_json=?,summary_digest=? WHERE sequence=?");
   update.Bind(1, summary_json);
   update.Bind(2, SummaryDigest(sequence, event_digest, summary_json));
   update.Bind(3, sequence);
   Require(!update.Step(), "historian_storage_corrupt", "summary update returned a row");
 }

  void IndexEvent(std::int64_t sequence, const HistorianEvent &event, const std::string &payload_json,
                  std::int64_t record_bytes, RetainedUsage &usage) {
    const auto utc = HistorianRecordedUtcNs(event.occurred_at_wall);
    const auto clock = RecordedClockOwner(event);
    bool regressed = false;
    if (clock && utc) {
      Statement previous(statements_, "SELECT occurred_at_ns,occurred_at_utc_ns FROM historian_events "
          "WHERE clock_owner=? AND occurred_at_utc_ns IS NOT NULL AND occurred_at_ns<? "
          "ORDER BY occurred_at_ns DESC,sequence DESC LIMIT 1");
      previous.Bind(1, *clock);
      previous.Bind(2, event.occurred_at_ns);
      if (previous.Step()) regressed = *utc < previous.Integer(1);
      // Late ingest can also prove a regression at an already retained
      // successor. This is a sticky derived uncertainty flag; original event
      // bytes/digests and source order never change.
      Statement next(statements_, std::string("SELECT sequence,occurred_at_utc_ns,retention_class,wall_clock_regressed,") +
          (captures_ready_ ? "capture_refs" : "0") + " FROM historian_events "
          "WHERE clock_owner=? AND occurred_at_utc_ns IS NOT NULL AND occurred_at_ns>? ORDER BY occurred_at_ns,sequence LIMIT 1");
      next.Bind(1, *clock);
      next.Bind(2, event.occurred_at_ns);
      if (next.Step() && *utc > next.Integer(1) && next.Integer(3) == 0) {
        Statement mark(statements_, "UPDATE historian_events SET wall_clock_regressed=1 WHERE sequence=?");
        mark.Bind(1, next.Integer(0));
        Require(!mark.Step(), "historian_storage_corrupt", "historian clock flag update returned a row");
        ++usage.at(UsageBucket(next.Integer(2), next.Integer(4))).regressed_time;
      }
    }
    const auto condition_severity = HistorianConditionSeverity(event);
    const auto partition = RetentionPartition(event);
    Statement time_index(statements_, "UPDATE historian_events SET occurred_at_utc_ns=?,clock_owner=?,wall_clock_regressed=?,"
        "condition_severity=?,retention_class=?,record_bytes=? WHERE sequence=?");
    if (utc) time_index.Bind(1, *utc); else time_index.BindNull(1);
    if (clock) time_index.Bind(2, *clock); else time_index.BindNull(2);
    time_index.Bind(3, static_cast<std::int64_t>(regressed));
    time_index.Bind(4, condition_severity);
    time_index.Bind(5, partition);
    time_index.Bind(6, record_bytes);
    time_index.Bind(7, sequence);
    Require(!time_index.Step(), "historian_storage_corrupt", "historian time index returned a row");
    Statement keys(statements_,
                   "INSERT OR IGNORE INTO historian_event_keys(sequence,kind,value) "
                   "SELECT ?1,key,value FROM json_tree(?2) WHERE type='text' AND key IN "
                   "(" +
                       std::string(kCorrelationNames) +
                       ") AND length(CAST(value AS BLOB)) BETWEEN 1 AND 1024 "
                       "AND "
                       "(COALESCE(json_extract(?2,'$.schema_version'),'') NOT IN (" +
                       kProgramDiagnosticSchemaList + ") OR path='$')");
    keys.Bind(1, sequence);
    keys.Bind(2, payload_json);
    Require(!keys.Step(), "historian_storage_corrupt", "historian correlation index returned a row");
    Statement search(statements_, "INSERT INTO historian_search(rowid,component,event_type,details) "
        "SELECT sequence,component,event_type,details FROM historian_search_content WHERE sequence=?");
    search.Bind(1, sequence);
    Require(!search.Step(), "historian_storage_corrupt", "historian text index returned a row");
    auto &part = usage.at(static_cast<std::size_t>(partition));
    ++part.count;
    part.bytes += record_bytes;
    part.unknown_time += !utc;
    part.regressed_time += regressed;
  }

  void Prune(RetainedUsage &usage) {
    for (std::int64_t partition : {0, 1}) {
      auto &part = usage.at(static_cast<std::size_t>(partition));
      for (;;) {
        if (part.count <= static_cast<std::int64_t>(record_retention_) && part.bytes <= 16LL * 1024LL * 1024LL) break;
        // Rows protected by a live failure capture are charged to the
        // capture budget and are never selected by ordinary pruning.
        Statement oldest(statements_, "SELECT " + kIndexedEventColumns + " FROM historian_events WHERE retention_class=?" +
            std::string(captures_ready_ ? " AND capture_refs=0" : "") + " ORDER BY sequence LIMIT 1");
        oldest.Bind(1, partition);
        Require(oldest.Step(), "historian_storage_corrupt", "historian retention count has no record");
        const auto stored = ReadStoredEvent(oldest);
        const auto sequence = oldest.Integer(0), bytes = oldest.Integer(15);
        Statement remove_index(statements_, "INSERT INTO historian_search(historian_search,rowid,component,event_type,details) "
            "SELECT 'delete',sequence,component,event_type,details FROM historian_search_content WHERE sequence=?");
        remove_index.Bind(1, sequence);
        Require(!remove_index.Step(), "historian_storage_corrupt", "historian index deletion returned a row");
        Statement remove(statements_, "DELETE FROM historian_events WHERE sequence=?");
        remove.Bind(1, sequence);
        Require(!remove.Step(), "historian_storage_corrupt", "historian deletion returned a row");
        Statement loss(statements_, "UPDATE historian_retention SET "
            "pruned_count=min(pruned_count,9007199254740990)+1,pruned_bytes=min(pruned_bytes,9223372036854775807-?)+?,"
            "last_pruned_sequence=? WHERE partition=?");
        loss.Bind(1, bytes);
        loss.Bind(2, bytes);
        loss.Bind(3, sequence);
        loss.Bind(4, partition);
        Require(!loss.Step(), "historian_storage_corrupt", "historian prune fact returned a row");
        --part.count;
        part.bytes -= bytes;
        part.unknown_time -= !stored.occurred_at_utc_ns;
        part.regressed_time -= stored.wall_clock_regressed;
      }
    }
  }

  CanonicalValue Retention() {
    Statement rows(database_, "SELECT partition,pruned_count,pruned_bytes,last_pruned_sequence FROM historian_retention ORDER BY partition");
    CanonicalValue::Array partitions;
    while (rows.Step()) {
      Require(rows.Integer(0) == static_cast<std::int64_t>(partitions.size()) && rows.Integer(0) < 2 &&
                  rows.Integer(1) >= 0 && static_cast<std::uint64_t>(rows.Integer(1)) <= kMaximumSafeInteger &&
                  rows.Integer(2) >= 0 && (rows.IsNull(3) || (rows.Integer(3) > 0 &&
                  static_cast<std::uint64_t>(rows.Integer(3)) <= kMaximumSafeInteger)),
              "historian_storage_corrupt", "historian loss metadata is invalid");
      CanonicalValue::Object part{{"partition", CanonicalValue(rows.Integer(0) == 0 ? "routine" : "important")}};
      const auto &usage = usage_.at(static_cast<std::size_t>(rows.Integer(0)));
      for (const auto &[name, value] : std::vector<std::pair<std::string_view, std::int64_t>>{
          {"retained_count",usage.count},{"retained_bytes",usage.bytes},
          {"unknown_time_count",usage.unknown_time},{"regressed_time_count",usage.regressed_time}}) {
        part.emplace_back(name, CanonicalValue(std::to_string(value)));
      }
      part.emplace_back("loss_counter_saturated", CanonicalValue(
          static_cast<std::uint64_t>(rows.Integer(1)) == kMaximumSafeInteger ||
          rows.Integer(2) == std::numeric_limits<std::int64_t>::max()));
      for (const auto &[column, name] : std::vector<std::pair<int, std::string_view>>{
          {1,"pruned_count"},{2,"pruned_bytes"},{3,"last_pruned_sequence"}}) {
        part.emplace_back(name, rows.IsNull(column) ? CanonicalValue(nullptr) : CanonicalValue(std::to_string(rows.Integer(column))));
      }
      for (bool latest : {false, true}) {
        Statement edge(database_, "SELECT occurred_at_utc_ns FROM historian_events WHERE retention_class=? AND occurred_at_utc_ns IS NOT NULL "
            "ORDER BY occurred_at_utc_ns " + std::string(latest ? "DESC" : "ASC") + " LIMIT 1");
        edge.Bind(1, rows.Integer(0));
        part.emplace_back(latest ? "last_recorded_utc_ns" : "first_recorded_utc_ns",
                          edge.Step() ? CanonicalValue(std::to_string(edge.Integer(0))) : CanonicalValue(nullptr));
      }
      partitions.emplace_back(std::move(part));
    }
    Require(partitions.size() == 2, "historian_storage_corrupt", "historian loss metadata is incomplete");
    const auto prior_pruning_known = ScalarInteger(database_, "SELECT prior_pruning_known FROM historian_metadata WHERE singleton=1");
    Require(prior_pruning_known == 0 || prior_pruning_known == 1,
            "historian_storage_corrupt", "historian coverage metadata is invalid");
    Require(captures_ready_, "historian_storage_corrupt", "historian capture schema is unavailable");
    const auto &protected_usage = usage_.at(kCaptureBucket);
    Statement capture_totals(database_, "SELECT (SELECT count(*) FROM historian_captures),"
        "(SELECT count(*) FROM historian_captures WHERE state='open'),"
        "retired_count,released_rows,released_bytes,last_retired_capture_id "
        "FROM historian_capture_retention WHERE singleton=1");
    Require(capture_totals.Step() && capture_totals.Integer(2) >= 0 && capture_totals.Integer(3) >= 0 &&
                capture_totals.Integer(4) >= 0,
            "historian_storage_corrupt", "historian capture accounting is invalid");
    CanonicalValue captures(CanonicalValue::Object{
        {"live_count", CanonicalValue(std::to_string(capture_totals.Integer(0)))},
        {"open_count", CanonicalValue(std::to_string(capture_totals.Integer(1)))},
        {"protected_count", CanonicalValue(std::to_string(protected_usage.count))},
        {"protected_bytes", CanonicalValue(std::to_string(protected_usage.bytes))},
        {"live_capture_limit", CanonicalValue(std::to_string(capture_limits_.live_captures))},
        {"protected_byte_limit", CanonicalValue(std::to_string(capture_limits_.protected_bytes))},
        {"retired_count", CanonicalValue(std::to_string(capture_totals.Integer(2)))},
        {"released_rows", CanonicalValue(std::to_string(capture_totals.Integer(3)))},
        {"released_bytes", CanonicalValue(std::to_string(capture_totals.Integer(4)))},
        {"last_retired_capture_id", capture_totals.IsNull(5)
            ? CanonicalValue(nullptr) : CanonicalValue(std::to_string(capture_totals.Integer(5)))},
    });
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.historian_retention.v2")},
        {"partitions", CanonicalValue(std::move(partitions))},
        {"captures", std::move(captures)},
        {"partition_row_limit", CanonicalValue(std::to_string(record_retention_))},
        {"partition_record_byte_limit", CanonicalValue("16777216")},
        {"database_page_byte_limit", CanonicalValue("536870912")},
        {"duration_guaranteed", CanonicalValue(false)},
        {"prior_pruning_known", CanonicalValue(prior_pruning_known == 1)},
    });
  }

  static std::string NewWriterEpoch() {
    std::random_device random;
    std::string text;
    for (int index = 0; index < 4; ++index) text += std::to_string(random()) + ':';
    return "writer:" + content_identity::sha256_digest(
        text + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  }

  /// Move rows of [first,last] whose protection changes between the class
  /// buckets and the capture bucket. ``from_refs`` is the reference count
  /// the moving rows have before the caller's reference update.
  void MoveRangeUsage(RetainedUsage &usage, std::int64_t first, std::int64_t last,
                      std::int64_t from_refs, bool into_capture) {
    Statement moving(statements_, "SELECT retention_class,count(*),coalesce(sum(record_bytes),0),"
        "coalesce(sum(occurred_at_utc_ns IS NULL),0),coalesce(sum(wall_clock_regressed),0) FROM historian_events "
        "WHERE sequence BETWEEN ? AND ? AND capture_refs=? GROUP BY retention_class");
    moving.Bind(1, first);
    moving.Bind(2, last);
    moving.Bind(3, from_refs);
    while (moving.Step()) {
      const auto partition = moving.Integer(0);
      Require(partition == 0 || partition == 1, "historian_storage_corrupt",
              "historian capture range contains an invalid partition");
      const PartitionUsage delta{moving.Integer(1), moving.Integer(2), moving.Integer(3),
                                 moving.Integer(4)};
      auto &from = usage.at(into_capture ? static_cast<std::size_t>(partition) : kCaptureBucket);
      auto &to = usage.at(into_capture ? kCaptureBucket : static_cast<std::size_t>(partition));
      from.count -= delta.count;
      from.bytes -= delta.bytes;
      from.unknown_time -= delta.unknown_time;
      from.regressed_time -= delta.regressed_time;
      to.count += delta.count;
      to.bytes += delta.bytes;
      to.unknown_time += delta.unknown_time;
      to.regressed_time += delta.regressed_time;
      Require(from.count >= 0 && from.bytes >= 0, "historian_storage_corrupt",
              "historian capture accounting would become negative");
    }
  }

  void CloseCapture(std::int64_t capture_id, std::string_view state, std::string_view reason) {
    // Delivery loss observed before the closure is recorded may belong to the
    // capture's window (after its last row, before its deadline): count it,
    // conservatively, without claiming a position.
    Statement close(statements_, "UPDATE historian_captures SET state=?,close_reason=?,"
        "loss_at_last=max(loss_at_last,?) WHERE capture_id=? AND state='open'");
    close.Bind(1, state);
    close.Bind(2, reason);
    close.Bind(3, delivery_loss_);
    close.Bind(4, capture_id);
    Require(!close.Step() && sqlite3_changes64(database_) == 1, "historian_storage_corrupt",
            "historian capture closure did not match one open capture");
  }

  /// The writer's only capture step, inside the append transaction. Expired
  /// captures close first, so a late event can never enter their tail.
  void CaptureAppend(std::int64_t sequence, const HistorianEvent &event, std::int64_t record_bytes,
                     std::int64_t epoch_first_sequence, RetainedUsage &usage) {
    const auto now = writer_clock_();
    std::vector<std::int64_t> open;
    {
      Statement candidates(statements_, "SELECT capture_id,tail_deadline_ns,writer_epoch FROM historian_captures WHERE state='open' ORDER BY capture_id");
      std::vector<std::pair<std::int64_t, bool>> rows;
      while (candidates.Step()) {
        Require(candidates.Text(2) == writer_epoch_, "historian_storage_corrupt",
                "an open historian capture belongs to another writer");
        rows.emplace_back(candidates.Integer(0), now >= candidates.Integer(1));
      }
      for (const auto &[identity, expired] : rows) {
        if (expired) CloseCapture(identity, "closed", "tail_duration");
        else open.push_back(identity);
      }
    }
    const bool trigger = IsFailureTrigger(event);
    {
      // A capture that this row would take past its byte limit closes first,
      // without the row; if the row is a failure it opens the next capture.
      std::vector<std::int64_t> fitting;
      for (const auto identity : open) {
        Statement size(statements_, "SELECT capture_bytes FROM historian_captures WHERE capture_id=?");
        size.Bind(1, identity);
        Require(size.Step(), "historian_storage_corrupt", "open capture disappeared");
        if (size.Integer(0) + record_bytes > capture_limits_.capture_bytes) {
          CloseCapture(identity, "truncated", "capture_bytes");
        } else {
          fitting.push_back(identity);
        }
      }
      open = std::move(fitting);
    }
    if (open.empty()) {
      if (trigger) OpenCapture(sequence, event, record_bytes, now, epoch_first_sequence, usage);
      return;
    }
    // The new row enters every open capture's tail; it was charged to its
    // class by IndexEvent and now moves to the capture bucket once.
    MoveRangeUsage(usage, sequence, sequence, 0, true);
    Statement protect(statements_, "UPDATE historian_events SET capture_refs=? WHERE sequence=?");
    protect.Bind(1, static_cast<std::int64_t>(open.size()));
    protect.Bind(2, sequence);
    Require(!protect.Step(), "historian_storage_corrupt", "capture protection returned a row");
    for (const auto identity : open) {
      Statement extend(statements_, "UPDATE historian_captures SET last_sequence=?,tail_rows=tail_rows+1,"
          "capture_bytes=capture_bytes+?,loss_at_last=max(loss_at_last,?),trigger_count=trigger_count+? "
          "WHERE capture_id=? RETURNING tail_rows,capture_bytes");
      extend.Bind(1, sequence);
      extend.Bind(2, record_bytes);
      extend.Bind(3, delivery_loss_);
      extend.Bind(4, static_cast<std::int64_t>(trigger));
      extend.Bind(5, identity);
      Require(extend.Step(), "historian_storage_corrupt", "open capture disappeared");
      const auto tail_rows = extend.Integer(0);
      const auto capture_bytes = extend.Integer(1);
      Require(!extend.Step(), "historian_storage_corrupt", "capture update returned multiple rows");
      if (trigger) RecordTrigger(identity, sequence, event.event_id);
      // Joining triggers never extend these absolute limits.
      if (capture_bytes == capture_limits_.capture_bytes) {
        CloseCapture(identity, "truncated", "capture_bytes");
      } else if (tail_rows >= static_cast<std::int64_t>(capture_limits_.tail_rows)) {
        CloseCapture(identity, "closed", "tail_rows");
      }
    }
  }

  void RecordTrigger(std::int64_t capture_id, std::int64_t sequence, const std::string &event_id) {
    const auto recorded = [&] {
      Statement count(statements_, "SELECT count(*) FROM historian_capture_triggers WHERE capture_id=?");
      count.Bind(1, capture_id);
      Require(count.Step(), "historian_storage_corrupt", "capture trigger count is unavailable");
      return count.Integer(0);
    }();
    if (recorded < static_cast<std::int64_t>(capture_limits_.recorded_triggers)) {
      Statement insert(statements_, "INSERT INTO historian_capture_triggers(capture_id,sequence,event_id) VALUES(?,?,?)");
      insert.Bind(1, capture_id);
      insert.Bind(2, sequence);
      insert.Bind(3, event_id);
      Require(!insert.Step(), "historian_storage_corrupt", "capture trigger insert returned a row");
      return;
    }
    Statement overflow(statements_, "UPDATE historian_captures SET trigger_overflow=trigger_overflow+1 WHERE capture_id=?");
    overflow.Bind(1, capture_id);
    Require(!overflow.Step(), "historian_storage_corrupt", "capture overflow update returned a row");
  }

  void OpenCapture(std::int64_t sequence, const HistorianEvent &event, std::int64_t record_bytes,
                   std::int64_t now, std::int64_t epoch_first_sequence, RetainedUsage &usage) {
    // Configuration guarantees a maximal record fits one capture.
    Require(record_bytes <= capture_limits_.capture_bytes, "historian_storage_corrupt",
            "a historian record exceeds the failure capture byte limit");
    // Preceding context across all partitions, bounded by rows and bytes.
    std::int64_t first = sequence;
    std::int64_t bytes = record_bytes;
    std::int64_t preceding = 0;
    std::string_view limit = "history_start";
    {
      Statement previous(statements_, "SELECT sequence,record_bytes FROM historian_events WHERE sequence<? "
                                    "ORDER BY sequence DESC LIMIT ?");
      previous.Bind(1, sequence);
      previous.Bind(2, static_cast<std::int64_t>(capture_limits_.preceding_rows + 1U));
      while (previous.Step()) {
        if (preceding == static_cast<std::int64_t>(capture_limits_.preceding_rows)) {
          limit = "rows";
          break;
        }
        const auto row_bytes = previous.Integer(1);
        if (bytes + row_bytes > capture_limits_.preceding_bytes + record_bytes ||
            bytes + row_bytes > capture_limits_.capture_bytes) {
          limit = "bytes";
          break;
        }
        first = previous.Integer(0);
        bytes += row_bytes;
        ++preceding;
      }
    }
    if (limit == "history_start" &&
        (ScalarInteger(statements_, "SELECT count(*) FROM historian_retention WHERE last_pruned_sequence IS NOT NULL") > 0 ||
         ScalarInteger(statements_, "SELECT prior_pruning_known FROM historian_metadata WHERE singleton=1") == 0)) {
      // Older context existed but was already pruned (or legacy coverage is
      // unknown): the capture reports that instead of claiming a full window.
      limit = "pruned";
    }
    MoveRangeUsage(usage, first, sequence, 0, true);
    Statement protect(statements_, "UPDATE historian_events SET capture_refs=capture_refs+1 WHERE sequence BETWEEN ? AND ?");
    protect.Bind(1, first);
    protect.Bind(2, sequence);
    Require(!protect.Step() && sqlite3_changes64(database_) == preceding + 1,
            "historian_storage_corrupt", "capture protection differs from its preceding context");
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        capture_limits_.tail_duration).count();
    Statement insert(statements_, "INSERT INTO historian_captures(state,close_reason,first_sequence,trigger_sequence,"
        "last_sequence,trigger_event_id,opened_utc_ns,writer_epoch,tail_deadline_ns,preceding_rows,preceding_limit,"
        "tail_rows,capture_bytes,trigger_count,trigger_overflow,loss_at_open,loss_at_last,spans_restart) "
        "VALUES('open',NULL,?,?,?,?,?,?,?,?,?,0,?,1,0,?,?,?)");
    insert.Bind(1, first);
    insert.Bind(2, sequence);
    insert.Bind(3, sequence);
    insert.Bind(4, event.event_id);
    const auto utc = HistorianRecordedUtcNs(event.occurred_at_wall);
    if (utc) insert.Bind(5, *utc); else insert.BindNull(5);
    insert.Bind(6, writer_epoch_);
    insert.Bind(7, now > std::numeric_limits<std::int64_t>::max() - duration
                       ? std::numeric_limits<std::int64_t>::max() : now + duration);
    insert.Bind(8, preceding);
    insert.Bind(9, limit);
    insert.Bind(10, bytes);
    insert.Bind(11, delivery_loss_);
    insert.Bind(12, delivery_loss_);
    insert.Bind(13, static_cast<std::int64_t>(first < epoch_first_sequence));
    Require(!insert.Step(), "historian_storage_corrupt", "capture insert returned a row");
    const auto capture_id = sqlite3_last_insert_rowid(database_);
    RecordTrigger(capture_id, sequence, event.event_id);
    if (capture_limits_.tail_rows == 0U || duration == 0) CloseCapture(capture_id, "closed",
        capture_limits_.tail_rows == 0U ? "tail_rows" : "tail_duration");
  }

  /// Keep the combined protection bounded: retire the oldest captures,
  /// release their references to ordinary pruning and record the loss.
  void EnforceCaptureBudget(RetainedUsage &usage) {
    for (;;) {
      const auto live = ScalarInteger(statements_, "SELECT count(*) FROM historian_captures");
      if (live <= static_cast<std::int64_t>(capture_limits_.live_captures) &&
          usage.at(kCaptureBucket).bytes <= capture_limits_.protected_bytes) return;
      Require(live > 1, "historian_storage_corrupt",
              "one failure capture exceeds the combined capture budget");
      Statement oldest(statements_, "SELECT capture_id,first_sequence,last_sequence FROM historian_captures "
                                  "ORDER BY capture_id LIMIT 1");
      Require(oldest.Step(), "historian_storage_corrupt", "historian capture budget has no capture");
      const auto identity = oldest.Integer(0), first = oldest.Integer(1), last = oldest.Integer(2);
      Statement released(statements_, "SELECT count(*),coalesce(sum(record_bytes),0) FROM historian_events "
                                    "WHERE sequence BETWEEN ? AND ? AND capture_refs=1");
      released.Bind(1, first);
      released.Bind(2, last);
      Require(released.Step(), "historian_storage_corrupt", "capture release is unavailable");
      const auto released_rows = released.Integer(0);
      const auto released_bytes = released.Integer(1);
      MoveRangeUsage(usage, first, last, 1, false);
      Statement unprotect(statements_, "UPDATE historian_events SET capture_refs=capture_refs-1 "
                                     "WHERE sequence BETWEEN ? AND ? AND capture_refs>0");
      unprotect.Bind(1, first);
      unprotect.Bind(2, last);
      Require(!unprotect.Step(), "historian_storage_corrupt", "capture release returned a row");
      Statement remove(statements_, "DELETE FROM historian_captures WHERE capture_id=?");
      remove.Bind(1, identity);
      Require(!remove.Step() && sqlite3_changes64(database_) == 1, "historian_storage_corrupt",
              "capture retirement did not remove one capture");
      Statement loss(statements_, "UPDATE historian_capture_retention SET "
          "retired_count=min(retired_count,9007199254740990)+1,"
          "released_rows=min(released_rows,9007199254740991-?)+?,"
          "released_bytes=min(released_bytes,9223372036854775807-?)+?,last_retired_capture_id=? WHERE singleton=1");
      loss.Bind(1, released_rows);
      loss.Bind(2, released_rows);
      loss.Bind(3, released_bytes);
      loss.Bind(4, released_bytes);
      loss.Bind(5, identity);
      Require(!loss.Step(), "historian_storage_corrupt", "capture retirement accounting returned a row");
    }
  }

  /// Evidence statement for one retained capture: range, contents and the
  /// known limits of what it can prove. Read-only; a passed deadline is
  /// derived, never written by a query.
  CanonicalValue CaptureEvidence(std::int64_t capture_id) {
    Statement row(database_, "SELECT capture_id,state,close_reason,first_sequence,trigger_sequence,last_sequence,"
        "trigger_event_id,opened_utc_ns,writer_epoch,tail_deadline_ns,preceding_rows,preceding_limit,tail_rows,"
        "capture_bytes,trigger_count,trigger_overflow,loss_at_open,loss_at_last,spans_restart "
        "FROM historian_captures WHERE capture_id=?");
    row.Bind(1, capture_id);
    Require(row.Step(), "historian_capture_not_found", "failure capture is not retained");
    const auto first = row.Integer(3), trigger = row.Integer(4), last = row.Integer(5);
    auto state = row.Text(1);
    std::optional<std::string> reason = row.IsNull(2) ? std::nullopt : std::optional(row.Text(2));
    bool closure_recorded = state != "open";
    if (state == "open" && row.Text(8) == writer_epoch_ && writer_clock_() >= row.Integer(9)) {
      state = "closed";
      reason = "tail_duration";
    }
    Statement present(database_, "SELECT count(*) FROM historian_events WHERE sequence BETWEEN ? AND ?");
    present.Bind(1, first);
    present.Bind(2, last);
    Require(present.Step(), "historian_storage_corrupt", "capture contents are unavailable");
    const auto retained = present.Integer(0);
    const auto missing = (last - first + 1) - retained;
    Require(missing >= 0, "historian_storage_corrupt", "capture range is inconsistent");
    CanonicalValue trigger_summary(nullptr);
    {
      Statement summary(database_, "SELECT summary_json FROM historian_events WHERE sequence=?");
      summary.Bind(1, trigger);
      if (summary.Step()) {
        try {
          const auto value = content_identity::parse_canonical_json(
              summary.Text(0), content_identity::CanonicalJsonParseLimits{kMaximumSummaryBytes, 4U, 64U});
          CanonicalValue::Object compact;
          for (const auto *name : {"event_type", "component", "condition_severity", "message"}) {
            if (const auto *field = value.find_object_member(name); field != nullptr) {
              compact.emplace_back(name, *field);
            }
          }
          trigger_summary = CanonicalValue(std::move(compact));
        } catch (const std::exception &error) {
          StoreFail("historian_storage_corrupt", "capture trigger summary is invalid: " + std::string(error.what()));
        }
      }
    }
    CanonicalValue::Array triggers;
    {
      Statement recorded(database_, "SELECT sequence,event_id FROM historian_capture_triggers WHERE capture_id=? ORDER BY sequence");
      recorded.Bind(1, capture_id);
      while (recorded.Step()) {
        triggers.emplace_back(CanonicalValue::Object{
            {"sequence", CanonicalValue(static_cast<double>(recorded.Integer(0)))},
            {"event_id", CanonicalValue(recorded.Text(1))}});
      }
    }
    // Context pruned before the capture opened is reported, not
    // reconstructed: gaps inside the range, or a window cut by earlier pruning
    // (decided by the writer when the capture opened).
    const bool preceding_complete = missing == 0 && row.Text(11) != "pruned";
    const auto loss_before = row.Integer(16);
    // Not yet closed by the writer: loss observed so far may belong to it.
    const auto loss_last = closure_recorded ? row.Integer(17) : std::max(row.Integer(17), delivery_loss_);
    const auto loss_during = loss_last - row.Integer(16);
    CanonicalValue::Array uncertainty;
    if (!preceding_complete) uncertainty.emplace_back("context_pruned_before_capture");
    if (loss_before > 0) uncertainty.emplace_back("writer_delivery_loss_before_capture");
    if (loss_during > 0) uncertainty.emplace_back("writer_delivery_loss_during_capture");
    if (row.Integer(18) != 0) uncertainty.emplace_back("context_spans_writer_restart");
    if (state == "closed_by_restart") uncertainty.emplace_back("tail_interrupted_by_restart");
    const bool complete = uncertainty.empty();
    return CanonicalValue(CanonicalValue::Object{
        {"capture_id", CanonicalValue(std::to_string(capture_id))},
        {"state", CanonicalValue(state)},
        {"close_reason", reason ? CanonicalValue(*reason) : CanonicalValue(nullptr)},
        {"closure_recorded", CanonicalValue(closure_recorded)},
        {"first_sequence", CanonicalValue(static_cast<double>(first))},
        {"trigger_sequence", CanonicalValue(static_cast<double>(trigger))},
        {"last_sequence", CanonicalValue(static_cast<double>(last))},
        {"trigger_event_id", CanonicalValue(row.Text(6))},
        {"trigger", std::move(trigger_summary)},
        {"opened_at_utc_ns", row.IsNull(7) ? CanonicalValue(nullptr) : CanonicalValue(std::to_string(row.Integer(7)))},
        {"preceding_rows", CanonicalValue(static_cast<double>(row.Integer(10)))},
        {"preceding_limit", CanonicalValue(row.Text(11))},
        {"tail_rows", CanonicalValue(static_cast<double>(row.Integer(12)))},
        {"capture_bytes", CanonicalValue(std::to_string(row.Integer(13)))},
        {"trigger_count", CanonicalValue(static_cast<double>(row.Integer(14)))},
        {"recorded_triggers", CanonicalValue(std::move(triggers))},
        {"unrecorded_trigger_count", CanonicalValue(static_cast<double>(row.Integer(15)))},
        {"retained_rows", CanonicalValue(static_cast<double>(retained))},
        {"missing_sequences", CanonicalValue(static_cast<double>(missing))},
        {"writer_delivery_loss_before", CanonicalValue(static_cast<double>(loss_before))},
        {"writer_delivery_loss_during", CanonicalValue(static_cast<double>(loss_during))},
        {"coverage", CanonicalValue(complete ? "complete" : "uncertain")},
        {"uncertainty", CanonicalValue(std::move(uncertainty))},
        {"contents", CanonicalValue("retained_event_context")},
    });
  }

  void RequireOpen() const {
    Require(!closed_ && database_ != nullptr, "historian_closed",
            "historian database is closed");
  }

  void RequireUnchanged() {
    // Called under BEGIN IMMEDIATE, not before acquiring the SQLite snapshot:
    // another connection cannot change an index between this check and use.
    Require(ScalarInteger(statements_, "PRAGMA data_version") == database_version_,
            "historian_storage_changed", "historian storage was changed by another writer; reopen and validate before use");
  }

  void Configure() {
    Check(database_, sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure historian busy timeout");
    // Write-ahead log: a commit appends the changed pages to the log without
    // waiting for the disk (synchronous=NORMAL); checkpoints sync them into
    // the database. Committed events survive a process crash; power loss can
    // lose recent routine commits but never corrupts the store, and a batch
    // with an ERROR/CRITICAL condition is synced at commit. The log is
    // bounded: a reader that prevents reclamation beyond
    // kMaximumWalBacklogBytes makes the next write fail through the existing
    // busy/full storage health instead of growing the log.
    Statement journal_mode(database_, "PRAGMA journal_mode=WAL");
    Require(journal_mode.Step() && journal_mode.Text(0) == "wal" && !journal_mode.Step(),
            "historian_storage_failure", "historian write-ahead log could not be configured");
    Exec(database_, "PRAGMA synchronous=NORMAL");
    Exec(database_, "PRAGMA journal_size_limit=" + std::to_string(kWalRetainedBytes));
    // Replaces automatic checkpointing: the writer checkpoints after its own
    // commits and learns the log size without another query.
    sqlite3_wal_hook(database_, &Impl::OnWalCommit, this);
    Exec(database_, "PRAGMA foreign_keys=ON");
    Exec(database_, "PRAGMA trusted_schema=OFF");
    Exec(database_, "PRAGMA recursive_triggers=OFF");
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

  struct LegacyLoss { std::int64_t cutoff{}, count{}, bytes{}; };

  std::array<LegacyLoss, 2> ReclaimLegacyIndexSpace() {
    RetainedUsage retained_usage{};
    std::array<LegacyLoss, 2> loss{};
    // Verify all originals before deleting any row. Keep each class's newest
    // bounded suffix; the same transaction records exactly what was pruned.
    {
      Statement rows(database_, "SELECT " + std::string(kEventColumns) + " FROM historian_events ORDER BY sequence DESC");
      while (rows.Step()) {
        const auto stored = ReadStoredEvent(rows, false);
        const auto bytes = static_cast<std::int64_t>(content_identity::canonical_json(HistorianPersistenceValue(stored.event)).size());
        const auto partition = static_cast<std::size_t>(RetentionPartition(stored.event));
        auto &kept = retained_usage.at(partition);
        auto &pruned = loss.at(partition);
        if (pruned.cutoff != 0 || kept.count >= static_cast<std::int64_t>(record_retention_) ||
            kept.bytes + bytes > 16LL * 1024LL * 1024LL) {
          if (pruned.cutoff == 0) pruned.cutoff = static_cast<std::int64_t>(stored.sequence);
          ++pruned.count;
          pruned.bytes += bytes;
        } else {
          ++kept.count;
          kept.bytes += bytes;
        }
      }
    }
    const std::string condition = "(CASE WHEN json_extract(payload_json,'$.schema_version')='manyforge.client_incident_event.v1' "
        "AND json_extract(payload_json,'$.original_severity') IN ('WARN','ERROR','CRITICAL') "
        "THEN json_extract(payload_json,'$.original_severity') ELSE severity END)";
    const std::string partition_sql = "CASE WHEN " + condition + " IN ('ERROR','CRITICAL') OR instr(event_type,'stop')>0 "
        "OR instr(event_type,'acknowledged')>0 THEN 1 ELSE 0 END";
    for (std::size_t partition = 0; partition < loss.size(); ++partition) {
      if (loss[partition].cutoff == 0) continue;
      Statement remove(database_, "DELETE FROM historian_events WHERE sequence<=? AND (" + partition_sql + ")=?");
      remove.Bind(1, loss[partition].cutoff);
      remove.Bind(2, static_cast<std::int64_t>(partition));
      Require(!remove.Step() && sqlite3_changes64(database_) == loss[partition].count,
              "historian_storage_corrupt", "legacy retention classification differs from verified records");
    }
    return loss;
  }

  /// ``capture_refs`` is a derived index, not independent evidence: every
  /// row in a capture range is protected and every protected row's count
  /// equals the number of retained captures covering it.
  void ValidateCaptureProtection() {
    Statement shape(database_, "SELECT 1 FROM historian_captures WHERE "
        "(state='open')<>(close_reason IS NULL) OR (state='closed_by_restart')<>(close_reason IS 'restart') "
        "OR (state='truncated')<>(close_reason IS 'capture_bytes') LIMIT 1");
    Require(!shape.Step(), "historian_storage_corrupt", "historian capture state is inconsistent");
    Statement unprotected(database_, "SELECT 1 FROM historian_captures c WHERE EXISTS("
        "SELECT 1 FROM historian_events e WHERE e.sequence BETWEEN c.first_sequence AND c.last_sequence "
        "AND e.capture_refs=0) OR NOT EXISTS(SELECT 1 FROM historian_events e WHERE e.sequence=c.trigger_sequence) LIMIT 1");
    Require(!unprotected.Step(), "historian_storage_corrupt",
            "historian capture range contains an unprotected or missing row");
    Statement miscounted(database_, "SELECT 1 FROM historian_events e WHERE e.capture_refs>0 AND e.capture_refs<>("
        "SELECT count(*) FROM historian_captures c WHERE e.sequence BETWEEN c.first_sequence AND c.last_sequence) LIMIT 1");
    Require(!miscounted.Step(), "historian_storage_corrupt",
            "historian capture protection differs from retained captures");
    Statement triggers(database_, "SELECT 1 FROM historian_capture_triggers t JOIN historian_captures c USING(capture_id) "
        "WHERE t.sequence NOT BETWEEN c.trigger_sequence AND c.last_sequence LIMIT 1");
    Require(!triggers.Step(), "historian_storage_corrupt", "historian capture trigger lies outside its capture");
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
           "PRAGMA user_version=1");
      Statement metadata(database_,
                         "INSERT INTO historian_metadata("
                         "singleton,schema_id,schema_version,record_retention) "
                         "VALUES(1,'manyforge.event_historian',1,?)");
      metadata.Bind(1, static_cast<std::int64_t>(record_retention_));
      Require(!metadata.Step(), "historian_storage_corrupt",
              "historian metadata insert returned a row");
    }
    std::set<std::string> expected{
        "index:historian_events_component_idx",
        "index:historian_events_time_idx",
        "index:historian_events_type_idx",
        "table:historian_events",
        "table:historian_metadata",
    };
    const auto version = ScalarInteger(database_, "PRAGMA user_version");
    if (version == 1) {
      Require(SchemaObjectNames(database_) == expected &&
                  ScalarInteger(database_, "PRAGMA application_id") == kHistorianApplicationId,
              "historian_storage_foreign", "legacy historian schema is invalid");
      Statement metadata(database_, "SELECT schema_id,schema_version,record_retention FROM historian_metadata WHERE singleton=1");
      Require(metadata.Step() && metadata.Text(0) == "manyforge.event_historian" && metadata.Integer(1) == 1 &&
                  metadata.Integer(2) == static_cast<std::int64_t>(record_retention_) && !metadata.Step(),
              "historian_storage_foreign", "legacy historian metadata is invalid");
      const auto loss = ReclaimLegacyIndexSpace();
      Exec(database_, std::string(kQueryIndexSchema));
      for (std::size_t partition = 0; partition < loss.size(); ++partition) {
        Statement record_loss(database_, "UPDATE historian_retention SET pruned_count=?,pruned_bytes=?,last_pruned_sequence=? WHERE partition=?");
        record_loss.Bind(1, loss[partition].count);
        record_loss.Bind(2, loss[partition].bytes);
        if (loss[partition].cutoff) record_loss.Bind(3, loss[partition].cutoff); else record_loss.BindNull(3);
        record_loss.Bind(4, static_cast<std::int64_t>(partition));
        Require(!record_loss.Step(), "historian_storage_corrupt", "legacy prune accounting returned a row");
      }
      if (objects.empty()) Exec(database_, "UPDATE historian_metadata SET prior_pruning_known=1 WHERE singleton=1");
      Statement rows(database_, "SELECT " + std::string(kEventColumns) + " FROM historian_events ORDER BY sequence");
      while (rows.Step()) {
        const auto stored = ReadStoredEvent(rows, false);
        IndexEvent(static_cast<std::int64_t>(stored.sequence), stored.event, rows.Text(7),
            static_cast<std::int64_t>(content_identity::canonical_json(HistorianPersistenceValue(stored.event)).size()), usage_);
      }
      Prune(usage_);
      Exec(database_, "UPDATE historian_metadata SET schema_version=2 WHERE singleton=1");
      Exec(database_, "PRAGMA user_version=2");
    }
    expected.insert({"index:historian_events_utc_idx", "index:historian_events_clock_idx", "index:historian_events_severity_idx",
        "index:historian_event_keys_lookup_idx", "index:historian_events_retention_idx", "index:historian_events_coverage_idx",
        "table:historian_retention", "table:historian_event_keys", "table:historian_search", "view:historian_search_content",
        "table:historian_search_data", "table:historian_search_idx", "table:historian_search_docsize", "table:historian_search_config"});
    const std::set<std::string> capture_objects{
        "index:historian_events_prune_idx", "table:historian_captures",
        "table:historian_capture_triggers", "table:historian_capture_retention"};
    const auto stored_version = ScalarInteger(database_, "PRAGMA user_version");
    if (stored_version == kHistorianUserVersion) expected.insert(capture_objects.begin(), capture_objects.end());
    Require(SchemaObjectNames(database_) == expected,
            "historian_storage_foreign",
            "historian database contains an unexpected schema");
    Require(ScalarInteger(database_, "PRAGMA application_id") == kHistorianApplicationId &&
                (stored_version == 2 || stored_version == 3 || stored_version == kHistorianUserVersion),
            "historian_storage_foreign", "historian SQLite identity is invalid");
    Statement metadata(database_,
                       "SELECT schema_id,schema_version,record_retention "
                       "FROM historian_metadata WHERE singleton=1");
    Require(metadata.Step() && metadata.Text(0) == "manyforge.event_historian" &&
                metadata.Integer(1) == stored_version &&
                metadata.Integer(2) == static_cast<std::int64_t>(record_retention_) &&
                !metadata.Step(),
            "historian_storage_foreign", "historian metadata is invalid or configuration changed");
    // There is no persisted usage counter that can authorize deleting rows.
    // Rebuild the tiny writer cache from intact rows once, then change a local
    // copy with each transaction and expose it only after commit.
    const bool migrate_summaries = stored_version == 2;
    if (migrate_summaries) {
      Exec(database_,
           "ALTER TABLE historian_events ADD COLUMN summary_json "
           "TEXT NOT NULL DEFAULT '';"
           "ALTER TABLE historian_events ADD COLUMN summary_digest "
           "TEXT NOT NULL DEFAULT '';");
    }
    if (stored_version != kHistorianUserVersion) {
      // Revision 4 adds only derived protection columns/tables. Original
      // event bytes, digests and cumulative prune facts are unchanged.
      Exec(database_, std::string(kCaptureSchema));
    }
    captures_ready_ = true;
    // A capture still open was interrupted by this writer's restart; its tail
    // cannot be completed on another process clock.
    Exec(database_, "UPDATE historian_captures SET state='closed_by_restart',close_reason='restart' WHERE state='open'");
    ValidateCaptureProtection();
    usage_ = {};
    Statement retained(database_, "SELECT " + kIndexedEventColumns +
                                      ",summary_json,summary_digest,capture_refs FROM "
                                      "historian_events ORDER BY sequence");
    while (retained.Step()) {
      const auto stored = ReadStoredEvent(retained);
      AccumulateUsage(usage_, stored, retained.Integer(15), retained.Integer(18));
      const auto summary = SummaryJson(stored.event);
      if (migrate_summaries)
        StoreSummary(static_cast<std::int64_t>(stored.sequence), retained.Text(9), summary);
      else
        Require(summary == retained.Text(16) && SummaryDigest(retained.Integer(0), retained.Text(9),
                                                              summary) == retained.Text(17),
                "historian_storage_corrupt", "historian summary differs from original event");
    }
    // Derived indexes are not independent evidence. Validate them against
    // intact originals once before any query can use their result.
    const std::string all_keys =
        "SELECT DISTINCT e.sequence,j.key,j.value FROM historian_events "
        "e,json_tree(e.payload_json) j "
        "WHERE j.type='text' AND j.key IN (" +
        std::string(kCorrelationNames) + ") AND length(CAST(j.value AS BLOB)) BETWEEN 1 AND 1024 ";
    const auto validate_keys = [&](bool producer_only) {
      const std::string expected_keys =
          all_keys + (producer_only
                          ? "AND "
                            "(COALESCE(json_extract(e.payload_json,'$.schema_version'),'')"
                            " NOT IN (" + kProgramDiagnosticSchemaList + ") OR j.path='$')"
                          : "");
      for (const bool missing : {false, true}) {
        const std::string actual = "SELECT sequence,kind,value FROM historian_event_keys";
        Statement difference(database_, (missing ? expected_keys + " EXCEPT " + actual
                                                 : actual + " EXCEPT " + expected_keys) +
                                            " LIMIT 1");
        Require(!difference.Step(), "historian_storage_corrupt",
                "historian correlation index differs from original records");
      }
    };
    if (version == 2) {
      // Committed v2 indexed nested diagnostic values as identities. Validate
      // that historical derivation before changing only this derived subset.
      // A v1 import above already built the current predicate from originals.
      validate_keys(false);
      Exec(database_,
           "DELETE FROM historian_event_keys WHERE sequence IN "
           "(SELECT sequence FROM historian_events "
           "WHERE json_extract(payload_json,'$.schema_version') IN (" +
               kProgramDiagnosticSchemaList + "))");
      Exec(database_, "INSERT INTO historian_event_keys(sequence,kind,value) " + all_keys +
                          "AND json_extract(e.payload_json,'$.schema_version') IN (" +
                          kProgramDiagnosticSchemaList + ") AND j.path='$'");
    }
    validate_keys(true);
    try {
      Exec(database_, "INSERT INTO historian_search(historian_search,rank) VALUES('integrity-check',1)");
    } catch (const EventHistorianError &error) {
      StoreFail("historian_storage_corrupt", "historian search index differs from original records: " + std::string(error.what()));
    }
    static_cast<void>(Retention());
    if (stored_version != kHistorianUserVersion) {
      Exec(database_,
           "UPDATE historian_metadata SET schema_version=" + std::to_string(kHistorianUserVersion) +
           " WHERE singleton=1; PRAGMA user_version=" + std::to_string(kHistorianUserVersion) + ";");
    }
    database_version_ = ScalarInteger(database_, "PRAGMA data_version");
    transaction.Commit();
  }

  std::filesystem::path database_path_;
  std::size_t record_retention_;
  HistorianCaptureLimits capture_limits_;
  HistorianWriterClock writer_clock_;
  std::string writer_epoch_;
  std::mutex mutex_;
  sqlite3 *database_{};
  StatementCache statements_{database_};
  std::int64_t wal_pages_{};
  bool closed_{};
  bool captures_ready_{};
  RetainedUsage usage_{};
  std::int64_t database_version_{};
  std::int64_t delivery_loss_{};
  std::optional<std::int64_t> epoch_first_sequence_{};
};

SqliteEventHistoryStore::SqliteEventHistoryStore(
    std::filesystem::path database_path, const std::size_t record_retention,
    HistorianCaptureLimits capture_limits, HistorianWriterClock writer_clock)
    : impl_(std::make_unique<Impl>(std::move(database_path), record_retention,
                                   capture_limits, std::move(writer_clock))) {}

void SqliteEventHistoryStore::ObserveDeliveryLoss(const std::uint64_t cumulative_losses) {
  impl_->ObserveDeliveryLoss(cumulative_losses);
}

CanonicalValue SqliteEventHistoryStore::Captures(
    const std::optional<std::uint64_t> before_capture_id, const std::size_t limit) {
  return impl_->Captures(before_capture_id, limit);
}

SqliteEventHistoryStore::~SqliteEventHistoryStore() = default;

IEventHistoryStore::AppendStatus
SqliteEventHistoryStore::Append(const HistorianEvent &event) {
  return impl_->Append(event);
}

IEventHistoryStore::AppendBatchResult
SqliteEventHistoryStore::AppendBatch(const std::span<const HistorianEvent> events) {
  return impl_->AppendBatch(events);
}

HistorianPage SqliteEventHistoryStore::Query(const HistorianQuery &query) {
  return impl_->Query(query);
}

CanonicalValue SqliteEventHistoryStore::QuerySummaries(const HistorianQuery& query) {
  return impl_->QuerySummaries(query);
}

HistorianPage SqliteEventHistoryStore::QueryExport(const HistorianQuery& query) {
  return impl_->Query(query, nullptr, true);
}

CanonicalValue SqliteEventHistoryStore::Discover(const std::string &kind, const std::string &after, std::size_t limit) {
  return impl_->Discover(kind, after, limit);
}

std::optional<HistorianStoredEvent>
SqliteEventHistoryStore::Read(const std::uint64_t sequence) {
  return impl_->Read(sequence);
}

void SqliteEventHistoryStore::Close() { impl_->Close(); }

} // namespace manyforge::headless
