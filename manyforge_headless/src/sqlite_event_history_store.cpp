#include "manyforge_headless/event_historian.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
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

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr int kHistorianApplicationId = 0x4D464548;
constexpr int kHistorianUserVersion = 3;
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

constexpr std::string_view kCorrelationNames =
    "'program_run_id','operation_id','action_span_id','node_instance_id','node_attempt_id',"
    "'intervention_session_id','contact_policy_id','incident_id','condition_id','alarm_condition_id',"
    "'activation_id','stop_id','resource_id','source_owner_epoch','phase','parent_node_attempt_id'";
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
         event.event_type.starts_with("gripper_confirmation_") ||
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
using RetainedUsage = std::array<PartitionUsage, 2>;

void AccumulateUsage(RetainedUsage &usage, const HistorianStoredEvent &stored, std::int64_t bytes) {
  auto &part = usage.at(static_cast<std::size_t>(RetentionPartition(stored.event)));
  ++part.count;
  part.bytes += bytes;
  part.unknown_time += !stored.occurred_at_utc_ns;
  part.regressed_time += stored.wall_clock_regressed;
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
      Require(sqlite3_db_readonly(database_, "main") == 0, "historian_storage_failure",
              "historian requires writable storage; read-only fallback is not healthy persistence");
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
    const auto summary_json = SummaryJson(event);
    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction transaction(database_);
    RequireUnchanged();
    // Check before allocating a sequence. Duplicate delivery must neither
    // prune nor consume a lifetime sequence allowance.
    Statement existing(database_, "SELECT event_digest FROM historian_events WHERE event_id=?");
    existing.Bind(1, event.event_id);
    if (existing.Step()) {
      Require(existing.Text(0) == event_digest && !existing.Step(), "historian_event_identity_conflict",
              "stored historian identity has conflicting content");
      transaction.Commit();
      return IEventHistoryStore::AppendStatus::kDuplicate;
    }
    Statement insert(
        database_, "INSERT INTO historian_events("
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
    auto usage = usage_;
    IndexEvent(sequence, event, payload_json, static_cast<std::int64_t>(event_json.size()), usage);
    StoreSummary(sequence, event_digest, summary_json);
    Prune(usage);
    transaction.Commit();
    usage_ = usage;
    return IEventHistoryStore::AppendStatus::kStored;
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
    sql += " ORDER BY sequence DESC LIMIT ?";

    std::lock_guard lock(mutex_);
    RequireOpen();
    Transaction snapshot(database_);
    RequireUnchanged();
    QueryBudget budget(database_);
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
    return {std::move(events), truncated, next, retention};
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
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      StoreFail("historian_storage_failure",
                "close historian database: " + SqliteDetail(database_, result));
    }
    database_ = nullptr;
    closed_ = true;
  }

private:
 void StoreSummary(std::int64_t sequence, const std::string& event_digest,
                   const std::string& summary_json) {
   Require(summary_json.size() <= kMaximumSummaryBytes, "historian_value_invalid",
           "historian summary is oversized");
   Statement update(database_,
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
      Statement previous(database_, "SELECT occurred_at_ns,occurred_at_utc_ns FROM historian_events "
          "WHERE clock_owner=? AND occurred_at_utc_ns IS NOT NULL AND occurred_at_ns<? "
          "ORDER BY occurred_at_ns DESC,sequence DESC LIMIT 1");
      previous.Bind(1, *clock);
      previous.Bind(2, event.occurred_at_ns);
      if (previous.Step()) regressed = *utc < previous.Integer(1);
      // Late ingest can also prove a regression at an already retained
      // successor. This is a sticky derived uncertainty flag; original event
      // bytes/digests and source order never change.
      Statement next(database_, "SELECT sequence,occurred_at_utc_ns,retention_class,wall_clock_regressed FROM historian_events "
          "WHERE clock_owner=? AND occurred_at_utc_ns IS NOT NULL AND occurred_at_ns>? ORDER BY occurred_at_ns,sequence LIMIT 1");
      next.Bind(1, *clock);
      next.Bind(2, event.occurred_at_ns);
      if (next.Step() && *utc > next.Integer(1) && next.Integer(3) == 0) {
        Statement mark(database_, "UPDATE historian_events SET wall_clock_regressed=1 WHERE sequence=?");
        mark.Bind(1, next.Integer(0));
        Require(!mark.Step(), "historian_storage_corrupt", "historian clock flag update returned a row");
        ++usage.at(static_cast<std::size_t>(next.Integer(2))).regressed_time;
      }
    }
    const auto condition_severity = HistorianConditionSeverity(event);
    const auto partition = RetentionPartition(event);
    Statement time_index(database_, "UPDATE historian_events SET occurred_at_utc_ns=?,clock_owner=?,wall_clock_regressed=?,"
        "condition_severity=?,retention_class=?,record_bytes=? WHERE sequence=?");
    if (utc) time_index.Bind(1, *utc); else time_index.BindNull(1);
    if (clock) time_index.Bind(2, *clock); else time_index.BindNull(2);
    time_index.Bind(3, static_cast<std::int64_t>(regressed));
    time_index.Bind(4, condition_severity);
    time_index.Bind(5, partition);
    time_index.Bind(6, record_bytes);
    time_index.Bind(7, sequence);
    Require(!time_index.Step(), "historian_storage_corrupt", "historian time index returned a row");
    Statement keys(database_,
                   "INSERT OR IGNORE INTO historian_event_keys(sequence,kind,value) "
                   "SELECT ?1,key,value FROM json_tree(?2) WHERE type='text' AND key IN "
                   "(" +
                       std::string(kCorrelationNames) +
                       ") AND length(CAST(value AS BLOB)) BETWEEN 1 AND 1024 "
                       "AND "
                       "(COALESCE(json_extract(?2,'$.schema_version'),'')!='manyforge."
                       "program_diagnostic.v1' OR path='$')");
    keys.Bind(1, sequence);
    keys.Bind(2, payload_json);
    Require(!keys.Step(), "historian_storage_corrupt", "historian correlation index returned a row");
    Statement search(database_, "INSERT INTO historian_search(rowid,component,event_type,details) "
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
        Statement oldest(database_, "SELECT " + kIndexedEventColumns + " FROM historian_events WHERE retention_class=? ORDER BY sequence LIMIT 1");
        oldest.Bind(1, partition);
        Require(oldest.Step(), "historian_storage_corrupt", "historian retention count has no record");
        const auto stored = ReadStoredEvent(oldest);
        const auto sequence = oldest.Integer(0), bytes = oldest.Integer(15);
        Statement remove_index(database_, "INSERT INTO historian_search(historian_search,rowid,component,event_type,details) "
            "SELECT 'delete',sequence,component,event_type,details FROM historian_search_content WHERE sequence=?");
        remove_index.Bind(1, sequence);
        Require(!remove_index.Step(), "historian_storage_corrupt", "historian index deletion returned a row");
        Statement remove(database_, "DELETE FROM historian_events WHERE sequence=?");
        remove.Bind(1, sequence);
        Require(!remove.Step(), "historian_storage_corrupt", "historian deletion returned a row");
        Statement loss(database_, "UPDATE historian_retention SET "
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
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.historian_retention.v1")},
        {"partitions", CanonicalValue(std::move(partitions))},
        {"partition_row_limit", CanonicalValue(std::to_string(record_retention_))},
        {"partition_record_byte_limit", CanonicalValue("16777216")},
        {"database_page_byte_limit", CanonicalValue("536870912")},
        {"duration_guaranteed", CanonicalValue(false)},
        {"prior_pruning_known", CanonicalValue(prior_pruning_known == 1)},
    });
  }

  void RequireOpen() const {
    Require(!closed_ && database_ != nullptr, "historian_closed",
            "historian database is closed");
  }

  void RequireUnchanged() const {
    // Called under BEGIN IMMEDIATE, not before acquiring the SQLite snapshot:
    // another connection cannot change an index between this check and use.
    Require(ScalarInteger(database_, "PRAGMA data_version") == database_version_,
            "historian_storage_changed", "historian storage was changed by another writer; reopen and validate before use");
  }

  void Configure() {
    Check(database_, sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure historian busy timeout");
    // This store already serializes its sole connection. WAL adds no reader
    // concurrency here, but a foreign reader can prevent its reclamation.
    // A rollback journal bounds storage by the page image, is reused after
    // each transaction, and reports a busy/full sink through existing health.
    Statement journal_mode(database_, "PRAGMA journal_mode=TRUNCATE");
    Require(journal_mode.Step() && journal_mode.Text(0) == "truncate" && !journal_mode.Step(),
            "historian_storage_failure", "historian rollback journal could not be configured");
    Exec(database_, "PRAGMA synchronous=FULL");
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
    Require(SchemaObjectNames(database_) == expected,
            "historian_storage_foreign",
            "historian database contains an unexpected schema");
    const auto stored_version = ScalarInteger(database_, "PRAGMA user_version");
    Require(ScalarInteger(database_, "PRAGMA application_id") == kHistorianApplicationId &&
                (stored_version == 2 || stored_version == kHistorianUserVersion),
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
    usage_ = {};
    Statement retained(database_, "SELECT " + kIndexedEventColumns +
                                      ",summary_json,summary_digest FROM "
                                      "historian_events ORDER BY sequence");
    while (retained.Step()) {
      const auto stored = ReadStoredEvent(retained);
      AccumulateUsage(usage_, stored, retained.Integer(15));
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
                            "!='manyforge.program_diagnostic.v1' OR j.path='$')"
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
           "WHERE "
           "json_extract(payload_json,'$.schema_version')='"
           "manyforge.program_diagnostic.v1')");
      Exec(database_, "INSERT INTO historian_event_keys(sequence,kind,value) " + all_keys +
                          "AND "
                          "json_extract(e.payload_json,'$.schema_version')='"
                          "manyforge.program_diagnostic.v1' AND j.path='$'");
    }
    validate_keys(true);
    try {
      Exec(database_, "INSERT INTO historian_search(historian_search,rank) VALUES('integrity-check',1)");
    } catch (const EventHistorianError &error) {
      StoreFail("historian_storage_corrupt", "historian search index differs from original records: " + std::string(error.what()));
    }
    static_cast<void>(Retention());
    if (migrate_summaries) {
      Exec(database_,
           "UPDATE historian_metadata SET schema_version=3 WHERE "
           "singleton=1; PRAGMA user_version=3;");
    }
    database_version_ = ScalarInteger(database_, "PRAGMA data_version");
    transaction.Commit();
  }

  std::filesystem::path database_path_;
  std::size_t record_retention_;
  std::mutex mutex_;
  sqlite3 *database_{};
  bool closed_{};
  RetainedUsage usage_{};
  std::int64_t database_version_{};
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
